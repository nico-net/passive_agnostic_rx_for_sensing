#!/usr/bin/env python3
"""dedicated_dmrs_census.py CAPDIR --ssb-mhz F --pointa-hz F --nrb N --pci P [--cfo HZ] [--nchunks 16]
Is there UE-specific PDSCH traffic? Per slot of the 20 ms burst frame and of the next frame: coherent PDSCH DM-RS correlation (type 1, nSCID 0 and 1, scrambling id = PCI,
reference = Point A, even and odd sub-carrier combs = CDM groups 0/1, all symbols 2..13), blocks of 4 RB, vs the same statistic of 24 other (null) PCIs.
Output: per slot the fraction of chunks whose statistic exceeds null mean + 5 sigma, and the number of slots/chunks lit. Offline, no receiver.
shortcut: flat channel per 4 RB block, timing slope searched on a coarse grid, 30 kHz SCS, 122.88 MS/s captures."""
import sys, os, argparse, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import cochannel as cc, ssb_search2 as S
ap = argparse.ArgumentParser(); ap.add_argument('cap'); ap.add_argument('--ssb-mhz', type=float, required=True); ap.add_argument('--pointa-hz', type=float, required=True)
ap.add_argument('--nrb', type=int, required=True); ap.add_argument('--pci', type=int, required=True); ap.add_argument('--cfo', type=float, default=-4800.0); ap.add_argument('--nchunks', type=int, default=16)
o = ap.parse_args(); cap = S.Cap(o.cap); nch = min(o.nchunks, cap.n // cc.CH - 1)
T, F, t0, ib = cc.frame_timing(cap, (o.ssb_mhz - cap.c) * 1e6 - 30e3, o.pci, nch)
g = cc.Grid(cap, o.pointa_hz, o.cfo, nsc=12 * o.nrb); nrb = o.nrb - o.nrb % 4
rng = np.random.RandomState(1); null = [int(x) for x in rng.choice([p for p in range(1008) if p != o.pci], 24, replace=False)]
taus = np.arange(-144, 145, 48); kk = np.arange(12 * nrb)
_C = {}
def seq(pci, n, l, scid):
    k = (pci, n % 20, l, scid)
    if k in _C: return _C[k]
    ci = ((1 << 17) * (14 * (n % 20) + l + 1) * (2 * pci + 1) + 2 * pci + scid) % (1 << 31); c = S.gold(ci, 12 * nrb); _C[k] = ((1 - 2 * c[0::2]) + 1j * (1 - 2 * c[1::2])) / np.sqrt(2); return _C[k]
res = np.zeros((nch, 40, 1 + len(null)))
for c in range(nch):
    for n in range(40):
        stat = np.zeros(1 + len(null))
        for l in range(2, 14):
            a0 = c * cc.CH + 16 * T[c] + cc.body(n, l)
            if a0 + 5000 > cap.n: continue
            Y = np.stack([g.sym(a0, ch) for ch in range(cap.nch)])[:, :12 * nrb]
            for comb in (0, 1):
                idx = np.arange(comb, 12 * nrb, 2)[: 6 * nrb]; Yd = Y[:, idx]
                for scid in (0, 1):
                    for pi, p in enumerate([o.pci] + null):
                        r = seq(p, n, l, scid)[: len(idx)]
                        best = 0
                        for tt in taus:
                            z = (np.conj(r)[None] * Yd * np.exp(2j * np.pi * tt * idx[None] / cc.N)).reshape(cap.nch, nrb // 4, -1).sum(2)
                            best = max(best, (np.abs(z) ** 2).sum())
                        stat[pi] += best
        res[c, n] = stat
mu = res[:, :, 1:].mean(2); sd = res[:, :, 1:].std(2) + 1e-9; z = (res[:, :, 0] - mu) / sd
print(f'{o.cap} PCI {o.pci} frame-timing strongest SSB {ib}; chunks {nch}; z = (stat - null mean)/null std over {len(null)} null PCIs')
print('slot: fraction of chunks with z>5 | max z   (slots 0..39 = burst frame + next frame)')
for n in range(40): print(f'{n:2d} {np.mean(z[:, n] > 5):4.2f} {z[:, n].max():6.1f}', end=' |' if n % 5 != 4 else '\n')
print('lit (slot,chunk) pairs z>5:', int((z > 5).sum()), 'of', z.size, ' null-pair false alarm rate expected < 1e-3')
