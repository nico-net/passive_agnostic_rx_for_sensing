#!/usr/bin/env python3
"""ANONLEN arms: usage anonlen_stats.py <replays dir> <tag-glob prefix, e.g. m>. Per cap/arm mean+-sd of scan stats, DCI-set overlap
(R-R, R-N, N-N), distinct RNTIs and first-seen time (ms after the run's first rnti_seen). Uses scan2_stats.stats()."""
import glob, itertools, os, re, statistics as st, sys
sys.path.insert(0, os.path.dirname(__file__))
import scan2_stats as S

def rnti_first(L):
    t0, f = None, {}
    for l in L:
        m = re.search(r'rnti_seen utc_ns=(\d+) rnti=(\S+) ', l)
        if m and m.group(2) != '0xffff':
            t = int(m.group(1)); t0 = t if t0 is None else t0
            f.setdefault(m.group(2), (t - t0) / 1e6)
    return f

def ms(v):
    v = [x for x in v if x is not None]
    return '%.1f+-%.1f' % (st.mean(v), st.pstdev(v)) if v else 'n/a'

root, pre = sys.argv[1], sys.argv[2]
for cap in ('tim', 'b'):
    sets, firsts, rows = {}, {}, {}
    for arm in ('R', 'N'):
        runs = sorted(glob.glob('%s/replay_%s*_%s_%s_*/run.log' % (root, pre, cap, arm)))
        rs = []
        for p in runs:
            o = S.stats(p); L = S.lines(p)
            sc = o.get('cpu_s_scan', {})
            tot = re.search(r'([\d.]+)', o.get('b_TOTAL', '0'))
            cr = re.match(r'(\d+)/(\d+)', o.get('pdsch_crc/dec', '0/0'))
            p99 = re.search(r'/(\d+)', o.get('occ_p50/p99_us', '0/0'))
            rs.append(dict(loss=o.get('loss%'), p50=float(o['occ_p50/p99_us'].split('/')[0]), p99=float(p99.group(1)) if p99 else None,
                           mx=o.get('occ_max_us', 0) / 1000.0, pas=float(tot.group(1)), dls=float(o.get('b_dlsweep', 0)),
                           cpu=sc.get('passivePdcch0', 0) + sc.get('passivePdcch1', 0), crc=int(cr.group(1)), acc=o.get('accepts'),
                           nr=len(rnti_first(L))))
            sets[(arm, p)] = S.dci_set(L); firsts[(arm, p)] = rnti_first(L)
        rows[arm] = rs
        print(cap, arm, 'n=%d' % len(rs), ' '.join('%s=%s' % (k, ms([r[k] for r in rs])) for k in rs[0]) if rs else '')
    def jac(a, b): return len(a & b) / max(1, len(a | b))
    def avg(pairs): return '%.3f (n=%d)' % (st.mean([jac(sets[x], sets[y]) for x, y in pairs]), len(pairs)) if pairs else 'n/a'
    key = lambda arm: [k for k in sets if k[0] == arm]
    print(cap, 'overlap R-R', avg(list(itertools.combinations(key('R'), 2))), 'R-N', avg(list(itertools.product(key('R'), key('N')))),
          'N-N', avg(list(itertools.combinations(key('N'), 2))))
    # discovery: RNTIs seen by >= half of the R runs; first-seen offset per arm
    cnt = {}
    for k in key('R'):
        for r in firsts[k]: cnt[r] = cnt.get(r, 0) + 1
    core = [r for r, c in cnt.items() if c >= max(1, len(key('R')) // 2)]
    for arm in ('R', 'N'):
        per = []
        for k in key(arm):
            f = firsts[k]
            per.append((sum(1 for r in core if r in f), st.median([f[r] for r in core if r in f]) if any(r in f for r in core) else None,
                        max([f[r] for r in core if r in f] or [0])))
        print(cap, arm, 'core RNTIs=%d found/run %s  median first-seen ms %s  max first-seen ms %s' % (
            len(core), ms([x[0] for x in per]), ms([x[1] for x in per]), ms([x[2] for x in per])))
