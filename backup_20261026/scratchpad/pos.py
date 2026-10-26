import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
D=collections.defaultdict(lambda:[0,0])
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+')); m=int(d['mcs'])
    if m<20 or w!=6: continue
    snr=float(d['snr_db'])
    if snr<36: continue
    D[(s,'hi' if snr>=42 else 'lo')][0]+=d['ok']=='1'; D[(s,'hi' if snr>=42 else 'lo')][1]+=1
for k in sorted(D):
    if D[k][1]>=30: print(k,D[k][0],D[k][1],"%.2f"%(D[k][0]/D[k][1]))
