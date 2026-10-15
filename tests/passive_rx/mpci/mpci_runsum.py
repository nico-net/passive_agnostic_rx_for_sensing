#!/usr/bin/env python3
"""Summaries of receiver logs for the MPCI validation (fanout_replay.sh arms and plain replays/OTA run logs).
   mpci_runsum.py LOG...         one block per log
   mpci_runsum.py --arms DIR     every arm directory under DIR (ue_*.log + mem.tsv + gpu.tsv of fanout_replay.sh)
Fields: sync PCI, MIB/SIB1 counts, CORESET bank adds, CSI-RS confirmations, last PDSCHQ line, EVMGRANT CRC, GPU_EXACT check_bad,
PDTIMPCT sum percentiles, SHMIQ_END counters; per arm: min MemAvailable, per process CPU s / peak RSS / anon, GPU MiB."""
import os, re, sys


def summ(path):
    r = dict(pci=None, mib=0, sib1=0, bank_add=0, csirs_conf=0, q=None, evm=0, evm_ok=0, rnti_ok=set(), check_bad={}, pdtim=None,
             shm_end=None, overrun_lines=0, discont_lines=0, lost=0, eof=0)
    for raw in open(path, 'rb'):
        l = raw.decode('utf-8', 'replace')
        if 'Initial sync successful, PCI:' in l and r['pci'] is None:
            r['pci'] = int(re.search(r'PCI: (\d+)', l).group(1))
        elif 'MIB fields' in l: r['mib'] += 1
        elif 'SIB1 CELL' in l: r['sib1'] += 1
        elif 'multi-CORESET bank add' in l: r['bank_add'] += 1
        elif 'CSIRS_BLIND CONFIRMED' in l: r['csirs_conf'] += 1
        elif 'PDSCHQ queued=' in l:
            m = re.search(r'queued=(\d+) decoded=(\d+) crc_ok=(\d+).*?dropped\[full=(\d+) stale=(\d+)\]', l)
            if m: r['q'] = tuple(map(int, m.groups()))
        elif 'EVMGRANT rnti=' in l:
            m = re.search(r'rnti=0x([0-9a-f]+) \S+ crc=(\d)', l)
            if m:
                r['evm'] += 1
                if m.group(2) == '1':
                    r['evm_ok'] += 1
                    if m.group(1) != 'ffff': r['rnti_ok'].add(m.group(1))
        elif 'check_bad=' in l and 'GPU_EXACT_' in l:
            m = re.search(r'(GPU_EXACT_\w+) calls=(\d+).*?check_bad=(\d+)', l)
            if m: r['check_bad'][m.group(1)] = (int(m.group(2)), int(m.group(3)))
        elif 'PDTIMPCT' in l:
            m = re.search(r'sum\[n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+)\]', l)
            if m: r['pdtim'] = tuple(map(int, m.groups()))
        elif 'SHMIQ_END' in l: r['shm_end'] = l.strip()[l.find('SHMIQ_END'):][:200]
        elif 'SHMIQ_OVERRUN' in l: r['overrun_lines'] += 1
        elif 'SHMIQ_DISCONT' in l: r['discont_lines'] += 1
        elif 'SHMIQ_PRODUCER_LOST' in l: r['lost'] += 1
        elif 'RAW_IQ_EOF' in l: r['eof'] += 1
    r['rnti_ok'] = len(r['rnti_ok'])
    return r


def line(name, r):
    q = r['q'] or (0, 0, 0, 0, 0)
    cb = max([v[1] for v in r['check_bad'].values()] or [0])
    end = re.search(r'overruns=(\d+) dropped_in=(\d+) discont=(\d+)', r['shm_end'] or '')
    return (f"  {name}: pci={r['pci']} sib1={r['sib1']} bank={r['bank_add']} csirs={r['csirs_conf']} PDSCHQ ok/dec={q[2]}/{q[1]} "
            f"({100 * q[2] / q[1] if q[1] else 0:.1f}%) stale={q[4]} EVM ok/n={r['evm_ok']}/{r['evm']} rnti_ok={r['rnti_ok']} "
            f"check_bad_max={cb} pdtim_sum p50/p90/p99={r['pdtim'][1:] if r['pdtim'] else None} "
            f"shm(ovr,drop_in,disc)={end.groups() if end else '-'} eof={r['eof']}")


def arms(root):
    for arm in sorted(os.listdir(root)):
        d = os.path.join(root, arm)
        if not os.path.isdir(d) or not os.path.exists(os.path.join(d, 'mem.tsv')): continue
        cpu, hwm, anon, mina = {}, {}, {}, 999
        for l in open(os.path.join(d, 'mem.tsv')):
            m = re.search(r'memavail_gb=(\d+)', l)
            if m: mina = min(mina, int(m.group(1)))
            for p, rss, h, a, c in re.findall(r' (\d+):(\d+)/(\d+)/(\d+)MB:cpu=([\d.]+)s', l):
                cpu[p] = float(c); hwm[p] = max(hwm.get(p, 0), int(h)); anon[p] = max(anon.get(p, 0), int(a))
        gmax = {}
        if os.path.exists(os.path.join(d, 'gpu.tsv')):
            for l in open(os.path.join(d, 'gpu.tsv')):
                for p, mib in re.findall(r'(\d+), (\d+) MiB', l): gmax[p] = max(gmax.get(p, 0), int(mib))
        print(f"== {arm}  min MemAvailable {mina} GB; per process cpu s / peak RSS / peak anon MB: "
              + ', '.join(f"{p}:{cpu[p]:.1f}s/{hwm[p]}/{anon[p]}" for p in cpu) + f"; GPU MiB {sorted(gmax.values())}")
        for log in sorted(f for f in os.listdir(d) if f.startswith('ue_') and f.endswith('.log')):
            print(line(log, summ(os.path.join(d, log))))
        cl = os.path.join(d, 'chan.log')
        if os.path.exists(cl):
            st = [l.strip() for l in open(cl) if 'SHMIQ_CHAN_STAT' in l or 'SHMIQ_CHAN_END' in l]
            print('  chan:', st[-1] if st else '-')


if __name__ == '__main__':
    if sys.argv[1:2] == ['--arms']:
        arms(sys.argv[2])
    else:
        for p in sys.argv[1:]:
            print(p); print(line('log', summ(p)))
