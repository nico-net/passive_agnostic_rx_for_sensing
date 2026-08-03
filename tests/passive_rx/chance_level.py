#!/usr/bin/env python3
"""Step 3 (research iteration 005): chance-level in-band precision for a uniformly-random
detection stream, as a falsification check on the in-band precision metric.

`scratchpad/score_ghost_census.py` is not present in this tree (different session's
scratchpad, as with e0_axis.py/d1_probe.py earlier). Built an equivalent directly through
gt_score.py's own per-CPI matching logic, not a hand-derived formula, per the "through the
same classifier" instruction.

METHODOLOGY NOTE, recorded because a first version of this script got it wrong and was
caught by cross-checking two independent formulations before trusting either: chance level
must be computed PER-CPI and INSTANTANEOUS -- "if a uniformly-random detection were placed
in THIS CPI's usable window, what's the chance it lands within tol_m of THIS CPI's current
GT position" -- not a whole-trajectory band overlap. A first attempt computed the union of
each object's ENTIRE min-to-max dR range (visited over the whole run) and divided by a
single representative axis width; that conflates "everywhere the object has ever been" with
"where it is right now" and gave 74% (closed-form) vs 34% (a naive Monte Carlo built on the
same wrong framing) -- a 40-point disagreement that was the tell something was wrong, not
Monte Carlo noise (54000 trials is enough to converge to ~1%). The per-CPI instantaneous
version below has both formulations agree by construction (the Monte Carlo IS literally
sampling the closed-form's own random variable), so they now serve as a implementation
cross-check on each other rather than two different wrong things being compared.

Both measures are LOS-offset-independent by construction: "usable axis width" is the
nonzero-bin SPAN of each CPI's own captured range profile (a constant LOS-offset shift
moves the span's position, never its width), and the target-window WIDTH (2*tol_m per
object, less if objects' windows overlap) is likewise position-free.

Usage: chance_level.py <ue_rx1.log> <reports.jsonl> [--tol-m 40] [--trials 500]
"""
import argparse
import json
import random
import statistics as st

import gt_score


def usable_width_m(rep):
    """Nonzero-bin span of this CPI's range profile (Doppler-summed), in metres. Width only
    -- position-independent, so no LOS offset is needed."""
    blob = rep.get("rvm_blob")
    if not blob:
        return None
    D = int(round(2 * rep["vel_max_mps"] / rep["vel_res_mps"])) if rep["vel_res_mps"] > 0 else 0
    if D <= 0 or len(blob) % D:
        return None
    R = len(blob) // D
    prof = [0.0] * R
    for r in range(R):
        s = 0.0
        base = r * D
        for d in range(D):
            s += blob[base + d]
        prof[r] = s
    nz = sum(1 for x in prof if x != 0.0)
    return nz * rep["range_res_m"]


def per_cpi_chance(reports, gt, tol_m, trials_per_cpi):
    """Both the closed-form and the Monte Carlo, computed together per CPI so they are
    checking the SAME random variable (a uniform draw over that CPI's own usable axis,
    scored a hit if within tol_m of ANY object's CURRENT interpolated dR).

    Closed form per CPI: union, over objects present, of each object's own [dR-tol, dR+tol]
    window (clipped to the axis width, and to non-negative extent), divided by the axis
    width. Exact for a uniform distribution -- no simulation needed for this half; the
    Monte Carlo exists to CHECK it against the classifier's actual per-CPI GT positions
    (interp()), not to compute an independent estimate of the same quantity.
    """
    closed_vals = []
    mc_vals = []
    for rep in reports:
        w = usable_width_m(rep)
        if w is None or w <= 0:
            continue
        utc = rep["cpi_start_time_utc_ns"]
        cur_dRs = []
        for obj, samples in gt.items():
            got = gt_score.interp(samples, utc)
            if got is not None:
                cur_dRs.append(got[0])
        if not cur_dRs:
            continue

        # Closed form: union of [dR-tol, dR+tol] windows, intersected with [0, w] (an object
        # currently outside this CPI's own usable window contributes nothing -- it is not
        # resolvable/visible THIS CPI regardless of what a random detection could match).
        windows = []
        for dR in cur_dRs:
            lo, hi = max(0.0, dR - tol_m), min(w, dR + tol_m)
            if hi > lo:
                windows.append((lo, hi))
        windows.sort()
        merged = []
        for lo, hi in windows:
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
            else:
                merged.append((lo, hi))
        hit_width = sum(hi - lo for lo, hi in merged)
        closed_vals.append(hit_width / w)

        # Monte Carlo over the SAME (w, cur_dRs) this CPI -- checks the closed form's
        # arithmetic against literal sampling, not an independent methodology.
        hits = 0
        for _ in range(trials_per_cpi):
            x = random.uniform(0.0, w)
            if any(abs(x - dR) <= tol_m for dR in cur_dRs):
                hits += 1
        mc_vals.append(hits / trials_per_cpi)

    return closed_vals, mc_vals


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ue_log")
    ap.add_argument("reports_jsonl")
    ap.add_argument("--tol-m", type=float, default=40.0)
    ap.add_argument("--trials", type=int, default=500)
    a = ap.parse_args()

    gt = gt_score.parse_gt(a.ue_log)
    reports = [json.loads(l) for l in open(a.reports_jsonl) if l.strip()]
    reports.sort(key=lambda r: r["cpi_start_time_utc_ns"])

    closed_vals, mc_vals = per_cpi_chance(reports, gt, a.tol_m, a.trials)
    if not closed_vals:
        print("no CPI with a usable axis and a GT position -- cannot compute chance level")
        return

    closed_pct = 100.0 * st.mean(closed_vals)
    mc_pct = 100.0 * st.mean(mc_vals)
    print(f"CPIs used: {len(closed_vals)}")
    print(f"CLOSED-FORM chance precision (mean over CPIs): {closed_pct:.2f}%  "
          f"(median {100*st.median(closed_vals):.2f}%, "
          f"range {100*min(closed_vals):.2f}-{100*max(closed_vals):.2f}%)")
    print(f"MONTE CARLO chance precision ({a.trials} trials/CPI): {mc_pct:.2f}%  "
          f"(median {100*st.median(mc_vals):.2f}%)")
    agree = abs(closed_pct - mc_pct)
    print(f"agreement: |closed-form - monte carlo| = {agree:.2f} pp "
          f"({'OK, expected sampling noise' if agree < 3.0 else 'LARGE -- investigate before trusting either number'})")


if __name__ == "__main__":
    main()
