#!/usr/bin/env python3
"""Paper statistics for the agnostic receiver, accumulated from the receiver's OWN log lines.

Every number here is traced to one log line the receiver prints (the regex names the line). Where
the receiver does not yet print what a statistic needs, the snapshot carries an explicit
`gap` string instead of a number, so the dashboard shows "not instrumented: needs <line>" rather
than a plausible zero.

Denominators are COMPLETED JOBS from PDSCHQ (one job = one grant decode), never decoder retries:
LDPCDIAG's seg counters are summed only over failing TBs and BRANCHSEL/RVRETRY are retries of the
same OTA TB, so neither may be a denominator.

Offline check against a real log:   python3 paper_stats.py run.log
"""
import json
import re
import sys
import time

# ponytail: timing is wall-clock at ingest. The receiver stamps no time on any SENSING line, so
# sample-time acquisition latency needs a frame/slot stamp on ACQ_EVENT; until then, only lines
# ingested LIVE (not seeded from history) get a time, and the snapshot says which clock it is.
GAP_SAMPLE_TIME = "not instrumented: needs frame/slot stamp on ACQ_EVENT lines (wall-clock shown)"
GAP_UNIQUE_TB = "not instrumented: needs a per-grant (rnti,harq_pid,ndi) line or UNIQUE_TB counter in PDSCHQ"
GAP_ORACLE = "not instrumented: needs a manual-oracle DCI file (gNB log) to compare against"
GAP_BRANCH = "not instrumented: needs per-branch TB CRC on EVERY grant (BRANCHSEL only retries failures)"
GAP_STALE = "not instrumented: needs a STALE_CONFIG_REUSED counter after ACQ_STATE -> LOST"
GAP_GAPS = "not run: needs the injected-gap experiment (50 ms / 500 ms / 3 s / 10 s)"
GAP_P95 = "PDTIM prints mean/max only: p95 needs a latency histogram line"
GAP_RTF = "not instrumented: real-time factor needs a slots-processed vs wall-clock line"

R = {
    "pss":      re.compile(r"Cell Detected with GSCN: (\d+), SSB SC offset: (\d+), SSB Ref: ([\d.]+)"),
    "cfo":      re.compile(r"Measured Carrier Frequency offset (-?\d+) Hz"),
    "pbch":     re.compile(r"ACQ_EVENT pbch_locked \(n=(\d+)"),
    "sib1":     re.compile(r"ACQ_EVENT sib1_decoded"),
    "pci":      re.compile(r"SIB1 common facts PCI=(\d+) DL-BWP=(\d+)\+(\d+)"),
    "carrier":  re.compile(r"ACQ carrier CONFIRMED from SIB1: (\d+) PRB mu=(\d+).*?derived carrier centre ([\d.]+) MHz"),
    "mismatch": re.compile(r"ACQ carrier MISMATCH|VOID_CFO_MISLOCK"),
    "coreset":  re.compile(r"CORESET VERIFIED by fresh dedicated DCI: offset=(\d+) span=(\d+)"),
    "cs_cand":  re.compile(r"CORESET candidate (\d+)/(\d+)"),
    "len_unres": re.compile(r"DCI length unresolved after (\d+) occasions"),
    "armed":    re.compile(r"DCI11_LAYOUT armed: (\d+) layouts consistent with dci_length=(\d+)"),
    "alive":    re.compile(r"DCI11_LAYOUT n=(\d+) observed \| (\d+) of (\d+) layouts"),
    "preferred": re.compile(r"PREFERRED by TB CRC: rnti=0x([0-9a-f]+) layout_id=(\d+) \((\d+) candidates\) passes=(\d+)"),
    "s2reject": re.compile(r"STAGE2 reject: .*?rejected=(\d+)"),
    "unsup":    re.compile(r"too many segments"),
    "pdschq":   re.compile(r"PDSCHQ queued=(\d+) decoded=(\d+) crc_ok=(\d+).*?dropped\[full=(\d+) stale=(\d+)\] max_lag_slots=(\d+)/(\d+)"),
    "ldpc":     re.compile(r"LDPCDIAG ok=(\d+) seg_fail=(\d+) tb_fail=(\d+) zero_tb=(\d+) iface_err=(\d+)"),
    "harqc":    re.compile(r"HARQC first=(\d+) retx_combined=(\d+)/(\d+).*?mcs<24 \[([\d ]+)\] mcs>=24 \[([\d ]+)\]"),
    "mcshist":  re.compile(r"MCSHIST (\S+) crc_ok% by mcs: (.*)"),
    "rbhist":   re.compile(r"RBHIST crc_ok% by PRB alloc: (.*)"),
    "branch":   re.compile(r"BRANCHSEL rescued=(\d+)/(\d+) retries \[(.*?)\]"),
    "pdtim":    re.compile(r"PDTIM calls=(\d+) (.*)"),
    "btim":     re.compile(r"BTIM (fep_llr.*)"),
    "slotshare": re.compile(r"SLOTSHARE fep hit/miss=(\d+)/(\d+) chest hit/miss=(\d+)/(\d+)"),
    "rfstall":  re.compile(r"RFSTALL"),
    "lost":     re.compile(r"ACQ_STATE (\w+) -> LOST"),
    "state":    re.compile(r"ACQ_STATE (\w+) -> (\w+) \(updates=(\d+) time_in_prev=(\d+)"),
    "oracle":   re.compile(r"DMRS_ORACLE slot="),
    "probe":    re.compile(r"LAYOUT_PROBE n=(\d+) cb0_ok=(\d+)"),
    "techd":    re.compile(r"Technique D CONVERGED rnti=0x([0-9a-f]+) tda=(\d+) S=(\d+) L=(\d+)"),
    "parmset":  re.compile(r"PARMSET overflow=(\d+)"),
    "ss":       re.compile(r"SEARCH_SPACE INFERRED .*?confirmed/examined\[(.*?)\] total_confirmed=(\d+)"),
    "ssreg":    re.compile(r"SS_REGISTRY live=(\d+)/(\d+)"),
}
STAGE = re.compile(r"(\w+)\[n=(\d+) mean=([\d.]+)us max=([\d.]+)us")
MCS = re.compile(r"(\d+):(\d+)%\((\d+),")
RB = re.compile(r"(\d+-\d+):(\d+)%\((\d+)\)")
BR = re.compile(r"(\d+):(\d+)/(\d+)")

