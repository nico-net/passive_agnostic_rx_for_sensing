# pooled paired TINT_AB: per Nl x Qm x snr(off arm) bin: n, CRC off/on, discordant on-only / off-only, mean DD snr off/on
import sys,re,collections
T=collections.defaultdict(lambda:[0,0,0,0,0,0.0,0.0])
for fn in sys.argv[1:]:
    for l in open(fn,errors='replace'):
        if 'TINTAB' not in l: continue
        d=dict(re.findall(r'(\w+)=(\S+)',l))
        c=int(d['crc']); a=int(d['alt_crc']); tint=int(d['tint'])
        if a<0: continue
        on,off=(c,a) if tint else (a,c); s=float(d['snr']); sa=float(d['snr_alt'])
        son,soff=(s,sa) if tint else (sa,s)
        sb='<10' if soff<10 else '10-16' if soff<16 else '16-22' if soff<22 else '>=22'
        k=(min(int(d['nl']),3),int(d['qm']),sb); t=T[k]
        t[0]+=1; t[1]+=off; t[2]+=on; t[3]+=on and not off; t[4]+=off and not on; t[5]+=soff; t[6]+=son
print("| Nl | Qm | DD snr (interp off) | n | CRC off | CRC on | on-only | off-only | mean snr off -> on |")
print("|---|---|---|---|---|---|---|---|---|")
for k in sorted(T, key=lambda k:(k[0],k[1],['<10','10-16','16-22','>=22'].index(k[2]))):
    n,off,on,a,b,so,sn=T[k]
    print(f"| {k[0]}{'+' if k[0]==3 else ''} | {k[1]} | {k[2]} | {n} | {100*off/n:.1f} % | {100*on/n:.1f} % | {a} | {b} | {so/n:.1f} -> {sn/n:.1f} |")
