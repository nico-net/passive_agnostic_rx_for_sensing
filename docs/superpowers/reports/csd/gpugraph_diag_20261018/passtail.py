import json,sys
def q(v,f): v=sorted(v); return v[min(len(v)-1,int(f*len(v)))] if v else float('nan')
for f in sys.argv[1:]:
    R=[r for r in json.load(open(f)) if 'tot' in r]
    p99=q([r['tot'] for r in R],.99); T=[r for r in R if r['tot']>=p99 and r.get('path')=='pass' and 'wait' in r]
    for r in T: r['gpu_part']=r['body']-r['host_pre']-r['host_post']
    print(f.split('/')[-2],'tail pass n',len(T),' med body %.0f host_pre %.0f gpu_part(launch..wait end) %.0f wait %.0f host_post %.0f'%tuple(q([r[k] for r in T],.5) for k in ('body','host_pre','gpu_part','wait','host_post')))
    for r in sorted(T,key=lambda r:-r['tot'])[:6]:
        print('   tot %5d a2e %4d qw %4d pre %4d dec %5d | body %5.0f hpre %4.0f gpu %5.0f (C %s D %s L %s) poll %4.0f post %4.0f nrb %d nl %d'%(r['tot'],r['a2e'],r['qw'],r['pre'],r['dec'],r['body'],r['host_pre'],r['gpu_part'],
          '%.0f/%.0f'%(r.get('q_C',0) or 0,r.get('x_C',0) or 0),'%.0f/%.0f'%(r.get('q_D',0),r.get('x_D',0)),'%.0f/%.0f'%(r.get('q_L',0),r.get('x_L',0)),r['poll_lat'] or 0,r['host_post'],r['nrb'],r['nl']))
