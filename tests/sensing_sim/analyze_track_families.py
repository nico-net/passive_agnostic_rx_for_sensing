#!/usr/bin/env python3
"""Are "ghost" tracks alternative LOCALISATIONS of a real target, rather than invented targets?

Motivation (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md §7.21-§7.22). A bistatic range measurement is
degenerate: an entire ellipse of world positions produces the same ΔR. Measured consequence — 69 % of
confirmed ghost track UPDATES sit on a real target's ellipse while being >15 m away in the world, and
ghost tracks receive detections no rival track can explain at the SAME rate as real tracks (24 % vs
22 %), so they are not redundant re-explanations of stolen measurements.

That suggests the tracker is over-reporting alternative solutions of one under-determined inverse
problem rather than inventing objects. If so, the fix is a REPRESENTATION change (publish one object
per family of competing localisations) rather than another rejection gate.

This script measures whether that is true at the TRACK level, which is the level such a rule would
act on, and — critically — measures it BOTH ways:

  * `--mode truth`  : is a ghost track persistently on a real TARGET's ellipse? (the physics; needs
                      ground truth, so it cannot ship, but it bounds what is there to exploit)
  * `--mode tracks` : are two concurrent TRACKS on the same ellipse while world-separated? (what a
                      deployable rule can see — no ground truth anywhere in the criterion)

The deployable number is the one that decides implementability; the truth number says whether the
deployable one is finding the right thing.

Usage: analyze_track_families.py <tracks.jsonl> <run_dir> [--tol-m 15] [--dr-tol 5] [--rate-tol 0.5]
"""
import argparse
import json
import re
import collections

import numpy as np


