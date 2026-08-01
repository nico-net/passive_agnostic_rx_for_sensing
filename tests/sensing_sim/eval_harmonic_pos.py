#!/usr/bin/env python3
"""Offline A/B of Phase A (position-anchored harmonic rejection) on a RECORDED capture.

Why offline rather than two live runs: everything `harmonic_pos_reject()` consumes -- range, velocity,
SNR, azimuth, and the surveyed tx/rx positions -- is already in the DetectionReport, so the filter can
be replayed exactly against recorded detections. That makes the two arms byte-identical in their
input, which matters a great deal on this harness: identical repetitions have been measured swinging
8-58 % in fused precision (CLAUDE.md §9), so a two-capture A/B of a filter this selective would be
swamped by run-to-run variance. Same design as `_run_aoa_vs_3rx.sh`: capture once, score every arm
from that one capture.

This mirrors `isac_aoa.cc`'s `harmonic_pos_reject()` step for step. If that function changes, this
must change with it -- the point of the duplication is to be able to A/B without a rebuild-and-recapture
cycle, not to be a second implementation of record.

The decisive output is not how many detections it drops but WHAT it drops, scored against ground
truth: dropping ghosts is the feature working; dropping detections that lie on a real target's
trajectory is the feature costing coverage.

Usage: eval_harmonic_pos.py <run_dir> [--chi2 9] [--gate-range-tol-m 8] [--max-k 4]
                            [--harmonic-tol 0.15] [--snr-margin 6] [--range-tol 15] [--vel-tol 3]
"""
import argparse
import json
import math
import re
import sys

import numpy as np


def curve_times(curves, d, range_tol, vel_tol, tf):
    """Every simulated time at which SOME object's trajectory explains this detection."""
    out = []
    for c_r, c_v in curves.values():
        hit = (np.abs(c_r - d["bistatic_range_m"]) <= range_tol) & (
            np.abs(c_v - d["bistatic_velocity_mps"]) <= vel_tol
        )
        out.extend(tf[hit].tolist())
    return out


def cpi_epoch(reports, curves, range_tol, vel_tol, tf, window_s):
    """Estimate each CPI's SIMULATED time from its own detections, and score consistency.

    Why this is needed, and why it is not the log-pairing trap: a purely time-alignment-free scorer
    (score_run.py, and the first version of this script) credits a detection to a target if it lies
    anywhere on that target's whole (dR, rate) curve. That is fine for measuring coverage, and WRONG
    for judging a harmonic rejector -- because a target whose range-rate sweeps through a 2:1 span at
    nearly constant dR passes through its own 2nd-harmonic locus later in the run. So a genuine ghost
    at 2x the CURRENT rate is scored "real", and rejecting it is counted as a cost rather than a win.

    The fix takes the time from the DATA, not from a log line: within one CPI the true target has ONE
    rate, so the times implied by that CPI's own detections cluster around the CPI's real epoch, while
    a ghost's implied time is an outlier. Densest-window vote per CPI, then a detection counts as real
    only if it can be explained at that epoch. Its own validity is checkable and IS checked below: the
    recovered epochs must increase monotonically with CPI index, which nothing in the estimator
    enforces.
    """
    epochs = []
    for rep in reports:
        cand = []
        for d in rep["detections"]:
            cand.extend(curve_times(curves, d, range_tol, vel_tol, tf))
        if not cand:
            epochs.append(None)
            continue
        cand.sort()
        best_n, best_t = 0, None
        j = 0
        for i, t0 in enumerate(cand):
            while j < len(cand) and cand[j] <= t0 + window_s:
                j += 1
            if j - i > best_n:
                best_n, best_t = j - i, cand[i + (j - i) // 2]
        epochs.append(best_t)
    known = [(i, t) for i, t in enumerate(epochs) if t is not None]
    rho = float("nan")
    if len(known) > 2:
        xs = np.array([k[0] for k in known], dtype=float)
        ys = np.array([k[1] for k in known], dtype=float)
        if xs.std() > 0 and ys.std() > 0:
            rho = float(np.corrcoef(xs, ys)[0, 1])
    return epochs, rho


def load_curves(ue_log):
    gt = {}
    pat = re.compile(
        r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+).*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s"
    )
    with open(ue_log, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                gt.setdefault(int(m.group(2)), []).append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4)))
                )
    curves, tf = {}, None
    for oid, pts in gt.items():
        pts = sorted(set(pts))
        if len(pts) < 2:
            continue
        ts = np.array([p[0] for p in pts])
        tf = np.linspace(ts[0], ts[-1], 2000)
        curves[oid] = (
            np.interp(tf, ts, np.array([p[1] for p in pts])),
            np.interp(tf, ts, np.array([p[2] for p in pts])),
        )
    return curves, tf


