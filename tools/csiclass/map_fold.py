#!/usr/bin/env python3
"""CSIRS-CLASS step 1: blind periodic-structure map of a raw 4-RX capture (no ID, no row, no alignment assumed).
Reuses tools/csirs_capture_map.py (CP timing, CFO, slotter). Folds slots mod 160 (relative index, symbol grid may be slipped by whole symbols;
the sequence match in resolve.py fixes that). Output npz: P mean power/RE, C coherence between consecutive 160-slot periods, N norm."""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import csirs_capture_map as M

ap = argparse.ArgumentParser()
ap.add_argument("cap"); ap.add_argument("--out", required=True)
ap.add_argument("--max-periods", type=int, default=1000)
a = ap.parse_args()
cap = M.Cap(a.cap, 4)
tA, cfoA, qA = M.timing(cap, 0)
es = cap.n - 42 * M.SLOT
tB, cfoB, qB = M.timing(cap, es - es % M.SLOT)
nsl = (cap.n - 2 * M.SLOT) // M.SLOT
d = ((tB - tA + M.SLOT // 2) % M.SLOT) - M.SLOT // 2
drift = d / nsl; cfo = 0.5 * (cfoA + cfoB)
print(f"t0={tA} q={qA:.1f}/{qB:.1f} drift={drift*1e3:.4f}/kslot cfo={cfoA:.0f}/{cfoB:.0f}", flush=True)
sl = M.Slotter(cap, tA, cfo, drift)
nper = min(a.max_periods, nsl // 160 - 1)
P = np.zeros((160, 14, M.NSC)); C = np.zeros_like(P); N = np.zeros_like(P); prev = [None] * 160
for p in range(nper):
    for ph in range(160):
        Y = sl.slot(p * 160 + ph)
        if Y is None: continue
        pw = (np.abs(Y) ** 2).sum(0); P[ph] += pw
        if prev[ph] is not None:
            Yp, pwp = prev[ph]
            C[ph] += np.abs((Y * np.conj(Yp)).sum(0)); N[ph] += np.sqrt(pw * pwp)
        prev[ph] = (Y, pw)
    if p % 5 == 0: print("period", p, nper, flush=True)
np.savez_compressed(a.out, P=P / nper, C=C, N=N, meta=json.dumps(dict(t0=int(tA), cfo=cfo, drift=drift, nper=nper, cap=a.cap)))
