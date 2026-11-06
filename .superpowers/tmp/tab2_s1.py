#!/usr/bin/env python3
"""tab2_s1.py <campaign dir>: per-cell BASE vs NEW mean (min-max) from <run>/m.json, plus per-run rows."""
import sys, json, collections
C = sys.argv[1]; rows = []
for ln in open(C + '/runs.tsv'):
    t, r, pci, ok, arm, wall = ln.rstrip('\n').split('\t')
    if ok != 'OK': print('NOT OK', t, ok); continue
    m = json.load(open(r + '/m.json')); m['tag'] = t; m['arm'] = arm; m['cell'] = pci; rows.append(m)
def ms(v, f='%.2f'):
    v = [x for x in v if x is not None]
    return '-' if not v else (f + ' (' + f + '-' + f + ')') % (sum(v) / len(v), min(v), max(v))
def get(m, path):
    for p in path.split('.'):
        if m is None: return None
        m = m.get(p) if isinstance(m, dict) else m[int(p)]
    return m
M = [('wide dec/s', 'wide_dec_s'), ('wide TB crc-ok/s', 'wide_crc_s'), ('wide partial-TB proven/s', 'wide_part_s'), ('wide proven/s', 'wide_proven_s'),
     ('dmrs rec/s', 'dmrs_s'), ('dmrs nb>=128 /s', 'dmrs_nb128_s'), ('dmrs valid>=128 /s', 'dmrs_v128_s'),
     ('proven wide w/ dmrs-or-data rec', 'match_proven.any'), ('proven wide w/ >=128 valid', 'match_proven.v128'),
     ('data rec/s', 'data_s'), ('data whole/s', 'data_whole_s'), ('data partial/s', 'data_part_s'), ('data valid>=128 /s', 'data_v128_s'),
     ('avail PDSCH 25ms', 'avail_pdsch.0'), ('avail PDSCH 100ms', 'avail_pdsch.1'), ('avail PDSCH 1s', 'avail_pdsch.2'),
     ('avail ALL 25ms', 'avail_all.0'), ('avail ALL 100ms', 'avail_all.1'), ('avail ALL 1s', 'avail_all.2'),
     ('lat all p50 us', 'lat_us_all.0'), ('lat all p99 us', 'lat_us_all.1'), ('lat wide p50 us', 'lat_us_wide.0'), ('lat wide p99 us', 'lat_us_wide.1'),
     ('cpu cores', 'cpu')]
for cell in sorted({m['cell'] for m in rows}):
    print('\n### cell', cell)
    for arm in ('base', 'fix'):
        print(arm, [m['tag'] for m in rows if m['cell'] == cell and m['arm'] == arm])
    print('| metric | BASE | NEW |\n|---|---|---|')
    for nm, p in M:
        c = []
        for arm in ('base', 'fix'):
            v = [get(m, p) for m in rows if m['cell'] == cell and m['arm'] == arm]; v = [float(x) if x is not None else None for x in v]
            c.append(ms(v, '%.3f' if 'avail' in nm or 'w/' in nm else '%.2f'))
        print('| %s | %s | %s |' % (nm, c[0], c[1]))
    print('\nper-run:')
    for m in rows:
        if m['cell'] != cell: continue
        n = m['n']; print(m['tag'], m['arm'], 'wide/s %.1f crc/s %.1f part/s %.1f | per-wide-grant: dmrs_v128 %.3f data_v128 %.3f | dmrs/s %.1f v128 %.2f | match any %.3f v128 %.3f | est %s | partial_tb dmrs %d | funnel %s | cpu %s %s pci %s' % (
            m['wide_dec_s'], m['wide_crc_s'], m['wide_part_s'], m['dmrs_v128_s'] / max(m['wide_dec_s'], 1e-9), m['data_v128_s'] / max(m['wide_dec_s'], 1e-9), m['dmrs_s'], m['dmrs_v128_s'], m['match_proven']['any'], m['match_proven']['v128'], m['cfr_est'], m['dmrs_partial'], m['funnel'], m['cpu'], m['exit'], m['pcis']))
        d = m['drops']; print('   drops', {k: d[k] for k in d if d[k]}, '\n   tap', m['tap'])
