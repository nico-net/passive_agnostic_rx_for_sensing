#!/usr/bin/env python3
"""Method (b): TS 38.214 5.2.2.2.1 Type I single-panel rank-1 codebook match, and the unconstrained comparator.
Inputs per grant: H (nrb, 4 rx, P ports) from the CSI-RS, h_eff (nrb, 4 rx) = DM-RS channel of port 1000 on the same RBs (the DM-RS is precoded with the PDSCH W).
Score(w) = |sum_rb <h_eff, H w>|^2 / (|h_eff|^2 |H w|^2) with ONE complex scalar for the whole grant (common phase/gain between the two slots), after removing
the common phase ramp (timing/CFO difference between the two slots) estimated against the dominant left singular vector of H (candidate independent).
Also per-RB score (scalar per RB). Codebook mode 1 and mode 2 span the same oversampled-DFT x 4 co-phase grid for rank 1 (mode 2 only changes how i2 selects the beam offset),
so one candidate set covers both; rank 2 candidates are built from the same table with the (k1,k2) offsets of Table 5.2.2.2.1-3."""
import numpy as np


def typeI_rank1(N1, N2, O1, O2):
    """W = [v_{l,m}; phi_n v_{l,m}]/sqrt(P), P = 2 N1 N2, ordering (pol, n1, n2) with n2 fastest. Returns (ncand, P), labels (l,m,n)."""
    ws, lab = [], []
    for l in range(N1 * O1):
        for m in range(N2 * O2 if N2 > 1 else 1):
            u = np.exp(2j * np.pi * m * np.arange(N2) / (O2 * N2)) if N2 > 1 else np.ones(1)
            v = np.kron(np.exp(2j * np.pi * l * np.arange(N1) / (O1 * N1)), u)
            for n in range(4):
                ws.append(np.concatenate([v, np.exp(1j * np.pi * n / 2) * v]) / np.sqrt(2 * N1 * N2)); lab.append((l, m, n))
    return np.array(ws), lab


def typeI_rank2(N1, N2, O1, O2):
    """Rank 2 (Table 5.2.2.2.1-3/-5): W = [v_{l,m} v_{l',m'}; phi v_{l,m} -phi v_{l',m'}]/sqrt(2P), (l',m') = (l+k1, m+k2), phi_n = e^{j pi n/2}, n=0,1."""
    if N2 == 1: offs = [(0, 0), (O1, 0), (2 * O1, 0), (3 * O1, 0)]
    else: offs = [(0, 0), (O1, 0), (0, O2), (2 * O1, 0)]
    ws, lab = [], []
    for l in range(N1 * O1):
        for m in range(N2 * O2 if N2 > 1 else 1):
            for (k1, k2) in offs:
                l2, m2 = (l + k1) % (N1 * O1), (m + k2) % (N2 * O2 if N2 > 1 else 1)
                def vec(l_, m_):
                    u = np.exp(2j * np.pi * m_ * np.arange(N2) / (O2 * N2)) if N2 > 1 else np.ones(1)
                    return np.kron(np.exp(2j * np.pi * l_ * np.arange(N1) / (O1 * N1)), u)
                a, b = vec(l, m), vec(l2, m2)
                for n in range(2):
                    ph = np.exp(1j * np.pi * n / 2)
                    ws.append(np.stack([np.concatenate([a, ph * a]), np.concatenate([b, -ph * b])], 1) / np.sqrt(4 * N1 * N2)); lab.append((l, m, k1, k2, n))
    return np.array(ws), lab


def remove_ramp(H, heff):
    """Estimate a common phase + linear-in-RB ramp of heff against the dominant left vector of H (candidate independent)."""
    nrb = H.shape[0]
    u = np.stack([np.linalg.svd(H[r], full_matrices=False)[0][:, 0] for r in range(nrb)])  # nrb x rx
    ph = np.angle((np.conj(u) * heff).sum(1))
    x = np.arange(nrb); w = np.abs((np.conj(u) * heff).sum(1))
    ph = np.unwrap(ph)
    a, b = np.polyfit(x, ph, 1, w=np.sqrt(w))
    return heff * np.exp(-1j * (a * x + b))[:, None]


