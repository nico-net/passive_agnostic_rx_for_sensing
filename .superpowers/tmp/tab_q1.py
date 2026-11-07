#!/usr/bin/env python3
"""tab_q1.py <campaign dir>: day A/B tables from <run>/m.json (m_q1.py). Per cell, BASE vs NEW, mean (min-max) over valid arms.
Arms that are not OK (wrong PCI / crash / not RX_EXIT=130), or invalid (SENSE_SCHED/pthread failure; NEW only for the invalid rule), are listed and excluded."""
import sys, json, re, collections, statistics
C = sys.argv[1]; rows = []; bad = []
for ln in open(C + '/runs.tsv'):
    t, r, pci, ok, arm, wall = ln.rstrip('\n').split('\t')
    if re.search(r'T[BN]$', t): continue
    m = json.load(open(r + '/m.json')) if ok == 'OK' else None
    ex = open(r + '/rx_exit.txt').read().strip() if ok == 'OK' else ''
    if ok != 'OK' or ex != 'RX_EXIT=130': bad.append((t, r, ok, ex)); continue
    if t.find('T') > 0 and re.search(r'T[BN]', t): continue
    m.update(tag=t, arm=arm, cell=pci); rows.append(m)
    if m['sched_fail'] or m['pthread_fail']: bad.append((t, r, 'INVALID sched/pthread', m['sched_fail'], m['pthread_fail']))
def ms(v, f='%.2f'):
    v = [x for x in v if x is not None and x == x]
    return '-' if not v else (f + ' (' + f + '-' + f + ')') % (sum(v) / len(v), min(v), max(v))
def g(m, p):
    for k in p.split('.'):
        if m is None: return None
        m = m.get(k) if isinstance(m, dict) else m[int(k)]
    return m
D = lambda m, k: (m['drops'].get(k) or 0)
print('Excluded / flagged arms:', bad)
def parse_wbq(m):
    if not m['wbq']: return None
    mm = re.search(r'wide=(\d+) short=(\d+) rules\(hit/first/prbs\)=\[(.*)\]', m['wbq'][0])
    if not mm: return None
    rules = {}
    for it in mm.group(3).split():
        n, v = it.split(':'); h, f, p = map(int, v.split('/')); rules[n] = (h, f, p)
    return int(mm.group(1)), int(mm.group(2)), rules
