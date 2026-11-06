#!/usr/bin/env python3
"""mk_day.py <runs.tsv ...>: markdown tables for the daytime report (reads each run's an.txt/an.json/score.json/cpu_cores.txt)."""
import sys, json, re, os, collections
rows = []
for f in sys.argv[1:]:
    for ln in open(f):
        t, r, pci, ok, arm, ts = ln.rstrip('\n').split('\t')
        if ok != 'OK': continue
        rows.append((t, r, pci, arm, r[-6:-4] + ':' + r[-4:-2] + ':' + r[-2:] + '-' + ts))
g = lambda r, k: open(r + '/' + k).read().strip() if os.path.exists(r + '/' + k) else '?'
def f(x, n=2): return '-' if x is None else (('%.*f' % (n, x)) if isinstance(x, float) else str(x))
cells = collections.OrderedDict()
for t, r, pci, arm, ts in rows: cells.setdefault(pci, []).append((t, r, arm, ts))
def txt(r): return open(r + '/an.txt', errors='replace').read()
def num(rx, s, grp=1, cast=float):
    m = re.search(rx, s); return cast(m.group(grp)) if m else None
for pci, runs in cells.items():
    print('\n## CELL', pci)
    print('\n### Traffic runs\n')
    print('| run | start-end (local) | CPU cores | DCI rec | RNTIs>=2 DCI (DL+UL) | SOLVED | full-field 1_1 | DL CRC ok/non-probe (%) | DL>=128PRB ok/n (%) | scanq full/stale of queued | pdschq full/stale of queued | other |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for t, r, arm, ts in runs:
        if 'T' not in t[5:]: continue
        s = txt(r); m = re.search(r'distinct RNTI \(dir,rnti\) (\d+) >=2 DCIs (\d+) by dir (\{.*?\}) SOLVED RNTIs (\d+)', s)
        dr = eval(re.search(r'by dir (\{.*?\})', m.group(0)).group(1))
        d = eval(re.search(r'decode records \(non-probe\) by dir: (\{.*?\})', s).group(1)); w = eval(re.search(r'>=128PRB: (\{.*?\})', s).group(1))
        fu = re.search(r'non-null: (\d+) / (\d+) = ([\d.]+)%', s)
        dq = re.search(r'scanq full/stale/epoch (\d+)/(\d+)/\d+ of (\d+) \(([\d.]+)%\) \| pdschq full/stale/epoch/narrow (\d+)/(\d+)/\d+/\d+ of (\d+) \(([\d.]+)%\)', s)
        nd = d.get('DL', 0); ok = d.get('DL_ok', 0); wn = w.get('DL', 0); wo = w.get('DL_ok', 0)
        dci = num(r'DCI records (\d+)', s, cast=int)
        ds = re.search(r'traffic_skip (\d+) ul_decode_skip (\d+) .*? e1_skip_classes (\d+) e3_skip (\d+) e3_trunc (\d+)', s)
        print('| %s | %s | %s | %d | %s (%s+%s) | %s | %s of %s = %s%% | %d/%d (%.1f%%) | %d/%d (%s) | %s/%s of %s (%s%%) | %s/%s of %s (%s%%) | traffic_skip %s, ul_decode_skip %s, e1 %s, e3_skip %s, e3_trunc %s |' % (
            t, ts, g(r, 'cpu_cores.txt'), dci, m.group(2), dr.get('DL', 0), dr.get('UL', 0), m.group(4), fu.group(1), fu.group(2), fu.group(3), ok, nd, 100. * ok / max(nd, 1), wo, wn,
            '%.1f%%' % (100. * wo / wn) if wn else 'n/a', dq.group(1), dq.group(2), dq.group(3), dq.group(4), dq.group(5), dq.group(6), dq.group(7), dq.group(8), *ds.groups()))
    print('\n### Sensing runs\n')
    srcs = ['ssb', 'csirs', 'pdsch_dmrs', 'pdsch_data', 'pusch_dmrs', 'pusch_data', 'srs']
    sr = [(t, r, arm, ts) for t, r, arm, ts in runs if 'S' in t[5:]]
    print('| source | run (arm) | rec/s | valid_cfr_prbs p10/p50/p90 (max) | wb rec/s | snr_db_p50 p10/p50/p90 | drop % (cfr+async) |')
    print('|---|---|---|---|---|---|---|')
    for s_ in srcs:
        for t, r, arm, ts in sr:
            a = json.load(open(r + '/an.json')).get(s_)
            if not a or a['n'] == 0: print('| %s | %s (%s) | 0 | - | - | - | - |' % (s_, t, arm)); continue
            print('| %s | %s (%s) | %.2f | %s/%s/%s (%s) | %.3f | %s/%s/%s | %.2f |' % (s_, t, arm, a['rate'], *[f(x, 0) for x in a['prb']], a['wb_rate'], *[f(x, 1) for x in a['snr']], a['drop_pct']))
    print('\n| run (arm) | time | CPU | avail25ms / 100ms / 1s (wideband) | wb records | gap p95/max s | units enabled series (min-max, mean) | SCHED binding | top | elig mean/max | cold_deferred mean/max | decisions w/ pdsch_dmrs / pdsch_data admitted (of N) | enables/disables |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for t, r, arm, ts in sr:
        s = txt(r); sc = json.load(open(r + '/score.json')) if os.path.exists(r + '/score.json') else {}
        j = json.load(open(r + '/an.json')); sd = j['sched']
        av = re.search(r'AGGREGATE .*?wb=(\d+) gap p50/p95/max=(\S+) s avail25ms=([\d.]+) avail100ms=([\d.]+) avail1s=([\d.]+)', s)
        es = sd['enabled_series']; ed = sd['adm']
        print('| %s (%s) | %s | %s | %s / %s / %s | %s | %s%s | %d-%d, %.1f | %s | %s | %s | %s | %s / %s (of %d) | %s/%s |' % (t, arm, ts, g(r, 'cpu_cores.txt'), av.group(3), av.group(4), av.group(5), av.group(1), av.group(2), '',
            min(es) if es else 0, max(es) if es else 0, sum(es) / max(len(es), 1), sd['binding'], sd['top'] or 'n/a', ('%.1f/%d' % tuple(sd['elig'])) if sd['elig'] else 'n/a', ('%.1f/%d' % tuple(sd['cold'])) if sd['cold'] else 'n/a',
            ed.get('pdsch_dmrs', 0), ed.get('pdsch_data', 0), sd['decisions'], num(r'enables_sum=(\d+)', s, cast=int), num(r'disables_sum=(\d+)', s, cast=int)))
