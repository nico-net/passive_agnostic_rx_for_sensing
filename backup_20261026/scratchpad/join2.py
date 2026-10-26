import sys,re,csv,collections
kv=re.compile(r'(\w+)=(\S+)')
sh=int(sys.argv[3])
gt={}
for r in csv.DictReader(open(sys.argv[1])):
    if r['dir']!='ul' or r['kind']!='pusch': continue
    m=re.match(r'\[(\d+)\.\.(\d+)\)',r['rb'])
    gt[(int(r['frame'])*20+int(r['slot']), r['rnti'])]=(int(m.group(1)),int(m.group(2))-int(m.group(1)),int(r['tbs']),r['newtx'],r['rv'],r['h_id'],r['layers'],r['dci_slot'])
C=collections.defaultdict(lambda:[0,0]); miss=[0,0]
for line in open(sys.argv[2],errors='replace'):
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+'))
    t=int(d['t']); k=((t+sh)%20480,d['rnti'])
    g=gt.get(k)
    if not g or g[:2]!=(s,w): miss[0]+=1; continue
    miss[1]+=1
    if w>8 or int(d['mcs'])<20: continue
    key=('slot',t%10,'new' if g[3]=='1' else 'rtx','rv'+g[4],'layers'+g[6])
    C[key][0]+=d['ok']=='1'; C[key][1]+=1
print("unmatched/matched",miss)
for k in sorted(C): print(k,C[k][0],C[k][1])
