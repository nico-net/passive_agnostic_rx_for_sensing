#!/usr/bin/env python3
"""D1: in resolvable CPIs that produced NO target detection, is the target's energy PRESENT
in the range-Doppler raster (detector problem) or ABSENT (upstream problem)?

Alignment-free by construction: instead of pairing a CPI to a ground-truth epoch (CPI stamps
are host wall clock, GT is simulated time), it scans the target's whole plausible
(range, |velocity|) box and takes the peak. A CONTROL box of identical size, at ranges where
no target exists, gives the false-peak level that box would show on noise alone.

  energy ABSENT  -> target-box peak ~= control-box peak
  energy PRESENT -> target-box peak >> control-box peak, and the loss is in CFAR/NMS/det_quality
"""
import json, re, sys, os
import numpy as np

GUARD = 3


def load_gt(logpath):
    pat = re.compile(r"t=([\d.]+)s.*?(obj\d+)\s+pos=.*?dR=([-\d.]+)m\s+range_rate=([-\d.]+)m/s")
    dR, rate = [], []
    if os.path.exists(logpath):
        for line in open(logpath, errors="ignore"):
            m = pat.search(line)
            if m:
                dR.append(float(m.group(3)))
                rate.append(abs(float(m.group(4))))
    return dR, rate


def box_peak(power, R, D, rlo, rhi, vlo, vhi, vres, floor):
    """Peak SNR (dB over floor) inside |v| in [vlo,vhi] (both signs), range in [rlo,rhi] bins."""
    rlo = max(0, int(rlo)); rhi = min(R - 1, int(rhi))
    if rhi <= rlo:
        return None
    half = D / 2.0
    dlo_p, dhi_p = half - vhi / vres, half - vlo / vres      # positive rate -> below centre
    dlo_n, dhi_n = half + vlo / vres, half + vhi / vres
    best = 0.0
    for a, b in ((dlo_p, dhi_p), (dlo_n, dhi_n)):
        a2, b2 = max(0, int(np.floor(a))), min(D - 1, int(np.ceil(b)))
        if b2 <= a2:
            continue
        best = max(best, float(power[rlo:rhi + 1, a2:b2 + 1].max()))
    if best <= 0 or floor <= 0:
        return None
    return 10.0 * np.log10(best / floor)


def run(name, rundir):
    rpath = os.path.join(rundir, "fused_reports.jsonl")
    dRs, rates = load_gt(os.path.join(rundir, "ue_rx1.log"))
    if not dRs or not os.path.exists(rpath):
        print(f"{name}: missing gt or reports"); return
    r_lo, r_hi = min(dRs), max(dRs)
    v_lo, v_hi = min(rates), max(rates)
    v_med = float(np.median(rates))

    tgt, ctl, tgt_det, n_res_undet, n_blob = [], [], [], 0, 0
    horizon_m = []
    for line in open(rpath, errors="ignore"):
        line = line.strip()
        if not line:
            continue
        try:
            rep = json.loads(line)
        except Exception:
            continue
        blob = rep.get("rvm_blob")
        if not blob or rep.get("vel_res_mps", 0) <= 0:
            continue
        vres, vmax, rres = rep["vel_res_mps"], rep["vel_max_mps"], rep["range_res_m"]
        # resolvable only: target outside the zero-Doppler guard and unaliased
        if v_med > vmax or (v_med / vres) < GUARD:
            continue
        D = int(round(2 * vmax / vres))
        if D <= 0 or len(blob) % D:
            continue
        R = len(blob) // D
        n_blob += 1
        power = np.asarray(blob, dtype=np.float64).reshape(R, D)
        floor = float(np.median(power[power > 0])) if (power > 0).any() else 0.0
        if floor <= 0:
            continue

        detected = any(abs(d.get("bistatic_range_m", -1e9) - x) < 40.0
                       for d in rep.get("detections", []) for x in dRs)

        # the raster is ZEROED beyond the auto range horizon -- work inside the valid extent
        nz = np.nonzero(power.max(axis=1))[0]
        if nz.size == 0:
            continue
        valid_R = int(nz.max())
        horizon_m.append((valid_R + 1) * rres)
        rb_lo, rb_hi = r_lo / rres, min(r_hi / rres, valid_R)
        p_t = box_peak(power, R, D, rb_lo, rb_hi, v_lo, v_hi, vres, floor)
        # control: SAME range band, a |v| band the target cannot occupy
        cv_lo, cv_hi = v_hi * 1.3, min(vmax * 0.95, v_hi * 2.6)
        p_c = (box_peak(power, R, D, rb_lo, rb_hi, cv_lo, cv_hi, vres, floor)
               if cv_hi > cv_lo else None)
        if p_t is None:
            continue
        if detected:
            tgt_det.append(p_t)
        else:
            n_res_undet += 1
            tgt.append(p_t)
            if p_c is not None:
                ctl.append(p_c)

    def q(a, s):
        if not a:
            return f"{s}: n=0"
        a = np.array(a)
        return (f"{s}: n={len(a):3d} med={np.median(a):6.2f} p25={np.percentile(a,25):6.2f} "
                f"p75={np.percentile(a,75):6.2f} max={a.max():6.2f} dB")

    hm = f"{np.median(horizon_m):.0f} m" if horizon_m else "n/a"
    print(f"\n=== {name} ===  resolvable CPIs with blob: {n_blob}   "
          f"resolvable+UNDETECTED: {n_res_undet}   range horizon med: {hm}  "
          f"(target dR {r_lo:.0f}-{r_hi:.0f} m)")
    print("  " + q(tgt_det, "target box, CPIs WITH a detection "))
    print("  " + q(tgt, "target box, resolvable+UNDETECTED  "))
    print("  " + q(ctl, "CONTROL box, same CPIs             "))
    if tgt and ctl:
        d = np.median(tgt) - np.median(ctl)
        print(f"  --> target-minus-control on UNDETECTED CPIs: {d:+.2f} dB")
        print("      %s" % ("ENERGY PRESENT (b): loss is in CFAR/NMS/det_quality" if d > 3.0
                            else "ENERGY ABSENT (a): loss is upstream (integration/ECA+/clutter)"))


if __name__ == "__main__":
    for r in (sys.argv[1:] or ["lostrack", "oscfar", "phase4", "nudft_fix"]):
        d = r if os.path.isdir(r) else f"/tmp/{r}"
        run(os.path.basename(d.rstrip("/")), d)
