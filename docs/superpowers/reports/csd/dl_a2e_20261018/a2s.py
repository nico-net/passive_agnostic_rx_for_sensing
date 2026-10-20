# a2e decomposition of tail grants: usage a2s.py idx [arms]. Per run: goal_metrics DL line + a2s medians for grants >= p99 and all grants.
import sys,re,subprocess,statistics as st
from collections import Counter
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx')
import goal_metrics as gm
G='/home/nicola/NICOLA/wt/rr-csd-int16kf/tests/passive_rx/goal_metrics.py'
A=('arr2occ','occ2pass','pass2p1','p12p2','p22enq','kind','bank')
A9=('arr2occ','occ2pass','pass2p1','p12p2','p22cand','cand2acc','acc2enq','kind','bank')
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
        if 'a2s' in kv:
            v=[int(x) for x in kv['a2s'].split(',')]; r.update(zip(A9 if len(v)==9 else A,v))
        if 'a2x' in kv: r.update(zip(('x_solv','x_read','x_enq'),(int(x) for x in kv['a2x'].split(','))))
        R.append(r)
    return R
def med(v): v=[x for x in v if x is not None and x>=0]; return st.median(v) if v else -1
if __name__=='__main__':
  for line in open(sys.argv[1]):
      p=line.split()
      if len(p)<4: continue
      cap,arm,i,d=p
      if len(sys.argv)>2 and arm not in sys.argv[2].split(','): continue
      o=subprocess.run(['python3',G,d+'/run.log'],capture_output=True,text=True).stdout.splitlines()
      dl=[l for l in o if 'tot_p99' in l]; print(cap,arm,i,dl[0] if dl else o[:1])
      R=rows(d+'/run.log'); p99=gm.pct([r['tot'] for r in R],.99)
      for nm,S in (('tail>=p99',[r for r in R if r['tot']>=p99]),('all',R)):
          print(f"   {nm:9} n={len(S)} "+' '.join(f"{k}={med([r.get(k) for r in S])}" for k in ('a2e','dec')+A9[:7]+('x_solv','x_read','x_enq')),
                'kind/bank',Counter((r.get('kind'),r.get('bank')) for r in S).most_common(5))
