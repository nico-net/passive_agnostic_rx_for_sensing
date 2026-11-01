#!/usr/bin/env python3
"""Offline SRS physics check on a raw sc16 capture (read-only). Finds the TDD UL/DL edge from the power envelope, then for every uplink slot measures per-symbol
power of the last 6 symbols against the other UL symbols and against empty spectrum, and looks for comb-2/comb-4 structure and its periodicity across slots.
usage: srs_physics_check.py <capture dir> [seconds=1.0] [offset_s=0.1]   (4096-point FFT, 30 kHz SCS, 217 RB centred, DDDDDDDSUU)"""
import json, sys
import numpy as np

d = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0; off_s = float(sys.argv[3]) if len(sys.argv) > 3 else 0.1
info = json.load(open(d + "/capture_info.json")); fs = info["sample_rate_hz"]; nch = info["channels"]
N = 4096; SLOT = fs // 2000; NRB = 217
n = int(secs * fs); o = int(off_s * fs)
x = []
for a in range(nch):
    r = np.memmap(f"{d}/rx{a}.sc16", dtype="<i2", mode="r", offset=4 * o, shape=(n, 2))
    x.append(r[:, 0].astype(np.float32) + 1j * r[:, 1].astype(np.float32))
x = np.stack(x)  # [ant, n]
# 1. envelope -> UL/DL edge. Period 5 ms (10 slots of 0.5 ms); the uplink is the longest low-power run of each period, its end is the start of DL slot 0.
blk = 128; nb = n // blk
env = (np.abs(x[:, :nb * blk]) ** 2).sum(0).reshape(nb, blk).mean(1)
fb = (fs // 200) // blk  # blocks per 5 ms
nf = nb // fb
fold = env[:nf * fb].reshape(nf, fb)
floor = np.percentile(env, 2)
low = fold.mean(0) < 4 * floor      # 6 dB above the quietest level
ln = []
start = None
for i in range(2 * fb):
    v = low[i % fb]
    if v and start is None: start = i
    if (not v) and start is not None: ln.append((i - start, start, i)); start = None
ln.sort(reverse=True)
L, a, b = ln[0]
print(f"uplink low-power run: {L*blk/fs*1e3:.3f} ms (expect 1.14 ms = 2 slots + 4 symbols) from block {a} to {b}; floor {10*np.log10(floor):.1f} dB")
# per-period rising edge (UL end) refined on the unfolded envelope
edges = []
for f in range(nf):
    seg = fold[f]
    for t in range(b % fb - 2, b % fb + 3):
        if seg[t % fb] > 4 * floor: edges.append(f * fb * blk + (t % fb) * blk); break
e0 = int(np.median(np.array(edges) % (fb * blk)))
print(f"slot 0 starts at sample {e0} (mod 5 ms)")
# 2. slot grid: slot s of a 10-slot half frame starts at e0 + s*SLOT (+ the edge sits inside symbol 0's CP, so back off ~ 100 samples)
start0 = e0 - blk // 2
cp = lambda l: 352 if l % 7 == 0 else 288   # normal CP, 30 kHz at 122.88 Msps (4096 FFT)
sym_off = np.cumsum([0] + [N + cp(l) for l in range(14)])
k0 = N - (NRB * 12) // 2  # first_carrier_offset
bins = (np.arange(NRB * 12) + k0) % N
empty = np.r_[np.arange(60, 360), np.arange(N - 360, N - 60)]  # outside the 78 MHz cell inside the 122.88 MHz capture
def sym_spec(slot_start, l):
    s = slot_start + sym_off[l] + cp(l) // 2  # window in the CP: pure delay
    if s < 0 or s + N > n: return None
    return np.fft.fft(x[:, s:s + N], axis=1)
rows = []
nslots = (n - start0) // SLOT - 1
for g in range(nslots):
    s_in = g % 10
    if s_in not in (7, 8, 9): continue
    ss = start0 + g * SLOT
    first = 10 if s_in == 7 else 0
    for l in range(first, 14):
        X = sym_spec(ss, l)
        if X is None: continue
        P = (np.abs(X) ** 2).sum(0)           # summed over antennas
        pe = P[empty].mean()                   # noise floor outside the cell BW
        pc = P[bins].reshape(NRB, 12)
        rbp = pc.sum(1) / 12
        row = dict(g=g, l=l, noise=pe, cell=10 * np.log10(pc.mean() / pe))
        for K in (2, 4):
            best = (0, 0)
            for off in range(K):
                on = pc[:, off::K].sum(1) / (12 // K); offp = (pc.sum(1) - pc[:, off::K].sum(1)) / (12 - 12 // K)
                ratio = on / np.maximum(offp, 1e-9)
                ok = (ratio > 4) & (rbp > 4 * pe)
                run = best[0]; cur = 0
                for v in ok:
                    cur = cur + 1 if v else 0; run = max(run, cur)
                best = (run, off) if run > best[0] else best
            row[f"c{K}"] = best[0]
        rows.append(row)
print(f"{len(rows)} uplink symbols examined in {nslots} slots")
def summarize(name, sel):
    rr = [r for r in rows if sel(r)]
    if not rr: return
    print(f"{name:34s} n={len(rr):5d} in-cell power over empty: median {np.median([r['cell'] for r in rr]):6.1f} dB p95 {np.percentile([r['cell'] for r in rr],95):6.1f} dB | "
          f"comb2 run>=4 RB: {sum(r['c2']>=4 for r in rr):4d} comb4 run>=4 RB: {sum(r['c4']>=4 for r in rr):4d}  max run c2 {max(r['c2'] for r in rr)} c4 {max(r['c4'] for r in rr)}")
summarize("UL symbols 0-7 (PUSCH)", lambda r: r["l"] < 8)
summarize("last-6 symbols 8-13", lambda r: r["l"] >= 8)
for l in range(14): summarize(f"  symbol {l}", lambda r, l=l: r["l"] == l)
# 3. periodicity of comb-like last-6 symbols: slot indices of symbols with a >=4 RB comb-2 or comb-4 run
hit = sorted({r["g"] for r in rows if r["l"] >= 8 and (r["c2"] >= 4 or r["c4"] >= 4)})
print("slots with comb-like last-6 symbols:", len(hit), hit[:40])
ul = sorted({r["g"] for r in rows if r["g"] % 10 in (8, 9)})
for P in [5 * 2 ** k for k in range(0, 8)] + [10, 20, 40, 80, 160]:
    best = 0
    for ph in range(P):
        c = sum(1 for g in hit if g % P == ph)
        best = max(best, c)
    cnt = len(hit)
    print(f"  period {P:4d} slots: most hits in one phase {best}/{cnt}")
