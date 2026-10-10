#!/usr/bin/env python3
"""DECODE-RES: pool DECODEEVMRB lines (ISAC_PDSCH_EVM_RB=1) of S=0 decodes into an absolute-RB map of how often symbol 0 is
dirty (SNR on symbol 0 more than --thr dB below symbol 3 on the same RB). Prints per-RB counts compressed to 6-RB groups.
Usage: decres_rbmap.py run.log [--thr 4] [--min-snr 10]"""
import re, sys, argparse, collections
P = re.compile(r"DECODEEVMRB rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) crc=(\d) attempt=(\d+) rb=(\d+)\+(\d+) S=(\d+) s0=(\S*) s1=(\S*) s3=(\S*)")
ap = argparse.ArgumentParser(); ap.add_argument("log"); ap.add_argument("--thr", type=float, default=4); ap.add_argument("--min-snr", type=float, default=10)
a = ap.parse_args()
dirty = collections.Counter(); tot = collections.Counter()
val = lambda c: -1 if c == '.' else int("0123456789abcdefghijklmnopqrstuvwxyz".index(c))
seen = set()
for l in open(a.log, errors="replace"):
    m = P.search(l)
    if not m: continue
    r, fr, sl, ab, crc, att, rb0, n, S, s0, s1, s3 = m.groups()
    if r == "ffff" or S != "0" or len(s0) != int(n) or "." in s0: continue
    key = (r, fr, sl, att)
    if key in seen: continue
    seen.add(key)
    n = int(n); rb0 = int(rb0)
    for i in range(n):
        v0, v3 = val(s0[i]), val(s3[i])
        if v3 < a.min_snr: continue
        tot[rb0 + i] += 1
        if v3 - v0 > a.thr: dirty[rb0 + i] += 1
print("RB  : dirty/total (symbol 0 more than %.0f dB below symbol 3 on the same RB, symbol-3 SNR >= %.0f)" % (a.thr, a.min_snr))
for g in range(0, 217, 6):
    d = sum(dirty[r] for r in range(g, g + 6)); t = sum(tot[r] for r in range(g, g + 6))
    print("RB %3d-%3d : %5d / %6d  %s" % (g, g + 5, d, t, ("%3.0f%%" % (100 * d / t)) if t else "  - "))
