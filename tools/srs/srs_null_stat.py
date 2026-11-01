#!/usr/bin/env python3
"""Offline: the production self-normalised comb statistic (nr_srs_td_comb_gate, per antenna-symbol Gamma(1) under 'no comb') on the uplink symbols 8..13 of a raw capture.
usage: srs_null_stat.py <capture dir> <slot0 sample of a 10-slot pattern> [seconds]; prints quantiles and the strongest symbols and their slot-in-period."""
import sys, numpy as np
d = sys.argv[1]; slot0 = int(sys.argv[2]); secs = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
import json
info = json.load(open(d + "/capture_info.json")); fs = info["sample_rate_hz"]; N = 4096; SLOT = fs // 2000
nmax = info["samples_per_channel"]; n = min(nmax, int(secs * fs))
x = np.stack([np.memmap(f"{d}/rx{a}.sc16", dtype="<i2", mode="r", shape=(nmax, 2))[:n].astype(np.float32).view(np.complex64).reshape(-1) if False else
              (lambda r: r[:n, 0].astype(np.float32) + 1j * r[:n, 1].astype(np.float32))(np.memmap(f"{d}/rx{a}.sc16", dtype="<i2", mode="r", shape=(nmax, 2))) for a in range(4)])
cp = lambda l: 352 if l % 7 == 0 else 288
so = np.cumsum([0] + [N + cp(l) for l in range(14)])
def cc(a, lag, m, off):
    p = a[:, off:off + m]; q = a[:, off + lag:off + lag + m]
    p = p - p.mean(1, keepdims=True); q = q - q.mean(1, keepdims=True)
    return np.abs((p * np.conj(q)).sum(1)) ** 2 / m
out = []
for g in range((n - slot0) // SLOT - 1):
    si = g % 10
    if si not in (7, 8, 9): continue
    for l in range(10 if si == 7 else 8, 14):
        s0 = slot0 + g * SLOT + so[l] + cp(l) // 2
        if s0 + N > n: continue
        a = x[:, s0:s0 + N]
        r = cc(a, 2048, 1024, 512) / np.mean([cc(a, 2048 + dl, 256, 512) for dl in (101, -173, 251, -331)], 0)
        out.append((g, si, l, (4 * np.log1p(r / 4)).sum()))   # Gamma(4) per symbol (4 antennas)
o = np.array(out)
ex = (o[:, 3] - 4) / 2.0                      # (sum - K)/sqrt(K), K=4: the single-symbol gate variable (opens at 9)
print(f"{d.split('/')[-1]}: {len(o)} uplink symbols (8..13) in {n/fs:.1f} s; single-symbol excess (sum-K)/sqrt(K): median {np.median(ex):.2f} p99 {np.percentile(ex,99):.2f} p99.9 {np.percentile(ex,99.9):.2f} max {ex.max():.2f}; >9 (gate): {(ex>9).sum()}")
top = np.argsort(-ex)[:8]
print("  strongest (slot, slot-in-period, symbol, excess):", [(int(o[i,0]), int(o[i,1]), int(o[i,2]), round(float(ex[i]),1)) for i in top])
