import re,sys,collections
c=collections.defaultdict(lambda:[0,0])
for f in sys.argv[1:]:
  for l in open(f,errors='ignore'):
    if 'EVMGRANT' not in l: continue
    d=dict(re.findall(r'(\w+)=([^ ]+)',l))
    try:
        mcs=int(d['mcs']);rb=int(d['rb'].split('+')[1]);nl=int(d['Nl']);kap=float(d['kappa']);ns=int(d['nsym']);qm=int(d['Qm'])
    except: continue
    if qm!=8 or ns<9 or rb>64 or d['rnti']=='0xffff' or nl not in(2,4): continue
    if nl==2 and d['ports']!='0x3': continue
    if nl==4 and d['ports']!='0xf': continue
    k=(nl,round(kap,1)); c[k][0]+=1; c[k][1]+=d['crc']=='1'
for k in sorted(c):
    if c[k][0]>=15: print(k,c[k],round(c[k][1]/c[k][0],2))
