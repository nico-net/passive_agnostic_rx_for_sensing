import sys,re,glob,statistics as st
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; v={}
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line or 'Nl=2' not in line or ' nsym=0 ' in line: continue
        l=A.sub('',line); m=re.search(r'crc=(\d) mcs=(\d+).*?rb=\d+\+(\d+).*?snr_l=([-\d.]+),([-\d.]+)',l); g=re.search(r'dmrs_snr=([-\d.,]+)',l)
        if not m or int(m.group(2))<24: continue
        n=int(m.group(3)); w='<=32' if n<=32 else 'wide'
        a,b=float(m.group(4)),float(m.group(5)); d=st.mean(float(x) for x in g.group(1).split(',')[:4]) if g else -1
        v.setdefault(w,[]).append((int(m.group(1)),min(a,b),abs(a-b),d))
    print(rd.split('/')[-1])
    for w,x in v.items(): print('  %-5s n=%5d crc=%.3f minlayer_snr_med=%.1f gap_med=%.1f dmrs_snr_med=%.1f'%(w,len(x),sum(i[0] for i in x)/len(x),st.median(i[1] for i in x),st.median(i[2] for i in x),st.median(i[3] for i in x)))
