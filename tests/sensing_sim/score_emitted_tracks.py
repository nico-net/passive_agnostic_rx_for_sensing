#!/usr/bin/env python3
r"""Score EVERY emitted track update, not just the confirmed ones.

`score_world_tracks.py` filters to `status == "confirmed"`. That was fine while confirmed was the
only thing anyone looked at, but the tracker also emits COASTING updates (pure extrapolation after a
miss) and a downstream consumer receives those too — `Track::is_output()` returns true for both. On a
real capture the coasting stream is **89 % wrong and outnumbers the confirmed stream** (179 vs 155),
so a confirmed-only metric flatters any tracker that copes with misses by coasting for several CPIs
and silently ignores most of what it actually publishes.

This scorer is the consumer's view: of everything that came out of the tracker, how much was right.
Use it ALONGSIDE score_world_tracks.py, not instead of it — the confirmed-only number is still the
right question when comparing detection/association quality, and the two answer different things.

BUG FOUND AND FIXED 2026-08-03 (research iteration 008 / implementation iteration 006): `nearest()`
used to interpolate each object's ENTIRE trajectory (the whole capture, via
`np.linspace(t.min(), t.max(), 4000)`) into a dense path and match a track's (x,y) against the
CLOSEST POINT ON THAT WHOLE PATH, with no time filtering at all. On this project's patrolling/
oscillating scenes (waypoints that revisit similar regions repeatedly), that scores a track "good"
if it is near anywhere the object EVER visited during the run, not near where it actually was at
the track's own timestamp -- inflating precision for every status category. Manually verified wrong
on a real run (SB_rep2, 2026-08-03): hand-computed nearest-GT-IN-TIME distances for 6 confirmed
updates gave 1/6 within 15 m; the old `nearest()` claimed 5/6. Fixed by looking up the GT position at the TRACK'S OWN timestamp instead of matching position
alone. First attempt (per-object nearest-in-time, then pick whichever object is spatially closest)
did NOT reproduce the SB_rep2 cross-check (gave 4/6, not 1/6) and was itself wrong to trust just
because it "sounded more correct" -- `score_passive_tracks.py`'s GT_RE does not capture the object
id at all (`obj\d+`, no group), so its `gt_at()` finds the nearest-in-time SAMPLE from ALL objects'
GT lines pooled together, whichever object that sample happens to belong to, not the nearest
position of each object independently. That is the convention this file now mirrors exactly (object
id kept alongside each pooled point only so the "per-target" coverage line can still be printed --
it plays no part in selecting which point is nearest). Both track `time_utc_ns` and GT `utc_ns` are
the same real wall-clock base (`std::chrono::system_clock::now()` / `clock_gettime(CLOCK_REALTIME)`),
compared directly, no unit conversion. Re-validated against the SB_rep2 cross-check before trusting
this version: now agrees with `score_passive_tracks.py`'s 1/6.

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
# utc_ns is REQUIRED now (time-aware matching, see the bug-fix note above), so this only accepts
# logs new enough to carry it -- every capture this project has used since 2026-07-28 does.
gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
# One POOLED list across all objects, object id carried along only for the "per-target" report --
# NOT used to pick a per-object position. See the bug-fix note above for why.
gt_pool = []
oids_seen = set()
with open(ue_log, errors="ignore") as f:
    for line in f:
        m = gt_re.search(line)
        if m:
            oid = int(m.group(3))
            oids_seen.add(oid)
            gt_pool.append((int(m.group(2)) / 1e9, float(m.group(4)), float(m.group(5)), oid))
gt_pool.sort()
paths = {o: None for o in oids_seen}  # kept only so downstream "for o in sorted(paths)" still works
if not gt_pool:
    print(f"no ground truth found in {ue_log}")
    sys.exit(1)


def nearest(x, y, t_ns):
    """Nearest-neighbour GT lookup, POOLED across objects (mirrors score_passive_tracks.py's
    gt_at() exactly: nearest-in-time SAMPLE from the combined pool, whichever object it happens to
    be, then spatial distance to THAT single sample -- not per-object interpolation)."""
    utc_s = t_ns / 1e9
    bt, bx, by, boid = min(gt_pool, key=lambda p: abs(p[0] - utc_s))
    d = ((x - bx) ** 2 + (y - by) ** 2) ** 0.5
    return d, boid


by_status = collections.defaultdict(lambda: [0, 0])
ids = set()
dists = []
cov = collections.Counter()
for line in open(tracks_path):
    t = json.loads(line)
    d, who = nearest(t["x"], t["y"], t["time_utc_ns"])
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
