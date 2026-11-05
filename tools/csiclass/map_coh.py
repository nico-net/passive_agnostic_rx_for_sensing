#!/usr/bin/env python3
"""CSIRS-CLASS step 1b: blind detector of anything that repeats with period dividing 160 slots, robust to a spatially-coherent
signal (plain per-RE antenna-summed coherence is ~0.8 everywhere because the 4-RX vector of any QPSK RE is coherent with itself).
z_p(re) = sum_ant Y_p conj Y_{p-1} (consecutive 160-slot periods). Data REs: z has random phase -> |sum over WIN RBs of z| ~ sqrt(WIN)*|z|.
Repeated deterministic REs (CSI-RS/TRS/SSB/DM-RS of repeated grants): phases agree -> ~ WIN*|z|. T = mean_p |sum_win z| / sqrt(sum_win |z|^2):
noise ~0.89, present up to sqrt(#REs in window) (we sum only over RBs of one subcarrier class k mod 12, WIN RBs)."""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import csirs_capture_map as M
WIN = 16
ap = argparse.ArgumentParser(); ap.add_argument("cap"); ap.add_argument("--out", required=True)
a = ap.parse_args()
cap = M.Cap(a.cap, 4)
def sync(cap, npts=5):
    """CP timing at npts points, unwrapped modulo one symbol (the CP fold cannot tell symbols apart)."""
    SYMP = M.NFFT + M.CP
    last = cap.n - 42 * M.SLOT; starts = [int(i * last / (npts - 1)) // M.SLOT * M.SLOT for i in range(npts)]
    res = [M.timing(cap, st) for st in starts]
    ts = [res[0][0]]
    for i in range(1, npts):
        d = ((res[i][0] - ts[-1] % M.SLOT + SYMP // 2) % SYMP) - SYMP // 2  # step of this interval, modulo one symbol
        ts.append(ts[-1] + d)
    x = np.array(starts, float); y = np.array(ts, float)
    drift = np.polyfit(x / M.SLOT, y, 1)[0]  # samples per slot
    return int(ts[0]) % M.SLOT, float(np.mean([r[1] for r in res])), drift, [r[2] for r in res], ts
tA, cfo, drift, qs, ts = sync(cap)
print("sync t0", tA, "drift samples/kslot %.4f" % (drift * 1e3), "cfo %.1f" % cfo, "q", np.round(qs, 1), "ts", ts, flush=True)
nsl = (cap.n - 2 * M.SLOT) // M.SLOT
qA, qB = qs[0], qs[-1]
sl = M.Slotter(cap, tA, cfo, drift)
nper = nsl // 160 - 1
nw = M.NRB // WIN  # 13 windows (last 9 RB dropped)
num = np.zeros((160, 14, 12, nw)); den = np.zeros_like(num); cnt = 0
prev = [None] * 160
for p in range(nper):
    for ph in range(160):
        Y = sl.slot(p * 160 + ph)
        if prev[ph] is not None:
            z = (Y * np.conj(prev[ph])).sum(0)[:, :nw * WIN * 12].reshape(14, nw, WIN, 12)
            num[ph] += np.abs(z.sum(2)).transpose(0, 2, 1)
            den[ph] += np.sqrt((np.abs(z) ** 2).sum(2)).transpose(0, 2, 1)
        prev[ph] = Y
    if p % 5 == 0: print("period", p, nper, flush=True)
np.savez_compressed(a.out, T=num / np.maximum(den, 1e-12), meta=json.dumps(dict(t0=int(tA), cfo=cfo, drift=drift, nper=nper, win=WIN, cap=a.cap, q=[qA, qB])))
