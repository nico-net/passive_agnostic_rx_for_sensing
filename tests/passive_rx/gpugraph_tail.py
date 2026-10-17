#!/usr/bin/env python3
"""GPUGRAPH tail-path classifier (DL GPU data path v2, plan 2026-10-17 Task 1).
Per run.log: the tail (EVMGRANT probe=0 grants with tot >= p99, goal_metrics' index rule) split into e2e components
(a2e, qw, pre, dec, first, bodies, tot), and how many tail grants the stage path decoded. A grant counts as stage-path when
its stage timers fep + chest + demod + ldpc (first number of each stg= entry) exceed STAGE_US; the graph pass bypasses
those timers (they read a few us in a pass body).
usage: gpugraph_tail.py run.log..."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goal_metrics as gm

STAGE_US = 200
E2E = ('a2e', 'qw', 'pre', 'dec', 'first', 'bodies', 'tot', 'lag')

def parse(path):
    rows = []
    with open(path, errors='replace') as fh:
        for raw in fh:
            if 'EVMGRANT' not in raw:
                continue
            kv = dict(gm.KV.findall(gm.ANSI.sub('', raw)))
            if kv.get('probe', '0') != '0' or 'e2e' not in kv:
                continue
            try:
                e = dict(zip(E2E, (int(x) for x in kv['e2e'].split(','))))
                stg = {s.split(':')[0]: int(s.split(':')[1].split('/')[0]) for s in kv.get('stg', '').split(',') if ':' in s}
            except ValueError:
                continue
            e['stage'] = sum(stg.get(k, 0) for k in ('fep', 'chest', 'demod', 'ldpc')) > STAGE_US
            rows.append(e)
    return rows

def summarize(rows):
    if not rows:
        return None
    p99 = gm.pct([r['tot'] for r in rows], .99)
    tail = [r for r in rows if r['tot'] >= p99]
    out = {'n': len(rows), 'p99': p99, 'n_tail': len(tail), 'tail_stage': sum(r['stage'] for r in tail),
           'stage_share_all': 100.0 * sum(r['stage'] for r in rows) / len(rows)}
    for k in E2E[:7]:
        out['tail_' + k] = sum(r[k] for r in tail) / len(tail)
    return out

def main(paths):
    for p in paths:
        s = summarize(parse(p))
        if not s:
            print(f"{p}: no EVMGRANT e2e lines")
            continue
        print(f"tail n={s['n_tail']}/{s['n']} p99={s['p99']} us | tail mean a2e {s['tail_a2e']:.0f} qw {s['tail_qw']:.0f} pre {s['tail_pre']:.0f} "
              f"dec {s['tail_dec']:.0f} first {s['tail_first']:.0f} bodies {s['tail_bodies']:.2f} tot {s['tail_tot']:.0f} | stage-path tail "
              f"{s['tail_stage']}/{s['n_tail']} ({100.0 * s['tail_stage'] / s['n_tail']:.0f} %), stage-path share all {s['stage_share_all']:.1f} %")

if __name__ == '__main__':
    main(sys.argv[1:])
