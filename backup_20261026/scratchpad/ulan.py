import gzip,sys,statistics as st,collections
def f(s):
    try: return float(s)
    except: return None
for r in sys.argv[1:]:
    g=collections.defaultdict(list)
    for l in gzip.open('out/'+r+'.ul.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<10 or not x[4].isdigit(): continue
        w=int(x[4]); wb='<=8' if w<=8 else '9-64' if w<=64 else '>64'
        g[wb].append((int(x[1]),f(x[5]),x[6],x[9]))
    print('==',r)
    for wb in ['<=8','9-64','>64']:
        v=g.get(wb)
        if not v: continue
        sn=[a[1] for a in v if a[1] is not None and a[1]>-29 and a[3]!='1']
        stc=collections.Counter(a[2] for a in v)
        print('  %-5s n=%6d crc=%.2f skip=%.2f snr_med(nonskip)=%s status=%s'%(wb,len(v),sum(a[0] for a in v)/len(v),sum(1 for a in v if a[3]=='1')/len(v),'%.1f'%st.median(sn) if sn else '-',dict(stc.most_common(4))))
