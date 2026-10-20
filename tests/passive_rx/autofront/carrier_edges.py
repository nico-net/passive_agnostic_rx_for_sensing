#!/usr/bin/env python3
"""carrier_edges.py CAPDIR SSB_MHZ K_SSB [--nch 4] [--seconds 0.27] : blind carrier geometry of a cell WITHOUT SIB1 carrier info
(NSA-style SIB1: PCI 403; no SIB1 at all: PCI 4 beside 508) from the occupied-bandwidth block that contains its SSB.

Method: 4-antenna average PSD (30 kHz bins) of the wideband capture, 360 kHz (= 1 PRB) moving average, noise floor = median of the lowest
quartile of the central 100 MHz, block = contiguous region around the SSB above the half-way level between the floor and the block's own
median level (half-power edge). Point A lies on the cell's CRB grid: SSB subcarrier 0 minus k_ssb/2 subcarriers (30 kHz) minus an integer
number of PRBs (the MIB fixes the phase, the edge fixes the integer, +-1 PRB). Output: lower edge, width, Point A, the supported receiver
size >= width (51/106/133/162/189/217/273) and the carrier-sized grid triple (-C, -r, --ssb) with Point A at grid sc 0, which is the
precondition of the SIB1-free ISAC_ACQ_CARRIER_FROM_RS path. Validated against SIB1 carriers in PCIS-20261013.md. Offline, no radio."""
import sys, json, numpy as np

SCS = 30e3
SIZES = (51, 106, 133, 162, 189, 217, 273)

