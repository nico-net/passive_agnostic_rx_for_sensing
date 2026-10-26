import gzip,sys,statistics as st,collections
def f(s):
    try: return float(s)
    except: return None
L=int(sys.argv[1])
for r in sys.argv[2:]:
    dl=[l.rstrip('\n').split('\t') for l in gzip.open('out/'+r+'.dl.gz','rt')]
    dl=[x for x in dl if len(x)>=19 and x[3].isdigit() and int(x[4])==L and x[6].isdigit()]
    print('==',r)
    for nm,lo,hi in (('n<=32',1,32),('33-128',33,128),('>128',129,999)):
        out=[]
        for m in range(20,28):
            g=[x for x in dl if int(x[3])==m and lo<=int(x[6])<=hi]
            if len(g)<100: continue
            out.append('m%d n%d crc%.2f snr%.1f'%(m,len(g),sum(int(x[2]) for x in g)/len(g),st.median(f(x[8]) for x in g)))
        print(' ',nm,' | '.join(out))
