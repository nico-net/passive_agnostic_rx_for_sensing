#!/usr/bin/env python3
"""RANK2-ITER scoring of the replay matrix (rank2_iter_batch.sh): per capture, per arm (LDPC cap), per repetition. Never pooled across captures.
usage: rank2_iter_score.py INDEX.tsv           markdown tables on stdout
       rank2_iter_score.py --selftest
Reuses rank2_diag.py (parse_line, real_grants, margin). Margin bins: <-2, -2..+2 (edge), >=+2, floor/unknown, skip (no-chance gate); rank-2 margin = weakest layer.
Pairing key of a grant: (rnti, 'frame.slot', occurrence index of that pair in log order): the same capture gives the same grants in every arm, so a paired
comparison removes between-grant variance (the arms differ only in discovery timing and decode)."""
import collections, os, re, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rank2_diag as d

ANSI = re.compile(r'\x1b\[[0-9;]*m')


def load(run_dir):
    """-> dict: grants (real, confirmed RNTIs), queue final counters, pool final counters"""
    path = os.path.join(run_dir, 'run.log')
    grants, rec = [], set()
    q = pool = None
    with open(path, errors='replace') as fh:
        for raw in fh:
            if 'EVMGRANT' in raw:
                g = d.parse_line(raw)
                if g:
                    grants.append(g)
            elif 'RECOVLATE' in raw:
                m = d.RECOV.search(ANSI.sub('', raw))
                if m:
                    rec.add(m.groups())
            elif 'PDSCHQ queued=' in raw:
                q = ANSI.sub('', raw)
            elif 'CORE_POOL n=' in raw:
                pool = ANSI.sub('', raw)
    occ = collections.Counter()
    for g in grants:
        k = (g['rnti'], g['t'])
        g['key'] = (g['rnti'], g['t'], occ[k])
        occ[k] += 1
    real = d.real_grants(grants, rec, confirmed_only=True)
    out = {'grants': real, 'q': {}, 'pool': {}}
    if q:
        m = re.search(r'queued=(\d+) decoded=(\d+) crc_ok=(\d+).*dropped\[full=(\d+) stale=(\d+)\]', q)
        if m:
            out['q'] = dict(zip(('queued', 'decoded', 'crc_ok', 'full', 'stale'), map(int, m.groups())))
    if pool:
        m = re.search(r'decode\[n=(\d+) waited=(\d+) promo=\d+ wait_us p50<=(\d+) p99<=(\d+) max=(\d+)\]', pool)
        if m:
            out['pool'] = dict(zip(('n', 'waited', 'p50', 'p99', 'max'), map(int, m.groups())))
    return out


def mclass(g):
    if g.get('skip', 0):
        return 'skip'
    m = d.margin(g, 'min')
    if m is None:
        return 'floor/unk'
    return '<-2' if m < -2 else 'edge' if m < 2 else '>=+2'


def ok_n(gs):
    return sum(g['crc'] for g in gs), len(gs)


def pc(a, n):
    return '%5.1f%% (%d/%d)' % (100.0 * a / n, a, n) if n else '   n/a (0)'


def subsets(gs):
    r2 = [g for g in gs if g['Nl'] == 2 and g['rb'] >= 100]
    return {
        'rank-2 rb>=100': r2,
        'rank-2 rb>=200': [g for g in r2 if g['rb'] >= 200],
        'rank-2 rb>=100 edge': [g for g in r2 if mclass(g) == 'edge'],
        'rank-2 rb>=100 <-2': [g for g in r2 if mclass(g) == '<-2'],
        'rank-2 rb>=100 >=+2': [g for g in r2 if mclass(g) == '>=+2'],
        'rank-2 rb>=100 floor/unk': [g for g in r2 if mclass(g) == 'floor/unk'],
        'rank-2 rb>=100 skip': [g for g in r2 if mclass(g) == 'skip'],
        'rank-2 any rb': [g for g in gs if g['Nl'] == 2],
        'rank-1 rb>=100': [g for g in gs if g['Nl'] == 1 and g['rb'] >= 100],
        'rank-1 any rb': [g for g in gs if g['Nl'] == 1],
        'all grants': gs,
    }


def p(v, q):
    return d.pctl(v, q)


def iters(gs):
    """cap-hit share and iteration histogram of grants that report it (it > 0)"""
    rep = [g for g in gs if g.get('it', -1) > 0]
    if not rep:
        return None
    hit = [g for g in rep if g['it'] >= g['itc'] > 0]
    okh = collections.Counter(int(g['it']) for g in rep if g['crc'] and not g.get('recov'))
    return len(rep), len(hit), okh


def paired(a, b, sel):
    """grants of both runs under the same key: (n common, a ok only, b ok only)"""
    ka = {g['key']: g for g in a if sel(g)}
    kb = {g['key']: g for g in b if sel(g)}
    com = set(ka) & set(kb)
    return len(com), sum(1 for k in com if ka[k]['crc'] and not kb[k]['crc']), sum(1 for k in com if kb[k]['crc'] and not ka[k]['crc'])


