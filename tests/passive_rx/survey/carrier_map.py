#!/usr/bin/env python3
"""carrier_map.py CAPROOT : stitched absolute-frequency PSD (2 MHz bins, dB above the noise floor, antenna 0) and SSB SNR of candidates."""
import numpy as np, glob, sys
sys.path.insert(0,__file__.rsplit('/',1)[0])
from ssb_search import *
FS=122.88e6
bins={}
for d in sorted(glob.glob(sys.argv[1]+'/c*/')):
    c=int(d.rstrip('/').split('/c')[-1])
    x=np.fromfile(d+'rx0.sc16',dtype=np.int16,count=2*16_000_000).astype(np.float32).reshape(-1,2); x=x[:,0]+1j*x[:,1]
    n=4096; w=np.hanning(n); m=len(x)//n
    P=np.mean(np.abs(np.fft.fftshift(np.fft.fft(x[:m*n].reshape(m,n)*w,axis=1),axes=1))**2,axis=0)
    f=c+(np.arange(n)-n//2)*FS/n/1e6
    for lo in np.arange(c-52,c+52,2.0):
        k=(f>=lo)&(f<lo+2); v=10*np.log10(P[k].mean()); key=int(lo)
        if key not in bins or abs(lo+1-c)<bins[key][1]: bins[key]=(v,abs(lo+1-c))
flo=np.percentile([v[0] for v in bins.values()],10)
print("noise floor (10th pct, arb dB) %.1f"%flo)
for k in sorted(bins): print("%d-%d MHz  +%4.1f dB %s"%(k,k+2,bins[k][0]-flo,'#'*int(max(0,bins[k][0]-flo)/1.5)))
cands=[(3426.24,'c3430'),(3515.52,'c3510'),(3624.96,'c3590'),(3679.68,'c3670'),(3731.52,'c3750'),(3760.32,'c3750')]
for fg,cap in cands:
    c=int(cap[1:]); n=int(0.3*FS)
    raw=np.fromfile(sys.argv[1]+'/%s/rx0.sc16'%cap,dtype=np.int16,count=2*n).astype(np.float32).reshape(-1,2); x=raw[:,0]+1j*raw[:,1]
    t=np.arange(len(x))/FS
    y=resample_poly(x*np.exp(-2j*np.pi*((fg-c)*1e6-30e3)*t),1,D)
    best=None
    for n2 in range(3):
        cc=np.abs(fftconvolve(y,np.conj(PT[n2][::-1]),'valid'))**2
        p=int(np.argmax(cc)); 
        if best is None or cc[p]>best[0]: best=(cc[p],n2,p)
    _,n2,p=best
    # PSS-symbol band power (127 sc) vs median per-window power of the same band (noise/other-time estimate)
    Xp=fsym(y[p:p+NF]); sig=np.mean(np.abs(Xp)**2)
    idx=np.arange(0,len(y)-NF,NF*3); nz=np.median([np.mean(np.abs(fsym(y[i:i+NF]))**2) for i in idx])
    print("GSCN f=%.2f MHz (cap %s) PSS-sym power / median window power = %.1f dB"%(fg,cap,10*np.log10(sig/nz)))
