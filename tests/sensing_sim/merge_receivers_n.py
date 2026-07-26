#!/usr/bin/env python3
"""N-receiver multi-static merge (generalises merge_receivers_simtime.py from 2 to any number).

Each CPI is stamped with its TRUE simulation time by inverting the known target trajectories against
that receiver's own geometry, so receivers with different CPI cadences still line up. The FIRST
receiver is the time reference; every other receiver contributes its nearest-in-sim-time CPI when
within PAIR_TOL_S. All reports of a group get one identical quantised timestamp so isac-track's
exact-timestamp Batcher fuses them into a single CPI.

Why N matters: with only 2 Tx-Rx pairs, birth is exactly determined (2 equations, 2 unknowns for
position and again for velocity), so ANY set of detections yields a zero-residual "fix" and no
consistency test is possible -- `birth_max_rate_rms_mps` is inert. A 3rd pair over-determines the
system, so a wrong cross-receiver pairing produces a large residual and can be rejected.

Usage: merge_receivers_n.py <out.jsonl> <pair_tol_ms> <rx1.jsonl@rx1_ue.log> <rx2.jsonl@rx2_ue.log> [...]
"""
import sys, json, re
import numpy as np

out_path = sys.argv[1]
PAIR_TOL_S = float(sys.argv[2]) / 1000.0
SPECS = sys.argv[3:]
QUANT_S = 50e-3
RTOL, VTOL = 15.0, 3.0
TGRID = np.linspace(0.0, 20.0, 8001)
TX = np.array([0.0, 0.0])

def gt_curves_from_log(ue_log):
    """Ground-truth (dR, range_rate) curves per object, parsed from the receiver's OWN ue.log.

    Scene-agnostic by construction: sensing_channel.c already logs, once per simulated second and per
    object, that receiver's true differential range and range-rate. Deriving the curves from the log
    instead of re-deriving them from hardcoded trajectory formulas means this script never silently
    disagrees with the scene actually simulated -- the failure mode when the formulas were inlined.
    """
    import collections
    gt = collections.defaultdict(list)
    rx = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s obj(\d+).*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s")
    with open(ue_log, errors="ignore") as f:
        for line in f:
            m = rx.search(line)
            if m:
                gt[int(m.group(2))].append((float(m.group(1)), float(m.group(3)), float(m.group(4))))
    out = {}
    for o, pts in gt.items():
        pts.sort()
        t = np.array([p[0] for p in pts])
        if len(t) < 2:
            continue
        tt = np.linspace(t.min(), t.max(), 4000)
        out[o] = (tt,
                  np.interp(tt, t, [p[1] for p in pts]),
                  np.interp(tt, t, [p[2] for p in pts]))
    return out

def stamp(path, curves):
    res = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line); d.pop("rvm_blob", None)
            bt, bc = None, 1e18
            for det in d.get('detections', []):
                dr, v = det['bistatic_range_m'], det['bistatic_velocity_mps']
                for o, (tt, dR, rr) in curves.items():
                    cost = ((dr - dR) / RTOL) ** 2 + ((v - rr) / VTOL) ** 2
                    j = int(np.argmin(cost))
                    if cost[j] < bc:
                        bc, bt = cost[j], float(tt[j])
            if bt is not None and bc <= 2.0:
                res.append((bt, d))
    res.sort(key=lambda x: x[0])
    return res

streams = []
for spec in SPECS:
    rep_path, log_path = spec.rsplit("@", 1)
    cv = gt_curves_from_log(log_path)
    st = stamp(rep_path, cv)
    streams.append(st)
    print(f"{rep_path} objs={len(cv)} stamped={len(st)}", file=sys.stderr)

ref = streams[0]
others = [(np.array([t for t, _ in s]), s) for s in streams[1:]]
merged, groups, full = [], 0, 0
for t0, d0 in ref:
    group = [d0]
    for bt, s in others:
        if len(bt) == 0:
            continue
        j = int(np.argmin(np.abs(bt - t0)))
        if abs(bt[j] - t0) <= PAIR_TOL_S:
            group.append(s[j][1])
    if len(group) < 2:
        continue
    groups += 1
    if len(group) == len(streams):
        full += 1
    t_ns = int(round(t0 / QUANT_S) * QUANT_S * 1e9)
    for d in group:
        dd = dict(d); dd["cpi_start_time_utc_ns"] = t_ns; dd["cpi_duration_ns"] = int(QUANT_S*1e9)
        merged.append(dd)
merged.sort(key=lambda d: d["cpi_start_time_utc_ns"])
print(f"fused CPIs = {groups} (with ALL {len(streams)} receivers: {full})", file=sys.stderr)
with open(out_path, "w") as o:
    for d in merged:
        o.write(json.dumps(d) + "\n")
print(f"wrote {len(merged)} reports to {out_path}", file=sys.stderr)
