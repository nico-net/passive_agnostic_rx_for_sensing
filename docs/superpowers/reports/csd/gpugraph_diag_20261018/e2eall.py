# per-run: p50/p99 tot, tail(>=p99) mean of a2e,qw,pre,dec,first; same for all grants; stage/pass share; by-nrb first p50
import sys,re,statistics as st
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16gpu/tests/passive_rx')
import goal_metrics as gm
E=('a2e','qw','pre','dec','first','bodies','tot','lag')
def rows(p):
    R=[]
    for raw in open(p,errors='replace'):
        if 'EVMGRANT' not in raw: continue
        kv=dict(gm.KV.findall(gm.ANSI.sub('',raw)))
        if kv.get('probe','0')!='0' or 'e2e' not in kv: continue
        try:
            e=dict(zip(E,(int(x) for x in kv['e2e'].split(','))))
            stg={s.split(':')[0]:int(s.split(':')[1].split('/')[0]) for s in kv.get('stg','').split(',') if ':' in s}
        except ValueError: continue
        if e['tot']<0: continue
        e['stage']=sum(stg.get(k,0) for k in ('fep','chest','demod','ldpc'))>200
        e['nrb']=int(kv.get('rb','0+0').split('+')[1]); e['nl']=int(kv.get('Nl',1)); e['core']=kv.get('core'); e['slot']=raw.split('EVMGRANT')[1].split()[1]
        R.append(e)
    return R
def m(v): return sum(v)/len(v) if v else float('nan')
if __name__=='__main__':
  for line in open(sys.argv[1]):
    cap,arm,i,d=line.split()
    if len(sys.argv)>2 and arm not in sys.argv[2].split(','): continue
    R=rows(d+'/run.log'); n=len(R)
    p99=gm.pct([r['tot'] for r in R],.99); p50=gm.pct([r['tot'] for r in R],.5)
    T=[r for r in R if r['tot']>=p99]
    pas=[r for r in R if not r['stage']]
    print(f"{cap:3} {arm:4} {i} n={n} p50={p50} p99={p99} | tail n={len(T)} "+' '.join(f"{k}={m([r[k] for r in T]):.0f}" for k in E[:5])+
          f" stageT={sum(r['stage'] for r in T)} nrbT_med={st.median([r['nrb'] for r in T]):.0f} | all mean "+' '.join(f"{k}={m([r[k] for r in R]):.0f}" for k in E[:5]))
