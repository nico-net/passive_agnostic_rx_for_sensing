#!/usr/bin/env python3
"""SCAN2 replay statistics. usage: scan2_stats.py run.log [..] | scan2_stats.py overlap a/run.log b/run.log
Per run: scan loss, occasion p50/p99/max (SCANTIME_FINE / SCANTIME / BTIM occ_total hist), BTIM stage means, SCAN2 counters,
PDSCH crc_ok/decoded, accepted DCI count, UL locks, CPU per scan thread (cpu.txt next to run.log, ticks/100 over the 12 s of
capture + start-up are NOT normalised: it is CPU seconds of the whole run). Overlap: Jaccard of the rnti_seen DCI sets."""
import json, os, re, sys
ansi = re.compile(r'\x1b\[[0-9;]*m')


def lines(path):
    return [ansi.sub('', l) for l in open(path, errors='replace')]


def last(L, pat):
    m = None
    for l in L:
        x = re.search(pat, l)
        if x:
            m = x
    return m


def dci_set(L):
    s = set()
    for l in L:
        m = re.search(r'rnti_seen utc_ns=\d+ rnti=(\S+) sfn=(\d+) slot=(\d+) fmt=(\S+) cce=(\d+) al=(\d+)', l)
        if m and m.group(1) != '0xffff':
            s.add(m.groups())
    return s


def stats(path):
    L = lines(path)
    o = {'run': os.path.dirname(os.path.abspath(path)).split('replay_')[-1]}
    m = last(L, r'ISAC_METRICS (\{.*\})')
    if m:
        d = json.loads(m.group(1))
        q = d.get('scanq_queued', 0) or 1
        o['loss%'] = round(100.0 * (d['scanq_drop_full'] + d['scanq_drop_stale']) / q, 2)
        o['full/stale'] = '%d/%d' % (d['scanq_drop_full'], d['scanq_drop_stale'])
        o['accepts'] = d['pdcch_accepts']
        o['pdsch_crc/dec'] = '%d/%d' % (d['pdschq_crc_ok'], d['pdschq_decoded'])
        o['occasions'] = d['pdcch_occasions']
    m = last(L, r'SCANTIME_FINE (\S+) occasions=(\d+) p50<=(\d+)us p99<=(\d+)us over500=(\d+)')
    if m:
        o['occ_p50/p99_us'] = '%s/%s' % (m.group(3), m.group(4))
        o['over500'] = '%s/%s' % (m.group(5), m.group(2))
    m = last(L, r'SCANTIME \S+ occasions=\d+ p50<=\d+us p99<=\d+us max=(\d+)us')
    if m:
        o['occ_max_us'] = int(m.group(1))
    m = last(L, r'occ_total_us_hist (.*?) over_slot\(500us\)=(\d+)/(\d+)')
    if m:
        b = [(int(x.group(2)), int(x.group(3))) for x in re.finditer(r'(<|>=)(\d+)=(\d+)', '') ] if False else None
        ub = [50, 100, 200, 400, 800, 1600, 3200, 1e9]
        cnt = [int(x) for x in re.findall(r'=(\d+)', m.group(1))]
        tot = sum(cnt) or 1

        def pct(p):
            c = 0
            for u, n in zip(ub, cnt):
                c += n
                if c >= p * tot:
                    return int(u) if u < 1e9 else '>=3200'
        o['pass_p50/p99_us(hist ub)'] = '%s/%s' % (pct(0.5), pct(0.99))
        o['pass_over_slot'] = '%.1f%%' % (100.0 * int(m.group(2)) / max(1, int(m.group(3))))
    m = last(L, r'BTIM fep_llr.*')
    if m:
        t = m.group(0)
        for k in ('fep_llr', 'demap', 'decode', 'post', 'dlsweep', 'ulsweep', 'p2wait', 'ulwait', 'TOTAL'):
            x = re.search(k + r'\[n=(\d+) mean=([\d.]+)us max=([\d.]+)us', t)
            if x:
                o['b_' + k] = '%s(n=%s,max=%.0f)' % (x.group(2), x.group(1), float(x.group(3))) if k in ('TOTAL', 'ulsweep') else x.group(2)
    m = last(L, r'SCAN2 ul\[(.*)')
    if m:
        o['SCAN2'] = m.group(1).strip()
    o['ul_locks'] = sum(1 for l in L if 'UL automatic DCI length locked' in l)
    o['ULDCI'] = sum(1 for l in L if ' SENSING: ULDCI ' in l or 'SENSING: ULDCI ' in l)
    o['dci_set'] = len(dci_set(L))
    cpu = os.path.join(os.path.dirname(path), 'cpu.txt')
    if os.path.exists(cpu):
        agg = {}
        for l in open(cpu):
            p = l.split()
            if len(p) == 3:
                agg[p[0]] = agg.get(p[0], 0) + int(p[1]) + int(p[2])
        sc = {k: round(v / 100.0, 1) for k, v in agg.items() if 'dcch' in k.lower() or 'scan' in k.lower() or k.startswith('passivePd')}
        o['cpu_s_scan'] = sc
        o['cpu_s_total'] = round(sum(agg.values()) / 100.0, 1)
    return o


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'overlap':
        A, B = dci_set(lines(a[1])), dci_set(lines(a[2]))
        print('jaccard=%.3f |A|=%d |B|=%d |A&B|=%d' % (len(A & B) / max(1, len(A | B)), len(A), len(B), len(A & B)))
    else:
        for p in a:
            print(json.dumps(stats(p), sort_keys=True))
