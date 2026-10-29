#!/usr/bin/env python3
"""B6: token accounting of a receiver run log from its CELL_CPU / CORE_POOL / DMRS_SCAN / scanq / PDSCHQ lines.
Usage: pool_accounting.py run.log [run.log ...]   (one block per log, one row per cell; never pools cells)
Reads only lines the receiver already prints (CORE_POOL grammar unchanged). Offline analysis, no receiver input."""
import re, sys, collections
ANSI = re.compile(r'\x1b\[[0-9;]*m')
CLS = ('scan', 'decode', 'retry', 'disc', 'house', 'ul')
cpu_re = re.compile(r'CELL_CPU cell=(\d+) pci=(\d+) win_ms=(\d+) tokens_avg=([\d.]+) share_pct=([\d.]+) fair_pct=([\d.]+) wait_p99_us=(\d+) n=(\d+)')
cls_re = re.compile(r'(scan|decode|retry|disc|house|ul)\[hold_ms=(\d+) n=(\d+)\]')
pool_re = re.compile(r'CORE_POOL n=(\d+) .*')
pdq_re = re.compile(r'PDSCHQ queued=(\d+) decoded=(\d+) crc_ok=(\d+).*?dropped\[full=(\d+) stale=(\d+)\]')
scanq_sum_re = re.compile(r'scanq\[queued=(\d+) done=(\d+) drop_full=(\d+) drop_stale=(\d+)')
dms_re = re.compile(r'DMRS_SCAN full .*full scans=(\d+) hits=(\d+) mean=(\d+) us')
tag_re = re.compile(r'^\[c(\d+)\]')

def one(path):
    win = collections.defaultdict(float); hold = collections.defaultdict(lambda: collections.defaultdict(float))
    cnt = collections.defaultdict(lambda: collections.defaultdict(float)); pci = {}; share = collections.defaultdict(list)
    waitp99 = collections.defaultdict(list); last_pool = None; dms = {}; pdq = {}; sqf = {}
    with open(path, 'rb') as f:
        for raw in f:
            if b'CELL_CPU' not in raw and b'CORE_POOL n=' not in raw and b'DMRS_SCAN full' not in raw and b'PDSCHQ queued' not in raw and b'scanq[queued' not in raw:
                continue
            l = ANSI.sub('', raw.decode('latin1'))
            t = tag_re.match(l); tc = int(t.group(1)) if t else 0
            m = cpu_re.search(l)
            if m:
                c = int(m.group(1)); pci[c] = m.group(2); win[c] += int(m.group(3)); share[c].append(float(m.group(5))); waitp99[c].append(int(m.group(7)))
                for k, h, n in cls_re.findall(l):
                    hold[c][k] += int(h); cnt[c][k] += int(n)
                continue
            if 'CORE_POOL n=' in l: last_pool = l.strip(); continue
            m = dms_re.search(l)
            if m: dms[tc] = tuple(int(x) for x in m.groups()); continue
            m = pdq_re.search(l)
            if m: pdq[tc] = tuple(int(x) for x in m.groups()); continue
            m = scanq_sum_re.search(l)
            if m: sqf[tc] = tuple(int(x) for x in m.groups())
    print('== %s' % path)
    if last_pool:
        m = re.search(r'CORE_POOL n=(\d+)', last_pool); print('  pool tokens n=%s' % m.group(1))
        for k in ('scan', 'decode', 'retry', 'disc', 'house', 'chan'):
            m = re.search(k + r'\[n=(\d+) waited=(\d+) promo=(\d+) wait_us p50<=(\d+) p99<=(\d+) max=(\d+)\]', last_pool)
            if m:
                n, w = int(m.group(1)), int(m.group(2))
                print('  %-6s leases=%-8d waited=%5.1f%% p50<=%sus p99<=%sus max=%sus promo=%s' % (k, n, 100.0 * w / max(n, 1), m.group(4), m.group(5), m.group(6), m.group(3)))
    for c in sorted(win):
        w = win[c] or 1
        tot = sum(hold[c].values())
        print('  cell %d pci=%s windows=%.0f s: tokens_avg(by class hold/win) total=%.2f  ' % (c, pci[c], w / 1000, tot / w) +
              ' '.join('%s=%.2f(n=%d,mean=%.2fms)' % (k, hold[c][k] / w, cnt[c][k], hold[c][k] / max(cnt[c][k], 1)) for k in CLS) +
              '  share_pct_mean=%.1f wait_p99_us_max=%d' % (sum(share[c]) / len(share[c]), max(waitp99[c])))
    for c in sorted(set(list(dms) + list(pdq) + list(sqf))):
        s = []
        if c in dms: s.append('DMRS_SCAN full scans=%d hits=%d mean=%.1f ms' % (dms[c][0], dms[c][1], dms[c][2] / 1000))
        if c in pdq:
            q, d, ok, df, ds = pdq[c]; s.append('PDSCHQ queued=%d stale=%d (%.1f%%) full=%d' % (q, ds, 100.0 * ds / max(q, 1), df))
        if c in sqf:
            q, d, df, ds = sqf[c]; s.append('scanq queued=%d drop_full=%.1f%% drop_stale=%.1f%%' % (q, 100.0 * df / max(q, 1), 100.0 * ds / max(q, 1)))
        print('  cell %d: ' % c + '; '.join(s))
for p in sys.argv[1:]: one(p)
