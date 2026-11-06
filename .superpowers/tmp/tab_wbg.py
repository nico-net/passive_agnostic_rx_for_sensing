#!/usr/bin/env python3
"""tab_wbg.py <campaign dir>: per-run metric table (markdown) from an.txt/inv.txt."""
import sys, re, os, collections
C = sys.argv[1]; rows = []
for ln in open(C + '/runs.tsv'):
    t, r, pci, ok, arm, wall = ln.rstrip('\n').split('\t')
    a = open(r + '/an.txt').read(); i = open(r + '/inv.txt').read()
    g = lambda p, s=a, d='?': (re.search(p, s) or [None, d])[1]
    src = {}
    for m in re.finditer(r'^  (\w+)\s+n=(\d+)\s+([\d.]+)/s \| prbs .*?\| wb (\d+) \(([\d.]+)/s\) \| snr n=\d+ p10/50/90=(\S+)', a, re.M): src[m.group(1)] = (float(m.group(3)), float(m.group(5)), m.group(6))
    wsnr = g(r'pdsch_dmrs wide n=\d+ \([\d.]+/s\) snr n=\d+ p10/50/90=(\S+)')
    pw = g(r'pdsch_dmrs wide n=(\d+)'); allw = g(r'all-source wide n=(\d+)'); bad = g(r'null snr\): (\d+)')
    dr = re.search(r'DROPS scanq full/stale/epoch (\S+) of (\d+) \(([\d.]+)%\) \| pdschq full/stale/epoch/narrow (\S+) of (\d+) \(([\d.]+)%\).*sense_rec_drop (\S+) sense_cfr_drop (\S+)', a)
    w = eval(g(r'WBG counters: (\{.*\})', d='{}'))
    ft = g(r'SENSE_FLOOR t_cell distribution \(n=\d+\): (\{.*\})', d='{}')
    cpu = open(r + '/cpu_cores.txt').read().strip()
    rows.append((t, arm, wall, cpu, g(r'grants/s ([\d.]+)', i), g(r'valid>=128 rs=\d+ \(([\d.]+)%\)', i), g(r'any pdsch rs=\d+ \(([\d.]+)%\)', i), src, wsnr, pw, allw, bad, dr, w, ft,
                g(r'avail25ms=([\d.]+)'), g(r'avail100ms=([\d.]+)'), g(r'avail1s=([\d.]+)'), g(r'rs span_s (\S+)'), g(r'floor_prb (\d+)'), g(r'binding (\S+) disabled'), g(r'wide DL grants=(\d+)', i)))
for x in rows:
    t, arm, wall, cpu, gps, cov, anyc, src, wsnr, pw, allw, bad, dr, w, ft, a25, a100, a1, span, fl, bnd, ng = x
    print("\n## %s %s %s cpu=%s span=%s wide grants=%s (%s/s) cov>=128=%s%% anyrs=%s%% avail=%s/%s/%s bind=%s" % (t, arm, wall, cpu, span, ng, gps, cov, anyc, a25, a100, a1, bnd))
    print("  pdsch_dmrs wide n=%s allsrc wide n=%s wide-snr(pdsch_dmrs)=%s bad(nb_rb>=128 & valid0/nullsnr)=%s wbg=%s floor=%s" % (pw, allw, wsnr, bad, w, ft))
    print("  scan full/stale %s | pdschq %s | rec_drop %s cfr_drop %s" % ((dr.group(1), dr.group(3)) if dr else None, (dr.group(4), dr.group(6)) if dr else None, dr and dr.group(7), dr and dr.group(8)))
    print("  rec/s (wb/s):", {k: (v[0], v[1]) for k, v in src.items()})

# ---- per cell/arm summary: mean (min-max) over the 3 runs
import statistics as st
def ms(v):
    v = [float(x) for x in v if x not in (None, '?')]
    return "%.2f (%.2f-%.2f)" % (st.mean(v), min(v), max(v)) if v else 'n/a'
print("\n# SUMMARY (mean (min-max) over 3 runs per arm)")
for cell in ('415', '923'):
    for arm in ('base', 'fix'):
        X = [x for x in rows if x[0].startswith('wbg' + cell) and x[1] == arm]
        sp = [float(x[18]) for x in X]
        pr = lambda k, j: [x[7].get(k, (0, 0, ''))[j] for x in X]
        pdw = [float(x[9]) / float(x[18]) for x in X]; allw = [float(x[10]) / float(x[18]) for x in X]
        ndw = [float(x[21]) / float(x[18]) for x in X]
        fl = collections.Counter()
        for x in X:
            d = eval(x[14]); n = sum(float(v[:-1]) for v in d.values()) or 100
            for k, v in d.items(): fl[k] += float(v[:-1])
        tot = sum(fl.values()) or 1
        print("%s %s: wide DL grants/s %s | cov>=128 %% %s | any-rs cov %% %s | pdsch_dmrs rec/s %s | pdsch_dmrs wide/s %s | pdsch_data wide/s %s | pdsch wide/s(dmrs only) %s | all-source wide/s %s | avail 25ms %s 100ms %s 1s %s | cpu %s" % (
            cell, arm, ms(ndw), ms([x[5] for x in X]), ms([x[6] for x in X]), ms(pr('pdsch_dmrs', 0)), ms(pdw), ms(pr('pdsch_data', 1)), ms(pdw), ms(allw), ms([x[15] for x in X]), ms([x[16] for x in X]), ms([x[17] for x in X]), ms([x[3] for x in X])))
        print("    wide-snr(pdsch_dmrs) per run p10/50/90: %s | floor share (pooled over runs) %s | scanq stale %% %s | pdschq stale %% %s" % ([x[8] for x in X], {k: "%.0f%%" % (100 * v / tot) for k, v in sorted(fl.items(), reverse=True)},
            ms([x[12].group(3) for x in X]), ms([x[12].group(6) for x in X])))
        for s in ('csirs', 'ssb', 'pdsch_data', 'pusch_dmrs', 'srs', 'prach'): print("    %s rec/s %s" % (s, ms(pr(s, 0) if False else [x[7].get(s, (0,))[0] for x in X])), end='')
        print()
