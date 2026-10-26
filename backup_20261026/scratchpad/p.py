import re,sys,collections
def load(f):
    out=[]
    for l in open(f,errors='ignore'):
        if 'EVMGRANT' not in l: continue
        d=dict(re.findall(r'(\w+)=([^ ]+)',l))
        out.append(d)
    return out
for f in sys.argv[1:]:
    g=load(f); print(f,len(g))
    c=collections.defaultdict(lambda:[0,0])
    for d in g:
        try: mcs=int(d['mcs']);rb=int(d['rb'].split('+')[1]);nl=int(d['Nl'])
        except: continue
        if mcs<20: continue
        k=(nl,'rb<=32' if rb<=32 else 'rb<=128' if rb<=128 else 'big', d['ports'],d['cdm'],d['comb'])
        c[k][0]+=1;c[k][1]+=d['crc']=='1'
    for k in sorted(c): print(k,c[k])
