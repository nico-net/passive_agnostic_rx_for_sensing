#!/usr/bin/env python3
"""N-receiver generalisation of merge_receivers_walltime.py (real wall-clock pairing via
cpi_start_time_utc_ns, no scene/ground-truth coupling). Written for tests/passive_rx because chaining
merge_receivers_walltime.py pairwise (merge(rx1,rx2) -> merge(that,rx3)) is lossy for N>2: each step
only keeps records that ALREADY paired in the previous step, so a receiver with imperfect pairwise
overlap against rx1 gets dropped even if it overlaps fine with rx2 or rx3 individually. This version
anchors on receiver 0's CPI times and, independently for every OTHER receiver, looks for its own
nearest-in-time CPI -- a group is emitted if AT LEAST MIN_RX receivers (including the anchor) have a
CPI within PAIR_TOL_S, not only when every receiver matches.

Usage: merge_receivers_walltime_n.py <out.jsonl> <min_rx> <pair_tol_s|auto> <rx1.jsonl> <rx2.jsonl> [...]

pair_tol_s: pass "auto" to derive the tolerance from the data instead of a fixed value -- half the
smallest stream's own median inter-CPI gap (floored at 2.0s), same rationale/fix as
tests/sensing_sim/merge_receivers_walltime.py -- a fixed 2.0s silently drops all pairing once
receivers run slow enough (~16-18s/CPI, seen at 100MHz) that phase offset between independently
started receivers exceeds it.
"""
import sys, json
import numpy as np

out_path = sys.argv[1]
MIN_RX = int(sys.argv[2])
TOL_ARG = sys.argv[3]
QUANT_NS = 100_000_000
# Each spec is "reports.jsonl" or "reports.jsonl@ue.log"; the log enables the wall->sim remap.
specs = sys.argv[4:]
rx_paths, rx_logs = [], []
for sp in specs:
    if "@" in sp:
        a, b = sp.rsplit("@", 1)
        rx_paths.append(a); rx_logs.append(b)
    else:
        rx_paths.append(sp); rx_logs.append(None)


def wall_to_sim(log_path):
    """Build a wall(utc_ns) -> simulated(seconds) mapping from a receiver's own gt lines.

    REQUIRED whenever simulated time does not track wall time. sensing_engine.cc stamps each report
    with CLOCK_REALTIME, but the SCENE evolves in simulated time, and on this harness simulated time
    advances at only ~3-7% of real time. A 6 h capture therefore spans 5.97 h of wall clock while the
    targets move just 646 s worth -- so a tracker fed the wall stamps sees dt ~86 s between updates
    for ~2.6 s of real target motion, a 33x error. Its process noise integrated over that bogus dt
    grows the covariance until it is no longer positive-definite and isac-track PANICS
    (ukf.rs:87) -- which is exactly what produced 0 fused tracks from 251 cleanly paired CPIs.
    tests/sensing_sim's merge_receivers_n.py never hit this because it stamps in simulated time by
    inverting the trajectory; the wall-clock merge here was only ever safe when sim ~= wall (106 PRB).
    """
    import re as _re
    rx = _re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+)")
    w, sim = [], []
    with open(log_path, errors="ignore") as f:
        for line in f:
            m = rx.search(line)
            if m:
                sim.append(float(m.group(1))); w.append(int(m.group(2)))
    if len(w) < 2:
        return None
    order = np.argsort(w)
    return np.array(w)[order], np.array(sim)[order]


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                d = json.loads(line)
                d.pop("rvm_blob", None)
                rows.append(d)
    rows.sort(key=lambda d: d["cpi_start_time_utc_ns"])
    return rows


def median_gap_ns(rows):
    if len(rows) < 2:
        return None
    ts = sorted(d["cpi_start_time_utc_ns"] for d in rows)
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    return float(np.median(gaps))


rx = [load(p) for p in rx_paths]
# Remap every report's timestamp into simulated time before any pairing, so the tolerance and the
# stamps handed to isac-track are both in the time base the SCENE actually evolves in.
for i, lg in enumerate(rx_logs):
    if not lg:
        continue
    mp = wall_to_sim(lg)
    if mp is None:
        print(f"rx{i+1}: no gt lines in {lg}; leaving wall-clock stamps", file=sys.stderr)
        continue
    wv, sv = mp
    for d in rx[i]:
        d["cpi_start_time_utc_ns"] = int(np.interp(d["cpi_start_time_utc_ns"], wv, sv) * 1e9)
    print(f"rx{i+1}: remapped wall->sim ({(wv[-1]-wv[0])/1e9/3600:.2f} h wall -> {sv[-1]-sv[0]:.0f} s sim)",
          file=sys.stderr)
for i, r in enumerate(rx):
    print(f"rx{i+1}={len(r)} CPIs", file=sys.stderr)

if TOL_ARG == "auto":
    gaps = [g for g in (median_gap_ns(r) for r in rx) if g is not None]
    PAIR_TOL_NS = max(int(2.0e9), int(0.5 * min(gaps))) if gaps else int(2.0e9)
    print(f"auto pair tolerance = {PAIR_TOL_NS/1e9:.2f}s", file=sys.stderr)
else:
    PAIR_TOL_NS = int(float(TOL_ARG) * 1e9)

rx_t = [np.array([d["cpi_start_time_utc_ns"] for d in r]) if r else np.array([]) for r in rx]

merged = []
ngroups = 0
for d0 in rx[0]:
    t0 = d0["cpi_start_time_utc_ns"]
    group = [d0]
    group_times = [t0]
    for i in range(1, len(rx)):
        if len(rx_t[i]) == 0:
            continue
        j = int(np.argmin(np.abs(rx_t[i] - t0)))
        if abs(int(rx_t[i][j]) - t0) <= PAIR_TOL_NS:
            group.append(rx[i][j])
            group_times.append(int(rx_t[i][j]))
    if len(group) >= MIN_RX:
        mid = sum(group_times) // len(group_times)
        t_ns = (mid // QUANT_NS) * QUANT_NS
        for d in group:
            dd = dict(d)
            dd["cpi_start_time_utc_ns"] = t_ns
            dd["cpi_duration_ns"] = QUANT_NS
            merged.append(dd)
        ngroups += 1

merged.sort(key=lambda d: d["cpi_start_time_utc_ns"])
print(f"GROUPS (>= {MIN_RX} of {len(rx)} receivers, within {PAIR_TOL_NS/1e9:.1f}s) = {ngroups}", file=sys.stderr)
with open(out_path, "w") as o:
    for d in merged:
        o.write(json.dumps(d) + "\n")
print(f"wrote {len(merged)} reports to {out_path}", file=sys.stderr)
