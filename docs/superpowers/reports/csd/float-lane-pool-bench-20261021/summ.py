#!/usr/bin/env python3
# usage: summ.py raw_dir ; medians over repeats per (cfg, mode, scale)
import re, sys, glob, statistics as st, collections
d = collections.defaultdict(list)
for f in sorted(glob.glob(sys.argv[1] + '/*.txt')):
    m = re.match(r'.*/(\w+?)_(sep|merged|none)_s([\d.]+)_r(\d+)\.txt', f)
    if not m: continue
    cfg, mode, sc, rep = m.groups()
    r = {}
    fl = {}
    for ln in open(f):
        m2 = re.match(r'\s+(scan|dl|ul|retry)\s+n=(\d+)\s+wait_us p50=\s*(\d+) p99=\s*(\d+) max=\s*(\d+).*miss=(\d+)/(\d+) drop=(\d+)', ln)
        if m2:
            k, n, p50, p99, mx, ms, pu, dr = m2.groups()
            r[k] = (int(p50), int(p99), int(mx), 100.0 * int(ms) / max(1, int(pu)), int(dr))
        m3 = re.match(r'\s+F (\w+)\s+duty_target=([\d.]+) duty_got=([\d.]+) jobs=(\d+) start_wait_us p50=(\d+) p99=(\d+) max=(\d+) \| done_us p50=(\d+) p99=(\d+) max=(\d+)', ln)
        if m3: fl[m3.group(1)] = (float(m3.group(2)), float(m3.group(3)), int(m3.group(6)), int(m3.group(9)), int(m3.group(10)))
    if r: d[(cfg, mode, sc)].append((r, fl))
med = lambda xs: statistics.median(xs) if xs else 0
import statistics
print('cfg mode scale reps | scan wait p99/max us miss% | dl p99/max miss% | ul p99/max miss% | retry p99/max | float duty got/target | float start-wait p99 (max over classes) | ulBgDisc done p99 | drops')
order = {'T4': 0, 'T4na': 0, 'T5': 1, 'T5na': 1, 'T6': 2, 'T6na': 2, 'T7': 3, 'T8': 4, 'T8u': 4, 'T11': 5, 'T11u': 5, 'T11b': 6}
for k in sorted(d, key=lambda k: (k[2], order.get(k[0], 9), k[1])):
    runs = d[k]
    def g(cls, i): return med([r[cls][i] for r, _ in runs if cls in r])
    gotsum = med([sum(v[1] for v in fl.values()) for _, fl in runs]) if runs[0][1] else 0
    tgsum = sum(v[0] for v in runs[0][1].values()) if runs[0][1] else 0
    fsw = med([max(v[2] for v in fl.values()) for _, fl in runs]) if runs[0][1] else 0
    ulb = med([fl['ulBgDisc'][3] for _, fl in runs if 'ulBgDisc' in fl]) if runs[0][1] else 0
    drops = sum(r[c][4] for r, _ in runs for c in r)
    print('%-4s %-6s %s n=%d | %5d/%6d %5.1f | %5d/%6d %5.1f | %5d/%6d %5.1f | %5d/%6d | %.2f/%.2f | %6d | %7d | %d' % (k[0], k[1], k[2], len(runs), g('scan', 1), g('scan', 2), g('scan', 3),
          g('dl', 1), g('dl', 2), g('dl', 3), g('ul', 1), g('ul', 2), g('ul', 3), g('retry', 1), g('retry', 2), gotsum, tgsum, fsw, ulb, drops))
