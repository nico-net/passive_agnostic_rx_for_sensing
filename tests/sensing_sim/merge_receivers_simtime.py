#!/usr/bin/env python3
"""Tight multi-static merge via ground-truth sim-time stamping + nearest-neighbour pairing.

Each CPI is stamped with its TRUE simulation time by inverting the known target trajectory against the
receiver's own geometry (so rx1 and rx2 CPIs at the same trajectory instant get the same sim-time,
independent of cadence/attach). Then each rx1 CPI is paired with its NEAREST-in-sim-time rx2 CPI when
within PAIR_TOL_S (nearest-neighbour, not exact-bin, so we don't discard CPIs that merely miss a bin
edge). Both reports of a pair get an identical quantised timestamp so isac-track's exact-timestamp
Batcher groups them into one fused CPI.

Scene (mot2, constant-velocity): objA(t)=(35+4t,55+9t), objB(t)=(70-3t,120+8t). Edit if scene changes.
Usage: merge_receivers_simtime.py <rx1.jsonl> <rx2.jsonl> <out.jsonl> [pair_tol_ms=150]
"""
import sys, json, numpy as np

rx1_path, rx2_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
PAIR_TOL_S = (float(sys.argv[4]) if len(sys.argv) > 4 else 150.0) / 1000.0
QUANT_S = 50e-3  # timestamp quantisation (< pair tol) so paired reports share an exact stamp
RTOL, VTOL = 15.0, 3.0
TGRID = np.linspace(0.0, 20.0, 8001)  # 2.5 ms trajectory resolution

def obj(name, t):
    # fast-target scene: A slow 7.0 m/s @15deg from (100,40); C fast 15.0 m/s @250deg from (180,-130)
    import math
    if name == 'A':
        return (100 + 7.0*math.cos(math.radians(15))*t, 40 + 7.0*math.sin(math.radians(15))*t)
    return (180 + 15.0*math.cos(math.radians(250))*t, -130 + 15.0*math.sin(math.radians(250))*t)

def curves(rx):
    tx = np.array([0.0, 0.0]); rxp = np.array(rx); r_los = np.hypot(*(tx - rxp))
    out = {}
    for g in ('A', 'B'):
        ox, oy = obj(g, TGRID)
        dR = np.hypot(tx[0]-ox, tx[1]-oy) + np.hypot(ox-rxp[0], oy-rxp[1]) - r_los
        out[g] = (dR, np.gradient(dR, TGRID))
    return out

def stamp(path, rx):
    """Return list of (sim_time_s, report) for CPIs whose best detection matches a real target."""
    cv = curves(rx); res = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            d = json.loads(line); d.pop("rvm_blob", None)
            best_t, best_cost = None, 1e18
            for det in d.get('detections', []):
                dr, v = det['bistatic_range_m'], det['bistatic_velocity_mps']
                for g in ('A', 'B'):
                    dRc, rc = cv[g]
                    cost = ((dr - dRc)/RTOL)**2 + ((v - rc)/VTOL)**2
                    j = int(np.argmin(cost))
                    if cost[j] < best_cost:
                        best_cost, best_t = cost[j], float(TGRID[j])
            if best_t is not None and best_cost <= 2.0:
                res.append((best_t, d))
    res.sort(key=lambda x: x[0])
    return res

a = stamp(rx1_path, [100.0, 0.0])
b = stamp(rx2_path, [0.0, 200.0])
bt = np.array([t for t, _ in b])
print(f"rx1 stamped={len(a)}, rx2 stamped={len(b)}", file=sys.stderr)

merged, npair = [], 0
used_b = set()
for t1, d1 in a:
    if len(bt) == 0: break
    j = int(np.argmin(np.abs(bt - t1)))
    if abs(bt[j] - t1) <= PAIR_TOL_S:
        t_ns = int(round(((t1 + bt[j]) / 2) / QUANT_S) * QUANT_S * 1e9)
        for d in (d1, b[j][1]):
            dd = dict(d); dd["cpi_start_time_utc_ns"] = t_ns; dd["cpi_duration_ns"] = int(QUANT_S*1e9)
            merged.append(dd)
        npair += 1
merged.sort(key=lambda d: d["cpi_start_time_utc_ns"])
print(f"PAIRED (nearest within {PAIR_TOL_S*1000:.0f}ms) = {npair}", file=sys.stderr)
with open(out_path, "w") as o:
    for d in merged:
        o.write(json.dumps(d) + "\n")
print(f"wrote {len(merged)} reports to {out_path}", file=sys.stderr)
