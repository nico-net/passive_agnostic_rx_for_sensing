import sys,re,collections
for fn in sys.argv[1:]:
    cd={}; T=collections.defaultdict(list)
    for l in open(fn,errors='replace'):
        if 'CDMDIAG' in l:
            d=dict(re.findall(r'(\w+)=(\S+)',l)); cd[(d['rnti'].lower(),d['fs'])]=d
        elif 'EVMGRANT' in l:
            d=dict(re.findall(r'(\w+)=(\S+)',l))
            if d.get('probe')!='0': continue
            m=re.search(r'rnti=(\S+) (\d+\.\d+)',l)
            r='0x%04x'%int(m.group(1),16); c=cd.get((r,m.group(2)))
            if not c: continue
            s=float(d['snr']); sb='<6' if s<6 else '6-10' if s<10 else '>=10'
            T[(d['Nl'],d['ports'],d['cdm'],sb)].append((float(c['other_vs_own_db']),int(d['crc'])))
    print('#',fn.split('/')[-2])
    for k in sorted(T):
        v=T[k]
        if len(v)<5: continue
        o=sorted(x[0] for x in v); hi=[x for x in v if x[0]>-6]; lo=[x for x in v if x[0]<=-6]
        f=lambda L: f"{100*sum(c for _,c in L)/len(L):.0f}%({len(L)})" if L else '-'
        print(f"Nl={k[0]} ports={k[1]} cdm={k[2]} snr {k[3]:>5}: n={len(v)} other/own p10/p50/p90 {o[len(o)//10]:.1f}/{o[len(o)//2]:.1f}/{o[9*len(o)//10]:.1f} dB | crc other>-6dB {f(hi)}  other<=-6dB {f(lo)}")
