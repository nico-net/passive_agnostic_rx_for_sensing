#!/usr/bin/env python3
"""Merge two SIMULTANEOUSLY-captured receiver streams using their real shared wall clock directly
(cpi_start_time_utc_ns) -- no trajectory inversion needed, unlike the sequential-run case. Each rx1
CPI is paired with its nearest-in-time rx2 CPI within PAIR_TOL_S; both get stamped with their
midpoint timestamp (quantised) so isac-track's exact-timestamp Batcher groups them into one fused CPI.

Usage: merge_receivers_walltime.py <rx1.jsonl> <rx2.jsonl> <out.jsonl> [pair_tol_s=2.0]
"""
import sys, json
import numpy as np

rx1_path, rx2_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
PAIR_TOL_NS = int((float(sys.argv[4]) if len(sys.argv) > 4 else 2.0) * 1e9)
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

a, b = load(rx1_path), load(rx2_path)
bt = np.array([d["cpi_start_time_utc_ns"] for d in b])
print(f"rx1={len(a)} CPIs, rx2={len(b)} CPIs", file=sys.stderr)

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
