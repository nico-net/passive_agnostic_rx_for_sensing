import gzip,sys,glob,statistics as st,collections
S='out/'
def f(s):
    try: return float(s)
    except: return None
runs=sys.argv[2:]; L=int(sys.argv[1])
for r in runs:
    dl=[l.rstrip('\n').split('\t') for l in gzip.open(S+r+'.dl.gz','rt')]
    dl=[x for x in dl if len(x)>=19 and x[3].isdigit() and int(x[4])==L and 24<=int(x[3])<=27 and x[6].isdigit()]
    print('==',r,len(dl))
    for nm,lo,hi in (('n<=32',1,32),('33-128',33,128),('>128',129,999)):
        g=[x for x in dl if lo<=int(x[6])<=hi]
        if not g: continue
        snr=[f(x[8]) for x in g]; cpe=[f(x[9]) for x in g]
        ml=[min([f(y) for y in x[11].split(',')[:L]]) for x in g]
        print(' %-7s n=%6d crc=%.3f snr_med=%.1f cpe=%.1f minlayer=%.1f'%(nm,len(g),sum(int(x[2]) for x in g)/len(g),st.median(snr),st.median(cpe),st.median(ml)))
        # crc vs min-layer snr bins of 1 dB
        b=collections.defaultdict(list)
        for x,m in zip(g,ml): b[int(m//1)].append(int(x[2]))
        print('    ',' '.join('%d:%.2f(%d)'%(k,sum(v)/len(v),len(v)) for k,v in sorted(b.items()) if len(v)>=30))
