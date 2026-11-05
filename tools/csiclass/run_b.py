#!/usr/bin/env python3
"""Methods (b) and (c) on decoded grants: H from the nearest CSI-RS occasion of a resource, h_eff from the DM-RS of a CRC-OK single-layer grant (grants_dmrs.py).
(b) best Type I rank-1 codeword over all (N1,N2,port-ordering) models: score = mean over RBs of cos^2(h_eff(rb), H(rb) w) (one complex scalar per RB);
    reported: best, second best (different codeword), best of N random isotropic w (N = candidates tested; null), and the same for h_eff taken from a different RNTI/slot
    (shuffled grant) so that H is the same.
(c) per-PRG LS: w_prg = argmin sum_{rb in prg} ||h_eff(rb) a - H(rb) w||  with a = 1 after removing a per-grant common phase ramp: residual/|h_eff| vs the same LS on shuffled data."""
import argparse, pickle, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import codebook_match as C

ap = argparse.ArgumentParser(); ap.add_argument("h"); ap.add_argument("grants"); ap.add_argument("--j0", type=int, required=True); ap.add_argument("--period", type=int, required=True)
ap.add_argument("--minrb", type=int, default=16); ap.add_argument("--rnti", type=int, default=-1); ap.add_argument("--minstat", type=float, default=0.8); ap.add_argument("--max", type=int, default=40)
ap.add_argument("--maxlag", type=int, default=100000)
a = ap.parse_args()
Hall = np.load(a.h)["H"]; occ, nrb_all, P, nrx = Hall.shape
G = [g for g in pickle.load(open(a.grants, "rb")) if g["stat"] >= a.minstat and g["nrb"] >= a.minrb and (a.rnti < 0 or g["rnti"] == a.rnti)][: a.max]
models = C.models_from(P); rng = np.random.default_rng(3)
print("H", Hall.shape, "grants", len(G), "models", len(models))
rows = []
for gi, g in enumerate(G):
    t = int(round((g["j"] - a.j0) / a.period)); t = min(max(t, 0), occ - 1); lag = g["j"] - (a.j0 + a.period * t)
    if abs(lag) > a.maxlag: continue
    sl = slice(g["rb0"], g["rb0"] + g["nrb"]); H = Hall[t, sl].transpose(0, 2, 1)  # rb rx P
    he = g["h"]
    # (b)
    best = None; allbest = []
    for name, perm in models:
        N1, N2 = name[0], name[1]; O1, O2 = C.O_of(N1, N2); W, lab = C.typeI_rank1(N1, N2, O1, O2); Wp = np.zeros_like(W); Wp[:, perm] = W
        s = C.scores(H, he, Wp); o = np.argsort(-s); allbest.append((float(s[o[0]]), name, lab[o[0]], float(s[o[1]]), s.size))
    allbest.sort(key=lambda r: -r[0]); b1 = allbest[0]
    # second best codeword of the winning model + best of other models
    second = max(allbest[0][3], allbest[1][0]) if len(allbest) > 1 else allbest[0][3]
    nc = int(sum(x[4] for x in allbest)); Wr = rng.normal(size=(nc, P)) + 1j * rng.normal(size=(nc, P)); Wr /= np.linalg.norm(Wr, axis=1, keepdims=True)
    null = float(C.scores(H, he, Wr).max())
    # shuffled h_eff: another grant of the list with same nrb window (different slot) -> same H, wrong channel
    other = G[(gi + 7) % len(G)]; nrb = g["nrb"]
    if other["nrb"] >= nrb:
        hs = other["h"][:nrb]; sh = max(float(C.scores(H, hs, np.where(True, np.zeros((1, P)), 0) + 0).max()) if False else 0, 0)
        shuf = 0.0
        for name, perm in models:
            N1, N2 = name[0], name[1]; O1, O2 = C.O_of(N1, N2); W, lab = C.typeI_rank1(N1, N2, O1, O2); Wp = np.zeros_like(W); Wp[:, perm] = W
            shuf = max(shuf, float(C.scores(H, hs, Wp).max()))
    else: shuf = float("nan")
    # (c) LS per PRG
    cres = {}
    for n in (2, 4, 8, 16, g["nrb"]):
        n = min(n, g["nrb"]); r_ = []; r_sh = []
        for r0 in range(0, g["nrb"] - n + 1, n):
            A = H[r0:r0 + n].reshape(n * nrx, P); y = he[r0:r0 + n].reshape(-1)
            # common scalar per PRG is absorbed by an unknown w; solve w = pinv(A) y; residual relative
            w = np.linalg.lstsq(A, y, rcond=1e-3)[0]; r_.append(np.linalg.norm(A @ w - y) / np.linalg.norm(y))
            if other["nrb"] >= r0 + n:
                ys = other["h"][r0:r0 + n].reshape(-1); ws = np.linalg.lstsq(A, ys, rcond=1e-3)[0]; r_sh.append(np.linalg.norm(A @ ws - ys) / np.linalg.norm(ys))
        cres[n] = (float(np.mean(r_)), float(np.mean(r_sh)) if r_sh else float("nan"))
    rows.append(dict(abs=g["abs"], rnti=g["rnti"], nrb=g["nrb"], lag=lag, b1=b1, second=second, null=null, shuf=shuf, c=cres))
    print("abs %d rnti %d nrb %3d lag %+4d | (b) best %.3f %s cw=%s | 2nd %.3f | null(best of %d random) %.3f | shuffled-heff best %.3f | (c) resid(prg2/4/8/16/all) %s vs shuffled %s" % (
        g["abs"], g["rnti"], g["nrb"], lag, b1[0], b1[1], b1[2], second, nc, null, shuf,
        "/".join("%.2f" % v[0] for v in cres.values()), "/".join("%.2f" % v[1] for v in cres.values())))
if rows:
    f = lambda k: np.nanmean([r[k] for r in rows])
    print("MEAN best %.3f second %.3f null %.3f shuffled %.3f over %d grants ; best>null in %d, best>shuffled in %d" % (f("b1") if False else np.mean([r["b1"][0] for r in rows]), f("second"), f("null"), f("shuf"), len(rows), sum(r["b1"][0] > r["null"] for r in rows), sum(r["b1"][0] > r["shuf"] for r in rows if r["shuf"] == r["shuf"])))
