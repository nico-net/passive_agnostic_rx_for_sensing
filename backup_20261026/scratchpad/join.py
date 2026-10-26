import sys,re,csv,collections
kv=re.compile(r'(\w+)=(\S+)')
gt={}
for r in csv.DictReader(open(sys.argv[1])):
    if r['dir']!='ul' or r['kind']!='pusch': continue
    m=re.match(r'\[(\d+)\.\.(\d+)\)',r['rb']); 
    # gnb slot of PUSCH: row frame/slot is the PUSCH slot (dci_slot separate)
    gt[(int(r['frame'])*20+int(r['slot']), r['rnti'])]=(int(m.group(1)),int(m.group(2))-int(m.group(1)),int(r['tbs']),r['newtx'],int(r['h_id']))
rows=[]
for line in open(sys.argv[2],errors='replace'):
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+'))
    rows.append((int(d['t']),d['rnti'],s,w,int(d['tbs']),d['ok']=='1',int(d['mcs'])))
    if len(rows)>=4000: break
best=None
for sh in range(0,20480*2):
    n=sum(1 for (t,rn,s,w,tbs,ok,m) in rows if ((t+sh)%20480,rn) in gt and gt[((t+sh)%20480,rn)][:2]==(s,w))
    if best is None or n>best[0]: best=(n,sh)
print("best shift",best,len(rows))
