import sys,re,glob,gzip,json
ANSI=re.compile(r'\x1b\[[0-9;]*m'); KV=re.compile(r'(\w+)=([^ ]+)')
r=sys.argv[1]
out=gzip.open('out/'+r+'.full.gz','wt')
keys=None
with open(glob.glob('/home/sens/NICOLA/captures/'+r+'/*/run.log')[0],errors='replace') as f:
    for line in f:
        if 'EVMGRANT' not in line: continue
        d=dict(KV.findall(ANSI.sub('',line)))
        if d.get('probe')!='0' or d.get('rnti')=='0xffff' or 'rb' not in d: continue
        m=re.search(r'EVMGRANT rnti=\S+ (\S+)',line); d['t']=m.group(1)
        out.write(json.dumps(d)+'\n')
out.close()
