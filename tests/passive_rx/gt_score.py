#!/usr/bin/env python3
"""GT-based per-CPI coverage/precision scorer (implementation-agent build for research
iteration 003/004's T1/T2 tasks).

NOTE: the Research Agent's own scripts (`scratchpad/e0_axis.py`, `scratchpad/d1_probe.py`)
are not present in this tree (different session's scratchpad, not shared). This is an
independent implementation of the same measurement, built from the `SENSING_CHANNEL gt:`
log lines `sensing_channel.c` emits (1 sample/simulated-second per object: t, utc_ns, pos,
bistatic_range, dR, range_rate, azimuth) rather than re-deriving position from conf
waypoints -- arguably a more direct ground truth, not merely a substitute. Coverage matching
uses the SAME "within 40 m of GT dR" convention iteration 002/003 used, for comparability.

Usage: gt_score.py <ue_rx1.log> <reports.jsonl> [--zero-doppler-guard N]
"""
import argparse
import json
import re
import sys

GT_RE = re.compile(
    r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m "
    r"bistatic_range=([\d.]+)m dR=(-?[\d.]+)m range_rate=(-?[\d.]+)m/s azimuth=([\d.]+)deg"
)


def parse_gt(log_path):
    """Returns {obj_id: [(utc_ns, dR_m, range_rate_mps), ...]} sorted by utc_ns."""
    by_obj = {}
    with open(log_path, errors="replace") as f:
        for line in f:
            m = GT_RE.search(line)
            if not m:
                continue
            utc_ns = int(m.group(2))
            obj = int(m.group(3))
            dR = float(m.group(7))
            rate = float(m.group(8))
            by_obj.setdefault(obj, []).append((utc_ns, dR, rate))
    for obj in by_obj:
        by_obj[obj].sort(key=lambda x: x[0])
    return by_obj


def interp(samples, utc_ns):
    """Linear interpolation of (dR, rate) at utc_ns from a sorted (utc_ns, dR, rate) list.
    Clips to the nearest endpoint outside the sampled range rather than extrapolating."""
    if not samples:
        return None
    if utc_ns <= samples[0][0]:
        return samples[0][1], samples[0][2]
    if utc_ns >= samples[-1][0]:
        return samples[-1][1], samples[-1][2]
    lo, hi = 0, len(samples) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if samples[mid][0] <= utc_ns:
            lo = mid
        else:
            hi = mid
    t0, dr0, rr0 = samples[lo]
    t1, dr1, rr1 = samples[hi]
    if t1 == t0:
        return dr0, rr0
    f = (utc_ns - t0) / (t1 - t0)
    return dr0 + f * (dr1 - dr0), rr0 + f * (rr1 - rr0)


def classify(dR, rate, vel_max, vel_res, zero_doppler_guard):
    aliased = abs(rate) > vel_max if vel_max > 0 else True
    bin_from_center = abs(rate) / vel_res if vel_res > 0 else 1e9
    notched = (not aliased) and (bin_from_center <= zero_doppler_guard)
    resolvable = (not aliased) and (not notched)
    return aliased, notched, resolvable, bin_from_center


def score(log_path, reports_path, zero_doppler_guard=3, range_tol_m=40.0, rate_tol_mps=None):
    gt = parse_gt(log_path)
    if not gt:
        sys.exit(f"no 'SENSING_CHANNEL gt:' lines found in {log_path}")
    reports = [json.loads(l) for l in open(reports_path) if l.strip()]
    reports.sort(key=lambda r: r["cpi_start_time_utc_ns"])

    n_cpi = len(reports)
    n_resolvable = n_notched = n_aliased = 0
    n_cpi_with_gt_hit = 0          # coverage numerator: >=1 target-band detection this CPI
    n_dets_total = n_dets_inband = 0
    per_obj_resolvable_undetected = {o: 0 for o in gt}
    bin_hist = []

    for r in reports:
        utc = r["cpi_start_time_utc_ns"]
        vel_max = r.get("vel_max_mps", 0.0)
        vel_res = r.get("vel_res_mps", 0.0)
        dets = r.get("detections", [])
        n_dets_total += len(dets)

        cpi_resolvable_any = False
        cpi_hit = False
        obj_targets = {}
        for obj, samples in gt.items():
            got = interp(samples, utc)
            if got is None:
                continue
            dR, rate = got
            aliased, notched, resolvable, b = classify(dR, rate, vel_max, vel_res, zero_doppler_guard)
            obj_targets[obj] = (dR, rate, resolvable)
            if resolvable:
                cpi_resolvable_any = True
                bin_hist.append(b)

        if obj_targets:
            # CPI-level classification: aliased/notched/resolvable is per-target, but for the
            # summary table we take the RESOLVABLE union (a CPI in which at least one target is
            # geometrically representable), matching iteration 002/003's per-CPI table framing.
            any_aliased = any(not obj_targets[o][2] and abs(obj_targets[o][1]) > vel_max for o in obj_targets)
            if cpi_resolvable_any:
                n_resolvable += 1
            elif any_aliased:
                n_aliased += 1
            else:
                n_notched += 1

        matched_dets = set()
        for obj, (dR, rate, resolvable) in obj_targets.items():
            hit = False
            for i, d in enumerate(dets):
                dr = abs(d.get("bistatic_range_m", 1e9) - dR)
                if dr > range_tol_m:
                    continue
                if rate_tol_mps is not None:
                    dv = abs(d.get("bistatic_velocity_mps", 1e9) - rate)
                    if dv > rate_tol_mps:
                        continue
                hit = True
                matched_dets.add(i)
                break
            if hit:
                cpi_hit = True
            elif resolvable:
                per_obj_resolvable_undetected[obj] = per_obj_resolvable_undetected.get(obj, 0) + 1
        n_dets_inband += len(matched_dets)
        if cpi_hit:
            n_cpi_with_gt_hit += 1

    import statistics as st
    out = {
        "n_cpi": n_cpi,
        "resolvable_pct": 100.0 * n_resolvable / n_cpi if n_cpi else 0.0,
        "notched_pct": 100.0 * n_notched / n_cpi if n_cpi else 0.0,
        "aliased_pct": 100.0 * n_aliased / n_cpi if n_cpi else 0.0,
        "coverage_pct": 100.0 * n_cpi_with_gt_hit / n_cpi if n_cpi else 0.0,
        "coverage_of_resolvable_pct": (100.0 * n_cpi_with_gt_hit / n_resolvable
                                       if n_resolvable else 0.0),
        "n_cpi_with_gt_hit": n_cpi_with_gt_hit,
        "n_resolvable": n_resolvable,
        "n_dets_total": n_dets_total,
        "n_dets_inband": n_dets_inband,
        "inband_precision_pct": 100.0 * n_dets_inband / n_dets_total if n_dets_total else 0.0,
        "target_bin_median": st.median(bin_hist) if bin_hist else float("nan"),
        "target_bin_p10": (sorted(bin_hist)[len(bin_hist) // 10] if len(bin_hist) >= 10 else
                           (min(bin_hist) if bin_hist else float("nan"))),
        "resolvable_undetected_per_obj": per_obj_resolvable_undetected,
    }
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ue_log")
    ap.add_argument("reports_jsonl")
    ap.add_argument("--zero-doppler-guard", type=float, default=3.0)
    ap.add_argument("--range-tol-m", type=float, default=40.0)
    ap.add_argument("--rate-tol-mps", type=float, default=None)
    a = ap.parse_args()
    out = score(a.ue_log, a.reports_jsonl, a.zero_doppler_guard, a.range_tol_m, a.rate_tol_mps)
    for k, v in out.items():
        print(f"{k}: {v}")
