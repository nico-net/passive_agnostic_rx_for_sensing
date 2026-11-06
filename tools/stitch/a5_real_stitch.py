#!/usr/bin/env python3
"""a5_real_stitch.py <923 run dir>: REAL same-slot multi-user chains (adjacent users, k-gap <= 4) on the cell with a non-precoded CSI-RS reference.
Per chain (>= 2 users, >= MINPRB PRB total): delay profile (PDP, PRB-sampled, noise floor removed) of the stitched CFR with
 (A) alignment by boundary fit (method ii), (N0) none (as received), (NR) random per-segment scalars (null: what an arbitrary phase gives),
 (S1) each single segment alone, vs the CSI-RS H on the SAME PRBs (nearest occasion, <= 40 ms) as truth. TV = total variation (0 identical).
Also the seam spatial coherence and each segment's per-PRB signature coherence with CSI-RS."""
import sys, numpy as np, collections
from load import *
from a3_align import align
import a4_csirs as A4   # prints its own summary on import; harmless
rng = np.random.default_rng(3)
def tv(a, b): return float(0.5 * np.abs(a - b).sum())
def first_per_prb(k, h):
    prb = k // 12; o = np.lexsort((k, prb)); prb, k, h = prb[o], k[o], h[:, o]; keep = np.r_[True, prb[1:] != prb[:-1]]; return prb[keep], h[:, keep]
def pdp_of(prb, h): return A4.prb_pdp(prb, h)
def chains(R, minprb):
    D = load_pdsch(R, 1); by = collections.defaultdict(list)
    for d in D: by[(d['slot'], d['l'])].append(d)
    out = []
    for key, L in by.items():
        us = collections.defaultdict(list)
        for d in L: us[d['rnti']].append(d)
        if len(us) < 2: continue
        segs = []
        for r, v in us.items():
            k = np.concatenate([d['k'] for d in v]); h = np.concatenate([d['h'] for d in v], 1); o = np.argsort(k); segs.append((k[o], h[:, o], r))
        segs.sort(key=lambda s: s[0][0]); cur = [segs[0]]
        for s in segs[1:] + [None]:
            if s is not None and s[0][0] - cur[-1][0][-1] <= 4 and s[2] != cur[-1][2]: cur.append(s); continue
            if len(cur) >= 2 and sum(len(np.unique(c[0] // 12)) for c in cur) >= minprb: out.append((key[0], cur))
            cur = [s] if s is not None else []
    return out
if __name__ == '__main__':
    R = sys.argv[1]; minprb = int(sys.argv[2]) if len(sys.argv) > 2 else 48
    css = A4.css; res = []
    for slot, ch in chains(R, minprb):
        c, gap = A4.nearest(slot)
        if gap > 80: continue
        s = align([(k, h) for k, h, _ in ch]); cm = {p: i for i, p in enumerate(c['k'] // 12)}
        def stitched(scal):
            k = np.concatenate([x[0] for x in ch]); h = np.concatenate([x[1] * sc for x, sc in zip(ch, scal)], 1); return first_per_prb(k, h)
        prb, ha = stitched(s)
        if not all(p in cm for p in prb): continue
        hc = c['h'][:, [cm[p] for p in prb]]; ref = pdp_of(prb, hc)
        _, hn = stitched(np.ones(len(ch))); r_n = tv(pdp_of(prb, hn), ref)
        r_r = np.median([tv(pdp_of(prb, stitched(np.exp(1j * rng.uniform(-np.pi, np.pi, len(ch))))[1]), ref) for _ in range(20)])
        # (iii) one complex scalar per segment fitted to the CSI-RS H on that segment's PRBs (1 DOF per segment; the residual PDP shape is the honest part)
        sc3 = []
        for x in ch:
            p1, h1 = first_per_prb(x[0], x[1]); hr = c['h'][:, [cm[p] for p in p1]]; sc3.append(np.vdot(h1, hr) / np.vdot(h1, h1))
        h3 = stitched(np.array(sc3))[1]; r_c = tv(pdp_of(prb, h3), ref)
        pk = lambda P: int(np.argmax(P)); n_ = len(ref); dpk = lambda a: min(abs(pk(a) - pk(ref)), n_ - abs(pk(a) - pk(ref)))
        pk_al, pk_no, pk_c = dpk(pdp_of(prb, ha)), dpk(pdp_of(prb, hn)), dpk(pdp_of(prb, h3))
        pk_r = np.median([dpk(pdp_of(prb, stitched(np.exp(1j * rng.uniform(-np.pi, np.pi, len(ch))))[1])) for _ in range(20)])
        maxseg = max(len(np.unique(x[0] // 12)) for x in ch)
        singles = []
        for x in ch:
            p1, h1 = first_per_prb(x[0], x[1]); cm1 = [cm[p] for p in p1]; singles.append(tv(pdp_of(p1, h1), pdp_of(p1, c['h'][:, cm1])))   # per-segment on ITS OWN PRBs vs CSI-RS on the same PRBs
        seam = A4.coh(ch[0][1][:, -1:], ch[1][1][:, :1])[0]
        res.append((len(ch), len(prb), tv(pdp_of(prb, ha), ref), r_n, r_r, np.mean(singles), seam, r_c, pk_al, pk_no, pk_r, pk_c, maxseg))
    r = np.array(res); print('chains >= %d PRB (users adjacent, k-gap<=4) with CSI-RS within 40 ms: %d' % (minprb, len(r)))
    for lo, hi, only in ((48, 96, False), (96, 128, False), (128, 300, False), (128, 300, True)):
        m = (r[:, 1] >= lo) & (r[:, 1] < hi) & ((r[:, 12] < 128) if only else True)
        if m.sum() < 5: continue
        q = lambda c: np.median(r[m][:, c])
        print(' %3d-%3d PRB%s n=%4d | PDP TV vs CSI-RS (0=same): aligned(ii) %.3f | CSIRS-fit scalar(iii) %.3f | none %.3f | random null %.3f | single-segment floor %.3f || peak-delay err (5.4 ns bins) aligned %.1f | iii %.1f | none %.1f | random %.1f || seam coh %.3f' % (
            lo, hi, ' stitched-only(no seg>=128)' if only else '', m.sum(), q(2), q(7), q(3), q(4), q(5), q(8), q(11), q(9), q(10), q(6)))
