import sqlite3,pickle,sys,collections,bisect
c=sqlite3.connect(sys.argv[1]).cursor(); D=pickle.load(open(sys.argv[2],'rb')); P=D['passes']
S={i:s for i,s in c.execute("select id,value from StringIds")}
LK=sorted(c.execute("select k.start,k.end,s.value from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.shortName where k.end-k.start>1000000 and s.value in ('refine_kernel','segment','lattice_screen','sweep','energies','normalize','finish','combine','fep_extract','vector_fft','gather','fep_unpack','accumulate_kernel','lattice_pool','collect')").fetchall())
def cover(a,b):
    i=bisect.bisect_left(LK,(a-2_000_000_000,)); t=0; names=collections.Counter()
    for s,e,n in LK[max(0,i):]:
        if s>b: break
        o=min(b,e)-max(a,s)
        if o>0: t+=o; names[n]+=o
    return min(t,b-a), names
def q(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
full=[p for p in P if [s[0] for s in p['subs']]==['C','D','L'] and p['seq'] is not None]
sp=sorted(p['span'] for p in full)
for name,lo,hi in (('typical p45-p55',q(sp,.45),q(sp,.55)),('tail >= p99',q(sp,.99),1<<62),('p90-p99',q(sp,.90),q(sp,.99))):
    G=[p for p in full if lo<=p['span']<=hi]
    A=collections.Counter(); who=collections.Counter()
    for p in G:
        A['span']+=p['span']; A['own busy']+=p['busy']
        subs=p['subs']; heads={sb[3]:sb for sb in subs}
        prev=None
        for i,n in enumerate(p['nodes']):
            if prev is not None and n[0]>prev:
                g=n[0]-prev
                sb=heads.get(n[0])
                if sb and sb[2]>n[0]-1_000_000 and g>200_000: kind='gap: host launch call still blocked'
                elif p['isbody'][i] and (i==0 or not p['isbody'][i-1]): kind='gap: IF body launch'
                else: kind='gap: other (node dependency / waiting to start)'
                A[kind]+=g
                cv,nm=cover(prev,n[0]); A[kind+' | under a long (>1ms) discovery kernel']+=cv
                for k,v in nm.items(): who[k]+=v
            prev=n[1] if prev is None else max(prev,n[1])
    print(f"== {name}: n={len(G)}, mean per pass (us)")
    for k,v in sorted(A.items()): print(f"   {k:80s} {v/len(G)/1e3:9.1f}")
    print('   long discovery kernels under the gaps:',[(k,round(v/len(G)/1e3,1)) for k,v in who.most_common(5)])
