# per run: DL goal line + medians of a2s/a2x/a2p for tail (tot>=p99) and all grants. usage: a2p.py idx
import sys,re,subprocess
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16kf/docs/superpowers/reports/csd/dl_a2e_20261018')
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx')
import goal_metrics as gm
from a2s import A9, med
from collections import Counter
G='/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx/goal_metrics.py'
P=('fep','demap','dlsw','ulsw','prep','p1dec')
def rows(p):
    R=[]
    for raw in open(p,errors='replace'):
        if 'EVMGRANT' not in raw: continue
        kv=dict(gm.KV.findall(gm.ANSI.sub('',raw)))
        if kv.get('probe','0')!='0' or 'e2e' not in kv: continue
        try: e=[int(x) for x in kv['e2e'].split(',')]
        except ValueError: continue
        if e[6]<0: continue
        r={'a2e':e[0],'tot':e[6],'dec':e[3]}
        if 'a2s' in kv: r.update(zip(A9,(int(x) for x in kv['a2s'].split(','))))
        if 'a2x' in kv: r.update(zip(('x_solv','x_read','x_enq'),(int(x) for x in kv['a2x'].split(','))))
        if 'a2p' in kv: r.update(zip(P,(int(x) for x in kv['a2p'].split(','))))
        R.append(r)
    return R
if __name__=='__main__':
  for line in open(sys.argv[1]):
      p=line.split()
      if len(p)<4: continue
      cap,arm,i,d=p
      o=subprocess.run(['python3',G,d+'/run.log'],capture_output=True,text=True).stdout.splitlines()
      dl=[l for l in o if 'tot_p99' in l]; print(cap,arm,i,dl[0] if dl else o[:1])
      R=rows(d+'/run.log'); p99=gm.pct([r['tot'] for r in R],.99)
      for nm,S in (('tail',[r for r in R if r['tot']>=p99]),('all',R)):
          print(f"  {nm:4} n={len(S)} "+' '.join(f"{k}={med([r.get(k) for r in S])}" for k in ('a2e','dec')+A9[:7]+('x_read',)+P),
                Counter((r.get('kind'),r.get('bank')) for r in S).most_common(4))