EVENTS = ["pss", "pbch", "sib1", "coreset", "dci", "first_tb"]


def pct(vals, q):
    v = sorted(x for x in vals if x is not None)
    if not v:
        return None
    return v[min(len(v) - 1, int(round(q * (len(v) - 1))))]


class PaperStats:
    def __init__(self):
        self.runs = []          # closed + current run records
        self.cur = None
        self.rnti = {}          # rnti -> sightings (persistence)

    # ---- run lifecycle ------------------------------------------------------------------------
    def begin_run(self, path, live):
        """A new run.log. `live` = lines will be stamped (False while seeding a file's history)."""
        self.end_run()
        self.cur = {"path": str(path), "live": live, "t0": None, "ev": {k: None for k in EVENTS},
                    "false_lock": False, "rfstall": 0, "lost": [], "relock_s": [], "_lost_at": None,
                    "pbch_locks": 0, "cs_cand": None, "len_unres": 0,
                    "armed": None, "alive": [], "preferred": None, "s2reject": 0, "unsup": 0,
                    "pdschq": None, "pdschq_at_pref": None, "ldpc": None, "harqc": None,
                    "mcs": {}, "rb": [], "branch": None, "pdtim": None, "btim": None,
                    "slotshare": None, "oracle": 0, "probe": None, "techd": None, "parmset_overflow": 0,
                    "ss": None, "ssreg": None, "ident": {}, "ended": False}
        self.runs.append(self.cur)
        return self.cur

    def end_run(self):
        if self.cur:
            self.cur["ended"] = True
        self.cur = None

    # ---- per line -----------------------------------------------------------------------------
    def ingest(self, line, now=None):
        c = self.cur
        if c is None:
            return
        if "rnti_seen" in line:
            k = line.rsplit("rnti=0x", 1)[-1][:4]
            self.rnti[k] = self.rnti.get(k, 0) + 1
            return
        if "SENSING" not in line and "PASSIVE:" not in line and "Cell Detected" not in line and "Measured Carrier" not in line:
            return
        now = now if now is not None else time.time()
        if c["t0"] is None and c["live"]:
            c["t0"] = now
        rel = (now - c["t0"]) if (c["live"] and c["t0"] is not None) else None

        def ev(k):
            if c["ev"][k] is None:
                c["ev"][k] = rel if rel is not None else -1   # -1 = seen, but no time (seeded)

        m = R["pss"].search(line)
        if m:
            ev("pss"); c["ident"].update(gscn=int(m[1]), ssb_sc=int(m[2]), ssb_hz=float(m[3])); return
        m = R["cfo"].search(line)
        if m:
            c["ident"]["cfo_hz"] = int(m[1]); return
        m = R["pbch"].search(line)
        if m:
            ev("pbch"); c["pbch_locks"] = int(m[1])
            if c["_lost_at"] is not None and rel is not None:
                c["relock_s"].append(rel - c["_lost_at"])
            c["_lost_at"] = None
            return
        if R["sib1"].search(line):
            ev("sib1"); return
        m = R["pci"].search(line)
        if m:
            c["ident"].update(pci=int(m[1]), bwp_start=int(m[2]), bwp_size=int(m[3])); return
        m = R["carrier"].search(line)
        if m:
            c["ident"].update(prb=int(m[1]), mu=int(m[2]), fc_mhz=float(m[3])); return
        if R["mismatch"].search(line):
            c["false_lock"] = True; return
        m = R["coreset"].search(line)
        if m:
            ev("coreset"); c["ident"].update(coreset_offset=int(m[1]), coreset_span=int(m[2])); return
        m = R["cs_cand"].search(line)
        if m:
            c["cs_cand"] = (int(m[1]), int(m[2])); return
        if R["len_unres"].search(line):
            c["len_unres"] += 1; return
        m = R["armed"].search(line)
        if m:
            c["armed"] = {"layouts": int(m[1]), "dci_len": int(m[2])}; return
        m = R["alive"].search(line)
        if m:
            n, a, t = int(m[1]), int(m[2]), int(m[3])
            if not c["alive"] or c["alive"][-1][0] != n:
                c["alive"].append((n, a, t))
                c["alive"] = c["alive"][-400:]
            return
        m = R["preferred"].search(line)
        if m:
            ev("dci")
            c["preferred"] = {"rnti": "0x" + m[1], "layout_id": int(m[2]), "candidates": int(m[3]),
                              "passes": int(m[4])}
            if c["pdschq"] and c["pdschq_at_pref"] is None:
                c["pdschq_at_pref"] = dict(c["pdschq"])
            return
        m = R["s2reject"].search(line)
        if m:
            c["s2reject"] = int(m[1]); return
        if R["unsup"].search(line):
            c["unsup"] += 1; return
        m = R["pdschq"].search(line)
        if m:
            q, d, ok, full, stale, lag, mx = map(int, m.groups())
            c["pdschq"] = {"queued": q, "decoded": d, "ok": ok, "full": full, "stale": stale,
                           "lag": lag, "lag_max": mx, "t": rel}
            if ok > 0:
                ev("first_tb")
            return
        m = R["ldpc"].search(line)
        if m:
            ok, sf, tf, zt, ie = map(int, m.groups())
            c["ldpc"] = {"ok": ok, "seg_fail": sf, "tb_fail": tf, "zero_tb": zt, "iface_err": ie}; return
        m = R["harqc"].search(line)
        if m:
            c["harqc"] = {"first": int(m[1]), "retx": int(m[2]), "retx_tot": int(m[3]),
                          "rv_lo": [int(x) for x in m[4].split()], "rv_hi": [int(x) for x in m[5].split()]}
            return
        m = R["mcshist"].search(line)
        if m:
            c["mcs"][m[1]] = [(int(a), int(b), int(n)) for a, b, n in MCS.findall(m[2])]; return
        m = R["rbhist"].search(line)
        if m:
            c["rb"] = [(a, int(b), int(n)) for a, b, n in RB.findall(m[1])]; return
        m = R["branch"].search(line)
        if m:
            c["branch"] = {"rescued": int(m[1]), "tried": int(m[2]),
                           "per": [(int(b), int(o), int(t)) for b, o, t in BR.findall(m[3])]}
            return
        m = R["pdtim"].search(line)
        if m:
            c["pdtim"] = {"calls": int(m[1]), "stages": [(s, int(n), float(mu), float(mx)) for s, n, mu, mx in STAGE.findall(m[2])]}
            return
        m = R["btim"].search(line)
        if m:
            c["btim"] = [(s, int(n), float(mu), float(mx)) for s, n, mu, mx in STAGE.findall(m[1])]; return
        m = R["slotshare"].search(line)
        if m:
            c["slotshare"] = list(map(int, m.groups())); return
        if R["rfstall"].search(line):
            c["rfstall"] += 1; return
        m = R["lost"].search(line)
        if m:
            c["lost"].append(m[1]); c["_lost_at"] = rel; return
        if R["oracle"].search(line):
            c["oracle"] += 1; return
        m = R["probe"].search(line)
        if m:
            c["probe"] = (int(m[1]), int(m[2])); return
        m = R["techd"].search(line)
        if m:
            c["techd"] = {"rnti": "0x" + m[1], "tda": int(m[2]), "S": int(m[3]), "L": int(m[4])}; return
        m = R["parmset"].search(line)
        if m:
            c["parmset_overflow"] = int(m[1]); return
        m = R["ss"].search(line)
        if m:
            c["ss"] = {"per_al": m[1], "confirmed": int(m[2])}; return
        m = R["ssreg"].search(line)
        if m:
            c["ssreg"] = (int(m[1]), int(m[2]))

    # ---- aggregate ----------------------------------------------------------------------------
    def snapshot(self):
        runs = self.runs
        timed = [r for r in runs if r["live"]]
        valid = [r for r in runs if r["ev"]["pss"] is not None]
        succ = [r for r in valid if r["ev"]["first_tb"] is not None]
        false_lock = [r for r in valid if r["false_lock"]]
        inconcl = [r for r in valid if r["ended"] and r["ev"]["first_tb"] is None and not r["false_lock"]]

        def lat(k):
            v = [r["ev"][k] for r in timed if r["ev"][k] is not None and r["ev"][k] >= 0]
            return {"n": len(v), "median": pct(v, 0.5), "p95": pct(v, 0.95)}

        c = self.cur or (runs[-1] if runs else None)
        out = {
            "n_runs": len(runs), "n_valid": len(valid), "n_timed": len(timed),
            "acq": {
                "success": len(succ), "false_lock": len(false_lock), "inconclusive": len(inconcl),
                "latency": {k: lat(k) for k in EVENTS},
                "clock": "wall-clock since first live line of the run", "gap_sample_time": GAP_SAMPLE_TIME,
            },
            "rnti": {"distinct": len(self.rnti), "persistent": sum(1 for v in self.rnti.values() if v >= 2),
                     "top": sorted(self.rnti.items(), key=lambda kv: -kv[1])[:8]},
            "gaps": {"unique_tb": GAP_UNIQUE_TB, "oracle": GAP_ORACLE, "branch": GAP_BRANCH,
                     "stale": GAP_STALE, "injected_gaps": GAP_GAPS, "p95": GAP_P95, "rtf": GAP_RTF},
            "run": None,
        }
        if c is None:
            return out
        q, q0 = c["pdschq"], c["pdschq_at_pref"]
        post = None
        if q and q0 and q["decoded"] > q0["decoded"]:
            post = {"decoded": q["decoded"] - q0["decoded"], "ok": q["ok"] - q0["ok"]}
        alive_now = c["alive"][-1] if c["alive"] else None
        armed = c["armed"]["layouts"] if c["armed"] else (alive_now[2] if alive_now else None)
        # interpretation verdict fractions over the layout hypothesis set
        verdict = None
        if armed:
            a = alive_now[1] if alive_now else armed
            validated = 1 if c["preferred"] else 0
            verdict = {"total": armed, "validated": validated, "ambiguous": max(0, a - validated),
                       "rejected": armed - a, "unsupported_jobs": c["unsup"]}
        out["run"] = {
            "path": c["path"], "live": c["live"], "ended": c["ended"], "ident": c["ident"],
            "events": c["ev"], "false_lock": c["false_lock"], "rfstall": c["rfstall"],
            "lost": len(c["lost"]), "relocks": max(0, c["pbch_locks"] - 1), "relock_s": c["relock_s"],
            "relock_median": pct(c["relock_s"], 0.5), "relock_p95": pct(c["relock_s"], 0.95),
            "coreset_hyp": c["cs_cand"], "len_unresolved": c["len_unres"],
            "layouts": {"armed": c["armed"], "alive": c["alive"], "now": alive_now,
                        "preferred": c["preferred"], "stage2_rejected": c["s2reject"], "verdict": verdict,
                        "obs_to_converge": next((n for n, a, t in c["alive"] if a <= 8), None)},
            "pdschq": q, "post_conv": post, "ldpc": c["ldpc"], "harqc": c["harqc"],
            "mcs": c["mcs"], "rb": c["rb"], "branch": c["branch"],
            "pdtim": c["pdtim"], "btim": c["btim"], "slotshare": c["slotshare"],
            "oracle_obs": c["oracle"], "probe": c["probe"], "techd": c["techd"],
            "parmset_overflow": c["parmset_overflow"], "ss": c["ss"], "ssreg": c["ssreg"],
        }
        return out


