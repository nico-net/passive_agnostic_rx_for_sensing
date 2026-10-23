#!/usr/bin/env python3
"""Rank-2 big-grant CRC diagnosis from existing OTA run.log files (analysis only, no receiver run).

usage: rank2_diag.py [--cache DIR] [--json] RUN_DIR...      per-run report, never pooled across runs
       rank2_diag.py --selftest                              unittest self-check

A "real" grant = EVMGRANT with probe=0, rnti != 0xffff (goal_metrics.parse_run filter) and, for the
confirmed-RNTI view, an RNTI that has >= CONF_MIN crc=1 grants (or a RECOVLATE rescue) in the same run.
RECOVLATE rescues are credited as CRC-ok (goal_metrics logic): key (rnti, "frame.slot").
snr_l is the per-layer decision-directed EVM SNR (-99 = n/a); the weak layer is min(snr_l[:Nl]).
"""
import collections, math, os, pickle, re, sys, unittest

ANSI = re.compile(r'\x1b\[[0-9;]*m')
KV = re.compile(r'(\w+)=(\S+)')
EVKEY = re.compile(r'EVMGRANT rnti=(\S+) (\d+\.\d+) ')
RECOV = re.compile(r'RECOVLATE rnti=(\S+) (\d+\.\d+) ')
CONF_MIN = 3
NUMF = ('mcs', 'tbl', 'Qm', 'Nl', 'nsym', 'evm', 'snr', 'nbrx', 'snr_cpe', 'probe', 'kappa', 'rho', 'comb', 'cdm', 'ndmrs',
        'it', 'itc', 'its', 'dec_us', 'rv', 'ndi', 'pid', 'tbs', 'l2', 'zabs', 'dmrs_pw', 'dmrs_ref', 'cb', 'core', 'crc', 'skip')


def _fl(s):
    try:
        return float(s)
    except ValueError:
        return float('nan')


def parse_line(line):
    """one EVMGRANT line -> dict or None"""
    line = ANSI.sub('', line)
    m = EVKEY.search(line)
    if not m:
        return None
    kv = dict(KV.findall(line))
    try:
        r = {k: _fl(kv[k]) for k in NUMF if k in kv}
        r['rnti'] = m.group(1)
        r['t'] = m.group(2)
        r['ports'] = int(kv['ports'], 16)
        rb = re.search(r'rb=(\d+)\+(\d+)', line)
        r['rb0'], r['rb'] = int(rb.group(1)), int(rb.group(2))
        for k in ('snr_l', 'dmrs_snr', 'br_snr', 'nvar_db', 'alpha'):
            if k in kv:
                r[k] = [_fl(x) for x in kv[k].split(',')]
        if 'e2e' in kv:
            e = kv['e2e'].split(',')
            r['qwait'] = int(e[1]); r['tot'] = int(e[6]); r['lag'] = int(e[7])
        if 'stg' in kv:
            ld = re.search(r'ldpc:(\d+)/(\d+)/(\d+)', line)
            if ld:
                r['ldpc_stg'] = tuple(int(x) for x in ld.groups())
        r['crc'] = int(r['crc'])
        r['probe'] = int(r['probe'])
        return r
    except (KeyError, ValueError, IndexError):
        return None


def parse_run(path, cache_dir=None):
    """-> (grants list, recov key set, misc counters). Cached as pickle when cache_dir is given."""
    ck = None
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        ck = os.path.join(cache_dir, os.path.basename(os.path.dirname(os.path.abspath(path))) + '.pkl')
        if os.path.exists(ck) and os.path.getmtime(ck) >= os.path.getmtime(path):
            with open(ck, 'rb') as f:
                return pickle.load(f)
    grants, rec = [], set()
    misc = collections.Counter()
    with open(path, errors='replace') as fh:
        for raw in fh:
            if 'EVMGRANT' in raw:
                g = parse_line(raw)
                if g:
                    grants.append(g)
            elif 'RECOVLATE' in raw:
                m = RECOV.search(ANSI.sub('', raw))
                if m:
                    rec.add(m.groups())
            elif 'CORE_POOL' in raw:
                misc['core_pool_lines'] += 1
            elif 'PDSCHQ queued=' in raw:
                m = re.search(r'queued=(\d+) decoded=(\d+) crc_ok=(\d+).*dropped\[full=(\d+) stale=(\d+)\]', ANSI.sub('', raw))
                if m:
                    for k, v in zip(('q_queued', 'q_decoded', 'q_crc_ok', 'q_full', 'q_stale'), m.groups()):
                        misc[k] += int(v)
    out = (grants, rec, dict(misc))
    if ck:
        with open(ck, 'wb') as f:
            pickle.dump(out, f)
    return out


