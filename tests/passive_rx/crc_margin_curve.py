#!/usr/bin/env python3
"""CRC-OK % per 1-dB bin of (true_snr_db(min used-layer SNR, Qm) - required SNR of the MCS); grants on the estimator floor go to the 'floor' row, DL (EVMGRANT probe=0, rv=0) and UL (UL_PUSCH_CRC).
Calibrates goal_metrics.IMPL_LOSS_DB: on a correct receiver CRC-OK should reach 80 % near margin 0..+2 dB.
usage: crc_margin_curve.py run.log..."""
import math, sys
from collections import defaultdict
import goal_metrics as gm

def cross80(rows):
    for b in sorted(k for k in rows if k != 'floor'):
        n, ok = rows[b]
        if n >= 10 and 100.0 * ok / n >= 80:
            return b
    return None

def curve(paths):
    dl, ul = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
    for p in paths:
        with open(p, errors='replace') as fh:
            lines = fh.readlines()
        for raw in lines:
            if 'EVMGRANT' not in raw and 'UL_PUSCH_CRC' not in raw:
                continue
            kv = dict(gm.KV.findall(gm.ANSI.sub('', raw)))
            try:
                if 'EVMGRANT' in raw:
                    if kv.get('probe', '0') != '0' or kv['rv'] != '0':
                        continue
                    nl = int(kv['Nl']); snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                    mcs, tbl = int(kv['mcs']), int(kv['tbl']); req = gm.required_snr_db(mcs, tbl); ok = int(kv['crc']); acc = dl
                else:
                    if kv.get('probe', '0') != '0':
                        continue
                    snr = float(kv['snr_db']); mcs, tbl = int(kv['mcs']), int(kv.get('tbl', 0)); req = gm.required_snr_db(mcs, tbl); ok = int(kv['ok']); acc = ul
            except (KeyError, ValueError, IndexError):
                continue
            if req is None:
                continue
            ts = gm.true_snr_db(snr, gm.TABLES.get(tbl, gm.T64)[mcs][0])
            b = 'floor' if ts is None else max(-20, min(20, math.floor(ts - req))); acc[b][0] += 1; acc[b][1] += ok
    return dl, ul

if __name__ == '__main__':
    for name, rows in zip(('DL', 'UL'), curve(sys.argv[1:])):
        print(name, 'cross80_db=%s' % cross80(rows))
        for b in sorted(rows, key=lambda k: -99 if k == 'floor' else k):
            n, ok = rows[b]
            print(f"  margin {b if b == 'floor' else format(b, '+d')} dB: n={n} crc_ok={100.0 * ok / n:.1f}%")
