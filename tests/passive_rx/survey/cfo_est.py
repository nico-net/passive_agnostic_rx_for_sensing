#!/usr/bin/env python3
"""cfo_est.py CAPDIR CAPCENTER_MHZ GSCN_MHZ PCI : CFO from the PSS->SSS channel-phase slope (symbols 0 and 2, 71 us apart,
unambiguous +-7 kHz), averaged over all SSB bursts in 0.3 s. Result in Hz of the carrier offset seen at the capture (positive = cell above nominal)."""
import numpy as np, sys
sys.path.insert(0,__file__.rsplit('/',1)[0])
from ssb_search import *
d,c,fg,pci=sys.argv[1],float(sys.argv[2]),float(sys.argv[3]),int(sys.argv[4])
n1,n2=pci//3,pci%3
n=int(0.3*FS); raw=np.fromfile(d+'/rx0.sc16',dtype=np.int16,count=2*n).astype(np.float32).reshape(-1,2); x=raw[:,0]+1j*raw[:,1]; t=np.arange(len(x))/FS
y=resample_poly(x*np.exp(-2j*np.pi*((fg-c)*1e6-30e3)*t),1,D)
cc=np.abs(fftconvolve(y,np.conj(PT[n2][::-1]),'valid'))**2
P=153600; p0=int(np.argmax(cc[:P])); 
# refine position of the global peak mod P
cand=[int(np.argmax(cc[k*P:(k+1)*P])) for k in range(len(cc)//P)]
p0=int(np.median(cand)); print("PSS offset mod 20ms:",p0,"spread",np.ptp(cand))
acc=0; cnt=0; sd=SS[n2][n1]
for k in range(len(y)//P-1):
    p=k*P+p0; q=p+2*(NF+CP)
    if q+NF>len(y): break
    Hp=fsym(y[p:p+NF])*np.conj(pss(n2)); Hs=fsym(y[q:q+NF])*np.conj(sd)
    acc+=np.sum(Hs*np.conj(Hp)); cnt+=1
dt=2*(NF+CP)/fs
print("bursts",cnt,"CFO (Hz, signal above the tuned centre) = %.0f"%(np.angle(acc)/(2*np.pi*dt)))
