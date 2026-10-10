#!/usr/bin/env python3
"""WIDE-S0 cohort (WIDES0.md): per grant (rnti, abs) from DECODEDIAG probe=0 lines + DCIRAW (TDRA field = payload bits len-20..len-17, value 8 = S=0/L=14).
pass = any CRC pass; base = any pass excluding PDCCHRM attempts (4+64*60, 4+64*61). margin = max DD-SNR - (Shannon(Qm*R)+2 dB).
classes: W = >=96 PRB from RB 0, N = <=32 PRB; S0 = TDRA 8. Usage: cohort.py run.log..."""
import re, sys, math, collections
T0 = [120,157,193,251,308,379,449,526,602,679, 340,378,434,490,553,616,658, 438,466,517,567,616,666,719,772,822,873,910,948]
T1 = [120,193,308,449,602, 378,434,490,553,616,658, 466,517,567,616,666,719,772,822,873, 682.5,711,754,797,841,885,916.5,948]
kv = re.compile(r"(\w+)=(\S+)")
RM = {str(4 + 64 * 60), str(4 + 64 * 61)}
tot = collections.Counter()
import os
RN = set(os.environ.get("RNTIS", "").split(",")) - {""}
for f in sys.argv[1:]:
    dci = {}; g = collections.OrderedDict()
    for l in open(f, errors='replace'):
        if 'DCIRAW' in l:
            m = re.search(r"DCIRAW (\S+) rnti=0x(\S+) len=(\d+) cce=\d+ al=\d+ mm=\d+ payload=0x(\w+)", l)
            if m: dci[(m.group(2), m.group(1))] = (int(m.group(4), 16) >> (int(m.group(3)) - 20)) & 15
        elif 'DECODEDIAG' in l and 'probe=0' in l:
            d = dict(kv.findall(l.split('DECODEDIAG', 1)[1]))
            g.setdefault((d['rnti'], d['abs']), []).append(d)
    for k, L in g.items():
        d0 = L[0]; rb0, nrb = map(int, d0['rb'].split('+'))
        t = dci.get((d0['rnti'][2:], d0['fs']))
        if t != 8: continue
        mcs, tbl, qm = int(d0['mcs']), int(d0['tbl']), int(d0['qm'])
        if tbl > 1 or mcs > 27: continue
        snr = max(float(x['snr']) for x in L)
        R = (T1 if tbl == 1 else T0)[mcs] / 1024.
        mg = snr - (10 * math.log10(2 ** (qm * R) - 1) + 2)
        if mg < 4: continue
        w = 'W' if nrb >= 96 and rb0 == 0 else 'N' if nrb <= 32 else 'M'
        q = 'Q' if qm == 2 else 'H'
        ok = any(x['crc'] == '1' for x in L); base = any(x['crc'] == '1' and x.get('attempt') not in RM for x in L)
        if len(sys.argv) > 1 and RN and d0['rnti'] not in RN: continue
        for key in ((w, q), (w, '*')):
            tot[key + ('n',)] += 1; tot[key + ('base',)] += base; tot[key + ('final',)] += ok
for c in sorted(set(k[:2] for k in tot)):
    n = tot[c + ('n',)]
    print('%s %s n=%4d base %4d (%5.1f%%) final %4d (%5.1f%%)' % (c[0], c[1], n, tot[c + ('base',)], 100. * tot[c + ('base',)] / n, tot[c + ('final',)], 100. * tot[c + ('final',)] / n))
