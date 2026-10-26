import sys,re,glob,statistics as st
A=re.compile(r'\x1b\[[0-9;]*m')
for rd in sys.argv[1:]:
    f=glob.glob(rd+'/*/run.log')[0]; v=[]; nv=[]; pw=[]
    for line in open(f,errors='replace'):
        if 'EVMGRANT' not in line or ' probe=0' not in line or 'Nl=4' not in line: continue
        l=A.sub('',line); r=re.search(r'rb=\d+\+(\d+)',l)
        if not r or not (5<=int(r.group(1))<=32): continue
        m=re.search(r'dmrs_snr=([-\d.,]+)',l); n=re.search(r'nvar_db=([-\d.,]+)',l); p=re.search(r'dmrs_pw=([-\d.]+)',l)
        if m: v.append(st.mean(float(x) for x in m.group(1).split(',')[:4]))
        if n: nv.append(st.mean(float(x) for x in n.group(1).split(',')))
        if p: pw.append(float(p.group(1)))
    if v: print('%-45s n=%5d dmrs_snr_med=%.1f nvar_med=%.1f dmrs_pw_med=%.1f'%(rd.split('/')[-1],len(v),st.median(v),st.median(nv) if nv else -1,st.median(pw) if pw else -1))
