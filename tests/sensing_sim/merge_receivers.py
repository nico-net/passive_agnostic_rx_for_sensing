#!/usr/bin/env python3
"""Merge two single-receiver DetectionReport streams into one multi-static replay file.

isac-track's `replay` batches by EXACT cpi_start_time_utc_ns, so the two receivers' reports for the
same CPI must carry IDENTICAL timestamps. The two sim runs replay the SAME deterministic trajectory
over a comparable wall span but at different CPI cadences (different #CPIs), so index-pairing would
drift. Instead align by TRAJECTORY-PROGRESS FRACTION: re-base each run to its first report, normalise
to f=(t-t0)/span in [0,1] (both traverse the identical trajectory from f=0..1), bin f into N bins, and
stamp each bin with a common synthetic sim-clock (bin * DT_NS). A bin holding one report from each
receiver becomes a fused 2-pair CPI.

Usage: merge_receivers.py <rx1.jsonl> <rx2.jsonl> <out.jsonl> [nbins=50] [sim_span_s=8.6]
"""
import sys, json

rx1_path, rx2_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
NBINS = int(sys.argv[4]) if len(sys.argv) > 4 else 50
SIM_SPAN_S = float(sys.argv[5]) if len(sys.argv) > 5 else 8.6
DT_NS = int(SIM_SPAN_S / NBINS * 1e9)

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

def binned(rows):
    """bin index -> first report in that trajectory-progress bin."""
    t0 = rows[0]["cpi_start_time_utc_ns"]
    span = rows[-1]["cpi_start_time_utc_ns"] - t0
    out = {}
    for d in rows:
        f = (d["cpi_start_time_utc_ns"] - t0) / span if span else 0.0
        b = min(NBINS - 1, int(f * NBINS))
        out.setdefault(b, d)  # keep the first report per bin
    return out

a, b = binned(load(rx1_path)), binned(load(rx2_path))
paired = sorted(set(a) & set(b))
print(f"rx1 bins={len(a)}, rx2 bins={len(b)}, PAIRED (both present)={len(paired)}", file=sys.stderr)

merged = []
for bin_idx in paired:
    t = bin_idx * DT_NS
    for d in (a[bin_idx], b[bin_idx]):
        d["cpi_start_time_utc_ns"] = t
        d["cpi_duration_ns"] = DT_NS
        merged.append(d)

with open(out_path, "w") as o:
    for d in merged:
        o.write(json.dumps(d) + "\n")
print(f"wrote {len(merged)} reports ({len(paired)} fused CPIs x 2 rx) to {out_path}", file=sys.stderr)
