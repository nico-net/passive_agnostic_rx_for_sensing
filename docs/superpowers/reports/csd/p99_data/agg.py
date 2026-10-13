#!/usr/bin/env python3
"""aggregate p99_stats per arm from runs.txt (arm = name minus trailing round digit)"""
import re, sys, statistics as st, subprocess, json, collections
sys.path.insert(0, '/home/nicola/NICOLA/wt/rr-csd-p99/tests/passive_rx')
import p99_stats
runs = collections.defaultdict(list)
for l in open(sys.argv[1]):
    m = re.match(r'(\S+) cap=(\S+) rc=(\d+) .*dir=(\S+)', l)
    if not m or m.group(3) != '0': continue
    name, cap, d = m.group(1), m.group(2), m.group(4)
    arm = re.sub(r'\d+$', '', name)
    if arm in ('probeA', 'perfA', 'perfP'): continue
    try: o = p99_stats.stats(d + '/run.log')
    except Exception as e: print('skip', d, e); continue
    runs[arm].append((name, o))
def ms(v): return '%.0f+-%.0f' % (st.mean(v), st.stdev(v)) if len(v) > 1 else ('%.0f' % v[0] if v else '-')
for arm, rs in runs.items():
    g = lambda f: [f(o) for _, o in rs if f(o) is not None]
    crc = collections.Counter(); tot = collections.Counter()
    for _, o in rs:
        for k, v in o.get('crc', {}).items():
            a, b = map(int, v.split('/')); crc[k] += a; tot[k] += b
    print('== %s n=%d runs=%s' % (arm, len(rs), ','.join(n for n, _ in rs)))
    print('  real mean %s | p50 %s | p90 %s | p99 %s | max_ms %s' % (ms(g(lambda o: o.get('real', {}).get('mean'))), ms(g(lambda o: o.get('real', {}).get('p50'))),
          ms(g(lambda o: o.get('real', {}).get('p90'))), ms(g(lambda o: o.get('real', {}).get('p99'))), ms(g(lambda o: o.get('real', {}).get('max_ms')))))
    print('  body p50 %s p99 %s | stale%% %s | queued %s decoded %s crc_ok %s | retry/real %s | stage_s real/retry/probe %s' % (
        ms(g(lambda o: o.get('body', {}).get('p50'))), ms(g(lambda o: o.get('body', {}).get('p99'))), ms(g(lambda o: o['pdschq']['stale_pct'])),
        ms(g(lambda o: o['pdschq']['queued'])), ms(g(lambda o: o['pdschq']['decoded'])), ms(g(lambda o: o['pdschq']['crc_ok'])),
        '%.2f' % st.mean(g(lambda o: o.get('retry_per_real'))), [tuple(o.get('stage_s', {}).values()) for _, o in rs]))
    print('  age real ge8%% %s ge18%% %s | scan lag ge8%% %s | slow20 n/a | yields %s' % (ms(g(lambda o: o.get('age_enq_real', {}).get('ge8'))),
          ms(g(lambda o: o.get('age_enq_real', {}).get('ge18'))), ms(g(lambda o: o.get('scan_lag_deq', {}).get('ge8'))),
          [o.get('retry_yield_gated_chains') for _, o in rs]))
    print('  CRC-OK pooled:', ' '.join('%s=%d/%d(%.1f%%)' % (k, crc[k], tot[k], 100.0 * crc[k] / max(1, tot[k])) for k in sorted(tot)))
    print('  bad chest/demod', [ (o.get('bad_chest'), o.get('bad_demod'), o.get('mismatch')) for _, o in rs])
