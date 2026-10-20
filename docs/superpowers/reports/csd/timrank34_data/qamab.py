import re,sys,collections
def bucket(m): return '0-4' if m<=4 else '5-9' if m<=9 else '10-16' if m<=16 else '17+'
for p in sys.argv[1:]:
    t=collections.defaultdict(lambda:collections.defaultdict(lambda:[0,0,0,0]))
    for ln in open(p,errors='replace'):
        if 'SENSING: QAMAB' not in ln: continue
        d=dict(re.findall(r'(\w+)=([^\s]+)',ln)); c=int(d['crc'])
        k=(int(d['nl']),int(d['qm']),bucket(int(d['mcs'])))
        for mode,a in re.findall(r' m(\d)=(-?\d)/',ln):
            a=int(a)
            if a<0: continue
            v=t[k][mode]; v[0]+=1; v[1]+=c; v[2]+=a; v[3]+=(a==1 and c==0)
    print(p)
    for k in sorted(t):
        s='  Nl=%d Qm=%d MCS %-5s'%k
        for mode,v in sorted(t[k].items()):
            s+=' | m%s: n=%d default %d (%.0f%%) alt %d (%.0f%%) alt-only %d def-only %d'%(mode,v[0],v[1],100*v[1]/v[0],v[2],100*v[2]/v[0],v[3],v[1]-v[2]+v[3])
        print(s)
