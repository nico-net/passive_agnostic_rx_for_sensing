#!/usr/bin/env python3
"""mpci_replay_score.py -- helper + scorer of tests/passive_rx/mpci/mpci_replay.sh (plan task G7, gate V3).

Subcommands (all offline, no receiver):
  cells  <inventory.json> <capture dir> <pci> [<pci>...]   cells.json for the arm (first PCI = the started cell = cell 0); window from capture_info.json
  cli    <pci>                                              the started cell's command-line tuning: -C .. --ssb .. -r .. --initial-fo ..
  src    <ring> <capture dir>                               g5_fast_pub source argument from capture_info.json
  order  <n> <arm>...                                       interleaved arm order (n rounds, each round rotated by one)
  score  <run dir>...                                       summary.json per run (rx.log + time.txt + arm.txt in the dir), one line each
  verdict <out root> [--min-dl N]                           verdict.json + verdict.txt over every run dir below <out root>

Every number is per cell and per run (never pooled across cells, hosts, captures); evidence label of a verdict is the caller's
([MEASURED, DGX replay, cell PCI .., n=..]). The thresholds are plan 2026-10-23-mpci-main-plan.md Global Constraint 6 (P1, P3) and gate V3.
"""
import glob, json, os, re, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import cellsel          # noqa: E402
import goal_metrics as gm  # noqa: E402

# Cell tunings. Source: tests/passive_rx/mpci/inventory/live_tuned_20261023.json "_comment" = PROJECT_MEMORY section 25 block 2026-10-23/24 item 4
# [MEASURED, DGX OTA 2026-10-23, receiver-side detections]: 508 -C 3590010000 --ssb 193 -r 162 fo -4868; 403 -C 3630000000 --ssb 18 -r 51 fo +2178.
# centre_hz, n_rb_hint and cfo_hz are cross-checked against the inventory entries when cells.json is built; --ssb exists only in the comment.
# (Survey c3610, PROJECT_MEMORY section 25 item 25: 508 ARFCN 7895 / 3570.24 MHz cfo -4761, 403 7933 / 3624.96 MHz cfo +2225: the
# GSCN/SSB frequency agree with the inventory entries, the CFO differs by 107 / 47 Hz, inside the tracker pull-in.)
TUNE = {
    508: dict(id="pci508", centre_hz=3590010000, ssb=193, n_rb=162, fo=-4868),
    403: dict(id="pci403", centre_hz=3630000000, ssb=18, n_rb=51, fo=2178),
}

# P1 / P3 limits (plan Global Constraint 6; user 2026-10-17 for P3)
P1 = dict(dl_p50=1.2, dl_p99=1.5, crc_q=0.95, drops_abs=4.0, drops_delta=1.0, ul_p50=1.2)
P3_RATIO = 1.25
MIN_DL = 200        # fewer real DL grants than this in a run: the percentiles are not a measurement
MIN_UL = 20


def load_json(p):
    with open(p) as f:
        return json.load(f)


def make_cells(inv_path, cap_dir, pcis):
    inv = load_json(inv_path)
    ci = load_json(os.path.join(cap_dir, "capture_info.json"))
    by_id = {c["id"]: c for c in inv["cells"]}
    out = []
    for p in pcis:
        t = TUNE[p]
        c = dict(by_id[t["id"]])
        for key, want in (("centre_hz", t["centre_hz"]), ("n_rb_hint", t["n_rb"]), ("cfo_hz", t["fo"]), ("pci", p)):
            if c[key] != want:
                raise SystemExit("inventory %s %s=%r differs from the harness tuning %r" % (t["id"], key, c[key], want))
        out.append(c)
    return {"schema": inv.get("schema", 1),
            "window": {"centre_hz": float(ci["rf_channels"][0]["frequency_hz"]), "rate_msps": ci["sample_rate_hz"] / 1e6, "n_rx": ci["channels"]},
            "cells": out}


def cli_args(pci):
    t = TUNE[pci]
    return "-C %d --ssb %d -r %d --initial-fo %d" % (t["centre_hz"], t["ssb"], t["n_rb"], t["fo"])


def pub_src(ring, cap_dir):
    d = load_json(os.path.join(cap_dir, "capture_info.json"))
    return "%s=%s,%d,%d,%d,%d,%d,2" % (ring, cap_dir, d["samples_per_channel"], d["first_sample_tick"], d["channels"], d["sample_rate_hz"],
                                         d["rf_channels"][0]["frequency_hz"])


