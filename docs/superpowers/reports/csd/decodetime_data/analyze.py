import json,re,sys,glob,numpy as np
from scipy import stats
sys.path.insert(0,'.'); from parse import parse
rows=[]
RUNS=sys.argv[1] if len(sys.argv)>1 else 'runs.txt'; ARMS=sys.argv[2] if len(sys.argv)>2 else 'AP'; SUM=sys.argv[3] if len(sys.argv)>3 else 'summary.txt'; BASE=ARMS[0]
for l in open(RUNS):
    m=re.match(r'(\w+) (\w) (\d+) dir=(\S+) rc=(\d+) other_recv=(\d) PRE effload=([\d.\-]+) load=([\d. ]+) recv=(\d+) (\S+) POST load=([\d.]+)',l)
    if not m: continue
    r=parse(m[4]); r.update(cap=m[1],arm=m[2],rep=int(m[3]),rc=int(m[5]),effload=float(m[7]),temp=m[10],postload=float(m[11]))
    if 'c_real' not in r: print('DROPPED (no PDTIMCLS line):',l.strip()[:60]); continue
    rows.append(r)
json.dump(rows,open(RUNS.replace('.txt','.json'),'w'))
def f(r,*k):
    for x in k:
        r=r.get(x) if r else None
    return r
def ms(v): v=np.array(v,float); return (v.mean(),v.std(ddof=1) if len(v)>1 else 0)
def boot(a,b,n=20000):
    rng=np.random.default_rng(1);a=np.array(a);b=np.array(b)
    d=[rng.choice(a,len(a)).mean()-rng.choice(b,len(b)).mean() for _ in range(n)]
    return np.percentile(d,[2.5,97.5])
out=[]
P=lambda s:(print(s),out.append(s))
P('## per-run table (b = Vodafone raw_pci415_12s_b, tim = raw_pci923_tim_12s_tile3)')
P('cap arm rep | real n mean p50 p90 p99 max gt5 gt20 | retry mean p99 | probe mean p99 | sum p50/p90/p99 | q/dec/stale/full | scanq_stale | crcOK real, #RNTI | slow>20ms logged, events | effload temp finished')
for r in rows:
    c=lambda k,x: r.get('c_'+k,{}).get(x)
    P('%s %s %d | %s %s %s %s %s %s %s %s | %s %s | %s %s | %s/%s/%s | %s/%s/%s/%s | %s | %s,%d | %s,%d | %s %s %s'%(r['cap'],r['arm'],r['rep'],c('real','n'),c('real','mean'),c('real','p50'),c('real','p90'),c('real','p99'),c('real','max'),c('real','gt5'),c('real','gt20'),c('retry','mean'),c('retry','p99'),c('probe','mean'),c('probe','p99'),f(r,'p_sum','p50'),f(r,'p_sum','p90'),f(r,'p_sum','p99'),r['pdschq_queued'],r['pdschq_decoded'],r['pdschq_drop_stale'],r['pdschq_drop_full'],r['scanq_drop_stale'],r['crc_ok_real'],len(r['crc_rntis']),r['slow_logged'],len(r['stall_events']),r['effload'],r['temp'],r['finished']))
