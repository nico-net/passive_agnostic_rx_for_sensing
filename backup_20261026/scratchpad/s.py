import re,sys,statistics as st
for f in sys.argv[1:]:
  rows={2:[],4:[],1:[]}
  for l in open(f,errors='ignore'):
    if 'EVMGRANT' not in l: continue
    d=dict(re.findall(r'(\w+)=([^ ]+)',l))
    try:
        mcs=int(d['mcs']);rb=int(d['rb'].split('+')[1]);nl=int(d['Nl']);ns=int(d['nsym'])
        snl=[float(x) for x in d['snr_l'].split(',')][:nl]; br=[float(x) for x in d['br_snr'].split(',')]
    except: continue
    if ns<9 or rb>64 or mcs<20 or d['rnti']=='0xffff' or nl not in rows: continue
    if nl==2 and d['ports']!='0x3': continue
    if nl==4 and d['ports']!='0xf': continue
    rows[nl].append((d['crc']=='1',max(br),min(snl),max(snl),float(d['snr']),float(d['evm'])))
  print(f)
  for nl,r in rows.items():
    if len(r)<20: continue
    for crc in (True,False):
      s=[x for x in r if x[0]==crc]
      if len(s)<10: continue
      print(' nl',nl,'crc',crc,len(s),'brmax %.1f snl_min %.1f snl_max %.1f snr %.1f evm %.1f'%tuple(st.median(x[i] for x in s) for i in range(1,6)))
