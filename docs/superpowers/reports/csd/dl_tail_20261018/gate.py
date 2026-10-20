# gate table: per run DL+UL goal lines + a2e split; per (cap, b n_real mode, arm) medians. usage: gate.py idx...
import sys,re,subprocess,statistics as st
from collections import defaultdict
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16kf/docs/superpowers/reports/csd/dl_tail_20261018')
from a2p import rows, med
import goal_metrics as gm
G='/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx/goal_metrics.py'
agg=defaultdict(lambda: defaultdict(list))
for idx in sys.argv[1:]:
  for line in open(idx):
    p=line.split()
    if len(p)!=4: continue
    cap,arm,i,d=p
    o=subprocess.run(['python3',G,d+'/run.log'],capture_output=True,text=True).stdout.splitlines()
    kv=dict(re.findall(r'(\w+)=([\d.]+)',[l for l in o if ' DL ' in l][0]))
    ul=dict(re.findall(r'(\w+)=([\d.]+)',[l for l in o if ' UL ' in l][0]))
    R=rows(d+'/run.log'); p99=gm.pct([r['tot'] for r in R],.99); T=[r for r in R if r['tot']>=p99]
    nr=float(kv['n_real']); mode='' if cap!='b' else ('low' if nr<2500 else 'mid' if nr<4000 else 'high')
    a=agg[(cap,mode,arm)]
    for k in ('tot_p50','tot_p99','tot_p999','crc_q','n_real','drops_pct'): a[k].append(float(kv[k]))
    a['ul_p99'].append(float(ul['tot_p99'])); a['ul_drops'].append(float(ul['drops_pct']))
    for k in ('a2e','x_read','occ2pass','pass2p1','acc2enq','dec'): a['T'+k].append(med([r.get(k) for r in T])); a['A'+k].append(med([r.get(k) for r in R]))
    print(f"{cap} {arm} {i} {mode:4} DL p50={kv['tot_p50']} p99={kv['tot_p99']} p999={kv['tot_p999']} crc_q={kv['crc_q']} n_real={kv['n_real']} drops={kv['drops_pct']} | UL p99={ul['tot_p99']} drops={ul['drops_pct']} | tail a2e={med([r['a2e'] for r in T])} occ2pass={med([r.get('occ2pass') for r in T])} x_read={med([r.get('x_read') for r in T])} | all a2e={med([r['a2e'] for r in R])} x_read={med([r.get('x_read') for r in R])} | {d.split('/')[-1]}",flush=True)
print()
for (cap,mode,arm),a in sorted(agg.items()):
    f=lambda k: st.median(a[k])
    print(f"{cap:3} {mode:4} {arm:4} n={len(a['tot_p99'])} DL p99 med {f('tot_p99'):.0f} {sorted(int(x) for x in a['tot_p99'])} p50 {f('tot_p50'):.0f} p99.9 {f('tot_p999'):.0f} crc_q {f('crc_q'):.1f} [{min(a['crc_q']):.1f}-{max(a['crc_q']):.1f}] n_real {sorted(int(x) for x in a['n_real'])} drops max {max(a['drops_pct'])} | UL p99 med {f('ul_p99'):.0f} drops max {max(a['ul_drops'])} | tail a2e {f('Ta2e'):.0f} occ2pass {f('Tocc2pass'):.0f} x_read {f('Tx_read'):.0f} | all a2e {f('Aa2e'):.0f} x_read {f('Ax_read'):.0f} pass {f('Apass2p1'):.0f}")
