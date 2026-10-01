#!/usr/bin/env python3
"""Matrix runner for nr_td_sim (Technique D convergence levers, spec 2026-10-01 section 6.1).

Matrix JSON: {"arms": {name: {flag: value}}, "cells": {"SA": {"sib1": 1}, "NSA-like": {"sib1": 0}},
              "rx": [4, 1], "oracle": [1, 0] (optional), "acq": 2000, "seed": 1, "common": {flag: value}}
--jobs N runs N simulator processes in parallel (output order unchanged).
Flags are nr_td_sim flags without the leading "--" and with "_" or "-" (e.g. "w-sib1", "K").
Writes <out>/results.jsonl (one line per RNTI, tagged arm/cell/rx) and <out>/summary.md.
All numbers are SIMULATED (nr_td_sim), never MEASURED. Cold = first two RNTIs of an acquisition, steady = later ones.
Lever columns (geom_pins, geom_blocks, crc_accepts: lever P / lever C events) come from the simulator summary and are 0 when the levers are off.
Field-book-2 columns (fail_opens, active_start_mean, recovery_*, untrusted_after) come from the simulator summary and are 0 / -1 (recovery, no injection) when absent.
"""
import argparse, itertools, json, os, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, -(-len(v) * p // 100) - 1))] if v else float("nan")


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def med(v):
    return statistics.median(v) if v else float("nan")


def run_one(sim, flags, acq, seed, rx):
    cmd = [sim, "--acq", str(acq), "--seed", str(seed), "--n-rx", str(rx)]
    for k, v in flags.items():
        cmd += ["--" + k.replace("_", "-"), str(v)]
    p = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True)
    ap.add_argument("--matrix", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=1)
    a = ap.parse_args(argv)
    m = json.load(open(a.matrix))
    os.makedirs(a.out, exist_ok=True)
    rows = []
    combos = [(arm, af, cell, cf, rx, orc) for arm, af in m["arms"].items() for cell, cf in m["cells"].items()
              for rx in m.get("rx", [4]) for orc in m.get("oracle", [None])]

    def job(c):
        arm, af, cell, cf, rx, orc = c
        flags = dict(m.get("common", {}), **af, **cf)
        if orc is not None:
            flags["oracle"] = orc
        return run_one(a.sim, flags, m.get("acq", 2000), m.get("seed", 1), rx)

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        results = list(ex.map(job, combos))
    with open(os.path.join(a.out, "results.jsonl"), "w") as rf:
        for (arm, af, cell, cf, rx, orc), recs in zip(combos, results):
            if True:
                if True:
                    summ = next((r["summary"] for r in recs if "summary" in r), {})
                    recs = [r for r in recs if "summary" not in r]
                    for r in recs:
                        rf.write(json.dumps(dict(r, arm=arm, cell=cell, rx=rx, oracle=orc)) + "\n")
                    # Undecidable (capped) RNTIs are CENSORED: excluded from quantiles/means, counted separately.
                    ok = [r for r in recs if not r["undecidable"]]
                    cold = [r["seconds"] for r in ok if r["rnti_rank"] < 2]
                    steady = [r["seconds"] for r in ok if r["rnti_rank"] >= 2]
                    allsec = [r["seconds"] for r in ok]
                    tab = {}
                    for t in (0, 1, 2):
                        v = [r["seconds"] for r in ok if r.get("truth_table") == t]
                        tab[t] = "%d/%.1f/%.1f/%d" % (len(v), med(v), mean(v), sum(r["wrong"] for r in ok if r.get("truth_table") == t))
                    rows.append((arm, cell, rx, "-" if orc is None else orc, med(cold), pct(cold, 95), med(steady), mean(cold), pct(steady, 95), mean(steady), mean(allsec),
                                 mean([r["grants"] for r in ok]), sum(r["wrong"] for r in recs),
                                 sum(r["undecidable"] for r in recs), sum(r["n_full"] for r in recs),
                                 sum(r["n_probe"] for r in recs), sum(r["gated_phys"] + r["gated_chan"] for r in recs),
                                 tab[0], tab[1], tab[2], sum(r.get("oracle_state") == "miss" for r in recs),
                                 sum(r.get("oracle_state") == "wrong" for r in recs), summ.get("harq_trap_passes", 0),
                                 summ.get("false_passes", 0), summ.get("fail_opens", 0), summ.get("active_start_mean", 0),
                                 summ.get("recovery_grants", 0), summ.get("recovery_rntis", 0), sum(r.get("withdrawals", 0) for r in recs),
                                 summ.get("untrusted_after", 0), summ.get("geom_pins", 0), summ.get("geom_blocks", 0),
                                 summ.get("crc_accepts", 0)))
    with open(os.path.join(a.out, "summary.md"), "w") as sf:
        sf.write("[SIMULATED, nr_td_sim] cold = first two RNTIs per acquisition; steady = later RNTIs; seconds = grants / grants-per-s.\n"
                 "Undecidable (capped) RNTIs are censored: excluded from medians/means/p95, counted in the undecidable column.\n"
                 "Medians are quantised (separation is checked every 16 trials): prefer the mean columns.\n"
                 "tbl N = truth mcs_table N as count/median s/mean s/wrong.\n\n")
        sf.write("| arm | cell | rx | oracle | cold median s | cold p95 s | steady median s | cold mean s | steady p95 s | steady mean s | mean s | mean grants | wrong | undecidable | n_full | n_probe | gated | tbl 0 | tbl 1 | tbl 2 | oracle_miss_rntis | oracle_wrong_rntis | harq_trap_passes | false_passes | fail_opens | active_start_mean | recovery_grants | recovery_rntis | withdrawals | untrusted_after | geom_pins | geom_blocks | crc_accepts |\n")
        sf.write("|---" * 33 + "|\n")
        for r in rows:
            sf.write("| %s | %s | %d | %s | %.1f | %.1f | %.1f | %.2f | %.1f | %.2f | %.2f | %.0f | %d | %d | %d | %d | %d | %s | %s | %s | %d | %d | %d | %d | %d | %.1f | %.1f | %.2f | %d | %d | %d | %d | %d |\n" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
