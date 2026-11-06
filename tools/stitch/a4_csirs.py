#!/usr/bin/env python3
"""a4_csirs.py <923 run dir>: (1) channel stationarity from the non-precoded full-band CSI-RS H (4 RX): spatial-signature coherence and PDP distance vs lag;
(2) user H*W_u (layer 0, wide pdsch_dmrs records) vs the nearest CSI-RS occasion: per-PRB spatial-signature coherence and PDP distance (is the delay structure/direction W-invariant here?);
(3) same-slot adjacent-user seams: signature coherence at the seam and each side's coherence to CSI-RS."""
import sys, numpy as np, collections, json
from load import *
from a3_align import dstat
R = sys.argv[1]
rec = records(R + '/sense_records_last.jsonl', ('csirs',)); cs = []
for f in frames(R + '/sense_cfr_last.bin'):
    r = rec.get(f['fid'])
    if r and r['valid_cfr_prbs'] >= 200: cs.append(dict(slot=r['abs_slot'], res=r['csirs_resource'], k=f['k'].astype(int), h=np.array(f['h'])))
cs.sort(key=lambda c: c['slot']); print('csirs frames full-band', len(cs), collections.Counter(c['res'][:21] for c in cs).most_common(4))
def prb_pdp(prb, h, n=512):
    g = np.zeros((h.shape[0], 217), complex); g[:, prb] = h; P = (np.abs(np.fft.ifft(g, n, axis=1))**2).sum(0)
    P = np.maximum(P - np.median(P[n//4:3*n//4]), 0)   # noise-floor removed (aliased delay axis, so use the median of the mid bins), else the TV measures noise
    P = np.convolve(np.r_[P[-3:], P, P[:3]], np.ones(7)/7, 'valid'); return P / max(P.sum(), 1e-12)
def coh(a, b): return np.abs(np.sum(np.conj(a) * b, 0)) / np.maximum(np.linalg.norm(a, axis=0) * np.linalg.norm(b, axis=0), 1e-12)
# (1) stationarity: same resource, lag in slots
by = collections.defaultdict(list)
for c in cs: by[c['res']].append(c)
lagres = collections.defaultdict(list)
for res, L in by.items():
    for i in range(len(L) - 1):
        for j in range(i + 1, min(i + 60, len(L))):
            lag = L[j]['slot'] - L[i]['slot']
            if lag > 4000: break
            if L[i]['k'].size != L[j]['k'].size: continue
            c = coh(L[i]['h'], L[j]['h']); a = np.median(c); P = dstat(prb_pdp(L[i]['k'] // 12, L[i]['h']), prb_pdp(L[j]['k'] // 12, L[j]['h']))[0]
            lagres[min(int(lag * 0.5 // 20) * 20, 1000)].append((a, P))
print('(1) CSI-RS same-resource stationarity: lag_ms: n, median per-PRB spatial coherence, median PDP TV')
for lag in sorted(lagres): v = np.array(lagres[lag]); print('  %5d ms  n=%5d  coh %.3f  TV %.3f' % (lag, len(v), np.median(v[:, 0]), np.median(v[:, 1])))
# (2),(3)
D = load_pdsch(R, 1); css = np.array([c['slot'] for c in cs])
def nearest(slot):
    i = int(np.argmin(np.abs(css - slot))); return cs[i], abs(int(css[i] - slot))
rows = []
for d in D:
    if d['valid'] < 128 or d['nb'] < 128: continue
    c, gap = nearest(d['slot'])
    if gap > 80: continue   # 40 ms
    k = d['k']; m = (k % 12 == 0)          # one DM-RS RE per PRB
    if m.sum() < 100: continue
    prb = k[m] // 12; hu = d['h'][:, m]; cm = {p: i for i, p in enumerate(c['k'] // 12)}
    pi = [i for i, p in enumerate(prb) if p in cm]
    if len(pi) < 100: continue
    hc = c['h'][:, [cm[prb[i]] for i in pi]]; hu2 = hu[:, pi]; cc = coh(hu2, hc)
    # PDP on the common PRB set
    pu = prb_pdp(prb[pi], hu2); pc = prb_pdp(prb[pi], hc)
    rows.append((d['nl'], np.median(cc), np.percentile(cc, 10), dstat(pu, pc)[0], gap * .5, d['rnti']))
rows = np.array(rows)
print('(2) wide user H*W_u vs nearest CSI-RS H (<=40 ms): n=%d | per-PRB spatial coherence median-of-medians %.3f (p10 of PRB p10: %.3f) | PDP TV user-vs-CSIRS median %.3f p90 %.3f | CSIRS-vs-CSIRS TV at similar lag is the floor' % (len(rows), np.median(rows[:, 1]), np.percentile(rows[:, 2], 50), np.median(rows[:, 3]), np.percentile(rows[:, 3], 90)))
for nl in (1, 2, 3, 4):
    r = rows[rows[:, 0] == nl]
    if len(r): print('   nl=%d n=%d coh %.3f TV %.3f' % (nl, len(r), np.median(r[:, 1]), np.median(r[:, 3])))
np.save(R + '/../stch_a4_%s.npy' % R.rstrip('/').split('/')[-1], rows)
# (3) seams
by = collections.defaultdict(list)
for d in D: by[(d['slot'], d['l'])].append(d)
seam = []; sc = []
for key, L in by.items():
    us = collections.defaultdict(list)
    for d in L: us[d['rnti']].append(d)
    if len(us) < 2: continue
    segs = [(np.concatenate([d['k'] for d in v]), np.concatenate([d['h'] for d in v], 1), r) for r, v in us.items()]
    c, gap = nearest(key[0])
    for a in segs:
        for b in segs:
            if a[2] == b[2] or a[0].max() >= b[0].min(): continue
            ia, ib = np.argmax(a[0]), np.argmin(b[0])
            if b[0][ib] - a[0][ia] <= 4:
                h = np.stack([a[1][:, ia], b[1][:, ib]], 1); seam.append(float(coh(h[:, :1], h[:, 1:])[0]))
print('(3) adjacent-user seams (923): n=%d seam spatial coherence median %.3f, frac<0.9 %.3f' % (len(seam), np.median(seam) if seam else float('nan'), np.mean(np.array(seam) < .9) if seam else float('nan')))
