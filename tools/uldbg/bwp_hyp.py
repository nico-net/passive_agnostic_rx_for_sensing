#!/usr/bin/env python3
"""UL-LAYOUT offline: DM-RS MRC SNR (ulphys.dmrs_est) of every <=8-PRB DCI 0_1 grant of a replay log under several UL BWP
hypotheses (N, start) and a 2-frames-later control.  usage: bwp_hyp.py run.log out.npy  (capture b)"""
import sys, numpy as np, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ulphys import *
log=sys.argv[1]; out=sys.argv[2]
D,ts=parse_dcis(log); OFF=frame_offset(ts,CAPS['b'][1]); print('OFF',OFF,len(D))
H=[(216,1),(216,0),(216,2),(217,0),(215,1),(215,2),(214,2),(216,1001)]  # 1001: control (2 frames later)
def rd(N,riv):
    return rivtab(N).get(riv)
rows=[]
for d in D:
    a,rnti,ln,p=d; b=bits(p,ln); riv=field(b,2,15); t=a+2
    fr=t//20-OFF; sl=t%20
    if sl not in (8,9,18,19) or fr<0 or fr+2>=1199: continue
    r0=rd(216,riv)
    if r0 is None or r0[1]>8: continue   # small grants only (cheap + where the evidence is)
    res=[]
    for N,bs in H:
        ctl = bs>=1000; bs=bs%1000
        r=rd(N,riv)
        if r is None or bs+r[0]+r[1]>217: res.append(np.nan); continue
        s,_=dmrs_est('b',fr+(2 if ctl else 0),sl,bs+r[0],bs+r[0]+r[1])
        res.append(10*np.log10(s.sum()))
    rows.append([a,rnti,ln,riv,r0[1],*res])
R=np.array(rows,float); np.save(out,R)
names=['%d@%d'%h for h in H]
for t in (0,3,6,10):
    print('>%d dB:'%t,' '.join('%s=%d'%(n,(R[:,5+i]>t).sum()) for i,n in enumerate(names)))
best=np.nanargmax(R[:,5:5+len(H)-1],axis=1); strong=np.nanmax(R[:,5:5+len(H)-1],axis=1)>6
print('argmax among strong(>6dB):',collections.Counter(names[i] for i in best[strong]))
