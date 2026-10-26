import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
C=collections.defaultdict(lambda:[0,0])
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+'))
    mcs=int(d['mcs'])
    if w>8 or mcs<20: continue
    k=(int(d['t'])%20, d['rnti'])
    C[k][0]+=d['ok']=='1'; C[k][1]+=1
for k in sorted(C):
    print(k,C[k][0],C[k][1],"%.2f"%(C[k][0]/C[k][1]))
