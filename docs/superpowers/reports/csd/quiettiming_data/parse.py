import re,sys,json,collections
ST=['fep','chest','alloc','demod','ldpc']
def num(x): return float(x)
def parse(d):
    t=re.sub(r'\x1b\[[0-9;]*m','',open(d+'/run.log',errors='replace').read())
    L=t.splitlines(); r={'dir':d.split('/')[-1]}
    def last(k):
        m=None
        for l in L:
            if k in l: m=l
        return m
    p=last('PDTIM calls')
    if p:
        for s in ST:
            m=re.search(s+r'\[n=(\d+) mean=([\d.]+)us cpu=([\d.]+)us max=([\d.]+)us tot=([\d.]+)s sd=([\d.]+)us\]',p)
            if m: r['t_'+s]=dict(n=int(m[1]),mean=num(m[2]),max=num(m[4]),tot=num(m[5]),sd=num(m[6]))
    p=last('PDTIMPCT')
    if p:
        for m in re.finditer(r'(fep|chest|demod|ldpc|sum)\[n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+)\]',p):
            r['p_'+m[1]]=dict(n=int(m[2]),p50=int(m[3]),p90=int(m[4]),p99=int(m[5]))
    p=last('PDTIMCLS')
    if p:
        for m in re.finditer(r'(real|retry|probe)\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+) gt5ms=(\d+) gt20ms=(\d+) share_gt1/5/20ms=([\d.]+)/([\d.]+)/([\d.]+)\]',p):
            r['c_'+m[1]]=dict(n=int(m[2]),mean=num(m[3]),p50=int(m[4]),p90=int(m[5]),p99=int(m[6]),max=num(m[7]),gt5=int(m[8]),gt20=int(m[9]),s1=num(m[10]),s5=num(m[11]),s20=num(m[12]))
    mt=last('ISAC_METRICS')
    for k in ('pdschq_queued','pdschq_decoded','pdschq_drop_stale','pdschq_drop_full','pdschq_crc_ok','scanq_drop_stale','scanq_drop_full','scanq_queued'):
        m=re.search('"%s":(\\d+)'%k,mt or ''); r[k]=int(m[1]) if m else None
    ev=[l for l in L if 'EVMGRANT rnti' in l and 'probe=0' in l and ' crc=1 ' in l]
    r['crc_ok_real']=len(ev); r['crc_rntis']=sorted(set(re.findall(r'rnti=(0x[0-9a-f]+)',' '.join(ev))))
    r['n_evm_real']=sum(1 for l in L if 'EVMGRANT rnti' in l and 'probe=0' in l)
    sl=[]
    for l in L:
        m=re.search(r'PDTIMSLOW t=([\d.]+) class=(\w+) us=(\d+) fep=(\d+)/\d+ chest=(\d+)/\d+ demod=(\d+)/\d+ ldpc=(\d+)/\d+',l)
        if m: sl.append((float(m[1]),m[2],int(m[3]),dict(zip(['fep','chest','demod','ldpc'],map(int,m.groups()[3:])))))
    r['slow_logged']=len(sl)
    ev_=[]  # cluster by time (50 ms)
    for s in sorted(sl):
        if ev_ and s[0]-ev_[-1]['t1']<0.05: ev_[-1]['t1']=s[0]; ev_[-1]['n']+=1; ev_[-1]['max']=max(ev_[-1]['max'],s[2])
        else: ev_.append(dict(t0=s[0],t1=s[0],n=1,max=s[2]))
    r['stall_events']=[(round(e['t0']%10000,3),e['n'],e['max']) for e in ev_]
    st=collections.Counter(); stus=collections.Counter()
    for s in sl:
        k=max(s[3],key=s[3].get); st[k]+=1; stus[k]+=s[3][k]
    r['slow_stage_dom']=dict(st); r['slow_stage_us']=dict(stus)
    r['slow_stage_sum']={k:sum(s[3][k] for s in sl) for k in ('fep','chest','demod','ldpc')}
    r['gpu_demod_disabled']=bool(re.search('GPU demodulation disabled',t)); r['finished']=bool(last('blind PDCCH monitor summary'))
    return r
if __name__=='__main__':
    for d in sys.argv[1:]: print(json.dumps(parse(d)))
