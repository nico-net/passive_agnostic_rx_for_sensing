#!/usr/bin/env python3
"""a1_within.py <run dir>: continuity of one user's own CFR (layer 0, DM-RS comb, 4 RX) between adjacent comb REs:
within a PRB vs across PRB boundaries, boundary class by (PRB index mod 16) absolute and relative to grant start.
dphi = angle of spatial correlation (scalar jump), coh = spatial-signature coherence (1 = same direction), ar = amplitude ratio."""
import sys, numpy as np, collections, json
from load import *
R = sys.argv[1]; min_valid = int(sys.argv[2]) if len(sys.argv) > 2 else 64
D = [d for d in load_pdsch(R, min_valid) if d['nl'] >= 1]
acc = {}
def add(key, dphi, coh, lar): acc.setdefault(key, [[], [], []]); a = acc[key]; a[0] += list(dphi); a[1] += list(coh); a[2] += list(lar)
for d in D:
    i, j = pairs(d['k'], d['h']); 
    if len(i) == 0: continue
    c, coh, ar = jump(d['h'], i, j); dphi = np.angle(c); lar = 20*np.log10(np.maximum(ar, 1e-6))
    kb = d['k'][i] // 12; kn = d['k'][j] // 12; bnd = kb != kn
    tag = 'nl1' if d['nl'] == 1 else 'nl>1'
    add((tag, 'within'), dphi[~bnd], coh[~bnd], lar[~bnd])
    for m in np.where(bnd)[0]:
        b = int(kn[m]); rel = b - d['start']
        for ref, v in (('abs', b), ('rel', rel)):
            cl = 'b%16=0' if v % 16 == 0 else 'b%4=0(not16)' if v % 4 == 0 else 'b%2=0(not4)' if v % 2 == 0 else 'odd'
            add((tag, ref, cl), dphi[m:m+1], coh[m:m+1], lar[m:m+1])
def rob(x): x = np.array(x); return float(np.median(np.abs(x - np.median(x))) * 1.4826) if len(x) else None
res = []
for key in sorted(acc, key=str):
    p, c, a = (np.array(v) for v in acc[key])
    res.append(dict(key=key, n=len(p), dphi_med=round(float(np.median(p)), 3), dphi_mad_rad=round(rob(p), 3), coh_med=round(float(np.median(c)), 3), coh_p10=round(float(np.percentile(c, 10)), 3), amp_db_mad=round(rob(a), 2), frac_coh_lt_0p9=round(float(np.mean(c < .9)), 3)))
for r in res: print(json.dumps(r))
