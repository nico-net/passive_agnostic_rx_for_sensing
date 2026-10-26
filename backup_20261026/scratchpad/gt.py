import sys,csv,collections,re
C=collections.Counter()
for r in csv.DictReader(open(sys.argv[1])):
    if r['dir']!='ul' or r['kind']!='pusch': continue
    m=re.match(r'\[(\d+)\.\.(\d+)\)',r['rb'])
    if not m: continue
    n=int(m.group(2))-int(m.group(1)); tbs=int(r['tbs'] or 0)
    pb='a1-8' if n<=8 else 'b9-64' if n<=64 else 'c>64'
    d=tbs*8/ max(n,1)  # bits/ (tbs bytes) per rb
    mb='lowrate' if d<150 else 'mid' if d<400 else 'high'
    C[(pb,mb,'new' if r['newtx']=='1' else 'rtx')]+=1
for k in sorted(C): print(k,C[k])
print(sum(C.values()))
