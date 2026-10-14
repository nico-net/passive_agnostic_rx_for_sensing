#!/usr/bin/env python3
"""ULLDPC: one summary line per replay/OTA run.log for the UL latency, stall, CRC and BWP-lock A/B.
usage: ulldpc_stats.py [--json] run.log [...]
Fields: UL per-grant cpu_us (UL_PUSCH_CRC, full decodes) p50/p99/max + n, UL CRC-OK, real-grant loss (PUSCHQ2 real full / stale),
GPU FEP / LDPC census (calls, late > 2 ms, deadline, check_bad), ULSTALL breakdown (ISAC_UL_STALL_DIAG), UCI trials (cut),
BWP lock (target, votes, time) and sign-test table, UL snr_db per RNTI, DL PDSCH real p99/max (PDTIMCLS) and gpu_budget_used_pct."""
import json, re, sys

ansi = re.compile(r'\x1b\[[0-9;]*m')
KV = re.compile(r'(\w+)=([^\s\]]+)')


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))]


def one(path):
    L = [ansi.sub('', l) for l in open(path, errors='replace')]
    o = {'log': path}
    cpu, ok, snr = [], 0, {}
    for l in L:
        if 'UL_PUSCH_CRC' in l:
            kv = dict(KV.findall(l))
            if 'cpu_us' in kv:
                cpu.append(int(kv['cpu_us']))
                ok += kv.get('ok') == '1'
                try:
                    snr.setdefault(kv['rnti'], []).append(float(kv['snr_db']))
                except (KeyError, ValueError):
                    pass
    o['ul_n'] = len(cpu)
    o['ul_cpu_us'] = [pct(cpu, 50), pct(cpu, 99), max(cpu) if cpu else None]
    o['ul_crc_ok'] = ok
    sn = sorted(x for v in snr.values() for x in v)
    o['ul_snr_db_p50'] = pct(sn, 50)
    o['ul_snr_db_max'] = sn[-1] if sn else None
    # per-RNTI median SNR of the best 5 RNTIs
    per = sorted(((pct(v, 50), len(v), r) for r, v in snr.items()), key=lambda t: -t[1])[:5]
    o['ul_snr_per_rnti_top5'] = [(r, n, m) for m, n, r in per]
    for key, pat in (('PUSCHQ2', 'PUSCHQ2 '), ('UL_GPU_FEP', 'UL_GPU_FEP on='), ('UL_GPU_LDPC', 'UL_GPU_LDPC on='), ('pusch_passive', 'pusch_passive[try=')):
        m = [l for l in L if pat in l]
        o[key] = m[-1].split('SENSING: ')[-1].strip()[:420] if m else None
    st = [l for l in L if 'ULSTALL' in l]
    o['ulstall_n'] = len(st)
    if st:
        agg = {}
        for l in st:
            kv = dict(KV.findall(l))
            who = l.split('ULSTALL ')[1].split()[0]
            a = agg.setdefault(who, {'n': 0, 'dt': [], 'launch': [], 'gpu': [], 'wake': [], 'iv': 0, 'cpu': []})
            a['n'] += 1
            a['dt'].append(int(kv['dt_us']))
            a['launch'].append(int(kv['launch_us']))
            a['gpu'].append(float(kv['gpu_us']))
            a['wake'].append(float(kv['wake_us']))
            a['cpu'].append(int(kv['cpu_us']))
            a['iv'] += int(kv['nivcsw']) > 0
        o['ulstall'] = {w: dict(n=a['n'], dt_p50=pct(a['dt'], 50), dt_max=max(a['dt']), launch_p50=pct(a['launch'], 50), launch_max=max(a['launch']),
                                gpu_p50=pct(a['gpu'], 50), gpu_max=max(a['gpu']), wake_p50=pct(a['wake'], 50), wake_max=max(a['wake']),
                                cpu_p50=pct(a['cpu'], 50), preempted=a['iv']) for w, a in agg.items()}
    lk = [l for l in L if 'ULLAYOUT BWP_LOCK' in l]
    if lk:
        kv = dict(KV.findall(lk[0]))
        o['bwp_lock'] = dict(bwp=kv.get('bwp'), votes=kv.get('votes'), runner=kv.get('runner'), t_conv_ms=kv.get('t_conv_ms'))
    stt = [l for l in L if 'ULLAYOUT STATS' in l]
    if stt:
        s = stt[-1]
        o['bwp_final'] = s.split('SENSING: ULLAYOUT STATS')[-1].split('tally[')[0].strip()[:90]
        m = re.search(r'tally\[(.*?)\]', s)
        o['tally'] = m.group(1).strip() if m else None
        m = re.search(r'signt\[(.*?)\]', s)
        o['signt'] = m.group(1).strip() if m else None
    m = [l for l in L if 'PDTIMCLS us real[' in l]
    if m:
        g = re.search(r'real\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+)', m[-1])
        if g:
            o['dl_real'] = dict(n=int(g[1]), p50=int(g[3]), p99=int(g[5]), max_ms=round(float(g[6]) / 1000, 1))
    b = [float(x) for l in L for x in re.findall(r'"gpu_budget_used_pct":([\d.]+)', l)]
    o['gpu_budget_used_pct_max'] = max(b) if b else None
    u = [l for l in L if 'uci[trials=' in l]
    if u:
        m = re.search(r'uci\[trials=(\d+).*?\]', u[-1])
        o['uci'] = m.group(0) if m else None
    return o


if __name__ == '__main__':
    js = '--json' in sys.argv
    for p in [a for a in sys.argv[1:] if not a.startswith('--')]:
        r = one(p)
        if js:
            print(json.dumps(r))
        else:
            print('==', p)
            for k, v in r.items():
                if k != 'log' and v is not None:
                    print('  %-22s %s' % (k, v))
