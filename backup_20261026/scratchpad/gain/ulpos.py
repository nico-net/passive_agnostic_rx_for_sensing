import sys,re,glob
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; t={}
    for line in open(f,errors='replace'):
        if 'UL_PUSCH_CRC' not in line or ' probe=0' not in line: continue
        l=A.sub('',line); m=re.search(r'ok=(\d) mcs=\d+ prb=(\d+)\+(\d+)',l)
        if not m: continue
        ok,s,n=int(m.group(1)),int(m.group(2)),int(m.group(3))
        w='n<=8' if n<=8 else 'n9-64' if n<=64 else 'n>64'
        c=s+n/2; pos='low(<90)' if c<90 else 'mid' if c<183 else 'high(>=183)'
        k=(w,pos); t.setdefault(k,[0,0]); t[k][0]+=ok; t[k][1]+=1
    print(rd.split('/')[-1]); 
    for k in sorted(t): print('  %-6s %-12s n=%6d crc=%.3f'%(k[0],k[1],t[k][1],t[k][0]/t[k][1]))
