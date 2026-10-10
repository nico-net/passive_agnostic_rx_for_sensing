#!/usr/bin/env python3
"""DECODE-RES: per-grant FINAL outcome (any logged full decode attempt of the grant passed, recovery arms included) from DECODEDIAG lines,
for MCS0-4 QPSK Nl=1 dedicated grants with DD-SNR(attempt 0) >= --snr-min. Classes: B = first RB < 52, C = first RB >= 52; by S/L of attempt 0.
Usage: decres_final.py run.log [run2.log ...] [--snr-min 10] [--mcs-max 4]  (several logs: one table each, then the paired-by-key delta of the last two)"""
import re, sys, argparse, collections
D = re.compile(r"DECODEDIAG rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) probe=(\d) crc=(\d) nl=(\d+) mcs=(\d+) tbl=(\d+) qm=(\d+) snr=(-?[\d.]+) .*?"
               r"S=(\d+) L=(\d+) .*?rb=(\d+)\+(\d+) .*?attempt=(-?\d+)")
ap = argparse.ArgumentParser(); ap.add_argument("logs", nargs="+"); ap.add_argument("--snr-min", type=float, default=10.0)
ap.add_argument("--mcs-max", type=int, default=4); ap.add_argument("--nl", type=int, default=1); a = ap.parse_args()
res = []
for f in a.logs:
    g = {}
    for l in open(f, errors="replace"):
        m = D.search(l)
        if not m: continue
        r, fr, sl, ab, pr, crc, nl, mcs, tbl, qm, snr, S, L, rb0, nrb, att = m.groups()
        if pr != "0" or r == "ffff": continue
        k = (r, fr, sl)
        e = g.setdefault(k, dict(fin=0, att0=None))
        e["fin"] |= int(crc)
        if int(att) == 0:
            e["att0"] = dict(nl=int(nl), mcs=int(mcs), qm=int(qm), snr=float(snr), S=int(S), L=int(L), rb0=int(rb0), nrb=int(nrb), crc0=int(crc))
    t = collections.defaultdict(lambda: [0, 0, 0]); sel = {}
    for k, e in g.items():
        x = e["att0"]
        if not x or x["nl"] != a.nl or x["qm"] != 2 or x["mcs"] > a.mcs_max or x["snr"] < a.snr_min: continue
        sel[k] = e["fin"]
        cls = "B" if x["rb0"] < 52 else "C"
        for kk in (cls, cls + " S%dL%d" % (x["S"], x["L"]), "all"):
            t[kk][0] += e["fin"]; t[kk][1] += 1; t[kk][2] += x["crc0"]
    print("==", f)
    for kk in sorted(t): print("   %-10s final %4d/%4d (%5.1f%%)   first-attempt %4d" % (kk, t[kk][0], t[kk][1], 100 * t[kk][0] / t[kk][1], t[kk][2]))
    res.append(sel)
if len(res) >= 2:
    A, B = res[-2], res[-1]
    ks = set(A) & set(B)
    print("paired on %d common grants: only-first-log-pass %d, only-last-log-pass %d" % (len(ks), sum(1 for k in ks if A[k] and not B[k]), sum(1 for k in ks if B[k] and not A[k])))
