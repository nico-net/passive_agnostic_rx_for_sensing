#!/usr/bin/env python3
"""fastscan.py : fast blind n78/n77 cell scanner (30 kHz SCS), same cells.json as autofront.py discover, ~100x faster.
  fastscan.py CAPDIR... [--out cells.json] [--start 0] [--m 12] [--nch 4] [--top 12] [--sss-min 2] [--dmrs-min 2.5] [--verify-chunks 50] [--n2-ex 3.0]
Stage 1 (detect): M 20 ms chunks x nch channels, one FFT per chunk/channel, every GSCN band cut in the frequency domain at 3.84 MS/s,
  PSS correlation by FFT, |cc|^2 summed over channels, folded modulo k*20 ms (k=1,2,4,8). One CFO hypothesis (0): the 30 kHz PSS symbol
  tolerates +-7 kHz at <= 1 dB loss, so the CFO grid of autofront.py is not needed.
Stage 2 (verify, top candidates): short raw windows (32768 samples) around the folded PSS time of EVERY k-th chunk of the capture
  (more integration than stage 1), per-occurrence timing, SSS -> PCI (ratio best/second), PBCH-DMRS -> i_ssb, and CFO from the
  DMRS/PSS channel phase between adjacent symbols (spacing NF+CP, unambiguous +-14 kHz, no sign alias), refined by the 2-symbol lag.
No per-cell constant. shortcut: 30 kHz raster only, strongest SSB per GSCN, CPU numpy (GPU port: stage 1 is batched FFTs)."""
import sys, os, json, time
import numpy as np, scipy.fft as sf
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'survey'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ssb_search2 as S
from autofront import pick_cells, tune

FS, CH, NF, CP = S.FS, S.CH, S.NF, S.CP
W = 32768                       # raw window, = 2048 samples at 7.68 MS/s
NB = 128                        # PSS body position in the window (decimated samples)
SYM = NF + CP
WK = int(os.environ.get('FASTSCAN_WORKERS', '10'))

def keep(rows, sss_min=2.0, dmrs_min=2.5, dmrs_min_multi=2.0, sss_strong=3.0, dmrs_min_strong=2.0):
    """reliable detections, best per (gscn, pci). Co-located cells (n_cells > 1) already passed the SSS group gap test, so a lower DMRS bar;
    so does a clear single SSS winner (gap >= sss_strong over 336 hypotheses: the noise gap is 1.0-1.4), e.g. the weak TIM 675 (sss 3.74, dmrs 2.47)."""
    lo = lambda r: dmrs_min_multi if r.get('n_cells', 1) > 1 else dmrs_min_strong if r['sss_ratio'] >= sss_strong else dmrs_min
    ok = [r for r in rows if r['sss_ratio'] >= sss_min and r['dmrs_ratio'] >= lo(r)]
    return pick_cells(ok, 0, 0)

def dedupe(cells, gscn_span=3):
    """one cell reported on neighbouring GSCNs (its PSS leaks into +-1..2 raster points): keep the highest SSS ratio per PCI."""
    keep = []
    for c in sorted(cells, key=lambda r: -r['sss_ratio']):
        if not any(k['pci'] == c['pci'] and abs(k['gscn'] - c['gscn']) <= gscn_span for k in keep): keep.append(c)
    return sorted(keep, key=lambda r: r['gscn'])

def cfo_from_phase(p2, fs=FS / 16):
    """Hz from p2 = sum conj(H1)*H3 (PBCH DMRS symbols 1 and 3, lag 2*SYM samples): unambiguous +-fs/(4*SYM) = +-7007 Hz, which covers the
    +-2 ppm oscillator tolerance at 3.6 GHz (7.2 kHz). The lag-1 pairs involving symbol 2 showed a pi flip on some cells (508, 923): unused."""
    return float(np.angle(p2) * fs / (2 * np.pi * 2 * SYM))

def period_k(a, t, n, k0, m, fold=8):
    """smallest burst period (in 20 ms chunks, <= k0) whose phase class holds the burst: foldmetric's max over k is biased to large k."""
    cnt = np.array([len(range(s, m, fold)) for s in range(fold)]); ok = cnt > 0
    e = np.array([a[s, t, max(n - 2, 0):n + 3].max() / max(cnt[s], 1) for s in range(fold)]); sp = int(np.argmax(np.where(ok, e, -1)))
    for k in (1, 2, 4, 8):
        if k > k0: break
        sl = [s for s in range(fold) if ok[s] and s % k == sp % k]
        if all(e[s] > 0.5 * e[sp] for s in sl): return k, sp % k
    return k0, sp % k0