def col(cap,arm,fn): return [fn(r) for r in rows if r['cap']==cap and r['arm']==arm]
metrics={
 'real mean us':lambda r:r['c_real']['mean'],'real p50':lambda r:r['c_real']['p50'],'real p90':lambda r:r['c_real']['p90'],'real p99':lambda r:r['c_real']['p99'],
 'real gt5ms share %':lambda r:100*r['c_real']['gt5']/r['c_real']['n'],'real gt20ms count':lambda r:r['c_real']['gt20'],'real max ms':lambda r:r['c_real']['max']/1000,
 'retry mean':lambda r:r['c_retry']['mean'],'retry p99':lambda r:r['c_retry']['p99'],'probe mean':lambda r:r['c_probe']['mean'],'probe p99':lambda r:r['c_probe']['p99'],
 'real n':lambda r:r['c_real']['n'],
 'sum p50':lambda r:r['p_sum']['p50'],'sum p90':lambda r:r['p_sum']['p90'],'sum p99':lambda r:r['p_sum']['p99'],
 **{'stage %s mean'%s:(lambda s:lambda r:r['t_'+s]['mean'])(s) for s in ['fep','chest','alloc','demod','ldpc']},
 **{'stage %s sd'%s:(lambda s:lambda r:r['t_'+s]['sd'])(s) for s in ['fep','chest','alloc','demod','ldpc']},
 **{'stage %s max ms'%s:(lambda s:lambda r:r['t_'+s]['max']/1000)(s) for s in ['fep','chest','alloc','demod','ldpc']},
 **{'stage %s %s'%(s,q):(lambda s,q:lambda r:r['p_'+s][q])(s,q) for s in ['fep','chest','demod','ldpc'] for q in ('p50','p90','p99')},
 'real mean excl >5ms bodies':lambda r:r['c_real']['mean']*r['c_real']['n']*(1-r['c_real']['s5'])/(r['c_real']['n']-r['c_real']['gt5']),
 'real mean excl >20ms bodies':lambda r:r['c_real']['mean']*r['c_real']['n']*(1-r['c_real']['s20'])/(r['c_real']['n']-r['c_real']['gt20']),
 'all-body mean (real+retry+probe)':lambda r:sum(r['c_'+k]['mean']*r['c_'+k]['n'] for k in ('real','retry','probe'))/sum(r['c_'+k]['n'] for k in ('real','retry','probe')),
 'pdschq drop_stale %':lambda r:100*r['pdschq_drop_stale']/max(1,r['pdschq_queued']),'pdschq drop_full':lambda r:r['pdschq_drop_full'],'pdschq queued':lambda r:r['pdschq_queued'],'pdschq decoded':lambda r:r['pdschq_decoded'],
 'scanq drop_stale':lambda r:r['scanq_drop_stale'],'CRC-OK real count':lambda r:r['crc_ok_real'],'CRC-OK RNTIs':lambda r:len(r['crc_rntis']),
 'stall events (>20ms, logged)':lambda r:len(r['stall_events']),'slow bodies logged':lambda r:r['slow_logged'],'longest stall ms':lambda r:max([e[2] for e in r['stall_events']] or [0])/1000}
P('\n## mean +- sd across runs, per arm')
for cap,arms in (('b',ARMS),('tim',ARMS)):
    P('### '+cap+'  (n runs: '+', '.join('%s=%d'%(a,len(col(cap,a,lambda r:1))) for a in arms)+')')
    P('metric | '+' | '.join(arms))
    for k,fn in metrics.items():
        P('%s | '%k+' | '.join('%.1f +- %.1f'%ms(col(cap,a,fn)) for a in arms))
for cap in ('b','tim'):
 for arm in ARMS[1:]:
  P('\n## significance on %s (%s vs %s), n = %d vs %d'%(cap,BASE,arm,len(col(cap,BASE,lambda r:1)),len(col(cap,arm,lambda r:1))))
  for nm in ('real mean us','real p50','real p90','real p99','real max ms','real gt5ms share %','real gt20ms count','sum p99','all-body mean (real+retry+probe)','real mean excl >20ms bodies','probe mean','retry mean','stage chest mean','stage demod mean','stage fep mean','stage chest max ms','stage demod max ms','pdschq drop_stale %','scanq drop_stale','CRC-OK real count','CRC-OK RNTIs','stall events (>20ms, logged)','longest stall ms'):
    fn=metrics[nm]; A=col(cap,BASE,fn); X=col(cap,arm,fn)
    if len(A)<2 or len(X)<2: continue
    t=stats.ttest_ind(A,X,equal_var=False); mw=stats.mannwhitneyu(A,X); ci=boot(X,A)
    P('%s: %s %.1f vs %s %.1f  diff=%.1f (%+.0f%%)  Welch p=%.3f  MWU p=%.3f  bootstrap95%% CI of diff [%.1f, %.1f]'%(nm,BASE,np.mean(A),arm,np.mean(X),np.mean(X)-np.mean(A),100*(np.mean(X)-np.mean(A))/max(1e-9,abs(np.mean(A))),t.pvalue,mw.pvalue,ci[0],ci[1]))
