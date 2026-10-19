import pickle,sys,re,collections
P=pickle.load(open(sys.argv[1],'rb'))['passes']
KV=re.compile(r'(\w+)=(\S+)'); EV={}
for l in open(sys.argv[2],errors='replace'):
    if 'EVMGRANT' in l and ' gq=' in l:
        k=dict(KV.findall(re.sub(r'\x1b\[[0-9;]*m','',l))); EV[int(k['gq'].split(',')[3])]=k
def q(v,p):
    v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
full=[p for p in P if [s[0] for s in p['subs']]==['C','D','L'] and p['seq'] in EV and EV[p['seq']].get('probe','0')=='0']
sp=sorted(p['span'] for p in full); T=[p for p in full if q(sp,.45)<=p['span']<=q(sp,.55)]
A=collections.defaultdict(list)
for p in T:
    prev=None; heads={sb[3] for sb in p['subs']}
    for i,n in enumerate(p['nodes']):
        if prev is not None and n[0]>prev:
            g=n[0]-prev
            if n[0] in heads: k='sub-graph head ('+n[2]+')'
            elif p['isbody'][i] and not p['isbody'][i-1]: k='IF body first node ('+n[2]+')'
            elif n[2].startswith('memcpy'): k='memcpy node'
            else: k='kernel node after kernel'
            A[k].append(g)
        prev=n[1] if prev is None else max(prev,n[1])
print(len(T),'typical real passes')
for k,v in sorted(A.items(),key=lambda x:-sum(x[1])): print(f"  {k:45s} per pass {sum(v)/len(T)/1e3:7.1f} us  (gap p50 {q(v,.5)/1e3:6.1f} p90 {q(v,.9)/1e3:6.1f}, n {len(v)})")
