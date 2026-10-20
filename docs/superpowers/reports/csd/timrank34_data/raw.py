import numpy as np, struct, functools
HN=['rnti','frame','slot','nl','ports','cdm','dmrs_type','dmrs_pos','S','L','rb0','nrb','bwp0','mcs','tbl','nscid','dmrs_id','data_id',
    'dci_len','w0lo','w0hi','w1lo','w1hi','N','fco','nrx','prg','nprbl','sc0','rv','pid','ndi']
def read(path):
    f=open(path,'rb'); out=[]
    while True:
        m=f.read(4)
        if len(m)<4: break
        assert m==b'PRW1', m
        h=dict(zip(HN,struct.unpack('<32i',f.read(128))))
        nsc=h['nrb']*12
        a=np.frombuffer(f.read(4*h['nrx']*14*nsc),dtype=np.int16).astype(np.float32)
        y=(a[0::2]+1j*a[1::2]).reshape(h['nrx'],14,nsc)
        h['y']=y; out.append(h)
    return out
@functools.lru_cache(maxsize=None)
def gold(cinit, n):
    Nc=1600; L=Nc+n+31
    x1=np.zeros(L,np.uint8); x2=np.zeros(L,np.uint8); x1[0]=1
    for i in range(31): x2[i]=(cinit>>i)&1
    for i in range(L-31):
        x1[i+31]=x1[i+3]^x1[i]; x2[i+31]=x2[i+3]^x2[i+2]^x2[i+1]^x2[i]
    return (x1[Nc:Nc+n]^x2[Nc:Nc+n]).copy()
