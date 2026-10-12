import re,sys
for f in sys.argv[1:]:
    prev=None;pl=0;out=[]
    for n,l in enumerate(open(f,errors="ignore")):
        m=re.search(r"utc_ns=(\d+) rnti=\S+ sfn=(\d+) slot=(\d+)",l)
        if not m: continue
        u=int(m.group(1))/1e6; s=int(m.group(2))*20+int(m.group(3))
        if prev:
            du=u-prev[0]; ds=(s-prev[1])%(1024*20)
            # wall minus signal time (ms)
            if du>150 and ds<200: out.append((n,round(du),ds))
        prev=(u,s)
    print(f,len(out),out[:12])
