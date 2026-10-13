import re,sys,glob,collections
def parse(path):
    rows=[]
    for line in open(path,errors='replace'):
        if 'EVMGRANT' not in line: continue
        d={}
        for k,v in re.findall(r'(\w+)=([^\s]+)',line):
            d[k]=v
        try:
            r=dict(rnti=d['rnti'],crc=int(d['crc']),mcs=int(d['mcs']),nl=int(d['Nl']),qm=int(d['Qm']),
              snr=float(d['snr']),evm=float(d['evm']),comb=int(d['comb']),probe=int(d.get('probe',0)),
              br=[float(x) for x in d['br_snr'].split(',')],nvar=[float(x) for x in d['nvar_db'].split(',')],
              alpha=[float(x) for x in d['alpha'].split(',')],rho=float(d.get('rho',0)),kappa=float(d.get('kappa',0)),
              rb=d['rb'],tbs=int(d.get('tbs',0)),rv=int(d.get('rv',0)),l2=int(d.get('l2',-1)))
            m=re.search(r'rnti=\S+ (\d+)\.(\d+)',line); r['frame']=int(m.group(1)); r['slot']=int(m.group(2))
            rows.append(r)
        except Exception as e: pass
    return rows
def bucket(m): return '0-4' if m<5 else '5-9' if m<10 else '10-16' if m<17 else '17+'