P('\n## decomposition: share of total decode (body) time by body duration, per class (PDTIMCLS), mean over runs')
for cap in ('b','tim'):
  for arm in ARMS:
    for k in ('real','retry','probe'):
        s=[(r['c_'+k]['s1'],r['c_'+k]['s5'],r['c_'+k]['s20']) for r in rows if r['cap']==cap and r['arm']==arm]
        if not s: continue
        a=np.array(s).mean(0)
        P('%s %s %s: <1ms %.2f | 1-5ms %.2f | 5-20ms %.2f | >20ms %.2f   (per-run >20ms share range %.2f-%.2f)'%(cap,arm,k,1-a[0],a[0]-a[1],a[1]-a[2],a[2],min(x[2] for x in s),max(x[2] for x in s)))
P('\n## PDTIMCLSST: per class x stage, ms of stage wall time in bodies <1 / 1-5 / 5-20 / >20 ms (sum over runs), per arm')
for cap in ('b','tim'):
  for arm in ARMS:
    rs=[r for r in rows if r['cap']==cap and r['arm']==arm and 'st_real' in r]
    if not rs: continue
    for k in ('real','probe'):
        line=[]
        for st in ('fep','chest','demod','ldpc'):
            v=np.sum([r['st_'+k][st] for r in rs],axis=0); T=max(1e-9,v.sum())
            line.append('%s %.0f/%.0f/%.0f/%.0f (>5ms share %.2f)'%(st,v[0],v[1],v[2],v[3],(v[2]+v[3])/T))
        P('%s %s %s: '%(cap,arm,k)+' | '.join(line))
P('\n## >20 ms bodies by stage (PDTIMSLOW, first 200/run, sum of stage wall us over logged slow bodies; share of that sum), per arm')
for cap in ('b','tim'):
  for arm in ARMS:
    tot={k:sum(r['slow_stage_sum'][k] for r in rows if r['cap']==cap and r['arm']==arm) for k in ('fep','chest','demod','ldpc')}
    T=max(1,sum(tot.values())); P('%s %s: '%(cap,arm)+', '.join('%s %.0f ms (%.0f%%)'%(k,v/1000,100*v/T) for k,v in tot.items())+'; dominant-stage counts '+str({k:sum(r['slow_stage_dom'].get(k,0) for r in rows if r['cap']==cap and r['arm']==arm) for k in ('fep','chest','demod','ldpc')}))
P('\n## per-stage mean vs p50 (tail excess), per arm')
for cap in ('b','tim'):
  for arm in ARMS:
    rs=[r for r in rows if r['cap']==cap and r['arm']==arm]
    if not rs: continue
    for s in ('fep','chest','demod','ldpc'):
        m=np.mean([r['t_'+s]['mean'] for r in rs]); p50=np.mean([r['p_'+s]['p50'] for r in rs]); p90=np.mean([r['p_'+s]['p90'] for r in rs]);p99=np.mean([r['p_'+s]['p99'] for r in rs])
        P('%s %s %s: mean %.0f us, p50 %.0f, p90 %.0f, p99 %.0f  => tail excess (mean-p50) %.0f us = %.0f%% of mean'%(cap,arm,s,m,p50,p90,p99,m-p50,100*(m-p50)/m))
P('\n## stall events (>20 ms body clusters; t mod 10000 s, #bodies, max us), per run')
for r in rows: P('%s %s %d: %s'%(r['cap'],r['arm'],r['rep'],r['stall_events']))
open(SUM,'w').write('\n'.join(out)+'\n')
