import pickle,sys,collections
P=pickle.load(open(sys.argv[1],'rb'))['passes']
def q(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
for lo,hi in ((200_000,1_000_000),(1_000_000,10_000_000),(10_000_000,1<<62)):
    G=[(g1-g0,o) for p in P for g0,g1,o in p['gaps'] if lo<=g1-g0<hi]
    if not G: continue
    tot=sum(d for d,_ in G)
    dl=sum(min(d,max(o.get('dl_pass_other',0),o.get('dl_nonpass',0),o.get('ul',0),o.get('recov',0))) for d,o in G)
    disc=sum(min(d,o.get('disc_csirs',0)) for d,o in G)
    nodl=[d for d,o in G if max(o.get('dl_pass_other',0),o.get('dl_nonpass',0),o.get('ul',0))<0.1*d]
    print(f"gaps {lo/1e3:.0f}-{hi/1e3 if hi<1<<61 else 'inf'} us: n={len(G)} total {tot/1e6:.1f} ms; covered by other DL/UL work >= {100*dl/tot:.0f} %, by CSI-RS discovery {100*disc/tot:.0f} %; gaps with <10 % DL/UL activity: {len(nodl)} ({sum(nodl)/1e6:.1f} ms)")
