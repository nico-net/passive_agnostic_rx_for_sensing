#!/usr/bin/env python3
"""Score the traffic-mode OTA A/B (legacy L vs traffic T). Usage: score_traffic_ab.py <campaign_dir>
Reads <campaign>/runs.tsv (tag, rundir, pcis, ok), each run.log (last ISAC_METRICS line per cell), the records
files (base = cell 0, .cell<k> = cell k) and <rundir>/cpu_cores.txt (mean cores of the process). Per case and
per cell, never pooled. Field agreement is n/a OTA (no identical (abs_slot,rnti) across runs)."""
import json, os, re, statistics as st, sys

def parse_tag(tag):
    m = re.match(r"^tm([LT])\d(S\d+|B)r?$", tag)
    return (m.group(1), m.group(2)) if m else None

def parse_metrics(text):
    out = {}
    for ln in re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines():
        i = ln.find("ISAC_METRICS ")
        if i < 0: continue
        try: d = json.loads(ln[i + 13:])
        except ValueError: continue  # truncated line
        out[d.get("cell", 0)] = d
    return out

def scan_records(path):
    """-> (solved RNTIs of type=dci, distinct non-SI RNTIs of type=grant)"""
    solved, grant = set(), set()
    if not os.path.exists(path): return 0, 0
    with open(path, errors="replace") as f:
        for ln in f:
            if '"type":"dci"' in ln:
                try: d = json.loads(ln)
                except ValueError: continue
                if d.get("layout_state") == "solved": solved.add(d.get("rnti"))
            elif '"type":"grant"' in ln:
                try: d = json.loads(ln)
                except ValueError: continue
                if d.get("rnti") != 65535: grant.add(d.get("rnti"))
    return len(solved), len(grant)

def cell_stats(rundir, arm, cell, met):
    base = os.path.join(rundir, "sense_records_last.jsonl")
    rec = base if cell == 0 else base.replace(".jsonl", ".cell%d.jsonl" % cell)
    sd, gr = scan_records(rec)
    acc = sum(met.get("dci_accepted_" + k, 0) for k in ("10", "11", "00", "01")) if "dci_accepted_10" in met else None  # legacy has no such keys
    try: cpu = float(open(os.path.join(rundir, "cpu_cores.txt")).read())
    except (OSError, ValueError): cpu = None
    queued = met.get("scanq_queued")
    drop = sum(met.get("scanq_drop_" + k, 0) for k in ("full", "stale", "epoch"))
    return dict(queued=queued, drop_pct=100.0 * drop / queued if queued else None, acc=acc, gen=met.get("dci_rec_generated"), lost=met.get("dci_rec_lost"),
                solved=sd if arm == "T" else gr,
                drop=drop,
                cpu=cpu, emit_p99=met.get("dci_emit_us_p99"))

def ms(runs, k):
    v = [r[k] for r in runs if r.get(k) is not None]
    return (st.mean(v), st.stdev(v) if len(v) > 1 else 0.0) if v else (None, None)

def evaluate(L, T):
    """L, T: lists of per-run stat dicts for one (case, cell). -> [(bound, pass|None, detail)]"""
    out = []
    for r in L + T: r.setdefault("drop_pct", None)
    bad = [r for r in T if r["acc"] != r["gen"]]
    out.append(("accepted==generated", not bad, "%d/%d runs differ" % (len(bad), len(T))))
    lost = [r["lost"] for r in T]
    out.append(("rec_lost==0", all(x == 0 for x in lost), "lost=%s" % lost))
    # arms count different things (T: solved dci RNTIs; L: non-SI grant RNTIs): info only
    out.append(("solved RNTIs", None, "n/a (not comparable); T=%s L=%s info" % (fmt(ms(T, "solved")[0]), fmt(ms(L, "solved")[0]))))
    (dl, dls), (dt, _) = ms(L, "drop_pct"), ms(T, "drop_pct")
    out.append(("scanq drop_pct<=L+sd", None if dl is None or dt is None else dt <= dl + dls,
                "T=%s%% L=%s%% sd=%s" % (fmt(dt), fmt(dl), fmt(dls))))
    (cl, cls), (ct, _) = ms(L, "cpu"), ms(T, "cpu")  # confound: T does less work (no CFR/PRACH/SRS, less LDPC)
    out.append(("cpu T<L-sd", None if cl is None or ct is None else ct < cl - cls,
                "T=%s L=%s sd=%s cores (process)" % (fmt(ct), fmt(cl), fmt(cls))))
    out.append(("field agreement>=99%", None, "n/a OTA"))
    return out

def fmt(x): return "n/a" if x is None else "%.1f" % x

def main(camp):
    cases = {}
    for ln in open(os.path.join(camp, "runs.tsv")):
        f = ln.rstrip("\n").split("\t")
        if len(f) < 4: continue
        tag, rd, pcis, ok = f[:4]
        pt = parse_tag(tag)
        if not pt or ok != "OK":
            print("# excluded: %s (%s)" % (tag, ok)); continue
        arm, case = pt
        met = parse_metrics(open(os.path.join(rd, "run.log"), errors="replace").read())
        for k, pci in enumerate(pcis.split()):
            if k not in met: print("# no ISAC_METRICS cell %d in %s" % (k, tag)); continue
            cases.setdefault((case, pci), {"L": [], "T": []})[arm].append(cell_stats(rd, arm, k, met[k]))
    fails = 0
    for (case, pci), a in sorted(cases.items()):
        print("\n== case %s cell PCI %s: L runs=%d T runs=%d" % (case, pci, len(a["L"]), len(a["T"])))
        for k in ("queued", "acc", "gen", "lost", "solved", "drop", "drop_pct", "cpu", "emit_p99"):
            print("  %-9s L mean=%s sd=%s | T mean=%s sd=%s" % ((k,) + tuple(fmt(x) for x in ms(a["L"], k) + ms(a["T"], k))))
        for arm in ("L", "T"):
            print("  per-run %s: " % arm + "; ".join("q=%s drop=%s (%s%%) cpu=%s" % (r["queued"], r["drop"], fmt(r["drop_pct"]), r["cpu"]) for r in a[arm]))
        if not a["L"] or not a["T"]: print("  (missing arm, not scored)"); continue
        for b, ok, d in evaluate(a["L"], a["T"]):
            fails += ok is False
            print("  %-22s %-4s %s" % (b, {True: "PASS", False: "FAIL", None: "n/a"}[ok], d))
    print("\nfailed bounds: %d" % fails)

if __name__ == "__main__":
    main(sys.argv[1])
