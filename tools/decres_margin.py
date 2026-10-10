#!/usr/bin/env python3
"""DECODE-RES item 4: SNR margin vs requirement for Qm>=4 grants (EVMGRANT probe=0 lines, last decode per grant, any pass counts via crc).
Requirement per grant = Shannon SNR for spectral efficiency Qm*R per layer (log2(1+snr)=Qm*R) + --gap dB implementation loss (default 2.0, max-log LDPC 8 it).
snr = decision-directed EVM SNR of the grant (mean over layers). Tables: 38.214 5.1.3.1-1 (tbl 0), 5.1.3.1-2 (tbl 1).
Usage: decres_margin.py run.log... [--gap 2] [--list]"""
import re, sys, argparse, math, collections
T0 = [120,157,193,251,308,379,449,526,602,679, 340,378,434,490,553,616,658, 438,466,517,567,616,666,719,772,822,873,910,948]
T1 = [120,193,308,449,602, 378,434,490,553,616,658, 466,517,567,616,666,719,772,822,873, 682.5,711,754,797,841,885,916.5,948]
P = re.compile(r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) rb=(\d+)\+(\d+) nsym=(\d+) evm=([\d.-]+) snr=(-?[\d.]+).*?kappa=([\d.]+).*?probe=(\d)|"
               r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) rb=(\d+)\+(\d+) nsym=(\d+) evm=([\d.-]+) snr=(-?[\d.]+).*?probe=(\d)")
ap = argparse.ArgumentParser(); ap.add_argument("logs", nargs="+"); ap.add_argument("--gap", type=float, default=2.0); ap.add_argument("--list", action="store_true"); a = ap.parse_args()
Q = re.compile(r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) rb=(\d+)\+(\d+) nsym=(\d+) evm=([\d.-]+) snr=(-?[\d.]+).*?probe=(\d)")
t = collections.defaultdict(lambda: [0, 0]); fails = []
for f in a.logs:
    g = {}
    for l in open(f, errors="replace"):
        if "EVMGRANT" not in l: continue
        m = Q.search(l)
        if not m: continue
        r, fr, sl, crc, mcs, tbl, qm, nl, rb0, nrb, nsym, evm, snr, pr = m.groups()
        if pr != "0" or r == "ffff" or int(qm) < 4 or int(tbl) > 1 or int(mcs) >= 29: continue
        k = (f, r, fr, sl); e = g.get(k)
        if e: e[0] = max(e[0], int(crc))
        else: g[k] = [int(crc), int(mcs), int(tbl), int(qm), int(nl), float(snr), int(nrb), int(rb0), int(nsym)]
    for k, (c, mcs, tbl, qm, nl, snr, nrb, rb0, ns) in g.items():
        R = (T1 if tbl == 1 else T0)[mcs] / 1024.0
        req = 10 * math.log10(2 ** (qm * R) - 1) + a.gap
        mg = snr - req
        b = "<0" if mg < 0 else "0-2" if mg < 2 else "2-4" if mg < 4 else ">=4"
        t[(nl, qm, b)][0] += c; t[(nl, qm, b)][1] += 1
        if mg >= 2 and not c: fails.append((mg, k, mcs, tbl, qm, nl, snr, nrb, rb0, ns))
print("Nl Qm margin(dB)   pass/n")
for k in sorted(t): print(" %d  %d  %-5s  %4d/%4d  %5.1f%%" % (k + (t[k][0], t[k][1], 100 * t[k][0] / t[k][1])))
print("fails with margin >= 2 dB:", len(fails))
if a.list:
    for x in sorted(fails, reverse=True)[:40]: print("  margin %.1f rnti %s %s.%s mcs %d tbl %d Qm %d Nl %d snr %.1f rb %d+%d nsym %d" % (x[0], x[1][1], x[1][2], x[1][3], x[2], x[3], x[4], x[5], x[6], x[8], x[7], x[9]))
