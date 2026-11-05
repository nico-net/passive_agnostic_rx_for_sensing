#!/usr/bin/env python3
"""Method (b), coherent variant for (nearly) rank-1 H: the per-RB cos^2 of run_b.py is at its ceiling for ANY w when H = u v^H, so it cannot separate codewords.
Here ONE complex scalar (and a common phase ramp over RB) for the whole grant is used, so the frequency profile of v(rb)^H w must match. Selection = top-K candidates by the
per-RB score, then coherent refine (codebook_match.refine). The null applies the identical selection to the same number of random isotropic w."""
import argparse, pickle, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import codebook_match as C
ap = argparse.ArgumentParser(); ap.add_argument("h"); ap.add_argument("grants"); ap.add_argument("--j0", type=int, required=True); ap.add_argument("--period", type=int, required=True)
ap.add_argument("--layer", type=int, default=0); ap.add_argument("--minrb", type=int, default=64); ap.add_argument("--max", type=int, default=12); ap.add_argument("--topk", type=int, default=150)
a = ap.parse_args()
Hall = np.load(a.h)["H"]; occ, _, P, nrx = Hall.shape; models = C.models_from(P); rng = np.random.default_rng(11)
G = [g for g in pickle.load(open(a.grants, "rb")) if g["stat"] >= 0.8 and g["nrb"] >= a.minrb][: a.max]
cbs = []
for name, perm in models:
    N1, N2 = name[0], name[1]; O1, O2 = C.O_of(N1, N2); W, lab = C.typeI_rank1(N1, N2, O1, O2); Wp = np.zeros_like(W); Wp[:, perm] = W; cbs.append((name, Wp, lab))
Wall = np.concatenate([c[1] for c in cbs]); labs = [(c[0], l) for c in cbs for l in c[2]]
wins = 0; res = []
for g in G:
    t = min(max(int(round((g["j"] - a.j0) / a.period)), 0), occ - 1); sl = slice(g["rb0"], g["rb0"] + g["nrb"])
    H = Hall[t, sl].transpose(0, 2, 1); he = g["h"] if a.layer == 0 else g["h1"]
    def best_coh(W):
        s = C.scores(H, he, W); top = np.argsort(-s)[: a.topk]
        return max((C.refine(H, he, W[i]), i) for i in top)
    cb, ci = best_coh(Wall)
    Wr = rng.normal(size=Wall.shape) + 1j * rng.normal(size=Wall.shape); Wr /= np.linalg.norm(Wr, axis=1, keepdims=True)
    nl_, _ = best_coh(Wr)
    # planted-codeword control on the SAME H (coherent method): synthetic h_eff = H w_true x random scalar+ramp, 10 dB noise
    ct = rng.integers(len(Wall)); y = np.einsum("rap,p->ra", H, Wall[ct]) * np.exp(1j * (0.4 + 0.07 * np.arange(H.shape[0])))[:, None]
    y = y + (rng.normal(size=y.shape) + 1j * rng.normal(size=y.shape)) * np.sqrt((np.abs(y) ** 2).mean() / 10 / 2)
    s = C.scores(H, y, Wall); top = np.argsort(-s)[: a.topk]; pc = max((C.refine(H, y, Wall[i]), i) for i in top)
    res.append((cb, nl_, pc[0], pc[1] == ct or C.refine(H, y, Wall[ct]) >= 0.98 * pc[0]))
    print("abs %d nrb %3d | coherent best codebook %.3f %s | random-w best (same selection) %.3f | planted control %.3f recovered=%s" % (g["abs"], g["nrb"], cb, labs[ci], nl_, pc[0], res[-1][3]))
r = np.array([(x[0], x[1], x[2]) for x in res]); print("MEAN codebook %.3f random %.3f ; codebook>random in %d/%d ; planted recovered %d/%d" % (r[:, 0].mean(), r[:, 1].mean(), (r[:, 0] > r[:, 1]).sum(), len(r), sum(x[3] for x in res), len(res)))
