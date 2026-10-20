import sys,re; sys.path.insert(0,'.')
sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-int16gpu/tests/passive_rx')
import goal_metrics as gm
from collections import Counter
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4 or p[1] not in ('OFF','ON'): continue
    cap,arm,i,d=p
    cnt=Counter(); prb=Counter(); R=[]
    for raw in open(d+'/run.log',errors='replace'):
        if 'EVMGRANT' not in raw: continue
        m=re.search(r'EVMGRANT rnti=\S+ (\d+)\.(\d+) .*?rb=\d+\+(\d+).*?probe=(\d).*?e2e=([-\d,]+)',gm.ANSI.sub('',raw))
        if not m: continue
        fs=(int(m.group(1)),int(m.group(2))); n=int(m.group(3))
        cnt[fs]+=1; prb[fs]+=n
        if m.group(4)=='0':
            tot=int(m.group(5).split(',')[6])
            if tot>=0: R.append((tot,fs))
    R.sort(); p99=R[int(.99*len(R))][0]; T=[fs for t,fs in R if t>=p99]
    A=[fs for t,fs in R]
    mean=lambda L,c: sum(c[x] for x in L)/len(L)
    # neighbouring-slot load: grants in slots s-2..s
    def nb(fs,c):
        f,s=fs; return sum(c[((f*20+s-k)//20,(f*20+s-k)%20)] for k in range(0,3))
    print(cap,arm,i,f"p99={p99} grants/slot all={mean(A,cnt):.2f} tail={mean(T,cnt):.2f} | PRB/slot all={mean(A,prb):.0f} tail={mean(T,prb):.0f} | grants in slots s-2..s all={sum(nb(x,cnt) for x in A)/len(A):.1f} tail={sum(nb(x,cnt) for x in T)/len(T):.1f}")
