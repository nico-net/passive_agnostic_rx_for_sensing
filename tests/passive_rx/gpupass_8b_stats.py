#!/usr/bin/env python3
"""Task 8b aggregator for gpupass_ab.sh runs: python3 -I gpupass_8b_stats.py <capset b|tim> ARM [ARM ...]  (reads replays/replay_gp_<cap>_<ARM>_<i>_*/run.log; one capture at a time, never pooled).
Per arm: median over runs of whole-call / first-body p50/p99 (<=96 and >96 PRB), CRC-OK per run (EVMGRANT crc=1 + RECOVLATE) with mean/sd, last GPUPASS stamp histograms, bad counters."""
import glob, os, re, statistics as st, sys
R = re.compile(r'EVMGRANT rnti=0x\w+ \S+ crc=(\d) .*? rb=\d+\+(\d+) .*? probe=0 .*? e2e=(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),')
pct = lambda v, p: sorted(v)[min(len(v) - 1, int(p / 100 * len(v)))] if v else None
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'replays')
cap = sys.argv[1]
SINCE = float(os.environ.get('SINCE', 0))  # epoch: ignore older replay dirs (Task 8 logs share the names)
for arm in sys.argv[2:]:
    runs = []
    for d in sorted((x for x in glob.glob(f'{root}/replay_gp_{cap}_{arm}_[0-9]*_*/run.log') if os.path.getmtime(x) > SINCE), key=lambda p: int(re.search(rf'_{arm}_(\d+)_', p).group(1))):
        whole, w96, wbig, f96, fbig, ok, n, late, bad, stamps, gate_retry, launches = [], [], [], [], [], 0, 0, 0, 0, None, None, None
        for l in open(d, errors='replace'):
            m = R.search(l)
            if m:
                crc, rb, dec, first = int(m.group(1)), int(m.group(2)), int(m.group(6)), int(m.group(7))
                n += 1; ok += crc
                whole.append(dec); (w96 if rb <= 96 else wbig).append(dec); (f96 if rb <= 96 else fbig).append(first)
            elif 'RECOVLATE' in l:
                late += 1
            elif 'GPUPASS MISMATCH' in l or 'GPUPASS DISABLED' in l or 'GPUPASS_UL DISABLED' in l:
                bad += 1
            elif 'SENSING: GPUPASS launches' in l:
                mm = re.search(r'prep_us\[p50<=(\d+) p99<=(\d+)\] run_us\[p50<=(\d+) p99<=(\d+)\] post_us\[p50<=(\d+) p99<=(\d+)\]', l)
                if mm: stamps = mm.groups()
                mm = re.search(r'retry=(\d+)\]', l); gate_retry = mm.group(1) if mm else gate_retry
                mm = re.search(r'launches=(\d+)', l); launches = mm.group(1)
            elif 'GPU_EXACT_FEP calls' in l:
                mm = re.search(r'check_bad=(\d+)', l); fepbad = mm.group(1)
            elif 'GPU_EXACT_CHEST calls' in l or 'GPU_EXACT_DEMOD calls' in l:
                mm = re.search(r'check_bad=(\d+)', l); bad += int(mm.group(1))
        tot = ok + late
        runs.append(dict(gt5=100.0 * sum(x > 5000 for x in whole) / len(whole) if whole else 0, f=d, n=n, ok=tot, rate=100.0 * tot / n if n else 0, whole=(pct(whole, 50), pct(whole, 99)), f96=(pct(f96, 50), pct(f96, 99)), fbig=(pct(fbig, 50), pct(fbig, 99)), stamps=stamps, gr=gate_retry, la=launches, bad=bad, fepbad=locals().get('fepbad')))
    if not runs: print(arm, 'no runs'); continue
    med = lambda k, i: st.median([r[k][i] for r in runs if r[k][i] is not None])
    rates = [r['rate'] for r in runs]
    print(f'{cap} {arm}: runs={len(runs)} whole p50/p99 {med("whole",0):.0f}/{med("whole",1):.0f}  first<=96 p50/p99 {med("f96",0):.0f}/{med("f96",1):.0f}  first>96 p50/p99 {med("fbig",0):.0f}/{med("fbig",1):.0f}  CRC-OK mean {st.mean(rates):.2f} sd {st.pstdev(rates) if len(rates)<2 else st.stdev(rates):.2f}')
    print('   per-run CRC-OK: ' + ', '.join(f'{r["n"]}/{r["ok"]}={r["rate"]:.2f}' for r in runs))
    print('   whole-call >5 ms %, per run: ' + ', '.join(f'{r["gt5"]:.2f}' for r in runs))
    print('   bad(mismatch/disabled/check_bad)=' + ','.join(str(r['bad']) for r in runs) + '  fep_check_bad=' + ','.join(str(r['fepbad']) for r in runs) + '  launches=' + ','.join(str(r['la']) for r in runs) + '  gate_retry=' + ','.join(str(r['gr']) for r in runs))
    sts = [r['stamps'] for r in runs if r['stamps']]
    if sts: print('   stamps (median of runs; prep/run/post p50|p99 us): ' + ' / '.join(f'{st.median(int(s[i]) for s in sts):.0f}|{st.median(int(s[i+1]) for s in sts):.0f}' for i in (0, 2, 4)))
