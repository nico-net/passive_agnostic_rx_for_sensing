#!/usr/bin/env python3
"""Score a receiver's reported Detection.azimuth_deg against the simulator's own ground truth.

Ground truth comes from the UE log's `SENSING_CHANNEL gt:` lines, which the sensing channel emits once
per SIMULATED second and which now carry `azimuth=<deg>` (the true ENU bearing of each object from
this receiver).

**Time-alignment-free**, for the same reason `score_run.py` is: a DetectionReport's
`cpi_start_time_utc_ns` is host WALL CLOCK, while the ground-truth `t=` is SIMULATED time, and rfsim
runs tens of times slower than real time -- so the two clocks cannot be compared directly. Instead a
detection is matched to the ground-truth sample that best explains it in (dR, range-rate) jointly, and
its azimuth is then scored against THAT sample's azimuth. Matching on both axes pins the trajectory
point tightly enough that the bearing comparison is meaningful; a ghost at a random range or a
harmonic at k x the rate matches nothing and is excluded, because its "bearing" is not a bearing of
any real object.

Usage: score_azimuth.py <run_dir> [range_tol_m] [rate_tol_mps]
"""
import json
import math
import re
import sys


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


def load_truth(ue_log):
    """{obj: [(t, dR, rate, azimuth_deg)]} from the SENSING_CHANNEL gt lines."""
    pat = re.compile(
        r"SENSING_CHANNEL gt: t=([\d.]+)s obj(\d+).*?dR=([-\d.]+)m "
        r"range_rate=([-\d.]+)m/s azimuth=([-\d.]+)deg"
    )
    gt = {}
    with open(ue_log, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                gt.setdefault(int(m.group(2)), []).append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4)), float(m.group(5)))
                )
    return gt


def main():
    csv_prefix = None
    if "--csv" in sys.argv:
        i = sys.argv.index("--csv")
        csv_prefix = sys.argv[i + 1]
        del sys.argv[i:i + 2]
    d = sys.argv[1].rstrip("/")
    rtol = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    vtol = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0

    gt = load_truth(f"{d}/logs/ue.log")
    if not gt:
        print("no SENSING_CHANNEL gt lines with azimuth= found -- old binary, or no sensing channel")
        return 1

    reports = [json.loads(l) for l in open(f"{d}/oaiue_reports.jsonl") if l.strip()]
    if not reports:
        print("no reports")
        return 1

    n_det = n_az = 0
    errs, sigmas = [], []
    per_obj = {o: [] for o in gt}
    for r in reports:
        for det in r["detections"]:
            n_det += 1
            az = det.get("azimuth_deg")
            if az is None:
                continue
            n_az += 1
            # Best-matching point on ANY object's trajectory, in (dR, rate) jointly.
            best = None
            for o, pts in gt.items():
                for (_t, dR, rate, azt) in pts:
                    dr = abs(det["bistatic_range_m"] - dR)
                    dv = abs(det["bistatic_velocity_mps"] - rate)
                    if dr <= rtol and dv <= vtol:
                        cost = (dr / rtol) ** 2 + (dv / vtol) ** 2
                        if best is None or cost < best[0]:
                            best = (cost, o, azt)
            if best is None:
                continue  # ghost / false alarm: not a bearing of any real object
            _c, o, azt = best
            e = wrap180(az - azt)
            errs.append(e)
            per_obj[o].append(e)
            if det.get("azimuth_std_deg") is not None:
                sigmas.append(det["azimuth_std_deg"])

    if csv_prefix is not None:
        a = sorted(abs(e) for e in errs)
        sg = sorted(sigmas)
        def q(v, i):
            return f"{v[i]:.3f}" if v else "nan"
        print(f"{csv_prefix},{len(reports)},{n_det},{n_az},{len(errs)},"
              f"{(sum(errs)/len(errs)) if errs else float('nan'):.3f},"
              f"{q(a, len(a)//2) if a else 'nan'},"
              f"{math.sqrt(sum(e*e for e in errs)/len(errs)) if errs else float('nan'):.3f},"
              f"{q(a, int(0.9*(len(a)-1))) if a else 'nan'},"
              f"{q(a, len(a)-1) if a else 'nan'},"
              f"{q(sg, len(sg)//2) if sg else 'nan'}")
        return 0
    print(f"run: {d}")
    print(f"  reports={len(reports)}  detections={n_det}  carrying an azimuth={n_az} "
          f"({100.0*n_az/max(n_det,1):.0f}%)")
    if not errs:
        print(f"  no detection matched a real target within {rtol:.0f} m / {vtol:.1f} m/s")
        return 1
    a = sorted(abs(e) for e in errs)
    rms = math.sqrt(sum(e * e for e in errs) / len(errs))
    print(f"  matched real-target detections: {len(errs)}")
    print(f"  AZIMUTH ERROR: mean {sum(errs)/len(errs):+6.2f}  median |e| {a[len(a)//2]:5.2f}  "
          f"RMS {rms:5.2f}  p90 {a[int(0.9*(len(a)-1))]:5.2f}  max {a[-1]:5.2f}   (degrees)")
    if sigmas:
        s = sorted(sigmas)
        print(f"  reported sigma (CRB): median {s[len(s)//2]:.2f} deg [{s[0]:.2f}..{s[-1]:.2f}]")
    for o in sorted(per_obj):
        if per_obj[o]:
            pa = sorted(abs(e) for e in per_obj[o])
            print(f"  obj{o}: n={len(pa):3d}  median |e| {pa[len(pa)//2]:5.2f} deg  "
                  f"RMS {math.sqrt(sum(e*e for e in per_obj[o])/len(per_obj[o])):5.2f} deg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
