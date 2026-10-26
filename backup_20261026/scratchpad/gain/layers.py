import sys,re,glob,statistics as st
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; b={}
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line or 'Nl=4' not in line: continue
        d=dict(re.findall(r'(\w+)=([^ ]+)',A.sub('',line)))
        if d.get('nsym')=='0': continue
        try:
            n=int(d['rb'].split('+')[1]); L=[float(x) for x in d['snr_l'].split(',')]; c=int(d['crc']); m=int(d['mcs'])
        except Exception: continue
        k='1-4' if n<=4 else '5-32' if n<=32 else '33-128' if n<=128 else '>128'
        b.setdefault(k,[]).append((c,sorted(L),m))
    print(rd)
    for k in ['1-4','5-32','33-128','>128']:
        v=b.get(k)
        if not v: continue
        med=[st.median(x[1][i] for x in v) for i in range(4)]
        print('  %-7s n=%6d crc=%.3f mcs_med=%d layer SNR sorted (min..max) medians: %s  spread=%.1f dB'%(k,len(v),sum(x[0] for x in v)/len(v),st.median(x[2] for x in v),' '.join('%.1f'%x for x in med),med[3]-med[0]))
