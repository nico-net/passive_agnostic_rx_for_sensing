#!/usr/bin/env python3
"""Phase C feasibility budget: can an intra-CPI bearing RATE constrain velocity at track birth?

GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md §3 Phase C proposes splitting a CPI's slow-time rows in half,
running the AoA estimator on each half, and using omega = (theta_2 - theta_1)/dt as a second, TRUSTED
constraint so velocity can be solved from ONE CPI at ONE receiver:

    [ (u_t + u_r)^T ]       [ Rdot_meas   ]
    [      n^T      ] v  =  [ R_r * omega ]

The handover's own instruction is to re-derive the error budget from a REAL capture before writing any
of that, and to stop if the ratio is not comfortably below 1. This script is that derivation. It uses
nothing but what a capture already reports, so it can be re-run on any future scene:

    sigma_theta   per-detection azimuth_std_deg (the estimator's own CRB), measured
    dt            half of cpi_duration_ns -- the separation of the two half-aperture bearings
    sigma_dtheta  = 2 * sigma_theta   (each half-aperture is sqrt(2) worse than the full CPI, and
                                       their DIFFERENCE is another sqrt(2))
    sigma_omega   = sigma_dtheta / dt
    sigma_vt      = R_r * sigma_omega  -- the tangential-velocity uncertainty the constraint carries
                                          R_r comes from the detection's own (range, bearing) fix

and compares sigma_vt against the target speeds the scene actually contains. If sigma_vt is several
times the target speed, the second row of that 2x2 system is noise and Phase C cannot work at this
CPI duration -- which is exactly the outcome the handover predicts.

Usage: phase_c_budget.py <run_dir> [more_run_dirs...]
"""
import json
import math
import re
import sys


def load_reports(run_dir):
    with open(f"{run_dir}/oaiue_reports.jsonl") as f:
        return [json.loads(l) for l in f if l.strip()]


def load_target_speeds(ue_log):
    """Per-object world speed (m/s) from consecutive gt positions, and |range_rate| samples."""
    pat = re.compile(
        r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m "
        r".*?range_rate=([-\d.]+)m/s"
    )
    pts, rates = {}, []
    with open(ue_log, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                pts.setdefault(int(m.group(2)), []).append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4)))
                )
                rates.append(abs(float(m.group(5))))
    speeds = {}
    for oid, p in pts.items():
        p.sort()
        seg = [
            math.hypot(b[1] - a[1], b[2] - a[2]) / (b[0] - a[0])
            for a, b in zip(p, p[1:])
            if b[0] > a[0]
        ]
        if seg:
            speeds[oid] = sum(seg) / len(seg)
    return speeds, rates


def localize(tx, rx, reported_range, az_deg):
    """Ray n bistatic ellipse -- the same closed form as isac_aoa.cc's aoa_localize()."""
    baseline = math.hypot(rx[0] - tx[0], rx[1] - tx[1])
    r_b = reported_range + baseline
    if r_b <= 0:
        return None
    th = math.radians(az_deg)
    ux, uy = math.cos(th), math.sin(th)
    ax, ay = rx[0] - tx[0], rx[1] - tx[1]
    denom = 2.0 * (r_b + ax * ux + ay * uy)
    if abs(denom) < 1e-9:
        return None
    t = (r_b * r_b - (ax * ax + ay * ay)) / denom
    if t <= 0:
        return None
    return (rx[0] + ux * t, rx[1] + uy * t)


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, int(q * (len(s) - 1)))]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for run_dir in sys.argv[1:]:
        run_dir = run_dir.rstrip("/")
        reports = load_reports(run_dir)
        speeds, rates = load_target_speeds(f"{run_dir}/logs/ue.log")
        if not reports:
            print(f"{run_dir}: no reports")
            continue

        sigmas, dts, r_rs, sigma_vts = [], [], [], []
        for rep in reports:
            dt = rep["cpi_duration_ns"] * 1e-9 / 2.0  # half-CPI separation of the two bearings
            tx, rx = rep["tx_position"], rep["rx_position"]
            for d in rep["detections"]:
                s = d.get("azimuth_std_deg")
                az = d.get("azimuth_deg")
                if s is None or az is None or not (s > 0):
                    continue
                p = localize(tx, rx, d["bistatic_range_m"], az)
                if p is None:
                    continue
                r_r = math.hypot(p[0] - rx[0], p[1] - rx[1])
                sigma_omega = 2.0 * math.radians(s) / dt
                sigmas.append(s)
                dts.append(dt)
                r_rs.append(r_r)
                sigma_vts.append(r_r * sigma_omega)

        print(f"=== Phase C budget: {run_dir} ===")
        if not sigma_vts:
            print("  no detection carried both an azimuth and its sigma -- nothing to budget")
            continue
        v_ref = max(speeds.values()) if speeds else float("nan")
        print(f"  CPIs {len(reports)}   detections with a bearing+sigma {len(sigma_vts)}")
        print(f"  cpi_duration        median {2 * pct(dts, 0.5):.3f} s   "
              f"-> half-aperture separation dt = {pct(dts, 0.5):.3f} s")
        print(f"  sigma_theta         median {pct(sigmas, 0.5):.3f} deg  "
              f"(p10 {pct(sigmas, 0.1):.3f}, p90 {pct(sigmas, 0.9):.3f})")
        print(f"  R_r (fix to rx)     median {pct(r_rs, 0.5):.1f} m")
        print(f"  => sigma_v_tangential  median {pct(sigma_vts, 0.5):.1f} m/s  "
              f"(best case p10 {pct(sigma_vts, 0.1):.1f} m/s)")
        print(f"  target world speeds: " +
              (", ".join(f"obj{o} {v:.2f} m/s" for o, v in sorted(speeds.items())) or "unknown"))
        if rates:
            print(f"  |bistatic range rate| median {pct(rates, 0.5):.2f} m/s "
                  f"(max {max(rates):.2f})")
        if speeds:
            ratio = pct(sigma_vts, 0.5) / v_ref
            best = pct(sigma_vts, 0.1) / v_ref
            print(f"  RATIO sigma_v / fastest target speed: median {ratio:.1f}x  (best case {best:.1f}x)")
            print("  VERDICT: " + (
                "viable -- the bearing-rate constraint carries real information."
                if ratio < 1.0 else
                f"NOT viable. The constraint is {ratio:.0f}x noisier than the signal it would "
                f"measure;\n           the second row of the 2x2 system would be pure noise. "
                f"Phase C needs a\n           dwell ~{ratio:.0f}x longer or a bearing accuracy "
                f"~{ratio:.0f}x better before it is worth building."))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
