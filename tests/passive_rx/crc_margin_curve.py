#!/usr/bin/env python3
"""CRC-OK % per 1-dB bin of (min used-layer SNR - required SNR of the MCS), DL (EVMGRANT probe=0, rv=0) and UL (UL_PUSCH_CRC).
Calibrates goal_metrics.IMPL_LOSS_DB: on a correct receiver CRC-OK should reach 80 % near margin 0..+2 dB.
usage: crc_margin_curve.py run.log..."""
import math, sys
from collections import defaultdict
import goal_metrics as gm

def cross80(rows):
    for b in sorted(rows):
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
                    req = gm.required_snr_db(int(kv['mcs']), int(kv['tbl'])); ok = int(kv['crc']); acc = dl
                else:
                    if kv.get('probe', '0') != '0':
                        continue
                    snr = float(kv['snr_db']); req = gm.required_snr_db(int(kv['mcs']), int(kv.get('tbl', 0))); ok = int(kv['ok']); acc = ul
            except (KeyError, ValueError, IndexError):
                continue
            if req is None:
                continue
            b = max(-20, min(20, math.floor(snr - req))); acc[b][0] += 1; acc[b][1] += ok
    return dl, ul

if __name__ == '__main__':
    for name, rows in zip(('DL', 'UL'), curve(sys.argv[1:])):
        print(name, 'cross80_db=%s' % cross80(rows))
        for b in sorted(rows):
            n, ok = rows[b]
            print(f"  margin {b:+d} dB: n={n} crc_ok={100.0 * ok / n:.1f}%")
