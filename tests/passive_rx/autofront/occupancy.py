#!/usr/bin/env python3
"""occupancy.py CAPDIR LO_MHZ HI_MHZ [--seconds 0.27] : is there DL data traffic on a carrier block? (offline, no radio)
Per 4096-sample window (one 30 kHz symbol, 33 us), 4-antenna energy per PRB over [LO, HI]; a PRB is lit when it exceeds 4x (6 dB) its own median
level over time (10th percentile / 0.85 = the quiet symbols: UL or idle); lit fraction f[t] = lit PRBs / PRBs.
Reported: share of windows with f >= 0.5 (wide PDSCH or a wideband reference symbol), 0.1 <= f < 0.5 (partial allocation, the 20-PRB SSB is f ~ 0.4 on a
51-PRB block), f < 0.1; windows per second with f >= 0.5. A TRS/CSI-RS symbol pair is < 0.5 % of the windows, so a share well above that is PDSCH.
Low-order statistic, not a decoder: it cannot say WHICH cell a PDSCH belongs to (508 / PCI 4 / 614 share a block)."""
import sys, json, numpy as np

def lit_fractions(cap, lo_hz, hi_hz, seconds=0.27, nch=4, n=4096):
    info = json.load(open(cap + '/capture_info.json')); fs = float(info['sample_rate_hz']); c = float(info['rf_channels'][0]['frequency_hz'])
    cnt = int(seconds * fs) // n * n; f = c + (np.arange(n) - n // 2) * fs / n; sel = np.where((f >= lo_hz) & (f < hi_hz))[0]
    nrb = len(sel) // 12; sel = sel[:nrb * 12]; e = 0
    for ch in range(nch):
        x = np.fromfile(f'{cap}/rx{ch}.sc16', dtype=np.int16, count=2 * cnt).astype(np.float32).reshape(-1, 2); x = x[:, 0] + 1j * x[:, 1]
        X = np.fft.fftshift(np.fft.fft(x.reshape(-1, n), axis=1), axes=1)[:, sel]; e = e + (np.abs(X) ** 2).reshape(-1, nrb, 12).sum(2)
    return lit_from_energy(e)

def lit_from_energy(e, db=6.0):
    """e (windows, PRBs) -> lit fraction per window"""
    base = np.percentile(e, 10, axis=0) / 0.85; return (e > base * 10 ** (db / 10)).mean(axis=1)

def summary(fr, seconds):
    return dict(windows=len(fr), wide=round(float((fr >= 0.5).mean()), 4), partial=round(float(((fr >= 0.1) & (fr < 0.5)).mean()), 4),
                idle=round(float((fr < 0.1).mean()), 4), wide_per_s=round(float((fr >= 0.5).sum() / seconds), 1))

def selftest():
    rng = np.random.default_rng(0); e = rng.gamma(48.0, 1 / 48, (4000, 51)); e[:1000, :] *= 30                          # 25 % of windows: all PRBs lit
    e[1000:1400, :20] *= 30                                                                                             # 10 %: a 20-PRB SSB-like block
    s = summary(lit_from_energy(e), 0.27); assert 0.22 < s['wide'] < 0.28 and 0.09 < s['partial'] < 0.12, s
    s = summary(lit_from_energy(rng.gamma(48.0, 1 / 48, (4000, 51))), 0.27); assert s['wide'] == 0 and s['idle'] > 0.9, s   # noise only
    print('ok occupancy', s)

if __name__ == '__main__':
    if len(sys.argv) == 1: selftest()
    else:
        sec = float(sys.argv[sys.argv.index('--seconds') + 1]) if '--seconds' in sys.argv else 0.27
        print(json.dumps(summary(lit_fractions(sys.argv[1], float(sys.argv[2]) * 1e6, float(sys.argv[3]) * 1e6, sec), sec)))