for cell in sorted({m['cell'] for m in rows}):
    R = {a: [m for m in rows if m['cell'] == cell and m['arm'] == a] for a in ('base', 'fix')}
    print('\n### cell', cell, '| BASE arms', [m['tag'] for m in R['base']], '| NEW arms', [m['tag'] for m in R['fix']])
    def row(nm, fn, f='%.2f'):
        print('| %s | %s | %s |' % (nm, ms([fn(m) for m in R['base']], f), ms([fn(m) for m in R['fix']], f)))
    print('\n#### 1. headline: records/s with valid PRBs >= 51 / >= 128 (rate = count / rs-record span)\n| source/layers | BASE | NEW |\n|---|---|---|')
    keys = sorted({k for m in rows if m['cell'] == cell for k in m['by_layer']})
    for k in keys:
        if k.endswith('Lna') and False: continue
        row(k + ' v>=51 /s', lambda m, k=k: g(m, 'by_layer.%s.v51_s' % k) if k in m['by_layer'] else 0.0)
        row(k + ' v>=128 /s', lambda m, k=k: g(m, 'by_layer.%s.v128_s' % k) if k in m['by_layer'] else 0.0)
    print('\n#### 1b. proven wide grants (CRC-ok or partial-TB proven) with a pdsch record of valid >= 51 / >= 128\n| allocation / record threshold | BASE | NEW |\n|---|---|---|')
    for a in ('alloc51', 'alloc128'):
        row('%s grants proven (n/arm)' % a, lambda m, a=a: m['yield'][a]['n'], '%.0f')
        row('%s: any pdsch record' % a, lambda m, a=a: 100 * m['yield'][a]['any'], '%.1f')
        row('%s: record valid>=51 %%' % a, lambda m, a=a: 100 * m['yield'][a]['v51'], '%.1f')
        row('%s: record valid>=128 %%' % a, lambda m, a=a: 100 * m['yield'][a]['v128'], '%.1f')
    print('\n#### 2. per-rule PRBs removed in wide records (records\' inval; per 1000 records with nb_rb>=T) and SENSE_WBQ last line\n| item | BASE | NEW |\n|---|---|---|')
    rules = sorted({r for m in rows if m['cell'] == cell for v in m['inval'].values() for r in v['prbs']})
    for T in (128, 51):
        for src in ('pdsch_dmrs', 'pdsch_data'):
            kk = '%s/nb%d' % (src, T)
            row('inval %s records n/arm' % kk, lambda m, kk=kk: m['inval'][kk]['n'], '%.0f')
            row('inval %s records with any removal %%' % kk, lambda m, kk=kk: 100 * m['inval'][kk]['with_inval'] / max(m['inval'][kk]['n'], 1), '%.1f')
            for r in rules: row('inval %s PRBs removed by %s /1000 rec' % (kk, r), lambda m, kk=kk, r=r: 1000 * m['inval'][kk]['prbs'].get(r, 0) / max(m['inval'][kk]['n'], 1), '%.0f')
    wq = {a: [parse_wbq(m) for m in R[a]] for a in R}
    for lab, i in (('wide', 0), ('short', 1)):
        print('| WBQ %s (limit: BASE 128 / NEW 51, NOT comparable) | %s | %s |' % (lab, ms([w[i] for w in wq['base'] if w], '%.0f'), ms([w[i] for w in wq['fix'] if w], '%.0f')))
    wr = sorted({r for a in wq for w in wq[a] if w for r in w[2]})
    for r in wr:
        print('| WBQ prbs %s (hit/first) | %s | %s |' % (r, ms([w[2][r][2] for w in wq['base'] if w and r in w[2]], '%.0f'), ms([w[2][r][2] for w in wq['fix'] if w and r in w[2]], '%.0f')))
    print('\n#### 3. median snr_db_p10 (dB), records with valid >= 51 (v51) / >= 128 (v128)\n| source | BASE | NEW |\n|---|---|---|')
    for k in ('pdsch_dmrs/Lall', 'pdsch_data/Lall'):
        for t in ('v51', 'v128'): row('%s %s' % (k, t), lambda m, k=k, t=t: g(m, 'by_layer.%s.snr_p10_med_%s' % (k, t)))
    print('\n#### 4. decode, drops, CPU\n| item | BASE | NEW |\n|---|---|---|')
    row('wide(>=128) grants decoded/s', lambda m: m['wide_dec_s']); row('wide(>=128) CRC-ok/s', lambda m: m['wide_crc_s']); row('wide(>=128) partial-TB proven/s', lambda m: m['wide_part_s'])
    row('wide(>=51) grants decoded/s', lambda m: m['wide51_dec_s']); row('wide(>=51) CRC-ok/s', lambda m: m['wide51_crc_s']); row('wide(>=51) partial-TB proven/s', lambda m: m['wide51_part_s'])
    row('sense_rec_drop (record ring/writer) count', lambda m: D(m, 'sense_rec_drop'), '%.0f'); row('sense_io_errors', lambda m: (json.load(open(re.sub('m.json$', 'm.json', '/dev/null'))) if False else 0), '%.0f')
    for s in ('pdsch_dmrs', 'pdsch_data'):
        row('CFR ring full drops %s (count)' % s, lambda m, s=s: D(m, 'sense_cfr_drop_' + s), '%.0f')
        row('DA async drop %s %% of queued' % s, lambda m, s=s: 100. * D(m, 'sense_async_drop_' + s) / max(D(m, 'sense_async_queued_' + s), 1))
    row('sense_cfr_drop total (count)', lambda m: D(m, 'sense_cfr_drop'), '%.0f')
    row('scanq drop % (full/stale/epoch)', lambda m: 100. * (D(m, 'scanq_drop_full') + D(m, 'scanq_drop_stale') + D(m, 'scanq_drop_epoch')) / max(D(m, 'scanq_queued'), 1))
    row('scanq full %', lambda m: 100. * D(m, 'scanq_drop_full') / max(D(m, 'scanq_queued'), 1), '%.3f'); row('scanq stale %', lambda m: 100. * D(m, 'scanq_drop_stale') / max(D(m, 'scanq_queued'), 1), '%.3f')
    row('pdschq full %', lambda m: 100. * D(m, 'pdschq_drop_full') / max(D(m, 'pdschq_queued'), 1), '%.3f'); row('pdschq stale %', lambda m: 100. * D(m, 'pdschq_drop_stale') / max(D(m, 'pdschq_queued'), 1), '%.3f')
    row('CPU cores', lambda m: float(m['cpu']) if m['cpu'] else None)
    print('\n#### integrity per arm\n| arm | sched_fail | pthread | noqual pdsch_dmrs / cfr_drop | noqual pdsch_data / cfr_drop | noqual all sources | sync_losses |\n|---|---|---|---|---|---|---|')
    for m in R['base'] + R['fix']:
        print('| %s | %d | %d | %d / %d | %d / %d | %s | %s |' % (m['tag'], m['sched_fail'], m['pthread_fail'], m['noqual'].get('pdsch_dmrs', 0), D(m, 'sense_cfr_drop_pdsch_dmrs'), m['noqual'].get('pdsch_data', 0), D(m, 'sense_cfr_drop_pdsch_data'), m['noqual'], m['sync_losses']))
