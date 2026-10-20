#!/usr/bin/env python3
"""SYSAN: GPU budget from an nsys sqlite export (nsys profile -t cuda; nsys export -t sqlite).
Per launching thread class (thread name without its index): kernels, GPU kernel time, share of the active window,
launch->start queue delay of its kernels (p50/p99), memcpy time; per PDSCH-consumer thread: CUDA API time by call.
Window = first to last PDSCH-consumer kernel.  usage: sysan_nsys.py file.sqlite [--json out.json]"""
import json, re, sqlite3, sys
from collections import defaultdict


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))]


def union_ns(iv):
    tot, cur_s, cur_e = 0, None, None
    for s, e in sorted(iv):
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                tot += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    return tot + ((cur_e - cur_s) if cur_e is not None else 0)


def main():
    db = sqlite3.connect(sys.argv[1])
    S = dict(db.execute('select id, value from StringIds'))
    tname = {tid: S.get(n, '?') for n, _, tid in db.execute('select nameId, priority, globalTid from ThreadNames')}
    cls = lambda t: re.sub(r'\d+$', '', tname.get(t, 'main'))
    rt = {}
    api = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    for s, e, tid, corr, nid in db.execute('select start, end, globalTid, correlationId, nameId from CUPTI_ACTIVITY_KIND_RUNTIME'):
        rt[corr] = (tid, e)
        a = api[tname.get(tid, 'main')][S.get(nid, '?').split('_v')[0]]
        a[0] += 1
        a[1] += e - s
    K = list(db.execute('select start, end, correlationId, shortName, streamId, greenContextId from CUPTI_ACTIVITY_KIND_KERNEL'))
    M = list(db.execute('select start, end, correlationId, bytes, copyKind from CUPTI_ACTIVITY_KIND_MEMCPY'))
    pd = [k for k in K if k[2] in rt and cls(rt[k[2]][0]).startswith('passivePdsch')]
    w0, w1 = min(k[0] for k in pd), max(k[1] for k in pd)
    win = w1 - w0
    by = defaultdict(lambda: dict(n=0, ns=0, iv=[], qd=[], names=defaultdict(int), green=set()))
    for s, e, corr, sn, st, gc in K:
        if s < w0 or s > w1:
            continue
        c = cls(rt[corr][0]) if corr in rt else 'unknown'
        b = by[c]
        b['n'] += 1
        b['ns'] += e - s
        b['iv'].append((s, e))
        b['names'][S.get(sn, '?')] += e - s
        b['green'].add(gc)
        if corr in rt:
            b['qd'].append(s - rt[corr][1])
    mc = defaultdict(lambda: [0, 0, 0])
    for s, e, corr, nb, kind in M:
        if s < w0 or s > w1:
            continue
        c = cls(rt[corr][0]) if corr in rt else 'unknown'
        mc[c][0] += 1
        mc[c][1] += e - s
        mc[c][2] += nb
    allk = [(s, e) for s, e, *_ in K if w0 <= s <= w1]
    out = dict(window_s=round(win / 1e9, 2), gpu_kernel_busy_pct=round(100.0 * union_ns(allk) / win, 1), classes={})
    print('window %.2f s; GPU busy (union of all kernels) %.1f %%' % (win / 1e9, out['gpu_kernel_busy_pct']))
    print('%-16s %8s %9s %7s %7s %9s %9s %8s %s' % ('class', 'kernels', 'k_ms', 'k%win', 'busy%', 'qd_p50us', 'qd_p99us', 'cpy_ms', 'top kernels (ms)'))
    for c, b in sorted(by.items(), key=lambda x: -x[1]['ns']):
        top = sorted(b['names'].items(), key=lambda x: -x[1])[:4]
        o = dict(kernels=b['n'], kernel_ms=round(b['ns'] / 1e6, 1), kernel_pct_window=round(100.0 * b['ns'] / win, 1),
                 busy_pct=round(100.0 * union_ns(b['iv']) / win, 1), qd_p50_us=round((pct(b['qd'], 50) or 0) / 1e3, 1),
                 qd_p99_us=round((pct(b['qd'], 99) or 0) / 1e3, 1), memcpy_ms=round(mc[c][1] / 1e6, 1), memcpy_n=mc[c][0],
                 green=sorted(x for x in b['green'] if x is not None), top={k: round(v / 1e6, 1) for k, v in top})
        out['classes'][c] = o
        print('%-16s %8d %9.1f %7.1f %7.1f %9.1f %9.1f %8.1f %s' % (c, o['kernels'], o['kernel_ms'], o['kernel_pct_window'], o['busy_pct'],
                                                               o['qd_p50_us'], o['qd_p99_us'], o['memcpy_ms'],
                                                               ', '.join('%s %.0f' % (k[:28], v) for k, v in o['top'].items())))
    print('CUDA API per thread (calls, ms) for the PDSCH consumers and scan threads:')
    out['api'] = {}
    for t, d in sorted(api.items()):
        if not re.match(r'passive(Pdsch|Pdcch|Pusch)|gpu|csirs|pdcchNid|UEthread', t):
            continue
        top = sorted(d.items(), key=lambda x: -x[1][1])[:6]
        out['api'][t] = {k: [v[0], round(v[1] / 1e6, 1)] for k, v in top}
        print('  %-14s %s' % (t, ', '.join('%s %d/%.0f' % (k, v[0], v[1] / 1e6) for k, v in top)))
    if '--json' in sys.argv:
        json.dump(out, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)


