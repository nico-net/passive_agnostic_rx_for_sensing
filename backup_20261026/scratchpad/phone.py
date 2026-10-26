import sys,re,collections
C='/home/sens/NICOLA/captures/'
hdr=None
for r in sys.argv[1:]:
    g=collections.defaultdict(lambda:[0,0])
    for i,l in enumerate(open(C+r+'/gnb/sched.csv')):
        x=l.rstrip('\n').split(',')
        if i==0: hdr={k:j for j,k in enumerate(x)}; continue
        if len(x)<20 or x[hdr['dir']]!='dl' or x[hdr['kind']]!='pdsch': continue
        m=re.match(r'\[(\d+)\.\.(\d+)\)',x[hdr['rb']])
        if not m: continue
        w=int(m.group(2))-int(m.group(1)); mc=x[hdr['mcs']]
        if not mc.isdigit() or not 24<=int(mc)<=27: continue
        wb='n<=32' if w<=32 else 'w>128' if w>128 else 'm'
        g[wb][0 if x[hdr['newtx']]=='1' else 1]+=1
    print(r,' '.join('%s: new=%d retx=%d retx/(all)=%.2f'%(k,v[0],v[1],v[1]/max(1,v[0]+v[1])) for k,v in sorted(g.items())))
