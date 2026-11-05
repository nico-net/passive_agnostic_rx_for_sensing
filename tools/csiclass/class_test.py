#!/usr/bin/env python3
"""Class A (non-precoded, one port per antenna element/pol) vs class B (beamformed ports) test on a despread multi-port CSI-RS (extract_h.py output).
Test: spatial covariance R (ports x ports) over RBs, rx and occasions; fit its dominant eigenvectors with dual-pol UPA DFT steering vectors
[a(n1,n2) ; e^{j phi} a(n1,n2)] over every port-ordering/(N1,N2) hypothesis; compare with the same maximisation on randomly permuted ports (null)."""
import argparse, itertools, numpy as np
ap = argparse.ArgumentParser(); ap.add_argument("h"); ap.add_argument("--nperm", type=int, default=200); ap.add_argument("--nev", type=int, default=3)
ap.add_argument("--osf", type=int, default=8)
a = ap.parse_args()
H = np.load(a.h)["H"]  # (occ, rb, P, rx)
occ, nrb, P, nrx = H.shape
print("H shape", H.shape)
pw = (np.abs(H) ** 2).mean((0, 1, 3)); print("port power dB (rel. mean):", np.round(10 * np.log10(pw / pw.mean()), 1).tolist())
print("port power spread max-min dB: %.1f ; std %.2f dB" % (10 * np.log10(pw.max() / pw.min()), (10 * np.log10(pw)).std()))


def cov(Hx):
    X = Hx.transpose(2, 0, 1, 3).reshape(P, -1)  # ports x samples
    return X @ X.conj().T / X.shape[1]


