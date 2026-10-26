import sys,re,glob
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; t={}
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line: continue
        l=A.sub('',line); m=re.search(r'crc=(\d) mcs=(\d+) tbl=(\d) Qm=\d Nl=(\d) rb=\d+\+(\d+)',l)
        if not m or ' nsym=0 ' in l or 'rnti=0xffff' in l: continue
        c,mcs,nl,n=int(m.group(1)),int(m.group(2)),m.group(4),int(m.group(5))
        w='<=32' if n<=32 else '33-128' if n<=128 else '>128'
        mb='%02d-%02d'%(mcs//4*4,mcs//4*4+3)
        k=(nl,w,mb); t.setdefault(k,[0,0]); t[k][0]+=c; t[k][1]+=1
    print(rd.split('/')[-1])
    for k in sorted(t):
        if t[k][1]>=30: print('  Nl=%s %-7s mcs %s n=%6d crc=%.3f'%(k[0],k[1],k[2],t[k][1],t[k][0]/t[k][1]))
