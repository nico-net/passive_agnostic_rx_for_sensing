#!/usr/bin/env python3
"""Estimated detections vs ground truth, in SIMULATED time. Usage: plot_est_vs_gt.py <reports> <ue.log> <out.png> [label]"""
import sys, re, json
import numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt

rep, log, out = sys.argv[1], sys.argv[2], sys.argv[3]
label = sys.argv[4] if len(sys.argv) > 4 else ""
GT = re.compile(r"gt: t=([\d.]+)s utc_ns=(\d+) obj(\d+) .*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s(?: azimuth=([-\d.]+)deg)?")
gt = {}
for line in open(log, errors="ignore"):
    m = GT.search(line)
    if m:
        gt.setdefault(int(m.group(3)), []).append(
            (float(m.group(1)), int(m.group(2)), float(m.group(4)), float(m.group(5)),
             float(m.group(6)) if m.group(6) else np.nan))
for o in gt: gt[o].sort()
rs = [json.loads(l) for l in open(rep) if l.strip()]

# wall -> simulated time, so the x axis is the one the scene actually evolves in
allw = np.array([r[1] for o in gt for r in gt[o]]); alls = np.array([r[0] for o in gt for r in gt[o]])
k = np.argsort(allw); allw, alls = allw[k], alls[k]
def sim_of(w): return float(np.interp(w, allw, alls))

CO = ["#2F6FB2", "#C4622D"]                     # 2 series -> fixed categorical hues, never cycled
fig, ax = plt.subplots(2, 1, figsize=(11, 7.4), dpi=140, sharex=True)
for o, rows in sorted(gt.items()):
    t = np.array([r[0] for r in rows]); dR = np.array([r[2] for r in rows]); az = np.array([r[4] for r in rows])
    ax[0].plot(t, dR, "-", color=CO[o % 2], lw=2, label=f"obj{o} truth", zorder=3)
    ax[1].plot(t, az, "-", color=CO[o % 2], lw=2, label=f"obj{o} truth", zorder=3)
    tw = np.array([r[1] for r in rows])
    er, ea, es, et = [], [], [], []
    for x in rs:
        tc = x["cpi_start_time_utc_ns"] + x.get("cpi_duration_ns", 0)//2
        if tc < tw[0]-2e9 or tc > tw[-1]+2e9: continue
        ts = sim_of(tc); d_t = np.interp(ts, t, dR); a_t = np.interp(ts, t, az)
        best, bc = None, None
        for d in x["detections"]:
            dr = abs(d["bistatic_range_m"]-d_t); dv = abs(abs(d["bistatic_velocity_mps"])-abs(np.interp(ts,t,[r[3] for r in rows])))
            if dr <= 12 and dv <= 4:
                c = (dr/12)**2+(dv/4)**2
                if bc is None or c < bc: best, bc = d, c
        if best:
            et.append(ts); er.append(best["bistatic_range_m"])
            ea.append(best.get("azimuth_deg", np.nan)); es.append(best.get("snr_db", np.nan))
    et, er, ea, es = map(np.array, (et, er, ea, es))
    if len(et):
        hi = es >= 20                      # split by SNR: that is what drives bearing quality
        ax[0].plot(et[hi], er[hi], "o", ms=4, color=CO[o%2], mec="white", mew=.4, zorder=4)
        ax[0].plot(et[~hi], er[~hi], "o", ms=4, mfc="none", mec=CO[o%2], mew=.9, alpha=.65, zorder=4)
        ax[1].plot(et[hi], ea[hi], "o", ms=4, color=CO[o%2], mec="white", mew=.4, zorder=4)
        ax[1].plot(et[~hi], ea[~hi], "o", ms=4, mfc="none", mec=CO[o%2], mew=.9, alpha=.65, zorder=4)
ax[0].plot([], [], "o", ms=5, color="#555", mec="white", label="estimate, SNR>=20 dB")
ax[0].plot([], [], "o", ms=5, mfc="none", mec="#555", label="estimate, SNR<20 dB")
ax[0].set_ylabel("bistatic range (m)", fontsize=9.5)
ax[1].set_ylabel("azimuth (deg)", fontsize=9.5); ax[1].set_xlabel("simulated time (s)", fontsize=9.5)
ax[0].set_title(f"Estimated detections vs ground truth{'  -  '+label if label else ''}", fontsize=11, pad=9)
for a in ax:
    a.grid(alpha=.18, lw=.6); a.tick_params(labelsize=8.5, colors="#666")
    for sp in a.spines.values(): sp.set_color("#DDD")
    lg = a.legend(fontsize=8, framealpha=.92, edgecolor="#DDD", loc="best"); lg.get_frame().set_linewidth(.6)
fig.tight_layout(); fig.savefig(out, bbox_inches="tight", facecolor="white")
print("wrote", out)
