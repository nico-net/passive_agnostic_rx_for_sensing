#!/usr/bin/env python3
"""Cross-receiver RNTI consistency gate for full-passive fusion.

WHY: the blind-PDCCH source's noise floor is a receiver-LOCAL problem -- a false CRC-recovered RNTI
is a decode error specific to that one receiver's own noisy candidate, so it will not recur on a
SECOND, INDEPENDENT receiver at the same wall-clock time. A real active UE's RNTI, by contrast, is
legitimately accepted by every receiver that can hear it (same air, same grants). This is a much
stronger noise-floor test than any single receiver's own RNTI-persistence gate (nr_pdcch_blind_monitor
already does that, per-receiver) can ever be, because it doesn't depend on tuning any threshold at
all -- it just asks "did anyone else see this."

INPUT: each receiver's own report JSONL (rows carry cpi_start_time_utc_ns/cpi_duration_ns, see
sensing_engine.cc) and its own ue log, which now carries a `SENSING: blind PDCCH rnti_seen
utc_ns=<ns> rnti=0x<hex>` line per accepted candidate (nr_pdcch_blind_monitor_rt.c, added alongside
this gate -- deliberately separate from the pre-existing LOG_D debug line, LOG_I so it survives the
harness's normal "info" log level, and a fixed schema meant to be grepped by tooling like this).

METHOD: for each report row (one CPI), collect that receiver's OWN rnti_seen sightings whose utc_ns
falls inside [cpi_start, cpi_start+duration] -- the RNTIs that "contributed" to that CPI's blind-PDCCH
CFR submissions. Keep the row only if at least one of those RNTIs was ALSO sighted by a DIFFERENT
receiver within +/-RNTI_TOL_S of the row's own window. csi_rs-sourced rows (no RNTI involved at all)
always pass through untouched -- this gate only targets the blind-PDCCH noise floor.

Usage: rnti_gate.py <out_dir> <rx1_reports.jsonl>:<rx1_ue.log> [<rx2_reports.jsonl>:<rx2_ue.log> ...]
Writes <out_dir>/<basename-of-reports>.gated.jsonl per receiver; prints kept/dropped counts to stderr.
"""
import sys, os, re, json
import numpy as np

RNTI_TOL_S = 2.0
RNTI_RE = re.compile(r"SENSING: blind PDCCH rnti_seen utc_ns=(\d+) rnti=(0x[0-9a-fA-F]+)")

out_dir = sys.argv[1]
specs = sys.argv[2:]
os.makedirs(out_dir, exist_ok=True)


def load_sightings(log_path):
    """[(utc_ns, rnti)], sorted by utc_ns."""
    out = []
    with open(log_path, errors="ignore") as f:
        for line in f:
            m = RNTI_RE.search(line)
            if m:
                out.append((int(m.group(1)), int(m.group(2), 16)))
    out.sort()
    return out


def load_reports(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


receivers = []
for spec in specs:
    rep_path, log_path = spec.rsplit(":", 1)
    receivers.append({"rep_path": rep_path, "sightings": load_sightings(log_path), "reports": load_reports(rep_path)})

for i, r in enumerate(receivers):
    print(f"rx{i+1}: {len(r['reports'])} reports, {len(r['sightings'])} rnti_seen sightings", file=sys.stderr)

TOL_NS = int(RNTI_TOL_S * 1e9)

for i, r in enumerate(receivers):
    others_t = []
    others_rnti = []
    for j, o in enumerate(receivers):
        if j == i:
            continue
        for t, rnti in o["sightings"]:
            others_t.append(t)
            others_rnti.append(rnti)
    others_t = np.array(others_t, dtype=np.int64)
    others_rnti = np.array(others_rnti, dtype=np.int64)

    own_t = np.array([t for t, _ in r["sightings"]], dtype=np.int64)
    own_rnti = np.array([rnti for _, rnti in r["sightings"]], dtype=np.int64)

    kept, dropped, passthrough = 0, 0, 0
    out_rows = []
    for row in r["reports"]:
        # csi_rs-only rows (no blind-PDCCH contribution at all) aren't what this gate is for --
        # pass them through. A row is "blind-PDCCH-involved" if any of its own sightings fall in
        # its window; if none do, there was nothing for this gate to check, so let it through too
        # (dropping it would be double-counting the persistence/SNR gates' own job).
        t0 = row["cpi_start_time_utc_ns"]
        t1 = t0 + row.get("cpi_duration_ns", 0)
        if own_t.size:
            mask = (own_t >= t0) & (own_t <= t1)
            contributing = set(own_rnti[mask].tolist())
        else:
            contributing = set()
        if not contributing:
            out_rows.append(row)
            passthrough += 1
            continue
        confirmed = False
        if others_t.size:
            win_mask = (others_t >= t0 - TOL_NS) & (others_t <= t1 + TOL_NS)
            seen_elsewhere = set(others_rnti[win_mask].tolist())
            confirmed = bool(contributing & seen_elsewhere)
        if confirmed:
            out_rows.append(row)
            kept += 1
        else:
            dropped += 1

    base = os.path.splitext(os.path.basename(r["rep_path"]))[0]
    out_path = os.path.join(out_dir, f"{base}.gated.jsonl")
    with open(out_path, "w") as f:
        for row in out_rows:
            f.write(json.dumps(row) + "\n")
    print(f"rx{i+1}: kept={kept} dropped={dropped} passthrough={passthrough} -> {out_path}", file=sys.stderr)
    r["gated_path"] = out_path

print(" ".join(r["gated_path"] for r in receivers))
