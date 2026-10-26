import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
D=collections.defaultdict(lambda:[0,0])
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+')); m=int(d['mcs'])
    if m<20 or w>8: continue
    b=int(float(d['snr_db'])//2*2)
    D[b][0]+=d['ok']=='1'; D[b][1]+=1
for b in sorted(D):
    if D[b][1]>=40: print(b,D[b][0],D[b][1],"%.2f"%(D[b][0]/D[b][1]))
