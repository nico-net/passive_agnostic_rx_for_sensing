#!/usr/bin/env python3
"""Equivalence table (2026-10-18 cleanup). usage: eq_cmp.py idx...   (idx lines: cap arm i dir)
Per (cap, arm): DL/UL n_real, crc_q, drops, p50, p99 per run + median/range; decoded-RNTI sets (probe=0 grants) per arm."""
import sys, re, subprocess, collections, statistics as st
G = '/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx/goal_metrics.py'
ANSI = re.compile(r'\x1b\[[0-9;]*m')
runs = collections.defaultdict(list)
for idx in sys.argv[1:]:
    for l in open(idx):
        p = l.split()
        if len(p) == 4: runs[(p[0], p[1])].append(p[3])
def num(o, k):
    m = re.search(rf'\b{k}=([\d.]+)', o); return float(m.group(1)) if m else float('nan')
def rntis(log):
    dl, ul = collections.Counter(), collections.Counter()
    for raw in open(log, errors='replace'):
        if 'EVMGRANT' in raw or 'UL_PUSCH_CRC' in raw:
            l = ANSI.sub('', raw); m = re.search(r'rnti=(\S+)', l)
            if not m or 'probe=0' not in l and 'probe=' in l: continue
            if 'crc=1' in l or 'ok=1' in l: (dl if 'EVMGRANT' in l else ul)[m.group(1)] += 1
    return dl, ul
res = {}
for (cap, arm), ds in sorted(runs.items()):
    rows = []
    for d in ds:
        o = subprocess.run(['python3', G, d + '/run.log'], capture_output=True, text=True).stdout.splitlines()
        dl = [x for x in o if ' DL ' in x][0]; ul = [x for x in o if ' UL ' in x][0]
        r = {k: num(dl, k) for k in ('n_real', 'crc_q', 'drops_pct', 'tot_p50', 'tot_p99')}
        r.update({'u_' + k: num(ul, k) for k in ('n_real', 'crc_q', 'drops_pct', 'tot_p50', 'tot_p99')})
        rows.append(r); print(f"{cap} {arm} {d.split('/')[-1]}", ' '.join(f"{k}={v:g}" for k, v in r.items()))
    res[(cap, arm)] = (rows, [rntis(d + '/run.log') for d in ds])
print()
for (cap, arm), (rows, rs) in res.items():
    f = lambda k: f"{st.median(r[k] for r in rows):g} [{min(r[k] for r in rows):g}-{max(r[k] for r in rows):g}]"
    print(f"{cap:3} {arm:3} n={len(rows)} DL n_real {f('n_real')} crc_q {f('crc_q')} drops {f('drops_pct')} p50 {f('tot_p50')} p99 {f('tot_p99')} | UL n_real {f('u_n_real')} crc_q {f('u_crc_q')} drops {f('u_drops_pct')} p50 {f('u_tot_p50')} p99 {f('u_tot_p99')}")
for cap in sorted({c for c, _ in res}):
    if (cap, 'OLD') in res and (cap, 'NEW') in res:
        for i, name in ((0, 'DL'), (1, 'UL')):
            sets = {a: set().union(*[set(r[i]) for r in res[(cap, a)][1]]) for a in ('OLD', 'NEW')}
            cnt = {a: collections.Counter() for a in sets}
            for a in sets:
                for r in res[(cap, a)][1]: cnt[a].update(r[i])
            big = {a: {k for k, v in cnt[a].items() if v >= 30} for a in sets}
            print(f"{cap} {name} RNTI (CRC ok) union OLD={len(sets['OLD'])} NEW={len(sets['NEW'])}; >=30 decodes OLD={len(big['OLD'])} NEW={len(big['NEW'])}; only-OLD(>=30)={sorted(big['OLD']-big['NEW'])} only-NEW(>=30)={sorted(big['NEW']-big['OLD'])}")
