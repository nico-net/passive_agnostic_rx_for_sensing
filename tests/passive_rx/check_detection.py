#!/usr/bin/env python3
"""Did the pipeline actually DETECT the scene's targets? Per-CPI, ground-truth-referenced.

Answers the question that has to be settled before spending hours on a capture: for each completed
CPI, was there a detection at the range/velocity the target genuinely occupied at that moment?
Anything downstream (fusion, tracking, gating) is meaningless if this is near zero -- which is
exactly the trap the previous scene fell into, where the target sat inside the zero-Doppler notch in
70% of CPIs and no amount of fusion tuning could have helped.

Time alignment uses the `utc_ns=` field on sensing_channel.c's SENSING_CHANNEL gt lines against the
report's own cpi_start_time_utc_ns -- both are CLOCK_REALTIME on the same host, so they are directly
comparable. (The older `t=` field is sample-clock derived and drifts; do not pair on it.)

Usage: check_detection.py <reports.jsonl> <ue.log> [range_tol_m] [vel_tol_mps]
"""
import sys, re, json
import numpy as np

rep_path, log_path = sys.argv[1], sys.argv[2]
RTOL = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
VTOL = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0

GT = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) .*?dR=([-\d.]+)m "
                r"range_rate=([-\d.]+)m/s(?: azimuth=([-\d.]+)deg)?")

gt = {}   # obj -> list of (utc_ns, dR, rate, az)
with open(log_path, errors="ignore") as f:
    for line in f:
        m = GT.search(line)
        if m:
            o = int(m.group(3))
            gt.setdefault(o, []).append((int(m.group(2)), float(m.group(4)), float(m.group(5)),
                                         float(m.group(6)) if m.group(6) else None))
for o in gt:
    gt[o].sort()
if not gt:
    print("no ground truth in log"); sys.exit(1)

reports = [json.loads(l) for l in open(rep_path) if l.strip()]
if not reports:
    print("no reports"); sys.exit(1)

print(f"{len(reports)} CPIs, {len(gt)} ground-truth objects, tol +/-{RTOL} m / +/-{VTOL} m/s")
print(f"CPI geometry: range_res={reports[-1]['range_res_m']:.2f} m  "
      f"vel_res={reports[-1].get('vel_res_mps', float('nan')):.3f}  "
      f"vel_max={reports[-1]['vel_max_mps']:.1f} m/s\n")

tot_det = sum(len(r["detections"]) for r in reports)
tot_az = sum(1 for r in reports for d in r["detections"] if "azimuth_deg" in d)
print(f"detections total {tot_det}, carrying azimuth {tot_az} ({100*tot_az/max(tot_det,1):.0f}%)")

overall_hit = 0
for o, rows in sorted(gt.items()):
    t = np.array([r[0] for r in rows])
    dR = np.array([r[1] for r in rows])
    rate = np.array([r[2] for r in rows])
    az = [r[3] for r in rows]
    hits = 0
    azerr = []
    for rep in reports:
        tc = rep["cpi_start_time_utc_ns"] + rep.get("cpi_duration_ns", 0) // 2
        j = int(np.argmin(np.abs(t - tc)))
        if abs(int(t[j]) - tc) > 2e9:      # no GT within 2 s -> can't score this CPI
            continue
        for d in rep["detections"]:
            if (abs(d["bistatic_range_m"] - dR[j]) <= RTOL
                    and abs(abs(d["bistatic_velocity_mps"]) - abs(rate[j])) <= VTOL):
                hits += 1
                if "azimuth_deg" in d and az[j] is not None:
                    e = (d["azimuth_deg"] - az[j] + 180) % 360 - 180
                    azerr.append(abs(e))
                break
    overall_hit += hits
    line = (f"  obj{o}: detected in {hits}/{len(reports)} CPIs ({100*hits/len(reports):.0f}%)   "
            f"dR {dR.min():.0f}..{dR.max():.0f} m, |rate| {np.abs(rate).min():.1f}..{np.abs(rate).max():.1f} m/s")
    if azerr:
        line += f"   bearing |err| median {np.median(azerr):.2f} deg, p90 {np.percentile(azerr,90):.2f}"
    print(line)

cov = sum(1 for rep in reports
          if any(any(abs(d["bistatic_range_m"] - gt[o][int(np.argmin(np.abs(np.array([g[0] for g in gt[o]])
                 - (rep["cpi_start_time_utc_ns"]))))][1]) <= RTOL for d in rep["detections"]) for o in gt))
print(f"\nCPIs with at least one true-target detection: {cov}/{len(reports)} ({100*cov/len(reports):.0f}%)")
print("VERDICT:", "USABLE" if cov >= 0.5 * len(reports) else "*** TOO FEW DETECTIONS -- fix the scene/geometry first ***")
