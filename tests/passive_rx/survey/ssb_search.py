#!/usr/bin/env python3
"""ssb_search.py CAPROOT : n78 sync-raster (30 kHz SCS) PSS search + SSS -> PCI on survey captures (ch0, first 120 ms).
GSCN freq = 3000 MHz + N*1.44 MHz (N=0..2499, GSCN=7499+N); SS_REF = SSB subcarrier 120, PSS/SSS centre = subcarrier 119."""
import numpy as np, glob, sys
from scipy.signal import resample_poly, fftconvolve
FS=122.88e6; D=16; fs=FS/D; NF=256; CP=18
def mseq(taps,init,n=127):
    x=list(init)+[0]*(n)
    for i in range(n): x[i+7]=(x[i+taps]+x[i])%2
    return np.array(x[:n])
xp=mseq(4,[0,1,1,0,1,1,1]); x0=mseq(4,[1,0,0,0,0,0,0]); x1=mseq(1,[1,0,0,0,0,0,0])
def pss(n2): return 1-2*xp[(np.arange(127)+43*n2)%127]
def sss(n1,n2):
    m0=15*(n1//112)+5*n2; m1=n1%112; n=np.arange(127)
    return (1-2*x0[(n+m0)%127])*(1-2*x1[(n+m1)%127])
def sym(seq):  # 127 sc centred on bin 0 -> time (NF), bins -63..63
    X=np.zeros(NF,complex); X[np.arange(-63,64)%NF]=seq; return np.fft.ifft(X)*np.sqrt(NF)
def fsym(y):   # FFT window -> 127 bins
    return np.fft.fft(y)[np.arange(-63,64)%NF]/np.sqrt(NF)
PT=[sym(pss(i)) for i in range(3)]
SS=np.array([[sss(a,b) for a in range(336)] for b in range(3)])  # [n2][n1][127]
def search(path,c_mhz,n=int(0.12*FS)):
    raw=np.fromfile(path,dtype=np.int16,count=2*n).astype(np.float32).reshape(-1,2); x=raw[:,0]+1j*raw[:,1]
    t=np.arange(len(x))/FS; out=[]
    N0=int(np.ceil(((c_mhz-61.44*0.82)-3000)/1.44)); N1=int(((c_mhz+61.44*0.82)-3000)/1.44)
    for N in range(N0,N1+1):
        fg=3000+N*1.44; off=(fg-c_mhz)*1e6
        y=resample_poly(x*np.exp(-2j*np.pi*(off-30e3)*t),1,D)  # PSS centre = GSCN - 30 kHz  -> bin 0
        pw=np.mean(np.abs(y)**2)
        best=None
        for n2 in range(3):
            cc=np.abs(fftconvolve(y,np.conj(PT[n2][::-1]),'valid'))**2/NF/ (pw+1e-12)
            p=int(np.argmax(cc)); med=np.median(cc); m=cc[p]/(med+1e-12)
            if best is None or m>best[0]: best=(m,n2,p,cc[p])
        m,n2,p,_=best
        pci=None;sm=0
        if m>20:
            # SSS two symbols later; also try neighbouring timing +-2
            s0=None
            for dp in range(-2,3):
                q=p+dp+2*(NF+CP)
                if q+NF>len(y):continue
                Z=fsym(y[q:q+NF]); Hp=fsym(y[p+dp:p+dp+NF])*np.conj(pss(n2))  # channel from PSS
                Zeq=Z*np.conj(Hp)
                sc=np.abs(SS[n2]@np.conj(Zeq))  # [336]
                if s0 is None or sc.max()>s0[0]: s0=(sc.max(),int(sc.argmax()),np.sort(sc)[-2])
            sm=s0[0]/(s0[2]+1e-12); pci=3*s0[1]+n2
        out.append((fg,7499+N,m,n2,p,pci,sm,10*np.log10(pw+1e-9)))
    return out
if __name__=="__main__":
    for d in sorted(glob.glob(sys.argv[1]+'/c*/')):
        c=int(d.rstrip('/').split('/c')[-1])
        for r in search(d+'rx0.sc16',c):
            if r[2]>12: print("cap %d  f=%.2f MHz GSCN=%d PSSpeak/med=%.1f N2=%d pos=%d PCI=%s SSSratio=%.2f"%(c,r[0],r[1],r[2],r[3],r[4],r[5],r[6]))
