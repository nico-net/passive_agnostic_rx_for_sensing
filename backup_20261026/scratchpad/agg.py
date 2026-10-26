import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
C=collections.defaultdict(lambda:[0,0])
tot=[0,0]
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    ok=d.get('ok')=='1'
    try: prb=int(d['prb'].split('+')[1])
    except: continue
    mcs=int(d.get('mcs',-1))
    pb='1-8' if prb<=8 else '9-64' if prb<=64 else '>64'
    mb='m0-9' if mcs<10 else 'm10-19' if mcs<20 else 'm20-28'
    keys=[('all',),('prb',pb),('mcs',mb),('prbmcs',pb,mb),('probe',d.get('probe')),('uci_hint',d.get('uci_hint')),('uci_re>0',int(d.get('uci_re','0'))>0),('hyp',d.get('hyp','').split('@')[-1]),('cdm',d.get('cdm')),('delay',d.get('delay')),('reason',d.get('reason','') if not ok else 'ok'),('snr',int(float(d.get('snr_db','0'))//5*5)),('skip',d.get('skip')),('nid',d.get('nid')),('data_id',d.get('data_id'))]
    for k in keys:
        C[k][0]+=ok; C[k][1]+=1
for k in sorted(C,key=lambda k:(k[0],str(k[1:]))):
    print(k,C[k][0],C[k][1],"%.2f"%(C[k][0]/C[k][1]))
