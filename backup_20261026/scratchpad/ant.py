import gzip,sys,json,statistics as st
def f(s):
    try: return float(s)
    except: return None
for r in sys.argv[2:]:
    L=int(sys.argv[1])
    i=json.load(open('out/'+r+'.json'))
    ant=[st.median(a[k] for a in i['ant'] if len(a)==4) for k in range(4)]
    br=[[],[],[],[]]; dm=[[],[],[],[]]; nv=[[],[],[],[]]
    for l in gzip.open('out/'+r+'.dl.gz','rt'):
        x=l.rstrip('\n').split('\t')
        if len(x)<19 or not x[3].isdigit() or int(x[4])!=L or int(x[3])<20 or not x[6].isdigit() or int(x[6])<33: continue
        b=[f(y) for y in x[10].split(',')]; d=[f(y) for y in x[13].split(',')]; n=[f(y) for y in x[12].split(',')]
        if len(b)==4 and None not in b and min(b)>0:
            for k in range(4): br[k].append(b[k]); nv[k].append(n[k]) if len(n)==4 and None not in n else 0
        if len(d)>=4 and None not in d[:4] and min(d[:4])>0:
            for k in range(4): dm[k].append(d[k])
    if br[0]:
        print(r[:26].ljust(26),'ANTPOW',' '.join('%5.1f'%a for a in ant),'| br_snr',' '.join('%5.1f'%st.median(b) for b in br),'| dmrs_snr',' '.join('%5.1f'%st.median(b) for b in dm) if dm[0] else '', '| nvar',' '.join('%5.1f'%st.median(b) for b in nv) if nv[0] else '')