def stage1(cap, c0, m, grid_hz, fold=8):
    D = 32; L = CH // D; nf = NF // 2; G = len(grid_hz)
    T = []
    for i in range(3):
        X = np.zeros(nf, complex); X[np.arange(-63, 64) % nf] = S.pss(i); T.append(np.fft.ifft(X) * np.sqrt(nf))
    Tf = np.stack([np.conj(sf.fft(t.astype(np.complex64), L)) for t in T])
    idx = [S.bandidx(o, D) for o in grid_hz]
    acc = np.zeros((G, fold, 3, L), np.float32); pool = ThreadPoolExecutor(WK)
    for c in range(m):
        a = (c0 + c) * CH
        X = np.stack([sf.fft(cap.x(ch, a, a + CH), workers=WK) for ch in range(cap.nch)])
        def one(g):                  # scipy.fft releases the GIL: threads over GSCNs scale, workers= on a 12-row batch does not
            P = sf.ifft(X[:, idx[g]][:, None, :] * Tf[None], axis=-1)
            acc[g, c % fold] += (P.real ** 2 + P.imag ** 2).sum(0)
        list(pool.map(one, range(G)))
    res = []; per_n2 = []
    for g in range(G):
        mm, k, sp, t, n = S.foldmetric(acc[g], m); k, sp = period_k(acc[g], t, n, k, m); res.append((mm, k, sp, t, n))
        # best burst per N_ID_2: a cell with another N_ID_2 at the same GSCN (PCI 614 beside 508 + 4) is hidden behind the strongest
        r3 = []
        for t2 in range(3):
            m2, k2, s2, _, n2 = S.foldmetric(acc[g][:, t2:t2 + 1], m); k2, s2 = period_k(acc[g], t2, n2, k2, m); r3.append((m2, k2, s2, t2, n2))
        per_n2.append(r3)
    return res, D, per_n2

def window(cap, a, off_hz):
    """(nch, 2048) baseband at 7.68 MS/s: band at off_hz (relative to the capture centre) of raw samples a..a+W."""
    t = np.arange(W); mix = np.exp(-2j * np.pi * off_hz * t / FS).astype(np.complex64); out = []
    for ch in range(cap.nch):
        Z = sf.fft(cap.x(ch, a, a + W) * mix); out.append(sf.ifft(np.concatenate([Z[:1024], Z[-1024:]])))
    return np.array(out)

_DM = {}
def dm(pci):
    if pci not in _DM: _DM[pci] = [S.dmrs(pci, i) for i in range(8)]
    return _DM[pci]

def clusters(ccs, thr=10.0, min_frac=0.15):
    """timing clusters of the per-occurrence PSS peaks (co-located cells with the same N_ID_2 sit a few samples apart, e.g. 508 and 4):
    -> list of (centre, [per-occurrence peak index or None])"""
    pk = [[(int(i), float(c[i] / np.median(c))) for i in range(1, len(c) - 1) if c[i] >= c[i - 1] and c[i] >= c[i + 1] and c[i] > thr * np.median(c) and c[i] >= 0.5 * c.max()] for c in ccs]   # 0.5*max: PSS sidelobes of a strong cell are not cells
    h = np.zeros(len(ccs[0]) + 8)
    for l in pk:
        for i, _ in l: h[i + 2:i + 7] += (1, 2, 3, 2, 1)   # triangular +-2 sample smoothing (index i+4 is the centre)
    out = []
    while h.max() >= 3 * max(3, min_frac * len(ccs)):
        ctr = int(np.argmax(h)) - 4; h[max(ctr, 0):ctr + 9] = 0
        out.append((ctr, [max([(r, i) for i, r in l if abs(i - ctr) <= 2], default=(0, None))[1] for l in pk]))
    return out