def order(n, arms):
    """n rounds over the arms; round r is the arm list rotated right by r (pair,508,403 / 403,pair,508 / 508,403,pair ...)."""
    k = len(arms)
    return [arms[(i - r) % k] for r in range(n) for i in range(k)]


# ------------------------------------------------------------------ one run
RX_POOL_CLS = re.compile(r"\b(scan|decode|retry|disc|house|chan)\[n=(\d+) waited=(\d+) promo=(\d+) wait_us p50<=(\d+) p99<=(\d+) max=(\d+)\]")
RX_TICKET = re.compile(r"\bc(\d+)\[grants=(\d+) contended=(\d+) wait_ms=(\d+) wait_max_ms=(\d+) held_ms=(\d+) held_max_ms=(\d+) pace_ms=(\d+)\]")
RX_CPU = re.compile(r"CELL_CPU cell=(\d+) pci=(\d+) win_ms=(\d+) tokens_avg=([\d.]+) share_pct=([\d.]+) fair_pct=([\d.]+) wait_p99_us=(\d+)")
RX_BAD = re.compile(r"SENSING: (\S+) .*?\bcheck_bad=(\d+)")
RX_FEED = re.compile(r"MPCI_FEED rate=.*")


def parse_time(text):
    """/usr/bin/time -v output -> dict(user, sys, wall, rss_kb)."""
    def num(rx):
        m = re.search(rx, text)
        return float(m.group(1)) if m else None
    wall = None
    m = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): (?:(\d+):)?(\d+):([\d.]+)", text)
    if m:
        wall = (int(m.group(1) or 0) * 3600) + int(m.group(2)) * 60 + float(m.group(3))
    return dict(user=num(r"User time \(seconds\): ([\d.]+)"), sys=num(r"System time \(seconds\): ([\d.]+)"), wall=wall,
                rss_kb=num(r"Maximum resident set size \(kbytes\): (\d+)"))


def kv_int(line, key):
    m = re.search(r"\b%s=(\d+)" % key, line)
    return int(m.group(1)) if m else None


def process_facts(raw):
    """Process-wide facts of a log (all lines, tagged or not): pool, tickets, feed, overruns, check_bad."""
    P = dict(shmiq_overrun=0, iq_chan_overrun=0, chan_overruns=None, chan_discont=None, chan_lag_max=None, feed_cells=None, feed_line=None,
             pool=None, pool_line=None, ticket=None, cell_cpu={}, check_bad_total=0, check_bad_by={}, ring_recover=0)
    last_bad = {}
    for l in raw:
        if "SHMIQ_OVERRUN" in l:
            P["shmiq_overrun"] += 1
        if "IQ_CHAN_OVERRUN" in l:
            P["iq_chan_overrun"] += 1
        if "USRP_RING_RECOVER" in l or "USRP_RING_RESTART" in l:
            P["ring_recover"] += 1
        m = RX_FEED.search(l)
        if m:
            P["feed_line"] = m.group(0)[:360]
            P["chan_overruns"] = kv_int(l, "chan_overruns")
            P["chan_discont"] = kv_int(l, "chan_discont")
            P["chan_lag_max"] = kv_int(l, "chan_lag_max")
            mc = re.search(r"cells=(\S*)", l)
            P["feed_cells"] = {}
            for part in (mc.group(1).split(";") if mc and mc.group(1) else []):
                mm = re.match(r"(\w+):ov=(\d+),drop=(\d+),lag=(\d+)", part)
                if mm:
                    P["feed_cells"][mm.group(1)] = dict(ov=int(mm.group(2)), drop=int(mm.group(3)), lag=int(mm.group(4)))
        if "CORE_POOL n=" in l and "yields=" in l:
            P["pool_line"] = l[l.find("CORE_POOL"):][:500]
            P["pool"] = {m2.group(1): dict(n=int(m2.group(2)), waited=int(m2.group(3)), p99_us=int(m2.group(6)), max_us=int(m2.group(7)))
                         for m2 in RX_POOL_CLS.finditer(l)}
        if "DISC_TICKET" in l:
            P["ticket"] = {int(m2.group(1)): dict(grants=int(m2.group(2)), contended=int(m2.group(3)), wait_ms=int(m2.group(4)), wait_max_ms=int(m2.group(5)),
                                                  pace_ms=int(m2.group(8))) for m2 in RX_TICKET.finditer(l)}
        m = RX_CPU.search(l)
        if m:
            P["cell_cpu"].setdefault(int(m.group(1)), []).append(float(m.group(4)))
        m = RX_BAD.search(l)
        if m and "check_bad=" in l:
            tag = cellsel.CELL_TAG.match(l)
            last_bad[(tag.group(2) if tag else "-", m.group(1))] = int(m.group(2))   # cumulative counters: the last value per (cell tag, counter)
    P["check_bad_by"] = {"%s:%s" % k: v for k, v in sorted(last_bad.items()) if v}
    P["check_bad_total"] = sum(last_bad.values())
    return P


