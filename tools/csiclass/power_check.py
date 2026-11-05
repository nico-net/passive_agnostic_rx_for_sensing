#!/usr/bin/env python3
"""Positive control for method (b): if the gNB WERE applying a Type I rank-1 codeword to this measured H, would the test recover it?
Synthetic h_eff = H w_true + complex Gaussian noise at a given per-RB SNR (relative to |H w|^2 mean), same RB windows as real grants, full sweep over all models.
Success = the true (model, codeword) scores >= 0.98 * the best score. Also reports the score gap true vs best-of-random for the null."""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import codebook_match as C
H = np.load(sys.argv[1])["H"]; nrb_list = [int(x) for x in sys.argv[2].split(",")]; snrs = [float(x) for x in sys.argv[3].split(",")]; ntr = int(sys.argv[4]) if len(sys.argv) > 4 else 20; lagocc = int(sys.argv[5]) if len(sys.argv) > 5 else 0
occ, nrb_all, P, nrx = H.shape; models = C.models_from(P); rng = np.random.default_rng(5)
cb = {}
for name, perm in models:
    N1, N2 = name[0], name[1]; O1, O2 = C.O_of(N1, N2); W, lab = C.typeI_rank1(N1, N2, O1, O2); Wp = np.zeros_like(W); Wp[:, perm] = W; cb[name] = (Wp, lab)
for snr in snrs:
    for nrb in nrb_list:
        ok = 0; gap = []
        for _ in range(ntr):
            t = rng.integers(occ - lagocc); r0 = rng.integers(0, nrb_all - nrb + 1); Hs = H[t, r0:r0 + nrb].transpose(0, 2, 1)
            name = models[rng.integers(len(models))][0]; Wp, lab = cb[name]; ci = rng.integers(len(Wp)); y = np.einsum("rap,p->ra", Hs, Wp[ci]); Hm = H[t + lagocc, r0:r0 + nrb].transpose(0, 2, 1)
            nz = (rng.normal(size=y.shape) + 1j * rng.normal(size=y.shape)) / np.sqrt(2) * np.sqrt((np.abs(y) ** 2).mean() / 10 ** (snr / 10))
            he = y + nz
            true_s = C.scores(Hm, he, Wp[ci:ci + 1])[0]; best = max(float(C.scores(Hm, he, cb[n][0]).max()) for n, _ in models)
            ok += true_s >= 0.98 * best; gap.append(best - true_s)
        print("lag %d occasion(s): SNR %5.1f dB  nrb %3d : true codeword within 2%% of best in %d/%d trials" % (lagocc, snr, nrb, ok, ntr))