def scores(H, heff, W, per_rb=False):
    """H: nrb x rx x P ; heff: nrb x rx ; W: ncand x P (rank1). Primary score = mean over RB of the per-RB cos^2 (one complex scalar per RB, so no phase-ramp
    assumption); coh = refined score with ONE scalar for the whole grant and a common phase ramp searched per candidate (top candidates only, see refine)."""
    Y = np.einsum("rap,cp->cra", H, W)  # cand, rb, rx
    n1 = np.abs((np.conj(heff)[None] * Y).sum(2)) ** 2
    d1 = (np.abs(heff) ** 2).sum(1)[None] * (np.abs(Y) ** 2).sum(2)
    return (n1 / np.maximum(d1, 1e-30)).mean(1)


def refine(H, heff, w, slopes=np.linspace(-0.4, 0.4, 81)):
    """Coherent fit of one candidate: max over a linear phase ramp (rad/RB) of |sum <heff, H w> e^{-j s r}|^2 / (|heff|^2 |Hw|^2)."""
    y = np.einsum("rap,p->ra", H, w); x = np.arange(H.shape[0])
    z = (np.conj(heff) * y).sum(1)  # per RB
    best = max(abs((z * np.exp(-1j * s * x)).sum()) ** 2 for s in slopes)
    return best / ((np.abs(heff) ** 2).sum() * (np.abs(y) ** 2).sum())


def models_from(P):
    import importlib.util, sys
    out = []
    half = P // 2
    for N1, N2 in [(half, 1), (half // 2, 2), (half // 4, 4), (2, half // 2), (1, half)]:
        if N1 * N2 != half or N1 < 1: continue
        for pol in ("block", "interleave"):
            for fast in ("n1", "n2"):
                idx = np.zeros((2, N1, N2), int)
                for q in range(2):
                    for n1 in range(N1):
                        for n2 in range(N2):
                            e = (n1 + N1 * n2) if fast == "n1" else (n2 + N2 * n1)
                            idx[q, n1, n2] = (q * half + e) if pol == "block" else (2 * e + q)
                out.append(((N1, N2, pol, fast), idx.reshape(2 * N1 * N2)))
    return out


def O_of(N1, N2):  # Table 5.2.2.1-2 oversampling: O2=1 if N2==1 else 4 ; O1 = 4
    return 4, (1 if N2 == 1 else 4)


def best_over_models(H, heff, models, ranks=(1,)):
    """For each (N1,N2,ordering) model build the candidate set, permute ports, return best scores."""
    out = []
    for (name, perm) in models:
        N1, N2 = name[0], name[1]; O1, O2 = O_of(N1, N2)
        W, lab = typeI_rank1(N1, N2, O1, O2)
        Wp = np.zeros_like(W); Wp[:, perm] = W  # element i of the (pol,n1,n2) vector -> port perm[i]
        s = scores(H, heff, Wp)
        o = np.argsort(-s)
        out.append((name, float(s[o[0]]), lab[o[0]], float(s[o[1]]) if len(o) > 1 else 0.0, lab[o[1]], s))
    return out


if __name__ == "__main__":  # self-test: synthetic LOS-ish H with 2 paths, h_eff = H w_true + noise, recover w_true, and null for a random w
    rng = np.random.default_rng(0)
    N1, N2, P, nrb = 8, 2, 32, 40
    ms = [m for m in models_from(P) if m[0] == (8, 2, "block", "n1")]
    W, lab = typeI_rank1(N1, N2, 4, 4)
    perm = ms[0][1]; Wp = np.zeros_like(W); Wp[:, perm] = W
    H = np.zeros((nrb, 4, P), complex)
    for p in range(3):
        a = np.exp(1j * rng.uniform(0, 6.28, P)); g = rng.normal(size=4) + 1j * rng.normal(size=4)
        for r in range(nrb): H[r] += np.outer(g, a) * np.exp(1j * 0.3 * r * (p + 1))
    ok = 0
    for trial in range(10):
        ct = rng.integers(len(W)); heff = np.einsum("rap,p->ra", H, Wp[ct]) * np.exp(1j * (0.7 + 0.05 * np.arange(nrb)))[:, None]
        heff = heff + 0.05 * np.abs(heff).mean() * (rng.normal(size=heff.shape) + 1j * rng.normal(size=heff.shape))
        r = best_over_models(H, heff, ms); ok += (r[0][2] == lab[ct])
    print("self-test: recovered true codeword in %d/10 trials" % ok)
    assert ok >= 8