def cell_metrics(path, cell, pci_expected):
    r = gm.parse_run(path, 2.0, cell)
    dl, ul = r["dl"], r["ul"]
    return dict(pci=pci_expected, dl_n=dl["n_real"], dl_p50=dl["tot_p50"], dl_p99=dl["tot_p99"], crc_q=dl["crc_q"], crc_all=dl["crc_all"],
                drops=dl["drops_pct"], ul_n=ul["n_real"], ul_p50=ul["tot_p50"], ul_p99=ul["tot_p99"])


def score_run(rundir, arm=None, expected=None):
    """expected: PCIs in cell order (cell 0 first); from arm.txt / cells.json of the run dir when omitted."""
    log = os.path.join(rundir, "rx.log")
    if arm is None:
        with open(os.path.join(rundir, "arm.txt")) as f:
            arm = f.read().split()[0]
    if expected is None:
        expected = [c["pci"] for c in load_json(os.path.join(rundir, "cells.json"))["cells"]]
    raw = [cellsel.ANSI.sub("", l) for l in cellsel.read_lines(log)]
    info = cellsel.cell_info(log)
    out = dict(run=rundir, arm=arm, expected_pcis=expected, cells={}, time=None)
    if info is None:   # one cell: untagged log
        out["cells"][str(expected[0])] = cell_metrics(log, None, expected[0])
        out["cell_slots"] = None
    else:
        slots = {}
        for k in info["cells"]:
            pci = info["pci"].get(k)
            if pci is None and k < len(expected):
                pci = expected[k]
            slots[k] = pci
            out["cells"][str(pci)] = cell_metrics(log, k, pci)
            out["cells"][str(pci)]["slot"] = k
            if k < len(expected) and pci != expected[k]:
                out["cells"][str(pci)]["pci_mismatch_expected"] = expected[k]
        out["cell_slots"] = slots
    out["process"] = process_facts(raw)
    tp = os.path.join(rundir, "time.txt")
    if os.path.exists(tp):
        with open(tp) as f:
            t = parse_time(f.read())
        t["cpu_s"] = (t["user"] or 0) + (t["sys"] or 0) if t["user"] is not None else None
        t["busy_cores"] = t["cpu_s"] / t["wall"] if t["cpu_s"] is not None and t["wall"] else None
        out["time"] = t
    return out


# ------------------------------------------------------------------ verdict
def med(vals):
    v = [x for x in vals if x is not None]
    return statistics.median(v) if v else None


def ratio(a, b):
    return a / b if a is not None and b else None


def chk(name, pair, solo, limit, ok, note=""):
    return dict(check=name, pair=pair, solo=solo, limit=limit, ok=ok, note=note)


