import gzip,sys,math,collections,glob
runs=sys.argv[2:]; L=int(sys.argv[1])
agg=collections.defaultdict(lambda:[0,0,0.0,0])  # n, ok, sum cb, 
for r in runs:
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or not x[6].isdigit() or int(x[4])!=L: continue
        try: cb=int(x[15])
        except: continue
        m=int(x[3]); w=int(x[6])
        wb='n<=8' if w<=8 else 'n9-32' if w<=32 else '33-128' if w<=128 else '>128'
        mb='<=16' if m<=16 else '17-20' if m<=20 else '21-23' if m<=23 else '24-27'
        a=agg[(mb,wb)]; a[0]+=1; a[1]+=int(x[2]); a[2]+=cb
for k in sorted(agg):
    n,ok,cb,_=agg[k]
    if n<200: continue
    crc=ok/n; C=cb/n
    p=1-crc**(1/C) if crc>0 else float('nan')
    print('L%d mcs%-6s %-7s n=%6d crc=%.3f meanC=%5.1f p_cb(eq)=%.4f'%(L,k[0],k[1],n,crc,C,p))
