#!/usr/bin/env python3
"""Score fused tracks from tests/passive_rx against the [sensing_channel] synthetic target's known
world-frame trajectory, mirroring tests/sensing_sim's score_world_tracks.py precision metric so the
two harnesses' numbers are at least computed the same way (see PHASE3_BLIND_PDCCH_LIVE_WIRING_HANDOVER.md
for why they are still NOT directly comparable beyond that).

Ground truth is logged as "SENSING_CHANNEL gt: t=%.2fs utc_ns=%lld ... pos=(%.1f,%.1f)m ..." in a
receiver's own log. Uses utc_ns DIRECTLY (real clock_gettime(CLOCK_REALTIME) at the point of injection,
added 2026-07-28) -- NOT the earlier "t + a captured process-start epoch" approximation, which was found
live to drift up to ~100s from real time because "t" is a simulated SAMPLE-CLOCK value (TS/fs in
sensing_channel.c), not wall time, and can run faster/slower than real time under load. utc_ns is
directly comparable to a track's time_utc_ns (sensing_engine.cc's std::chrono::system_clock::now()) --
same clock source, no drift/offset to approximate.

Usage: score_passive_tracks.py <tracks.jsonl> <gt_log_file> [tol_m=15]
"""
import sys, json, re

tracks_path, gt_log_path = sys.argv[1], sys.argv[2]
TOL = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

GT_RE = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+) obj\d+ pos=\(([-\d.]+),([-\d.]+)\)")
# The merge now emits track timestamps in SIMULATED time (see merge_receivers_walltime_n.py's
# wall_to_sim: wall-clock stamps made the tracker's dt 33x too large and crashed it). So ground
# truth has to be indexed by the SAME base. utc_ns values are ~1.7e18; simulated seconds are ~1e3,
# so the two are trivially distinguishable and this auto-detects rather than needing a flag.
USE_SIM_TIME = True

gt = []  # (utc_s, x, y)
with open(gt_log_path, errors="ignore") as f:
    for line in f:
        m = GT_RE.search(line)
        if m:
            utc_ns, x, y = int(m.group(2)), float(m.group(3)), float(m.group(4))
            gt.append((utc_ns / 1e9, x, y))
gt.sort()

if not gt:
    print("score: NO ground-truth lines found in gt log -- nothing to score against", file=sys.stderr)
    sys.exit(0)


def gt_at(utc):
    # nearest-neighbour lookup (gt is sampled ~1/s; linear interp would be marginally better but the
    # target moves slowly enough that this is not worth the extra complexity here).
    best = min(gt, key=lambda g: abs(g[0] - utc))
    return best[1], best[2], abs(best[0] - utc)


n, hit = 0, 0
dists = []
max_gt_age = 0.0
with open(tracks_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        t = json.loads(line)
        if t.get("status") != "confirmed":
            continue
        n += 1
        gx, gy, age = gt_at(t["time_utc_ns"] / 1e9)
        max_gt_age = max(max_gt_age, age)
        d = ((t["x"] - gx) ** 2 + (t["y"] - gy) ** 2) ** 0.5
        dists.append(d)
        if d <= TOL:
            hit += 1

if n == 0:
    print("score: 0 confirmed track updates -- nothing to score", file=sys.stderr)
    sys.exit(0)

dists.sort()
med = dists[len(dists) // 2]
print(f"score: confirmed={n} within_{TOL:.0f}m={hit} precision={100*hit/n:.1f}% "
      f"median_dist={med:.2f}m max_gt_lookup_age={max_gt_age:.1f}s")