def p1_cell(pci, pair_runs, solo_runs, min_dl=MIN_DL):
    pc = [r["cells"][str(pci)] for r in pair_runs if str(pci) in r["cells"]]
    sc = [r["cells"][str(pci)] for r in solo_runs if str(pci) in r["cells"]]
    res = dict(pci=pci, n_pair=len(pc), n_solo=len(sc), checks=[])
    if not pc or not sc:
        res["checks"].append(chk("data", len(pc), len(sc), "both arms present", False, "no run of this cell in one of the arms"))
        res["ok"] = False
        return res
    m = {k: (med([c[k] for c in pc]), med([c[k] for c in sc])) for k in ("dl_n", "dl_p50", "dl_p99", "crc_q", "drops", "ul_n", "ul_p50")}
    C = res["checks"]
    thin = [x for x in pc + sc if (x["dl_n"] or 0) < min_dl]
    C.append(chk("DL grants per run", m["dl_n"][0], m["dl_n"][1], ">= %d in every run" % min_dl, not thin,
                 "%d run(s) below" % len(thin) if thin else ""))
    for key, lim in (("dl_p50", P1["dl_p50"]), ("dl_p99", P1["dl_p99"])):
        r = ratio(*m[key])
        C.append(chk(key + " (us, median of runs)", m[key][0], m[key][1], "<= %.2f x solo" % lim, r is not None and r <= lim, "ratio %s" % ("%.2f" % r if r is not None else "n/a")))
    r = ratio(*m["crc_q"])
    C.append(chk("crc_q (%)", m["crc_q"][0], m["crc_q"][1], ">= %.0f%% of solo" % (100 * P1["crc_q"]), r is not None and r >= P1["crc_q"], "ratio %s" % ("%.3f" % r if r is not None else "n/a")))
    d, s = m["drops"]
    C.append(chk("drops (%)", d, s, "<= %.0f and <= solo + %.0f" % (P1["drops_abs"], P1["drops_delta"]),
                 d is not None and s is not None and d <= P1["drops_abs"] and d <= s + P1["drops_delta"]))
    if (m["ul_n"][1] or 0) >= MIN_UL:   # UL locked in the solo arm: it must be there in the pair arm too
        r = ratio(*m["ul_p50"])
        C.append(chk("UL locked p50 (us)", m["ul_p50"][0], m["ul_p50"][1], "<= %.2f x solo" % P1["ul_p50"], r is not None and (m["ul_n"][0] or 0) >= MIN_UL and r <= P1["ul_p50"],
                     "ratio %s, UL n pair/solo %s/%s" % ("%.2f" % r if r is not None else "n/a", m["ul_n"][0], m["ul_n"][1])))
    else:
        C.append(chk("UL locked p50 (us)", m["ul_p50"][0], m["ul_p50"][1], "skipped: no UL in the solo arm (n=%s)" % m["ul_n"][1], True, "not evaluated"))
    res["ok"] = all(c["ok"] for c in C)
    return res


def binding(pair_runs, solo_runs):
    """Which resource the pair arm hits (heuristic thresholds, [INFERRED]; the lines are quoted)."""
    ev, cand = [], []
    def worst(runs, f):
        v = [f(r["process"]) for r in runs]
        v = [x for x in v if x is not None]
        return max(v) if v else None
    chan = [("chan_overruns", worst(pair_runs, lambda p: p["chan_overruns"])), ("chan_discont", worst(pair_runs, lambda p: p["chan_discont"])),
            ("SHMIQ_OVERRUN lines", worst(pair_runs, lambda p: p["shmiq_overrun"])), ("IQ_CHAN_OVERRUN lines", worst(pair_runs, lambda p: p["iq_chan_overrun"]))]
    cell_ov = max([c["ov"] + c["drop"] for r in pair_runs for c in ((r["process"]["feed_cells"] or {}).values())] or [0])
    if cell_ov:
        chan.append(("per-cell ring ov+drop", cell_ov))
    if any(v for _, v in chan):
        cand.append("channelizer")
        ev.append("MPCI_FEED: " + ", ".join("%s=%s" % x for x in chan if x[1]))
    def waited_frac(runs):
        out = {}
        for r in runs:
            for cls, d in (r["process"]["pool"] or {}).items():
                if d["n"]:
                    out[cls] = max(out.get(cls, 0), d["waited"] / d["n"])
        return out
    pf, sf = waited_frac(pair_runs), waited_frac(solo_runs)
    tripped = [(cls, f, sf.get(cls, 0)) for cls, f in pf.items() if cls != "chan" and f > max(0.05, 2 * sf.get(cls, 0))]
    p99 = max([d["p99_us"] for r in pair_runs for cls, d in (r["process"]["pool"] or {}).items() if cls != "chan"] or [0])
    if tripped:
        cand.append("cores")
        ev.append("CORE_POOL waited/leases pair vs solo: " + ", ".join("%s %.1f%% vs %.1f%%" % (c, 100 * a, 100 * b) for c, a, b in tripped) + "; worst class wait p99<=%d us" % p99)
    tk = [t for r in pair_runs for t in ((r["process"]["ticket"] or {}).values())]
    g, cont, pace = sum(t["grants"] for t in tk), sum(t["contended"] for t in tk), sum(t["pace_ms"] for t in tk)
    if g and (cont / g > 0.2 or pace > 0):
        cand.append("discovery")
        ev.append("DISC_TICKET grants=%d contended=%d (%.0f%%) pace_ms=%d wait_ms=%d" % (g, cont, 100.0 * cont / g, pace, sum(t["wait_ms"] for t in tk)))
    if any(r["process"]["check_bad_total"] for r in pair_runs):
        cand.append("gpu")
        ev.append("check_bad > 0")
    if not cand:
        ev.append("no tripped indicator in CORE_POOL / DISC_TICKET / MPCI_FEED lines: undetermined (check the pool line quoted below)")
    return dict(candidates=cand, binding=cand[0] if cand else "undetermined", evidence=ev,
                pool_line=next((r["process"]["pool_line"] for r in pair_runs if r["process"]["pool_line"]), None),
                feed_line=next((r["process"]["feed_line"] for r in pair_runs if r["process"]["feed_line"]), None))


