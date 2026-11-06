#!/usr/bin/env python3
"""a3_align.py <run dir>... : ground-truth validation of boundary-continuity alignment (method ii) on real single-user wide grants.
Take a contiguous wide record (>=128 valid PRB, comb k step 2, one symbol), cut it into S segments (optionally removing GAP PRBs between them),
multiply each segment by a random complex scalar (amp 0.5..2, phase uniform), align by boundary fit, compare with the intact record.
Errors: per-segment scalar (phase rad, amp dB) after fixing the gauge on segment 0; stitched CFR error; delay-profile error; NULL: no alignment, and
splice of two DIFFERENT records (no true continuity) to show the fit makes anything look continuous."""
import sys, numpy as np, json, collections
from load import *
rng = np.random.default_rng(1)
import os
M = int(os.environ.get('STITCH_M', 6))  # edge REs per side used for the local fit (6 = 1 PRB); chosen on stch_415_1, reported held-out on the other runs
ADD_SNR = os.environ.get('STITCH_ADD_SNR')  # dB: add white noise at this SNR (re mean power) on top of the measured one
def edge_fit(k, p, side, deg=int(os.environ.get('STITCH_DEG', 1))):
    """local fit of complex scalar profile p(k): ln|p| and unwrapped phase linear in k over M REs at 'side'; returns fn(k)->p."""
    idx = np.arange(len(k))[:M] if side == 'lo' else np.arange(len(k))[-M:]
    kk = k[idx].astype(float); ph = np.unwrap(np.angle(p[idx])); am = np.log(np.maximum(np.abs(p[idx]), 1e-9))
    cph = np.polyfit(kk - kk.mean(), ph, deg); cam = np.polyfit(kk - kk.mean(), am, deg); k0 = kk.mean()
    return lambda x: np.exp(np.polyval(cam, x - k0) + 1j*np.polyval(cph, x - k0))
def align(segs):
    """segs: list of (k sorted ascending, h[4,n]) in frequency order. returns scalars s_i (gauge s_0=1) that multiply segment i. Uses antenna-vector direction u
    from the edge of the left segment and projects both sides on it (spatial-signature coherence is NOT assumed: the projection uses the left edge direction)."""
    s = [1.0 + 0j]
    for i in range(len(segs) - 1):
        (ka, ha), (kb, hb) = segs[i], segs[i + 1]
        u = ha[:, -M:].mean(1); u = u / np.linalg.norm(u)           # direction at the right edge of A
        pa = (np.conj(u)[:, None] * ha).sum(0) * s[i]; pb = (np.conj(u)[:, None] * hb).sum(0)
        fa = edge_fit(ka, pa, 'hi'); fb = edge_fit(kb, pb, 'lo'); kmid = 0.5 * (ka[-1] + kb[0])
        s.append(fa(kmid) / fb(kmid))                                  # make the two local fits meet at the midpoint
    return np.array(s)
def pdp(k, h, nfft=4096):
    """power-delay profile from comb samples: IFFT over the 60 kHz comb grid (missing = 0), summed over RX; delay axis 1/(2*30 kHz*...)."""
    g = np.zeros((h.shape[0], 217*6*2 + 1), complex); 
    # place on the k//2 grid relative to min
    q = (k - k.min()) // 2; g = np.zeros((h.shape[0], int(q.max()) + 1), complex); g[:, q] = h
    P = (np.abs(np.fft.ifft(g, nfft, axis=1))**2).sum(0); return P / P.sum()
def dstat(Pa, Pb):
    tv = float(0.5 * np.abs(Pa - Pb).sum())   # total variation distance of the normalised PDPs: 0 identical, 1 disjoint
    n = len(Pa); da = abs(int(np.argmax(Pa)) - int(np.argmax(Pb))); da = min(da, n - da)
    tau = np.arange(n); tau = np.minimum(tau, n - tau)
    return tv, int(np.argmax(Pa)), int(np.argmax(Pb)), da
