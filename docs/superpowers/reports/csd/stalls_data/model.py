import sys,re,json,glob
def clean(l): return re.sub(r"\x1b\[[0-9;]*m","",l)
def load(d):
    ms=[];pt=[]
    for l in open(f"/home/nicola/NICOLA/ota_runs/{d}/run.log",errors="ignore"):
        l=clean(l)
        i=l.find('ISAC_METRICS {')
        if i>=0:
            try: ms.append(json.loads(l[i+13:].strip()))
            except Exception: pass
        if "PDTIM calls" in l:
            c=int(re.search(r"calls=(\d+)",l).group(1))
            mx=0;sm=0;ss={}
            for st in ("fep","chest","demod","ldpc"):
                m=re.search(st+r"\[n=\d+ mean=([\d.]+)us cpu=[\d.]+us max=([\d.]+)us tot=([\d.]+)s sd=([\d.]+)us",l)
                if m: mx=max(mx,float(m.group(2))); ss[st]=(float(m.group(1)),float(m.group(3)),float(m.group(4)))
            pt.append((c,mx,ss))
    return ms,pt
for d in sys.argv[1:]:
    ms,pt=load(d)
    if len(ms)<3: print(d,"no metrics");continue
    T=ms[-1]["t_mono_ns"]/1e9-ms[0]["t_mono_ns"]/1e9
    f=lambda k:ms[-1][k]-ms[0][k]
    q=f("pdschq_queued");dec=f("pdschq_decoded");st=f("pdschq_drop_stale")
    # stale in first 30 s vs rest
    t0=ms[0]["t_mono_ns"]
    first=[m for m in ms if (m["t_mono_ns"]-t0)/1e9<=30][-1]
    s30=first["pdschq_drop_stale"]-ms[0]["pdschq_drop_stale"]; q30=first["pdschq_queued"]-ms[0]["pdschq_queued"]
    c,mx,ss=pt[-1] if pt else (0,0,{})
    busy=sum(v[1] for v in ss.values())
    lat=ms[-1]["pdschq_latency"]["enqueue_dequeue"]
    print(f"{d}: T={T:.0f}s q={q} ({q/T:.0f}/s) dec={dec} stale={st} ({100*st/q:.0f}%) | first30s q={q30} stale={s30} ({100*s30/max(q30,1):.0f}%), rest stale {100*(st-s30)/max(q-q30,1):.0f}% | bodies={c} ({c/max(dec,1):.2f}/dec) stage_busy_tot={busy:.0f}s -> util_of_5={100*busy/(5*T):.0f}% | enq_deq>8ms {100*(lat[7]+lat[8])/q:.1f}% >1ms {100*sum(lat[3:])/q:.0f}% | maxstage_ms={mx/1e3:.0f} sd={{{','.join(f'{k}:{v[2]:.0f}' for k,v in ss.items())}}}")