def verdict(runs, min_dl=MIN_DL):
    arms = {}
    for r in runs:
        arms.setdefault(r["arm"], []).append(r)
    out = dict(arms={a: len(v) for a, v in sorted(arms.items())}, cells={}, p3=None, gpu=None, overruns=None, problems=[])
    pair = arms.get("pair", [])
    solos = {508: arms.get("solo508", []), 403: arms.get("solo403", [])}
    for pci, sr in solos.items():
        out["cells"][str(pci)] = p1_cell(pci, pair, sr, min_dl)
    # P3: busy cores of the pair process vs the single-cell process (the heavier of the two solo arms = today's single-cell core set)
    bc = lambda runs: [r["time"]["busy_cores"] for r in runs if r.get("time") and r["time"].get("busy_cores") is not None]
    pb, s508, s403 = med(bc(pair)), med(bc(solos[508])), med(bc(solos[403]))
    solo_ref = max([x for x in (s508, s403) if x is not None] or [None]) if (s508 is not None or s403 is not None) else None
    r3 = ratio(pb, solo_ref)
    out["p3"] = dict(pair_busy_cores=pb, solo508_busy_cores=s508, solo403_busy_cores=s403, ref="max of the two solo medians", ratio=r3, ratio_vs_508=ratio(pb, s508),
                     ratio_vs_403=ratio(pb, s403), limit=P3_RATIO, ok=r3 is not None and r3 <= P3_RATIO, n=[len(bc(pair)), len(bc(solos[508])), len(bc(solos[403]))],
                     cpu_s=dict(pair=med([r["time"]["cpu_s"] for r in pair if r.get("time")]), solo508=med([r["time"]["cpu_s"] for r in solos[508] if r.get("time")]),
                                solo403=med([r["time"]["cpu_s"] for r in solos[403] if r.get("time")])),
                     pool_tokens_avg_pair=med([sum(statistics.median(v) for v in r["process"]["cell_cpu"].values()) for r in pair if r["process"]["cell_cpu"]]))
    allr = [r for v in arms.values() for r in v]
    bad = {r["run"]: r["process"]["check_bad_total"] for r in allr if r["process"]["check_bad_total"]}
    out["gpu"] = dict(check_bad_by_run=bad, ok=not bad)
    ov = {}
    for r in pair:
        p = r["process"]
        n = (p["chan_overruns"] or 0) + p["shmiq_overrun"] + p["iq_chan_overrun"]
        if n:
            ov[r["run"]] = dict(chan_overruns=p["chan_overruns"], shmiq_overrun=p["shmiq_overrun"], iq_chan_overrun=p["iq_chan_overrun"])
    out["overruns"] = dict(pair_runs_with_overruns=ov, ok=not ov and bool(pair),
                           solo_runs_with_overruns={r["run"]: r["process"]["shmiq_overrun"] for a in ("solo508", "solo403") for r in arms.get(a, []) if r["process"]["shmiq_overrun"]})
    for a in ("pair", "solo508", "solo403"):
        if len(arms.get(a, [])) < 3:
            out["problems"].append("arm %s has n=%d (< 3)" % (a, len(arms.get(a, []))))
    for r in allr:
        for pci, c in r["cells"].items():
            if c.get("pci_mismatch_expected") is not None:
                out["problems"].append("%s: slot %s locked PCI %s, expected %s" % (r["run"], c.get("slot"), pci, c["pci_mismatch_expected"]))
    out["p1_ok"] = all(c["ok"] for c in out["cells"].values())
    out["ok"] = out["p1_ok"] and out["p3"]["ok"] and out["gpu"]["ok"] and out["overruns"]["ok"] and not out["problems"]
    out["binding"] = None if out["ok"] else binding(pair, solos[508] + solos[403])
    return out


def fmt(x):
    return "n/a" if x is None else ("%.1f" % x if isinstance(x, float) else str(x))


