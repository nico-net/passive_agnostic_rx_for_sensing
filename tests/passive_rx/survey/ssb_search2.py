#!/usr/bin/env python3
"""ssb_search2.py : multi-channel, long-accumulation SSB/PSS search + SSS->PCI + PBCH-DMRS i_ssb, plus PSD / CP-OFDM / TDD tools.
usage: ssb_search2.py ssb CAPDIR [--dur 1] [--start 0] [--scs 30|15|both] [--nch 4] [--top 10]
       ssb_search2.py psd CAPDIR...      ssb_search2.py cp CAPDIR LO_MHz HI_MHz      ssb_search2.py tdd CAPDIR LO_MHz HI_MHz
CAPDIR holds rx0..rx3.sc16 (sc16 interleaved) + capture_info.json.  Offline analysis only.
Method: 20 ms chunks; per chunk FFT, each GSCN band (+-fs/2) is cut out in frequency domain (= shift+decimate), PSS correlation done by FFT,
|cc|^2 summed over channels (non-coherent) and over chunks folded modulo k*20 ms (k=1,2,4,8; SSB period 20..160 ms).
Metric = folded peak / folded median (dB) minus the null (median of that metric over all raster points of the scan).
shortcut: no CFO search (PSS tolerates ~+-6 kHz at 30 kHz SCS, ~1.5 dB loss); LTE only PSS (no SSS); one SSB (strongest) per candidate."""
import numpy as np, scipy.fft as sf, sys, json, os, glob
FS = 122.88e6; NF = 256; CP = 18; CH = 2457600  # 20 ms chunk
def mseq(taps, init, n=127):
    x = list(init) + [0]*n
    for i in range(n): x[i+7] = (x[i+taps]+x[i]) % 2
    return np.array(x[:n])