def one(d, S, gap_prb, cut_mode):
    k, h = d['k'], d['h']; o = np.argsort(k); k, h = k[o], h[:, o]
    prb = k // 12; ps = prb.min(); n = prb.max() - ps + 1
    if n < 128: return None
    # cut points (PRB index relative): 'odd' = not on a 4-PRB boundary, 'prg' = on a 4-PRB boundary
    pool = [c for c in range(24, n - 24) if (c % 4 == 0) == (cut_mode == 'prg')]
    if len(pool) < S: return None
    cuts = sorted(rng.choice(pool, S - 1, replace=False))
    if any(np.diff(cuts) < 24 + 2*gap_prb) if S > 2 else False: return None
    bounds = [0] + list(cuts) + [n]; parts = []
    for i in range(S):
        lo = bounds[i] + (gap_prb if i > 0 else 0); hi = bounds[i + 1]
        m = (prb - ps >= lo) & (prb - ps < hi); parts.append((k[m], h[:, m]))
    parts0 = parts
    true_s = np.exp(1j * rng.uniform(-np.pi, np.pi, S)) * rng.uniform(.5, 2, S)
    if ADD_SNR is not None:
        sig = np.mean(np.abs(h)**2); sg = np.sqrt(sig / 10**(float(ADD_SNR)/10) / 2)
        parts = [(kk, hh + sg*(rng.standard_normal(hh.shape) + 1j*rng.standard_normal(hh.shape))) for kk, hh in parts]
    corrupted = [(kk, hh * ts) for (kk, hh), ts in zip(parts, true_s)]
    est = align(corrupted)                                  # multiplies segment i; gauge s_0 = 1
    rel_true = true_s / true_s[0]; resid = est * true_s / (est * true_s)[0]     # ideal: est*true_s = const -> resid 1
    ph_err = np.abs(np.angle(resid[1:])); amp_err = np.abs(20*np.log10(np.abs(resid[1:])))
    kk = np.concatenate([p[0] for p in parts]); H0 = np.concatenate([p[1] for p in parts0], 1)
    Ha = np.concatenate([hh * e for (_, hh), e in zip(corrupted, est)], 1); Hn = np.concatenate([hh for _, hh in corrupted], 1)
    def ferr(Hx): c = np.vdot(Hx, H0) / np.vdot(Hx, Hx); return float(np.linalg.norm(c * Hx - H0) / np.linalg.norm(H0))
    Pr = pdp(k, h); Pa = pdp(kk, Ha); Pn = pdp(kk, Hn); P0 = pdp(kk, H0)
    return dict(ph=ph_err.max(), amp=amp_err.max(), e_al=ferr(Ha), e_no=ferr(Hn), pdp_al=dstat(Pa, P0)[0], pdp_no=dstat(Pn, P0)[0], pdp_gapped_true=dstat(P0, Pr)[0],
                dpk_al=dstat(Pa, P0)[3])
def null_splice(D, gap_prb=0):
    """two different wide records (different slots/users): left half of A + right half of B, align -> looks continuous?; compare with A's own PDP (truth for A) and B's."""
    A, B = rng.choice(len(D), 2, replace=False); A, B = D[A], D[B]
    def half(d, side):
        k, h = d['k'], d['h']; o = np.argsort(k); k, h = k[o], h[:, o]; m = len(k)//2
        return (k[:m], h[:, :m]) if side == 0 else (k[m:], h[:, m:])
    a = half(A, 0); b = half(B, 1)
    if abs(a[0][-1] - b[0][0]) > 40: return None
    s = align([a, b]); Ha = np.concatenate([a[1], b[1]*s[1]], 1); kk = np.concatenate([a[0], b[0]])
    c, coh, _ = jump(Ha, np.array([int(np.sum(kk <= a[0][-1])) - 1]), np.array([int(np.sum(kk <= a[0][-1]))]))
    # discontinuity after alignment at the seam (should be ~within-PRG noise if method forces continuity) and spatial coherence at seam
    ref = pdp(kk, np.concatenate([a[1], b[1]], 1)); 
    return dict(seam_coh=float(coh[0]), pdp_vs_A=dstat(pdp(kk, Ha), pdp(a[0], a[1]))[0], pdp_vs_B=dstat(pdp(kk, Ha), pdp(b[0], b[1]))[0])
if __name__ == '__main__':
    out = {}
    for R in sys.argv[1:]:
        D = [d for d in load_pdsch(R, 128) if d['valid'] >= 128 and d['nb'] >= 128 and (d['k'].max()//12 - d['k'].min()//12 + 1) == d['valid']]
        print(R.split('/')[-1], 'contiguous wide records', len(D), 'nl1', sum(d['nl'] == 1 for d in D))
        for cut_mode in ('odd', 'prg'):
            for S, g in ((2, 0), (3, 0), (2, 4), (2, 16)):
                rs = [r for d in D for r in [one(d, S, g, cut_mode)] if r][:400]
                if not rs: continue
                a = lambda key: np.array([r[key] for r in rs])
                print(' cut=%s S=%d gap=%2d PRB n=%3d | scalar err max-seg phase p50/p90 %.2f/%.2f rad, amp p50/p90 %.1f/%.1f dB | CFR rel.err aligned p50 %.2f vs none p50 %.2f | PDP total-variation dist vs intact (0 = identical) aligned p50 %.3f p90 %.3f; none p50 %.3f; (intact-with-gap-REs-removed %.3f) | peak shift aligned p90 %d bins' % (
                    cut_mode, S, g, len(rs), *np.percentile(a('ph'), [50, 90]), *np.percentile(a('amp'), [50, 90]), np.median(a('e_al')), np.median(a('e_no')), np.median(a('pdp_al')), np.percentile(a('pdp_al'), 90), np.median(a('pdp_no')), np.median(a('pdp_gapped_true')), np.percentile(a('dpk_al'), 90)))
        ns = [r for _ in range(300) for r in [null_splice(D)] if r]
        if ns: print(' NULL splice of two different records (n=%d): seam spatial coherence p50 %.2f (same-user boundary ref ~0.98); PDP TV distance stitched-vs-A-half %.2f, vs B-half %.2f (0 = same)' % (len(ns), np.median([r['seam_coh'] for r in ns]), np.median([r['pdp_vs_A'] for r in ns]), np.median([r['pdp_vs_B'] for r in ns])))
