#!/usr/bin/env python3
"""Recorded-input type-1 DM-RS CDM/S alternatives. LOG DUMP OUTPUT; use heavy.sh.
Missing REs are erasures; never synthesize received data. QPSK/ports 0,1 only.
"""
import sys, json, struct
import numpy as np
import decodefix_inputs as df
log,dump,output=sys.argv[1:]
meta=[]
with open(output,'wb') as f:
 for d,raw,desc in df.records(log,dump):
  if d.get('attempt','0')!='0' or d['qm']!='2' or int(d['S'])>1 or int(d['S'])+int(d['L'])!=14 or d['dmrs']!='0x804' or d.get('dmrs_type','0')!='0' or int(d['ports'],16)&~3 or d['csi_re']!='0' or d['ssb_re']!='0' or d['ptrs_re']!='0':continue
  nrb=int(d['rb'].split('+')[1]);nl=int(d['nl']);unit=2*nl; cdm=int(d['cdm']);seed=(int(d['rnti'],16)<<15)+int(d['dataid'])
  if cdm not in [1,2]:continue
  if desc:raw=raw*df.gold(len(raw),seed)
  slot=np.zeros((14,nrb*12,unit),dtype=np.int16)
  at=0
  for sy in range(int(d['S']),14):
   re=np.arange(nrb*12)
   if sy in [2,11]:re=re[re%2==1] if cdm==1 else re[:0]
   n=len(re)*unit;slot[sy,re]=raw[at:at+n].reshape(len(re),unit);at+=n
  assert at==len(raw),(at,len(raw),d)
  gs=df.gold(14*nrb*12*unit,seed)
  for S in [0,1]:
   for altcdm in [1,2]:
    parts=[]
    for sy in range(S,14):
     re=np.arange(nrb*12)
     if sy in [2,11]:re=re[re%2==1] if altcdm==1 else re[:0]
     parts.append(slot[sy,re].flatten())
    x=np.concatenate(parts)
    fake=dict(d,G=str((int(d['L'])*12-12*altcdm)*nrb*unit))
    A,BG=df.tbs(fake,14-S)
    llr,shift=df.normalized(x*gs[:len(x)],A,BG)
    ix=len(meta);f.write(struct.pack('<12I',ix,A,len(llr),2,nl,BG,int(d['rv']),int(d['lbrm']),8,1,1,0));f.write(llr.astype('<i2').tobytes());meta.append(dict(d,altS=S,alt_cdm=altcdm,alt_tbs=A,altG=len(llr)))
json.dump(meta,open(output+'.json','w'));print(len(meta))
