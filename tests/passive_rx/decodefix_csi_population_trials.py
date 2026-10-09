#!/usr/bin/env python3
"""Recorded-input, band-limited CSI-RS hypotheses; run via heavy.sh.
Arguments: LOG DUMP PRIOR_META PRIOR_LOG OUTPUT (the prior is the TDRA run).
The three bands and row-1 patterns come from measured CSI-RS discovery logs.
For a noncontiguous PRB allocation, physical-band inference is approximate;
only the full TB CRC establishes recovery. This is not a resource estimator.
"""
import sys,struct,json,re,itertools,numpy as np
import decodefix_inputs as df
log, dump, prior_meta, prior_log, output = sys.argv[1:]
prior=json.load(open(prior_meta));passed=set()
for line in open(prior_log):
 m=re.search(r'OFFLINE id=(\d+) crc=1',line)
 if m:
  d=prior[int(m[1])];passed.add((d['rnti'],d['fs']))
meta=[]
with open(output,'wb') as f:
 for d,raw,desc in df.records(log,dump):
  if d.get('attempt','0')!='0' or d['nl']!='1' or int(d['mcs'])>4 or float(d['snr'])<6 or (d['rnti'],d['fs']) in passed or d['S']!='0' or d['dmrs']!='0x804' or d['cdm']!='2' or any(d[k]!='0' for k in ['csi_re','ssb_re','ptrs_re']):continue
  rb,nrb=map(int,d['rb'].split('+'));seed=(int(d['rnti'],16)<<15)+int(d['dataid']);gs=df.gold(len(raw),seed)
  if desc:raw=raw*gs
  sy=np.repeat([i for i in range(14) if i not in [2,11]],nrb*24);sc=np.tile(np.repeat(np.arange(nrb*12),2),12);rbs=rb+sc//12
  # Band-limited resources actually observed by the discovery logger.
  for widths in [(52,52,52),(52,56,52),(64,64,64)]:
   for masks in itertools.product(range(4),repeat=3):
    if masks==(0,0,0):continue
    rm=np.zeros(len(raw),dtype=bool)
    for band,(start,width,mask) in enumerate(zip([0,64,128],widths,masks)):
     region=(rbs>=start)&(rbs<start+width)
     # Middle band ID249 uses fd2 on symbols5/9; no fd1 resource observed.
     if mask&1:rm|=region&((np.isin(sy,[4,8])&(sc%4==0)&(band!=1))|(np.isin(sy,[5,9])&(sc%4==1)))
     if mask&2:rm|=region&((np.isin(sy,[4,8])&(sc%4==2))|(np.isin(sy,[5,9])&(sc%4==3)))
    for S in [0,1]:
     A,BG=df.tbs(d,14-S);x=raw[(sy>=S)&~rm];llr,_=df.normalized(x*gs[:len(x)],A,BG);ix=len(meta)
     f.write(struct.pack('<12I',ix,A,len(llr),2,1,BG,0,int(d['lbrm']),8,1,1,0));f.write(llr.astype('<i2').tobytes());meta.append(dict(d,altS=S,widths=widths,masks=masks,altA=A,altG=len(llr)))
json.dump(meta,open(output+'.json','w'));print(len(meta))
