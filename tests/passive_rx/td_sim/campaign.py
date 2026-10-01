#!/usr/bin/env python3
"""Matrix runner for nr_td_sim (Technique D convergence levers, spec 2026-10-01 section 6.1).

Matrix JSON: {"arms": {name: {flag: value}}, "cells": {"SA": {"sib1": 1}, "NSA-like": {"sib1": 0}},
              "rx": [4, 1], "acq": 2000, "seed": 1, "common": {flag: value}}
Flags are nr_td_sim flags without the leading "--" and with "_" or "-" (e.g. "w-sib1", "K").
Writes <out>/results.jsonl (one line per RNTI, tagged arm/cell/rx) and <out>/summary.md.
All numbers are SIMULATED (nr_td_sim), never MEASURED. Cold = first two RNTIs of an acquisition, steady = later ones.
"""
import argparse, json, os, statistics, subprocess, sys


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, -(-len(v) * p // 100) - 1))] if v else float("nan")


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
    a = ap.parse_args(argv)
    m = json.load(open(a.matrix))
    os.makedirs(a.out, exist_ok=True)
    rows = []
    with open(os.path.join(a.out, "results.jsonl"), "w") as rf:
        for arm, af in m["arms"].items():
            for cell, cf in m["cells"].items():
                for rx in m.get("rx", [4]):
                    flags = dict(m.get("common", {}), **af, **cf)
                    recs = [r for r in run_one(a.sim, flags, m.get("acq", 2000), m.get("seed", 1), rx) if "summary" not in r]
                    for r in recs:
                        rf.write(json.dumps(dict(r, arm=arm, cell=cell, rx=rx)) + "\n")
                    cold = [r["seconds"] for r in recs if r["rnti_rank"] < 2]
                    steady = [r["seconds"] for r in recs if r["rnti_rank"] >= 2]
                    rows.append((arm, cell, rx, med(cold), pct(cold, 95), med(steady), sum(r["wrong"] for r in recs),
                                 sum(r["undecidable"] for r in recs), sum(r["n_full"] for r in recs),
                                 sum(r["n_probe"] for r in recs), sum(r["gated_phys"] + r["gated_chan"] for r in recs)))
    with open(os.path.join(a.out, "summary.md"), "w") as sf:
        sf.write("[SIMULATED, nr_td_sim] cold = first two RNTIs per acquisition; steady = later RNTIs; seconds = grants / grants-per-s\n\n")
        sf.write("| arm | cell | rx | cold median s | cold p95 s | steady median s | wrong | undecidable | n_full | n_probe | gated |\n")
        sf.write("|---|---|---|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            sf.write("| %s | %s | %d | %.1f | %.1f | %.1f | %d | %d | %d | %d | %d |\n" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
