#!/usr/bin/env python3
"""Per-grant decode-stage split from EVMGRANT stg= (ISAC_E2E_TRACE=1 + ISAC_PDCCH_TIMING=1 ISAC_PDTIM_PCT=1), one run at a time, never pooled.
usage: stage_split.py run.log [...]
stg=fep:w/c/g,chest:w/c/g,demod:w/c/g,ldpc:w/c/g,prep:p,wq:q cb=C core=X lk=L   (us; w wall, c thread-CPU, g GPU-poll wait; first body, probe=0)
Share of the first body (e2e field 5): gpuwait = sum g; cpu = sum max(c-g,0) (poll spin is CPU too, counted once as gpuwait); lock = lk;
unaccounted = rest (descheduled/blocked outside the instrumented waits, untimed code between stages). wq (GPU FEP worker wait) is the pre-step, not in the body."""
import re, sys
R = re.compile(r'EVMGRANT rnti=0x\w+ (\d+)\.(\d+) crc=\d .*? Nl=(\d) rb=(\d+)\+(\d+) .*? probe=0 .*? e2e=-?\d+,-?\d+,-?\d+,-?\d+,(-?\d+),\d+,-?\d+,-?\d+.*? stg=(\S+) cb=(\d+) core=(-?\d+) lk=(\d+)')
S = re.compile(r'(\w+):(\d+)/(\d+)/(\d+)')
ST = ('fep', 'chest', 'demod', 'ldpc')
def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p / 100.0 * len(v)))] if v else 0
def load(path):
    out = []
    for l in open(path, errors='replace'):
        if 'EVMGRANT' not in l or ' stg=' not in l: continue
        m = R.search(l)
        if not m: continue
        f, s, nl, rb0, nrb, first, stg, cb, core, lk = m.groups()
        d = {'first': int(first), 'nl': int(nl), 'nrb': int(nrb), 'cb': int(cb), 'core': int(core), 'lk': int(lk), 'prep': 0, 'wq': 0}
        for k, a, b, c in S.findall(stg): d[k] = (int(a), int(b), int(c))
        for k in ('prep', 'wq'):
            mm = re.search(k + r':(\d+)', stg)
            if mm: d[k] = int(mm.group(1))
        if d['first'] > 0 and all(k in d for k in ST): out.append(d)
    return out
def shares(rows):
    t = sum(r['first'] for r in rows) or 1
    g = sum(r[k][2] for r in rows for k in ST); c = sum(max(r[k][1] - r[k][2], 0) for r in rows for k in ST); lk = sum(r['lk'] for r in rows)
    return 100.0 * c / t, 100.0 * g / t, 100.0 * lk / t, 100.0 * (t - c - g - lk) / t
def table(name, rows):
    if not rows: print('  %-26s n=0' % name); return
    f = [r['first'] for r in rows]
    cells = ['n=%d first %d/%d' % (len(rows), pct(f, 50), pct(f, 90))]
    for k in ST: cells.append('%s w%d/%d c%d/%d g%d/%d' % (k, pct([r[k][0] for r in rows], 50), pct([r[k][0] for r in rows], 90), pct([r[k][1] for r in rows], 50),
                                                           pct([r[k][1] for r in rows], 90), pct([r[k][2] for r in rows], 50), pct([r[k][2] for r in rows], 90)))
    for k in ('prep', 'lk', 'wq'): cells.append('%s %d/%d' % (k, pct([r[k] for r in rows], 50), pct([r[k] for r in rows], 90)))
    c, g, lk, u = shares(rows)
    print('  %-26s %s | share%% cpu %.0f gpuwait %.0f lock %.0f unacc %.0f' % (name, ' | '.join(cells), c, g, lk, u))
def report(path):
    rows = load(path)
    print('== %s: %d first bodies with stg= (median/p90 us per column; wall w, cpu c, gpuwait g)' % (path, len(rows)))
    if not rows: return
    p99 = pct([r['first'] for r in rows], 99)
    sets = (('all', rows), ('> p99 (%d us)' % p99, [r for r in rows if r['first'] > p99]), ('> 5 ms', [r for r in rows if r['first'] > 5000]))
    for nm, rs in sets:
        print(' [%s]' % nm); table('all', rs)
        for lab, fn in (('cb 1-3', lambda r: r['cb'] <= 3), ('cb 4-15', lambda r: 4 <= r['cb'] <= 15), ('cb >=16', lambda r: r['cb'] >= 16),
                        ('Nl 1-2', lambda r: r['nl'] <= 2), ('Nl 3-4', lambda r: r['nl'] >= 3),
                        ('core X925 7,8,9', lambda r: r['core'] in (7, 8, 9)), ('core A725 3,4', lambda r: r['core'] in (3, 4))):
            table(lab, [r for r in rs if fn(r)])
if __name__ == '__main__':
    for p in sys.argv[1:]: report(p)
