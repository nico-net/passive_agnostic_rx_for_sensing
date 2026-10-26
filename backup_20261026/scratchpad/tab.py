import json,glob,gzip,os,re,statistics as st
S='/tmp/claude-1000/-home-sens-NICOLA-adaptive-rx-UL-DL/6de2bc9d-5f38-466a-9776-f15a57560647/scratchpad/out/'
C='/home/sens/NICOLA/captures/'
runs=sorted([os.path.basename(p)[:-5] for p in glob.glob(S+'*.json')], key=lambda r:r.split('_lab_')[1])
prev_lmx=False
def med(x): return st.median(x) if x else float('nan')
def crc(v): return (sum(v)/len(v), len(v)) if v else (float('nan'),0)
rows=[]
for r in runs:
    i=json.load(open(S+r+'.json'))
    ts=r.split('_lab_')[1]
    try: lg=open(C+r+'/receiver_launcher.log',errors='replace').read()
    except: lg=''
    m=re.search(r'binary (\S+)/nr-uesoftmodem',lg); b=m.group(1).split('/')[-2] if m else '?'
    dl=[l.rstrip('\n').split('\t') for l in gzip.open(S+r+'.dl.gz','rt')]; dl=[x for x in dl if len(x)>=19 and x[3].isdigit() and x[4].isdigit() and x[6].isdigit()]
    ul=[l.rstrip('\n').split('\t') for l in gzip.open(S+r+'.ul.gz','rt')]; ul=[x for x in ul if len(x)>=10 and x[4].isdigit()]
    hi={}
    for L in range(1,5):
        for nm,lo,hi_ in (('n',1,32),('w',129,999)):
            v=[int(x[2]) for x in dl if 24<=int(x[3])<=27 and int(x[4])==L and lo<=int(x[6])<=hi_]
            hi[(L,nm)]=crc(v)
    ulw={}
    for nm,lo,hi_ in (('n',1,8),('m',9,64),('w',65,999)):
        v=[int(x[1]) for x in ul if lo<=int(x[4])<=hi_]
        ulw[nm]=crc(v)
    hs=[x for x in dl if 24<=int(x[3])<=27]
    def f(s): 
        try: return float(s)
        except: return None
    gap=[f(x[9])-f(x[8]) for x in dl if f(x[8]) is not None and f(x[9]) is not None and f(x[8])>5 and int(x[3])>=20]
    # dmrs_snr: last element combined? take mean of first four
    dm=[]
    for x in dl:
        v=[f(y) for y in x[13].split(',')]
        if len(v)>=4 and all(a is not None for a in v[:4]) and int(x[3])>=20 and v[0]>-50: dm.append(sum(v[:4])/4)
    sp=[]
    for x in dl:
        if int(x[3])>=20 and int(x[4])==1:
            v=[f(y) for y in x[10].split(',')]
            if len(v)==4 and all(a is not None for a in v) and min(v)>0: sp.append(max(v)-min(v))
    ant=[med([a[k] for a in i['ant']]) for k in range(4)] if i['ant'] and len(i['ant'][0])==4 else None
    rf=[med([a[k] for a in i['rf'] if len(a)==4]) for k in range(4)] if i['rf'] and any(len(a)==4 for a in i['rf']) else None
    gp=','.join('%s=%s'%(k[:4],v[0]) for k,v in sorted(i['gp'].items()))
    pq=i['pq'] or ''
    mq=re.search(r'queued=(\d+) decoded=(\d+) crc_ok=(\d+) \(([\d.]+)%\) dropped\[full=(\d+) stale=(\d+)',pq)
    stale=100*int(mq.group(6))/max(1,int(mq.group(1))) if mq else float('nan')
    rows.append((r,ts,b,prev_lmx,i['lmx'],hi,ulw,med(dm),med(gap),med(sp),ant,rf,i['sfodm'] or i['sfo'],med(i['cfo']) if i['cfo'] else None,gp,stale,i['timemut'],len(hs)))
    prev_lmx = i['lmx']>0
import pickle; pickle.dump(rows,open(S+'rows.pkl','wb'))
for x in rows:
    r,ts,b,pl,lm,hi,ulw,dm,gap,sp,ant,rf,sfo,cfo,gp,stale,tm,nh=x
    print(r.split('_lab_')[0][:9].ljust(9),ts[4:],b[:10].ljust(10),'LMX' if lm else ('post' if pl else '-  '),
      ' '.join('L%d n%.2f(%d)/w%.2f(%d)'%(L,hi[(L,'n')][0],hi[(L,'n')][1],hi[(L,'w')][0],hi[(L,'w')][1]) for L in (1,2,4) if hi[(L,'n')][1]+hi[(L,'w')][1]>0),
      '| UL n%.2f m%.2f w%.2f(%d)'%(ulw['n'][0],ulw['m'][0],ulw['w'][0],ulw['w'][1]),
      '| dm %.1f gap %.1f sp %.1f'%(dm,gap,sp), '| ant', ant and ' '.join('%.1f'%a for a in ant), '| rf', rf and ' '.join('%.0f'%a for a in rf), '| eps',sfo,'cfo',cfo and round(cfo), '|',gp,'| stale %.1f'%stale,'tm',tm, flush=True)
