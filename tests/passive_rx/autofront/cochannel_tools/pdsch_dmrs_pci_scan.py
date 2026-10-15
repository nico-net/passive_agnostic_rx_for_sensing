import sys; sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-pcis2/tests/passive_rx/autofront')
import cochannel as cc, numpy as np, ssb_search2 as S, time
cap=S.Cap(sys.argv[1]); d=np.load(sys.argv[2]); T=d['T']; cfo=float(sys.argv[3]); chunks=[int(x) for x in sys.argv[4].split(',')] if len(sys.argv)>4 else list(range(1,48,2))
slots=(10,11,12,16,17)
g=cc.Grid(cap,3560.85e6,cfo,nsc=12*60)
def sy(c,n,l): return np.stack([g.sym(c*cc.CH+16*T[c]+cc.body(n,l),ch) for ch in range(4)])
nre=6*16; kk=2*np.arange(nre)
keys=[];Ys=[]
for c in chunks:
    for n in slots:
        for l in (3,7,11):
            keys.append((c,n,l)); Ys.append(sy(c,n,l)[:,24+kk])
Ys=np.array(Ys)  # (K,4,nre)
taus=np.arange(-320,321,16); Ph=np.exp(2j*np.pi*taus[:,None]*kk[None,:]/cc.N)   # (nt,nre)
BL=24
t=time.time(); score=np.zeros((1008,len(taus))); 
for pci in range(1008):
    R=np.array([cc.dmrs_pdsch(pci,n%20,l,0,nre) for (c,n,l) in keys])   # (K,nre)
    X=np.conj(R)[:,None,:]*Ys                                      # (K,4,nre)
    Z=np.einsum('kae,te->kat',X,Ph)  if False else None
    # blockwise: (K,4,nblk,BL) * phase -> sum
    Xb=X.reshape(len(keys),4,nre//BL,BL); Pb=Ph.reshape(len(taus),nre//BL,BL)
    Zb=np.einsum('kabe,tbe->katb',Xb,Pb)
    score[pci]=(np.abs(Zb)**2).sum((0,1,3))
med=np.median(score); 
best=score.max(1)/med; bt=taus[score.argmax(1)]
o=np.argsort(-best)[:10]
print('chunks',len(chunks),'top PCIs: (pci, dB over median, best tau)',[(int(p),round(10*np.log10(best[p]),1),int(bt[p])) for p in o],f'{time.time()-t:.0f}s')
print('508,4,614:',[(p,round(10*np.log10(best[p]),1),int(bt[p])) for p in (508,4,614)])
np.save(sys.argv[2].replace('.npz','_scan.npy'),score)
