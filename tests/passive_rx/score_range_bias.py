#!/usr/bin/env python3
"""Measure the SIGNED bias between a receiver's reported bistatic range and ground truth.

Written 2026-07-30 to characterise a ~40 m systematic range offset: every target was being found
with excellent bearings (median 0.04-0.17 deg) but in the wrong range bin, which made the standard
12 m detection tolerance score 0/77 on a scene that was demonstrably being detected.

Pairing rule, and it is the whole reason this is a separate script rather than a flag on
check_detection.py: a detection is matched to a target by VELOCITY ONLY (within --vtol), then the
range error is whatever it is. Matching on range would beg the question -- any range tolerance wide
enough to admit a biased detection also admits unrelated clutter, and a tolerance narrow enough to
exclude clutter rejects the very detections being measured. Velocity is independent of the quantity
under test, which is what makes it a valid discriminator.

Ground truth is interpolated in utc_ns exactly as check_detection.py does (both clocks are
CLOCK_REALTIME on the same host; the `t=` field is sample-clock derived and drifts -- do not pair on
it).

Usage: score_range_bias.py <reports.jsonl> <ue_rx.log> [--vtol 3.0]
"""
import argparse
import json
import re
import sys

import numpy as np

GT = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) .*?dR=([-\d.]+)m "
                r"range_rate=([-\d.]+)m/s")

ap = argparse.ArgumentParser()
ap.add_argument("reports")
ap.add_argument("log")
ap.add_argument("--vtol", type=float, default=3.0,
                help="velocity tolerance (m/s) used to PAIR a detection with a target")
a = ap.parse_args()

gt = {}
for line in open(a.log, errors="ignore"):
    m = GT.search(line)
    if m:
        gt.setdefault(int(m.group(3)), []).append(
            (int(m.group(2)), float(m.group(4)), float(m.group(5))))
for o in gt:
    gt[o].sort()
if not gt:
    sys.exit("no ground truth in log")

reports = [json.loads(l) for l in open(a.reports) if l.strip()]
if not reports:
    sys.exit("no reports")

print(f"{len(reports)} CPIs, {len(gt)} objects, paired on velocity within +/-{a.vtol} m/s")
all_err = []
for o in sorted(gt):
    arr = np.array(gt[o])
    errs, ranges = [], []
    for r in reports:
        t = r["cpi_start_time_utc_ns"]
        if t < arr[0, 0] or t > arr[-1, 0] or not r["detections"]:
            continue
        true_dr = np.interp(t, arr[:, 0], arr[:, 1])
        true_rate = np.interp(t, arr[:, 0], arr[:, 2])
        cand = [d for d in r["detections"]
                if abs(abs(d["bistatic_velocity_mps"]) - abs(true_rate)) < a.vtol]
        if not cand:
            continue
        # Among the velocity-matched detections take the nearest in range. This still biases toward
        # small |error|, so the MEDIAN is the statistic to read, not the mean or the minimum.
        d0 = min(cand, key=lambda d: abs(d["bistatic_range_m"] - true_dr))
        errs.append(d0["bistatic_range_m"] - true_dr)
        ranges.append(true_dr)
    if errs:
        e = np.array(errs)
        all_err += errs
        print(f"  obj{o}: n={len(e):3d}  true dR {min(ranges):5.0f}..{max(ranges):5.0f} m   "
              f"SIGNED bias median {np.median(e):+7.1f} m   "
              f"p25 {np.percentile(e, 25):+7.1f}  p75 {np.percentile(e, 75):+7.1f}")
    else:
        print(f"  obj{o}: no velocity-matched detections")

if all_err:
    e = np.array(all_err)
    med = float(np.median(e))
    # 1 sample at the 273 PRB gNB rate; the bias is expressed in samples because a constant offset
    # in samples (rather than in metres or in % of range) is what points at a fixed delay error.
    m_per_sample = 299792458.0 / 122.88e6
    print(f"\nOVERALL median bias {med:+.1f} m  =  {med / m_per_sample:+.2f} samples @122.88 Msps "
          f"({m_per_sample:.4f} m/sample)")
