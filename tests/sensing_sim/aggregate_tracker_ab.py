#!/usr/bin/env python3
"""Aggregate _run_tracker_ab.sh's per-capture CSV into a per-arm summary with significance.

Paired across captures (every arm replays the SAME detections), so a paired t-test against the
legacy arm is the right test — it removes this harness's large capture-to-capture variance, which
otherwise swamps the effect being measured.

Usage: aggregate_tracker_ab.py <results.csv>
"""
import sys
import csv
import collections
import math

rows = list(csv.DictReader(open(sys.argv[1])))
# Baseline arm: "legacy" for the tracker A/B, "baseline" for the oracle budget. Falls back to
# whichever arm appears first, so the aggregator works on any arm-set without editing.
BASE = None
if not rows:
    print("no rows")
    sys.exit(1)

by = collections.defaultdict(dict)  # (metric, arm) -> {capture: row}
for r in rows:
    by[(r["metric"], r["arm"])][r["capture"]] = r

all_arms = {r["arm"] for r in rows}
for cand in ("legacy", "baseline"):
    if cand in all_arms:
        BASE = cand
        break
if BASE is None:
    BASE = sorted(all_arms)[0]
arms = sorted(all_arms, key=lambda a: (a != BASE, a))
caps = sorted({r["capture"] for r in rows})


def mean_sd(v):
    if not v:
        return float("nan"), float("nan")
    m = sum(v) / len(v)
    if len(v) < 2:
        return m, 0.0
    return m, math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def paired_t(a, b):
    """Paired t-test p-value (two-sided), normal approximation for small n."""
    d = [x - y for x, y in zip(a, b)]
    n = len(d)
    if n < 2:
        return float("nan")
    m, s = mean_sd(d)
    if s == 0:
        return 0.0 if m != 0 else 1.0
    t = m / (s / math.sqrt(n))
    # two-sided normal approx; with n=6 this is indicative, not exact
    return math.erfc(abs(t) / math.sqrt(2))


for metric in ("emitted", "confirmed"):
    print(f"\n===== {metric.upper()} =====")
    print(f"{'arm':<12}{'precision %':>14}{'correct upd':>13}{'median err m':>14}{'ids':>7}{f'p vs {BASE}':>13}")
    base = [float(by[(metric, BASE)][c]["precision"]) for c in caps if c in by[(metric, BASE)]]
    for arm in arms:
        d = by[(metric, arm)]
        cs = [c for c in caps if c in d and c in by[(metric, BASE)]]
        if not cs:
            continue
        prec = [float(d[c]["precision"]) for c in cs]
        good = [int(d[c]["good"]) for c in cs]
        err = [float(d[c]["median_err"]) for c in cs if d[c]["median_err"]]
        ids = [int(d[c]["ids"]) for c in cs]
        bp = [float(by[(metric, BASE)][c]["precision"]) for c in cs]
        pm, ps = mean_sd(prec)
        gm, _ = mean_sd(good)
        em, _ = mean_sd(err)
        im, _ = mean_sd(ids)
        p = paired_t(prec, bp) if arm != BASE else float("nan")
        pstr = "--" if arm == BASE else f"{p:.3f}"
        print(f"{arm:<12}{pm:>9.1f} ±{ps:<4.1f}{gm:>13.0f}{em:>14.1f}{im:>7.1f}{pstr:>13}")
    print(f"  n = {len(caps)} captures, paired")
