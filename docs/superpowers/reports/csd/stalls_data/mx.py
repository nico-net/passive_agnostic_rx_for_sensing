import re,sys
for f in sys.argv[1:]:
    print("==",f); seen=set()
    for n,l in enumerate(open(f,errors="ignore")):
        if "PDTIM calls" not in l: continue
        c=int(re.search(r"calls=(\d+)",l).group(1))
        for st in ("fep","chest","demod","ldpc"):
            m=re.search(st+r"\[n=\d+ mean=[\d.]+us cpu=[\d.]+us max=([\d.]+)us",l)
            if m and float(m.group(1))>50000 and (st,float(m.group(1))) not in seen:
                seen.add((st,float(m.group(1)))); print(" line",n,"calls",c,st,"max_us",m.group(1))
