#!/usr/bin/env python3
"""Animate a receiver's per-CPI range-Doppler maps with detections + true-target overlays.

Shows "tracking in the RVM": each frame is one CPI's range vs bistatic-range-rate power map, with
- white stars  = the two targets' TRUE (dR, range-rate) at that CPI's sim-time (from the known
                 trajectory + this receiver's geometry),
- green dots   = detections that match a real target (within tol of a star),
- red x's      = ghost detections (everything else).
Each CPI is stamped with its sim-time by inverting the trajectory from its own detections.

Usage: make_tracking_gif.py <run_dir> <out.gif> <rx_x> <rx_y> [range_crop_m=600]
Scene (mot2): objA(t)=(35+4t,55+9t), objB(t)=(70-3t,120+8t). Edit if the scene changes.
"""
import sys, glob, re
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_agg import FigureCanvasAgg
from PIL import Image

run_dir = sys.argv[1]
gif_path = sys.argv[2]
RX = (float(sys.argv[3]), float(sys.argv[4]))
RANGE_CROP_M = float(sys.argv[5]) if len(sys.argv) > 5 else 600.0
NOF_SUBC, CPI_SLOTS = 3276, 128
RANGE_RES = 3.0517578
RTOL, VTOL = 15.0, 3.0
TGRID = np.linspace(0.0, 20.0, 8001)

def obj(name, t):
    return (35 + 4*t, 55 + 9*t) if name == 'A' else (70 - 3*t, 120 + 8*t)
def curves(rx):
    tx = np.array([0.0, 0.0]); rxp = np.array(rx); r_los = np.hypot(*(tx - rxp)); out = {}
    for g in ('A', 'B'):
        ox, oy = obj(g, TGRID)
        dR = np.hypot(tx[0]-ox, tx[1]-oy) + np.hypot(ox-rxp[0], oy-rxp[1]) - r_los
        out[g] = (dR, np.gradient(dR, TGRID))
    return out
CV = curves(RX)

# detections grouped by CPI: cpi,range_bin,dopp_bin,range_m,vel_mps,snr
dets = {}
for line in open(f"{run_dir}/oaiue_sensing_detections.csv"):
    p = line.strip().split(",")
    if len(p) < 6: continue
    c = int(p[0]); dets.setdefault(c, []).append((float(p[3]), float(p[4]), float(p[5])))

def stamp_simtime(cpi):
    best_t, best_cost = None, 1e18
    for dr, v, _ in dets.get(cpi, []):
        for g in ('A', 'B'):
            dRc, rc = CV[g]; cost = ((dr - dRc)/RTOL)**2 + ((v - rc)/VTOL)**2
            j = int(np.argmin(cost))
            if cost[j] < best_cost: best_cost, best_t = cost[j], float(TGRID[j])
    return best_t if best_cost <= 2.0 else None

files = sorted(glob.glob(f"{run_dir}/oaiue_sensing_rvm_*.f32"),
               key=lambda p: int(re.search(r"_rvm_(\d+)\.f32", p).group(1)))
crop = int(RANGE_CROP_M / RANGE_RES)
# global colour scale
meds = []
for f in files[:15]:
    p = np.fromfile(f, dtype=np.float32)
    if p.size == NOF_SUBC*CPI_SLOTS: meds.append(np.median(p[p>0]) if np.any(p>0) else 1e-9)
gmed = float(np.median(meds)) if meds else 1e-9
VEL_RES = 0.29  # nominal m/s per Doppler bin (axis scale ~const across CPIs); detections use exact m/s

frames = []
for f in files:
    cpi = int(re.search(r"_rvm_(\d+)\.f32", f).group(1))
    raw = np.fromfile(f, dtype=np.float32)
    if raw.size != NOF_SUBC*CPI_SLOTS: continue
    power = raw.reshape(NOF_SUBC, CPI_SLOTS)[:crop, :]
    db = 10.0*np.log10(power/gmed + 1e-12)
    vel_axis = (np.arange(CPI_SLOTS) - CPI_SLOTS/2)*VEL_RES

    t = stamp_simtime(cpi)
    fig = plt.figure(figsize=(5.4, 6.6), dpi=100); ax = fig.add_subplot(111)
    ax.imshow(db, aspect="auto", origin="lower", cmap="turbo", vmin=0, vmax=40,
              extent=[vel_axis[0], vel_axis[-1], 0, crop*RANGE_RES])
    # true target stars
    stars = []
    if t is not None:
        for g in ('A', 'B'):
            dRc, rc = CV[g]; j = int(np.argmin(np.abs(TGRID - t)))
            stars.append((rc[j], dRc[j]))
            ax.plot(rc[j], dRc[j], marker='*', ms=20, mfc='white', mec='k', mew=1.2, ls='')
    # detections: green if near a star, red otherwise
    for dr, v, _ in dets.get(cpi, []):
        if dr > RANGE_CROP_M: continue
        near = any(abs(dr-sr) <= RTOL and abs(v-sv) <= VTOL for sv, sr in stars)
        ax.plot(v, dr, marker=('o' if near else 'x'), ms=7,
                mfc=('lime' if near else 'none'), mec=('green' if near else 'red'), mew=1.6, ls='')
    ax.set_xlim(vel_axis[0], vel_axis[-1]); ax.set_ylim(0, RANGE_CROP_M)
    ax.set_xlabel("bistatic range-rate (m/s)"); ax.set_ylabel("bistatic differential range (m)")
    tt = f"t={t:.1f}s" if t is not None else "t=?"
    ax.set_title(f"RVM  rx@({RX[0]:.0f},{RX[1]:.0f})  CPI {cpi}  {tt}\n★ true target   ● matched det   ✕ ghost", fontsize=10)
    fig.tight_layout()
    c = FigureCanvasAgg(fig); c.draw()
    frames.append(Image.frombuffer("RGBA", c.get_width_height(), c.buffer_rgba(), "raw", "RGBA", 0, 1).convert("RGB"))
    plt.close(fig)

if frames:
    frames[0].save(gif_path, save_all=True, append_images=frames[1:], duration=350, loop=0)
    print(f"wrote {len(frames)} frames -> {gif_path}")
else:
    print("no frames")
