import sys,re,collections
def parse(fn):
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' not in l: continue
        d={}
        for m in re.finditer(r'(\w+)=(\S+)',l): d[m.group(1)]=m.group(2)
        yield d
def bucket(m):
    return '0-4' if m<=4 else '5-9' if m<=9 else '10-16' if m<=16 else '17-22' if m<=22 else '23+'
tab=collections.defaultdict(lambda:[0,0])
other=collections.Counter()
for fn in sys.argv[1:]:
    for d in parse(fn):
        if d.get('rnti')=='0xffff' or d.get('probe')!='0': continue
        try: snr=float(d['snr']); mcs=int(d['mcs']); nl=int(d['Nl']); crc=int(d['crc'])
        except: continue
        other[(d.get('tbl'),d.get('Qm'))]+=1
        sb='<10' if snr<10 else '10-16' if snr<16 else '16-22' if snr<22 else '>=22'
        k=(nl,bucket(mcs),sb); tab[k][0]+=crc; tab[k][1]+=1
print(other)
for k in sorted(tab): 
    ok,n=tab[k]; print(k, f"{100*ok/n:5.1f}% ({n})")
