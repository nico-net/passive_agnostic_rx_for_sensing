#!/usr/bin/env python3
"""cochannel.py : offline co-channel measurement of co-sited cells on one carrier (PCIS2 2026-10-13).
  cochannel.py CAPDIR --ssb-mhz 3570.24 --pointa-hz 3560850000 --pcis 508,4,614 --cfo -4800 [--coreset-rb0 2 --coreset-nrb 48] [--out OUT.npz]
Frame timing from the SSB bursts (PSS fold at 7.68 MS/s, per-chunk), then for every slot of the 1 s capture:
  * PDSCH DM-RS (type 1, port 1000, nSCID 0, scrambling id = PCI, SIB1 case: reference = lowest RB of CORESET#0) of every PCI hypothesis,
  * joint least squares of ALL listed cells per block of B RB (flat channel per cell and block, per-cell timing slope), residual = unexplained,
  * outputs per (slot, symbol): per-cell channel vector h[cell][4 RX] per block and the residual power per RX.
Gold sequences use the receiver's definition (ssb_search2.gold); nothing here is cell specific: the cells, Point A and CORESET#0 are arguments.
shortcut: 30 kHz SCS, 122.88 MS/s captures, SSB case C (L=8) in the first half frame, SFN-even burst frame; CPU numpy."""
import sys, os, argparse, time
import numpy as np, scipy.fft as sf
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'survey'))
import ssb_search2 as S