def real_grants(grants, rec, confirmed_only=True):
    """probe=0, rnti != 0xffff, RECOVLATE credited; confirmed_only keeps RNTIs with >= CONF_MIN crc-ok grants."""
    g2 = []
    for g in grants:
        if g['probe'] != 0 or g['rnti'].lower() == '0xffff':
            continue
        g = dict(g)
        g['recov'] = 0
        if not g['crc'] and (g['rnti'], g['t']) in rec:
            g['crc'] = 1; g['recov'] = 1
        g2.append(g)
    if confirmed_only:
        okc = collections.Counter(g['rnti'] for g in g2 if g['crc'])
        g2 = [g for g in g2 if okc[g['rnti']] >= CONF_MIN]
    return g2


def wsnr(g):
    nl = max(1, int(g['Nl']))
    v = [x for x in g.get('snr_l', [])[:nl] if x > -90]
    return min(v) if v else float('nan')


def pct(a, b):
    return 100.0 * a / b if b else float('nan')


def crc_table(gs, keyf, label):
    d = collections.defaultdict(lambda: [0, 0])
    for g in gs:
        k = keyf(g)
        d[k][0] += g['crc']; d[k][1] += 1
    lines = ['  %s' % label]
    for k in sorted(d, key=lambda x: (str(type(x)), x)):
        lines.append('    %-18s n=%6d ok=%5.1f%%' % (k, d[k][1], pct(*d[k])))
    return '\n'.join(lines)


def bin_of(v, edges):
    if v != v:
        return 'nan'
    for i, e in enumerate(edges):
        if v < e:
            return '%s<%g' % ('' if i == 0 else '>=%g,' % edges[i - 1], e)
    return '>=%g' % edges[-1]


def mcs_class(g):
    m = int(g['mcs'])
    return 'mcs0-4' if m <= 4 else 'mcs5-9' if m <= 9 else 'mcs10-16' if m <= 16 else 'mcs17+'


_GM = None


def _gm():
    global _GM
    if _GM is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import goal_metrics as m
        _GM = m
    return _GM


def margin(g, mode='min'):
    """true (bias-inverted) layer SNR minus required SNR, dB. mode 'min': weakest layer; 'cap': capacity-averaged layers.
    None: no estimate (reserved MCS, or a layer within 1 dB of the DD noise floor); 'floor' is returned by margin_or_floor."""
    gm = _gm()
    nl = int(g['Nl']); qm = int(g['Qm'])
    v = [x for x in g.get('snr_l', [])[:nl] if x > -90]
    if len(v) < nl or nl < 1:
        return None
    req = gm.required_snr_db(int(g['mcs']), int(g['tbl']))
    if req is None:
        return None
    ts = [gm.true_snr_db(x, qm) for x in v]
    if any(t is None for t in ts):
        return None
    if mode == 'min':
        return min(ts) - req
    return 10 * math.log10(2 ** (sum(math.log2(1 + 10 ** (t / 10)) for t in ts) / nl) - 1) - req


def attribute(g):
    """disjoint failure class of one grant: skip | floor | below | edge | above (margin = min-layer margin)"""
    if g.get('skip', 0):
        return 'skip(no-chance gate)'
    m = margin(g, 'min')
    if m is None:
        return 'floor/unknown'
    return 'margin<-2' if m < -2 else 'margin-2..+2' if m < 2 else 'margin>=+2'


