#!/usr/bin/env python3
"""Stability of the dominant spatial structure of a despread multi-port CSI-RS: per-occasion and per-RB-block best DFT beam (model fixed to the winner of class_test.py),
and port-phase stability relative to port 0 (|mean_t h_p conj(h_0)| / mean_t |h_p h_0|) on the dominant eigenvector."""
import sys, numpy as np
sys.path.insert(0, __import__("os").path.dirname(__file__)); import codebook_match as C
H = np.load(sys.argv[1])["H"]; model = eval(sys.argv[2]); occ, nrb, P, nrx = H.shape
ms = [m for m in C.models_from(P) if m[0] == model]; name, perm = ms[0]; N1, N2 = name[0], name[1]
O1, O2 = C.O_of(N1, N2); W, lab = C.typeI_rank1(N1, N2, O1, O2); Wp = np.zeros_like(W); Wp[:, perm] = W
def dom(Hs):  # dominant right vector (ports) of the stacked data
    X = Hs.transpose(2, 0, 1, 3).reshape(P, -1); u = np.linalg.eigh(X @ X.conj().T)[1][:, -1]; return u
def beam(u):
    c = np.abs(Wp.conj() @ u) ** 2 / np.linalg.norm(u) ** 2; i = int(c.argmax()); return lab[i][:2], float(c[i])
print("model", name, "beam grid", N1 * O1, "x", (N2 * O2 if N2 > 1 else 1))
res = [beam(dom(H[t:t + 10]))[0] for t in range(0, occ - 9, 10)]
print("per 10-occasion block (0.4 s) best beam (l,m):", res)
res2 = [beam(dom(H[:, r:r + 36]))[0] for r in range(0, nrb - 35, 36)]
print("per 36-RB block best beam (l,m):", res2)
u = dom(H); print("whole-capture best beam", beam(u))
# port-phase stability: phase of dominant-vector entries per occasion block vs whole
ph = []
for t in range(0, occ - 9, 10):
    ub = dom(H[t:t + 10]); ph.append(np.angle(ub * np.conj(ub[0])))
ph = np.array(ph); R = np.abs(np.exp(1j * ph).mean(0))
print("port-phase circular concentration across 0.4 s blocks (1 = identical every block): min %.3f median %.3f" % (R.min(), np.median(R)))
