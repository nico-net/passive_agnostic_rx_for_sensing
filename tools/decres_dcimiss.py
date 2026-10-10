#!/usr/bin/env python3
"""DECODE-RES item 5: DCI-miss estimate per RNTI from HARQ-PID continuity of DCIRAW payloads (ISAC_DCI_RAW_LOG=1).
For each RNTI with >= --min DCIs (49-bit DCI 1_1), the 4-bit payload window whose consecutive-DCI step is +1 (mod 16) most often is taken as the HARQ PID
(validated: it must beat the second-best window by a margin). Steps of 0 are retransmissions/repeats (no miss); a step d in 2..15 means d-1 missed DCIs.
The PID is a 3-bit field at payload bit 28 (measured), modulo M = 8. Steps of 0 are repeats (no miss), 1 is the normal step (a wrap is a step of
1 mod M), 2 and 3 count 1 and 2 misses; larger steps (ambiguous with reordering/retransmissions) are counted separately as 'other' and not as misses.
miss = missed / (seen + missed). The PID need not step by +1 for every scheduler, so this is an ESTIMATE, labelled as such.
Usage: decres_dcimiss.py run.log [--min 40]"""
import re, sys, argparse, collections
X = re.compile(r"DCIRAW (\d+)\.(\d+) rnti=0x([0-9a-f]+) len=(\d+) cce=\d+ al=\d+ mm=(\d+) payload=0x([0-9a-f]+)")
ap = argparse.ArgumentParser(); ap.add_argument("log"); ap.add_argument("--min", type=int, default=40); ap.add_argument("--window", type=int, default=28); ap.add_argument("--mod", type=int, default=8); ap.add_argument("--mm", type=int, default=0); a = ap.parse_args()
seq = collections.defaultdict(dict)
for l in open(a.log, errors="replace"):
    m = X.search(l)
    if not m or m.group(4) != "49" or int(m.group(5)) > a.mm: continue
    fr, sl, r, ln, mm, p = m.groups()
    seq[r][(int(fr), int(sl))] = bin(int(p, 16))[2:].zfill(49)[-49:]
print("rnti   n   pidbits  +1 frac  seen  missed  miss%   (second-best window +1 frac)")
tot = [0, 0]
for r, d in sorted(seq.items(), key=lambda x: -len(x[1])):
    if len(d) < a.min: continue
    ks = sorted(d); best = []
    for w in [a.window]:
        v = [int(d[k][w:w + 3], 2) for k in ks]
        M = a.mod
        v = [x for x in v if x < M]
        ks = ks[:len(v)] if len(v) == len(ks) else ks
        st = [(v[i + 1] - v[i]) % M for i in range(len(v) - 1)]
        nz = [s for s in st if s != 0]
        if len(nz) < 20: best.append((0, w, st, M)); continue
        best.append((sum(1 for s in nz if s == 1) / len(nz) - 0.002 * M, w, st, M))
    best.sort(key=lambda x: -x[0])
    f, w, st, M = best[0]
    ok = f >= 0.5
    missed = sum(s - 1 for s in st if 2 <= s <= 3)
    other = sum(1 for s in st if s > 3)
    seen = len(ks)
    if ok: tot[0] += missed; tot[1] += seen + missed
    print("0x%s %4d  %2d-%2d M=%2d %5.2f  %5d  %5d other %3d %5.1f%%  (%.2f) %s" % (r, seen, w, w + 3, M, f, seen, missed, other, 100 * missed / (seen + missed), (best[1][0] if len(best)>1 else 0), "" if ok else "UNRELIABLE"))
if tot[1]: print("reliable RNTIs pooled: missed %d of %d = %.1f%%" % (tot[0], tot[1], 100 * tot[0] / tot[1]))
