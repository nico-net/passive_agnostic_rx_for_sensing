import sys,re,glob
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; t={}
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line or ' nsym=0 ' in line: continue
        l=A.sub('',line); m=re.search(r'EVMGRANT rnti=\S+ (\d+)\.(\d+) crc=(\d).*?Nl=(\d) rb=\d+\+(\d+)',l)
        if not m: continue
        fr,sl,c,nl,n=int(m.group(1)),int(m.group(2)),int(m.group(3)),m.group(4),int(m.group(5))
        if not (5<=n<=32): continue
        k=(nl,(fr*20+sl)%40); t.setdefault(k,[0,0]); t[k][0]+=c; t[k][1]+=1
    print(rd.split('/')[-1])
    for nl in sorted(set(k[0] for k in t)):
        print('  Nl=%s'%nl, ' '.join('s%d:%.2f/%d'%(s,t[(nl,s)][0]/t[(nl,s)][1],t[(nl,s)][1]) for s in range(40) if (nl,s) in t and t[(nl,s)][1]>=30))
