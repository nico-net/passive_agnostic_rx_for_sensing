#!/usr/bin/env python3
"""sum_s1.py <campaign dir>: derived per-cell tables (drops %, est split, outside-base-range test, per-wide-grant rates)."""
import sys, json, collections
C = sys.argv[1]; rows = []
for ln in open(C + '/runs.tsv'):
    t, r, pci, ok, arm, wall = ln.rstrip('\n').split('\t'); m = json.load(open(r + '/m.json')); m.update(tag=t, arm=arm, cell=pci); rows.append(m)
f = lambda v: '%.3f (%.3f-%.3f)' % (sum(v) / len(v), min(v), max(v))
def D(m, k): return (m['drops'].get(k) or 0)
for cell in ('415', '923'):
    print('\n##', cell); R = {a: [m for m in rows if m['cell'] == cell and m['arm'] == a] for a in ('base', 'fix')}
    def row(nm, fn):
        b = [fn(m) for m in R['base']]; n = [fn(m) for m in R['fix']]
        out = 'above' if min(n) > max(b) else 'below' if max(n) < min(b) else 'inside'
        print('| %s | %s | %s | %s |' % (nm, f(b), f(n), out))
    print('| metric | BASE | NEW | NEW mean range vs BASE min-max (all 5 NEW runs) |\n|---|---|---|---|')
    row('wide dec/s', lambda m: m['wide_dec_s']); row('wide proven/s', lambda m: m['wide_proven_s'])
    row('dmrs v128 per wide grant', lambda m: m['dmrs_v128_s'] / m['wide_dec_s']); row('data v128 per wide grant', lambda m: m['data_v128_s'] / m['wide_dec_s'])
    row('dmrs v128+data v128 per wide grant', lambda m: (m['dmrs_v128_s'] + m['data_v128_s']) / m['wide_dec_s'])
    row('scanq drop % (full+stale+epoch)', lambda m: 100. * (D(m, 'scanq_drop_full') + D(m, 'scanq_drop_stale') + D(m, 'scanq_drop_epoch')) / D(m, 'scanq_queued'))
    row('scanq full %', lambda m: 100. * D(m, 'scanq_drop_full') / D(m, 'scanq_queued')); row('scanq stale %', lambda m: 100. * D(m, 'scanq_drop_stale') / D(m, 'scanq_queued'))
    row('pdschq full %', lambda m: 100. * D(m, 'pdschq_drop_full') / D(m, 'pdschq_queued')); row('pdschq stale %', lambda m: 100. * D(m, 'pdschq_drop_stale') / D(m, 'pdschq_queued'))
    row('sense ring rec drop /1e6 recs', lambda m: 1e6 * D(m, 'sense_rec_drop') / max(D(m, 'sense_rec_count'), 1))
    row('sense_cfr_drop (count)', lambda m: D(m, 'sense_cfr_drop'))
    row('DA async drop pdsch_data % of queued', lambda m: 100. * D(m, 'sense_async_drop_pdsch_data') / max(D(m, 'sense_async_queued_pdsch_data'), 1))
    row('DA async drop pdsch_dmrs (count)', lambda m: D(m, 'sense_async_drop_pdsch_dmrs'))
    row('records total (sense_rec_count)', lambda m: D(m, 'sense_rec_count') / m['dur'])
    row('lat wide p50 ms', lambda m: m['lat_us_wide'][0] / 1000.); row('lat wide p99 ms', lambda m: m['lat_us_wide'][1] / 1000.)
    row('lat all p50 ms', lambda m: m['lat_us_all'][0] / 1000.); row('lat all p99 ms', lambda m: m['lat_us_all'][1] / 1000.)
    for a in ('base', 'fix'):
        est = collections.Counter(); e128 = collections.Counter(); pt = pt128 = 0; nd = 0
        for m in R[a]:
            est.update(m['cfr_est']); e128.update(m['cfr_est_v128']); pt += m['dmrs_partial']; pt128 += m['dmrs_partial_v128']
        print(a, 'cfr_est all dmrs recs (5 runs summed):', dict(est), '| v128:', dict(e128), '| partial_tb dmrs recs', pt, 'of which v128', pt128)
        fn = collections.Counter()
        for m in R[a]: fn.update(m['funnel'])
        print(a, 'funnel proven wide (summed, 5 runs):', dict(fn), 'tot', sum(fn.values()))
    print('pdsch_data partial/whole per s: NEW', f([m['data_part_s'] for m in R['fix']]), f([m['data_whole_s'] for m in R['fix']]))
    print('tap totals NEW (last line per run):'); [print('  ', m['tag'], m['tap']) for m in R['fix']]
