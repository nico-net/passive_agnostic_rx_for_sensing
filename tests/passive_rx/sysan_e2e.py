#!/usr/bin/env python3
"""SYSAN: per-grant end-to-end latency from a run.log with ISAC_E2E_TRACE=1.
DL (EVMGRANT ... e2e=arr2enq,qwait,pre,dec,first_body,bodies,tot,lag; probe=0 = real grants):
  arr2enq = slot samples read -> DCI accepted and job enqueued (PDCCH scan latency)
  qwait   = enqueued -> dequeued by a PDSCH consumer
  pre     = dequeued -> decode call start (IQ copy, stale check, GPU FEP group)
  dec     = whole decode call incl. retries; first = its first body; bodies = passes
  tot     = slot samples read -> decode done
UL (UL_E2E; pick=1 jobs are probe-ring layout-search work, pick=0 real grants): arr2deq = target slot read -> dequeued, wall = decode wall, tot = arrival -> done.
usage: sysan_e2e.py [--json] run.log [...]"""
import json, re, sys

R = re.compile(r'EVMGRANT rnti=0x(\w+) \S+ crc=(\d) mcs=(\d+) .*? Nl=(\d) rb=(\d+)\+(\d+) .*? probe=(\d) .*? e2e=(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(-?\d+),(-?\d+)')
U = re.compile(r'UL_E2E rnti=0x(\w+) st=(\d+) prb=(\d+) mcs=(\d+) pick=(\d) arr2deq=(-?\d+) wall=(\d+) tot=(-?\d+) cpu_us=(-?\d+)')


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))]


def summ(v):
    v = [x for x in v if x >= 0]
    return dict(n=len(v), p50=pct(v, 50), p90=pct(v, 90), p99=pct(v, 99), max=max(v) if v else None,
                gt500=round(100.0 * sum(1 for x in v if x > 500) / len(v), 1) if v else None)


def run(path):
    dl = dict(arr2enq=[], qwait=[], pre=[], dec=[], first=[], tot=[], lag=[], retry_dec=[], crc_ok=0, n=0, bodies=[])
    dl_ok, dl_fail = [], []
    ul = dict(arr2deq=[], wall=[], tot=[], cpu=[], n=0, ok=0)
    for l in open(path, errors='replace'):
        m = R.search(l)
        if m and m.group(7) == '0':
            g = m.groups()
            a, q, pre, dec, first, bod, tot, lag = map(int, g[7:15])
            dl['n'] += 1
            dl['crc_ok'] += g[1] == '1'
            for k, v in (('arr2enq', a), ('qwait', q), ('pre', pre), ('dec', dec), ('first', first), ('tot', tot), ('lag', lag), ('bodies', bod)):
                dl[k].append(v)
            (dl_ok if g[1] == '1' else dl_fail).append(dec)
            if bod > 1:
                dl['retry_dec'].append(dec - first)
            continue
        m = U.search(l)
        if m:
            g = m.groups()
            ul['n'] += 1
            ul['ok'] += g[1] == '0'
            ul['arr2deq'].append(int(g[5])); ul['wall'].append(int(g[6])); ul['tot'].append(int(g[7])); ul['cpu'].append(int(g[8]))
    o = dict(dl_n=dl['n'], dl_crc_ok=dl['crc_ok'])
    for k in ('arr2enq', 'qwait', 'pre', 'first', 'dec', 'tot', 'retry_dec'):
        o['dl_' + k] = summ(dl[k])
    o['dl_dec_crc_ok'] = summ(dl_ok)
    o['dl_dec_crc_fail'] = summ(dl_fail)
    b = dl['bodies']
    o['dl_bodies_mean'] = round(sum(b) / len(b), 2) if b else None
    o['dl_multi_body_pct'] = round(100.0 * sum(1 for x in b if x > 1) / len(b), 1) if b else None
    # share of decode-call time spent beyond the first body (recovery passes)
    o['dl_retry_time_pct'] = round(100.0 * sum(dl['retry_dec']) / max(1, sum(x for x in dl['dec'] if x > 0)), 1)
    o['ul_n'], o['ul_ok'] = ul['n'], ul['ok']
    for k in ('arr2deq', 'wall', 'tot', 'cpu'):
        o['ul_' + k] = summ(ul[k])
    return o


def main():
    js = '--json' in sys.argv
    for p in [a for a in sys.argv[1:] if a != '--json']:
        o = run(p)
        if js:
            print(json.dumps(dict(run=p, **o)))
            continue
        print('==', p)
        print('DL real grants n=%d crc_ok=%d bodies mean=%s multi-body=%s%% retry share of decode time=%s%%' % (
            o['dl_n'], o['dl_crc_ok'], o['dl_bodies_mean'], o['dl_multi_body_pct'], o['dl_retry_time_pct']))
        for k in ('arr2enq', 'qwait', 'pre', 'first', 'dec', 'dec_crc_ok', 'dec_crc_fail', 'retry_dec', 'tot'):
            s = o['dl_' + k]
            print('  %-13s n=%-6d p50=%-7s p90=%-7s p99=%-7s max=%-8s >500us=%s%%' % (k, s['n'], s['p50'], s['p90'], s['p99'], s['max'], s['gt500']))
        print('UL real grants n=%d ok(st=0)=%d' % (o['ul_n'], o['ul_ok']))
        for k in ('arr2deq', 'wall', 'cpu', 'tot'):
            s = o['ul_' + k]
            print('  %-13s n=%-6d p50=%-7s p90=%-7s p99=%-7s max=%-8s >500us=%s%%' % (k, s['n'], s['p50'], s['p90'], s['p99'], s['max'], s['gt500']))


if __name__ == '__main__':
    main()
