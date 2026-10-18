# A/B summary: per run DL goal line + tail(>=p99) a2e decomposition; per cap/arm medians; b split by n_real mode (<3000 low, else high).
import sys,re,subprocess,statistics as st
from collections import defaultdict
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from a2s import rows, med
import goal_metrics as gm
G='/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx/goal_metrics.py'
agg=defaultdict(lambda: defaultdict(list))
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    cap,arm,i,d=p
    o=[l for l in subprocess.run(['python3',G,d+'/run.log'],capture_output=True,text=True).stdout.splitlines() if 'tot_p99' in l][0]
    kv=dict(re.findall(r'(\w+)=([\d.]+)',o))
    R=rows(d+'/run.log'); p99=gm.pct([r['tot'] for r in R],.99); T=[r for r in R if r['tot']>=p99]
    mode='' if cap!='b' else ('low' if float(kv['n_real'])<2500 else 'mid' if float(kv['n_real'])<4000 else 'high')
    a=agg[(cap,mode,arm)]
    for k in ('tot_p50','tot_p99','tot_p999','crc_q','n_real','drops_pct'): a[k].append(float(kv[k]))
    for k in ('a2e','x_read','acc2enq','pass2p1','occ2pass','dec'): a['T'+k].append(med([r.get(k) for r in T]))
    a['Aread'].append(med([r.get('x_read') for r in R])); a['Aa2e'].append(med([r.get('a2e') for r in R]))
    ta=sorted(r['a2e'] for r in T)
    print(f"{cap} {arm} {i} {mode:4} p50={kv['tot_p50']} p99={kv['tot_p99']} p999={kv['tot_p999']} crc_q={kv['crc_q']} n_real={kv['n_real']} drops={kv['drops_pct']} | tail n={len(T)} a2e p10/50/90={ta[len(ta)//10]}/{ta[len(ta)//2]}/{ta[9*len(ta)//10]} x_read={med([r.get('x_read') for r in T])} dec={med([r['dec'] for r in T])} | all a2e={med([r['a2e'] for r in R])} x_read={med([r.get('x_read') for r in R])}",flush=True)
print()
for (cap,mode,arm),a in sorted(agg.items()):
    f=lambda k: st.median(a[k])
    print(f"{cap:3} {mode:4} {arm:3} n={len(a['tot_p99'])} p50 {f('tot_p50'):.0f} p99 {f('tot_p99'):.0f} {sorted(int(x) for x in a['tot_p99'])} p99.9 {f('tot_p999'):.0f} crc_q {f('crc_q'):.1f} [{min(a['crc_q']):.1f}-{max(a['crc_q']):.1f}] n_real {f('n_real'):.0f} drops max {max(a['drops_pct'])} | tail a2e {f('Ta2e'):.0f} x_read {f('Tx_read'):.0f} acc2enq {f('Tacc2enq'):.0f} pass2p1 {f('Tpass2p1'):.0f} occ2pass {f('Tocc2pass'):.0f} dec {f('Tdec'):.0f} | all a2e {f('Aa2e'):.0f} x_read {f('Aread'):.0f}")
