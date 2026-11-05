#!/usr/bin/env python3
"""Method (c) feasibility from the CSI-RS channel alone: per-RB H (rx x ports) -> noise (occasion-to-occasion difference after removing a common scalar),
SNR, singular spectrum per RB and of the stacked PRG system [H(rb1);...;H(rbn)] (4n x P), and the fraction of a random/codebook-like W that is identifiable
(energy of W inside the span of right singular vectors with sigma^2 > 4*noise). W has P*Nl unknowns per PRG; each RB gives 4*Nl equations."""
import argparse, numpy as np
ap = argparse.ArgumentParser(); ap.add_argument("h"); ap.add_argument("--prgs", default="2,4,8,16,217")
a = ap.parse_args()
H = np.load(a.h)["H"]  # occ rb P rx
occ, nrb, P, nrx = H.shape
Hm = H.transpose(0, 1, 3, 2)  # occ rb rx P
# noise: consecutive occasions, common scalar per occasion pair and per RB (timing slope across RB removed by per-RB scalar)
d = []; sp = []
for t in range(occ - 1):
    A = Hm[t].reshape(nrb, -1); B = Hm[t + 1].reshape(nrb, -1)
    c = (np.conj(B) * A).sum(1) / np.maximum((np.abs(B) ** 2).sum(1), 1e-30); c = c / np.maximum(np.abs(c), 1e-30)
    d.append(np.abs(A - B * c[:, None]) ** 2); sp.append((np.abs(A) ** 2))
n0 = float(np.mean(d) / 2); s0 = float(np.mean(sp))  # per RE: noise var of one estimate = mean|A-B|^2 /2
print("per-element (rx,port) H power %.3g, noise-ish (static-channel difference) %.3g -> SNR %.1f dB (upper bound on noise: the channel also changes)" % (s0, n0, 10 * np.log10(s0 / max(n0, 1e-30))))
sv = np.linalg.svd(Hm[occ // 2], compute_uv=False)  # rb, 4
sv2 = (sv ** 2).mean(0); print("per-RB singular values^2 (mean over RB) rel. to noise*P: ", np.round(10 * np.log10(sv2 / (n0 * P)), 1).tolist(), "dB  (sigma1^2/sigma_k^2:", np.round(sv2[0] / sv2, 1).tolist(), ")")
t0 = occ // 2
for n in [int(x) for x in a.prgs.split(",")]:
    n = min(n, nrb); fr = []; cond = []; rk = []
    for r0 in range(0, nrb - n + 1, max(n, 1)):
        S = Hm[t0, r0:r0 + n].reshape(n * nrx, P)
        u, s, vh = np.linalg.svd(S, full_matrices=False)
        thr = 4 * n0 * 1.0  # sigma^2 > 4*noise var (each singular value of an n*4 x P noisy matrix: noise floor ~ n0*(sqrt(P)+sqrt(4n))^2; use that)
        floor = n0 * (np.sqrt(P) + np.sqrt(n * nrx)) ** 2
        good = s ** 2 > 2 * floor
        k = int(good.sum()); rk.append(k)
        cond.append(s[0] / s[max(k - 1, 0)] if k else np.inf)
        fr.append(k / P)  # a random W (isotropic in C^P) has k/P of its energy in the identifiable subspace
    print("PRG %3d RB: unknowns/layer %d, equations/layer %d | usable rank (sigma^2 > 2*noise floor) median %d of %d | identifiable fraction of an isotropic W %.2f | cond(usable) median %.1f" % (
        n, P, n * nrx, int(np.median(rk)), min(P, n * nrx), np.mean(fr), np.median(cond)))
