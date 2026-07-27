#!/usr/bin/env python3
"""Aggregate the repeated "2 array receivers vs 3 receivers" arms into mean +/- SD per arm.

Reporting the mean alone would be misleading on this harness: fused precision was measured swinging
8-58% across IDENTICAL repetitions (handover section 1), so the SD and the per-rep values are the
part that decides whether a difference between arms is real.

Usage: aggregate_aoa_vs_3rx.py results.csv
"""
import csv
import math
import sys

ORDER = [
    ("3rx_norange_aoa", "3 rx, no AoA        (today's baseline)"),
    ("2rx_no_aoa", "2 rx, no AoA        (the structural case)"),
    ("2rx_aoa_nogate", "2 rx + AoA, no gates"),
    ("2rx_aoa_gated", "2 rx + AoA, GATED   (the claim)"),
    ("3rx_aoa_gated", "3 rx + AoA, gated"),
]


def stat(v):
    if not v:
        return float("nan"), float("nan")
    m = sum(v) / len(v)
    sd = math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1)) if len(v) > 1 else 0.0
    return m, sd


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    if not rows:
        print("no results")
        return 1
    by = {}
    for r in rows:
        by.setdefault(r["arm"], []).append(r)
    reps = sorted({int(r["rep"]) for r in rows})
    print(f"repetitions: {len(reps)}  {reps}")
    print()
    print(f"{'arm':38s} {'precision %':>16s} {'median err m':>16s} {'obj0 cov':>10s} {'obj1 cov':>10s}")
    print("-" * 94)
    for key, label in ORDER:
        rs = by.get(key, [])
        if not rs:
            continue
        p = [float(r["precision_pct"]) for r in rs]
        e = [float(r["median_err_m"]) for r in rs if r["median_err_m"] != "nan"]
        o0 = [int(r["obj0"]) for r in rs]
        o1 = [int(r["obj1"]) for r in rs]
        pm, ps = stat(p)
        em, es = stat(e)
        # "coverage" here = how many reps tracked that target AT ALL. A 100%-precision arm that never
        # sees one of the two targets has not solved the problem, it has narrowed it.
        n0 = sum(1 for x in o0 if x > 0)
        n1 = sum(1 for x in o1 if x > 0)
        print(f"{label:38s} {pm:7.1f} +/- {ps:4.1f} {em:9.1f} +/- {es:4.1f} "
              f"{n0:4d}/{len(rs):<5d} {n1:4d}/{len(rs):<5d}")
    print()
    print("obj cov = repetitions in which that target was tracked at all (updates > 0).")
    print()
    print("per-rep detail:")
    for key, label in ORDER:
        rs = by.get(key, [])
        if rs:
            d = "  ".join(
                f"r{r['rep']}:{r['precision_pct']}%/{r['median_err_m']}m/o0={r['obj0']},o1={r['obj1']}"
                for r in sorted(rs, key=lambda x: int(x["rep"]))
            )
            print(f"  {label:38s} {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
