#!/usr/bin/env python3
"""P99: tail decomposition of the PDSCH decode per run.log (replay or OTA), one block per run.
Needs ISAC_PDTIM_PCT=1 ISAC_PDTIM_CLASS=1 for the class split; ISAC_METRICS=1 for the queue histograms.
usage: p99_stats.py [--json] run.log [run.log ...]
Reports: body counts per class (real / retry / probe) and the stage time each class holds, bodies per real grant,
real p50/p99, per-stage p99, queue wait histogram (enqueue->dequeue) and stale %, drop reasons, retry recoveries per arm,
retry yields (ISAC_PDSCH_RETRY_YIELD), scan-pool service, and CRC-OK per MCS bucket (EVMGRANT probe=0, all SNR / SNR>=10)."""
import json, re, sys
ansi = re.compile(r'\x1b\[[0-9;]*m')


def last(lines, pat):
    m = None
    for l in lines:
        x = re.search(pat, l)
        if x:
            m = x
    return m


def bucket(mcs):
    return '0-4' if mcs <= 4 else '5-9' if mcs <= 9 else '10-16' if mcs <= 16 else '17+'


def stats(path):
    L = [ansi.sub('', l) for l in open(path, errors='replace')]
    o = {}
    m = last(L, r'PDTIMCLS us real\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+).*?\] retry\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+).*?\] probe\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+)')
    if m:
        g = m.groups()
        o['real'] = dict(n=int(g[0]), mean=float(g[1]), p50=int(g[2]), p90=int(g[3]), p99=int(g[4]), max_ms=round(float(g[5]) / 1000, 1))
        o['retry'] = dict(n=int(g[6]), mean=float(g[7]), p99=int(g[10]))
        o['probe'] = dict(n=int(g[12]), mean=float(g[13]), p99=int(g[16]))
        o['retry_per_real'] = round(o['retry']['n'] / max(1, o['real']['n']), 2)
    m = last(L, r'PDTIMCLSST ms\(<1/1-5/5-20/>20ms bodies\) real\[(.*?)\] retry\[(.*?)\] probe\[(.*?)\]')
    if m:
        tot = {}
        for cls, s in zip(('real', 'retry', 'probe'), m.groups()):
            tot[cls] = sum(int(x) for x in re.findall(r'(\d+)', s.replace('<1', '')) if x)
        # each stage field is a/b/c/d ms; re-parse cleanly
        for cls, s in zip(('real', 'retry', 'probe'), m.groups()):
            tot[cls] = sum(sum(int(v) for v in f.split('/')) for f in re.findall(r'=([\d/]+)', s))
        o['stage_s'] = {k: round(v / 1000, 1) for k, v in tot.items()}
    m = last(L, r'PDTIMPCT us fep\[n=\d+ p50=(\d+) p90=(\d+) p99=(\d+)\] chest\[n=\d+ p50=(\d+) p90=(\d+) p99=(\d+)\] demod\[n=\d+ p50=(\d+) p90=(\d+) p99=(\d+)\] ldpc\[n=\d+ p50=(\d+) p90=(\d+) p99=(\d+)\] sum\[n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+)\] ldpc_tb=(\d+) ldpc_cb=(\d+)')
    if m:
        g = list(map(int, m.groups()))
        o['p99_stage'] = dict(fep=g[2], chest=g[5], demod=g[8], ldpc=g[11])
        o['body'] = dict(n=g[12], p50=g[13], p90=g[14], p99=g[15])
        o['cb_per_tb'] = round(g[17] / max(1, g[16]), 2)
    m = last(L, r'SENSING: PDTIM calls=\d+ fep\[n=\d+ mean=([\d.]+)us cpu=([\d.]+)us.*?chest\[n=\d+ mean=([\d.]+)us cpu=([\d.]+)us.*?demod\[n=\d+ mean=([\d.]+)us cpu=([\d.]+)us.*?ldpc\[n=\d+ mean=([\d.]+)us cpu=([\d.]+)us')
    if m:
        g = list(map(float, m.groups()))
        o['mean_wall_cpu'] = dict(fep=g[0:2], chest=g[2:4], demod=g[4:6], ldpc=g[6:8])
    mj = None
    for l in L:
        if 'ISAC_METRICS' in l and '"pdschq_queued"' in l:
            mj = l
    if mj:
        def gi(k):
            x = re.search(r'"%s":(\d+)' % k, mj)
            return int(x.group(1)) if x else None
        q, st = gi('pdschq_queued'), gi('pdschq_drop_stale')
        o['pdschq'] = dict(queued=q, decoded=gi('pdschq_decoded'), crc_ok=gi('pdschq_crc_ok'), stale=st,
                           stale_pct=round(100.0 * st / max(1, q), 1) if q is not None else None, full=gi('pdschq_drop_full'))
        o['scanq'] = dict(queued=gi('scanq_queued'), stale=gi('scanq_drop_stale'), full=gi('scanq_drop_full'), max_lag=gi('scanq_max_lag'))
        x = re.search(r'"enqueue_dequeue":\[([\d,]+)\]', mj)
        if x:
            h = list(map(int, x.group(1).split(',')))
            n = max(1, sum(h))
            o['wait_hist_pct'] = dict(zip(['<0.1', '<0.25', '<0.5', '<1', '<2', '<4', '<8', '<16', '>16ms'], [round(100.0 * v / n, 1) for v in h]))
        x = re.search(r'"pdschq_reasons":\{([^}]*)\}', mj)
        if x:
            o['reasons'] = {k: int(v) for k, v in re.findall(r'"(\w+)":(\d+)', x.group(1))}
    tdra = [l for l in L if 'SENSING: DECODETDRA' in l]
    if tdra:
        ok = [l for l in tdra if 'after=1' in l]
        o['retry_seq'] = dict(n=len(tdra), recovered=len(ok), arms={})
        for l in ok:
            a = re.search(r' arm=(\d+)', l).group(1)
            o['retry_seq']['arms'][a] = o['retry_seq']['arms'].get(a, 0) + 1
    m = last(L, r'P99AGE slots\S+ pdsch_enq_real=\[([\d,]+)\] pdsch_enq_probe=\[([\d,]+)\] scan_deq=\[([\d,]+)\] scanq_fresh=(\d+) retry_chains=(\d+) retry_yields=(\d+) retry_gated=(\d+)')
    if m:
        def pct(h):
            v = list(map(int, h.split(',')))
            n = max(1, sum(v))
            return dict(n=sum(v), lt4=round(100.0 * sum(v[:2]) / n, 1), ge8=round(100.0 * sum(v[3:]) / n, 1), ge12=round(100.0 * sum(v[4:]) / n, 1),
                        ge18=round(100.0 * sum(v[6:]) / n, 1))
        o['age_enq_real'] = pct(m.group(1))
        o['age_enq_probe'] = pct(m.group(2))
        o['scan_lag_deq'] = pct(m.group(3))
        o['scanq_fresh'] = int(m.group(4))
        o['retry_yield_gated_chains'] = '%s/%s/%s' % (m.group(6), m.group(7), m.group(5))
    m = last(L, r'SCANTIME ded-only occasions=(\d+) p50<=(\d+)us p99<=(\d+)us max=(\d+)us')
    if m:
        o['scan'] = dict(occ=int(m.group(1)), p50=int(m.group(2)), p99=int(m.group(3)), max=int(m.group(4)))
    crc = {}
    for l in L:
        if 'EVMGRANT' not in l or ' probe=0' not in l:
            continue
        x = re.search(r' crc=(\d) mcs=(\d+) .*? Nl=(\d) .*? snr=(-?[\d.]+)', l)
        if not x:
            continue
        b = bucket(int(x.group(2)))
        for key in (b, b + '@10'):
            if key.endswith('@10') and float(x.group(4)) < 10:
                continue
            c = crc.setdefault(key, [0, 0])
            c[0] += int(x.group(1))
            c[1] += 1
    o['crc'] = {k: '%d/%d' % tuple(v) for k, v in sorted(crc.items())}
    for k in ('GPU_EXACT_CHEST', 'GPU_EXACT_DEMOD', 'GPU_EXACT_FEP'):
        m = last(L, k + r' calls=\d+ check_ok=\d+ check_bad=(\d+)')
        if m:
            o['bad_' + k.split('_')[-1].lower()] = int(m.group(1))
    o['mismatch'] = sum(1 for l in L if 'MISMATCH' in l and 'GPU_EXACT' in l)
    return o


if __name__ == '__main__':
    js = '--json' in sys.argv
    for p in [a for a in sys.argv[1:] if a != '--json']:
        o = stats(p)
        name = p.split('/')[-2] if '/' in p else p
        if js:
            print(json.dumps({name: o}))
        else:
            print('==', name)
            for k, v in o.items():
                print('  %-14s %s' % (k, v))
