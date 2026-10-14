#!/usr/bin/env python3
"""ULQ: per-run UL load and UL decode cost from a receiver run.log (OTA or replay).

usage: ulq_stats.py [--json] run.log [run.log ...]
Reports, per log: real UL grants/s (ULDCI, unique rnti+slot, mu=1 2000 slots/s), rv!=0 share,
PUSCHQ / PUSCHQ2 final counters, UL_PIPELINE core_fraction, full-decode cpu_us by PRB bucket and MCS
(UL_PUSCH_CRC), probe cost (UL_PUSCH_PROBE, ULLAYOUT probes), UCI trials per decode, UL snr_db,
UL_HARQ combining counters and the DL PDSCH p50/p99 (PDSCHQ real_us line when present).
"""
import json, re, sys
from collections import Counter, defaultdict

KV = re.compile(r'(\w+)=([^\s\]]+)')


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))]


def last(lines, key):
    for l in reversed(lines):
        if key in l:
            return l
    return ''


def parse(path):
    uldci, rv = set(), Counter()
    crc_cpu = defaultdict(list)
    mcs_cpu = defaultdict(list)
    cpu_all, snr, crc_ok, crc_n = [], [], 0, 0
    probe_cpu = []
    tail = []
    slots = []
    with open(path, errors='replace') as f:
        for l in f:
            if 'SENSING: ULDCI ' in l:
                d = dict(KV.findall(l))
                k = (d.get('rnti'), d.get('abs'))
                if k not in uldci:
                    uldci.add(k)
                    rv[d.get('rv', '?')] += 1
                    try:
                        slots.append(int(d['abs']))
                    except (KeyError, ValueError):
                        pass
            elif 'SENSING: UL_PUSCH_CRC ' in l:
                d = dict(KV.findall(l))
                try:
                    nrb = int(d['prb'].split('+')[1]); cpu = int(d['cpu_us'])
                except (KeyError, ValueError, IndexError):
                    continue
                b = '1-12' if nrb <= 12 else '13-48' if nrb <= 48 else '49-106' if nrb <= 106 else '107+'
                crc_cpu[b].append(cpu); mcs_cpu[int(d.get('mcs', -1)) // 5 * 5].append(cpu); cpu_all.append(cpu)
                crc_n += 1; crc_ok += d.get('ok') == '1'
                s = float(d.get('snr_db', 'nan'))
                if s == s and s != 0.0:
                    snr.append(s)
            elif 'SENSING: UL_PUSCH_PROBE ' in l:
                d = dict(KV.findall(l))
                if 'cpu_us' in d:
                    probe_cpu.append(int(d['cpu_us']))
            elif any(k in l for k in ('PUSCHQ ', 'PUSCHQ2 ', 'UL_PIPELINE', 'pusch_passive[', 'ULLAYOUT STATS',
                                      'UL_CHAIN_DROP', 'UL_PUSCH_SOLVER', 'pusch_book[', 'UL_HARQ ', 'PDSCHQ ',
                                      'PDSCH_LAT', 'PDTIM ')):
                tail.append(l.strip())
    dur = (max(slots) - min(slots)) / 2000.0 if len(slots) > 1 else 0.0
    out = {'log': path, 'span_s': round(dur, 1), 'uldci_unique': len(uldci),
           'grants_per_s': round(len(uldci) / dur, 1) if dur else None,
           'rv_nonzero_pct': round(100.0 * (len(uldci) - rv.get('0', 0)) / len(uldci), 1) if uldci else None,
           'full_decodes': crc_n, 'full_crc_ok': crc_ok,
           'cpu_us_p50': pct(cpu_all, 50), 'cpu_us_p90': pct(cpu_all, 90), 'cpu_us_p99': pct(cpu_all, 99),
           'cpu_us_mean': round(sum(cpu_all) / len(cpu_all)) if cpu_all else None,
           'cpu_by_prb': {k: (len(v), pct(v, 50), pct(v, 90), round(sum(v) / len(v))) for k, v in sorted(crc_cpu.items())},
           'cpu_by_mcs5': {k: (len(v), pct(v, 50), pct(v, 90)) for k, v in sorted(mcs_cpu.items())},
           'probe_n': len(probe_cpu), 'probe_cpu_p50': pct(probe_cpu, 50), 'probe_cpu_p90': pct(probe_cpu, 90),
           'snr_n': len(snr), 'snr_p10': pct(snr, 10), 'snr_p50': pct(snr, 50), 'snr_p90': pct(snr, 90)}
    for key in ('PUSCHQ ', 'PUSCHQ2 ', 'UL_PIPELINE', 'pusch_passive[', 'ULLAYOUT STATS', 'UL_CHAIN_DROP',
                'UL_PUSCH_SOLVER', 'pusch_book[', 'UL_HARQ ', 'PDSCHQ '):
        l = last(tail, key)
        if l:
            out['last_' + key.strip(' [')] = l.split('SENSING: ', 1)[-1][:400]
    return out


if __name__ == '__main__':
    js = '--json' in sys.argv
    for p in [a for a in sys.argv[1:] if a != '--json']:
        r = parse(p)
        if js:
            print(json.dumps(r))
        else:
            for k, v in r.items():
                print(f'{k}: {v}')
            print()
