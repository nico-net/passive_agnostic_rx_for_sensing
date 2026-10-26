import gzip,sys,numpy as np,collections
def f(s):
    try: return float(s)
    except: return None
L=int(sys.argv[1]); runs=sys.argv[2:]
for wname,lo,hi in (('wide>128',129,999),('33-128',33,128)):
    X=[]
    for r in runs:
        for l in gzip.open('out/'+r+'.dl.gz','rt'):
            x=l.rstrip('\n').split('\t')
            if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or not (24<=int(x[3])<=27) or not x[6].isdigit() or not lo<=int(x[6])<=hi: continue
            s=f(x[8]); c=f(x[9])
            if s is None or c is None or s>60 or c>60: continue
            X.append((int(x[2]),s,c))
    X=np.array(X); 
    bins=np.floor(X[:,1]).astype(int); tab=collections.defaultdict(list)
    for b,y in zip(bins,X[:,0]): tab[b].append(y)
    f_={b:np.mean(v) for b,v in tab.items() if len(v)>=30}
    def F(s):
        b=int(np.floor(s)); 
        if b in f_: return f_[b]
        return 1.0 if b>max(f_) else 0.0
    base=np.mean([F(s) for s in X[:,1]]); cpe=np.mean([F(c) for c in X[:,2]])
    print('rank',L,wname,'n=%d actual CRC %.3f  model(snr) %.3f  model(snr_cpe, if CPE removed in full) %.3f'%(len(X),X[:,0].mean(),base,cpe))
