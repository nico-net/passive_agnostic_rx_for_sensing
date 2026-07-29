#!/usr/bin/env python3
"""Plot per-CPI Range-Velocity Maps (RVM) from a sensing_engine capture, and animate them into a GIF.

Reads <out_prefix>_rvm_<N>.f32 (raw sensing_rvm_t.power rasters, capture=1 in [sensing]) and pairs
each with its own CPI's line in the matching reports.jsonl by ORDER (both are written together per
CPI close in sensing_engine::write_outputs(), so the i-th rvm file matches the i-th report line for
a fresh output directory) -- this is deliberate: range_res_m/vel_res_mps/nof_range_bins can differ
CPI-to-CPI (fractional cpi_period_slots), so a single fixed axis assumption across the whole run
would silently mislabel some frames.

Row-major layout confirmed against range_doppler.cc: rvm.power[r * nof_dopp + d].

Usage: plot_rvm.py <run_dir> [--out DIR] [--gif PATH] [--max-frames N]
  <run_dir> must contain oaiue_sensing_rvm_*.f32 and oaiue_reports.jsonl (or pass --rvm-prefix /
  --reports explicitly for a non-standard layout).
"""
import argparse
import glob
import json
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


def load_reports(path):
    reports = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                reports.append(json.loads(line))
    return reports


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--rvm-prefix", default=None, help="override: glob prefix for _rvm_N.f32 files")
    ap.add_argument("--reports", default=None, help="override: path to reports.jsonl")
    ap.add_argument("--out", default=None, help="PNG output dir (default: <run_dir>/rvm_frames)")
    ap.add_argument("--gif", default=None, help="GIF output path (default: <run_dir>/rvm.gif)")
    ap.add_argument("--max-frames", type=int, default=0, help="0 = all")
    ap.add_argument("--fps", type=float, default=2.0)
    args = ap.parse_args()

    rvm_prefix = args.rvm_prefix
    if rvm_prefix is None:
        candidates = glob.glob(os.path.join(args.run_dir, "*_rvm_*.f32"))
        if not candidates:
            raise SystemExit(f"no *_rvm_*.f32 files under {args.run_dir}")
        m = re.match(r"(.*)_rvm_\d+\.f32$", candidates[0])
        rvm_prefix = m.group(1)

    reports_path = args.reports or os.path.join(args.run_dir, "oaiue_reports.jsonl")
    reports = load_reports(reports_path)
    if not reports:
        raise SystemExit(f"no reports found at {reports_path}")

    rvm_files = sorted(
        glob.glob(f"{rvm_prefix}_rvm_*.f32"),
        key=lambda p: int(re.search(r"_rvm_(\d+)\.f32$", p).group(1)),
    )
    if not rvm_files:
        raise SystemExit(f"no rvm files matching {rvm_prefix}_rvm_*.f32")

    n = min(len(rvm_files), len(reports))
    if args.max_frames > 0:
        n = min(n, args.max_frames)
    if len(rvm_files) != len(reports):
        print(f"WARNING: {len(rvm_files)} rvm files vs {len(reports)} report lines -- "
              f"pairing the first {n} in order; a mismatch means the run/capture is incomplete "
              f"or this out_path was reused across sessions.")

    out_dir = args.out or os.path.join(args.run_dir, "rvm_frames")
    os.makedirs(out_dir, exist_ok=True)
    gif_path = args.gif or os.path.join(args.run_dir, "rvm.gif")

    png_paths = []
    vmax_global = 0.0
    rasters = []
    for i in range(n):
        rep = reports[i]
        nof_range = rep.get("nof_range_bins")
        nof_dopp = rep.get("nof_doppler_bins")
        range_res = rep["range_res_m"]
        range_max = rep["range_max_m"]
        vel_max = rep["vel_max_mps"]
        raw = np.fromfile(rvm_files[i], dtype=np.float32)
        if not nof_range or not nof_dopp:
            # Not carried in every report schema version -- derive from the raster size + axis
            # metadata that IS always present, rather than guessing a fixed size.
            nof_range = int(round(range_max / range_res))
            nof_dopp = raw.size // max(1, nof_range)
        if nof_range * nof_dopp != raw.size:
            print(f"  frame {i}: size mismatch (expected {nof_range}x{nof_dopp}={nof_range*nof_dopp}, "
                  f"got {raw.size}) -- skipping")
            continue
        rvm = raw.reshape(nof_range, nof_dopp)
        rasters.append((i, rep, rvm, range_res, range_max, vel_max))
        vmax_global = max(vmax_global, float(rvm.max()))

    if not rasters:
        raise SystemExit("no valid frames after size-matching -- check nof_range/nof_dopp derivation")

    # dB scale, common floor across all frames so brightness is comparable frame-to-frame in the GIF.
    floor_db = -30.0
    vmax_db = 10.0 * np.log10(vmax_global + 1e-12)

    for i, rep, rvm, range_res, range_max, vel_max in rasters:
        rvm_db = 10.0 * np.log10(rvm + 1e-12) - vmax_db  # 0 dB = this run's global peak
        fig, ax = plt.subplots(figsize=(7, 5), dpi=110)
        im = ax.imshow(
            rvm_db.T,
            origin="lower",
            aspect="auto",
            extent=[0.0, range_max, -vel_max, vel_max],
            vmin=floor_db,
            vmax=0.0,
            cmap="viridis",
        )
        dets = rep.get("detections", [])
        if dets:
            ax.scatter(
                [d["bistatic_range_m"] for d in dets],
                [d["bistatic_velocity_mps"] for d in dets],
                s=40, facecolors="none", edgecolors="red", linewidths=1.2, label="CFAR detection",
            )
            ax.legend(loc="upper right", fontsize=8)
        ax.set_xlabel("Bistatic differential range (m)")
        ax.set_ylabel("Bistatic range-rate (m/s)")
        rx_id = rep.get("rx_id", "?")
        ax.set_title(f"RVM  rx={rx_id}  CPI #{i}  ({len(dets)} detections)")
        cbar = fig.colorbar(im, ax=ax)
        cbar.set_label("Power (dB rel. run peak)")
        fig.tight_layout()
        png_path = os.path.join(out_dir, f"rvm_cpi{i:04d}.png")
        fig.savefig(png_path)
        plt.close(fig)
        png_paths.append(png_path)

    print(f"wrote {len(png_paths)} PNG frames to {out_dir}")

    frames = [Image.open(p).convert("P", palette=Image.ADAPTIVE) for p in png_paths]
    duration_ms = int(1000.0 / max(0.1, args.fps))
    frames[0].save(
        gif_path, save_all=True, append_images=frames[1:], duration=duration_ms, loop=0,
    )
    print(f"wrote GIF ({len(frames)} frames, {duration_ms}ms/frame) to {gif_path}")


if __name__ == "__main__":
    main()
