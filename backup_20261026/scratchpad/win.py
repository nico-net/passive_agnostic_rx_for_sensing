import gzip,json,sys,numpy as np
r=sys.argv[1]; W=int(sys.argv[2])
rows=[json.loads(l) for l in gzip.open('out/'+r+'.full.gz','rt')]
def fl(s): return [float(x) for x in s.split(',')]
sel=[]
for d in rows:
    try:
        w=int(d['rb'].split('+')[1]); m=int(d['mcs']); 
        if d['Nl']!='2' or w<129 or not 24<=m<=27: continue
        dm=fl(d['dmrs_snr']); 
        if dm[0]<-50: continue
        sel.append((int(d['crc']),float(d['snr']),dm[4],np.mean(dm[:4]),np.mean(fl(d['br_snr'])),float(d['kappa']),float(d['rho']),np.mean(fl(d['nvar_db'])),float(d['dmrs_pw']),m,int(d['rb'].split('+')[0]),float(d['evm']),float(d['snr_cpe']),int(d['pid']),float(d['snr_l'].split(',')[0])-float(d['snr_l'].split(',')[1])))
    except Exception as e: pass
a=np.array(sel); print(r,len(a))
names=['crc','snr','dmcomb','dmmean','brmean','kappa','rho','nvar','dmpw','mcs','rbstart','evm','cpe','pid','layergap']
print(' '.join('%7s'%n for n in names))
for i in range(0,len(a),W):
    b=a[i:i+W]
    if len(b)<W//2: continue
    print(' '.join('%7.2f'%v for v in b.mean(0)), ' snr_med %.1f'%np.median(b[:,1]))
print('corr with snr:',{n:round(float(np.corrcoef(a[:,1],a[:,k])[0,1]),2) for k,n in enumerate(names) if k!=1})
