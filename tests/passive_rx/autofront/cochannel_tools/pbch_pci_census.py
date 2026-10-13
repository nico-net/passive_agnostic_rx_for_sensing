import sys; sys.path.insert(0,'/home/nicola/NICOLA/wt/rr-csd-pcis2/tests/passive_rx/autofront')
import cochannel as cc, numpy as np, ssb_search2 as S, time
cap=S.Cap(sys.argv[1]); d=np.load(sys.argv[2]); T=d['T']; nch=int(sys.argv[3]) if len(sys.argv)>3 else 12
off=(3570.24e6-cap.c*1e6)-30e3
ys=[np.stack([cc.band(cap,c,off,ch) for ch in range(4)]) for c in range(nch)]
taus=np.arange(-12,13,1.0)
kk=np.arange(240)
def layout(pci):
    v=pci%4; p1=kk[(kk%4)==v]; p2=p1[(p1<48)|(p1>=192)]; return v,p1,p2
t=time.time(); score=np.zeros((8,1008,len(taus)))
# per (c,i) FFT'd symbols 1..3 at the PSS body position
Yf={}
for i in range(8):
    s=cc.ssb_sym(i); p0=cc.body76(s//14,s%14)
    for c in range(nch):
        p=T[c]+p0
        Yf[(c,i)]=[np.fft.fft(ys[c][:,p+sy*274:p+sy*274+256],axis=1)/16.0 for sy in (1,2,3)]
for pci in range(1008):
    v,p1,p2=layout(pci)
    for i in range(8):
        R=S.dmrs(pci,i); parts=((0,p1,R[:60]),(1,p2,R[60:84]),(2,p1,R[84:144]))
        sc=np.zeros(len(taus))
        for c in range(nch):
            for si,pos,r in parts:
                Y=Yf[(c,i)][si][:,(pos-119)%256]                      # (4,npos)
                ph=np.exp(-2j*np.pi*taus[:,None]*(pos-119)[None,:]/256)  # (nt,npos)
                z=(Y[:,None,:]*ph[None,:,:]*np.conj(r)[None,None,:]).sum(2)  # (4,nt)
                sc+=(np.abs(z)**2).sum(0)
        score[i,pci]=sc
print('done',time.time()-t)
np.save(sys.argv[2].replace('.npz','_ssbcensus.npy'),score)
best=score.max(2)            # (8,1008)
med=np.median(best,axis=1,keepdims=True)
rat=best/med
idx=np.dstack(np.unravel_index(np.argsort(-rat,axis=None)[:25],rat.shape))[0]
for i,p in idx: print('i_ssb',i,'PCI',p,'dB over median',round(10*np.log10(rat[i,p]),1),'tau',taus[score[i,p].argmax()])