def psd(cap, nch=4, seconds=0.27, n=4096):
    """-> (freq_hz relative to the capture centre, linear PSD averaged over antennas), 30 kHz bins"""
    info = json.load(open(cap + '/capture_info.json')); fs = float(info['sample_rate_hz']); c = float(info['rf_channels'][0]['frequency_hz'])
    cnt = int(seconds * fs) // n * n; acc = np.zeros(n); w = np.hanning(n)
    for ch in range(nch):
        x = np.fromfile(f'{cap}/rx{ch}.sc16', dtype=np.int16, count=2 * cnt).astype(np.float32).reshape(-1, 2); x = x[:, 0] + 1j * x[:, 1]
        acc += np.mean(np.abs(np.fft.fftshift(np.fft.fft(x.reshape(-1, n) * w, axis=1), axes=1)) ** 2, axis=0)
    return c + (np.arange(n) - n // 2) * fs / n, acc / nch

def block(f, p, ssb_hz, win_hz=1.5e6, reach_hz=(55e6, 70e6), min_db=6.0, rel=0.5):
    """occupied block around ssb_hz from PSD steps -> (lo_hz, hi_hz, step_lo_db, step_hi_db); None edges when no strong step.
    step(f) = mean dB over [f, f+win) minus mean dB over [f-win, f). Lower edge = the NEAREST rise below the SSB that is >= max(min_db, rel x the strongest rise
    within reach); upper edge = the nearest fall above it. The rise at an edge of a loaded carrier is 12-27 dB; below ~8 dB (weak / idle carrier) the edge is
    not trustworthy (TIM 675: 3-6 dB, edge 0.8 MHz off)."""
    df = f[1] - f[0]; w = max(2, int(win_hz / df))
    sm = 10 * np.log10(np.convolve(p, np.ones(4) / 4, mode='same') + 1e-9)
    cs = np.concatenate([[0.0], np.cumsum(sm)]); step = np.full(len(f), 0.0)
    N = len(f); step[w:N - w + 1] = (cs[2 * w:N + 1] - 2 * cs[w:N - w + 1] + cs[0:N - 2 * w + 1]) / w
    def edge(sel, sign):
        idx = np.where(sel)[0]
        if len(idx) == 0: return None, 0.0
        pk = sign * step[idx]; thr = max(min_db, rel * pk.max())
        cand = [i for i, k in zip(idx, pk) if k >= thr and k >= sign * step[max(i - 1, 0)] and k >= sign * step[min(i + 1, len(f) - 1)]]
        if not cand: return None, 0.0
        i = max(cand, key=lambda i: f[i]) if sign > 0 else min(cand, key=lambda i: f[i])      # nearest to the SSB
        return float(f[i]), float(step[i])
    lo, sl = edge((f > ssb_hz - reach_hz[0]) & (f < ssb_hz - 0.3e6), +1)
    hi, sh = edge((f > ssb_hz + 0.3e6) & (f < ssb_hz + reach_hz[1]), -1)
    return lo, hi, sl, sh

def geometry(lo_hz, hi_hz, ssb_hz, k_ssb):
    """block edges + MIB k_ssb -> Point A on the CRB grid, PRB width, supported size, carrier-sized grid triple."""
    sc0 = ssb_hz - 120 * SCS                                                   # SSB subcarrier 0 (240 subcarriers centred on the GSCN frequency)
    phase = sc0 - (k_ssb / 2.0) * SCS                                          # CRB boundary below the SSB (k_ssb in 15 kHz units)
    n = int(round((phase - lo_hz) / (12 * SCS)))                               # PRBs between Point A and the SSB's CRB boundary
    point_a = phase - n * 12 * SCS
    width_prb = int(round((hi_hz - point_a) / (12 * SCS))) if hi_hz else 0
    size = next((s for s in SIZES if s >= width_prb), SIZES[-1]) if width_prb else None
    centre = point_a + size * 6 * SCS if size else None
    return dict(point_a_hz=round(point_a), lower_edge_hz=round(lo_hz), upper_edge_hz=round(hi_hz) if hi_hz else None, width_prb=width_prb, size=size,
                C_hz=round(centre) if centre else None, ssb=int(round((sc0 - point_a) / SCS)), prb_below_ssb=n)

def main(a):
    cap, ssb, k = a[1], float(a[2]) * 1e6, int(a[3]); nch = int(a[a.index('--nch') + 1]) if '--nch' in a else 4
    sec = float(a[a.index('--seconds') + 1]) if '--seconds' in a else 0.27
    f, p = psd(cap, nch, sec); lo, hi, sl, sh = block(f, p, ssb)
    if lo is None: sys.exit('no lower carrier edge (idle / weak carrier): use SIB1 or the Point-A sweep')
    g = geometry(lo, hi, ssb, k); g['rise_db'] = round(sl, 1); g['fall_db'] = round(sh, 1); g['confident'] = sl >= 10 and bool(hi) and sh <= -10; print(json.dumps(g))

def selftest():
    n = 4096; fs = 122.88e6; f = np.arange(n) * fs / n - fs / 2 + 3610e6
    p = np.full(n, 1.0); sel = (f >= 3620.82e6) & (f < 3639.18e6); p[sel] = 30.0; p[(f > 3624.9e6) & (f < 3628.5e6)] = 60.0     # 51 PRB block, SSB on top of it
    p[f < 3600e6] *= 8                                                                                                # a stronger neighbour below: the NEAREST rise wins
    lo, hi, sl, sh = block(f, p, 3624.96e6); g = geometry(lo, hi, 3624.96e6, 12)
    assert abs(lo - 3620.82e6) < 100e3 and abs(hi - 3639.18e6) < 100e3, (lo, hi)
    assert g['width_prb'] in (50, 51, 52) and g['size'] == 51 and g['ssb'] % 12 == 6, g
    assert (g['point_a_hz'] - (3624.96e6 - 126 * SCS)) % (12 * SCS) in (0, 12 * SCS) or abs((g['point_a_hz'] - (3624.96e6 - 126 * SCS)) % (12 * SCS) - 12 * SCS) < 1, g
    assert block(f, np.ones(n), 3624.96e6)[0] is None                                                                  # idle: no edge
    print('ok carrier_edges', g)

if __name__ == '__main__':
    selftest() if len(sys.argv) == 1 else main(sys.argv)
