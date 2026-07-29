#!/usr/bin/env python3
"""Ground-truth-referenced scorer for a sensing_sim MOT run.

A detection/track is credited to a real target k ONLY IF it lies near target k's actual
(differential-range, range-rate) TRAJECTORY CURVE -- simultaneously within range_tol AND vel_tol of
SOME point on the curve. This is time-alignment-free (searches the whole curve) yet still rejects
harmonic ghosts: a ghost at 2x/3x a target's velocity never lands on the curve, because at the time
the curve passes through the ghost's range, the true velocity is the fundamental, not the multiple.

Reports the metrics that actually matter (vs. the misleading unique_track_ids we optimized before):
  - detection precision : fraction of raw detections that match SOME real target (rest = false/ghost)
  - per-target detection coverage : fraction of CPIs in which >=1 detection matches that target
  - same two for CONFIRMED tracks
Usage: score_run.py <run_dir> [range_tol_m] [vel_tol_mps]
"""
import sys, re
import numpy as np

# Optional "--csv PREFIX": one machine-readable row instead of the human report, so an A/B batch can
# aggregate many runs. Also harvests the per-CPI sync diagnostics (locked rows / flywheel count) from
# the same ue.log, since a sync A/B needs to see WHY an arm behaved as it did, not just that it did.
CSV = None
if "--csv" in sys.argv:
    i = sys.argv.index("--csv")
    CSV = sys.argv[i + 1]
    del sys.argv[i:i + 2]

run_dir  = sys.argv[1]
RANGE_TOL = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0   # ~5 range bins
VEL_TOL   = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0    # ~4 velocity bins, << the 8-20 m/s harmonic offset

ue_log = f"{run_dir}/logs/ue.log"
det_csv = f"{run_dir}/oaiue_sensing_detections.csv"

# --- Parse ground-truth trajectory curves: obj_id -> list of (t, dR, range_rate) ---
gt = {}
gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s(?:\s+utc_ns=\d+)? obj(\d+).*?dR=([\-\d.]+)m range_rate=([\-\d.]+)m/s")
with open(ue_log, errors="ignore") as f:
    for line in f:
        m = gt_re.search(line)
        if m:
            t, oid, dR, rate = float(m[1]), int(m[2]), float(m[3]), float(m[4])
            gt.setdefault(oid, []).append((t, dR, rate))

if not gt:
    print("NO GROUND TRUTH FOUND"); sys.exit(1)

# Fine-resample each target's (dR, rate) curve for nearest-point matching.
curves = {}
for oid, pts in gt.items():
    pts = sorted(set(pts))
    ts = np.array([p[0] for p in pts])
    dR = np.array([p[1] for p in pts])
    rate = np.array([p[2] for p in pts])
    tf = np.linspace(ts[0], ts[-1], 2000)
    curves[oid] = (np.interp(tf, ts, dR), np.interp(tf, ts, rate))

def match_target(rng, vel):
    """Return the obj id whose trajectory curve this (range,velocity) lies on, or None."""
    for oid, (cR, cV) in curves.items():
        if np.any((np.abs(cR - rng) <= RANGE_TOL) & (np.abs(cV - vel) <= VEL_TOL)):
            return oid
    return None

# --- Raw detections from the CSV: cpi,range_bin,doppler_bin,range_m,vel_mps,snr_db ---
det_by_cpi = {}
with open(det_csv, errors="ignore") as f:
    for line in f:
        p = line.strip().split(",")
        if len(p) != 6: continue
        cpi, rng, vel = int(p[0]), float(p[3]), float(p[4])
        det_by_cpi.setdefault(cpi, []).append((rng, vel))

det_total = det_match = 0
det_cpi_cover = {oid: set() for oid in curves}
for cpi, dets in det_by_cpi.items():
    for rng, vel in dets:
        det_total += 1
        oid = match_target(rng, vel)
        if oid is not None:
            det_match += 1
            det_cpi_cover[oid].add(cpi)

# --- Confirmed tracks from the ue.log track lines ---
trk_re = re.compile(r"SENSING: track CPI #(\d+) .*?range=([\-\d.]+) m rate=([+\-\d.]+) m/s")
trk_by_cpi = {}
with open(ue_log, errors="ignore") as f:
    for line in f:
        m = trk_re.search(line)
        if m:
            cpi, rng, vel = int(m[1]), float(m[2]), float(m[3])
            trk_by_cpi.setdefault(cpi, []).append((rng, vel))

trk_total = trk_match = 0
trk_cpi_cover = {oid: set() for oid in curves}
for cpi, trks in trk_by_cpi.items():
    for rng, vel in trks:
        trk_total += 1
        oid = match_target(rng, vel)
        if oid is not None:
            trk_match += 1
            trk_cpi_cover[oid].add(cpi)

n_det_cpi = len(det_by_cpi)
n_trk_cpi = len(trk_by_cpi)

if CSV is not None:
    # STO[n=<locked> fly=<flywheel> ...] from the per-CPI sync log line.
    sync_re = re.compile(r"SENSING: sync CPI #\d+ STO\[n=(\d+) fly=(\d+)")
    locked, fly = [], []
    with open(ue_log, errors="ignore") as fh:
        for line in fh:
            m = sync_re.search(line)
            if m:
                locked.append(int(m[1])); fly.append(int(m[2]))
    fly_mean = (sum(fly) / len(fly)) if fly else float("nan")
    lock_mean = (sum(locked) / len(locked)) if locked else float("nan")
    cov = [len(det_cpi_cover[o]) / max(n_det_cpi, 1) * 100.0 for o in sorted(curves)]
    cov += [float("nan")] * (4 - len(cov))
    print(f"{CSV},{n_det_cpi},{n_trk_cpi},{det_total},{det_match},"
          f"{100*det_match/max(det_total,1):.2f},{trk_total},{trk_match},"
          f"{100*trk_match/max(trk_total,1):.2f},"
          + ",".join(f"{c:.2f}" for c in cov[:4])
          + f",{fly_mean:.2f},{lock_mean:.2f}")
    sys.exit(0)

print(f"=== {run_dir}  (tol: range=±{RANGE_TOL}m vel=±{VEL_TOL}m/s) ===")
print(f"targets: {sorted(curves)}   detection-CPIs: {n_det_cpi}   track-CPIs: {n_trk_cpi}")
print(f"\nRAW DETECTIONS: {det_total} total, {det_match} match a real target "
      f"({100*det_match/max(det_total,1):.0f}% precision, {det_total-det_match} false)")
for oid in sorted(curves):
    print(f"  obj{oid} detection coverage: {len(det_cpi_cover[oid])}/{n_det_cpi} CPIs "
          f"({100*len(det_cpi_cover[oid])/max(n_det_cpi,1):.0f}%)")
print(f"\nCONFIRMED TRACKS: {trk_total} occurrences, {trk_match} match a real target "
      f"({100*trk_match/max(trk_total,1):.0f}% precision, {trk_total-trk_match} ghost)")
for oid in sorted(curves):
    print(f"  obj{oid} track coverage: {len(trk_cpi_cover[oid])}/{n_trk_cpi} CPIs "
          f"({100*len(trk_cpi_cover[oid])/max(n_trk_cpi,1):.0f}%)")
