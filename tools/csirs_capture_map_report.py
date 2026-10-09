#!/usr/bin/env python3
"""Summarise csirs_capture_map.py outputs (light: reads the .npz only).
map:  per slot%160 / symbol, the RB runs of each subcarrier class (k mod 12) whose RE repeats coherently every 160
      slots (CSI-RS/TRS/DM-RS/SSB) and the REs that stay empty under loaded neighbours (ZP-CSI-RS candidates).
ids:  per phase / symbol / k0, the scrambling IDs whose row-1 matched filter beats noise, with their RB runs."""
import json, sys
import numpy as np


def runs(mask, gap=1):
    idx = np.flatnonzero(mask)
    if idx.size == 0:
        return []
    out, s, p = [], idx[0], idx[0]
    for i in idx[1:]:
        if i - p > gap + 1:
            out.append((s, p))
            s = i
        p = i
    out.append((s, p))
    return out


def fmt(rs):
    return ",".join(f"{a}-{b}" for a, b in rs)


def report_map(f, coh_thr, zp_thr, minrb):
    d = np.load(f)
    C, N, P = d["C"], d["N"], d["P"]
    print(json.loads(str(d["meta"])))
    coh = C / np.maximum(N, 1e-9)
    nph = coh.shape[0]
    coh = coh.reshape(nph, 14, 217, 12)
    P = P.reshape(nph, 14, 217, 12)
    for ph in range(nph):
        lines = []
        for l in range(14):
            parts = []
            for c in range(12):
                rs = [r for r in runs(coh[ph, l, :, c] > coh_thr) if r[1] - r[0] + 1 >= minrb]
                if rs:
                    parts.append(f"c{c}:{fmt(rs)}")
            # ZP: RE power << median of its RB on this symbol, where the RB is loaded on average
            med = np.median(P[ph, l], axis=1)
            load = med > 0.3 * np.median(P[ph, :, :, :])
            zp = []
            for c in range(12):
                rs = [r for r in runs(load & (P[ph, l, :, c] < zp_thr * med)) if r[1] - r[0] + 1 >= minrb]
                if rs:
                    zp.append(f"c{c}:{fmt(rs)}")
            if parts or zp:
                lines.append(f"  l{l:2d} coh[{' '.join(parts)}]" + (f" ZP[{' '.join(zp)}]" if zp else ""))
        if lines:
            print(f"phase {ph:3d} (slot {ph % 20:2d} frame%8={ph // 20})")
            print("\n".join(lines))


def report_ids(f, thr, minrb):
    d = np.load(f)
    R = d["R"]  # (phases, syms, k0, 1024, 217)
    phases, syms = list(d["phases"]), list(d["syms"])
    print(json.loads(str(d["meta"])))
    for pi, ph in enumerate(phases):
        for si, l in enumerate(syms):
            for k0 in range(4):
                r = R[pi, si, k0]  # (1024, 217)
                # 4-RB smoothing to beat per-RB noise
                rr = np.apply_along_axis(lambda v: np.convolve(v, np.ones(4) / 4, mode="same"), 1, r)
                best = rr.max(1)
                cand = np.flatnonzero(best > thr)
                for i in cand[np.argsort(-best[cand])][:6]:
                    rs = [x for x in runs(rr[i] > thr) if x[1] - x[0] + 1 >= minrb]
                    if rs:
                        print(f"ph{ph:3d} l{l:2d} k0={k0} id={i:4d} peak={best[i]:.2f} mean_in={np.mean([r[i, a:b+1].mean() for a, b in rs]):.2f} RB {fmt(rs)}")


if __name__ == "__main__":
    mode, f = sys.argv[1], sys.argv[2]
    if mode == "map":
        report_map(f, float(sys.argv[3]) if len(sys.argv) > 3 else 0.5, float(sys.argv[4]) if len(sys.argv) > 4 else 0.25,
                   int(sys.argv[5]) if len(sys.argv) > 5 else 4)
    else:
        report_ids(f, float(sys.argv[3]) if len(sys.argv) > 3 else 1.6, int(sys.argv[4]) if len(sys.argv) > 4 else 4)
