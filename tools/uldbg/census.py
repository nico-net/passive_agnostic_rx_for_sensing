#!/usr/bin/env python3
"""UL-DBG census subcommands (see ULDBG.md for results).  All reads stream from np.memmap.
  census.py tdd    <cap> <frames>          per-symbol power of one frame + UL PRB occupancy
  census.py cp     <cap> <frame> <slot> <prb0> <prb1>   15 vs 30 kHz CP structure in a band
  census.py pairs  <cap> <fr0:fr1>         blind DM-RS (ID 415, syms 2+11) per UL slot; comb1 = control
  census.py grants <cap> <replay run.log> <out.npy>   DM-RS SNR at each DCI 0_1's predicted PRBs vs control
"""
import sys, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ulphys import *

UL = (8, 9, 18, 19)


def tdd(cap, nfr):
    F0 = CAPS[cap][1]
    x = read(cap, 0, F0, FRAME)
    for s in range(20):
        print('slot %2d' % s, ' '.join('%5.1f' % (10 * np.log10(np.mean(np.abs(grid(x[s * SLOT:], l, 0)) ** 2) + 1e-9))
                                      for l in range(14) if s * SLOT + sym_start(l) + FFT + CP0 < len(x)))
    occ = np.zeros(NPRB); n = 0
    for fr in range(nfr):
        for sl in UL:
            xs = slot_iq(cap, fr, sl)
            P = sum((np.abs(np.array([grid(x, l) for l in range(1, 13)])) ** 2).reshape(12, NPRB, 12).mean((0, 2)) for x in xs)
            occ += P; n += 1
    occ /= n; occ = 10 * np.log10(occ / np.median(occ[120:]))
    print('mean UL power per 10-PRB group (dB rel. PRB120+ median):', ' '.join('%.1f' % occ[i:i + 10].mean() for i in range(0, NPRB, 10)))


def cp(cap, fr, slot, b0, b1):
    """CP-correlation fold in a band: N=8192/CP=576 (15 kHz, LTE-like) vs N=4096/CP=288 (NR 30 kHz)."""
    F0 = CAPS[cap][1]
    f = np.fft.fftfreq(8192) * FS
    m = ((f >= (b0 * 12 - 1302) * 30e3) & (f <= (b1 * 12 - 1302) * 30e3)).astype(float)
    for N, c in ((8192, 576), (4096, 288)):
        acc = np.zeros(SLOT)
        for ch in range(4):
            x = read(cap, ch, F0 + fr * FRAME + slot * SLOT, SLOT + N + c + 8192)
            y = np.concatenate([np.fft.ifft(np.fft.fft(x[i:i + 8192]) * m) for i in range(0, len(x) - 8191, 8192)])
            p = y[:SLOT + c] * np.conj(y[N:N + SLOT + c]); cs = np.cumsum(np.r_[0, p])
            acc += np.abs(cs[c:c + SLOT] - cs[:SLOT])
        ph = np.arange(SLOT) % (N + c)
        fo = np.bincount(ph, weights=acc) / np.bincount(ph)
        print('band PRB %d-%d N=%d CP=%d: fold peak/median %.2f' % (b0, b1, N, c, fo.max() / np.median(fo)))


