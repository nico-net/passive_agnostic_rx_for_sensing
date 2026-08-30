#!/usr/bin/env python3
"""Synthesise a report JSONL with bearings and tracks, to exercise the monitor with no receiver.

The recorded captures in NICOLA/captures/ predate AoA, so they carry no azimuth and no tracks --
replaying them correctly leaves the Tracking map empty. This writes a scene that has both, with
the geometry computed FORWARD (place a target, derive its differential range and bearing) so the
map is checking the receiver's inverse solution against a known answer, not echoing itself back.

    ./demo_scene.py > /tmp/demo.jsonl
    ./monitor.py --replay /tmp/demo.jsonl
"""
import argparse
import json
import math
import sys

RX = (0.0, 0.0)
TX = (412.0, 338.0)
BASELINE = math.dist(RX, TX)


def target_report(t, cpi, targets):
    dets, tracks = [], []
    for i, (x0, y0, vx, vy) in enumerate(targets):
        x, y = x0 + vx * t, y0 + vy * t
        dR = math.dist((x, y), TX) + math.dist((x, y), RX) - BASELINE
        bearing = math.degrees(math.atan2(y - RX[1], x - RX[0]))
        # range rate by finite difference, which is what the receiver actually measures
        dt = 0.05
        x2, y2 = x0 + vx * (t + dt), y0 + vy * (t + dt)
        dR2 = math.dist((x2, y2), TX) + math.dist((x2, y2), RX) - BASELINE
        rate = (dR2 - dR) / dt
        snr = 14.0 - 2.0 * i
        dets.append({
            "bistatic_range_m": dR, "bistatic_velocity_mps": rate, "snr_db": snr,
            "azimuth_deg": bearing, "azimuth_std_deg": 0.6 + 0.4 * i,
            "range_std_m": 0.9, "rate_std_mps": 0.02, "p_real": 0.95 - 0.1 * i,
        })
        tracks.append({
            "track_id": 14 + i, "bistatic_range_m": dR, "bistatic_velocity_mps": rate,
            "sigma_range_m": 1.2, "coast_count": 0, "updated": True,
            "azimuth_deg": bearing, "azimuth_std_deg": 0.6 + 0.4 * i,
            # what sensing_engine's aoa_localize() would produce; here it is the ground truth,
            # so a map that draws these anywhere else is wrong.
            "position": [x, y],
        })
    return {
        "rx_id": "rx1-demo",
        "illuminator": {"id": "gnb-demo", "pci": 2, "ref_type": "fused(csi_rs+pdsch_data)"},
        "tx_position": list(TX), "rx_position": list(RX),
        "cpi_start_time_utc_ns": int(1.7e18 + cpi * 1e9),
        "cpi_duration_ns": int(1.0e9), "fc_hz": 3748.8e6,
        "range_res_m": 3.05, "vel_res_mps": 0.021,
        "range_max_m": 832.0, "vel_max_mps": 12.0,
        "p_detect": 0.79, "subbin_interp": True,
        "detections": dets, "tracks": tracks,
        "src_occ": [13, 0, 19, 19, 4],
        "sync": {
            "sto": {"n_valid": 72, "n_flywheel": 16, "mean_frac_bin": 0.14, "drift_bins_cpi": 0.02,
                    "is_constant": True, "n_flywheel_rows": 16, "absolute_drift_bins": 0.3},
            "cfo": {"n_valid": 72, "cfo_hz": 42.0 + 6.0 * math.sin(t), "cfo_hz_filtered": 41.0,
                    "residual_phase_rms_rad": 0.08},
            "sfo": {"n_fit": 60, "n_candidate": 72, "sfo_ppm": 0.31, "corrected": True},
            "los_residual": {"baseline_established": True, "detection_found": True,
                             "range_residual_m": 0.2, "vel_residual_mps": 0.001},
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--cpis", type=int, default=120, help="number of CPIs (default 120)")
    args = ap.parse_args()

    # (x0, y0, vx, vy) -- one crossing left-to-right, one receding, one slow
    targets = [(-80.0, 300.0, 6.0, 1.0), (300.0, 420.0, -2.0, 4.5), (250.0, 120.0, 0.6, -0.4)]
    for cpi in range(args.cpis):
        print(json.dumps(target_report(cpi * 1.0, cpi, targets)))


if __name__ == "__main__":
    main()
