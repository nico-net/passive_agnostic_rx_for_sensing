#!/usr/bin/env python3
"""DECODE-RES: summarise TINTAB lines (paired channel-estimate time interpolation A/B on identical samples).
Usage: decres_tintab.py run.log [--mcs-max 4] [--snr-min 10]"""
import re, sys, argparse, collections
P = re.compile(r"TINTAB rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) nl=(\d+) qm=(\d+) mcs=(\d+) rb=(\d+)\+(\d+) S=(\d+) L=(\d+) "
               r"snr=(-?[\d.]+) snr_alt=(-?[\d.]+) tint=(\d) crc=(\d) alt_crc=(-?\d) rec=(\d) alt_rec=(\d)")
ap = argparse.ArgumentParser(); ap.add_argument("logs", nargs="+"); ap.add_argument("--mcs-max", type=int, default=4)
ap.add_argument("--snr-min", type=float, default=10.0); a = ap.parse_args()
for f in a.logs:
    t = collections.defaultdict(lambda: [0, 0, 0, 0, 0])  # n, base ok, alt ok, base-only, alt-only
    for l in open(f, errors="replace"):
        m = P.search(l)
        if not m: continue
        r, fr, sl, ab, nl, qm, mcs, rb0, nrb, S, L, snr, snra, tint, crc, acrc, rec, arec = m.groups()
        if r == "ffff" or int(mcs) > a.mcs_max or float(snr) < a.snr_min: continue
        cls = ("C" if int(rb0) >= 52 else "B")
        wide = "wide" if int(nrb) >= 40 else "narrow"
        for k in ((int(nl), cls, wide), (int(nl), cls, "S%sL%s" % (S, L)), (int(nl), "all", "all")):
            e = t[k]; e[0] += 1; e[1] += int(crc); e[2] += max(int(acrc), 0)
            e[3] += int(crc) and not int(acrc) > 0; e[4] += (not int(crc)) and int(acrc) > 0
    print("==", f, "(tint=%s is the primary; alt = flipped)" % tint)
    for k in sorted(t): print("  nl=%d %s %-8s n=%4d  primary ok %4d  flipped ok %4d  primary-only %3d  flipped-only %3d" % (k + tuple(t[k])))
