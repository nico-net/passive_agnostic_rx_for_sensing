#!/usr/bin/env python3
"""SNR-qualified DL CRC split by grant dimension, to find which grant class fails although the SNR suffices.
usage: dl_crc_split.py [--margin 2.0] run.log..."""
import sys
from collections import defaultdict
import goal_metrics as gm

def rb_bucket(n):
    return '<=24' if n <= 24 else '25-96' if n <= 96 else '97-200' if n <= 200 else '>200'

def cb_bucket(n):
    return '1' if n <= 1 else '2-7' if n <= 7 else '8-15' if n <= 15 else '16+'

def split(paths, margin=2.0):
    out = defaultdict(lambda: defaultdict(lambda: [0, 0]))
    for p in paths:
        with open(p, errors='replace') as fh:
            lines = fh.readlines()
        rec = {m.groups() for m in (gm.RECOV.search(r) for r in lines if 'RECOVLATE' in r) if m}  # async-rescued first-pass failures
        for raw in lines:
            if 'EVMGRANT' not in raw:
                continue
            kv = dict(gm.KV.findall(gm.ANSI.sub('', raw)))
            try:
                if kv.get('probe', '0') != '0' or kv['rv'] != '0':
                    continue
                nl = int(kv['Nl']); snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                req = gm.required_snr_db(int(kv['mcs']), int(kv['tbl']))
                if req is None or snr < req + margin:
                    continue
                m = gm.EVKEY.search(raw); ok = int(kv['crc']) or int(bool(m and m.groups() in rec)); rnti = int(raw.split('rnti=')[1].split()[0], 16)
                dims = {'Nl': kv['Nl'], 'tbl': kv['tbl'], 'Qm': kv['Qm'], 'nsym': kv['nsym'], 'cdm': kv.get('cdm', '?'),
                        'mcs': kv['mcs'], 'rnti_class': 'si' if rnti == 0xffff else 'ra' if rnti < 0x100 else 'c',
                        'rb_bucket': rb_bucket(int(kv['rb'].split('+')[1])), 'cb_bucket': cb_bucket(int(kv.get('cb', 1))),
                        'core': kv.get('core', '?')}
            except (KeyError, ValueError, IndexError):
                continue
            for d, v in dims.items():
                out[d][v][0] += 1; out[d][v][1] += ok
    return {d: dict(v) for d, v in out.items()}

if __name__ == '__main__':
    a = sys.argv[1:]; m = 2.0
    if '--margin' in a:
        i = a.index('--margin'); m = float(a[i + 1]); del a[i:i + 2]
    for d, vals in split(a, m).items():
        print(d, ' '.join(f"{v}:{n}/{100.0 * ok / n:.0f}%" for v, (n, ok) in sorted(vals.items())))