xp = mseq(4, [0,1,1,0,1,1,1]); x0 = mseq(4, [1,0,0,0,0,0,0]); x1 = mseq(1, [1,0,0,0,0,0,0])
def pss(n2): return 1-2*xp[(np.arange(127)+43*n2) % 127]
def sss(n1, n2):
    m0 = 15*(n1//112)+5*n2; m1 = n1 % 112; n = np.arange(127)
    return (1-2*x0[(n+m0) % 127])*(1-2*x1[(n+m1) % 127])
SS = np.array([[sss(a, b) for a in range(336)] for b in range(3)])
def tsym(bins, vals):
    X = np.zeros(NF, complex); X[np.asarray(bins) % NF] = vals; return np.fft.ifft(X)*np.sqrt(NF)
def fsym(y): return np.fft.fft(y)[np.arange(-63, 64) % NF]/np.sqrt(NF)
def lte_pss(u):
    n = np.arange(31); a = np.exp(-1j*np.pi*u*n*(n+1)/63); n = np.arange(31, 62); b = np.exp(-1j*np.pi*u*(n+1)*(n+2)/63)
    return np.concatenate([a, b])
LBINS = np.r_[np.arange(-31, 0), np.arange(1, 32)]
T_NR = [tsym(np.arange(-63, 64), pss(i)) for i in range(3)]
T_LTE = [tsym(LBINS, lte_pss(u)) for u in (25, 29, 34)]

class Cap:
    def __init__(s, d, nch=4):
        s.d = d.rstrip('/'); s.info = json.load(open(s.d+'/capture_info.json')); s.nch = nch
        s.c = s.info['rf_channels'][0]['frequency_hz']/1e6
        s.mm = [np.memmap(f'{s.d}/rx{i}.sc16', dtype=np.int16, mode='r').reshape(-1, 2) for i in range(nch)]
        s.n = len(s.mm[0])
    def x(s, ch, a, b):
        r = s.mm[ch][a:b]; return (r[:, 0].astype(np.float32)+1j*r[:, 1].astype(np.float32)).astype(np.complex64)
def bandidx(off_hz, D, N=CH):
    L = N//D; kc = int(round(off_hz/(FS/N)))
    fb = np.fft.fftfreq(L, 1/L).astype(int)  # standard FFT order bin numbers
    return (kc+fb) % N
def pass1(cap, c0, nchunks, D, templates, fold_chunks, grid_hz, nrm=None, old=False):
    """returns acc[g] : (slots, nT, L) summed over channels; slot=chunk%fold_chunks"""
    L = CH//D; nT = len(templates); G = len(grid_hz)
    Tf = [np.conj(sf.fft(t.astype(np.complex64), L)) for t in templates]
    idx = [bandidx(o, D) for o in grid_hz]
    acc = np.zeros((G, fold_chunks, nT, L), np.float32); o6 = np.zeros((G, nT, L), np.float32) if old else None
    for c in range(nchunks):
        a = (c0+c)*CH
        for ch in range(cap.nch):
            X = sf.fft(cap.x(ch, a, a+CH), workers=4)
            for g in range(G):
                S = X[idx[g]]
                for t in range(nT):
                    p = np.abs(sf.ifft(S*Tf[t], workers=2))**2
                    acc[g, c % fold_chunks, t] += p
                    if old and ch == 0 and c < 6: o6[g, t] = np.maximum(o6[g, t], p)
        print(f'  chunk {c+1}/{nchunks}', file=sys.stderr, end='\r')
    return acc, o6
def foldmetric(a, kmax):
    """a (slots,nT,L) -> best (metric_dB, k, s', t, n)"""
    best = None
    for k in (1, 2, 4, 8):
        if k > kmax: break
        F = np.stack([a[s::k].sum(0) for s in range(k)])  # (k,nT,L)
        for t in range(F.shape[1]):
            v = F[:, t]; med = np.median(v); i = np.unravel_index(np.argmax(v), v.shape)
            m = 10*np.log10(v[i]/med)
            if best is None or m > best[0]: best = (m, k, i[0], t, i[1])
    return best
def gold(cinit, n):
    N = 1600+n; x1_ = np.zeros(N+31, int); x2_ = np.zeros(N+31, int); x1_[0] = 1
    for i in range(31): x2_[i] = (cinit >> i) & 1
    for i in range(N): x1_[i+31] = (x1_[i+3]+x1_[i]) % 2; x2_[i+31] = (x2_[i+3]+x2_[i+2]+x2_[i+1]+x2_[i]) % 2
    return (x1_[1600:1600+n]+x2_[1600:1600+n]) % 2
def dmrs(pci, ib):
    ci = (1 << 11)*(ib+1)*(pci//4+1)+(1 << 6)*(ib+1)+(pci % 4); c = gold(ci, 288)
    return ((1-2*c[0::2])+1j*(1-2*c[1::2]))/np.sqrt(2)
def pass2(cap, c0, nchunks, D, off_hz, n2, k, sp, n, scs):
    """SSS->PCI and PBCH DMRS i_ssb on the folded occurrences of one 30 kHz-numerology candidate."""
    L = CH//D; idx = bandidx(off_hz, D); sc_tot = np.zeros((3, 336)) ; dsum = np.zeros(3)
    sss_sc = {dp: np.zeros(336) for dp in (-1, 0, 1)}; occ = []; hz = {dp: [] for dp in (-1, 0, 1)}
    ys = {}
    for c in range(nchunks):
        if c % k != sp: continue
        a = (c0+c)*CH
        y = [sf.ifft(sf.fft(cap.x(ch, a, a+CH), workers=4)[idx], workers=4) for ch in range(cap.nch)]
        if n+4*(NF+CP) + 3 >= L or n < 2: continue
        occ.append(y)
        for dp in (-1, 0, 1):
            for yy in y:
                q = n+dp; Hp = fsym(yy[q:q+NF])*np.conj(pss(n2)); Z = fsym(yy[q+2*(NF+CP):q+2*(NF+CP)+NF])
                sss_sc[dp] += np.abs(SS[n2] @ np.conj(Z*np.conj(Hp)))**2; hz[dp].append((Hp, Z))
    if not occ: return None
    tot = sum(sss_sc.values()); dpb = max(sss_sc, key=lambda d: sss_sc[d].max()); sc = sss_sc[dpb]
    o = np.sort(sc); pci = 3*int(sc.argmax())+n2; ratio = float(np.sqrt(o[-1]/(o[-2]+1e-30)))
    # DMRS: SSB subcarrier j -> bin j-119
    k240 = np.arange(240); v = pci % 4
    pos1 = k240[(k240 % 4) == v]; pos2 = pos1[(pos1 < 48) | (pos1 >= 192)]
    # CFO (Hz, signal above the cut-out centre): PSS->SSS channel-phase slope, symbols 0 and 2 = 2*(NF+CP) samples apart, unambiguous +-fs/(2*2*(NF+CP))
    acc = sum(np.sum(Z*np.conj(SS[n2][pci//3])*np.conj(Hp)) for Hp, Z in hz[dpb])
    cfo = float(np.angle(acc)/(2*np.pi*2*(NF+CP)/(FS/D)))
    best_tau = None; sc_i = None
    for tau in np.arange(-1, 1.01, 0.25):
        s = np.zeros(8)
        for y in occ:
            for yy in y:
                for i in range(8):
                    r = dmrs(pci, i) if True else None
                    for si, (sym, pos, rr) in enumerate(((1, pos1, r[:60]), (2, pos2, r[60:84]), (3, pos1, r[84:144]))):
                        q = n+dpb+sym*(NF+CP); Y = np.fft.fft(yy[q:q+NF])/np.sqrt(NF)
                        b = (pos-119) % NF; ph = np.exp(-2j*np.pi*tau*((pos-119)))/1.0
                        ph = np.exp(-2j*np.pi*tau*(pos-119)/NF)
                        s[i] += np.abs(np.sum(Y[b]*ph*np.conj(rr)))**2
        if best_tau is None or s.max() > sc_i.max(): best_tau, sc_i = tau, s
    so = np.sort(sc_i)
    return dict(pci=pci, sss_ratio=ratio, n_occ=len(occ), i_ssb=int(sc_i.argmax()), dmrs_ratio=float(np.sqrt(so[-1]/(so[-2]+1e-30))),
                dmrs_vs_med=float(np.sqrt(so[-1]/(np.median(so)+1e-30))), tau=best_tau, cfo_hz=cfo)
def ssb(d, dur=1.0, start=0.0, scs='both', nch=4, top=10):
    cap = Cap(d, nch); c0 = int(start/0.02); nchunks = min(int(round(dur/0.02)), cap.n//CH-c0)
    print(f'## {cap.d} centre {cap.c} MHz  {nchunks*20} ms x {nch} ch')
    rows = []
    if scs in ('30', 'both'):
        D = 16; fs = FS/D; lim = 61.44*0.82
        N0 = int(np.ceil((cap.c-lim-3000)/1.44)); N1 = int((cap.c+lim-3000)/1.44); Ns = np.arange(N0, N1+1)
        fg = 3000+Ns*1.44; off = (fg-cap.c)*1e6-30e3
        acc, o6 = pass1(cap, c0, nchunks, D, T_NR, 8, off, old=True)
        res = [foldmetric(acc[g], nchunks) for g in range(len(Ns))]
        old = [10*np.log10(o6[g].max()/np.median(o6[g][np.argmax(o6[g].max(1))])) for g in range(len(Ns))]
        null = np.median([r[0] for r in res])
        print(f'30 kHz raster: {len(Ns)} GSCNs, null (median folded peak/med) {null:.2f} dB; old-style(ch0, 120 ms, no fold) median {np.median(old):.2f} dB')
        order = np.argsort([-r[0] for r in res])
        for g in order[:top]:
            m, k, sp, t, n = res[g]; r2 = pass2(cap, c0, nchunks, D, off[g], t, k, sp, n, 30) if m-null > 1.0 else None
            rows.append(('NR30', fg[g], 7499+Ns[g], m-null, old[g]-np.median(old), k, n/fs*1e3+sp*20, r2))
            print(f'  f={fg[g]:.2f} GSCN={7499+Ns[g]} peak/med {m:.2f} dB (null+{m-null:.2f}) old {old[g]:.2f} fold={k*20}ms N2={t} t={n/fs*1e3+sp*20:.3f}ms', r2 if r2 else '')
    if scs in ('15', 'both'):
        D = 32; fs = FS/D; L = CH//D; lim = 61.44*0.82; n15 = min(nchunks, 10)
        fr = np.arange(np.ceil((cap.c-lim)*10), np.floor((cap.c+lim)*10)+1)/10  # 100 kHz raster
        off = (fr-cap.c)*1e6
        acc, _ = pass1(cap, c0, n15, D, T_NR+T_LTE, 1, off)
        for name, sl, per in (('NR15', slice(0, 3), 1), ('LTE', slice(3, 6), 4)):
            res = []
            for g in range(len(fr)):
                a = acc[g, 0, sl]
                if per > 1: a = a.reshape(3, per, L//per).sum(1)
                med = np.median(a); i = np.unravel_index(np.argmax(a), a.shape); res.append((10*np.log10(a[i]/med), i[0], i[1]))
            null = np.median([r[0] for r in res]); order = np.argsort([-r[0] for r in res])
            print(f'{name} (15 kHz, 100 kHz raster, first {n15*20} ms): null {null:.2f} dB')
            for g in order[:top]:
                print(f'  f={fr[g]:.1f} MHz peak/med {res[g][0]:.2f} dB (null+{res[g][0]-null:.2f}) id={res[g][1]} t={res[g][2]/fs*1e3:.3f}ms')
                rows.append((name, fr[g], None, res[g][0]-null, 0, 0, 0, None))
    return rows
def welch(cap, n=4096, dur=0.5):
    p = 0
    for ch in range(cap.nch):
        x = cap.x(ch, 0, int(dur*FS)); m = len(x)//n
        p = p+np.mean(np.abs(np.fft.fftshift(np.fft.fft(x[:m*n].reshape(m, n)*np.hanning(n), axis=1), axes=1))**2, axis=0)
    return (np.arange(n)-n//2)*FS/n/1e6, 10*np.log10(p/cap.nch+1e-9)
def psd(dirs):
    for d in dirs:
        cap = Cap(d); f, db = welch(cap); fl = np.percentile(db, 5)
        s = ''
        for lo in np.arange(-60, 60, 2.0):
            k = (f >= lo) & (f < lo+2); s += f'{cap.c+lo+1:.0f}:{db[k].mean()-fl:+.0f} '
        print(f'## {d} centre {cap.c} floor(5th pct) {fl:.1f} dB\n{s}')
def subband(cap, lo, hi, D=4, dur=1.0):
    """band lo..hi MHz -> baseband at FS/D (complex), ch0 only"""
    n = int(dur*FS)//CH*CH; out = []
    c = (lo+hi)/2; W = FS/D
    for a in range(0, n, CH):
        X = sf.fft(cap.x(0, a, a+CH), workers=4); idx = bandidx((c-cap.c)*1e6, D)
        out.append(sf.ifft(X[idx], workers=4))
    x = np.concatenate(out)
    # keep only the requested bandwidth (zero the rest)
    X = sf.fft(x); f = np.fft.fftfreq(len(x), 1/W)/1e6; X[np.abs(f) > (hi-lo)/2] = 0
    return sf.ifft(X), W
def cp(d, lo, hi):
    cap = Cap(d); x, fs = subband(cap, lo, hi); x = x/np.sqrt(np.mean(np.abs(x)**2)); out = {}
    for name, N, cpl in (('15kHz', int(fs/15e3), int(fs/15e3*144/2048)), ('30kHz', int(fs/30e3), int(fs/30e3*144/2048))):
        r = x[:-N]*np.conj(x[N:]); cs = np.concatenate([[0], np.cumsum(r)]); s = np.abs(cs[cpl:]-cs[:-cpl])/cpl
        slot = int(round(fs*0.5e-3)); seg_n = 200*slot; mets = []  # 100 ms segments (clock drift smears longer folds)
        for a in range(0, len(s)-seg_n+1, seg_n):
            seg = s[a:a+seg_n]
            F = seg.reshape(-1, slot).mean(0); mets.append(F.max()/np.median(F))
        out[name] = (np.mean(mets), np.max(mets), s.mean())
        print(f'{name} CP-corr {lo}-{hi} MHz: folded(0.5 ms, 100 ms segs) peak/median mean {np.mean(mets):.2f} max {np.max(mets):.2f}; mean |rho| {s.mean():.4f}')
    return out
def tdd(d, lo, hi):
    cap = Cap(d); x, fs = subband(cap, lo, hi); p = np.abs(x)**2; b = int(fs*0.25e-3); m = len(p)//b
    q = p[:m*b].reshape(m, b).mean(1); per = 40  # 10 ms in 0.25 ms bins
    F = q[:m//per*per].reshape(-1, per).mean(0); fl = np.percentile(q, 2)
    print(f'## {d} {lo}-{hi} MHz power vs time folded at 10 ms (0.25 ms bins, dB re 2nd pct)')
    print(' '.join(f'{v:.1f}' for v in 10*np.log10(F/fl)))
    r = 10*np.log10(F/fl); print(f'fold contrast max-min {r.max()-r.min():.2f} dB; non-folded 10 ms-lag autocorr of envelope:',
        float(np.corrcoef(q[:-per], q[per:])[0, 1]))
if __name__ == '__main__':
    a = sys.argv
    if a[1] == 'ssb':
        kw = {}; i = 3
        while i < len(a): kw[a[i].lstrip('-')] = a[i+1]; i += 2
        ssb(a[2], float(kw.get('dur', 1)), float(kw.get('start', 0)), kw.get('scs', 'both'), int(kw.get('nch', 4)), int(kw.get('top', 10)))
    elif a[1] == 'psd': psd(a[2:])
    elif a[1] == 'cp': cp(a[2], float(a[3]), float(a[4]))
    elif a[1] == 'tdd': tdd(a[2], float(a[3]), float(a[4]))
