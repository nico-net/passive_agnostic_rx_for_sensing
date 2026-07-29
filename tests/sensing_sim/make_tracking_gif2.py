#!/usr/bin/env python3
"""Animate a run's per-CPI range-Doppler maps with detections + true-target overlays.

Scene-agnostic: ground truth (dR, range_rate) and the per-CPI axis scaling (range_res / vel_res, which
now VARIES per CPI under sub-slot sampling) are both parsed from the run's own ue.log, so this works
for any scene and any subslot setting without edits.

  white star = true target at that CPI's inferred sim-time
  green dot  = detection matching a real target
  red x      = ghost detection

Usage: make_tracking_gif2.py <run_dir> <out.gif> [range_crop_m=700] [stride=1]
"""
import sys, glob, re, collections
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_agg import FigureCanvasAgg
from PIL import Image

run = sys.argv[1]
gif = sys.argv[2]
CROP = float(sys.argv[3]) if len(sys.argv) > 3 else 700.0
STRIDE = int(sys.argv[4]) if len(sys.argv) > 4 else 1
NOF_SUBC, CPI_SLOTS = 3276, 128
RTOL, VTOL = 15.0, 3.0

log = f"{run}/logs/ue.log"
# --- ground truth curves: obj -> (t, dR, rate) ---
gt = collections.defaultdict(list)
gtre = re.compile(r"SENSING_CHANNEL gt: t=([\d.]+)s(?:\s+utc_ns=\d+)? obj(\d+).*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s")
# --- per-CPI axis scaling ---
cpire = re.compile(r"SENSING: CPI #(\d+).*?range\[res=([\d.]+).*?vel\[res=([\d.]+) max=([\d.]+)\]")
axes = {}
with open(log, errors="ignore") as f:
    for line in f:
        m = gtre.search(line)
        if m:
            gt[int(m.group(2))].append((float(m.group(1)), float(m.group(3)), float(m.group(4))))
        m = cpire.search(line)
        if m:
            axes[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))

curves = {}
for o, pts in gt.items():
    pts.sort()
    t = np.array([p[0] for p in pts])
    tt = np.linspace(t.min(), t.max(), 4000)
    curves[o] = (tt,
                 np.interp(tt, t, [p[1] for p in pts]),
                 np.interp(tt, t, [p[2] for p in pts]))

dets = collections.defaultdict(list)
for line in open(f"{run}/oaiue_sensing_detections.csv"):
    p = line.strip().split(",")
    if len(p) >= 6:
        dets[int(p[0])].append((float(p[3]), float(p[4])))

def sim_time(cpi):
    """Infer this CPI's trajectory time by matching its detections to the GT curves."""
    best, bt = 1e18, None
    for dr, v in dets.get(cpi, []):
        for o, (tt, dR, rr) in curves.items():
            c = ((dr - dR) / RTOL) ** 2 + ((v - rr) / VTOL) ** 2
            j = int(np.argmin(c))
            if c[j] < best:
                best, bt = c[j], float(tt[j])
    return bt if best <= 2.0 else None

files = sorted(glob.glob(f"{run}/oaiue_sensing_rvm_*.f32"),
               key=lambda p: int(re.search(r"_rvm_(\d+)\.f32", p).group(1)))[::STRIDE]
meds = []
for f in files[:15]:
    a = np.fromfile(f, dtype=np.float32)
    if a.size == NOF_SUBC * CPI_SLOTS:
        meds.append(np.median(a[a > 0]) if np.any(a > 0) else 1e-9)
gmed = float(np.median(meds)) if meds else 1e-9

frames = []
for fp in files:
    cpi = int(re.search(r"_rvm_(\d+)\.f32", fp).group(1))
    if cpi not in axes:
        continue
    rres, vres, vmax = axes[cpi]
    raw = np.fromfile(fp, dtype=np.float32)
    if raw.size != NOF_SUBC * CPI_SLOTS:
        continue
    crop = int(CROP / rres)
    db = 10.0 * np.log10(raw.reshape(NOF_SUBC, CPI_SLOTS)[:crop, :] / gmed + 1e-12)
    vax = (np.arange(CPI_SLOTS) - CPI_SLOTS / 2) * vres

    t = sim_time(cpi)
    stars = []
    if t is not None:
        for o, (tt, dR, rr) in curves.items():
            j = int(np.argmin(np.abs(tt - t)))
            stars.append((rr[j], dR[j]))

    fig = plt.figure(figsize=(5.4, 6.6), dpi=100)
    ax = fig.add_subplot(111)
    ax.imshow(db, aspect="auto", origin="lower", cmap="turbo", vmin=0, vmax=40,
              extent=[vax[0], vax[-1], 0, crop * rres])
    for sv, sr in stars:
        if abs(sv) <= abs(vax[-1]) and sr <= CROP:
            ax.plot(sv, sr, marker="*", ms=20, mfc="white", mec="k", mew=1.2, ls="")
    for dr, v in dets.get(cpi, []):
        if dr > CROP:
            continue
        near = any(abs(dr - sr) <= RTOL and abs(v - sv) <= VTOL for sv, sr in stars)
        ax.plot(v, dr, marker=("o" if near else "x"), ms=7,
                mfc=("lime" if near else "none"), mec=("green" if near else "red"), mew=1.6, ls="")
    ax.set_xlim(vax[0], vax[-1]); ax.set_ylim(0, CROP)
    ax.set_xlabel("bistatic range-rate (m/s)")
    ax.set_ylabel("bistatic differential range (m)")
    ax.set_title(f"CPI {cpi}   vel_max=±{vmax:.0f} m/s   {f't={t:.1f}s' if t else 't=?'}\n"
                 f"★ true target   ● matched   ✕ ghost", fontsize=10)
    fig.tight_layout()
    c = FigureCanvasAgg(fig); c.draw()
    frames.append(Image.frombuffer("RGBA", c.get_width_height(), c.buffer_rgba(), "raw", "RGBA", 0, 1).convert("RGB"))
    plt.close(fig)

if frames:
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=350, loop=0)
    print(f"wrote {len(frames)} frames -> {gif}")
else:
    print("no frames")
