import sys,re,collections
tab=collections.defaultdict(lambda:[0,0])
for fn in sys.argv[2:]:
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if d.get('rnti')=='0xffff' or d.get('probe')!='0': continue
        try: snr=float(d['snr']); mcs=int(d['mcs']); nl=int(d['Nl']); crc=int(d['crc'])
        except: continue
        if mcs<11: continue
        sb=int(snr//3)*3
        if sb<13: continue
        k=(nl,mcs,sb,d.get('comb')); tab[k][0]+=crc; tab[k][1]+=1
for k in sorted(tab):
    ok,n=tab[k]
    if n>=3: print(k, f"{100*ok/n:5.1f}% ({n})")
