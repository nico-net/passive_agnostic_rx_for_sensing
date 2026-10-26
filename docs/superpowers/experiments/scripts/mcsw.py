# TB CRC and CB CRC (sum(cb_ok)/sum(cb_tried), EVMGRANT) per Nl / grant width / MCS bin. usage: mcsw.py RUNDIR [RUNDIR...]
# CAVEAT: decoding of a TB stops after 5 failed code blocks (NR_CB_FAIL_LIMIT), so cb_tried is biased toward the early blocks of failing TBs;
# CB CRC is a quality index of the decodable blocks, not the channel's per-block error rate. Logs before 2026-10-26 have no cb fields (cb=-).
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
        k=(nl,w,mb); t.setdefault(k,[0,0,0,0]); t[k][0]+=c; t[k][1]+=1
        cb=re.search(r' cb_ok=(\d+) cb_tried=(\d+)',l)
        if cb: t[k][2]+=int(cb.group(1)); t[k][3]+=int(cb.group(2))
    print(rd.split('/')[-1])
    for k in sorted(t):
        if t[k][1]>=30: print('  Nl=%s %-7s mcs %s n=%6d tb_crc=%.3f cb_crc=%s'%(k[0],k[1],k[2],t[k][1],t[k][0]/t[k][1],'%.3f'%(t[k][2]/t[k][3]) if t[k][3] else '-'))
