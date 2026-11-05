#!/usr/bin/env python3
"""Quantisation test for method (b): a gNB that applies a Type I codeword uses beams exactly ON the oversampled DFT grid (O1=4, O2=4 or 1) and co-phases {0,pi/2,pi,3pi/2}.
For each grant compare the best score over the on-grid candidates with the best over the same family shifted by half a grid step in every dimension and a co-phase offset of pi/4
(same number of candidates, same structure, same models). A random choice of precoder gives on-grid > off-grid in 50 % of grants; a Type I gNB wins systematically.
Also the per-grant gap and the free-w upper bound (per-RB cos^2 of w = least squares of the whole grant)."""
import argparse, pickle, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import codebook_match as C
ap = argparse.ArgumentParser(); ap.add_argument("h"); ap.add_argument("grants"); ap.add_argument("--j0", type=int, required=True); ap.add_argument("--period", type=int, required=True)
ap.add_argument("--layer", type=int, default=0); ap.add_argument("--minrb", type=int, default=16); ap.add_argument("--max", type=int, default=60)
a = ap.parse_args()
Hall = np.load(a.h)["H"]; occ, _, P, nrx = Hall.shape; models = C.models_from(P)
def family(N1, N2, O1, O2, off):
    ws = []
    for l in range(N1 * O1):
        for m in range(N2 * O2 if N2 > 1 else 1):
            u = np.exp(2j * np.pi * (m + (off if N2 > 1 else 0)) * np.arange(N2) / (O2 * N2)) if N2 > 1 else np.ones(1)
            v = np.kron(np.exp(2j * np.pi * (l + off) * np.arange(N1) / (O1 * N1)), u)
            for n in range(4):
                ws.append(np.concatenate([v, np.exp(1j * (np.pi * n / 2 + off * np.pi / 2)) * v]) / np.sqrt(P))
    return np.array(ws)
fam = []
for name, perm in models:
    N1, N2 = name[0], name[1]; O1, O2 = C.O_of(N1, N2)
    for off in (0.0, 0.5):
        W = family(N1, N2, O1, O2, off); Wp = np.zeros_like(W); Wp[:, perm] = W; fam.append((name, off, Wp))
G = [g for g in pickle.load(open(a.grants, "rb")) if g["stat"] >= 0.8 and g["nrb"] >= a.minrb][: a.max]
win = 0; gaps = []; ub = []
for g in G:
    t = min(max(int(round((g["j"] - a.j0) / a.period)), 0), occ - 1); sl = slice(g["rb0"], g["rb0"] + g["nrb"])
    H = Hall[t, sl].transpose(0, 2, 1); he = g["h"] if a.layer == 0 else g["h1"]
    on = max(float(C.scores(H, he, W).max()) for n, o, W in fam if o == 0.0); off = max(float(C.scores(H, he, W).max()) for n, o, W in fam if o == 0.5)
    win += on > off; gaps.append(on - off)
    A = H.reshape(-1, P); y = he.reshape(-1); w = np.linalg.lstsq(A, y, rcond=1e-3)[0]; ub.append(float(C.scores(H, he, w[None])[0]))
n = len(G); from math import comb
p = sum(comb(n, k) for k in range(win, n + 1)) / 2 ** n
print("layer %d: %d grants (>=%d RB): on-grid beats half-step-shifted grid in %d (sign test p=%.3g) ; mean gap on-off %.4f ; mean free-w LS score %.3f" % (a.layer, n, a.minrb, win, p, np.mean(gaps), np.mean(ub)))
