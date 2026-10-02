import re, sys, glob
def an(path):
    t0=tr=tc=None; o_r=o_c=None; last=None; added=None; mode=None; first_ev=None
    for line in open(path+'/rx/rx.log', errors='replace'):
        line=re.sub(r'\x1b\[[0-9;]*m','',line); m=re.match(r'([0-9.]+) ',line)
        if not m: continue
        t=float(m.group(1))
        if t0 is None and 'rnti_seen' in line and 'fmt=1_1' in line: t0=t
        if tr is None and 'ORACLE_RESTORE rnti=0x1234 tda=0 ' in line: tr=t; added=re.search(r'added=(\d+)',line).group(1)
        m2=re.search(r'Technique D evidence rnti=0x1234 config=\S+ tda=0 outcomes=(\d+)',line)
        if m2 and tc is None:
            last=int(m2.group(1)); first_ev = first_ev or t
            if tr is not None and o_r is None: o_r=last
        m3=re.search(r'Qm oracle rnti=0x1234 .* -> (\d+) hypotheses',line)
        if m3 and tc is None: mode=m3.group(1)
        if tc is None and 'Technique D CONVERGED rnti=0x1234 tda=0' in line: tc=t; o_c=last
    return dict(ttc0=round(tc-t0,2), restore=None if tr is None else round(tr-t0,2), out_at_restore=o_r, out_at_conv=o_c, added=added,
                n_hyp_qm=mode, rate=round(o_c/(tc-first_ev),0) if o_c else None)
for d in sys.argv[1:]:
    print(d.split('/')[-2]+'/'+d.split('/')[-1], an(d))