def main(idx):
    runs = collections.defaultdict(dict)  # cap name -> {(arm, rep): run}
    for ln in open(idx):
        f = ln.rstrip('\n').split('\t')
        if len(f) == 4:
            runs[f[0]][(int(f[1]), int(f[2]))] = load(f[3])
    for cap in runs:
        print('\n## capture %s' % cap)
        arms = sorted({a for a, _ in runs[cap]})
        reps = sorted({r for _, r in runs[cap]})
        print('\n### CRC-ok by subset (per run; reps side by side)\n')
        print('| subset | ' + ' | '.join('cap %d rep %d' % (a, r) for a in arms for r in reps if (a, r) in runs[cap]) + ' |')
        print('|---|' + '---|' * sum(1 for a in arms for r in reps if (a, r) in runs[cap]))
        for name in subsets([]):
            row = []
            for a in arms:
                for r in reps:
                    if (a, r) in runs[cap]:
                        row.append(pc(*ok_n(subsets(runs[cap][(a, r)]['grants'])[name])))
            print('| %s | %s |' % (name, ' | '.join(row)))
        print('\n### iterations, latency, queue (rank-2 rb>=100 = R2; all = every real grant)\n')
        print('| run | R2 n | R2 cap-hit | all cap-hit | ok it hist (all grants) | dec_us p50/p99 (all) | dec_us p50/p99 (R2) | tot_us p50/p99 (all) | queued | stale | full | pool decode waited/n | pool p99/max us |')
        print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
        for a in arms:
            for r in reps:
                if (a, r) not in runs[cap]:
                    continue
                R = runs[cap][(a, r)]
                gs = R['grants']
                r2 = subsets(gs)['rank-2 rb>=100']
                ir2, ia = iters(r2), iters(gs)
                hist = ' '.join('%d:%d' % kv for kv in sorted(ia[2].items())) if ia else 'n/a'
                dec = [g['dec_us'] for g in gs if g.get('dec_us') == g.get('dec_us')]
                dec2 = [g['dec_us'] for g in r2 if g.get('dec_us') == g.get('dec_us')]
                tot = [g['tot'] for g in gs if 'tot' in g]
                q, pl = R['q'], R['pool']
                print('| cap %d rep %d | %d | %s | %s | %s | %.0f/%.0f | %.0f/%.0f | %.0f/%.0f | %s | %s | %s | %s | %s |' % (
                    a, r, len(r2), '%d/%d' % (ir2[1], ir2[0]) if ir2 else 'n/a', '%d/%d' % (ia[1], ia[0]) if ia else 'n/a', hist,
                    p(dec, .5), p(dec, .99), p(dec2, .5), p(dec2, .99), p(tot, .5), p(tot, .99), q.get('queued', 'n/a'), q.get('stale', 'n/a'),
                    q.get('full', 'n/a'), '%s/%s' % (pl.get('waited', 'n/a'), pl.get('n', 'n/a')), '%s/%s' % (pl.get('p99', 'n/a'), pl.get('max', 'n/a'))))
        print('\n### paired on identical grants (same key in both runs): a=cap-8 rep r, b=cap X rep r; "gain" = ok in b only, "loss" = ok in a only\n')
        print('| subset | pair | common | gain | loss |')
        print('|---|---|---|---|---|')
        sels = {'R2 rb>=100': lambda g: g['Nl'] == 2 and g['rb'] >= 100 and not g.get('skip'),
                'R2 rb>=100 edge': lambda g: g['Nl'] == 2 and g['rb'] >= 100 and mclass(g) == 'edge',
                'rank-2 any rb': lambda g: g['Nl'] == 2 and not g.get('skip'),
                'rank-1 rb>=100': lambda g: g['Nl'] == 1 and g['rb'] >= 100,
                'all': lambda g: True}
        for nm, sel in sels.items():
            for r in reps:
                if (8, r) not in runs[cap]:
                    continue
                for a in arms:
                    if a != 8 and (a, r) in runs[cap]:
                        n, la, lb = paired(runs[cap][(8, r)]['grants'], runs[cap][(a, r)]['grants'], sel)
                        print('| %s | cap 8 vs %d rep %d | %d | %d | %d |' % (nm, a, r, n, lb, la))
            if (8, reps[0]) in runs[cap] and len(reps) > 1 and (8, reps[1]) in runs[cap]:
                n, la, lb = paired(runs[cap][(8, reps[0])]['grants'], runs[cap][(8, reps[1])]['grants'], sel)
                print('| %s | cap 8 rep %d vs rep %d (noise floor) | %d | %d | %d |' % (nm, reps[0], reps[1], n, lb, la))


class T(unittest.TestCase):
    def test_pc(self):
        self.assertEqual(pc(0, 0).strip(), 'n/a (0)')
        self.assertIn('50.0%', pc(1, 2))

    def test_paired(self):
        a = [{'key': (1, 'x', 0), 'crc': 1, 'Nl': 2, 'rb': 100}, {'key': (1, 'x', 1), 'crc': 0, 'Nl': 2, 'rb': 100}]
        b = [{'key': (1, 'x', 0), 'crc': 0, 'Nl': 2, 'rb': 100}, {'key': (1, 'x', 1), 'crc': 1, 'Nl': 2, 'rb': 100}, {'key': (9, 'y', 0), 'crc': 1, 'Nl': 2, 'rb': 100}]
        self.assertEqual(paired(a, b, lambda g: True), (2, 1, 1))

    def test_iters(self):
        gs = [{'it': 8, 'itc': 8, 'crc': 0}, {'it': 3, 'itc': 8, 'crc': 1}, {'it': -1, 'itc': 0, 'crc': 1}]
        n, hit, h = iters(gs)
        self.assertEqual((n, hit, dict(h)), (2, 1, {3: 1}))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        unittest.main(argv=[sys.argv[0]])
    else:
        main(sys.argv[1])
