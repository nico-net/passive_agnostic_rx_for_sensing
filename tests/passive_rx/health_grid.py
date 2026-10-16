#!/usr/bin/env python3
"""Receiver health grid (Phase 1 Task 1): CRC per grant class (tbl, mcs, PRB bucket, Nl, nsym) against the SNR margin.
margin = goal_metrics.true_snr_db(min used-layer snr_l, Qm) - required_snr_db(mcs, tbl); floor grants, probes, rv>0 and
reserved MCS are excluded; async recoveries (RECOVLATE) are credited. A class PASSes when >= min_n grants have margin >= 2 dB
and >= 80 % of them decode; fewer grants = UNVERIFIED. cross80_db = smallest margin from which the class decodes >= 80 %.
usage: health_grid.py [--min-n 50] run.log..."""
import re, sys
from collections import defaultdict
import goal_metrics as gm

FS = re.compile(r'EVMGRANT rnti=(\S+) (\d+\.\d+) ')
RB_EDGES = ((8, '1-8'), (24, '9-24'), (51, '25-51'), (106, '52-106'), (162, '107-162'), (217, '163-217'), (273, '218-273'))

def rb_bucket(n):
    return next(name for top, name in RB_EDGES if n <= top)

def grid(paths):
    out = defaultdict(list)
    for p in paths:
        with open(p, errors='replace') as fh:
            lines = fh.readlines()
        rec = {(m.group(1), m.group(2)) for m in (gm.RECOV.search(r) for r in lines if 'RECOVLATE' in r) if m}  # RECOV may gain more groups (Task 3)
        for raw in lines:
            if 'EVMGRANT' not in raw:
                continue
            line = gm.ANSI.sub('', raw); kv = dict(gm.KV.findall(line)); m = FS.search(line)
            try:
                if not m or kv.get('probe', '0') != '0' or kv['rv'] != '0':
                    continue
                tbl, mcs, nl = int(kv['tbl']), int(kv['mcs']), int(kv['Nl'])
                req = gm.required_snr_db(mcs, tbl)
                if req is None:
                    continue
                ts = gm.true_snr_db(min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)]), gm.TABLES.get(tbl, gm.T64)[mcs][0])
                if ts is None:
                    continue
                ok = 1 if kv['crc'] == '1' or (m.group(1), m.group(2)) in rec else 0
                key = (tbl, mcs, rb_bucket(int(kv['rb'].split('+')[1])), nl, int(kv['nsym']))
            except (KeyError, ValueError, IndexError, StopIteration):
                continue
            out[key].append((ts - req, ok))
    return dict(out)

def verdict(samples, min_n=50):
    q = [ok for mg, ok in samples if mg >= 2]
    crc_q = 100.0 * sum(q) / len(q) if q else None
    cross = None
    for m in range(0, 13):
        s = [ok for mg, ok in samples if mg >= m]
        if len(s) >= min(min_n, 20) and 100.0 * sum(s) / len(s) >= 80:
            cross = m
            break
    status = 'UNVERIFIED' if len(q) < min_n else ('PASS' if crc_q >= 80 else 'FAIL')
    return {'n_q': len(q), 'crc_q': crc_q, 'cross80_db': cross, 'status': status}

def coverage(classes, min_n=50):
    seen = {(k[0], k[1]) for k, s in classes.items() if verdict(s, min_n)['n_q'] >= min_n}
    return [(t, m) for t, tab in sorted(gm.TABLES.items()) for m in range(len(tab)) if (t, m) not in seen]

if __name__ == '__main__':
    a = sys.argv[1:]; min_n = 50
    if '--min-n' in a:
        i = a.index('--min-n'); min_n = int(a[i + 1]); del a[i:i + 2]
    g = grid(a)
    for k in sorted(g):
        v = verdict(g[k], min_n)
        crc = '-' if v['crc_q'] is None else f"{v['crc_q']:.1f}"
        print(f"tbl={k[0]} mcs={k[1]} rb={k[2]} Nl={k[3]} nsym={k[4]} n={len(g[k])} n_q={v['n_q']} crc_q={crc} cross80={v['cross80_db']} {v['status']}")
    print('NOT COVERED (tbl, mcs):', ' '.join(f"{t}/{m}" for t, m in coverage(g, min_n)))
