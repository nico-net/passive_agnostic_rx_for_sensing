import sys,re,glob
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; t={}
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line or 'Nl=4' not in line: continue
        l=A.sub('',line); m=re.search(r'EVMGRANT rnti=\S+ (\d+)\.(\d+) crc=(\d)',l); r=re.search(r'rb=\d+\+(\d+)',l)
        if not m or not r or ' nsym=0 ' in l: continue
        n=int(r.group(1)); k='5-32' if 5<=n<=32 else '33-128' if 33<=n<=128 else None
        if not k: continue
        s=int(m.group(2)); t.setdefault((k,s),[0,0]); t[(k,s)][0]+=int(m.group(3)); t[(k,s)][1]+=1
    print(rd)
    for k in ['5-32','33-128']:
        print('  %-7s'%k, ' '.join('s%d:%.2f/%d'%(s,t[(k,s)][0]/t[(k,s)][1],t[(k,s)][1]) for s in range(20) if (k,s) in t and t[(k,s)][1]>=20))
