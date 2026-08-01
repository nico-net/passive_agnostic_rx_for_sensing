#!/usr/bin/env python3
"""Fused world tracks AND velocity vs ground truth. Run on EVERY capture (standing instruction).

Track timestamps are in SIMULATED time (merge_receivers_walltime_n.py remaps wall->sim), so ground
truth is indexed the same way. Usage: plot_fused_tracks.py <tracks.jsonl> <ue.log> <out.png> [label]
"""
import sys, re, json
import numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt

tr, log, out = sys.argv[1], sys.argv[2], sys.argv[3]
label = sys.argv[4] if len(sys.argv) > 4 else ""
GT = re.compile(r"gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m")
gt = {}
for line in open(log, errors="ignore"):
    m = GT.search(line)
    if m: gt.setdefault(int(m.group(3)), []).append(
        (float(m.group(1)), float(m.group(4)), float(m.group(5))))
for o in gt: gt[o].sort()
T = [json.loads(l) for l in open(tr) if l.strip()] if tr else []
T = [t for t in T if t.get("status") == "confirmed"] or T

CO = ["#2F6FB2", "#C4622D"]; TRK = "#3C3C3C"
fig, ax = plt.subplots(1, 3, figsize=(15, 4.6), dpi=140)

# --- world XY ---
for o, rows in sorted(gt.items()):
    x = [r[1] for r in rows]; y = [r[2] for r in rows]
    ax[0].plot(x, y, "-", color=CO[o % 2], lw=2.2, label=f"obj{o} truth", zorder=3)
    ax[0].plot(x[0], y[0], "o", color=CO[o % 2], ms=6, mec="white", zorder=4)
tx = [t.get("x_m", np.nan) for t in T]; ty = [t.get("y_m", np.nan) for t in T]
ax[0].plot(tx, ty, ".", color=TRK, ms=3.2, alpha=.55, label=f"fused tracks (n={len(T)})", zorder=2)
ax[0].plot(0, 0, "^", color="#666", ms=9); ax[0].annotate("gNB", (0, 0), xytext=(6, -13),
           textcoords="offset points", fontsize=8, color="#666")
ax[0].set_xlabel("x (m)"); ax[0].set_ylabel("y (m)"); ax[0].set_title("world position", fontsize=10)

# --- position error vs simulated time ---
tt = np.array([t.get("time_s", t.get("t_s", np.nan)) for t in T], float)
if len(T) and np.isfinite(tt).any():
    err = []
    for t, X, Y in zip(tt, tx, ty):
        best = np.inf
        for o, rows in gt.items():
            g = np.array(rows)
            gx = np.interp(t, g[:, 0], g[:, 1]); gy = np.interp(t, g[:, 0], g[:, 2])
            best = min(best, float(np.hypot(X - gx, Y - gy)))
        err.append(best)
    err = np.array(err)
    ax[1].plot(tt, err, ".", color=TRK, ms=3.2, alpha=.6)
    ax[1].axhline(15, color="#C4622D", lw=1.4, ls="--", label="15 m tolerance")
    ax[1].set_yscale("log")
    ax[1].set_title(f"position error (median {np.nanmedian(err):.1f} m)", fontsize=10)
ax[1].set_xlabel("simulated time (s)"); ax[1].set_ylabel("|error| (m)")

# --- SPEED vs ground truth ---
for o, rows in sorted(gt.items()):
    g = np.array(rows); vt = (g[1:, 0] + g[:-1, 0]) / 2
    sp = np.hypot(np.diff(g[:, 1]), np.diff(g[:, 2])) / np.maximum(np.diff(g[:, 0]), 1e-9)
    ax[2].plot(vt, sp, "-", color=CO[o % 2], lw=2.2, label=f"obj{o} truth", zorder=3)
tv = np.array([np.hypot(t.get("vx_mps", np.nan), t.get("vy_mps", np.nan)) for t in T], float)
if len(T) and np.isfinite(tt).any():
    ax[2].plot(tt, tv, ".", color=TRK, ms=3.2, alpha=.55, label="fused tracks", zorder=2)
ax[2].set_xlabel("simulated time (s)"); ax[2].set_ylabel("speed (m/s)")
ax[2].set_title("speed vs truth", fontsize=10)

for a in ax:
    a.grid(alpha=.18, lw=.6); a.tick_params(labelsize=8.5, colors="#666")
    for sp_ in a.spines.values(): sp_.set_color("#DDD")
    lg = a.legend(fontsize=8, framealpha=.92, edgecolor="#DDD", loc="best")
    if lg: lg.get_frame().set_linewidth(.6)
fig.suptitle(f"Fused tracks vs ground truth{'  -  '+label if label else ''}", fontsize=11.5)
fig.tight_layout(); fig.savefig(out, bbox_inches="tight", facecolor="white")
print("wrote", out)
