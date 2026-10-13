# CRC vs margin over Shannon, margin from the ideal-MRC bound of per-branch LS SNR (br_snr), Nl=1 comb=3, MCS table 2 (256QAM)
import sys,re,collections,math
T2={0:(2,120),1:(2,193),2:(2,308),3:(2,449),4:(2,602),5:(4,378),6:(4,434),7:(4,490),8:(4,553),9:(4,616),10:(4,658),
11:(6,466),12:(6,517),13:(6,567),14:(6,616),15:(6,666),16:(6,719),17:(6,772),18:(6,822),19:(6,873),
20:(8,682.5),21:(8,711),22:(8,754),23:(8,797),24:(8,841),25:(8,885),26:(8,916.5),27:(8,948)}
T=collections.defaultdict(lambda:[0,0]); comb=sys.argv[1]; nlw=int(sys.argv[2])
for fn in sys.argv[3:]:
    for l in open(fn,errors='replace'):
        if 'EVMGRANT' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        if d.get('probe')!='0' or d['rnti']=='0xffff' or d.get('tbl')!='1' or int(d['Nl'])!=nlw: continue
        if comb!='any' and d.get('comb')!=comb: continue
        mcs=int(d['mcs'])
        if mcs>27: continue
        qm,r=T2[mcs]; se=qm*r/1024; req=10*math.log10(2**se-1)
        br=[float(x) for x in d['br_snr'].split(',') if float(x)>-50]
        if not br: continue
        if comb=='1': bound=max(br)
        else: bound=10*math.log10(sum(10**(x/10) for x in br))
        m=bound-req; mb=int(math.floor(m/2))*2
        T[(qm,mb)][0]+=int(d['crc']); T[(qm,mb)][1]+=1
for k in sorted(T):
    ok,n=T[k]
    if n>=8: print(f"Qm={k[0]} margin[{k[1]:3d},{k[1]+2:3d}) {100*ok/n:5.1f}% n={n}")