def _selfcheck():
    ps = PaperStats()
    ps.begin_run("x/run.log", live=True)
    t = 100.0
    for dt, ln in [(0, "[NR_PHY] Cell Detected with GSCN: 7783, SSB SC offset: 150, SSB Ref: 3408960000.000000, PSS Corr peak: 112 dB"),
                   (0.1, "[PHY] SENSING: ACQ_EVENT pbch_locked (n=1, state=SEARCHING)"),
                   (0.3, "[PHY] SENSING: ACQ_EVENT sib1_decoded (n=1, state=PBCH_LOCKED)"),
                   (0.3, "[PHY] PASSIVE: SIB1 common facts PCI=2 DL-BWP=0+273 DL-TDAs=2"),
                   (0.5, "[PHY] SENSING: DCI11_LAYOUT armed: 455 layouts consistent with dci_length=47 (riv=16 bits"),
                   (0.6, "[PHY] SENSING: DCI11_LAYOUT n=4000 observed | 209 of 455 layouts still plausible"),
                   (0.7, "[PHY] SENSING: PDSCHQ queued=10 decoded=10 crc_ok=0 (0.0%) dropped[full=0 stale=1] max_lag_slots=3/20"),
                   (0.8, "[PHY] SENSING: DCI11_LAYOUT n=8000 observed | 4 of 455 layouts still plausible"),
                   (0.9, "[PHY] SENSING: DL layout family PREFERRED by TB CRC: rnti=0x656f layout_id=12 (8 candidates) passes=9"),
                   (1.2, "[PHY] SENSING: PDSCHQ queued=110 decoded=110 crc_ok=50 (45.5%) dropped[full=0 stale=1] max_lag_slots=3/20"),
                   (1.3, "[PHY] SENSING: ACQ_STATE CELL_CONFIGURED -> LOST (receive-stream discontinuity"),
                   (2.3, "[PHY] SENSING: ACQ_EVENT pbch_locked (n=2, state=LOST)"),
                   (2.4, "[PHY] SENSING: BRANCHSEL rescued=3/100 retries [0:0/25 1:1/25 2:2/25 3:0/25] (branch:rescued/tried)"),
                   (2.5, "[PHY] SENSING: PDTIM calls=73000 fep[n=73146 mean=164.5us max=99922.5us tot=12.03s] chest[n=73001 mean=3391.1us max=43752104.0us tot=247.55s]"),
                   ]:
        ps.ingest(ln, now=t + dt)
    s = ps.snapshot()
    r = s["run"]
    assert s["acq"]["success"] == 1 and s["acq"]["false_lock"] == 0, s["acq"]
    assert abs(r["events"]["first_tb"] - 1.2) < 1e-6 and abs(r["events"]["dci"] - 0.9) < 1e-6, r["events"]
    assert r["post_conv"] == {"decoded": 100, "ok": 50}, r["post_conv"]
    assert r["layouts"]["obs_to_converge"] == 8000 and r["layouts"]["verdict"]["rejected"] == 451, r["layouts"]
    assert r["relocks"] == 1 and abs(r["relock_s"][0] - 1.0) < 1e-6, (r["relocks"], r["relock_s"])
    assert r["branch"]["per"][2] == (2, 2, 25) and r["pdtim"]["stages"][1][0] == "chest", (r["branch"], r["pdtim"])
    print("ok  paper_stats self-check")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        ps = PaperStats()
        ps.begin_run(sys.argv[1], live=False)
        with open(sys.argv[1], errors="replace") as f:
            for ln in f:
                ps.ingest(ln.replace("\033", ""))
        ps.end_run()
        print(json.dumps(ps.snapshot(), indent=1, default=str)[:6000])
    else:
        _selfcheck()
