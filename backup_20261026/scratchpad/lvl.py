import gzip,json,sys,numpy as np,collections
runs=sys.argv[2:]; mode=sys.argv[1]
def fl(s): return [float(x) for x in s.split(',')]
B=collections.defaultdict(list)
for r in runs:
  for l in gzip.open('out/'+r+'.full.gz','rt'):
    d=json.loads(l)
    try:
        w=int(d['rb'].split('+')[1]); m=int(d['mcs'])
        if d['Nl']!=mode.split(',')[0] or not 24<=m<=27: continue
        if mode.split(',')[1]=='wide' and w<129: continue
        if mode.split(',')[1]=='narrow' and w>32: continue
        dm=fl(d['dmrs_snr']); s=float(d['snr'])
        if dm[0]<-50 or s>60: continue
        B[int(float(d['dmrs_pw'])//1)].append((int(d['crc']),s,np.mean(dm[:4]),dm[4],float(d['kappa']),np.mean(fl(d['nvar_db'])),float(d['snr_cpe'])))
    except Exception as e: pass
print('dmrs_pw  n  crc  snr_med  dm_mean  dm_comb  kappa  nvar  cpe_gap')
for k in sorted(B):
    v=np.array(B[k])
    if len(v)<150: continue
    print('%4d %6d %.2f  %5.1f  %5.1f  %5.1f  %.3f %5.1f %4.1f'%(k,len(v),v[:,0].mean(),np.median(v[:,1]),np.median(v[:,2]),np.median(v[:,3]),v[:,4].mean(),np.median(v[:,5]),np.median(v[:,6]-v[:,1])))