def crc50(gs, mode='min', minn=8):
    """margin (dB) at which the 1-dB-binned CRC curve first crosses 50 %; None when it never does"""
    bins = collections.defaultdict(lambda: [0, 0])
    for g in gs:
        m = margin(g, mode)
        if m is None:
            continue
        k = int(math.floor(m)); bins[k][0] += g['crc']; bins[k][1] += 1
    pts = [(k + 0.5, pct(*bins[k])) for k in sorted(bins) if bins[k][1] >= minn]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if y0 < 50 <= y1:
            return x0 + (50 - y0) / (y1 - y0) * (x1 - x0)
    return None


def mbin(g, mode='min'):
    m = margin(g, mode)
    if m is None:
        return 'floor/unk'
    for e in (-8, -6, -4, -2, 0, 2, 4, 6):
        if m < e:
            return '<%+d' % e
    return '>=+6'


MB_ORDER = ['<-8', '<-6', '<-4', '<-2', '<+0', '<+2', '<+4', '<+6', '>=+6', 'floor/unk']


def line(tab, order=None, minn=1):
    ks = order or sorted(tab, key=str)
    return ' | '.join('%s n=%d %.0f%%' % (k, tab[k][1], pct(*tab[k])) for k in ks if k in tab and tab[k][1] >= minn)


def tabulate(gs, keyf):
    d = collections.defaultdict(lambda: [0, 0])
    for g in gs:
        k = keyf(g); d[k][0] += g['crc']; d[k][1] += 1
    return d


def retx_census(gs):
    """chronological (log order) same (rnti,pid) with same NDI: 'retx_cand' = same TBS and rv 0 after a failed first pass (what the
    HARQ store could combine); 'ndi_same_other_tbs' = same NDI but another TBS (not this TB's retransmission)."""
    last, c = {}, collections.Counter()
    for g in gs:
        k = (g['rnti'], int(g['pid'])); p = last.get(k)
        if p is not None and p['ndi'] == g['ndi']:
            if p['tbs'] != g['tbs']:
                c['ndi_same_other_tbs'] += 1
            elif p['crc']:
                c['same_ndi_after_ok'] += 1
            else:
                c['retx_cand_after_fail'] += 1
                c['retx_cand_ok'] += g['crc']
        last[k] = g
    return c


def pctl(v, p):
    v = sorted(x for x in v if x is not None)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')


