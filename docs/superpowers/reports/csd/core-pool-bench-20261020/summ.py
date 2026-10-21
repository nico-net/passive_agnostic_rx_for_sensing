import re,glob,statistics as st,collections,sys,os
D=sys.argv[1]
R=collections.defaultdict(list)
for f in glob.glob(D+'/raw/*.txt'):
    m=re.match(r'(pool|cfs)_N(\d+)_s([\d.]+)_r(\d)',os.path.basename(f))
    mode,N,S,r=m.group(1),int(m.group(2)),float(m.group(3)),int(m.group(4))
    d={}
    for l in open(f):
        h=re.match(r'(POOL|CFS) .*busy_cores=([\d.]+) disc_chunks/s=(\d+) yields=(\d+) nested=(\d+)',l)
        if h: d['busy']=float(h[2]);d['disc']=float(h[3]);d['nested']=int(h[5]);continue
        for k2,nm in(('scan','ts'),('decode','td')):
            t=re.search(k2+r'\[n=\d+ waited=\d+ promo=\d+ wait_us p50<=(\d+) p99<=(\d+) max=(\d+)\]',l)
            if t: d[nm+'50']=float(t[1]);d[nm+'99']=float(t[2]);d[nm+'max']=float(t[3])
        c=re.match(r'\s+(scan|dl|ul|retry)\s+n=(\d+)\s+wait_us p50=\s*(\d+) p99=\s*(\d+) max=\s*(\d+) \| lat_us p50=\s*(\d+) p99=\s*(\d+) max=\s*(\d+) \| svc_p50=\s*(\d+) miss=(\d+)/(\d+) drop=(\d+)',l)
        if c:
            k=c[1];v=list(map(float,c.groups()[1:]))
            d[k]=dict(n=v[0],w50=v[1],w99=v[2],wmax=v[3],l50=v[4],l99=v[5],lmax=v[6],miss=v[8]/v[9]*100,drop=v[10])
    R[(mode,N,S)].append(d)
med=lambda xs:st.median(xs)
rows=[]
for (mode,N,S),L in sorted(R.items(),key=lambda x:(x[0][1],x[0][2],x[0][0])):
    row=dict(mode=mode,N=N,scale=S,load=round(3.41*S,2),ratio=round(3.41*S/N,2),reps=len(L),busy=med([d.get('busy',0) for d in L]),nested=med([d.get('nested',0) for d in L]),**{k:med([d.get(k,0) for d in L]) for k in('ts50','ts99','tsmax','td50','td99','tdmax')})
    for k in('scan','dl','ul','retry'):
        for f in('w50','w99','wmax','l50','l99','lmax','miss','drop'):
            row[k+'_'+f]=med([d[k][f] for d in L if k in d])
    rows.append(row)
cols=list(rows[0].keys())
open(D+'/summary_medians.csv','w').write(','.join(cols)+'\n'+'\n'.join(','.join(str(round(r[c],1)) if isinstance(r[c],float) else str(r[c]) for c in cols) for r in rows)+'\n')
with open(D+'/summary_table.txt','w') as o:
    o.write('[Bench, cloud x86] medians over repeats, 15 s runs, taskset -c 0-3, pool/cfs cores 0..N-1. times in us; wait = arrival->start, lat = arrival->done; miss%% = lat > deadline (scan 500 us, DL/UL 2 ms)\n')
    o.write('mode N scale load ratio | scan w99 l99 miss%% | dl w50 w99 l99 miss%% | ul w50 w99 l99 miss%% | retry w99 | busy nested | token wait scan p99<=/max decode p99<=/max\n')
    for r in rows:
        o.write('%-4s %d %.2f %.2f %.2f | %7.0f %7.0f %5.1f | %8.0f %8.0f %8.0f %5.1f | %8.0f %8.0f %8.0f %5.1f | %8.0f | %.2f %d | %5.0f %6.0f %7.0f %7.0f\n'%(r['mode'],r['N'],r['scale'],r['load'],r['ratio'],r['scan_w99'],r['scan_l99'],r['scan_miss'],r['dl_w50'],r['dl_w99'],r['dl_l99'],r['dl_miss'],r['ul_w50'],r['ul_w99'],r['ul_l99'],r['ul_miss'],r['retry_w99'],r['busy'],r['nested'],r['ts99'],r['tsmax'],r['td99'],r['tdmax']))
print(open(D+'/summary_table.txt').read())
