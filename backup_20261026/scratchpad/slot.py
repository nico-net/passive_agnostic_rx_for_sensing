import gzip,sys,statistics as st,collections
def f(s):
    try: return float(s)
    except: return None
L=int(sys.argv[1]); mode=sys.argv[2]
for r in sys.argv[3:]:
    g=collections.defaultdict(list)
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or not (24<=int(x[3])<=27) or not x[6].isdigit(): continue
        w=int(x[6])
        if mode=='wide' and w<=128: continue
        if mode=='narrow' and w>32: continue
        try: sl=int(x[0].split('.')[1])
        except: continue
        d=[f(y) for y in x[13].split(',')[:4]]
        g[sl].append((int(x[2]),f(x[8]),sum(d)/4 if None not in d else None))
    print('==',r,L,mode)
    print(' '.join('s%d:%.2f/%.1f/%.1f(%d)'%(s,sum(a[0] for a in v)/len(v),st.median(a[1] for a in v),st.median(a[2] for a in v if a[2] is not None and a[2]>-50) if any(a[2] is not None and a[2]>-50 for a in v) else 0,len(v)) for s,v in sorted(g.items()) if len(v)>=100))