def report_run(path, cache_dir=None):
    grants, rec, misc = parse_run(path, cache_dir)
    out = ['== %s' % os.path.dirname(path)]
    P = out.append
    allr = real_grants(grants, rec, confirmed_only=False)
    gs = real_grants(grants, rec, confirmed_only=True)
    P('  EVMGRANT lines %d, probe=0 non-ffff %d, confirmed-RNTI %d, RECOVLATE credited %d, skip=1 %d; queue %s' % (
        len(grants), len(allr), len(gs), sum(g['recov'] for g in gs), sum(1 for g in gs if g.get('skip')), misc))
    for nl in (1, 2):
        for lo, hi, lab in ((200, 999, 'rb>=200'), (100, 199, 'rb100-199'), (0, 99, 'rb<100')):
            s = [g for g in gs if min(int(g['Nl']), 2) == nl and lo <= g['rb'] <= hi]
            P('  Nl%s %-9s n=%6d ok=%5.1f%%' % (nl if nl == 1 else '>=2', lab, len(s), pct(sum(g['crc'] for g in s), len(s))))
    for lab, lo in (('rb>=100', 100), ('rb>=200', 200)):
        b = [g for g in gs if g['Nl'] == 2 and g['rb'] >= lo]
        f = [g for g in b if not g['crc']]
        P('  [Q1/Q2] rank-2 %s: n=%d failures=%d; failure attribution %s' % (lab, len(b), len(f), dict(
            (k, '%d (%.0f%%)' % (v, pct(v, len(f)))) for k, v in collections.Counter(attribute(g) for g in f).most_common())))
    b2 = [g for g in gs if g['Nl'] == 2 and g['rb'] >= 100 and not g.get('skip')]
    b1 = [g for g in gs if g['Nl'] == 1 and g['rb'] >= 100 and not g.get('skip')]
    P('  [Q1] CRC vs min-layer margin (true SNR - required), rank-2 rb>=100: ' + line(tabulate(b2, mbin), MB_ORDER))
    P('  [Q1] CRC vs min-layer margin, rank-1 rb>=100:                      ' + line(tabulate(b1, mbin), MB_ORDER))
    for qm in (4, 6):
        c2 = crc50([g for g in gs if g['Nl'] == 2 and g['Qm'] == qm and g['rb'] >= 100 and not g.get('skip')])
        c1 = crc50([g for g in gs if g['Nl'] == 1 and g['Qm'] == qm and not g.get('skip')])
        c2c = crc50([g for g in gs if g['Nl'] == 2 and g['Qm'] == qm and g['rb'] >= 100 and not g.get('skip')], 'cap')
        P('  [Q1] Qm%d CRC50 margin: rank-1 all-rb %s, rank-2 rb>=100 min-layer %s, rank-2 rb>=100 capacity-avg %s' % (
            qm, '%.1f' % c1 if c1 is not None else 'n/a', '%.1f' % c2 if c2 is not None else 'n/a', '%.1f' % c2c if c2c is not None else 'n/a'))
    # raw (decision-directed) weak-layer SNR and MCS, rank-2 vs rank-1, same Qm
    E = (6, 8, 10, 12, 14, 16, 18, 20)
    for qm in (4, 6):
        for lab, sel in (('rank-2 rb>=100', b2), ('rank-1 rb>=100', b1)):
            q = [g for g in sel if g['Qm'] == qm]
            P('  [Q1] Qm%d %s CRC vs raw min(snr_l): %s' % (qm, lab, line(tabulate(q, lambda g: bin_of(wsnr(g), E)), minn=10)))
    P('  [Q1] rank-2 rb>=100 CRC vs MCS: ' + line(tabulate(b2, lambda g: int(g['mcs'])), sorted({int(g['mcs']) for g in b2}), minn=30))
    fl_unk = [g for g in b2 if not g['crc'] and margin(g) is None]
    P('  [Q1] rank-2 rb>=100 failures with unknown margin (layer at DD floor / reserved MCS): n=%d by Qm %s' % (
        len(fl_unk), dict(collections.Counter(int(g['Qm']) for g in fl_unk))))
    # layer imbalance at fixed capacity-averaged margin
    gm = _gm()
    for qm in (4, 6):
        for lo, hi in ((2, 4), (4, 6)):
            t = collections.defaultdict(lambda: [0, 0])
            for g in b2:
                if g['Qm'] != qm:
                    continue
                m = margin(g, 'cap')
                tt = [gm.true_snr_db(x, qm) for x in g['snr_l'][:2]]
                if m is None or None in tt or not (lo <= m < hi):
                    continue
                gp = abs(tt[0] - tt[1])
                k = 'gap<2' if gp < 2 else 'gap2-5' if gp < 5 else 'gap5-8' if gp < 8 else 'gap>=8'
                t[k][0] += g['crc']; t[k][1] += 1
            P('  [Q1] rank-2 rb>=100 Qm%d capacity-avg margin [%d,%d): %s' % (qm, lo, hi, line(t, ['gap<2', 'gap2-5', 'gap5-8', 'gap>=8'])))
    P('  [Q1] rho bins (rank-2 rb>=100 margin>=+2): ' + line(tabulate([g for g in b2 if (margin(g) or -99) >= 2], lambda g: bin_of(g['rho'], (0.1, 0.2, 0.3, 0.4)))))
    # which layer is the weak one when the gap is large
    wk = collections.Counter()
    for g in b2:
        a, c = g['snr_l'][0], g['snr_l'][1]
        if a > -90 and c > -90 and abs(a - c) >= 8:
            wk['layer0_weaker' if a < c else 'layer1_weaker'] += 1
    P('  [Q1] rank-2 rb>=100 with |snr_l gap| >= 8 dB: weak layer %s' % dict(wk))
    # failures a capacity-averaging receiver would have decoded (cap margin >= 0) although the min-layer margin is < +2
    fl2 = [g for g in b2 if not g['crc']]
    rec_cap = [g for g in fl2 if (margin(g, 'cap') is not None and margin(g, 'cap') >= 0 and (margin(g) is None or margin(g) < 2))]
    P('  [Q1] rank-2 rb>=100 failures with capacity-avg margin >= 0 but min-layer margin < +2 (weak-layer loss candidates): %d of %d (%.0f%%)' % (
        len(rec_cap), len(fl2), pct(len(rec_cap), len(fl2))))
    # residual (min-layer margin >= +2) failures by first matching structural cause
    gm = _gm()

    def rcause(g):
        if g['ports'] & 0xc:
            return 'ports 0x4/0x8 (CDM group 1)'
        if g['nsym'] <= 12:
            return 'nsym<=12 (TDRA S/L ambiguity, CORESET, CSI-RS)'
        if g['cdm'] == 2:
            return 'cdm=2'
        tt = [gm.true_snr_db(x, int(g['Qm'])) for x in g['snr_l'][:2]]
        if None not in tt and abs(tt[0] - tt[1]) >= 8:
            return 'layer gap>=8'
        if g['rho'] >= 0.3:
            return 'rho>=0.3'
        return 'other'
    res = [g for g in b2 if g['nsym'] > 0 and (margin(g) or -99) >= 2]
    rf = collections.Counter(rcause(g) for g in res if not g['crc']); rn = collections.Counter(rcause(g) for g in res)
    P('  [Q4] residual margin>=+2 failures by cause (fail/n): ' + '; '.join('%s %d/%d (%.0f%%)' % (k, rf[k], rn[k], pct(rf[k], rn[k])) for k in rn))
    # Q2 decoder / queue
    ok = [g for g in b2 if g['crc']]; fl = [g for g in b2 if not g['crc']]
    for nm in ('dec_us', 'tot', 'qwait', 'lag'):
        P('  [Q2] %s p50/p99 ok %s/%s fail %s/%s' % (nm, pctl([g.get(nm) for g in ok], .5), pctl([g.get(nm) for g in ok], .99),
                                                     pctl([g.get(nm) for g in fl], .5), pctl([g.get(nm) for g in fl], .99)))
    P('  [Q2] tot_us bins at margin>=+2: ' + line(tabulate([g for g in b2 if (margin(g) or -99) >= 2], lambda g: bin_of(g.get('tot', float('nan')), (1000, 2000, 3000, 5000)))))
    # Q3 HARQ
    P('  [Q3] rv dist rank-2 rb>=100: %s; HARQ census (all grants): %s; rank-2 rb>=100 only: %s' % (
        dict(collections.Counter(int(g['rv']) for g in b2)), dict(retx_census(gs)), dict(retx_census([g for g in gs if g['Nl'] == 2 and g['rb'] >= 100]))))
    # Q4 DM-RS config at margin >= +2 and at all margins
    hi = [g for g in b2 if (margin(g) or -99) >= 2]
    P('  [Q4] margin>=+2 rank-2 rb>=100: n=%d ok=%.1f%%' % (len(hi), pct(sum(g['crc'] for g in hi), len(hi))))
    for nm, kf in (('ports', lambda g: hex(g['ports'])), ('cdm', lambda g: int(g['cdm'])), ('ndmrs', lambda g: int(g['ndmrs'])),
                   ('comb', lambda g: int(g['comb'])), ('nsym', lambda g: int(g['nsym'])), ('tbl/Qm', lambda g: '%d/%d' % (g['tbl'], g['Qm'])),
                   ('slot', lambda g: int(g['t'].split('.')[1]))):
        P('  [Q4] %-6s %s' % (nm, line(tabulate(hi, kf), minn=20)))
    P('  [Q4] all-margin ports/cdm/ndmrs: ' + line(tabulate(b2, lambda g: (hex(g['ports']), int(g['cdm']), int(g['ndmrs']))), minn=20))
    # Q5
    P('  [Q5] cb (code blocks) at margin>=+2: ' + line(tabulate(hi, lambda g: bin_of(g['cb'], (2, 4, 8, 16, 32))), minn=20))
    # population
    by = collections.defaultdict(list)
    for g in b2:
        by[g['rnti']].append(g)
    for k, v in sorted(by.items(), key=lambda x: -len(x[1]))[:4]:
        w = [wsnr(g) for g in v]
        P('  [pop] %s n=%d ok=%.1f%% weak-layer snr p50 %.1f mcs p50 %d Qm6 %.0f%%' % (
            k, len(v), pct(sum(g['crc'] for g in v), len(v)), pctl(w, .5), pctl([g['mcs'] for g in v], .5), pct(sum(1 for g in v if g['Qm'] == 6), len(v))))
    return '\n'.join(out)


