import re,sys,collections
f=sys.argv[1]
c=collections.defaultdict(lambda:[0,0])
for l in open(f,errors='ignore'):
    if 'EVMGRANT' not in l: continue
    d=dict(re.findall(r'(\w+)=([^ ]+)',l))
    try:
        mcs=int(d['mcs']);rb=int(d['rb'].split('+')[1]);nl=int(d['Nl']);snl=[float(x) for x in d['snr_l'].split(',')][:nl]
    except: continue
    if mcs<20 or rb>64: continue
    if d['rnti']=='0xffff': continue
    mn=min(snl); b=int(mn//3*3)
    k=(nl,d['ports'],b); c[k][0]+=1; c[k][1]+=d['crc']=='1'
for k in sorted(c): 
    if c[k][0]>=20: print(k,c[k], round(c[k][1]/c[k][0],2))
