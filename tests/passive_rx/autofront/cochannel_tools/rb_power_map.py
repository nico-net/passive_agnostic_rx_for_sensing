import sys; sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-pcis2/tests/passive_rx/autofront')
import cochannel as cc, numpy as np, ssb_search2 as S
cap=S.Cap(sys.argv[1]); d=np.load(sys.argv[2]); T=d['T']
g=cc.Grid(cap,3560.85e6,-4761,nsc=12*162)
np.set_printoptions(linewidth=250, precision=0, suppress=True)
def sy(c,n,l): return np.stack([g.sym(c*cc.CH+16*T[c]+cc.body(n,l),ch) for ch in range(4)])
for n in (10,11,12):
  for par,cs in (('odd',range(1,50,2)),('even',range(0,50,2))):
    P=np.zeros((14,162))
    for c in cs:
        for l in range(14):
            if l==0: continue
            Y=sy(c,n,l); P[l]+=(np.abs(Y)**2).sum(0).reshape(162,12).mean(1)
    P/=len(cs); print('slot',n,par,'RB power dB, rows=l (RB 0..79 step 2):')
    for l in range(1,14): print(l, ' '.join(f'{10*np.log10(P[l,r]+1):2.0f}' for r in range(0,80,2)))
