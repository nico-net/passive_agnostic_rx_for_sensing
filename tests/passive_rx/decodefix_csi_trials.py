#!/usr/bin/env python3
"""Diagnostic row-1 CSI-RS exclusion trials on recorded full-slot QPSK LLRs.
Run via heavy.sh. Arguments: LOG DUMP PRIOR_META PRIOR_LOG OUTPUT.
PRIOR files are the decodefix_inputs.py tdra run and decodefix_ldpc output.
This is an offline hypothesis search, not a CSI-RS configuration estimator.
"""
import sys, json, re, struct
import numpy as np
import decodefix_inputs as df
log, dump, prior_meta, prior_log, output = sys.argv[1:]
m=json.load(open(prior_meta));ok=set()
for l in open(prior_log):
 if not l.startswith('OFFLINE'):continue
 d=dict(re.findall(r'(\w+)=([^\s]+)',l));r=m[int(d['id'])]
 if d['crc']=='1':ok.add((r['rnti'],r['abs']))
meta=[]
with open(output,'wb') as f:
 for d,raw,desc in df.records(log,dump):
  if d.get('attempt','0')!='0' or d['nl']!='1' or int(d['mcs'])>4 or float(d['snr'])<6 or (d['rnti'],d['abs']) in ok or d['S']!='0' or d['csi_re']!='0':continue
  nrb=int(d['rb'].split('+')[1]);G=len(raw);gs=df.gold(G,(int(d['rnti'],16)<<15)+int(d['dataid']))
  if desc:raw=raw*gs
  # QPSK, one layer, two CDM groups; symbol 2 and 11 contain no data.
  if d['qm']!='2' or d['cdm']!='2' or d['dmrs']!='0x804':continue
  sy=np.repeat([i for i in range(14) if i not in [2,11]],nrb*24)
  sc=np.tile(np.repeat(np.arange(12),2),12*nrb)
  assert len(sy)==len(sc)==G
  for S in [0,1]:
   A,BG=df.tbs(d,14-S)
   for ma in range(16):
    for mb in range(16):
     if ma==mb==0:continue
     rm=np.zeros(G,dtype=bool)
     for bit in range(4):
      if (ma>>bit)&1:rm|=np.isin(sy,[4,8]) & (sc%4==bit)
      if (mb>>bit)&1:rm|=np.isin(sy,[5,9]) & (sc%4==bit)
     llr=raw[(sy>=S)&~rm];llr,shift=df.normalized(llr*gs[:len(llr)],A,BG)
     ix=len(meta); f.write(struct.pack('<12I',ix,A,len(llr),2,1,BG,int(d['rv']),int(d['lbrm']),8,1,1,0));f.write(llr.astype('<i2').tobytes());meta.append(dict(d,altS=S,ma=ma,mb=mb,alt_tbs=A,altG=len(llr)))
json.dump(meta,open(output+'.json','w'));print(len(meta))
