"""summ.py LOG... : CRC by Nl x MCS bucket (EVMGRANT probe=0, all SNR and DD snr>=10), check_bad, PDTIM last means"""
import sys,re,collections
def bucket(m): return '0-4' if m<=4 else '5-9' if m<=9 else '10-16' if m<=16 else '17+'
for p in sys.argv[1:]:
    t=collections.defaultdict(lambda:[0,0,0,0]); bad=[]; pd=None; dm=[]
    for l in open(p,errors='replace'):
        if 'EVMGRANT' in l:
            d=dict(re.findall(r'(\w+)=(\S+)',l))
            if d.get('probe')!='0' or d.get('rnti')=='0xffff': continue
            try: k=(int(d['Nl']),bucket(int(d['mcs']))); c=int(d['crc']); s=float(d['snr'])
            except: continue
            v=t[k]; v[0]+=c; v[1]+=1
            if s>=10: v[2]+=c; v[3]+=1
        elif 'GPU_EXACT_' in l and 'check_bad=' in l:
            m=re.search(r'GPU_EXACT_(\w+) calls=(\d+).*?check_bad=(\d+)',l)
            if m: bad.append((m.group(1),int(m.group(3))))
        elif 'SENSING: PDTIM calls' in l: pd=l
    print(p)
    for k in sorted(t):
        v=t[k]; print('  Nl=%d MCS %-5s all %4d/%-5d %5.1f%%   snr>=10 %4d/%-5d %5.1f%%'%(k[0],k[1],v[0],v[1],100*v[0]/max(v[1],1),v[2],v[3],100*v[2]/max(v[3],1)))
    mx=collections.defaultdict(int)
    for n,b in bad: mx[n]=max(mx[n],b)
    print('  check_bad max:',dict(mx))
    if pd:
        print('  PDTIM', ' '.join('%s=%s'%(a,b) for a,b in re.findall(r'(\w+)\[n=\d+ mean=([\d.]+us)',pd)), re.search(r'calls=(\d+)',pd).group(0))