def localize(tx, rx, reported_range, az_deg):
    """Ray n bistatic ellipse -- byte-for-byte the algebra in isac_aoa.cc's aoa_localize()."""
    baseline = math.hypot(rx[0] - tx[0], rx[1] - tx[1])
    r_b = reported_range + baseline
    if not math.isfinite(r_b) or r_b <= 0:
        return None
    th = math.radians(az_deg)
    ux, uy = math.cos(th), math.sin(th)
    ax, ay = rx[0] - tx[0], rx[1] - tx[1]
    denom = 2.0 * (r_b + ax * ux + ay * uy)
    if abs(denom) < 1e-9:
        return None
    t = (r_b * r_b - (ax * ax + ay * ay)) / denom
    if not math.isfinite(t) or t <= 0:
        return None
    return (rx[0] + ux * t, rx[1] + uy * t)


def same_reflection(rx, pi, az_deg_i, az_std_i, pj, az_std_j, a):
    """Byte-for-byte the algebra in isac_aoa.cc's same_reflection() (2026-07-30 rewrite).

    Decomposes the two fixes' separation, along detection i's own line of sight, into a RADIAL
    residual (flat metre tolerance -- no per-detection range sigma exists to chi2-ize it) and a
    TANGENTIAL residual, chi2-normalised by each detection's OWN reported azimuth_std_deg. Replaces
    the flat Euclidean gate that was measured to lose to the range anchor (GHOST_KINEMATIC_
    CONSISTENCY_HANDOVER.md 7.3): a real harmonic pair's two fixes are dominated by independent
    bearing noise, not geometry, so a gate tight enough to reject unrelated targets was also too
    tight to hold real pairs together.
    """
    th = math.radians(az_deg_i)
    ur_x, ur_y = math.cos(th), math.sin(th)
    ut_x, ut_y = -math.sin(th), math.cos(th)
    dx, dy = pi[0] - pj[0], pi[1] - pj[1]
    d_r = dx * ur_x + dy * ur_y
    d_t = dx * ut_x + dy * ut_y
    if abs(d_r) > a.gate_range_tol_m:
        return False
    rr_i = math.hypot(pi[0] - rx[0], pi[1] - rx[1])
    rr_j = math.hypot(pj[0] - rx[0], pj[1] - rx[1])
    sig_i = rr_i * math.radians(max(az_std_i, 0.1))
    sig_j = rr_j * math.radians(max(az_std_j, 0.1))
    var_t = sig_i * sig_i + sig_j * sig_j
    return d_t * d_t <= a.chi2 * var_t


