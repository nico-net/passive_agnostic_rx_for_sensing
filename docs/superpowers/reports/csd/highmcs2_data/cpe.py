import sys,re,collections,statistics as st
rows=collections.defaultdict(list)
for fn in sys.argv[1:]:
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if d.get('probe')!='0' or 'snr_cpe' not in d: continue
        snr=float(d['snr']); c=float(d['snr_cpe']); nl=int(d['Nl']); q=int(d['Qm'])
        b=int(snr//4)*4
        rows[(nl,q,b,int(d['crc']))].append(c-snr)
for k in sorted(rows):
    v=rows[k]
    if len(v)>=10: print(k,len(v),'mean %.2f p90 %.2f'%(st.mean(v),sorted(v)[int(.9*len(v))]))