def load_gt(ue_log):
    gt_re = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
    raw = collections.defaultdict(list)
    for line in open(ue_log, errors="ignore"):
        m = gt_re.search(line)
        if m:
            raw[int(m.group(2))].append(
                (float(m.group(1)), float(m.group(3)), float(m.group(4))))
    paths = {}
    for o, p in raw.items():
        p.sort()
        t = np.array([x[0] for x in p])
        X = np.array([x[1] for x in p])
        Y = np.array([x[2] for x in p])
        tt = np.linspace(t.min(), t.max(), 3000)
        paths[o] = np.stack([np.interp(tt, t, X), np.interp(tt, t, Y)], 1)
    return paths


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tracks")
    ap.add_argument("run_dir")
    ap.add_argument("--tol-m", type=float, default=15.0)
    ap.add_argument("--dr-tol", type=float, default=5.0, help="same-ellipse |ΔR| tolerance (m)")
    ap.add_argument("--rate-tol", type=float, default=0.5, help="same-manifold |Δrate| tol (m/s)")
    a = ap.parse_args()

    rep = json.loads(open(f"{a.run_dir}/oaiue_reports.jsonl").readline())
    tx = np.array(rep["tx_position"], float)
    rx = np.array(rep["rx_position"], float)
    base = float(np.linalg.norm(rx - tx))
    paths = load_gt(f"{a.run_dir}/logs/ue.log")

    def dR(P):
        return np.linalg.norm(P - tx, axis=-1) + np.linalg.norm(P - rx, axis=-1) - base

    def rate(P, V):
        u1 = (P - tx) / max(np.linalg.norm(P - tx), 1e-9)
        u2 = (P - rx) / max(np.linalg.norm(P - rx), 1e-9)
        return float(V @ (u1 + u2))

    tgt_dR = {o: dR(P) for o, P in paths.items()}

    # ---- load track updates, grouped by CPI ----------------------------------------------------
    upd = collections.defaultdict(list)          # t_ns -> [(id, P, V, world_err)]
    per_track = collections.defaultdict(list)    # id  -> [world_err]
    for line in open(a.tracks):
        t = json.loads(line)
        if t["status"] not in ("confirmed", "coasting"):
            continue
        P = np.array([t["x"], t["y"]], float)
        V = np.array([t["vx"], t["vy"]], float)
        we = min(float(np.min(np.hypot(Q[:, 0] - P[0], Q[:, 1] - P[1]))) for Q in paths.values())
        upd[t["time_utc_ns"]].append((t["id"], P, V, we))
        per_track[t["id"]].append(we)

    label = {i: ("real" if sum(1 for e in v if e <= a.tol_m) * 2 > len(v) else "ghost")
             for i, v in per_track.items()}
    ghosts = [i for i, l in label.items() if l == "ghost"]
    reals = [i for i, l in label.items() if l == "real"]
    print(f"tracks: {len(per_track)}  real={len(reals)}  ghost={len(ghosts)}   "
          f"(tol {a.tol_m} m; same-ellipse |dR|<{a.dr_tol} m, |drate|<{a.rate_tol} m/s)")

    # ---- MODE truth: ghost persistently on a real TARGET's ellipse ------------------------------
    on_tgt = collections.defaultdict(lambda: [0, 0])
    for t_ns, rows in upd.items():
        for tid, P, V, we in rows:
            if label[tid] != "ghost":
                continue
            g = dR(P)
            best = min(float(np.min(np.abs(v - g))) for v in tgt_dR.values())
            on_tgt[tid][0] += 1
            if best <= a.dr_tol:
                on_tgt[tid][1] += 1
    frac_truth = {t: c[1] / c[0] for t, c in on_tgt.items() if c[0] > 0}
    hi = [t for t, f in frac_truth.items() if f > 0.8]
    mid = [t for t, f in frac_truth.items() if 0.2 <= f <= 0.8]
    lo = [t for t, f in frac_truth.items() if f < 0.2]
    print(f"\n[truth]  ghost tracks vs REAL TARGET ellipses:")
    print(f"   >80% of life on one target's ellipse : {len(hi):3d} / {len(frac_truth)}")
    print(f"   20-80% (switches between ellipses)   : {len(mid):3d}")
    print(f"   <20% (belongs to no target ellipse)  : {len(lo):3d}")

    # ---- MODE tracks: two concurrent TRACKS on the same manifold, world-separated ---------------
    pair_same = collections.defaultdict(lambda: [0, 0])   # (i,j) -> [co-occurrences, same-manifold]
    for t_ns, rows in upd.items():
        for x in range(len(rows)):
            for y in range(x + 1, len(rows)):
                i, Pi, Vi, _ = rows[x]
                j, Pj, Vj, _ = rows[y]
                key = (min(i, j), max(i, j))
                pair_same[key][0] += 1
                if (abs(dR(Pi) - dR(Pj)) <= a.dr_tol
                        and abs(rate(Pi, Vi) - rate(Pj, Vj)) <= a.rate_tol
                        and float(np.linalg.norm(Pi - Pj)) > a.tol_m):
                    pair_same[key][1] += 1

    # a family edge: co-existed >= 3 CPIs AND same-manifold for >80% of them
    edges = [(k, c) for k, c in pair_same.items() if c[0] >= 3 and c[1] / c[0] > 0.8]
    fam = collections.defaultdict(set)
    for (i, j), _ in edges:
        fam[i].add(j)
        fam[j].add(i)
    absorbed = [g for g in ghosts if any(label.get(o) == "real" for o in fam.get(g, ()))]
    any_edge = [g for g in ghosts if fam.get(g)]
    print(f"\n[tracks] DEPLOYABLE rule (no ground truth used in the criterion):")
    print(f"   family edges found                        : {len(edges)}")
    print(f"   ghost tracks in a family with a REAL track : {len(absorbed):3d} / {len(ghosts)}")
    print(f"   ghost tracks in ANY family                 : {len(any_edge):3d} / {len(ghosts)}")

    # ---- what collapsing families would do to precision -----------------------------------------
    tot = sum(len(v) for v in per_track.values())
    good = sum(1 for v in per_track.values() for e in v if e <= a.tol_m)
    drop = sum(len(per_track[g]) for g in absorbed)
    dropgood = sum(1 for g in absorbed for e in per_track[g] if e <= a.tol_m)
    print(f"\n   updates now      : {tot}  good {good}  -> precision {100*good/max(tot,1):.1f}%")
    print(f"   absorbed by rule : {drop}  (of which good {dropgood})")
    print(f"   after collapsing : {tot-drop}  good {good-dropgood}  "
          f"-> precision {100*(good-dropgood)/max(tot-drop,1):.1f}%")


if __name__ == "__main__":
    main()
