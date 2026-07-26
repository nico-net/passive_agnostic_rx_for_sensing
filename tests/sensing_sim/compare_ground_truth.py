#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Compare NR_UE_ISAC detections against the sensing_channel ground-truth log.

Parses "SENSING_CHANNEL gt: ..." lines (injected object ground truth, logged once/sim-second by
sensing_channel_update()) and "SENSING: CPI #N ... detections=K ... top: range=... vel=..." summary
lines from the UE log, and reports the nearest-in-time detection's range/velocity error against
each ground-truth sample.

Usage: compare_ground_truth.py <ue.log>
"""
import re
import sys

GT_RE = re.compile(
    r"SENSING_CHANNEL gt: t=([\d.]+)s obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m "
    r"bistatic_range=([\d.]+)m dR=([-\d.]+)m range_rate=([-\d.]+)m/s"
)
CPI_RE = re.compile(
    r"SENSING: CPI #(\d+) .* detections=(\d+) top: range=([\d.]+) m vel=([-\d.]+) m/s snr=([\d.]+) dB"
)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    gts = []
    cpis = []
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            m = GT_RE.search(line)
            if m:
                gts.append(
                    dict(
                        t=float(m.group(1)),
                        obj=int(m.group(2)),
                        x=float(m.group(3)),
                        y=float(m.group(4)),
                        bistatic_range=float(m.group(5)),
                        dR=float(m.group(6)),
                        range_rate=float(m.group(7)),
                    )
                )
                continue
            m = CPI_RE.search(line)
            if m:
                cpis.append(
                    dict(
                        cpi=int(m.group(1)),
                        detections=int(m.group(2)),
                        range_m=float(m.group(3)),
                        vel_mps=float(m.group(4)),
                        snr_db=float(m.group(5)),
                    )
                )

    print(f"Parsed {len(gts)} ground-truth samples, {len(cpis)} CPI summaries with detections.")
    if not gts:
        print("No ground-truth lines found -- is [sensing_channel] enable=1 and did the run last >=1s?")
        return
    if not cpis:
        print("No CPI detections found -- check csirs_monitor values / RA success / cpi_slots duration.")
        return

    # CPIs don't carry an absolute timestamp in this summary line; approximate by matching each
    # ground-truth sample to the temporally-nearest CPI by ORDER (both are monotonic in time), i.e.
    # scale CPI index across the run's time span. This is a coarse but dependency-free approximation
    # -- for precise correlation, cross-reference cpi_start_time_utc_ns in the DetectionReport
    # JSON-lines file against the ground truth's wall-clock-relative "t=" instead.
    t_span = gts[-1]["t"] - gts[0]["t"] if len(gts) > 1 else 1.0
    print(f"{'t(s)':>6} {'gt_dR(m)':>10} {'gt_rate(m/s)':>13} {'det_range(m)':>13} {'det_vel(m/s)':>13} "
          f"{'range_err(m)':>13} {'vel_err(m/s)':>13}")
    for gt in gts:
        frac = (gt["t"] - gts[0]["t"]) / t_span if t_span > 0 else 0.0
        idx = min(int(frac * len(cpis)), len(cpis) - 1)
        c = cpis[idx]
        range_err = c["range_m"] - gt["dR"]
        vel_err = c["vel_mps"] - gt["range_rate"]
        print(
            f"{gt['t']:6.1f} {gt['dR']:10.2f} {gt['range_rate']:13.3f} {c['range_m']:13.2f} "
            f"{c['vel_mps']:13.3f} {range_err:13.2f} {vel_err:13.3f}"
        )


if __name__ == "__main__":
    main()
