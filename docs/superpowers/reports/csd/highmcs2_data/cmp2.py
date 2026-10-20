# side-by-side whole-run CRC: cmp2.py "A logs,comma" "B logs,comma" ; buckets Nl x MCS(legacy) x snr bin; + PDTIM means
import sys,re,collections
def mb(m): return '0-4' if m<=4 else '5-9' if m<=9 else '10-16' if m<=16 else '17+'
def load(fns):
    T=collections.defaultdict(lambda:[0,0]); P=collections.defaultdict(list)
    for fn in fns:
        last=None
        for l in open(fn,errors='replace'):
            if 'EVMGRANT' in l:
                d=dict(re.findall(r'(\w+)=(\S+)',l))
                if d.get('probe')!='0' or d['rnti']=='0xffff': continue
                s=float(d['snr']); nl=min(int(d['Nl']),3); m=mb(int(d['mcs'])); c=int(d['crc'])
                sb='<10' if s<10 else '10-16' if s<16 else '16-22' if s<22 else '>=22'
                for k in ((nl,m,sb),(nl,m,'>=10' if s>=10 else None),(nl,m,'all')):
                    if k[2]: T[k][0]+=c; T[k][1]+=1
            elif 'SENSING: PDTIM calls' in l: last=l
        if last:
            for st in ('fep','chest','demod','ldpc'):
                mm=re.search(r'\b'+st+r'\[n=\d+ mean=([\d.]+)us',last)
                if mm: P[st].append(float(mm.group(1)))
    return T,P
A,PA=load(sys.argv[1].split(',')); B,PB=load(sys.argv[2].split(','))
order={'<10':0,'10-16':1,'16-22':2,'>=22':3,'>=10':4,'all':5}
for k in sorted(set(A)|set(B), key=lambda k:(k[0],['0-4','5-9','10-16','17+'].index(k[1]),order[k[2]])):
    a=A.get(k,[0,0]); b=B.get(k,[0,0])
    f=lambda x: f"{100*x[0]/x[1]:5.1f}% ({x[0]}/{x[1]})" if x[1] else "     -       "
    print(f"Nl={k[0]}{'+' if k[0]==3 else ' '} MCS {k[1]:>5} snr {k[2]:>6} | {f(a):>20} | {f(b):>20}")
for st in ('fep','chest','demod','ldpc'):
    print(f"PDTIM {st:6s} mean us  A {['%.0f'%x for x in PA[st]]}  B {['%.0f'%x for x in PB[st]]}")
