import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
C=collections.defaultdict(lambda:[0,0])
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    ok=d.get('ok')=='1'
    try: prb=int(d['prb'].split('+')[1])
    except: continue
    if d.get('data_id')!='2' or d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    mcs=int(d.get('mcs',-1))
    pb='a1-8' if prb<=8 else 'b9-64' if prb<=64 else 'c>64'
    mb='m0-9' if mcs<10 else 'm10-19' if mcs<20 else 'm20-28'
    for k in [('all',),(pb,mb),('uci',pb,mb,d.get('uci_hint')),('seg>0',pb,mb,d.get('seg','').split('/')[0]!='0'),('sfo?',d.get('sfo'),)]:
        C[k][0]+=ok; C[k][1]+=1
for k in sorted(C,key=str):
    print(k,C[k][0],C[k][1],"%.2f"%(C[k][0]/C[k][1]))