def dmrs_seq(nid, nscid, slot, sym, nmax, lam=0, rel16=False):
    """r(m) m=0..nmax-1 ; rel16 adds 2^17*floor(lam/2) (only matters lam=2)"""
    ci=((1<<17)*(14*slot+sym+1)*(2*nid+1) + (2*nid) + nscid + ((1<<17)*(lam//2) if rel16 else 0)) % (1<<31)
    c=gold(ci, 2*nmax).astype(np.float32)
    return ((1-2*c[0::2]) + 1j*(1-2*c[1::2]))/np.sqrt(2)
def ls_type1(h, sym, lam, nid, nscid, crb0=None):
    """LS z[a, m_local] for CDM group lam (delta=lam) on DM-RS symbol sym. returns z, kprime (0/1 per pilot), pilot subcarrier idx"""
    y=h['y']; nsc=y.shape[2]
    c0=(h['bwp0']+h['rb0']) if crb0 is None else crb0
    kabs=12*c0+np.arange(nsc)
    sel=((kabs-lam)%2==0)
    m=(kabs[sel]-lam)//2
    r=dmrs_seq(nid,nscid,h['slot'],sym,int(m.max())+1)[m]
    z=y[:,sym,sel]*np.conj(r)[None,:]
    return z, (m%2), np.nonzero(sel)[0]
def port_est(z, kp, occ):
    """pairs (k'=0,k'=1) adjacent: occ=+1 -> (z0+z1)/2, occ=-1 -> (z0-z1)/2. assumes pilots start at k'=0"""
    i0=np.nonzero(kp==0)[0]; i0=i0[i0+1<z.shape[1]]
    return (z[:,i0]+occ*z[:,i0+1])/2
def coh_pow(hp):
    """noise-unbiased power: |mean h_n conj h_{n+1}| over adjacent pair estimates, averaged over antennas"""
    return float(np.mean(np.abs(np.mean(hp[:,:-1]*np.conj(hp[:,1:]),axis=1))))
def raw_pow(hp): return float(np.mean(np.abs(hp)**2))
def ports_of(h):
    return [p for p in range(8) if (h['ports']>>p)&1]
def chan(h, sym, nid=None, nscid=None, ports=None, smooth=3):
    """per-port channel per pair (each pair = 4 subcarriers = 1/3 PRB), smoothed over `smooth` pairs (boxcar), plus noise var per RE.
    returns H[a, p, npair], sigma2 (per RE, per antenna)"""
    nid=h['dmrs_id'] if nid is None else nid; nscid=h['nscid'] if nscid is None else nscid
    ports=ports_of(h) if ports is None else ports
    Hs=[]; res=[]
    for p in ports:
        lam=(p%4)//2; occ=1 if p%2==0 else -1
        z,kp,_=ls_type1(h,sym,lam,nid,nscid)
        hp=port_est(z,kp,occ)
        Hs.append(hp)
    n=min(x.shape[1] for x in Hs)
    H=np.stack([x[:,:n] for x in Hs],axis=1)
    # noise: from pair difference of the OCC-orthogonal unused combination is not available (ports used); use 2nd difference
    d2=H[:,:,1:-1]-0.5*(H[:,:,:-2]+H[:,:,2:])
    s2_pair=np.mean(np.abs(d2)**2)/1.5   # var(d2)=1.5 var(pair est) for white noise
    sigma2=s2_pair*2                       # pair est = mean of 2 LS samples
    if smooth>1:
        k=np.ones(smooth)/smooth
        Hf=np.apply_along_axis(lambda v: np.convolve(v,k,mode='same'),2,H)
        # fix edges
        Hf[:,:,0]=H[:,:,:2].mean(2); Hf[:,:,-1]=H[:,:,-2:].mean(2)
        H=Hf
    return H, sigma2
def mmse_sinr(H, s2):
    """H[a,p,n] -> per-layer LMMSE SINR (linear) per n"""
    A,P,Nn=H.shape; out=np.zeros((P,Nn))
    for n in range(Nn):
        Hn=H[:,:,n]
        G=Hn.conj().T@Hn
        M=np.linalg.inv(G/s2+np.eye(P))
        out[:,n]=1/np.real(np.diag(M))-1
    return out
def dmrs_syms(h): return [m for m in range(14) if (h['dmrs_pos']>>m)&1]
def chan_full(h, sym, ports=None, nid=None, nscid=None, smooth=3, occ_override=None):
    """per-subcarrier channel H[a,p,k] (linear interp of smoothed pair estimates) and sigma2 per RE"""
    nid=h['dmrs_id'] if nid is None else nid; nscid=h['nscid'] if nscid is None else nscid
    ports=ports_of(h) if ports is None else ports
    nsc=h['y'].shape[2]; A=h['y'].shape[0]
    out=np.zeros((A,len(ports),nsc),complex); s2s=[]
    for j,p in enumerate(ports):
        lam=(p%4)//2; occ=1 if p%2==0 else -1
        if occ_override is not None: occ=occ_override[j]
        z,kp,pos=ls_type1(h,sym,lam,nid,nscid)
        i0=np.nonzero(kp==0)[0]; i0=i0[i0+1<z.shape[1]]
        hp=(z[:,i0]+occ*z[:,i0+1])/2
        ctr=(pos[i0]+pos[i0+1])/2.0
        d2=hp[:,1:-1]-0.5*(hp[:,:-2]+hp[:,2:]); s2s.append(np.mean(np.abs(d2)**2)/1.5*2)
        if smooth>1:
            k=np.ones(smooth)/smooth
            hs=np.array([np.convolve(v,k,mode='same') for v in hp]); hs[:,0]=hp[:,:2].mean(1); hs[:,-1]=hp[:,-2:].mean(1); hp=hs
        for a in range(A):
            out[a,j]=np.interp(np.arange(nsc),ctr,hp[a].real)+1j*np.interp(np.arange(nsc),ctr,hp[a].imag)
    return out, float(np.mean(s2s))
def equalise(h, ports=None, nid=None, nscid=None, smooth=3, mode='mmse', tinterp=True, occ_override=None, data_syms=None):
    ds=dmrs_syms(h); y=h['y']; A,_,nsc=y.shape
    Hd={s:chan_full(h,s,ports,nid,nscid,smooth,occ_override) for s in ds}
    s2=np.mean([v[1] for v in Hd.values()])
    if data_syms is None:
        data_syms=[m for m in range(h['S'],h['S']+h['L']) if m not in ds]
    X=[]; P=None
    for m in data_syms:
        if tinterp and len(ds)>=2:
            a,b=ds[0],ds[-1]; t=(m-a)/(b-a); H=(1-t)*Hd[a][0]+t*Hd[b][0]
        else:
            prev=[s for s in ds if s<=m]; H=Hd[prev[-1] if prev else ds[0]][0]
        P=H.shape[1]
        xs=np.zeros((P,nsc),complex)
        for k in range(nsc):
            Hk=H[:,:,k]; G=Hk.conj().T@Hk
            if mode=='mmse':
                W=np.linalg.solve(G+s2*np.eye(P),Hk.conj().T)
                x=W@y[:,m,k]; bias=np.real(np.diag(W@Hk)); x=x/bias
            else:
                x=np.linalg.solve(G,Hk.conj().T@y[:,m,k])
            xs[:,k]=x
        X.append(xs)
    return np.stack(X,axis=1), s2   # [layer, sym, k]
def dd_snr_qpsk(x):
    x=x/np.sqrt(np.mean(np.abs(x)**2))
    d=(np.sign(x.real)+1j*np.sign(x.imag))/np.sqrt(2)
    e=np.mean(np.abs(x-d)**2); return 10*np.log10(1/e)
def scramble_seq(rnti, nid, n, q=0):
    return gold(((rnti<<15)+(q<<14)+nid)%(1<<31), n)
def llr_qpsk(X, s2eff=None):
    """X[layer,sym,k] -> codeword LLR float (layer-interleaved per RE, k fastest within symbol)"""
    L,S,K=X.shape
    sym=X.transpose(1,2,0).reshape(-1)  # order: sym, k, layer
    l=np.empty(2*sym.size); l[0::2]=sym.real; l[1::2]=sym.imag
    return l
def write_rec(f, rid, A, llr, Qm, Nl, BG, rv=0, lbrm=10**7, iters=8):
    llr=np.asarray(llr)
    sc=30.0/max(np.mean(np.abs(llr)),1e-9)
    q=np.clip(np.round(llr*sc),-127,127).astype('<i2')
    f.write(struct.pack('<12I',rid,A,llr.size,Qm,Nl,BG,rv,lbrm,iters,1,1,0)); f.write(q.tobytes())
def read_pdl(path):
    out=[]
    with open(path,'rb') as f:
        while (m:=f.read(4)):
            assert m==b'PDL1'
            v=struct.unpack('<14i',f.read(56))
            d=dict(zip(['rnti','frame','slot','qm','nl','rb0','nrb','S','L','mcs','nid','desc','G','tbl'],v))
            d['llr']=np.frombuffer(f.read(2*d['G']),dtype='<i2').astype(np.float32)
            out.append(d)
    return out
def equalise_q(h, scale=1/5.6, gain_k=0, exact=False, shift_delta=0, nbrx=4):
    """emulate receiver mode-2: G=(H^H H)>>shift, b=(H^H y)>>shift rounded int16 (unless exact), w=1/[G^-1]_ll, z=round(w x 2^gain_k)"""
    ds=dmrs_syms(h); y=h['y']*scale; A,_,nsc=y.shape
    Hd={s:chan_full(h,s)[0]*scale for s in ds}
    med=max(np.median(np.abs(Hd[ds[0]][a,p])**2) for a in range(A) for p in range(Hd[ds[0]].shape[1]))
    shift=int(np.floor(np.log2(max(med,1))))//2 + int(np.log2(nbrx>>1)) + shift_delta
    shift=max(shift,0)
    X=[]
    for m in [m for m in range(h['S'],h['S']+h['L']) if m not in ds]:
        a,b=ds[0],ds[-1]; t=(m-a)/(b-a); H=(1-t)*Hd[a]+t*Hd[b]
        H=np.round(H.real)+1j*np.round(H.imag); Y=np.round(y[:,m].real)+1j*np.round(y[:,m].imag)
        P=H.shape[1]; xs=np.zeros((P,nsc),complex)
        for k in range(nsc):
            Hk=H[:,:,k]; G=Hk.conj().T@Hk/2**shift; bb=Hk.conj().T@Y[:,k]/2**shift
            if not exact: G=np.round(G.real)+1j*np.round(G.imag); bb=np.round(bb.real)+1j*np.round(bb.imag)
            try: Gi=np.linalg.inv(G)
            except np.linalg.LinAlgError: continue
            x=Gi@bb; w=1/np.maximum(np.real(np.diag(Gi)),1e-12)
            z=w*x*2**gain_k; xs[:,k]=np.round(z.real)+1j*np.round(z.imag)
        X.append(xs)
    return np.stack(X,axis=1), shift
T2=[(2,1200),(2,1930),(2,3080),(2,4490),(2,6020),(4,3780),(4,4340),(4,4900),(4,5530),(4,6160),(4,6580),(6,4660),(6,5170),(6,5670),(6,6160),(6,6660),(6,7190),(6,7720),(6,8220),(6,8730),(8,6825),(8,7110),(8,7540),(8,7970),(8,8410),(8,8850),(8,9165),(8,9480)]
TBSTAB=[24,32,40,48,56,64,72,80,88,96,104,112,120,128,136,144,152,160,168,176,184,192,208,224,240,256,272,288,304,320,336,352,368,384,408,432,456,480,504,528,552,576,608,640,672,704,736,768,808,848,888,928,984,1032,1064,1128,1160,1192,1224,1256,1288,1320,1352,1416,1480,1544,1608,1672,1736,1800,1864,1928,2024,2088,2152,2216,2280,2408,2472,2536,2600,2664,2728,2792,2856,2976,3104,3240,3368,3496,3624,3752,3824]
import math
def tbs(h, nre_prb=None):
    qm,r10=T2[h['mcs']]; R=r10/10240.0
    nd=bin(h['dmrs_pos']).count('1')
    if nre_prb is None: nre_prb=min(156,12*h['L']-12*nd)
    ninfo=nre_prb*h['nrb']*R*qm*h['nl']
    if ninfo<=3824:
        n=max(3,int(math.floor(math.log2(ninfo)))-6); ni=max(24,(2**n)*int(ninfo//2**n))
        A=next(v for v in TBSTAB if v>=ni)
    else:
        n=int(math.floor(math.log2(ninfo-24)))-5; ni=max(3840,(2**n)*round((ninfo-24)/2**n))
        if R<=0.25: C=math.ceil((ni+24)/3816); A=8*C*math.ceil((ni+24)/(8*C))-24
        elif ni>8424: C=math.ceil((ni+24)/8424); A=8*C*math.ceil((ni+24)/(8*C))-24
        else: A=8*math.ceil((ni+24)/8)-24
    BG=2 if (A<=292 or (A<=3824 and R<=0.67) or R<=0.25) else 1
    return A,qm,BG
def llr_qam(X, qm):
    L,S,K=X.shape; s=X.transpose(1,2,0).reshape(-1)
    if qm==2:
        l=np.empty(2*s.size); l[0::2]=s.real; l[1::2]=s.imag; return l
    if qm==4:
        a=2/np.sqrt(10); l=np.empty(4*s.size)
        l[0::4]=s.real; l[1::4]=s.imag; l[2::4]=a-np.abs(s.real); l[3::4]=a-np.abs(s.imag); return l
    if qm==6:
        a=4/np.sqrt(42); b=2/np.sqrt(42); l=np.empty(6*s.size)
        l[0::6]=s.real; l[1::6]=s.imag; l[2::6]=a-np.abs(s.real); l[3::6]=a-np.abs(s.imag)
        l[4::6]=b-np.abs(a-np.abs(s.real)); l[5::6]=b-np.abs(a-np.abs(s.imag)); return l
def equalise_cdm1(h, smooth=3):
    """1 CDM group: data also on the odd-comb REs of DM-RS symbols. returns list of per-symbol arrays [layer, nre] in RE order"""
    ds=dmrs_syms(h); y=h['y']; A,_,nsc=y.shape
    Hd={s:chan_full(h,s,smooth=smooth) for s in ds}
    s2=np.mean([v[1] for v in Hd.values()])
    kabs=12*(h['bwp0']+h['rb0'])+np.arange(nsc)
    out=[]
    for m in range(h['S'],h['S']+h['L']):
        a,b=ds[0],ds[-1]; t=(m-a)/(b-a); H=(1-t)*Hd[a][0]+t*Hd[b][0]
        ks=np.nonzero(kabs%2==1)[0] if m in ds else np.arange(nsc)
        P=H.shape[1]; xs=np.zeros((P,ks.size),complex)
        for j,k in enumerate(ks):
            Hk=H[:,:,k]; G=Hk.conj().T@Hk; W=np.linalg.solve(G+s2*np.eye(P),Hk.conj().T)
            x=W@y[:,m,k]; xs[:,j]=x/np.real(np.diag(W@Hk))
        out.append(xs)
    return out
def llr_list(xl, qm):
    return np.concatenate([llr_qam(x[:,None,:],qm) for x in xl])
def grant_capacity(h, smooth=3):
    """per-layer LMMSE SINR from estimated H and sigma2 (pilot-based), averaged as capacity: returns eff SINR dB per layer and overall, required SE per layer"""
    ds=dmrs_syms(h); Hd={s:chan_full(h,s,smooth=smooth) for s in ds}
    s2=np.mean([v[1] for v in Hd.values()])
    H=0.5*(Hd[ds[0]][0]+Hd[ds[-1]][0]); A,P,K=H.shape
    caps=np.zeros(P); 
    for k in range(K):
        G=H[:,:,k].conj().T@H[:,:,k]; M=np.linalg.inv(G/s2+np.eye(P)); sinr=1/np.real(np.diag(M))-1
        caps+=np.log2(1+np.maximum(sinr,0))
    caps/=K
    qm,r=T2[h['mcs']]; se=qm*r/10240.0
    return 10*np.log10(2**caps-1), 10*np.log10(2**np.mean(caps)-1), se, 10*np.log10(2**se-1)
