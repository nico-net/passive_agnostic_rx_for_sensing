#!/usr/bin/env python3
"""Plot the system geometry for a sensing_sim capture: illuminator + receiver positions (with array
boresight, if AoA is enabled) and every object's true trajectory, parsed straight from the
SENSING_CHANNEL ground-truth log lines (receiver-agnostic -- the real objects don't depend on which
receiver logged them, so this reads whichever ue.log has the most samples).

Usage: plot_scene_geometry.py --tx X,Y --rx rx1:X,Y[:boresight_deg] [--rx rx2:X,Y[:boresight_deg] ...]
                              --gt-log PATH --out PATH.png
"""
import argparse
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

GT_RE = re.compile(
    r"SENSING_CHANNEL gt: t=([\d.]+)s(?:\s+utc_ns=\d+)?\s+obj(\d+) pos=\(([-\d.]+),([-\d.]+)\)m"
)


def parse_gt(path):
    objs = {}
    with open(path, errors="replace") as f:
        for line in f:
            line = re.sub(r"\x1b\[[0-9;]*m", "", line)
            m = GT_RE.search(line)
            if m:
                t, obj, x, y = m.groups()
                objs.setdefault(int(obj), []).append((float(t), float(x), float(y)))
    for k in objs:
        objs[k].sort(key=lambda p: p[0])
    return objs


def parse_rx(spec):
    parts = spec.split(":")
    name = parts[0]
    x, y = (float(v) for v in parts[1].split(","))
    boresight = float(parts[2]) if len(parts) > 2 else None
    return name, x, y, boresight


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tx", required=True, help="X,Y")
    ap.add_argument("--rx", action="append", required=True, help="name:X,Y[:boresight_deg]")
    ap.add_argument("--gt-log", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    tx_x, tx_y = (float(v) for v in args.tx.split(","))
    rxs = [parse_rx(s) for s in args.rx]
    objs = parse_gt(args.gt_log)
    if not objs:
        raise SystemExit(f"no SENSING_CHANNEL gt: lines found in {args.gt_log}")

    fig, ax = plt.subplots(figsize=(7.5, 7), dpi=110)

    # Illuminator.
    ax.scatter([tx_x], [tx_y], marker="*", s=350, color="gold", edgecolors="black",
               zorder=5, label="Illuminator (gNB)")
    ax.annotate("TX", (tx_x, tx_y), textcoords="offset points", xytext=(8, 8), fontsize=9)

    # Receivers, with boresight arrow if AoA array is present.
    rx_colors = ["tab:blue", "tab:green", "tab:purple", "tab:brown"]
    for i, (name, x, y, boresight) in enumerate(rxs):
        c = rx_colors[i % len(rx_colors)]
        ax.scatter([x], [y], marker="^", s=250, color=c, edgecolors="black", zorder=5, label=f"{name} (receiver)")
        ax.annotate(name, (x, y), textcoords="offset points", xytext=(8, 8), fontsize=9)
        if boresight is not None:
            span = max(50.0, 0.15 * max(abs(x), abs(y), 1.0))
            rad = np.radians(boresight)
            ax.annotate(
                "", xy=(x + span * np.cos(rad), y + span * np.sin(rad)), xytext=(x, y),
                arrowprops=dict(arrowstyle="->", color=c, lw=1.5, alpha=0.7),
            )

    # Object trajectories.
    obj_colors = ["tab:red", "tab:orange", "tab:cyan", "tab:pink", "tab:olive", "tab:gray"]
    for obj_id in sorted(objs):
        pts = objs[obj_id]
        xs = [p[1] for p in pts]
        ys = [p[2] for p in pts]
        c = obj_colors[obj_id % len(obj_colors)]
        ax.plot(xs, ys, "-", color=c, linewidth=1.8, alpha=0.85, label=f"obj{obj_id} trajectory")
        ax.scatter([xs[0]], [ys[0]], marker="o", s=60, color=c, edgecolors="black", zorder=4)
        ax.scatter([xs[-1]], [ys[-1]], marker="s", s=60, color=c, edgecolors="black", zorder=4)
        ax.annotate(f"obj{obj_id} start", (xs[0], ys[0]), textcoords="offset points",
                    xytext=(6, -10), fontsize=7, color=c)
        ax.annotate(f"obj{obj_id} end", (xs[-1], ys[-1]), textcoords="offset points",
                    xytext=(6, 6), fontsize=7, color=c)

    ax.set_xlabel("ENU x (m)")
    ax.set_ylabel("ENU y (m)")
    ax.set_title("Scene geometry: illuminator, receivers (o = start, sq = end of trajectory)")
    ax.set_aspect("equal", adjustable="datalim")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
