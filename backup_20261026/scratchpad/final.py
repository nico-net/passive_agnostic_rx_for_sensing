import pickle,json,gzip,re,statistics as st,numpy as np,collections,os
rows=pickle.load(open('out/rows.pkl','rb'))
gn={}
for l in open('gnbm.txt'):
    m=re.match(r'(\S+)\s+(\d{4}_\d{6}) gNB DL ranks (.*?)\s*\| phones: cqi (\S+) ri (\S+) pusch_snr (\S+) rsrp (\S+) \| DL nack ([\d.]+)%',l)
    if m: gn[m.group(1)+m.group(2)]=(m.group(3).strip(),m.group(8),m.group(6))
def f(s):
    try: return float(s)
    except: return None
out=[]
hdr='| run (MMDD_HHMM) | build | gNB DL rank | LMX before | phones DL NACK% | DL CRC MCS24-27 narrow<=32 / wide>128 (rank:n/w) | UL CRC <=8/9-64/>64 | DM-RS SNR (4-ant mean, med) | wide data SNR (DD) | snr_cpe-snr | br_snr spread (max-min) | ANTPOW dB (ch0-3) | eps ppm | GPU demod/pass_dl/chest | PDSCHQ stale% |'
out.append(hdr); out.append('|'+'---|'*15)
for x in rows:
    r,ts,b,pl,lm,hi,ulw,dm,gap,sp,ant,rf,sfo,cfo,gp,stale,tm,nh=x
    if not ts.startswith('2026'): continue
    t=ts[4:8]+'_'+ts[9:13]
    key=r.split('_lab_')[0][:9]
    # gnb
    g=None
    for k,v in gn.items():
        if k.endswith(ts[4:]) : g=v
    # wide data snr & spread for dominant rank
    dl=[l.rstrip('\n').split('\t') for l in gzip.open('out/'+r+'.dl.gz','rt')]
    dl=[y for y in dl if len(y)>=19 and y[3].isdigit() and y[4].isdigit() and y[6].isdigit() and int(y[3])>=24]
    dom=collections.Counter(int(y[4]) for y in dl).most_common(1)
    L=dom[0][0] if dom else 0
    w=[y for y in dl if int(y[4])==L and int(y[6])>128 and f(y[8]) is not None and f(y[8])<60]
    ws=st.median(f(y[8]) for y in w) if w else float('nan')
    wg=st.median(f(y[9])-f(y[8]) for y in w if f(y[9]) is not None and f(y[9])<60) if w else float('nan')
    spr=[]
    for y in w:
        bb=[f(z) for z in y[10].split(',')]
        if len(bb)==4 and None not in bb and min(bb)>0: spr.append(max(bb)-min(bb))
    cr=' '.join('r%d:%s/%s'%(l_,('%.2f'%hi[(l_,'n')][0]) if hi[(l_,'n')][1]>=100 else '-',('%.2f'%hi[(l_,'w')][0]) if hi[(l_,'w')][1]>=100 else '-') for l_ in (1,2,4) if hi[(l_,'n')][1]>=100 or hi[(l_,'w')][1]>=100)
    uc='/'.join(('%.2f'%ulw[k][0]) if ulw[k][1]>=100 else '-' for k in 'nmw')
    gpd=gp
    gm=re.findall(r'(demo|ches|pass)\w*=(\w)',gpd)
    gps='-'
    jj=json.load(open('out/'+r+'.json'))['gp']
    gps='%s/%s/%s'%(jj.get('demod','-')[0],jj.get('pass_dl','-')[0],jj.get('chest','-')[0])
    out.append('| %s %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |'%(key,t,b[:12],g[0] if g else '-','LMX-FAIL' if lm else ('after-fail' if pl else 'no'),g[1] if g else '-',cr or '-',uc,'%.1f'%dm if dm==dm else '-','L%d %.1f'%(L,ws) if w else '-','%.1f'%wg if w else '-','%.1f'%st.median(spr) if spr else '-',' '.join('%.1f'%a for a in ant) if ant else '-',('%.2f'%sfo) if sfo else '-',gps,'%.0f'%stale if stale==stale else '-'))
open('runtable.md','w').write('\n'.join(out)+'\n')
print(len(out))
