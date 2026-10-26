import gzip,sys,collections,statistics as st
def f(s):
    try: return float(s)
    except: return None
L=int(sys.argv[1]); runs=sys.argv[2:]
B=collections.defaultdict(lambda:collections.defaultdict(list))
for r in runs:
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or not (24<=int(x[3])<=27) or not x[6].isdigit(): continue
        if int(x[6])<129: continue
        s=f(x[8]); c=f(x[9])
        if s is None or c is None or s>60 or c>60: continue
        gap=c-s
        B[int(s//1)]['lo' if gap<1.2 else 'hi'].append(int(x[2]))
print('snr_bin  CRC(gap<1.2dB) n | CRC(gap>=1.2dB) n')
for k in sorted(B):
    lo=B[k]['lo']; hi=B[k]['hi']
    if len(lo)>=100 and len(hi)>=100: print('%3d  %.2f %6d | %.2f %6d'%(k,sum(lo)/len(lo),len(lo),sum(hi)/len(hi),len(hi)))
