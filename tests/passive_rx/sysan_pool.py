#!/usr/bin/env python3
"""SYSAN: pooled per-arm e2e summary over replay dirs named replay_<tag>_<cap>_<arm>_<rep>_HHMMSS. usage: sysan_pool.py 'replays/replay_m2_*'"""
import glob, re, sys
from collections import defaultdict
R = re.compile(r'EVMGRANT rnti=0x(\w+) \S+ crc=(\d) .*? probe=0 .*? e2e=(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(-?\d+),(-?\d+)')
U = re.compile(r'UL_E2E .*? wall=(\d+) tot=(-?\d+)')
pool = defaultdict(lambda: defaultdict(list))
for d in glob.glob(sys.argv[1]):
    m = re.search(r'replay_[^_]+_(\w+?)_(\w+)_(\d+)_\d{6}$', d)
    if not m:
        continue
    k = (m.group(1), m.group(2))
    n0 = len(pool[k]['dec'])
    for l in open(d + '/run.log', errors='replace'):
        x = R.search(l)
        if x:
            g = list(map(int, x.groups()[2:]))
            pool[k]['first'].append(g[4]); pool[k]['dec'].append(g[3]); pool[k]['tot'].append(g[6]); pool[k]['arr2enq'].append(g[0])
            pool[k]['ok'].append(int(x.group(2)))
            if x.group(2) == '1':
                pool[k]['dec_ok'].append(g[3])
            continue
        x = U.search(l)
        if x:
            pool[k]['ul_wall'].append(int(x.group(1)))
    pool[k]['runs'].append(len(pool[k]['dec']) - n0)
pct = lambda v, q: sorted(v)[min(len(v) - 1, int(q * len(v)))] if v else -1
print('%-10s %-4s %-16s %5s %6s | %-22s | %-22s | %-22s | %-16s | %s' % ('cap', 'arm', 'grants/run', 'okpct', '', 'first p50/p99/>500%', 'dec p50/p99/max', 'dec_ok p50/p99', 'tot p50/p99', 'UL wall p50/p99'))
for k in sorted(pool):
    P = pool[k]
    f = lambda n: '%d/%d' % (pct(P[n], .5), pct(P[n], .99))
    gt = lambda n: 100.0 * sum(1 for x in P[n] if x > 500) / max(1, len(P[n]))
    print('%-10s %-4s %-16s %5.1f %6s | %-22s | %-22s | %-22s | %-16s | %s' % (k[0], k[1], ','.join(map(str, P['runs'])), 100.0 * sum(P['ok']) / max(1, len(P['ok'])), '',
          '%s/%.0f%%' % (f('first'), gt('first')), '%s/%d %.0f%%' % (f('dec'), max(P['dec'] or [0]), gt('dec')), '%s %.0f%%' % (f('dec_ok'), gt('dec_ok')), f('tot'), f('ul_wall')))
