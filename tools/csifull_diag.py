#!/usr/bin/env python3
"""CSI-FULL: per-grant DECODEDIAG/DECODEEVM view of low-MCS dedicated full decodes (attempt 0, probe 0, QPSK MCS0-4,
DD-SNR >= 10 dB). Groups by class (A/B/C as csifull_classes.py), wide/narrow, slot group; prints geometry stats
and, for failures, the per-symbol SNR spread (max-min) and the worst k mod 12 spread."""
import re, sys, collections
D = re.compile(r"DECODEDIAG rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) probe=(\d) crc=(\d) nl=(\d+) mcs=(\d+) tbl=(\d+) "
               r"qm=(\d+) snr=(-?[\d.]+) .*? rv=(\d+) ndi=(\d+) pid=(\d+) combined=(-?\d+) S=(\d+) L=(\d+) dmrs=0x([0-9a-f]+) "
               r".*?cdm=(\d+) .*?dmrsid=(\d+) dataid=(\d+) .*?csi_re=(\d+) ssb_re=(\d+) .*?rb=(\d+)\+(\d+) .*?sign_errors=(\d+) "
               r"signs=(\d+) .*?attempt=(-?\d+)")
E = re.compile(r"DECODEEVM rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) crc=(\d) attempt=(-?\d+) sym=\[([^\]]*)\] worst=(-?\d+) k12=\[([^\]]*)\]")
for f in sys.argv[1:]:
    diag, evm = {}, {}
    for line in open(f, errors="replace"):
        m = D.search(line)
        if m:
            g = m.groups()
            if g[4] != "0" or g[-1] != "0" or g[0] == "ffff" or int(g[7]) > 4 or g[9] != "2" or float(g[10]) < 10:
                continue
            diag[(g[0], g[1], g[2])] = dict(crc=int(g[5]), nl=int(g[6]), mcs=int(g[7]), snr=float(g[10]), rv=int(g[11]),
                                           S=int(g[15]), L=int(g[16]), dmrs=g[17], cdm=int(g[18]), id=int(g[19]),
                                           csi=int(g[21]), ssb=int(g[22]), rb0=int(g[23]), nrb=int(g[24]),
                                           se=int(g[25]), signs=int(g[26]), slot=int(g[2]), ph=(int(g[1]) * 20 + int(g[2])) % 160)
            continue
        m = E.search(line)
        if m and m.group(6) == "0":
            syms = [float(x.split(":")[1]) for x in m.group(7).split(",") if ":" in x]
            k12 = [float(x) for x in m.group(9).split(",") if x]
            evm[(m.group(1), m.group(2), m.group(3))] = (max(syms) - min(syms) if syms else 0, max(k12) - min(k12) if k12 else 0, m.group(7))
    T = collections.defaultdict(lambda: [0, 0])
    fails = []
    for k, e in diag.items():
        c = "C" if e["rb0"] >= 52 else ("A" if e["ph"] in (15, 16, 20, 21) else "B")
        w = "wide" if e["nrb"] >= 52 else "narrow"
        sg = "s0/6/10/16" if e["slot"] in (0, 6, 10, 16) else "other"
        for key in ((c,), (c, w), (c, w, sg), (c, "rv>0" if e["rv"] else "rv0")):
            T[key][0] += e["crc"]; T[key][1] += 1
        if not e["crc"]:
            fails.append((k, e, c, w, sg))
    print("==", f)
    for key in sorted(T):
        print("  ", "/".join(key), f"{T[key][0]}/{T[key][1]}")
    for k, e, c, w, sg in sorted(fails, key=lambda x: (x[2], x[3]))[:80]:
        ev = evm.get(k)
        print(f"  FAIL {c} {w:6s} {sg:10s} 0x{k[0]} {k[1]}.{k[2]} ph={e['ph']} rb={e['rb0']}+{e['nrb']} S={e['S']} L={e['L']} dmrs=0x{e['dmrs']} "
              f"cdm={e['cdm']} id={e['id']} rv={e['rv']} csi={e['csi']} ssb={e['ssb']} snr={e['snr']:.1f} se={e['se']}/{e['signs']}"
              + (f" symspread={ev[0]:.1f} k12spread={ev[1]:.1f} sym=[{ev[2]}]" if ev else ""))
