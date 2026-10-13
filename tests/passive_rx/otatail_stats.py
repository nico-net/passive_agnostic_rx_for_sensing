#!/usr/bin/env python3
"""OTATAIL: one line of decode-tail / discovery-load figures per run.log (replay or OTA).
usage: otatail_stats.py run.log [run.log ...]"""
import re, sys
ansi = re.compile(r'\x1b\[[0-9;]*m')
def last(lines, pat):
    m = None
    for l in lines:
        x = re.search(pat, l)
        if x: m = x
    return m
for path in sys.argv[1:]:
    L = [ansi.sub('', l) for l in open(path, errors='replace')]
    out = {}
    m = last(L, r'PDTIMPCT us .*sum\[n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+)\]')
    if m: out.update(n=m.group(1), p50=m.group(2), p90=m.group(3), p99=m.group(4))
    m = last(L, r'SENSING: PDTIM calls=.*chest\[n=\d+ mean=([\d.]+)us.*max=([\d.]+)us.*demod\[n=\d+ mean=([\d.]+)us.*max=([\d.]+)us')
    if m: out.update(chest_mean=m.group(1), chest_max=m.group(2), demod_mean=m.group(3), demod_max=m.group(4))
    sums = [sum(float(x) for x in re.findall(r'mean=([\d.]+)us', l)[:5]) for l in L if 'SENSING: PDTIM calls=' in l]
    if sums: out['stage_mean_sum'] = sums[-1]
    out['slow20'] = sum(1 for l in L if 'PDTIMSLOW' in l)
    out['slow20_real'] = sum(1 for l in L if 'PDTIMSLOW' in l and 'class=real' in l)
    m = last(L, r'"pdschq_queued":(\d+),"pdschq_decoded":(\d+),"pdschq_crc_ok":(\d+),"pdschq_drop_full":\d+,"pdschq_drop_stale":(\d+)')
    if m:
        q, d, ok, st = map(int, m.groups())
        out.update(queued=q, decoded=d, crc_ok=ok, stale_pct=f'{100.0*st/max(1,q):.1f}')
    m = last(L, r'"csirs_confirmed":(\d+)')
    if m: out['csirs_conf'] = m.group(1)
    m = last(L, r'"gpu_budget_used_pct":([\d.]+),"discovery_yields":(\d+)')
    if m: out.update(bud_pct=m.group(1))
    passes = [l for l in L if 'PDCCH_NID_GPU pass ' in l]
    out['nid_passes'] = len(passes)
    out['nid_kmax50'] = sum(1 for l in passes if float(re.search(r'kernels max ([\d.]+)', l).group(1)) >= 50)
    out['nid_slice_over5'] = sum(1 for l in L if 'GPU_DISCOVERY_SLICE' in l)
    for i, l in enumerate(L):
        if 'PDCCH_NID_GPU pass 1:' in l:
            out['first_pass_line'] = i; break
    m = last(L, r'GPU_GREEN_VERIFY (.*)')
    if m: out['green'] = m.group(1).split(' rt_mask')[0]
    hb = [l for l in L if 'GPU_HEARTBEAT t=' in l]
    if hb:
        dev = max(float(re.search(r'dev_max_ms=[\d.]+/([\d.]+)', l).group(1)) for l in hb)
        host = max(float(re.search(r'host_max_ms=[\d.]+/([\d.]+)', l).group(1)) for l in hb)
        o2 = sum(int(re.search(r'dev_over2=(\d+)', l).group(1)) + int(re.search(r'host_over2=(\d+)', l).group(1)) for l in hb)
        out.update(hb_dev_max=dev, hb_host_max=host, hb_over2=o2, hb_stalls=sum(1 for l in L if 'GPU_HEARTBEAT STALL' in l))
    for k in ('GPU_EXACT_CHEST', 'GPU_EXACT_DEMOD', 'GPU_EXACT_FEP'):
        m = last(L, k + r' calls=\d+ check_ok=\d+ check_bad=(\d+)')
        if m: out[k.lower().replace('gpu_exact_', 'bad_')] = m.group(1)
    print(path.split('/')[-2] if '/' in path else path, ' '.join(f'{k}={v}' for k, v in out.items()))
