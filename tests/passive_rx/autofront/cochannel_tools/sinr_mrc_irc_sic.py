import numpy as np, sys
d=np.load(sys.argv[1]); H=d['H']; RES=d['RES']; pcis=list(d['pcis']); C=len(pcis); nre=96
sig2=10**7.2/4
def sinr(h,Rint,hh_c): 
    mrc=hh_c*np.vdot(h,h).real/np.vdot(h,Rint@h).real
    irc=hh_c/np.vdot(h,h).real*np.vdot(h,np.linalg.solve(Rint+1e-3*np.eye(4)*np.trace(Rint).real/4,h)).real
    return mrc,irc
print('PCI slot  n  #interf | SNRonly  MRC  IRC  IRC+SIC(strongest removed)  unused | sv ratio of [h_all] (sigma_min/sigma_max dB)')
for n in (11,12,16,17):
  for ci,pc in enumerate(pcis):
    R=[]; 
    for c in range(1,48,2):
      for l in (3,7,11):
        res=RES[c,n,l,0]; bias=res.sum()/nre
        pw=[(np.abs(H[c,n,l,0,j])**2).sum() for j in range(C)]
        pres=[(p-bias)/bias>4 for p in pw]
        if not pres[ci]: continue
        h=H[c,n,l,0,ci]; hh_c=max(pw[ci]-bias,1e-3)
        oth=[j for j in range(C) if j!=ci and pres[j]]
        Rn=np.diag(res).astype(complex)
        def Rint(js):
            R_=Rn.copy()
            for j in js: R_+=np.outer(H[c,n,l,0,j],H[c,n,l,0,j].conj())
            return (R_+R_.conj().T)/2
        m,i=sinr(h,Rint(oth),hh_c)
        strongest=max(oth,key=lambda j:pw[j]) if oth else None
        m2,i2=sinr(h,Rint([j for j in oth if j!=strongest]),hh_c)
        Hm=np.array([H[c,n,l,0,j] for j in [ci]+oth]); sv=np.linalg.svd(Hm,compute_uv=False)
        thermal=np.diag(np.full(4,max(res.mean()*0+sig2,1.0))).astype(complex)
        R.append((hh_c/sig2,m,i,i2,hh_c/np.vdot(h,np.eye(4)*sig2@h).real*np.vdot(h,h).real/sig2*0+hh_c/sig2,len(oth),10*np.log10(sv[-1]/sv[0]) if len(sv)==4 else np.nan))
    if len(R)>=int(sys.argv[2]) if len(sys.argv)>2 else len(R)>=6:
      a=np.array(R); db=lambda x:10*np.log10(np.mean(x))
      print(f'{pc:4d} {n:3d} {len(R):3d}  {a[:,5].mean():.1f}   | {db(a[:,0]):5.1f} {db(a[:,1]):5.1f} {db(a[:,2]):5.1f}   {db(a[:,3]):5.1f}   {db(a[:,0]):5.1f}  | {np.nanmean(a[:,6]):.1f}')
