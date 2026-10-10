import numpy as np, sys, glob
root=sys.argv[1]; fs=122.88e6
for d in sorted(glob.glob(root+'/c*/')):
    c=int(d.rstrip('/').split('/c')[-1])
    x=np.fromfile(d+'rx0.sc16',dtype=np.int16,count=2*8_000_000).astype(np.float32).reshape(-1,2)
    x=x[:,0]+1j*x[:,1]
    n=4096; w=np.hanning(n); m=len(x)//n
    P=np.mean(np.abs(np.fft.fftshift(np.fft.fft(x[:m*n].reshape(m,n)*w),axes=1))**2,axis=1) if False else np.mean(np.abs(np.fft.fftshift(np.fft.fft(x[:m*n].reshape(m,n)*w,axis=1),axes=1))**2,axis=0)
    f=(np.arange(n)-n//2)*fs/n/1e6
    db=10*np.log10(P+1e-9)
    print("center",c,"rms %.0f"%np.sqrt(np.mean(np.abs(x)**2)))
    # 5 MHz bins in +-50 MHz
    s=''
    for lo in range(-60,60,5):
        k=(f>=lo)&(f<lo+5); s+="%d:%.0f "%(c+lo+2.5,db[k].mean())
    print(s)
