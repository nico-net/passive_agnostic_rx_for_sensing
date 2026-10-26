import gzip,sys,statistics as st,collections
def f(s):
    try: return float(s)
    except: return None
W=float(sys.argv[1]); L=int(sys.argv[2])
for r in sys.argv[3:]:
    dl=[l.rstrip('\n').split('\t') for l in gzip.open('out/'+r+'.dl.gz','rt')]
    dl=[x for x in dl if len(x)>=19 and x[3].isdigit() and int(x[4])==L and 24<=int(x[3])<=27 and f(x[0]) is not None and x[6].isdigit()]
    print('==',r,'L',L)
    # t is frame.slot -> seconds = frame*0.01 (10 ms frames)
    b=collections.defaultdict(list)
    for i,x in enumerate(dl): b[i//int(W)].append(x)
    for k in sorted(b):
        g=b[k]
        if len(g)<50: continue
        print(' c%5d n=%5d crc=%.2f snr=%.1f cpe=%.1f minl=%.1f dm=%s'%(k*int(W),len(g),sum(int(x[2]) for x in g)/len(g),st.median(f(x[8]) for x in g),st.median(f(x[9]) for x in g),st.median(min(f(y) for y in x[11].split(',')[:L]) for x in g), st.median(sum(f(y) for y in x[13].split(',')[:4])/4 for x in g)))
