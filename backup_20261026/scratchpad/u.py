import re,sys,collections
c=collections.defaultdict(lambda:[0,0])
for f in sys.argv[1:]:
  for l in open(f,errors='ignore'):
    if 'EVMGRANT' not in l: continue
    d=dict(re.findall(r'(\w+)=([^ ]+)',l))
    try:
        mcs=int(d['mcs']);rb=int(d['rb'].split('+')[1]);nl=int(d['Nl']);ns=int(d['nsym']);l2=int(d['l2'])
        snl=[float(x) for x in d['snr_l'].split(',')][:nl]
    except: continue
    if ns<9 or mcs<20 or d['rnti']=='0xffff' or nl not in(2,4,1) or rb>64: continue
    if nl==2 and d['ports']!='0x3': continue
    if nl==4 and d['ports']!='0xf': continue
    if nl==1 and d['ports'] not in('0x1','0x2'): continue
    m=min(snl)
    if m<24: continue
    k=(nl,l2); c[k][0]+=1;c[k][1]+=d['crc']=='1'
for k in sorted(c):
  if c[k][0]>=20: print(k,c[k],round(c[k][1]/c[k][0],2))
