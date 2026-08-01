#!/usr/bin/env python3
"""Score one tracker arm's output both ways and emit two CSV rows.

Both metrics come from the same labelling so they cannot drift apart:
  * emitted   -- every update the tracker published (what a consumer receives)
  * confirmed -- status=="confirmed" only (the project's historical metric)

Usage: score_arm_csv.py <tracks.jsonl> <run_dir> <capture_name> <arm_name> [tol_m=15]
"""
import sys
import json
import re
import collections
import numpy as np

tracks_path, run_dir, cap, arm = sys.argv[1:5]
TOL = float(sys.argv[5]) if len(sys.argv) > 5 else 15.0

gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
raw = collections.defaultdict(list)
try:
    with open(f"{run_dir}/logs/ue.log", errors="ignore") as f:
        for line in f:
            m = gt_re.search(line)
            if m:
                raw[int(m.group(2))].append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4))))
except OSError:
    pass
paths = {}
for oid, pts in raw.items():
    pts.sort()
    t = np.array([p[0] for p in pts])
    x = np.array([p[1] for p in pts])
    y = np.array([p[2] for p in pts])
    tt = np.linspace(t.min(), t.max(), 4000)
    paths[oid] = np.stack([np.interp(tt, t, x), np.interp(tt, t, y)], 1)


def nearest(x, y):
    if not paths:
        return 1e18
    return min(float(np.min(np.hypot(P[:, 0] - x, P[:, 1] - y))) for P in paths.values())


rows = {"emitted": [], "confirmed": []}
try:
    for line in open(tracks_path):
        t = json.loads(line)
        d = nearest(t["x"], t["y"])
        rows["emitted"].append((t["id"], d))
        if t["status"] == "confirmed":
            rows["confirmed"].append((t["id"], d))
except OSError:
    pass

for metric, rs in rows.items():
    n = len(rs)
    if n == 0:
        print(f"{cap},{arm},{metric},0,0,0,,0")
        continue
    good = sum(1 for _, d in rs if d <= TOL)
    med = float(np.median([d for _, d in rs]))
    ids = len({i for i, _ in rs})
    print(f"{cap},{arm},{metric},{n},{good},{100.0*good/n:.1f},{med:.2f},{ids}")