def rejected_indices(rep, a, anchor="position"):
    """Indices the harmonic test would drop from this report's detection list.

    anchor="position" is Phase A (isac_aoa.cc's harmonic_pos_reject, chi2-gated). anchor="range"
    emulates the EXISTING range-bin-anchored test in range_doppler.cc, using `harmonic_guard` bins x
    range_res_m as the range window, so the two anchors can be compared on identical detections.

    The comparison matters because the position anchor is STRICTLY TIGHTER than the range anchor: a
    pair that shares a fix necessarily shares a range, but not the reverse. Phase A is therefore a
    REPLACEMENT for the range anchor rather than something to run after it.
    """
    dets = rep["detections"]
    tx, rx = rep["tx_position"], rep["rx_position"]
    pos, az_std = [], []
    for d in dets:
        az = d.get("azimuth_deg")
        s = d.get("azimuth_std_deg")
        if az is None or (anchor == "position" and s is None):
            pos.append(None)
            az_std.append(None)
            continue
        pos.append(localize(tx, rx, d["bistatic_range_m"], az))
        az_std.append(s)
    if anchor == "position" and sum(p is not None for p in pos) < 2:
        return set()
    rwin = a.harmonic_guard * (rep.get("range_res_m") or 0.0)
    out = set()
    for i, di in enumerate(dets):
        if anchor == "position" and pos[i] is None:
            continue
        vc = abs(di["bistatic_velocity_mps"])
        for j, dj in enumerate(dets):
            if i == j or (anchor == "position" and pos[j] is None):
                continue
            vo = abs(dj["bistatic_velocity_mps"])
            if vo < 1e-3 or vc <= vo or dj["snr_db"] < di["snr_db"] - a.snr_margin:
                continue
            if anchor == "position":
                if not same_reflection(rx, pos[i], di["azimuth_deg"], az_std[i], pos[j], az_std[j], a):
                    continue
            elif abs(di["bistatic_range_m"] - dj["bistatic_range_m"]) > rwin:
                continue
            ratio = vc / vo
            k = round(ratio)
            if 2 <= k <= a.max_k and abs(ratio - k) <= a.harmonic_tol:
                out.add(i)
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--chi2", type=float, default=9.0,
                    help="1-dof chi2 threshold on the tangential (bearing-driven) residual, 9=3sigma")
    ap.add_argument("--gate-range-tol-m", type=float, default=8.0, dest="gate_range_tol_m",
                    help="flat tolerance on the RADIAL residual only (see same_reflection())")
    ap.add_argument("--max-k", type=int, default=4)
    ap.add_argument("--harmonic-tol", type=float, default=0.15)
    ap.add_argument("--snr-margin", type=float, default=6.0)
    ap.add_argument("--range-tol", type=float, default=15.0)
    ap.add_argument("--vel-tol", type=float, default=3.0)
    ap.add_argument("--harmonic-guard", type=float, default=4.0,
                    help="range-bin window of the EXISTING range-anchored test, for the comparison")
    ap.add_argument("--epoch-window-s", type=float, default=0.6,
                    help="half-width of the per-CPI epoch vote (simulated seconds)")
    a = ap.parse_args()
    run_dir = a.run_dir.rstrip("/")

    reports = [json.loads(l) for l in open(f"{run_dir}/oaiue_reports.jsonl") if l.strip()]
    curves, tf = load_curves(f"{run_dir}/logs/ue.log")
    if not reports:
        print("no reports")
        return 1
    if not curves:
        print("NO GROUND TRUTH -- cannot say whether a drop is a ghost or a real target")
        return 1

    epochs, rho = cpi_epoch(reports, curves, a.range_tol, a.vel_tol, tf, a.epoch_window_s)

    def real_anytime(d):
        return bool(curve_times(curves, d, a.range_tol, a.vel_tol, tf))

    def real_at_epoch(d, ep):
        if ep is None:
            return real_anytime(d)  # no epoch recoverable: fall back, do not invent a verdict
        return any(abs(t - ep) <= a.epoch_window_s
                   for t in curve_times(curves, d, a.range_tol, a.vel_tol, tf))

    n_det = 0
    cpis_touched = 0
    # Two scorings, deliberately reported side by side (see cpi_epoch's docstring).
    tally = {k: dict(drop_real=0, drop_ghost=0, kept_real=0, kept_total=0, before=0)
             for k in ("anytime", "epoch")}
    cov_before, cov_after = set(), set()
    for idx, rep in enumerate(reports):
        rej = rejected_indices(rep, a)
        if rej:
            cpis_touched += 1
        ep = epochs[idx]
        for i, d in enumerate(rep["detections"]):
            n_det += 1
            verdict = {"anytime": real_anytime(d), "epoch": real_at_epoch(d, ep)}
            if verdict["epoch"]:
                cov_before.add(idx)
            for k, real in verdict.items():
                t = tally[k]
                t["before"] += real
                if i in rej:
                    t["drop_real"] += real
                    t["drop_ghost"] += not real
                else:
                    t["kept_total"] += 1
                    t["kept_real"] += real
            if i not in rej and verdict["epoch"]:
                cov_after.add(idx)

    n_drop = sum(1 for idx, rep in enumerate(reports) for i in range(len(rep["detections"]))
                 if i in rejected_indices(rep, a))
    n_cpi = len(reports)
    print(f"=== Phase A offline A/B: {run_dir} ===")
    print(f"  params: chi2={a.chi2}  gate_range_tol_m={a.gate_range_tol_m}  max_k={a.max_k}  "
          f"harmonic_tol={a.harmonic_tol}  snr_margin={a.snr_margin} dB")
    print(f"  CPIs {n_cpi}   detections {n_det}")
    print()
    print(f"  recovered per-CPI epoch vs CPI index: rho = {rho:+.3f} "
          f"({'usable -- the epochs advance with the run' if rho > 0.8 else 'NOT USABLE: read only the anytime rows'})")
    print()
    print(f"  DROPPED {n_drop} detections ({100.0 * n_drop / max(n_det, 1):.1f} %) "
          f"across {cpis_touched} CPIs")
    for k, label in (("anytime", "anytime  (whole trajectory; inflates the cost -- see cpi_epoch)"),
                     ("epoch", "at-epoch (this CPI's own recovered time; the honest one)")):
        t = tally[k]
        before = 100.0 * t["before"] / max(n_det, 1)
        after = 100.0 * t["kept_real"] / max(t["kept_total"], 1)
        print(f"    scored {label}")
        print(f"      dropped ghosts {t['drop_ghost']:4d}  <- win     "
              f"dropped real {t['drop_real']:4d}  <- cost")
        print(f"      detection precision {before:.1f} % -> {after:.1f} %")
    print(f"  CPIs containing >=1 real-target detection: {len(cov_before)} -> {len(cov_after)} "
          f"(coverage must NOT drop)")
    # ---- anchor comparison: what does each anchor reject, and is it real? -------------------------
    print()
    print("  ANCHOR COMPARISON on the same detections (position = Phase A, range = the existing test)")
    only = {"position": [0, 0], "range": [0, 0], "both": [0, 0]}
    for idx, rep in enumerate(reports):
        rp = rejected_indices(rep, a, "position")
        rr = rejected_indices(rep, a, "range")
        ep = epochs[idx]
        for i, d in enumerate(rep["detections"]):
            key = None
            if i in rp and i in rr:
                key = "both"
            elif i in rp:
                key = "position"
            elif i in rr:
                key = "range"
            if key is None:
                continue
            only[key][0 if real_at_epoch(d, ep) else 1] += 1
    for k, label in (("both", "rejected by BOTH anchors      "),
                     ("position", "rejected ONLY by the position "),
                     ("range", "rejected ONLY by the range    ")):
        real_n, ghost_n = only[k]
        print(f"    {label}: {real_n + ghost_n:4d}  (real {real_n:4d} / ghost {ghost_n:4d})")
    spared_real, spared_ghost = only["range"]
    print(f"    -> switching the anchor from range to position SPARES {spared_real} real detections "
          f"and {spared_ghost} ghosts,")
    print(f"       and newly rejects {only['position'][0]} real / {only['position'][1]} ghosts.")
    # Resulting precision per anchor, computed rather than eyeballed: sparing a real detection is a
    # gain and sparing a ghost is a loss, so the sign of the swap is not readable off the two counts.
    base_real = tally["epoch"]["before"]
    for k, label in (("none", "no harmonic rejection    "),
                     ("range", "range-anchored (existing)"),
                     ("position", "position-anchored (Phase A)")):
        if k == "none":
            dr = dg = 0
        elif k == "range":
            dr = only["both"][0] + only["range"][0]
            dg = only["both"][1] + only["range"][1]
        else:
            dr = only["both"][0] + only["position"][0]
            dg = only["both"][1] + only["position"][1]
        kept = n_det - dr - dg
        print(f"    precision with {label}: {100.0 * (base_real - dr) / max(kept, 1):.2f} %  "
              f"(dropped {dr + dg}: {dg} ghost / {dr} real)")

    if n_drop == 0:
        print()
        print("  The filter fired on nothing. On this capture it is UNMEASURABLE -- which is a")
        print("  statement about the scene's ghost population, not evidence that the filter works.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
