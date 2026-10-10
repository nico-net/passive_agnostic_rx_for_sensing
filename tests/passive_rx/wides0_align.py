#!/usr/bin/env python3
"""WIDE-S0: align the re-encoded codeword (wides0_oracle BITS) with the received S=0 stream; per RB of symbol 0 'a' = carries the
codeword before the gap, 'b' = after it (offset = full RE deficit), 'X' = neither (unavailable or overwritten). Usage: wides0_align.py OUT BITS LOG"""
import sys, os, json, re, numpy as np, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decodefix_inputs as df
out, bitsf, log = sys.argv[1:4]
meta=json.load(open(out+'.json'))
bits={}
b=open(bitsf,'rb').read(); i=0
while i<len(b):
  idx=int.from_bytes(b[i:i+4],'little'); G=int.from_bytes(b[i+4:i+8],'little'); bits[idx]=np.frombuffer(b[i+8:i+8+G],np.uint8); i+=8+G
occ={}; dci=collections.defaultdict(list)
for l in open(log,errors='replace'):
  m=re.search(r"DECODEPDCCHOCC rnti=0x(\S+) fs=(\S+) abs=\d+ S=0 attempt=\d+ nid=(\d+) rb=(\d+)\+(\d+) o0=(\S+) o1=(\S+) b0=(\S+) e0=(\S+)",l)
  if m: occ[(m.group(1),m.group(2),m.group(3))]=(int(m.group(4)),m.group(6),m.group(8),m.group(9))
  m=re.search(r"DCIRAW (\S+) rnti=0x(\S+) len=\d+ cce=(\d+) al=(\d+)",l)
  if m: dci[m.group(1)].append((m.group(2),int(m.group(3))*6,(int(m.group(3))+int(m.group(4)))*6-1))
for idx,t in bits.items():
  d=meta[idx]; raw=np.load(out+'.raw%d.npy'%idx).astype(np.float64)
  rb0,nrb=map(int,d['rb'].split('+')); qm=int(d['qm']); nl=int(d['nl']); u=12*qm*nl
  Gp=len(t); g=df.gold(max(len(raw),Gp),d['seed']); c=((1-g)//2).astype(np.int32)
  s=1-2*((t.astype(np.int32)^c[:Gp])); D=len(raw)-Gp
  tail=np.arange(nrb*u*2, len(raw)-100); sg=np.sign(np.mean(np.sign(raw[tail])*s[tail-D]))
  def agr(p0,p1,o):
    p=np.arange(p0,p1); j=p-o; m=(j>=0)&(j<Gp)
    return np.mean(np.sign(raw[p[m]])*sg==s[j[m]]) if m.any() else 0
  st=''
  for k in range(nrb):
    a0=agr(k*u,(k+1)*u,0); aD=agr(k*u,(k+1)*u,D)
    st+= 'a' if a0>0.8 and a0>=aD else ('b' if aD>0.8 else 'X')
  ex=[rb0+k for k,ch in enumerate(st) if ch=='X']
  r=d['rnti'][2:]
  print(d['rnti'],d['fs'],d['rb'],'deficitRB',D//u,'excl',(ex[0],ex[-1],len(ex)) if ex else None,'dci',dci.get(d['fs']))
  for nid in ('62922','10163','415'):
    o=occ.get((r,d['fs'],nid))
    if o and ex:
      lo=max(rb0,ex[0]-8); hi=min(rb0+nrb,ex[-1]+9)
      print('   nid %-5s RB %d..%d o0 %s b0 %s e0 %s'%(nid,lo,hi-1,o[1][lo-rb0:hi-rb0],o[2][lo-rb0:hi-rb0],o[3][lo-rb0:hi-rb0]))
  if ex: print('   map', st[max(0,ex[0]-8-rb0):ex[-1]+9-rb0])