def pairs(cap, fr0, fr1, SB=24):
    ks_all = {c: np.arange(c, NPRB * 12, 2) for c in (0, 1)}
    rec = []
    for fr in range(fr0, fr1):
        for sl in UL:
            xs = slot_iq(cap, fr, sl)
            Y = {l: np.array([grid(x, l) for x in xs]) for l in (2, 11)}
            for comb in (0, 1):
                ks = ks_all[comb]; m = ks // 2; n = SB * 6
                pw = 0
                for l in (2, 11):
                    z = Y[l][:, ks] * np.conj(dmrs(415, sl, l)[m])[None, :]
                    pw = pw + np.array([(np.abs(np.fft.ifft(z[:, i:i + n], n=512, axis=1)) ** 2).sum(0)
                                        for i in range(0, len(ks) - n + 1, n // 2)])
                r = pw / np.median(pw, axis=1, keepdims=True)
                i, j = np.unravel_index(np.argmax(r), r.shape)
                rec.append((float(r[i, j]), fr, sl, comb, i * SB // 2, j))
    R = np.array(rec)
    for t in (8, 10, 12, 15):
        print('ratio>%d: comb0 (DM-RS) %d   comb1 (control) %d   of %d UL slots' % (
            t, ((R[:, 0] > t) & (R[:, 3] == 0)).sum(), ((R[:, 0] > t) & (R[:, 3] == 1)).sum(), len(R) // 2))
    return R


def grants(cap, log, out):
    F0 = CAPS[cap][1]
    D, ts = parse_dcis(log); OFF = frame_offset(ts, F0)
    rows = []
    for d in D:
        g = decode(d)
        if not g or g['p0'] + g['L'] > NPRB: continue
        fr = g['target'] // 20 - OFF; sl = g['target'] % 20
        if sl not in UL or fr < 0 or fr + 2 >= 1199: continue
        s, tau = dmrs_est(cap, fr, sl, g['p0'], g['p0'] + g['L'])
        c, _ = dmrs_est(cap, fr + 2, sl, g['p0'], g['p0'] + g['L'])
        rows.append((g['abs'], g['rnti'], g['len'], g['p0'], g['L'], g['mcs'], g['rv'], fr, sl, tau, *s, *c))
    R = np.array(rows, float); np.save(out, R)
    mrc = 10 * np.log10(R[:, 10:14].sum(1)); ctl = 10 * np.log10(R[:, 14:18].sum(1))
    print('frame offset abs-cap = %d; grants %d' % (OFF, len(R)))
    for t in (-3, 0, 3, 6, 10):
        print('  DM-RS MRC SNR > %3d dB: grants %4d   control %4d' % (t, (mrc > t).sum(), (ctl > t).sum()))
    g = mrc > 3
    if g.any():
        print('  per-antenna median (grants >3 dB):', [round(float(np.median(10 * np.log10(R[g, 10 + i]))), 1) for i in range(4)])
    print('  MCS histogram all:', np.bincount(R[:, 5].astype(int), minlength=28).tolist())
    print('  MCS<=4: n=%d  >0 dB: %d' % ((R[:, 5] <= 4).sum(), ((R[:, 5] <= 4) & (mrc > 0)).sum()))
    for i in np.argsort(-mrc)[:25]:
        print('   abs %d rnti %04x len %d prb %d+%d mcs %d rv %d -> fr %d sl %d tau %.0f MRC %.1f dB ant %s ctl %.1f' % (
            R[i, 0], int(R[i, 1]), R[i, 2], R[i, 3], R[i, 4], R[i, 5], R[i, 6], R[i, 7], R[i, 8], R[i, 9], mrc[i],
            ' '.join('%.1f' % (10 * np.log10(v)) for v in R[i, 10:14]), ctl[i]))


def energy(cap, log, out):
    """ID- and waveform-agnostic check: mean RE power (4 RX, symbols 0-13) on each grant's predicted PRBs,
    relative to the same PRBs/slot two frames later (control)."""
    F0 = CAPS[cap][1]
    D, ts = parse_dcis(log); OFF = frame_offset(ts, F0)
    rows = []
    for d in D:
        g = decode(d)
        if not g or g['p0'] + g['L'] > NPRB: continue
        fr = g['target'] // 20 - OFF; sl = g['target'] % 20
        if sl not in UL or fr < 0 or fr + 2 >= 1199: continue
        e = []
        for f in (fr, fr + 2):
            xs = slot_iq(cap, f, sl)
            e.append(sum(np.mean([np.mean(np.abs(grid(x, l)[g['p0'] * 12:(g['p0'] + g['L']) * 12]) ** 2) for l in range(14)]) for x in xs))
        rows.append((g['abs'], g['rnti'], g['p0'], g['L'], g['mcs'], fr, sl, e[0], e[1]))
    R = np.array(rows, float); np.save(out, R)
    r = 10 * np.log10(R[:, 7] / R[:, 8])
    print('grants %d; energy grant/control dB: median %.2f' % (len(R), np.median(r)))
    for name, s in (('L<=3', R[:, 3] <= 3), ('L4-9', (R[:, 3] > 3) & (R[:, 3] < 10)), ('L>=10', R[:, 3] >= 10),
                    ('MCS<=4', R[:, 4] <= 4), ('MCS>=17', R[:, 4] >= 17)):
        if s.any():
            print('  %-8s n=%4d median %+.2f dB  >+3 dB %4d  <-3 dB %4d' % (name, s.sum(), np.median(r[s]), (r[s] > 3).sum(), (r[s] < -3).sum()))


if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'tdd': tdd(sys.argv[2], int(sys.argv[3]))
    elif c == 'cp': cp(sys.argv[2], *map(int, sys.argv[3:7]))
    elif c == 'pairs':
        a, b = map(int, sys.argv[3].split(':')); R = pairs(sys.argv[2], a, b)
        if len(sys.argv) > 4: np.save(sys.argv[4], R)
    elif c == 'grants': grants(sys.argv[2], sys.argv[3], sys.argv[4])
    elif c == 'energy': energy(sys.argv[2], sys.argv[3], sys.argv[4])