def models(P):
    """(name, perm) perm[i] = list index of port for (pol, n1, n2) element ordering; returns (N1,N2,pol_order,perm_matrix)."""
    out = []
    half = P // 2
    for N1, N2 in [(half, 1), (half // 2, 2), (half // 4, 4), (2, half // 2), (1, half)]:
        if N1 * N2 != half: continue
        for pol in ("block", "interleave"):
            for fast in ("n1", "n2"):
                idx = np.zeros((2, N1, N2), int)
                for q in range(2):
                    for n1 in range(N1):
                        for n2 in range(N2):
                            e = (n1 + N1 * n2) if fast == "n1" else (n2 + N2 * n1)
                            idx[q, n1, n2] = (q * half + e) if pol == "block" else (2 * e + q)
                out.append(((N1, N2, pol, fast), idx))
    return out


def steer_bank(N1, N2, osf):
    f1 = np.arange(N1 * osf) / (N1 * osf); f2 = np.arange(N2 * osf if N2 > 1 else 1) / (N2 * osf if N2 > 1 else 1)
    a1 = np.exp(2j * np.pi * np.outer(np.arange(N1), f1))  # N1 x B1
    a2 = np.exp(2j * np.pi * np.outer(np.arange(N2), f2))
    A = np.einsum("ib,jc->ijbc", a1, a2).reshape(N1 * N2, -1) / np.sqrt(N1 * N2)  # (N1*N2, B)
    return A


def fit(u, idx, A):
    """u: eigenvector over ports (P). best |u^H [a; e^{jphi} a]|^2/|u|^2 over beams and co-phase."""
    N1, N2 = idx.shape[1:]
    u0 = u[idx[0].ravel(order="F") if False else idx[0].reshape(-1)]
    u1 = u[idx[1].reshape(-1)]
    # element order inside idx[q].reshape(-1) is (n1,n2) C-order == A rows (i*N2+j)
    c0 = A.conj().T @ u0; c1 = A.conj().T @ u1  # beams
    # max over phi of |c0 + e^{-j phi} c1|^2 /2 = (|c0|+|c1|)^2/2
    s = (np.abs(c0) + np.abs(c1)) ** 2 / 2
    return s.max() / (np.linalg.norm(u) ** 2), int(s.argmax())


def best_over_models(R, ms, banks, nev):
    w, V = np.linalg.eigh(R); w = w[::-1]; V = V[:, ::-1]
    res = []
    for name, idx in ms:
        A = banks[(idx.shape[1], idx.shape[2])]
        res.append((name, [fit(V[:, i], idx, A)[0] for i in range(nev)]))
    return w, res


ms = models(P)
banks = {(m[1].shape[1], m[1].shape[2]): steer_bank(m[1].shape[1], m[1].shape[2], a.osf) for m in ms}
R = cov(H)
w, res = best_over_models(R, ms, banks, a.nev)
print("eigenvalue fractions top5:", np.round(w[:5] / w.sum(), 3).tolist(), " noise-ish (median ev / top) %.3g" % (np.median(w) / w[0]))
res.sort(key=lambda r: -r[1][0])
print("top models (fit of eigvec 1..%d):" % a.nev)
for name, f in res[:6]: print(" ", name, np.round(f, 3).tolist())
rng = np.random.default_rng(1)
null = []
for _ in range(a.nperm):
    pm = rng.permutation(P); Rp = R[np.ix_(pm, pm)]
    _, rp = best_over_models(Rp, ms, banks, 1)
    null.append(max(r[1][0] for r in rp))
null = np.array(null); obs = res[0][1][0]
print("NULL (random port permutation, best over all %d models) eigvec1 fit: mean %.3f p95 %.3f max %.3f ; observed %.3f -> %s" % (
    len(ms), null.mean(), np.percentile(null, 95), null.max(), obs, "STRUCTURED (class A like)" if obs > null.max() else "not distinguishable from null"))
# stability: halves of the occasions and of the band
for tag, sl in (("occ first half", H[: occ // 2]), ("occ second half", H[occ // 2:]), ("RB low half", H[:, : nrb // 2]), ("RB high half", H[:, nrb // 2:])):
    w2, r2 = best_over_models(cov(sl), ms, banks, 1); r2.sort(key=lambda r: -r[1][0])
    print("  %-16s best model %s fit %.3f" % (tag, r2[0][0], r2[0][1][0]))
# per-sample (single RB, single occasion) compaction: fraction of top-eigvec of the local 32x32 (rank<=4) captured by best model of the global winner


def omp_resid(R, idx, A, Ks=(2, 4, 6, 8)):
    """Greedy atom selection on R with single-pol UPA DFT atoms; returns residual energy fraction after K atoms."""
    N1, N2 = idx.shape[1:]; B = A.shape[1]
    atoms = np.zeros((P, 2 * B), complex)
    for q in range(2): atoms[idx[q].reshape(-1), q * B:(q + 1) * B] = A
    tot = np.trace(R).real; sel = []; out = {}
    for K in range(1, max(Ks) + 1):
        Rm = R if not sel else (lambda Q: (np.eye(P) - Q) @ R @ (np.eye(P) - Q))(
            (lambda S: S @ np.linalg.pinv(S))(atoms[:, sel]))
        sc = np.einsum("ij,ik,kj->j", atoms.conj(), Rm, atoms).real
        sc[sel] = -1; sel.append(int(sc.argmax()))
        if K in Ks:
            S = atoms[:, sel]; Q = S @ np.linalg.pinv(S); Rr = (np.eye(P) - Q) @ R @ (np.eye(P) - Q)
            out[K] = float(np.trace(Rr).real / tot)
    return out


print("sparse-atom OMP residual (fraction of energy left after K single-pol DFT atoms) for the top models:")
for name, _ in res[:4]:
    idx = [m[1] for m in ms if m[0] == name][0]
    print("  ", name, {k: round(v, 3) for k, v in omp_resid(R, idx, banks[(idx.shape[1], idx.shape[2])]).items()})
rn = []
for _ in range(20):
    pm = rng.permutation(P); Rp = R[np.ix_(pm, pm)]
    idx = res[0][0]; idx = [m[1] for m in ms if m[0] == idx][0]
    rn.append(omp_resid(Rp, idx, banks[(idx.shape[1], idx.shape[2])])[8])
print("   null (permuted ports, top model, K=8): mean residual %.3f min %.3f" % (np.mean(rn), np.min(rn)))
