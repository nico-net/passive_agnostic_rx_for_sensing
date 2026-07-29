#!/usr/bin/env python3
"""Merge two SIMULTANEOUSLY-captured receiver streams using their real shared wall clock directly
(cpi_start_time_utc_ns) -- no trajectory inversion needed, unlike the sequential-run case. Each rx1
CPI is paired with its nearest-in-time rx2 CPI within PAIR_TOL_S; both get stamped with their
midpoint timestamp (quantised) so isac-track's exact-timestamp Batcher groups them into one fused CPI.

Usage: merge_receivers_walltime.py <rx1.jsonl> <rx2.jsonl> <out.jsonl> [pair_tol_s=auto]

pair_tol_s: a fixed 2.0s default silently zeroes fusion whenever receivers' independent CPI cadence
is slow (e.g. throughput-starved 100MHz runs measured ~16-18s wall-clock per CPI, where each
receiver's phase within that cycle is essentially random -- see PHASE3 comparison batch
2026-07-28/29). Left unspecified (or passed as "auto"), the tolerance is instead derived from the
data itself: half the smaller stream's own median inter-CPI gap, which is exactly the guarantee
needed to always catch the true nearest neighbour regardless of phase offset, floored at 2.0s so
fast (106 PRB) runs keep their previous behaviour unchanged.
"""
import sys, json
import numpy as np

rx1_path, rx2_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
TOL_ARG = sys.argv[4] if len(sys.argv) > 4 else "auto"
QUANT_NS = 100_000_000  # 100ms quantisation for the shared stamp

def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                d = json.loads(line); d.pop("rvm_blob", None)
                rows.append(d)
    rows.sort(key=lambda d: d["cpi_start_time_utc_ns"])
    return rows

def median_gap_ns(rows):
    if len(rows) < 2:
        return None
    ts = sorted(d["cpi_start_time_utc_ns"] for d in rows)
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    return float(np.median(gaps))

a, b = load(rx1_path), load(rx2_path)
bt = np.array([d["cpi_start_time_utc_ns"] for d in b])
print(f"rx1={len(a)} CPIs, rx2={len(b)} CPIs", file=sys.stderr)

if TOL_ARG == "auto":
    gaps = [g for g in (median_gap_ns(a), median_gap_ns(b)) if g is not None]
    PAIR_TOL_NS = max(int(2.0e9), int(0.5 * min(gaps))) if gaps else int(2.0e9)
    print(f"auto pair tolerance = {PAIR_TOL_NS/1e9:.2f}s", file=sys.stderr)
else:
    PAIR_TOL_NS = int(float(TOL_ARG) * 1e9)

merged, npair = [], 0
for d1 in a:
    t1 = d1["cpi_start_time_utc_ns"]
    if len(bt) == 0: break
    j = int(np.argmin(np.abs(bt - t1)))
    if abs(int(bt[j]) - t1) <= PAIR_TOL_NS:
        mid = (t1 + int(bt[j])) // 2
        t_ns = (mid // QUANT_NS) * QUANT_NS
        for d in (d1, b[j]):
            dd = dict(d); dd["cpi_start_time_utc_ns"] = t_ns; dd["cpi_duration_ns"] = QUANT_NS
            merged.append(dd)
        npair += 1
merged.sort(key=lambda d: d["cpi_start_time_utc_ns"])
print(f"PAIRED (nearest within {PAIR_TOL_NS/1e9:.1f}s) = {npair}", file=sys.stderr)
with open(out_path, "w") as o:
    for d in merged:
        o.write(json.dumps(d) + "\n")
print(f"wrote {len(merged)} reports to {out_path}", file=sys.stderr)
