import re,sys,pickle
def parse(path):
    rows=[]
    ansi=re.compile(r'\x1b\[[0-9;]*m')
    for l in open(path,errors='ignore'):
        if 'EVMGRANT' not in l: continue
        l=ansi.sub('',l)
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        m=re.search(r'EVMGRANT rnti=(\S+) (\d+)\.(\d+)',l)
        if not m: continue
        try:
            r=dict(key=(m.group(1),int(m.group(2)),int(m.group(3))),rnti=m.group(1),probe=int(d.get('probe',0)),crc=int(d['crc']),mcs=int(d['mcs']),tbl=int(d['tbl']),Qm=int(d['Qm']),Nl=int(d['Nl']),
              tbs=int(d['tbs']),snr=float(d['snr']),it=int(d['it']),itc=int(d.get('itc',0)),rv=int(d['rv']),cb_ok=int(d['cb_ok']),cb_tried=int(d['cb_tried']),cb_total=int(d['cb_total']),
              evm=float(d['evm']),nsym=int(d['nsym']),dec_us=int(d['dec_us']))
            dm=[float(x) for x in d['dmrs_snr'].split(',')]; r['dmrs']=dm[-1]; 
            e=[int(x) for x in d['e2e'].split(',')] if 'e2e' in d else None
            r['e2e']=e; r['core']=int(d.get('core',-1))
            r['gq']=[int(x) for x in d['gq'].split(',')] if 'gq' in d else None
            r['snr_l']=[float(x) for x in d['snr_l'].split(',')]
            r['S']=d.get('S')
        except Exception as ex: continue
        rows.append(r)
    return rows
if __name__=='__main__':
    rows=parse(sys.argv[1]); pickle.dump(rows,open(sys.argv[2],'wb')); print(len(rows))
