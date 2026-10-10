#!/usr/bin/env python3
"""UL physics census for the passive receiver (UL-DBG).  Offline only; reads the 4-channel sc16 raw
captures through np.memmap (never loads a capture into RAM).

Conventions (receiver-derived, see docs/superpowers/reports/csd/ULDBG.md):
  * 122.88 MS/s, mu=1, FFT 4096, CP 288 (352 for symbol 0 of every half-subframe = every slot here).
  * Carrier: 217 PRB centred on the capture centre; carrier PRB k == CRB k (validated by PDSCH DM-RS).
  * F0 = sample index of a frame start (slot 0) in the capture, from the receiver's sync
    ("Resynchronizing RX by <F0>") -- validated by PDSCH DM-RS with ns = slot.
  * UL symbol window: DL timing - S (S=1600 = N_TA_offset 25600 Tc), FFT started 144 samples inside
    the CP; the residual delay appears as the DM-RS CIR position tau.
"""
import os, re, sys, numpy as np
from functools import lru_cache

FS = 122880000; SLOT = 61440; FFT = 4096; CP = 288; CP0 = 352; FRAME = 1228800; NPRB = 217
CAPS = {'b': ('/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b', 302892),
        'o': ('/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008', 301934)}
_mm = {}
BINS = (np.arange(NPRB * 12) - NPRB * 6) % FFT


def mm(cap, ch):
    if (cap, ch) not in _mm:
        _mm[(cap, ch)] = np.memmap(os.path.join(CAPS[cap][0], 'rx%d.sc16' % ch), dtype=np.int16, mode='r')
    return _mm[(cap, ch)]


def read(cap, ch, start, n):
    a = np.asarray(mm(cap, ch)[2 * start:2 * (start + n)], dtype=np.float32)
    return a[0::2] + 1j * a[1::2]


def sym_start(l):
    """CP start of symbol l inside a slot (mu=1: every slot starts a half-subframe)."""
    return l * (FFT + CP) + (CP0 - CP if l > 0 else 0)


def grid(x, l, back=144):
    o = sym_start(l) + (CP0 if l == 0 else CP) - back
    return np.fft.fft(x[o:o + FFT])[BINS]


def gold(cinit, n):
    Nc = 1600; L = n + Nc + 31
    x1 = np.zeros(L, np.uint8); x2 = np.zeros(L, np.uint8); x1[0] = 1
    for i in range(31): x2[i] = (cinit >> i) & 1
    for i in range(L - 31):
        x1[i + 31] = x1[i + 3] ^ x1[i]
        x2[i + 31] = x2[i + 3] ^ x2[i + 2] ^ x2[i + 1] ^ x2[i]
    return x1[Nc:Nc + n] ^ x2[Nc:Nc + n]


@lru_cache(maxsize=4096)
def dmrs(nid, ns, l, nscid=0):
    """TS 38.211 6.4.1.1.1.1 CP-OFDM DM-RS (same generator as PDSCH): one value per type-1 comb RE,
    index m = 6*CRB + j."""
    cinit = ((1 << 17) * (14 * ns + l + 1) * (2 * nid + 1) + 2 * nid + nscid) % (1 << 31)
    c = gold(cinit, 2 * 6 * 275).astype(np.float32)
    return ((1 - 2 * c[0::2]) + 1j * (1 - 2 * c[1::2])) / np.sqrt(2)


def slot_iq(cap, fr, slot, S=1600, F0=None):
    F0 = CAPS[cap][1] if F0 is None else F0
    base = F0 + fr * FRAME + slot * SLOT - S
    return [read(cap, ch, base, SLOT + FFT) for ch in range(4)]


def dmrs_est(cap, fr, slot, p0, p1, syms=(2, 11), comb=0, nid=415, S=1600, taus=np.arange(-60, 260, 4.0)):
    """Per-antenna DM-RS SNR (linear) on PRBs [p0,p1) with a delay search.  Returns (snr[4], tau)."""
    xs = slot_iq(cap, fr, slot, S)
    ks = np.arange(p0 * 12 + comb, p1 * 12, 2); m = ks // 2
    Z = np.array([np.array([grid(x, l)[ks] for x in xs]) * np.conj(dmrs(nid, slot, l)[m])[None, :] for l in syms])
    best = (-1, 0)
    for tau in taus:
        s = (np.abs((Z * np.exp(2j * np.pi * ks * tau / FFT)).reshape(len(syms), 4, -1, 6).sum(-1)) ** 2).sum()
        if s > best[0]: best = (s, tau)
    tau = best[1]; Zc = (Z * np.exp(2j * np.pi * ks * tau / FFT)).reshape(len(syms), 4, -1, 6)
    H = Zc.mean(-1, keepdims=True); res = Zc - H
    nv = (np.abs(res) ** 2).sum((-1, -2)) / (res.shape[2] * 5)
    sp = (np.abs(H[..., 0]) ** 2).mean(-1) - nv / 6
    return (np.maximum(sp, 1e-6 * nv) / nv).mean(0), tau


# ---- DCI 0_1 layout inferred from physics (ULDBG.md): id(1) bwp(1) FDRA(15, N=216, start 1) TDRA(4)
#      MCS(5) NDI(1) RV(2) HARQ(4) DAI(2) TPC(2) ... UL-SCH(1); TDRA row 0 -> k2=2, S=0, L=14 ----
def bits(h, n):
    v = int(h, 16); return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def field(b, a, n):
    return int(''.join(map(str, b[a:a + n])), 2)


@lru_cache(maxsize=64)
def rivtab(N):
    t = {}
    for L in range(1, N + 1):
        for s in range(0, N - L + 1):
            t[(N * (L - 1) + s) if (L - 1) <= N // 2 else (N * (N - L + 1) + (N - 1 - s))] = (s, L)
    return t


def parse_dcis(log):
    t = re.sub(r'\x1b\[[0-9;]*m', '', open(log, errors='ignore').read())
    D = [(int(a), int(r, 16), int(l), p) for a, r, l, p in re.findall(
        r'ULDBG_DCI abs=(\d+) f=\d+ s=\d+ rnti=0x([0-9a-f]+) fmt=\d+ len=(\d+) .*? payload=0x([0-9a-f]+)', t)]
    ts = {int(a): int(b) for a, b in re.findall(r'ULDBG_TS abs_slot=(\d+) ts=(\d+)', t)}
    return sorted({(d[0], d[1]): d for d in D}.values()), ts


def frame_offset(ts, F0):
    """abs_frame - capture_frame, from ULDBG_TS (ts = capture position + 4448: the UE reads the first
    symbol of the next frame at the end of the previous one)."""
    d = [a // 20 - int(round((s - 4448 - F0) / FRAME)) for a, s in ts.items()]
    v, c = np.unique(d, return_counts=True)
    return int(v[np.argmax(c)])


def decode(d, N=216, bs=1, k2=2):
    a, rnti, ln, p = d
    b = bits(p, ln); r = rivtab(N).get(field(b, 2, 15))
    if r is None: return None
    return dict(abs=a, rnti=rnti, len=ln, p0=bs + r[0], L=r[1], tdra=field(b, 17, 4), mcs=field(b, 21, 5),
                ndi=b[26], rv=field(b, 27, 2), harq=field(b, 29, 4), target=a + k2)
