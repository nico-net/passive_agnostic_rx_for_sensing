#!/usr/bin/env python3
"""Offline PDSCH DM-RS (type 1, port 1000, comb-even, CRB0-relative sequence index m = k/2) identity scan on a raw capture slot set.
For every N_ID in 0..65535 and n_SCID in {0,1} (TS 38.211 7.4.1.1.1) the adjacent-DM-RS-RE phase coherence of y*conj(r) is evaluated in 12-RB windows; a repeated UE shows
a single (N_ID, SCID) far above the rest. Output: ranked hypotheses."""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import csilib as L

def gold_bits(cinit, nbits):
    x1 = np.zeros(nbits + 1600 + 31, np.uint8); x1[0] = 1
    for i in range(nbits + 1600): x1[i + 31] = x1[i + 3] ^ x1[i]
    x = cinit.astype(np.uint32) & np.uint32(0x7FFFFFFF)
    out = np.empty((cinit.size, nbits), np.uint8)
    for n in range(nbits + 1600):
        if n >= 1600: out[:, n - 1600] = (x & 1).astype(np.uint8) ^ x1[n]
        nb = ((x ^ (x >> 1) ^ (x >> 2) ^ (x >> 3)) & 1)
        x = (x >> 1) | (nb << 30)
    return out

def dmrs_seq(nid, scid, ns, l, nm):
    cinit = ((np.uint64(1 << 17) * np.uint64(14 * ns + l + 1) * (2 * nid.astype(np.uint64) + 1) + 2 * nid.astype(np.uint64) + np.uint64(scid)) % np.uint64(1 << 31))
    c = gold_bits(cinit, 2 * nm).astype(np.float32)
    return ((1 - 2 * c[:, 0::2]) + 1j * (1 - 2 * c[:, 1::2])) / np.sqrt(2)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("cap"); ap.add_argument("coh"); ap.add_argument("--j0", type=int, required=True); ap.add_argument("--period", type=int, default=160)
    ap.add_argument("--ns", type=int, required=True); ap.add_argument("--l", type=int, default=2); ap.add_argument("--nocc", type=int, default=6); ap.add_argument("--maxnid", type=int, default=65536)
    a = ap.parse_args()
    sl, meta = L.open_slotter(a.cap, a.coh)
    nm = 6 * L.NRB; W = 72
    ys = []
    for p in range(2, 2 + a.nocc):
        Y = sl.slot(a.j0 + a.period * p, [a.l])[:, 0]
        ys.append(Y[:, 0:2 * nm:2])  # DM-RS REs k = 2m, (rx, nm)
    best = []
    for scid in (0, 1):
        for n0 in range(0, a.maxnid, 8192):
            nid = np.arange(n0, min(a.maxnid, n0 + 8192))
            r = dmrs_seq(nid, scid, a.ns, a.l, nm)  # cand, nm
            st = np.zeros((nid.size, nm // W))
            for y in ys:
                z = (y[None] * np.conj(r)[:, None, :]).sum(1)  # cand, nm
                q = z[:, 1:] * np.conj(z[:, :-1]); nrm = np.abs(z[:, 1:]) * np.abs(z[:, :-1])
                for w in range(nm // W):
                    sl_ = slice(w * W, min((w + 1) * W, nm - 1))
                    st[:, w] += np.abs(q[:, sl_].sum(1)) / np.maximum(nrm[:, sl_].sum(1), 1e-30) / a.nocc
            sc = st.max(1)
            for i in np.argsort(-sc)[:3]: best.append((float(sc[i]), int(nid[i]), scid, int(st[i].argmax())))
    best.sort(reverse=True)
    print("top hypotheses (stat, nid, scid, best window of 12 RB):"); [print(" ", b) for b in best[:8]]
    print("median-ish of tops:", np.median([b[0] for b in best]))