def text(v, label="[MEASURED, DGX replay c3610 l0942_c3610x8]"):
    L = ["V3 verdict %s   arms n: %s" % (label, v["arms"]), ""]
    for pci, c in sorted(v["cells"].items()):
        L.append("P1 cell PCI %s (pair n=%d vs its own solo n=%d): %s" % (pci, c["n_pair"], c["n_solo"], "PASS" if c["ok"] else "FAIL"))
        for k in c["checks"]:
            L.append("   %-4s %-26s pair=%-10s solo=%-10s limit %s %s" % ("ok" if k["ok"] else "FAIL", k["check"], fmt(k["pair"]), fmt(k["solo"]), k["limit"], k["note"]))
    p = v["p3"]
    L += ["", "P3 busy cores (user+sys CPU s / wall s, /usr/bin/time -v): pair %s, solo508 %s, solo403 %s; ratio vs heavier solo %s (vs508 %s, vs403 %s) limit %.2f: %s" % (
        fmt(p["pair_busy_cores"]), fmt(p["solo508_busy_cores"]), fmt(p["solo403_busy_cores"]), fmt(p["ratio"]), fmt(p["ratio_vs_508"]), fmt(p["ratio_vs_403"]), p["limit"], "PASS" if p["ok"] else "FAIL"),
          "   cpu_s %s  pool tokens (sum of CELL_CPU tokens_avg, pair) %s  n pair/508/403 %s" % (p["cpu_s"], fmt(p["pool_tokens_avg_pair"]), p["n"]),
          "GPU check_bad == 0 in every run: %s %s" % ("PASS" if v["gpu"]["ok"] else "FAIL", v["gpu"]["check_bad_by_run"] or ""),
          "channelizer overruns == 0 in every pair run: %s %s" % ("PASS" if v["overruns"]["ok"] else "FAIL", v["overruns"]["pair_runs_with_overruns"] or ""),
          "solo runs with SHMIQ_OVERRUN (baseline contaminated if any): %s" % (v["overruns"]["solo_runs_with_overruns"] or "none")]
    for pr in v["problems"]:
        L.append("PROBLEM: " + pr)
    L.append("")
    L.append("V3 OVERALL: %s" % ("PASS" if v["ok"] else "FAIL"))
    if v["binding"]:
        b = v["binding"]
        L.append("binding resource: %s (candidates in order: %s)" % (b["binding"], ", ".join(b["candidates"]) or "none"))
        for e in b["evidence"]:
            L.append("   " + e)
        if b["pool_line"]:
            L.append("   CORE_POOL: " + b["pool_line"])
        if b["feed_line"]:
            L.append("   " + b["feed_line"])
    return "\n".join(L)


def find_runs(root):
    runs = []
    for p in sorted(glob.glob(os.path.join(root, "*", "summary.json"))):
        runs.append(load_json(p))
    return runs


def main(a):
    if not a:
        print(__doc__)
        return 2
    c = a[0]
    if c == "cells":
        print(json.dumps(make_cells(a[1], a[2], [int(x) for x in a[3:]]), indent=1))
    elif c == "cli":
        print(cli_args(int(a[1])))
    elif c == "src":
        print(pub_src(a[1], a[2]))
    elif c == "order":
        print(" ".join(order(int(a[1]), a[2:])))
    elif c == "score":
        for d in a[1:]:
            s = score_run(d)
            with open(os.path.join(d, "summary.json"), "w") as f:
                json.dump(s, f, indent=1)
            print("%s arm=%s %s busy_cores=%s check_bad=%d chan_overruns=%s" % (
                d, s["arm"], " ".join("pci%s[dl n=%s p50=%s p99=%s crc_q=%s drops=%s ul n=%s p50=%s]" % (k, v["dl_n"], fmt(v["dl_p50"]), fmt(v["dl_p99"]), fmt(v["crc_q"]), fmt(v["drops"]), v["ul_n"], fmt(v["ul_p50"]))
                                      for k, v in sorted(s["cells"].items())),
                fmt((s["time"] or {}).get("busy_cores")), s["process"]["check_bad_total"], s["process"]["chan_overruns"]))
    elif c == "verdict":
        min_dl = int(a[a.index("--min-dl") + 1]) if "--min-dl" in a else MIN_DL
        runs = find_runs(a[1])
        v = verdict(runs, min_dl)
        with open(os.path.join(a[1], "verdict.json"), "w") as f:
            json.dump(v, f, indent=1)
        t = text(v)
        with open(os.path.join(a[1], "verdict.txt"), "w") as f:
            f.write(t + "\n")
        print(t)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