FS = 122.88e6; N = 4096; CH = S.CH
def body(slot, l): return slot * 61440 + l * N + 352 + 288 * l            # first sample of the symbol body (after the CP), 122.88 MS/s
def body76(slot, l): return slot * 3840 + l * 256 + 22 + 18 * l           # same at 7.68 MS/s
def ssb_sym(i): return 14 * (i // 2) + (2 if i % 2 == 0 else 8)

def band(cap, c, off_hz, ch):
    a = c * CH; X = sf.fft(cap.x(ch, a, a + CH), workers=4); return sf.ifft(X[S.bandidx(off_hz, 16)], workers=2)

def pbch_power(y, p, pci, ib, nf=256, sym=274):
    """PBCH DM-RS correlation power of SSB index ib at PSS body start p (7.68 MS/s band signal y (nrx, L)): sum over RX of |<r, Y>|^2 / n."""
    kk = np.arange(240); v = pci % 4; pos1 = kk[(kk % 4) == v]; pos2 = pos1[(pos1 < 48) | (pos1 >= 192)]
    R = S.dmrs(pci, ib); tot = 0.0
    for s, pos, r in ((1, pos1, R[:60]), (2, pos2, R[60:84]), (3, pos1, R[84:144])):
        q = p + s * sym
        if q + nf > y.shape[1]: return 0.0
        Y = np.fft.fft(y[:, q:q + nf], axis=1)[:, (pos - 119) % nf] / np.sqrt(nf); tot += (np.abs(Y @ np.conj(r)) ** 2).sum()
    return tot

def frame_timing(cap, ssb_off_hz, pci, nchunks):
    """Frame start per chunk (7.68 MS/s samples, start of the SFN-even burst frame) of the cell `pci`: PSS fold gives the candidate SSB peaks, the cell's
    PBCH DM-RS (every SSB index) says which peak is which index; per-chunk refinement +-24 samples. -> (T[c], fold, t0, index of the strongest SSB)"""
    L = CH // 16; Tn = np.conj(sf.fft(S.T_NR[pci % 3].astype(np.complex64), L)); cc = []; ys = []
    for c in range(nchunks):
        p = np.zeros(L, np.float32); y = np.stack([band(cap, c, ssb_off_hz, ch) for ch in range(cap.nch)]); ys.append(y)
        for ch in range(cap.nch): p += np.abs(sf.ifft(sf.fft(y[ch]) * Tn)) ** 2
        cc.append(p)
    F = np.sum(cc, 0); med = np.median(F); cand = [int(i) for i in np.argsort(-F)[:60] if F[i] > 8 * med and F[i] >= np.max(F[max(0, i - 40):i + 41])]
    best = (0, None, None)
    for p in cand:
        for ib in range(8):
            sc = sum(pbch_power(ys[c], p, pci, ib) for c in range(min(nchunks, 8)))
            if sc > best[0]: best = (sc, p, ib)
    _, p, ib = best; t0 = (p - body76(ssb_sym(ib) // 14, ssb_sym(ib) % 14)) % L; T = []
    for c in range(nchunks):
        sc = [sum(pbch_power(ys[cc_], (t0 + d + body76(ssb_sym(ib) // 14, ssb_sym(ib) % 14)) % L, pci, ib) for cc_ in range(max(0, c - 1), min(nchunks, c + 2))) for d in range(-24, 25, 2)]
        T.append(t0 + range(-24, 25, 2)[int(np.argmax(sc))])
    return np.array(T), F, t0, ib

class Grid:
    def __init__(s, cap, pointa_hz, cfo_hz, nsc=720):
        s.cap = cap; fc = cap.c * 1e6; nom = pointa_hz - fc                      # sub-carrier 0 relative to the capture centre
        s.nbin0 = int(np.floor(nom / 30e3)); frac = nom - s.nbin0 * 30e3           # 0..30 kHz above the integer bin
        s.delta = cfo_hz + frac                                                     # shift that puts the signal on the integer grid
        s.nsc = nsc; s.bins = (s.nbin0 + np.arange(nsc)) % N
        s.rot = np.exp(-2j * np.pi * s.delta * np.arange(N) / FS).astype(np.complex64)
        s.signed = np.where(s.bins < N // 2, s.bins, s.bins - N)
    def sym(s, a0, ch, adv=144):
        """FFT (nsc sub-carriers) of the symbol whose body starts at a0 (window starts adv samples earlier, inside the CP)."""
        st = a0 - adv; x = s.cap.x(ch, st, st + N) * s.rot * np.complex64(np.exp(-2j * np.pi * s.delta * st / FS))
        Y = sf.fft(x)[s.bins]; return Y * np.exp(2j * np.pi * s.signed * adv / N).astype(np.complex64)

_G = {}
def dmrs_pdsch(pci, slot20, l, m0, nm):
    k = (pci, slot20, l, m0, nm)
    if k not in _G:
        ci = ((1 << 17) * (14 * slot20 + l + 1) * (2 * pci + 1) + 2 * pci) % (1 << 31)
        c = S.gold(ci, 2 * (m0 + nm)); r = ((1 - 2 * c[0::2]) + 1j * (1 - 2 * c[1::2])) / np.sqrt(2); _G[k] = r[m0:m0 + nm].astype(np.complex64)
    return _G[k]

def joint_ls(Y, R, blk):
    """Y (nrx, nre) data at the DM-RS REs; R (ncell, nre) reference sequences incl. each cell's phase slope; blocks of blk REs.
    -> H (nb, ncell, nrx), resid (nb, nrx) mean |residual|^2 per RE, ydot (nb, nrx) mean |y|^2"""
    nre = Y.shape[1]; nb = nre // blk; C = R.shape[0]; H = np.zeros((nb, C, Y.shape[0]), np.complex64); res = np.zeros((nb, Y.shape[0]), np.float32); pw = np.zeros_like(res)
    A = R[:, :nb * blk].reshape(C, nb, blk).transpose(1, 2, 0)                       # (nb, blk, C)
    y = Y[:, :nb * blk].reshape(Y.shape[0], nb, blk).transpose(1, 2, 0)              # (nb, blk, nrx)
    G = np.conj(A).transpose(0, 2, 1) @ A + 1e-6 * np.eye(C); rhs = np.conj(A).transpose(0, 2, 1) @ y
    H = np.linalg.solve(G, rhs)                                                      # (nb, C, nrx)
    e = y - A @ H; res = np.mean(np.abs(e) ** 2, 1) * blk / max(blk - C, 1); pw = np.mean(np.abs(y) ** 2, 1)
    return H, res, pw

def main(a):
    ap = argparse.ArgumentParser(); ap.add_argument('cap'); ap.add_argument('--ssb-mhz', type=float, required=True); ap.add_argument('--pointa-hz', type=float, required=True)
    ap.add_argument('--pcis', required=True); ap.add_argument('--cfo', type=float, default=-4800.0); ap.add_argument('--coreset-rb0', type=int, default=2)
    ap.add_argument('--nchunks', type=int, default=50); ap.add_argument('--blk-rb', type=int, default=3); ap.add_argument('--rb', type=int, default=60)
    ap.add_argument('--taus', default='', help='per-cell timing slope (samples @122.88, same order as --pcis), e.g. 0,-64,-80,16; default: estimated')
    ap.add_argument('--m0', type=int, default=0, help='DM-RS sequence index of the first RE (0: reference = CORESET#0 lowest RB; 12*rb0/2: CRB 0)')
    ap.add_argument('--out', default='cochannel.npz'); ap.add_argument('--ls-only-slots', default='', help='comma list of slot numbers (0..39) to process; default all')
    o = ap.parse_args(a); pcis = [int(x) for x in o.pcis.split(',')]
    cap = S.Cap(o.cap); nch = min(o.nchunks, cap.n // CH); 
    t0 = time.time(); T, F, tf, ib0 = frame_timing(cap, (o.ssb_mhz - cap.c) * 1e6 - 30e3, pcis[0], nch)  # band centre = SSB sub-carrier 119 (fastscan convention)
    print(f'frame timing (PCI {pcis[0]} strongest SSB index {ib0}): t0 {tf} (7.68 MS/s), per-chunk drift {T[0]}..{T[-1]} (slope {(T[-1] - T[0]) / max(nch - 1, 1):.2f} samples/20 ms), {time.time() - t0:.1f} s', file=sys.stderr)
    g = Grid(cap, o.pointa_hz, o.cfo, nsc=12 * (o.rb + o.coreset_rb0)); blk = 6 * o.blk_rb; nre = 6 * o.rb; nbk = nre // blk
    slots = [int(x) for x in o.ls_only_slots.split(',')] if o.ls_only_slots else list(range(40))
    nrb0 = o.coreset_rb0; kk = 2 * np.arange(nre); ncell = len(pcis)
    def get(c, n, l):
        a0 = c * CH + 16 * T[c] + body(n, l)
        if a0 - 300 < 0 or a0 + N + 300 > cap.n: return None
        return np.stack([g.sym(a0, ch) for ch in range(cap.nch)])[:, 12 * nrb0 + kk]
    # per-cell timing slope (samples @122.88 relative to the SSB-derived frame): coherent correlation over tau, summed over the DL slots 10..19
    taus = np.arange(-320, 321, 16); Ph = np.exp(2j * np.pi * taus[:, None] * kk[None, :] / N); TA = np.zeros((ncell, len(taus)))
    for c in range(min(10, nch)):
        for n in range(10, 20):
            for l in range(1, 14):
                Yd = get(c, n, l)
                if Yd is None: continue
                for i, p in enumerate(pcis):
                    TA[i] += (np.abs(Ph @ (np.conj(dmrs_pdsch(p, n % 20, l, o.m0, nre))[:, None] * Yd.T)) ** 2).sum(1)
    tau = taus[np.argmax(TA, 1)].astype(float)
    if o.taus: tau = np.array([float(x) for x in o.taus.split(',')])
    print('timing slope tau (samples @122.88) per cell', dict(zip(pcis, tau)), 'peak/median', np.round(TA.max(1) / np.median(TA, 1), 2), file=sys.stderr)
    H = np.zeros((nch, 40, 14, nbk, ncell, cap.nch), np.complex64); RES = np.zeros((nch, 40, 14, nbk, cap.nch), np.float32); PW = np.zeros_like(RES)
    for c in range(nch):
        for n in slots:
            for l in range(1, 14):
                Yd = get(c, n, l)
                if Yd is None: continue
                R = np.stack([dmrs_pdsch(p, n % 20, l, o.m0, nre) * np.exp(-2j * np.pi * tau[i] * kk / N) for i, p in enumerate(pcis)])
                H[c, n, l], RES[c, n, l], PW[c, n, l] = joint_ls(Yd, R, blk)
        print(f'chunk {c + 1}/{nch} {time.time() - t0:.0f} s', file=sys.stderr, end='\r')
    np.savez_compressed(o.out, H=H, RES=RES, PW=PW, T=T, pcis=np.array(pcis), tau=tau, slots=np.array(slots), blk_rb=o.blk_rb)
    print('saved', o.out, f'{time.time() - t0:.0f} s', file=sys.stderr)

if __name__ == '__main__': main(sys.argv[1:])
