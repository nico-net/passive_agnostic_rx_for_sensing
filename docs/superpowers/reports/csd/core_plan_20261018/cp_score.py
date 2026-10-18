#!/usr/bin/env python3
"""Score a core-plan gate index (cap arm i dir per line): goal_metrics DL/UL per run, per-arm medians, and from the
thread sample (samp_<tag>_<cap>_<arm>_<i>.json) the cores the receiver may use (union of allowed masks) and the
cores with >= 5 % receiver CPU. usage: cp_score.py <idx> <tag>"""
import json, os, re, statistics, subprocess, sys
GM = '/home/nicola/NICOLA/wt/rr-csd-int16gpu/tests/passive_rx/goal_metrics.py'
idx, tag = sys.argv[1], sys.argv[2]
rows = {}

def cpus(lst):
    out = set()
    for part in lst.split(','):
        if '-' in part:
            a, b = part.split('-'); out |= set(range(int(a), int(b) + 1))
        elif part.strip().isdigit():
            out.add(int(part))
    return out

for line in open(idx):
    f = line.split()
    if len(f) != 4:
        continue
    cap, arm, i, d = f
    r = dict(run='%s_%s_%s' % (cap, arm, i))
    out = subprocess.run(['python3', GM, d + '/run.log'], capture_output=True, text=True).stdout
    for l in out.splitlines():
        kv = dict(re.findall(r'(\w+)=(\S+)', l))
        k = 'DL' if ' DL ' in l else 'UL'
        for m in ('n_real', 'tot_p50', 'tot_p99', 'crc_q', 'drops_pct'):
            r[k + '_' + m] = kv.get(m)
    sp = os.path.join(os.path.dirname(os.path.dirname(d)), 'replays', 'samp_%s_%s_%s_%s.json' % (tag, cap, arm, i))
    sp = '/home/nicola/NICOLA/wt/rr-csd-int16gpu/replays/samp_%s_%s_%s_%s.json' % (tag, cap, arm, i)
    try:
        s = json.load(open(sp))
        allowed = set()
        for t in s['threads']:
            allowed |= cpus(t['allowed'])
        load = {}
        for t in s['threads']:
            for c, sh in t['core_share'].items():
                load[int(c)] = load.get(int(c), 0) + t['pct'] * sh
        r['allowed'] = len(allowed); r['allowed_set'] = sorted(allowed)
        r['busy5'] = sorted(c for c, v in load.items() if v >= 5)
        r['nthreads'] = len(s['threads'])
    except (OSError, ValueError, KeyError):
        pass
    rows.setdefault((cap, arm), []).append(r)
    print(json.dumps(r))

def med(v):
    v = [float(x) for x in v if x not in (None, 'None')]
    return round(statistics.median(v), 1) if v else None

print('\ncap arm n | DL p99 runs | DL p99 med | DL crc_q range | DL n_real | DL drops | UL p99 runs | UL p99 med | UL drops | allowed cores')
for (cap, arm), rs in sorted(rows.items()):
    g = lambda k: [float(r[k]) if r.get(k) not in (None, 'None') else None for r in rs]
    mn = lambda v: min([x for x in v if x is not None], default=None)
    mx = lambda v: max([x for x in v if x is not None], default=None)
    print(cap, arm, len(rs), '|', g('DL_tot_p99'), '|', med(g('DL_tot_p99')), '|', mn(g('DL_crc_q')), '-', mx(g('DL_crc_q')), '|',
          g('DL_n_real'), '|', mx(g('DL_drops_pct')), '|', g('UL_tot_p99'), '|', med(g('UL_tot_p99')), '|', mx(g('UL_drops_pct')), '|',
          sorted(set(r.get('allowed', -1) for r in rs)))