def pci_group(sc, n2, gap=2.0, jmax=3):
    """PCIs of the SSS score vector: the smallest group of top scores separated from the rest by amplitude ratio >= gap (1 = one cell;
    2-3 = co-located cells sharing N_ID_2, e.g. 508 + 4: their PSS overlap so best/second alone is ~1.3). -> ([pci...], ratio)"""
    i = np.argsort(-sc); a = np.sqrt(sc[i] / (sc[i[0]] + 1e-30))
    for j in range(1, jmax + 1):
        if a[j - 1] / (a[j] + 1e-30) >= gap: return [3 * int(x) + n2 for x in i[:j]], float(a[j - 1] / (a[j] + 1e-30))
    return [], float(a[0] / (a[1] + 1e-30))

def dmrs_cfo(occ, dpb, pci):
    """PBCH DMRS of symbols 1..3 (bin = subcarrier - 119; tau = fractional timing grid) -> i_ssb, ratio best/second, CFO (Hz)."""
    kk = np.arange(240); v = pci % 4; pos1 = kk[(kk % 4) == v]; pos2 = pos1[(pos1 < 48) | (pos1 >= 192)]
    R = np.array(dm(pci))                  # (8, 144)
    layout = ((1, pos1, R[:, :60]), (2, pos2, R[:, 60:84]), (3, pos1, R[:, 84:144]))
    taus = np.arange(-1, 1.01, 0.25); score = np.zeros((len(taus), 8)); Ys = []
    for y, n in occ:
        for yy in y:
            q = n + dpb; Y = {s: np.fft.fft(yy[q + s * SYM:q + s * SYM + NF]) / np.sqrt(NF) for s in range(1, 4)}; Ys.append(Y)
            for s, pos, r in layout:
                b = (pos - 119) % NF
                for ti, tau in enumerate(taus):
                    score[ti] += np.abs(np.conj(r) @ (Y[s][b] * np.exp(-2j * np.pi * tau * (pos - 119) / NF))) ** 2
    ti, ib = np.unravel_index(np.argmax(score), score.shape); so = np.sort(score[ti])
    r1, r3 = R[ib, :60], R[ib, 84:144]; b1 = (pos1 - 119) % NF; p2 = 0j     # CFO: phase between the DMRS channel estimates of symbols 1 and 3
    for Y in Ys: p2 += np.sum(np.conj(Y[1][b1] * np.conj(r1)) * (Y[3][b1] * np.conj(r3)))
    return int(ib), float(np.sqrt(so[-1] / (so[-2] + 1e-30))), cfo_from_phase(p2)

def decode(occ, ns, n2):
    """SSS -> PCI(s), DMRS i_ssb and CFO for the occurrences (y, window-index of the PSS body) whose ns entry is not None -> list of dicts."""
    occ = [(y, n) for (y, _), n in zip(occ, ns) if n is not None]
    if len(occ) < 2: return []
    sss_sc = {dp: np.zeros(336) for dp in (-1, 0, 1)}; pn = np.conj(S.pss(n2))
    for y, n in occ:
        for dp in sss_sc:
            q = n + dp
            for yy in y:
                Hp = S.fsym(yy[q:q + NF]) * pn; Z = S.fsym(yy[q + 2 * SYM:q + 2 * SYM + NF])
                sss_sc[dp] += np.abs(S.SS[n2] @ np.conj(Z * np.conj(Hp))) ** 2
    dpb = max(sss_sc, key=lambda d: sss_sc[d].max()); pcis, ratio = pci_group(sss_sc[dpb], n2)
    out = []
    for pci in pcis:
        ib, dr, cfo = dmrs_cfo(occ, dpb, pci); out.append(dict(pci=pci, sss_ratio=ratio, dmrs_ratio=dr, i_ssb=ib, n_occ=len(occ), cfo_hz=cfo, n_cells=len(pcis)))
    return out

def verify(cap, c0, nchunks, off_hz, n2, k, sp, n32, maxocc=40):
    """cells at one GSCN: occurrences = chunks with (c % k) == sp over the whole capture; one result per PSS timing cluster."""
    Tn = S.T_NR[n2].astype(np.complex64); occ = []; ccs = []
    for c in range(nchunks):
        if c % k != sp: continue
        a0 = (c0 + c) * CH + n32 * 32 - NB * 16
        if a0 < 0 or a0 + W > cap.n: continue
        y = window(cap, a0, off_hz)
        ccs.append(sum(np.abs(np.lib.stride_tricks.sliding_window_view(yy, NF)[NB - 24:NB + 25] @ np.conj(Tn)) ** 2 for yy in y)); occ.append((y, None))
        if len(occ) >= maxocc: break
    if len(occ) < 2: return []
    res = []
    for ctr, ns in clusters(ccs):
        for r in decode(occ, [None if n is None else NB - 24 + n for n in ns], n2): r['dt_samples'] = ctr - 24; res.append(r)
    return res

