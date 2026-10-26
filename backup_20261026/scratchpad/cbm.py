import re,sys,glob,collections
r=sys.argv[1]
f=glob.glob('/home/sens/NICOLA/captures/'+r+'/*/run.log')[0]
pat=re.compile(r'CBMAP rnti=(\S+) (\S+) crc=(\d) mcs=(\d+) Nl=(\d) Qm=(\d) rb=(\d+)\+(\d+) s=(\d+)\+(\d+) .*? C=(\d+) E=(\d+) E2=(\d+) rE2=(\d+) ok=(\w+) it=(\w+) re=([\d,]+)')
n=0; stat=collections.defaultdict(lambda:[0,0]); nsample=0; ex=None
with open(f,errors='replace') as fh:
    for line in fh:
        if 'CBMAP' not in line: continue
        m=pat.search(line)
        if not m: continue
        Nl=int(m.group(5)); C=int(m.group(11)); w=int(m.group(8))
        if Nl!=4 or w<250: continue
        ok=m.group(15); it=m.group(16)
        # bit order guess A: big-endian string of C bits (MSB first)
        bits=bin(int(ok,16))[2:].zfill(len(ok)*4)
        nz=sum(1 for ch in it if ch!='g')
        if ex is None: ex=(C,ok,it,bits.count('1'),nz,len(it))
        nsample+=1
        # use it string char positions only if len(it)==C
        if len(it)==C:
            for i,ch in enumerate(it): stat[i][0]+=1; stat[i][1]+= (ch!='g')
print(r,'samples',nsample,'example (C,ok,it,popcount,nonG,len)',ex)
if stat:
    C=max(stat)+1
    print(' P(CB i decodes) first 60:',' '.join('%.2f'%(stat[i][1]/stat[i][0]) for i in range(min(C,60)) if stat[i][0]>50))
