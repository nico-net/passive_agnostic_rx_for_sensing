import sys,re,collections,statistics
kv=re.compile(r'(\w+)=(\S+)')
D=collections.defaultdict(list)
for line in sys.stdin:
    if 'UL_PUSCH_CRC' not in line: continue
    d=dict(kv.findall(line))
    if d.get('probe')!='0' or d.get('hyp','').split('@')[-1]!='0': continue
    s,w=map(int,d['prb'].split('+')); m=int(d['mcs'])
    c='narrow' if w<=8 else 'mid' if w<=64 else 'wide'
    D[(c,'HI' if m>=20 else 'lo',d['ok'])].append(float(d['snr_db']))
for k in sorted(D):
    v=sorted(D[k]); print(k,len(v),"p10 %.1f med %.1f p90 %.1f"%(v[len(v)//10],v[len(v)//2],v[len(v)*9//10]))
