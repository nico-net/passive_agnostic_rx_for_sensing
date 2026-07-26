#!/usr/bin/env python3
"""Track-identity continuity check, for the crossing scene.

Coverage and precision say whether targets were FOUND; they say nothing about whether the tracker kept
the same id on the same target while the two shared a range bin. This measures that directly:

For every CPI, each confirmed track is assigned to the ground-truth object whose (dR, rate) it is
nearest (within tolerance). Then, per ground-truth object, we count how many DISTINCT track ids served
it and how many times the serving id CHANGED. A clean run is 1 id per object and 0 switches; an
identity swap at the crossing shows up as a switch for both objects at the same CPI.

Usage: check_identity.py <run_dir> [range_tol_m] [vel_tol_mps]
"""
import sys, re, collections
import numpy as np

run = sys.argv[1]
RTOL = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
VTOL = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
log = f"{run}/logs/ue.log"

# ground truth curves per object, from this receiver's own log
gt = collections.defaultdict(list)
gre = re.compile(r"gt: t=([\d.]+)s obj(\d+).*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s")
tre = re.compile(r"SENSING: track CPI #(\d+) track_id=(\d+) range=([-\d.]+) m rate=([+\-\d.]+) m/s")
tracks = collections.defaultdict(list)
with open(log, errors="ignore") as f:
    for line in f:
        m = gre.search(line)
        if m:
            gt[int(m.group(2))].append((float(m.group(1)), float(m.group(3)), float(m.group(4))))
        m = tre.search(line)
        if m:
            tracks[int(m.group(1))].append((int(m.group(2)), float(m.group(3)), float(m.group(4))))

curves = {}
for o, pts in gt.items():
    pts.sort()
    t = np.array([p[0] for p in pts])
    tt = np.linspace(t.min(), t.max(), 4000)
    curves[o] = (np.interp(tt, t, [p[1] for p in pts]), np.interp(tt, t, [p[2] for p in pts]))

# per CPI: which id serves which object
serving = collections.defaultdict(dict)   # obj -> {cpi: id}
for cpi in sorted(tracks):
    for tid, rng, rate in tracks[cpi]:
        best, bo = 1e18, None
        for o, (dR, rr) in curves.items():
            d = np.min(((rng - dR) / RTOL) ** 2 + ((rate - rr) / VTOL) ** 2)
            if d < best:
                best, bo = d, o
        if best <= 2.0:                      # within ~1 combined sigma of that object's curve
            serving[bo].setdefault(cpi, tid)

print(f"=== {run} ===")
if not serving:
    print("no confirmed tracks matched any target"); sys.exit(0)
total_sw = 0
for o in sorted(serving):
    seq = [serving[o][c] for c in sorted(serving[o])]
    ids = sorted(set(seq))
    sw = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i - 1])
    total_sw += sw
    cpis = sorted(serving[o])
    print(f"  obj{o}: served in {len(seq)} CPIs by {len(ids)} distinct id(s) {ids}, {sw} id switch(es)")
    if sw:
        for i in range(1, len(seq)):
            if seq[i] != seq[i - 1]:
                print(f"        switch at CPI {cpis[i]}: id {seq[i-1]} -> {seq[i]}")
# a genuine SWAP is two objects switching at the same CPI, exchanging ids
swaps = []
objs = sorted(serving)
for a in range(len(objs)):
    for b in range(a + 1, len(objs)):
        oa, ob = objs[a], objs[b]
        common = sorted(set(serving[oa]) & set(serving[ob]))
        for i in range(1, len(common)):
            p, c = common[i - 1], common[i]
            if serving[oa][p] == serving[ob][c] and serving[ob][p] == serving[oa][c]:
                swaps.append((c, oa, ob))
print(f"  TOTAL id switches: {total_sw}   IDENTITY SWAPS (ids exchanged between two objects): {len(swaps)}")
for c, oa, ob in swaps:
    print(f"        swap at CPI {c} between obj{oa} and obj{ob}")