# ---------------------------------------------------------------- selftest
class SelfTest(unittest.TestCase):
    LINE = ('[0m[32m[PHY]    SENSING: EVMGRANT rnti=0x4e9d 96.5 crc=1 mcs=0 tbl=1 Qm=2 Nl=2 rb=192+16 nsym=12 evm=31.0 snr=10.2 '
            'br_snr=11.8,13.7,15.3,13.4 nbrx=4 tok=2191894 snr_cpe=10.3 probe=0 kappa=0.00 alpha=0.99,0.95,0.98,0.95 '
            'nvar_db=27.7,19.6,23.4,20.5 rho=0.17 comb=1 ports=0x3 cdm=2 ndmrs=2 eq=-1 occ=-1 it=-1 dec_us=500 rv=0 ndi=1 '
            'pid=15 tbs=1128 snr_l=15.1,7.9,-99.0,-99.0 l2=6 zabs=0 dmrs_snr=2.9,14.6,0.2,8.0,15.8 e2e=-1,163,166,500,497,1,668,1 '
            'stg=fep:0/0/0,chest:0/0/0,demod:2/2/0,ldpc:0/0/0,prep:0,wq:0 cb=1 core=7 lk=0')

    def test_parse(self):
        g = parse_line(self.LINE)
        self.assertEqual(g['rb'], 16); self.assertEqual(g['rb0'], 192); self.assertEqual(g['Nl'], 2)
        self.assertEqual(g['ports'], 3); self.assertEqual(g['tot'], 668)
        self.assertAlmostEqual(wsnr(g), 7.9)

    def test_real_filter(self):
        a = parse_line(self.LINE)
        b = dict(a); b['rnti'] = '0xffff'
        c = dict(a); c['probe'] = 1
        d = dict(a); d['crc'] = 0; d['rnti'] = '0x1111'
        gs = [a, a, a, b, c, d]
        self.assertEqual(len(real_grants(gs, set(), True)), 3)
        r = real_grants(gs, {('0x1111', '96.5')}, False)
        self.assertEqual(sum(g['recov'] for g in r), 1)

    def test_margin_and_attribute(self):
        a = parse_line(self.LINE)
        a['mcs'] = 0; a['tbl'] = 1; a['Qm'] = 2; a['snr_l'] = [20.0, 20.0, -99, -99]
        self.assertGreater(margin(a), 10)
        self.assertEqual(attribute(a), 'margin>=+2')
        a['snr_l'] = [20.0, 3.9, -99, -99]  # weak layer on the QPSK DD floor -> unknown
        self.assertEqual(attribute(a), 'floor/unknown')
        a['skip'] = 1
        self.assertTrue(attribute(a).startswith('skip'))

    def test_crc50_monotone(self):
        gs = []
        for m_snr in range(6, 24):
            g = parse_line(self.LINE)
            g['mcs'] = 5; g['tbl'] = 1; g['Qm'] = 4; g['Nl'] = 1; g['snr_l'] = [float(m_snr), -99, -99, -99]
            for i in range(20):
                h = dict(g); h['crc'] = 1 if m_snr >= 15 else 0
                gs.append(h)
        c = crc50(gs)
        self.assertIsNotNone(c)

    def test_retx_census(self):
        a = parse_line(self.LINE)
        a['crc'] = 0
        b = dict(a); b['crc'] = 1
        c = retx_census([a, b])
        self.assertEqual(c['retx_cand_after_fail'], 1); self.assertEqual(c['retx_cand_ok'], 1)

    def test_bins(self):
        self.assertEqual(bin_of(5, (4, 8)), '>=4,<8')
        self.assertEqual(bin_of(1, (4, 8)), '<4')
        self.assertEqual(bin_of(9, (4, 8)), '>=8')


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == '--selftest':
        unittest.main(argv=[sys.argv[0]])
    cache = None
    if a and a[0] == '--cache':
        cache = a[1]; a = a[2:]
    for d in a:
        print(report_run(os.path.join(d, 'run.log'), cache))
