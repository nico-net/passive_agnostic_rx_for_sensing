import gzip,sys,numpy as np
def f(s):
    try: return float(s)
    except: return None
def auc(score,y):
    o=np.argsort(score); r=np.empty(len(o)); r[o]=np.arange(1,len(o)+1)
    n1=y.sum(); n0=len(y)-n1
    return (r[y==1].sum()-n1*(n1+1)/2)/(n1*n0)
L=int(sys.argv[1]); runs=sys.argv[2:]
X=[]
for r in runs:
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or not (24<=int(x[3])<=27) or not x[6].isdigit() or int(x[6])<129: continue
        s=f(x[8]); c=f(x[9]); d=[f(y) for y in x[13].split(',')]; b=[f(y) for y in x[10].split(',')]; sl=[f(y) for y in x[11].split(',')[:L]]
        if s is None or c is None or s>60 or c>60 or None in d or d[0]<-50 or None in b or None in sl: continue
        X.append((int(x[2]),s,c,np.mean(d[:4]),d[4],min(b),min(sl),f(x[14]) or 0,f(x[18]) or 0))
X=np.array(X); y=X[:,0]
print(len(X),'crc',y.mean())
for k,n in enumerate(['snr','snr_cpe','dm_mean','dm_comb','min_br_snr','min_layer_snr','rho','evm'],1):
    print('%-14s AUC=%.3f'%(n,auc(X[:,k],y)))
print('snr-vs-dm_comb corr',np.corrcoef(X[:,1],X[:,4])[0,1],' gap(cpe-snr) vs dm_comb',np.corrcoef(X[:,2]-X[:,1],X[:,4])[0,1])
