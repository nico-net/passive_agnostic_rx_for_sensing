#!/usr/bin/env python3
"""World-frame scorer for fused multi-static tracks (isac-track output).

Credits a confirmed track update to a real target if it falls within TOL_M of that target's true
world (x, y) path. Time-alignment-free (searches the whole path), mirroring score_run.py.

Ground truth is parsed from a run's ue.log `SENSING_CHANNEL gt:` lines (which carry pos=(x,y) per
object), NOT hardcoded -- so this works for any scene without edits. (The previous hardcoded-mot2
version is kept as score_world_tracks_mot2.py.)

Usage: score_world_tracks.py <tracks.jsonl> <run_dir | ue.log> [tol_m=15]
"""
import sys, json, re, collections
import numpy as np

tracks_path = sys.argv[1]
gt_src = sys.argv[2]
TOL = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

ue_log = gt_src if gt_src.endswith(".log") else f"{gt_src}/logs/ue.log"
gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
raw = collections.defaultdict(list)
with open(ue_log, errors="ignore") as f:
    for line in f:
        m = gt_re.search(line)
        if m:
            raw[int(m.group(2))].append((float(m.group(1)), float(m.group(3)), float(m.group(4))))

# Densify each object's path so "distance to the path" is well approximated.
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

conf, ids = [], set()
for line in open(tracks_path):
    t = json.loads(line)
    if t["status"] != "confirmed":
        continue
    d, who = nearest(t["x"], t["y"])
    conf.append((d, who))
    ids.add(t["id"])

n = len(conf)
hit = sum(1 for d, _ in conf if d <= TOL)
print(f"confirmed updates: {n}  within {TOL}m: {hit} ({100*hit//max(n,1)}% precision)  distinct ids: {len(ids)}")
if hit:
    ds = np.array([d for d, _ in conf])
    cov = collections.Counter(w for d, w in conf if d <= TOL)
    per = "  ".join(f"obj{o}={cov[o]}" for o in sorted(paths))
    print(f"median world err: {np.median(ds):.1f} m (matched {np.median(ds[ds<=TOL]):.1f} m)  per-target: {per}")
