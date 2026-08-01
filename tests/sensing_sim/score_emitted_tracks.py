#!/usr/bin/env python3
"""Score EVERY emitted track update, not just the confirmed ones.

`score_world_tracks.py` filters to `status == "confirmed"`. That was fine while confirmed was the
only thing anyone looked at, but the tracker also emits COASTING updates (pure extrapolation after a
miss) and a downstream consumer receives those too — `Track::is_output()` returns true for both. On a
real capture the coasting stream is **89 % wrong and outnumbers the confirmed stream** (179 vs 155),
so a confirmed-only metric flatters any tracker that copes with misses by coasting for several CPIs
and silently ignores most of what it actually publishes.

This scorer is the consumer's view: of everything that came out of the tracker, how much was right.
Use it ALONGSIDE score_world_tracks.py, not instead of it — the confirmed-only number is still the
right question when comparing detection/association quality, and the two answer different things.

Usage: score_emitted_tracks.py <tracks.jsonl> <run_dir | ue.log> [tol_m=15]
"""
import sys
import json
import re
import collections
import numpy as np

tracks_path = sys.argv[1]
gt_src = sys.argv[2]
TOL = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

ue_log = gt_src if gt_src.endswith(".log") else f"{gt_src}/logs/ue.log"
gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
raw = collections.defaultdict(list)
with open(ue_log, errors="ignore") as f:
    for line in f:
        m = gt_re.search(line)
        if m:
            raw[int(m.group(2))].append((float(m.group(1)), float(m.group(3)), float(m.group(4))))

paths = {}
for oid, pts in raw.items():
    pts.sort()
    t = np.array([p[0] for p in pts])
    x = np.array([p[1] for p in pts])
    y = np.array([p[2] for p in pts])
    tt = np.linspace(t.min(), t.max(), 4000)
    paths[oid] = np.stack([np.interp(tt, t, x), np.interp(tt, t, y)], 1)
if not paths:
    print(f"no ground truth found in {ue_log}")
    sys.exit(1)


def nearest(x, y):
    best, who = 1e18, None
    for oid, P in paths.items():
        d = float(np.min(np.hypot(P[:, 0] - x, P[:, 1] - y)))
        if d < best:
            best, who = d, oid
    return best, who


by_status = collections.defaultdict(lambda: [0, 0])
ids = set()
dists = []
cov = collections.Counter()
for line in open(tracks_path):
    t = json.loads(line)
    d, who = nearest(t["x"], t["y"])
    ok = d <= TOL
    by_status[t["status"]][0] += 1
    by_status[t["status"]][1] += int(ok)
    ids.add(t["id"])
    dists.append(d)
    if ok:
        cov[who] += 1

n = sum(v[0] for v in by_status.values())
g = sum(v[1] for v in by_status.values())
if n == 0:
    print("no track updates emitted")
    sys.exit(0)

print(f"EMITTED updates: {n}  within {TOL}m: {g} ({100*g//n}% precision)  distinct ids: {len(ids)}")
print(f"median world err: {np.median(dists):.1f} m")
for k, (kn, kg) in sorted(by_status.items()):
    print(f"  {k:11s} {kn:5d} updates, {kg:5d} good ({100*kg/kn:.0f}%)")
if cov:
    print("  per-target: " + "  ".join(f"obj{o}={cov[o]}" for o in sorted(paths)))