def scan(capdir, start=0.0, m=12, nch=4, top=40, vchunks=50, ex_min=1.0, n2_ex=3.0, log=None):
    cap = S.Cap(capdir, nch); c0 = int(start / 0.02); nchunks = min(vchunks, cap.n // CH - c0); m = min(m, nchunks)
    lim = 61.44 * 0.82; N0 = int(np.ceil((cap.c - lim - 3000) / 1.44)); N1 = int((cap.c + lim - 3000) / 1.44); Ns = np.arange(N0, N1 + 1)
    base = (3000 + Ns * 1.44 - cap.c) * 1e6 - 30e3
    t0 = time.time(); res, D, per_n2 = stage1(cap, c0, m, base); t1 = time.time()
    null = float(np.median([r[0] for r in res])); order = np.argsort([-r[0] for r in res]); out = []
    for g in order[:top]:
        ex = res[g][0] - null
        if ex < ex_min: break
        cands = [res[g]] + [r for r in per_n2[g] if r[3] != res[g][3] and r[0] - null >= max(ex_min, n2_ex)]   # strongest N_ID_2 first, then the others with their own burst
        for mm, k, sp, t, n in cands:
            for r2 in verify(cap, c0, nchunks, base[g], t, k, sp, n):
                if log: print(f'  cand GSCN {7499 + Ns[g]} n2 {t} ex {mm - null:.2f} k {k} dt {r2["dt_samples"]} -> {(r2["pci"], round(r2["sss_ratio"], 2), round(r2["dmrs_ratio"], 2), round(r2["cfo_hz"]))}', file=log)
                out.append(dict(gscn=int(7499 + Ns[g]), ssb_mhz=round(3000 + Ns[g] * 1.44, 2), pci=int(r2['pci']), metric_db=round(mm - null, 2),
                    sss_ratio=round(r2['sss_ratio'], 2), dmrs_ratio=round(r2['dmrs_ratio'], 2), i_ssb=r2['i_ssb'], scs_khz=30, cfo_hz=round(r2['cfo_hz']),
                    n_occ=r2['n_occ'], n_cells=r2['n_cells'], dt_samples_7p68=r2['dt_samples'], capture=os.path.abspath(capdir), capture_centre_mhz=cap.c))
    if log: print(f'  {capdir}: stage1 {t1 - t0:.1f} s, verify {time.time() - t1:.1f} s', file=log)
    return out

def main(a):
    kw = dict(start=0.0, m=12, nch=4, top=40, vchunks=50, n2_ex=3.0); out = 'cells.json'; sss, dmrs = 2.0, 2.5; dirs = []; i = 1
    while i < len(a):
        if a[i].startswith('--'):
            k = a[i][2:].replace('-', '_')
            if k == 'out': out = a[i + 1]
            elif k == 'sss_min': sss = float(a[i + 1])
            elif k == 'dmrs_min': dmrs = float(a[i + 1])
            elif k == 'verify_chunks': kw['vchunks'] = int(a[i + 1])
            else: kw[k] = type(kw[k])(a[i + 1])
            i += 2
        else: dirs.append(a[i]); i += 1
    t0 = time.time(); rows = [r for d in dirs for r in scan(d, log=sys.stderr, **kw)]
    cells = dedupe(keep(rows, sss, dmrs))
    for c in cells: c['tune_pass1'] = tune(c)
    json.dump(cells, open(out, 'w'), indent=1); print(f'{len(cells)} cells -> {out}  ({time.time() - t0:.1f} s)')
    for c in cells: print(f"  GSCN {c['gscn']} {c['ssb_mhz']} MHz PCI {c['pci']} i_ssb {c['i_ssb']} sss {c['sss_ratio']} dmrs {c['dmrs_ratio']} cfo {c['cfo_hz']} -> {c['tune_pass1']}")
if __name__ == '__main__': main(sys.argv)
