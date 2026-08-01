#!/usr/bin/env python3
"""ORACLE: rewrite a report stream keeping only detections a perfect harmonic-family model would keep.

Purpose. Before building the architecture that propagates DSP-level family structure into the
tracker, measure the ceiling that architecture could possibly reach. This filter is that ceiling: it
uses GROUND TRUTH to label every detection, then emits a report stream containing only the ones a
perfect family model would have identified as real. Replaying it gives an upper bound on the whole
programme in one run — no plumbing, no likelihood design. If the oracle barely moves the numbers, the
architecture cannot either.

It is an ORACLE, not a method: it reads the simulator's ground truth. Nothing here can ship.

Labelling is AT-EPOCH, reusing eval_harmonic_pos.py's `cpi_epoch`. This matters and is not a detail —
the project's usual time-alignment-free scorer is WRONG for anything harmonic-related, because a
target whose range-rate sweeps a 2:1 span at near-constant dR passes through its OWN 2nd-harmonic
locus later in the run, so a genuine ghost scores "real" and rejecting it counts as a cost
(GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md §7.13). The epoch is recovered from each CPI's own detections
and its validity is checked (recovered epochs must advance with CPI index).

Usage:
  oracle_family_filter.py <run_dir> --out <reports.jsonl> [--keep real|real+other]
      real        keep only detections explained by a target AT that CPI's epoch  (tightest bound)
      real+other  additionally keep unclassifiable detections, dropping only identified k-harmonics
                  and mirrors — i.e. the bound for a model that only knows about the HARMONIC family,
                  which is what the proposed architecture would actually know
"""
import argparse
import json
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from eval_harmonic_pos import cpi_epoch, curve_times, load_curves  # noqa: E402


def classify(d, ep, curves, rtol, vtol, tf, max_k):
    """'real' | 'harmonic' | 'other', evaluated AT the CPI's own epoch."""
    if ep is None:
        return "other"

    def at_epoch(vel, tol):
        dd = {"bistatic_range_m": d["bistatic_range_m"], "bistatic_velocity_mps": vel}
        return any(abs(t - ep) <= 0.6 for t in curve_times(curves, dd, rtol, tol, tf))

    if at_epoch(d["bistatic_velocity_mps"], vtol):
        return "real"
    for k in range(2, max_k + 1):
        if at_epoch(d["bistatic_velocity_mps"] / k, vtol / k):
            return "harmonic"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keep", default="real", choices=["real", "real+other"])
    ap.add_argument("--range-tol", type=float, default=15.0)
    ap.add_argument("--vel-tol", type=float, default=3.0)
    ap.add_argument("--max-k", type=int, default=4)
    a = ap.parse_args()

    reports = [json.loads(l) for l in open(f"{a.run_dir}/oaiue_reports.jsonl")]
    curves, tf = load_curves(f"{a.run_dir}/logs/ue.log")
    if not curves:
        print("no ground truth", file=sys.stderr)
        sys.exit(1)
    epochs, rho = cpi_epoch(reports, curves, a.range_tol, a.vel_tol, tf, 0.6)
    if not (rho == rho) or rho < 0.5:
        print(f"WARNING: recovered epochs are not monotone in CPI index (rho={rho:.2f}); "
              f"the at-epoch labelling is unreliable on this run", file=sys.stderr)

    keep_set = {"real"} | ({"other"} if a.keep == "real+other" else set())
    counts = {"real": 0, "harmonic": 0, "other": 0}
    with open(a.out, "w") as f:
        for rep, ep in zip(reports, epochs):
            kept = []
            for d in rep["detections"]:
                lab = classify(d, ep, curves, a.range_tol, a.vel_tol, tf, a.max_k)
                counts[lab] += 1
                if lab in keep_set:
                    kept.append(d)
            out = dict(rep)
            out["detections"] = kept
            f.write(json.dumps(out) + "\n")
    tot = sum(counts.values())
    print(f"epoch rho={rho:.2f}  detections={tot}  "
          f"real={counts['real']} harmonic={counts['harmonic']} other={counts['other']}  "
          f"-> kept {sum(counts[k] for k in keep_set)} ({a.keep})")


if __name__ == "__main__":
    main()
