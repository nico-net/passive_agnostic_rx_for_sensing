#!/usr/bin/env python3
"""G1/G2 scorecard per run (roadmap 2026-10-16): DL/UL latency over all real grants, SNR-qualified CRC, queue drops.
A grant is SNR-qualified when min per-layer SNR >= required_snr_db(mcs, tbl) + margin (default 2 dB).
required_snr_db = 10*log10(2**SE - 1) + IMPL_LOSS_DB, SE = Qm*R/1024 (TS 38.214 tables 5.1.3.1-1/2/3).
shortcut: Shannon gap + fixed 1.5 dB loss, not a calibrated BLER curve; recalibrate from Task 2's CRC-vs-margin curve.
usage: goal_metrics.py [--margin 2.0] [--json] run.log..."""
import json, math, re, sys

IMPL_LOSS_DB = 1.5
# (Qm, R*1024) per MCS index; None = reserved (retransmission only)
T64 = [(2,120),(2,157),(2,193),(2,251),(2,308),(2,379),(2,449),(2,526),(2,602),(2,679),(4,340),(4,378),(4,434),(4,490),
       (4,553),(4,616),(4,658),(6,438),(6,466),(6,517),(6,567),(6,616),(6,666),(6,719),(6,772),(6,822),(6,873),(6,910),(6,948)]
T256 = [(2,120),(2,193),(2,308),(2,449),(2,602),(4,378),(4,434),(4,490),(4,553),(4,616),(4,658),(6,466),(6,517),(6,567),
        (6,616),(6,666),(6,719),(6,772),(6,822),(6,873),(8,682.5),(8,711),(8,754),(8,797),(8,841),(8,885),(8,916.5),(8,948)]
TLOW = [(2,30),(2,40),(2,50),(2,64),(2,78),(2,99),(2,120),(2,157),(2,193),(2,251),(2,308),(2,379),(2,449),(2,526),(2,602),
        (4,340),(4,378),(4,434),(4,490),(4,553),(4,616),(6,438),(6,466),(6,517),(6,567),(6,616),(6,666),(6,719),(6,772)]
TABLES = {0: T64, 1: T256, 2: TLOW}
ANSI = re.compile(r'\x1b\[[0-9;]*m')
KV = re.compile(r'(\w+)=(\S+)')

def required_snr_db(mcs, tbl):
    t = TABLES.get(tbl, T64)
    if mcs < 0 or mcs >= len(t):
        return None
    qm, r = t[mcs]
    return 10 * math.log10(2 ** (qm * r / 1024) - 1) + IMPL_LOSS_DB

def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]

def empty():
    return {'lat': [], 'crc_all': [0, 0], 'n_q': 0, 'ok_q': 0, 'low': 0, 'retx': 0, 'drops': None, 'skipped': 0}

def classify(acc, crc, mcs, tbl, rv, snr, margin):
    req = required_snr_db(mcs, tbl) if rv == 0 else None
    if req is None:
        acc['retx'] += 1
    elif snr >= req + margin:
        acc['n_q'] += 1; acc['ok_q'] += crc
    else:
        acc['low'] += 1

def summary(acc):
    lat = acc['lat']; n = acc['crc_all'][1]
    return {'n_real': n, 'tot_p50': pct(lat, .5), 'tot_p99': pct(lat, .99), 'tot_p999': pct(lat, .999),
            'crc_all': 100.0 * acc['crc_all'][0] / n if n else None, 'n_q': acc['n_q'],
            'crc_q': 100.0 * acc['ok_q'] / acc['n_q'] if acc['n_q'] else None,
            'unqualified_low_snr': acc['low'], 'unqualified_retx': acc['retx'],
            'drops_pct': acc['drops'], 'skipped': acc['skipped']}

def parse_run(path, margin=2.0):
    dl, ul = empty(), empty()
    with open(path, errors='replace') as fh:
        lines = fh.readlines()
    for raw in lines:
        if 'SENSING:' not in raw:
            continue
        line = ANSI.sub('', raw)
        kv = dict(KV.findall(line))
        try:
            if 'EVMGRANT' in line:
                if kv.get('probe', '0') != '0':
                    continue
                crc, mcs, tbl, nl, rv = (int(kv[k]) for k in ('crc', 'mcs', 'tbl', 'Nl', 'rv'))
                tot = int(kv['e2e'].split(',')[6])
                snr = min(float(x) for x in kv['snr_l'].split(',')[:max(1, nl)])
                dl['lat'].append(tot); dl['crc_all'][0] += crc; dl['crc_all'][1] += 1
                classify(dl, crc, mcs, tbl, rv, snr, margin)
            elif 'UL_PUSCH_CRC' in line:
                if kv.get('probe', '0') != '0':
                    continue
                ok, mcs = int(kv['ok']), int(kv['mcs'])
                ul['crc_all'][0] += ok; ul['crc_all'][1] += 1
                classify(ul, ok, mcs, int(kv.get('tbl', 0)), 0, float(kv['snr_db']), margin)
            elif 'UL_E2E' in line:
                ul['lat'].append(int(kv['tot']))
            elif 'PDSCHQ queued=' in line:
                m = re.search(r'queued=(\d+).*full=(\d+) stale=(\d+).*dropped_narrow=(\d+)', line)
                q, f, s, n = map(int, m.groups())
                dl['drops'] = 100.0 * (f + s + n) / q if q else None
            elif 'PUSCHQ2' in line:
                m = re.search(r'real\[queued=(\d+) full=(\d+)\]', line)
                q, f = map(int, m.groups())
                ul['drops'] = 100.0 * f / q if q else None
        except (KeyError, ValueError, IndexError, AttributeError):
            (dl if 'EVMGRANT' in line or 'PDSCHQ' in line else ul)['skipped'] += 1
    return {'dl': summary(dl), 'ul': summary(ul)}

def verdict(s):
    ok = (s['tot_p99'] is not None and s['tot_p99'] <= 500, s['crc_q'] is not None and s['crc_q'] >= 80,
          s['drops_pct'] is not None and s['drops_pct'] < 1)
    return 'PASS' if all(ok) else 'FAIL(' + ','.join(n for n, g in zip(('lat', 'crc', 'drops'), ok) if not g) + ')'

if __name__ == '__main__':
    args = sys.argv[1:]; margin = 2.0; as_json = False
    if '--margin' in args:
        i = args.index('--margin'); margin = float(args[i + 1]); del args[i:i + 2]
    if '--json' in args:
        args.remove('--json'); as_json = True
    for p in args:
        r = parse_run(p, margin)
        if as_json:
            print(json.dumps({'run': p, **r}))
            continue
        for d in ('dl', 'ul'):
            s = r[d]
            print(f"{p} {d.upper()} {verdict(s)} " + ' '.join(f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}" for k, v in s.items()))
