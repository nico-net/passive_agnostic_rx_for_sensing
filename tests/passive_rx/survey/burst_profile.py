#!/usr/bin/env python3
"""burst_profile.py CAPDIR SSB_MHZ PCI [DUR_S] : per-20 ms PSS profile of one SSB (offline, numpy).
Prints per chunk: peak/median (dB) of |PSS corr|^2 (4 ch non-coherent) for N_ID2 = PCI%3 and the peak time (ms mod 20),
then the folded metric for K = 1, 2, 4, 8, 16 consecutive bursts at the dominant phase (non-coherent accumulation gain).
Reuses ssb_search2.py (pass1). Offline analysis only."""
import sys, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import ssb_search2 as s2
d, f, pci = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]); dur = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
cap = s2.Cap(d, 4); n = min(int(round(dur/0.02)), cap.n//s2.CH); D = 16; fs = s2.FS/D
acc, _ = s2.pass1(cap, 0, n, D, [s2.T_NR[pci % 3]], n, [(f-cap.c)*1e6-30e3])
a = acc[0][:, 0]                           # (chunks, L)
med = np.median(a, axis=1)
tot = a.sum(0); ph = int(tot.argmax())      # dominant phase over the whole capture
print(f'## {d} SSB {f} MHz PCI {pci} N_ID2 {pci%3}: {n} chunks, dominant phase {ph/fs*1e3:.3f} ms')
for c in range(n):
    w = a[c, max(0, ph-2):ph+3].max()
    print(f'chunk {c:3d} t={c*20:5d} ms  at-phase {10*np.log10(w/med[c]):6.2f} dB  chunk-max {10*np.log10(a[c].max()/med[c]):6.2f} dB @ {a[c].argmax()/fs*1e3:7.3f} ms')
print('accumulation (consecutive chunks from 0, at dominant phase, metric = value/median; noise-only max over L positions in brackets):')
for K in (1, 2, 4, 8, 16, 32, n):
    if K > n: break
    A = a[:K].sum(0); m = np.median(A); print(f'K={K:3d}  peak/med {10*np.log10(A[max(0,ph-2):ph+3].max()/m):6.2f} dB   max-elsewhere/med {10*np.log10(np.delete(A, range(max(0,ph-40), ph+41)).max()/m):6.2f} dB')
