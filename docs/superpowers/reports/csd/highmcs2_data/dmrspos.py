import sys,re,collections
for fn in sys.argv[1:]:
    cd={}; T=collections.Counter(); C=collections.Counter()
    ev={}
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' in l:
            m=re.search(r'rnti=(\S+) (\d+\.\d+)',l); d=dict(re.findall(r'(\w+)=(\S+)',l))
            if d.get('probe')=='0': ev[('0x%04x'%int(m.group(1),16),m.group(2))]=d
    for l in open(fn,errors='replace'):
        if 'CDMDIAG' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if d['cdm']!='2': continue
        sym=[float(x) for x in re.search(r'sym_db=\[([^\]]*)\]',l).group(1).split(',')]
        S=int(d['S']); L=int(d['L']); assumed=int(d['dmrs'],16)
        det=0
        for m in range(S,min(14,S+L)):
            if sym[m]<=-6: det|=1<<m
        e=ev.get((d['rnti'].lower(),d['fs']))
        crc=e['crc'] if e else '?'
        k=(d['nl'],hex(assumed),hex(det)); T[k]+=1; C[k+(crc,)]+=1
    print('#',fn.split('/')[-2],' (cdm=2 grants: assumed DMRS mask vs symbols whose other-group energy is <= -6 dB)')
    for k,n in T.most_common(20):
        print(f"  nl={k[0]} assumed={k[1]} detected={k[2]}: n={n} crc_ok={C[k+('1',)]} crc_fail={C[k+('0',)]} no_evm={C[k+('?',)]}")
