import numpy as np, sys
d=np.load(sys.argv[1]); H=d['H']; RES=d['RES']; pcis=list(d['pcis'])
P=(np.abs(H)**2).sum(-1)          # (c,n,l,blk,cell)
res=RES.sum(-1)                   # (c,n,l,blk)  sum over rx of residual power per RE
bias=res[...,None]/18.0           # LS noise bias on sum_rx |h|^2 (single column)
L=[3,7,11]; nb=P.shape[3]
print('pcis',pcis)
# noise reference (thermal) from UL slots: PW in slot 4,5,14,15 
noise=d['PW'][:,[4,5,14,15]][:,:,[3,7,11]].mean()   # sum over rx? PW is per-rx -> sum
noise=(d['PW'][:,[4,5,14,15]][:,:,[3,7,11]].sum(-1)).mean()
print('thermal noise sum_rx per RE dB',round(10*np.log10(noise),1))
for n in (10,11,12,13,16,17):
    print('slot',n)
    for par,cs in (('odd',range(1,49,2)),('even',range(0,48,2))):
        p=P[list(cs)][:,n][:,L]; b=bias[list(cs)][:,n][:,L]; r=res[list(cs)][:,n][:,L]
        pm=(p-b).mean((1,2))                  # (chunk,cell) mean over l and blocks
        sd=(b.mean((1,2))/np.sqrt(len(L)*nb)) # per chunk
        z=pm/sd
        present=(z>5)
        tot=PW=d['PW'][list(cs)][:,n][:,L].sum(-1).mean((1,2))
        print(f'  {par}: total dB {10*np.log10(tot.mean()):5.1f}  resid dB {10*np.log10(r.mean()):5.1f} |', ' | '.join(f'{pc}: pres {present[:,i].mean():.2f} pow {10*np.log10(max(pm[present[:,i],i].mean() if present[:,i].any() else 1e-3,1e-3)):5.1f}dB' for i,pc in enumerate(pcis)))
