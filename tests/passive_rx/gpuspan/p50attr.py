import pickle,sys,re,collections,statistics as st
P=pickle.load(open(sys.argv[1],'rb'))['passes']
KV=re.compile(r'(\w+)=(\S+)'); EV={}
for l in open(sys.argv[2],errors='replace'):
    if 'EVMGRANT' in l and ' gq=' in l:
        k=dict(KV.findall(re.sub(r'\x1b\[[0-9;]*m','',l))); EV[int(k['gq'].split(',')[3])]=k
def q(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
full=[p for p in P if [s[0] for s in p['subs']]==['C','D','L']]
solo=collections.defaultdict(list)
for p in full:
    for s,e,n,stv,gc,gx,o in p['nodes']:
        if max([v for k,v in o.items() if k!='ANY'] or [0])<0.05*(e-s): solo[(n,gx)].append(e-s)
solo={k:sorted(v)[len(v)//2] for k,v in solo.items() if len(v)>=20}
real=[p for p in full if p['seq'] in EV and EV[p['seq']].get('probe','0')=='0']
sp=sorted(p['span'] for p in real); T=[p for p in real if q(sp,.45)<=p['span']<=q(sp,.55)]
A=collections.Counter()
for p in T:
    A['span']+=p['span']; A['gaps']+=p['span']-p['busy']
    for i,(s,e,n,stv,gc,gx,o) in enumerate(p['nodes']):
        sm=solo.get((n,gx))
        if sm is None: A['own kernel (no solo ref)']+=e-s; continue
        A['own kernel at solo speed']+=min(e-s,sm); ex=max(0,e-s-sm)
        cats=[k for k,v in o.items() if k!='ANY' and v>0.2*(e-s)]
        key='stretch: '+('+'.join(sorted(c for c in cats if c!='disc_csirs')) or ('discovery only' if 'disc_csirs' in cats else 'nothing overlapping'))
        A[key]+=ex
print('real typical passes',len(T),'span p50',q([p['span']/1e3 for p in T],.5))
for k,v in sorted(A.items(),key=lambda x:-x[1]): print(f"  {k:55s} {v/len(T)/1e3:7.1f} us")
