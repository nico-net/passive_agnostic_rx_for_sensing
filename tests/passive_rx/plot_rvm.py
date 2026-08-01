#!/usr/bin/env python3
"""Plot a range-velocity map (RVM) raster dumped by the sensing engine ([sensing] capture = 1).

The raster is flat float32 POWER, nof_range * nof_doppler, written by sensing_engine.cc as
<out_path>_rvm_<N>.f32. Axis metadata (range_res_m, vel_res_mps, vel_max_mps) comes from the
matching DetectionReport line, so the two must be paired by index.

Magnitude data -> SEQUENTIAL single-hue ramp, light (low power) to dark (high power). Deliberately
not a rainbow: a multi-hue ramp invents boundaries in continuous data and reads differently under
colour-vision deficiency. Ground-truth target positions are drawn as open rings so they never
occlude the cell they are marking.

Usage: plot_rvm.py <rvm.f32> <reports.jsonl> <cpi_index> <out.png> [ue.log] [max_range_m]
"""
import sys, re, json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rvm_path, rep_path, idx, out_png = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
log_path = sys.argv[5] if len(sys.argv) > 5 else None
RMAX = float(sys.argv[6]) if len(sys.argv) > 6 else 500.0

reports = [json.loads(l) for l in open(rep_path) if l.strip()]
rep = reports[idx]
rres, vres = rep["range_res_m"], rep["vel_res_mps"]
vmax = rep["vel_max_mps"]

a = np.fromfile(rvm_path, dtype=np.float32)
ndop = int(round(2 * vmax / vres))
nrng = a.size // ndop
assert nrng * ndop == a.size, f"{a.size} not divisible into {ndop} Doppler bins"
# Range-major (each range bin's Doppler spectrum contiguous) is what sensing_engine.cc writes.
P = a.reshape(nrng, ndop)

rng = np.arange(nrng) * rres
vel = (np.arange(ndop) - ndop // 2) * vres
keep = rng <= RMAX
P, rng = P[keep], rng[keep]

PdB = 10 * np.log10(np.maximum(P, P[P > 0].min() if (P > 0).any() else 1e-12))
floor = np.percentile(PdB, 50)
PdB = np.clip(PdB, floor, None)

fig, ax = plt.subplots(figsize=(9, 5.2), dpi=140)
im = ax.pcolormesh(vel, rng, PdB, cmap="Blues", shading="nearest")
cb = fig.colorbar(im, ax=ax, pad=0.015)
cb.set_label("power (dB, relative)", fontsize=9, color="#444")
cb.ax.tick_params(labelsize=8, colors="#666")
cb.outline.set_visible(False)

# Ground truth for this CPI, interpolated in utc_ns (gt is logged once per SIMULATED second, which
# is tens of wall-seconds apart on this harness -- see check_detection.py).
if log_path:
    GT = re.compile(r"SENSING_CHANNEL gt: t=[\d.]+s utc_ns=(\d+) obj(\d+) .*?dR=([-\d.]+)m "
                    r"range_rate=([-\d.]+)m/s")
    gt = {}
    for line in open(log_path, errors="ignore"):
        m = GT.search(line)
        if m:
            gt.setdefault(int(m.group(2)), []).append(
                (int(m.group(1)), float(m.group(3)), float(m.group(4))))
    tc = rep["cpi_start_time_utc_ns"] + rep.get("cpi_duration_ns", 0) // 2
    for o, rows in sorted(gt.items()):
        rows.sort()
        t = np.array([r[0] for r in rows])
        if not (t[0] - 2e9 <= tc <= t[-1] + 2e9):
            continue
        r_t = np.interp(tc, t, [r[1] for r in rows])
        v_t = np.interp(tc, t, [r[2] for r in rows])
        if r_t <= RMAX:
            ax.plot(v_t, r_t, "o", mfc="none", mec="#D1495B", mew=1.8, ms=13, zorder=5)
            ax.annotate(f"obj{o} (truth)", (v_t, r_t), textcoords="offset points",
                        xytext=(11, 7), fontsize=8.5, color="#D1495B", zorder=6)

for d in rep["detections"]:
    if d["bistatic_range_m"] <= RMAX:
        ax.plot(d["bistatic_velocity_mps"], d["bistatic_range_m"], "x",
                color="#1B1B1B", mew=1.3, ms=6, zorder=4)
ax.plot([], [], "x", color="#1B1B1B", mew=1.3, ms=6, label="CFAR detections")
ax.plot([], [], "o", mfc="none", mec="#D1495B", mew=1.8, ms=9, label="ground truth")
leg = ax.legend(loc="upper right", fontsize=8.5, framealpha=0.92, edgecolor="#DDD")
leg.get_frame().set_linewidth(0.6)

ax.set_xlabel("bistatic velocity (m/s)", fontsize=9.5)
ax.set_ylabel("bistatic differential range (m)", fontsize=9.5)
ax.set_title(f"Range-velocity map, CPI #{idx}   "
             f"(range_res {rres:.2f} m, vel_res {vres:.3f} m/s, vel_max +/-{vmax:.2f} m/s)",
             fontsize=10.5, pad=10)
ax.tick_params(labelsize=8.5, colors="#666")
for sp in ax.spines.values():
    sp.set_color("#DDD")
fig.tight_layout()
fig.savefig(out_png, bbox_inches="tight", facecolor="white")
print(f"wrote {out_png}  ({nrng} range x {ndop} Doppler bins, shown to {RMAX:.0f} m)")
