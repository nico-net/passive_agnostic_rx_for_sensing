import sys,re,subprocess,statistics as st
from collections import defaultdict
sys.path.insert(0,'.'); from e2eall import rows, m
G='/home/nicola/NICOLA/wt/rr-csd-int16gpu/tests/passive_rx/goal_metrics.py'
agg=defaultdict(lambda: defaultdict(list))
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    cap,arm,i,d=p
    o=subprocess.run(['python3',G,d+'/run.log'],capture_output=True,text=True).stdout.splitlines()[0]
    kv=dict(re.findall(r'(\w+)=([\d.]+|None)',o))
    R=rows(d+'/run.log'); p99=int(kv['tot_p99']); T=[r for r in R if r['tot']>=p99]
    a=agg[(cap,arm)]
    for k in ('tot_p50','tot_p99','crc_q','n_real','drops_pct'): a[k].append(float(kv[k]) if kv[k]!='None' else float('nan'))
    for k in ('a2e','qw','pre','first'): a['T'+k].append(m([r[k] for r in T]))
    a['dec_mean'].append(m([r['dec'] for r in R]))
    log=open(d+'/run.log',errors='replace').read()
    g=re.findall(r'probe_busy=(\d+)',log); a['probe_busy'].append(int(g[-1]) if g else -1)
    print(cap,arm,i,int(kv['tot_p50']),p99,kv['crc_q'],kv['n_real'],kv['drops_pct'],flush=True)
print()
for (cap,arm),a in sorted(agg.items()):
    print(f"{cap:3} {arm:5} n={len(a['tot_p99'])} p99 med {st.median(a['tot_p99']):.0f} runs {[int(x) for x in a['tot_p99']]} p50 {st.median(a['tot_p50']):.0f} crc_q med {st.median(a['crc_q']):.1f} min {min(a['crc_q']):.1f} n_real {[int(x) for x in a['n_real']]} drops max {max(a['drops_pct'])} | dec mean {st.median(a['dec_mean']):.0f} | tail a2e {st.median(a['Ta2e']):.0f} qw {st.median(a['Tqw']):.0f} pre {st.median(a['Tpre']):.0f} first {st.median(a['Tfirst']):.0f} | probe_busy {a['probe_busy']}")
