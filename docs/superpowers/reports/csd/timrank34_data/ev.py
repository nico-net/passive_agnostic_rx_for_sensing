import re,sys,math
def parse(path):
    out=[]
    for ln in open(path,errors='replace'):
        if 'EVMGRANT' not in ln: continue
        d={}
        m=re.search(r'rnti=(0x[0-9a-f]+) (\d+)\.(\d+)',ln)
        if not m: continue
        d['rnti']=m.group(1); d['f']=int(m.group(2)); d['s']=int(m.group(3))
        for k,v in re.findall(r' ([a-z_]+)=([^ \n]+)',ln):
            if k in d: continue
            d[k]=v
        d['Nl']=int(re.search(r'Nl=(\d)',ln).group(1)); d['Qm']=int(re.search(r'Qm=(\d)',ln).group(1))
        for k in ('crc','mcs','probe'): d[k]=int(d[k])
        for k in ('snr',): d[k]=float(d[k])
        d['br']=[float(x) for x in d['br_snr'].split(',')]
        d['mrc']=10*math.log10(sum(10**(x/10) for x in d['br']))
        out.append(d)
    return out
if __name__=='__main__':
    import collections
    for p in sys.argv[1:]:
        g=[d for d in parse(p) if d['probe']==0 and d['Nl']>=3]
        t=collections.defaultdict(lambda:[0,0])
        for d in g:
            b=d['mrc']; bb='<5' if b<5 else '5-10' if b<10 else '10-15' if b<15 else '15-20' if b<20 else '>=20'
            k=(d['Nl'],d['Qm'],bb); t[k][0]+=d['crc']; t[k][1]+=1
        print(p)
        for k in sorted(t): print('  Nl=%d Qm=%d mrcbound %s: %d/%d'%(k+tuple(t[k])))
