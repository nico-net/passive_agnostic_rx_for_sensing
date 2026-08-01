#!/usr/bin/env python3
"""Does a track's mean detection confidence (`det_confidence`) tell real tracks from ghosts?

This is the measurement that decides whether propagating the receiver's per-detection `p_real` to the
tracker was worth doing. It is deliberately run BEFORE any gate is enabled, so the question is asked
of the raw statistic rather than of a threshold already chosen with hindsight.

Method, mirroring analyze_separability.py:
  * label every confirmed track update real / ghost by distance to the nearest ground-truth path
    (time-alignment-free, the same convention score_world_tracks.py uses);
  * label a TRACK by the majority of its updates -- the unit a confirmation gate actually acts on;
  * report ROC AUC (Mann-Whitney U) of det_confidence for real-vs-ghost at BOTH levels. AUC 0.5 means
    the statistic carries no information and the gate should not be enabled;
  * sweep the gate value and print the precision/coverage it would have produced, so the cost of any
    given operating point is visible rather than assumed.

Usage: score_track_confidence.py <tracks.jsonl> <run_dir | ue.log> [tol_m=15]
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
    return min(float(np.min(np.hypot(P[:, 0] - x, P[:, 1] - y))) for P in paths.values())


updates = []  # (track_id, is_real, det_confidence)
missing = 0
for line in open(tracks_path):
    t = json.loads(line)
    if t["status"] != "confirmed":
        continue
    c = t.get("det_confidence")
    if c is None:
        missing += 1
        continue
    updates.append((t["id"], nearest(t["x"], t["y"]) <= TOL, float(c)))

if missing:
    print(f"WARNING: {missing} confirmed updates carry no det_confidence "
          f"(receiver did not emit p_real, or the gate is off) -- excluded")
if not updates:
    print("no scored updates: nothing to measure")
    sys.exit(1)


def auc(pos, neg):
    """ROC AUC via Mann-Whitney U, ties counted as half."""
    if not len(pos) or not len(neg):
        return float("nan")
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty(len(allv))
    ranks[order] = np.arange(1, len(allv) + 1)
    # average ranks over ties so a constant statistic scores exactly 0.5, not 1.0
    _, inv, cnt = np.unique(allv, return_inverse=True, return_counts=True)
    sums = np.bincount(inv, weights=ranks)
    ranks = (sums / cnt)[inv]
    u = ranks[:len(pos)].sum() - len(pos) * (len(pos) + 1) / 2.0
    return u / (len(pos) * len(neg))


# --- update level -------------------------------------------------------------------------------
cu = np.array([c for _, _, c in updates])
ru = np.array([r for _, r, _ in updates])
print(f"updates: {len(updates)}  real {int(ru.sum())}  ghost {int((~ru).sum())}")
print(f"  det_confidence  real: mean {cu[ru].mean():.4f}  ghost: mean {cu[~ru].mean():.4f}"
      if ru.any() and (~ru).any() else "  (one class only)")
print(f"  AUC (update level): {auc(cu[ru], cu[~ru]):.3f}")

# --- track level: the unit a confirmation gate acts on -------------------------------------------
per = collections.defaultdict(list)
for tid, r, c in updates:
    per[tid].append((r, c))
t_real, t_conf, t_n = [], [], []
for tid, v in per.items():
    t_real.append(sum(1 for r, _ in v if r) * 2 > len(v))  # majority real
    t_conf.append(float(np.mean([c for _, c in v])))
    t_n.append(len(v))
t_real = np.array(t_real)
t_conf = np.array(t_conf)
t_n = np.array(t_n)
print(f"tracks: {len(t_real)}  mostly-real {int(t_real.sum())}  mostly-ghost {int((~t_real).sum())}")
if t_real.any() and (~t_real).any():
    print(f"  det_confidence  real: mean {t_conf[t_real].mean():.4f}  "
          f"ghost: mean {t_conf[~t_real].mean():.4f}")
    print(f"  AUC (track level):  {auc(t_conf[t_real], t_conf[~t_real]):.3f}")
    print(f"  lifetimes (updates) real median {np.median(t_n[t_real]):.0f}  "
          f"ghost median {np.median(t_n[~t_real]):.0f}")

# --- what a gate would have cost -----------------------------------------------------------------
# Approximate: a gate applied at confirmation suppresses a track entirely, so drop every update of a
# track whose mean confidence is below the value. This is an upper bound on the benefit (the real
# gate sees a partial mean at confirmation time, not the final one) and is labelled as such.
print("\ngate sweep (approximate: whole-track suppression by FINAL mean confidence)")
print("  thresh   updates  precision  tracks_kept")
base_n = len(updates)
for th in [0.0, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95]:
    keep = [(r, c) for tid, r, c in updates if np.mean([x for _, x in per[tid]]) >= th]
    if not keep:
        print(f"  {th:5.2f}   {0:7d}       ----   {0:5d}")
        continue
    prec = 100.0 * sum(1 for r, _ in keep if r) / len(keep)
    ntr = sum(1 for tid in per if np.mean([x for _, x in per[tid]]) >= th)
    print(f"  {th:5.2f}   {len(keep):7d}   {prec:8.1f}%   {ntr:5d}"
          + ("   <- current (gate off)" if th == 0.0 else ""))
print(f"(baseline {base_n} updates)")
