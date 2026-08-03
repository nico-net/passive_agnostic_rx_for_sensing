#!/usr/bin/env python3
"""E0: is the target inside the zero-Doppler notch? Per-CPI axis geometry vs ground truth.

Reports, for each run: the delivered Doppler axis (vel_res/vel_max), where the target's
bistatic range-rate lands on it in BINS, and the fraction of CPIs in which the target is
(a) aliased, (b) inside the zero_doppler_guard notch, (c) resolvable.
"""
import json, re, sys, os, statistics as st

GUARD = 3  # zero_doppler_guard, bins


def load_gt(logpath):
    """-> dict obj -> list of (t, dR, |dR/dt|)"""
    gt = {}
    if not os.path.exists(logpath):
        return gt
    pat = re.compile(
        r"t=([\d.]+)s.*?(obj\d+)\s+pos=\(([-\d.]+),([-\d.]+)\)m.*?dR=([-\d.]+)m\s+range_rate=([-\d.]+)m/s")
    for line in open(logpath, errors="ignore"):
        m = pat.search(line)
        if m:
            gt.setdefault(m.group(2), []).append(
                (float(m.group(1)), float(m.group(5)), abs(float(m.group(6)))))
    return gt


def load_reports(path):
    rs = []
    if not os.path.exists(path):
        return rs
    for line in open(path, errors="ignore"):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except Exception:
            continue
        if "vel_res_mps" in r:
            rs.append(r)
    return rs


def pct(x, n):
    return 100.0 * x / n if n else 0.0


def analyse(name, rundir):
    rs = load_reports(os.path.join(rundir, "fused_reports.jsonl"))
    gt = load_gt(os.path.join(rundir, "ue_rx1.log"))
    if not rs:
        print(f"{name:12s} NO REPORTS")
        return None
    rates = [v for lst in gt.values() for (_, _, v) in lst]
    dRs = [d for lst in gt.values() for (_, d, _) in lst]
    if not rates:
        print(f"{name:12s} NO GROUND TRUTH")
        return None
    v_lo, v_med, v_hi = min(rates), st.median(rates), max(rates)

    vres = [r["vel_res_mps"] for r in rs]
    vmax = [r["vel_max_mps"] for r in rs]
    ndop = [2 * a / b for a, b in zip(vmax, vres)]
    ndet = [len(r.get("detections", [])) for r in rs]

    # classify each CPI at the MEDIAN target rate
    alias = notch = ok = 0
    bins = []
    for r in rs:
        b = v_med / r["vel_res_mps"]
        bins.append(b)
        if v_med > r["vel_max_mps"]:
            alias += 1
        elif b < GUARD:
            notch += 1
        else:
            ok += 1
    n = len(rs)

    # target-band detection coverage: any detection within +-40 m of any GT dR
    tol = 40.0
    hit = 0
    for r in rs:
        got = False
        for d in r.get("detections", []):
            rg = d.get("bistatic_range_m", d.get("range_m"))
            if rg is None:
                continue
            if any(abs(rg - x) < tol for x in dRs):
                got = True
                break
        if got:
            hit += 1

    print(f"{name:12s} CPIs={n:5d}  vel_res med={st.median(vres):7.3f}  "
          f"vel_max med={st.median(vmax):7.1f}  Ndopp med={st.median(ndop):5.0f}")
    print(f"{'':12s}   target |dR/dt| {v_lo:.1f}-{v_hi:.1f} (med {v_med:.1f}) m/s "
          f"-> BIN med={st.median(bins):6.2f} p10={sorted(bins)[n//10]:6.2f} "
          f"p90={sorted(bins)[9*n//10]:6.2f}")
    print(f"{'':12s}   aliased {pct(alias,n):5.1f}%   NOTCHED {pct(notch,n):5.1f}%   "
          f"resolvable {pct(ok,n):5.1f}%   | dets/CPI={sum(ndet)/n:.2f}  "
          f"target-band coverage {pct(hit,n):5.1f}%")
    return dict(name=name, n=n, vres=st.median(vres), vmax=st.median(vmax),
                ndop=st.median(ndop), bin=st.median(bins), notched=pct(notch, n),
                resolvable=pct(ok, n), cov=pct(hit, n), dets=sum(ndet) / n)


if __name__ == "__main__":
    runs = sys.argv[1:] or ["nudft_fix", "lostrack", "phase4", "oscfar", "longrun", "cpucheck"]
    out = []
    for r in runs:
        d = r if os.path.isdir(r) else f"/tmp/{r}"
        res = analyse(os.path.basename(d.rstrip("/")), d)
        if res:
            out.append(res)
    print("\n=== SUMMARY (guard=%d bins) ===" % GUARD)
    print(f"{'run':12s} {'CPIs':>5s} {'vel_res':>8s} {'Ndopp':>6s} {'bin':>7s} "
          f"{'notched%':>9s} {'resolv%':>8s} {'cov%':>6s}")
    for r in out:
        print(f"{r['name']:12s} {r['n']:5d} {r['vres']:8.3f} {r['ndop']:6.0f} "
              f"{r['bin']:7.2f} {r['notched']:9.1f} {r['resolvable']:8.1f} {r['cov']:6.1f}")
