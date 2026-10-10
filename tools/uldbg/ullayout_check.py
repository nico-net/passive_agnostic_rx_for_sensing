#!/usr/bin/env python3
"""UL-LAYOUT replay check: ULLAYOUT FIT/LOCK lines, post-lock ULDCI rb/k2 vs the 216@1 reading of the raw ULDBG_DCI payload
(capture b / PCI 415), and the UL pipeline counters.  usage: ullayout_check.py run.log  (needs ISAC_UL_DBG_LOG=1 ISAC_UL_DCI_LOG=1)"""
import re,sys,collections,numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ulphys import rivtab
log=sys.argv[1]
t=re.sub(r'\x1b\[[0-9;]*m','',open(log,errors='ignore').read())
for l in t.splitlines():
    if 'ULLAYOUT' in l and ('LOCK' in l or 'FIT len' in l or 'reset' in l): print(l[l.find('ULLAYOUT'):][:900])
st=[l for l in t.splitlines() if 'ULLAYOUT STATS' in l]
if st: print(st[-1][st[-1].find('ULLAYOUT'):][:900])
pay={}
for a,r,n,p in re.findall(r'ULDBG_DCI abs=(\d+) f=\d+ s=\d+ rnti=0x([0-9a-f]+) fmt=\d+ len=(\d+) .*? payload=0x([0-9a-f]+)',t):
    pay[(int(a),int(r,16))]=(int(n),int(p,16))
lock_pos={}
for m in re.finditer(r'ULLAYOUT LOCK len=(\d+) ',t): lock_pos.setdefault(int(m.group(1)),m.start())
ulm=list(re.finditer(r'ULDCI rnti=0x([0-9a-f]+) abs=(\d+) fs=\S+ fmt=0_1 len=(\d+) k2=(\d+) rb=(\d+)\+(\d+) mcs=(\d+)',t))
ul=[m.groups() for m in ulm]
ok=bad=pre=0; ex=[]
for m in ulm:
    r,a,n,k2,rb,nr,mcs=m.groups()
    a=int(a); n=int(n); key=(a,int(r,16))
    if n not in lock_pos or m.start()<lock_pos[n]: pre+=1; continue
    if key not in pay: continue
    L,p=pay[key]; riv=(p>>(L-17))&0x7fff; d=rivtab(216).get(riv)
    if d and 1+d[0]==int(rb) and d[1]==int(nr) and int(k2)==2: ok+=1
    else:
        bad+=1
        if len(ex)<5: ex.append((r,a,n,k2,rb,nr,riv,d))
print('ULDCI lines',len(ul),'pre-lock',pre,'post-lock matching 216@1/k2=2:',ok,'mismatch',bad,ex)
by=collections.Counter(int(n) for _,_,n,*_ in ul); print('ULDCI by len',dict(by))
for k in ('UL_PUSCH_CRC','PUSCHQ','UL_PIPELINE','UL_CHAIN_DROP'):
    m=[l for l in t.splitlines() if k in l]
    if m: print(k,len(m), m[-1][m[-1].find(k):][:300])
print('UL_PUSCH_CRC ok=1:',len(re.findall(r'UL_PUSCH_CRC rnti=\S+ ok=1',t)))
print('EOF:', 'RAW_IQ_EOF' in t)
