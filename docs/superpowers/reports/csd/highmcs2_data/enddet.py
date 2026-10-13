# allocation end from per-symbol power (CDMDIAG pow_db): last symbol before a >= 15 dB drop that persists to the assumed end
import sys,re,collections
for fn in sys.argv[1:]:
    ev={}
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' in l:
            m=re.search(r'rnti=(\S+) (\d+\.\d+)',l); d=dict(re.findall(r'(\w+)=(\S+)',l))
            if d.get('probe')=='0': ev[('0x%04x'%int(m.group(1),16),m.group(2))]=d
    T=collections.Counter(); C=collections.Counter(); ends=collections.Counter()
    for l in open(fn,errors='replace'):
        if 'CDMDIAG' not in l or 'pow_db' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        pw=[float(x) for x in re.search(r'pow_db=\[([^\]]*)\]',l).group(1).split(',')]
        S=int(d['S']); L=int(d['L']); end=S+L
        e=end
        for m in range(S+1,end):
            if all(pw[k]<=-15 for k in range(m,end)): e=m; break
        short = e<end
        ev_=ev.get((d['rnti'].lower(),d['fs'])); crc=ev_['crc'] if ev_ else '?'
        k=(d['cdm'],min(int(d['nl']),3),'short' if short else 'full'); T[k]+=1; C[k+(crc,)]+=1
        if short: ends[(S,e-S)]+=1
    print('#',fn.split('/')[-2])
    for k in sorted(T): print(f"  cdm={k[0]} nl={k[1]} {k[2]:5s}: n={T[k]} crc_ok={C[k+('1',)]} fail={C[k+('0',)]} ?={C[k+('?',)]}")
    print('  detected (S, L) of short grants:', dict(ends))