def stalls(db_path, gap_ms=50.0, consumer_prefixes=('passivePdsch', 'passivePdcch', 'passivePusch')):
    """Consumer kernels whose launch->start delay is >= gap_ms and is NOT explained by an earlier kernel on the same stream:
    the GPU (or the driver) was held by someone else. Returns the kernels running at launch time and the long CUDA API calls
    of any thread overlapping the wait."""
    c = sqlite3.connect(db_path)
    s = dict(c.execute('SELECT id, value FROM StringIds'))
    tname = {t: s.get(n, '') for t, n in c.execute('SELECT globalTid, nameId FROM ThreadNames')}
    launches = c.execute('SELECT r.start, r.globalTid, k.start, k.end, k.shortName, k.streamId FROM CUPTI_ACTIVITY_KIND_RUNTIME r '
                         'JOIN CUPTI_ACTIVITY_KIND_KERNEL k ON k.correlationId = r.correlationId').fetchall()
    kernels = c.execute('SELECT start, end, shortName, streamId FROM CUPTI_ACTIVITY_KIND_KERNEL').fetchall()
    api = c.execute('SELECT start, end, globalTid, nameId FROM CUPTI_ACTIVITY_KIND_RUNTIME').fetchall()
    c.close()
    gap = gap_ms * 1e6
    out = []
    for r0, tid, k0, k1, kn, st in launches:
        th = tname.get(tid, '')
        if not th.startswith(consumer_prefixes) or (k0 - r0) < gap:
            continue
        own = [b for a, b, n, sid in kernels if sid == st and a < k0 and b > r0]
        if own and max(own) >= k0 - gap:   # it waited for its own stream
            continue
        running = sorted({s.get(n, str(n)) for a, b, n, sid in kernels if a <= r0 < b and sid != st})
        blocking = [(tname.get(t, str(t)), s.get(n, str(n)), (b - a) / 1e6) for a, b, t, n in api
                    if t != tid and a < k0 and b > r0 and (b - a) >= gap / 10]
        out.append({'start_ns': r0, 'dur_ms': (k0 - r0) / 1e6, 'thread': th, 'kernel': s.get(kn, str(kn)),
                    'running_kernels': running, 'blocking_api': blocking})
    return out


if __name__ == '__main__' and '--bursts' not in sys.argv:
    if '--stalls' in sys.argv:
        g = float(sys.argv[sys.argv.index('--gap-ms') + 1]) if '--gap-ms' in sys.argv else 50.0
        print(json.dumps(stalls(sys.argv[1], g), indent=1)); sys.exit(0)
    main()


def bursts(path, gap_us=150):
    """Per PDSCH-consumer thread, split its GPU activity into bursts (~ decode bodies): launch API calls and kernels
    closer than gap_us. Per burst: wall D, union of its kernels K, kernel count. Prints p50/p90/p99 of D, K, D-K."""
    db = sqlite3.connect(path)
    S = dict(db.execute('select id, value from StringIds'))
    tname = {tid: S.get(n, '?') for n, _, tid in db.execute('select nameId, priority, globalTid from ThreadNames')}
    rt = {c: (t, s, e) for s, e, t, c in db.execute('select start, end, globalTid, correlationId from CUPTI_ACTIVITY_KIND_RUNTIME')}
    ev = defaultdict(list)
    for s, e, c in db.execute('select start, end, correlationId from CUPTI_ACTIVITY_KIND_KERNEL'):
        if c in rt and tname.get(rt[c][0], '').startswith('passivePdsch'):
            ev[rt[c][0]].append((rt[c][1], e, s))  # launch start, kernel end, kernel start
    D, K, N, H = [], [], [], []
    for t, L in ev.items():
        L.sort()
        cur = None
        for ls, ke, ks in L:
            if cur and ls - cur['end'] < gap_us * 1000:
                cur['end'] = max(cur['end'], ke)
                cur['k'].append((ks, ke))
            else:
                if cur:
                    D.append(cur['end'] - cur['start']); K.append(union_ns(cur['k'])); N.append(len(cur['k']))
                cur = dict(start=ls, end=ke, k=[(ks, ke)])
        if cur:
            D.append(cur['end'] - cur['start']); K.append(union_ns(cur['k'])); N.append(len(cur['k']))
    G = [d - k for d, k in zip(D, K)]
    for name, v, f in (('burst wall us', D, 1e3), ('kernel-busy us', K, 1e3), ('wall-minus-kernel us', G, 1e3), ('kernels/burst', N, 1)):
        print('  %-22s n=%d p50=%.0f p90=%.0f p99=%.0f mean=%.0f' % (name, len(v), pct(v, 50) / f, pct(v, 90) / f, pct(v, 99) / f, sum(v) / len(v) / f))
    print('  kernel-busy share of burst wall: %.1f %%' % (100.0 * sum(K) / sum(D)))


if __name__ == '__main__' and '--bursts' in sys.argv:
    bursts(sys.argv[1])
