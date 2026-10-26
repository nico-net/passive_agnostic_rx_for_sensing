import sys,re,collections
kv=re.compile(r'(\w+)=(\S+)')
H=collections.Counter(); N=0
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+'))
    if w<=64 or int(d['mcs'])<20 or int(d['tbs'])<20000: continue
    a,b=map(int,d['seg'].split('/'))
    if b==0: continue
    H[(b, min(10,a*10//b))]+=1; N+=1
print(N)
for k in sorted(H): print(k,H[k])
