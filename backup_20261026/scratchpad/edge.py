import gzip,sys,statistics as st,collections
def f(s):
    try: return float(s)
    except: return None
L=int(sys.argv[1])
for r in sys.argv[2:]:
    g=collections.defaultdict(list)
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or not (24<=int(x[3])<=27) or not x[6].isdigit(): continue
        w=int(x[6]); s=int(x[5])
        if w>16: continue
        c=s+w/2
        b='lo(0-16)' if c<17 else 'mid(17-254)' if c<255 else 'hi(255-272)'
        d=[f(y) for y in x[13].split(',')[:4]]
        g[b].append((int(x[2]),f(x[8]),sum(d)/4 if None not in d and min(d)>-50 else None))
    print(r[:28],' | '.join('%s n=%d crc=%.2f snr=%.1f dm=%.1f'%(k,len(v),sum(a[0] for a in v)/len(v),st.median(a[1] for a in v if a[1]<60),st.median([a[2] for a in v if a[2] is not None] or [0])) for k,v in sorted(g.items()) if len(v)>=100))
