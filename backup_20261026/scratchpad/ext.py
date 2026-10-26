import sys,re,glob,gzip,json,os
ANSI=re.compile(r'\x1b\[[0-9;]*m')
KV=re.compile(r'(\w+)=([^ ]+)')
OUT='/tmp/claude-1000/-home-sens-NICOLA-adaptive-rx-UL-DL/6de2bc9d-5f38-466a-9776-f15a57560647/scratchpad/out/'
def fl(s):
    try: return [float(x) for x in s.split(',')]
    except: return []
for rd in sys.argv[1:]:
    name=os.path.basename(rd.rstrip('/'))
    logs=glob.glob(rd+'/*/run.log')
    if not logs: continue
    info={'run':name,'ant':[], 'rf':[], 'chp':[], 'sfo':None,'sfodm':None,'gp':{}, 'pdq':None,'pq':None,'timemut':0,'lmx':0,'lines':0,'adc':None,'recov':0,'lost':0,'cfo':[]}
    dl=gzip.open(OUT+name+'.dl.gz','wt'); ul=gzip.open(OUT+name+'.ul.gz','wt')
    with open(logs[0],errors='replace') as f:
        for line in f:
            info['lines']+=1
            if 'EVMGRANT' in line:
                d=dict(KV.findall(ANSI.sub('',line)))
                if d.get('probe')!='0' or d.get('rnti')=='0xffff' or 'rb' not in d: continue
                m=re.search(r'EVMGRANT rnti=\S+ ([\d.]+)',line); t=m.group(1) if m else ''
                try: s,n=d['rb'].split('+')
                except: continue
                dm=d.get('dmrs_snr','')
                dl.write('\t'.join([t,d['rnti'],d['crc'],d['mcs'],d['Nl'],s,n,d.get('nsym',''),d.get('snr',''),d.get('snr_cpe',''),d.get('br_snr',''),d.get('snr_l',''),d.get('nvar_db',''),dm,d.get('rho',''),d.get('cb',''),d.get('comb',''),d.get('tbs',''),d.get('evm','')])+'\n')
            elif 'UL_PUSCH_CRC' in line:
                d=dict(KV.findall(ANSI.sub('',line)))
                if d.get('probe')!='0': continue
                try: s,n=d['prb'].split('+')
                except: continue
                ul.write('\t'.join([d['rnti'],d['ok'],d.get('mcs',''),s,n,d.get('snr_db',''),d.get('status',''),d.get('seg',''),d.get('tbs',''),d.get('skip','')])+'\n')
            elif 'SENSING: ANTPOW' in line:
                m=re.search(r'dB=\[([^\]]*)\]',line); r=re.search(r'raw=\[([^\]]*)\]',line)
                if m: info['ant'].append([float(x) for x in m.group(1).split()])
            elif 'SENSING: RFPOW' in line:
                info['rf'].append([float(x) for x in re.findall(r'rf=(-?[\d.]+)dBm',line)])
            elif 'SENSING: CHPOW' in line:
                info['chp'].append([float(x) for x in re.findall(r'ch\d=(-?[\d.]+)',line)])
            elif 'SENSING: SFO eps=' in line:
                m=re.search(r'eps=([+-][\d.]+)',line); info['sfo']=float(m.group(1)) if m else info['sfo']
            elif 'SENSING: SFO_DMRS' in line:
                m=re.search(r'applied eps ([+-][\d.]+)',line); info['sfodm']=float(m.group(1)) if m else info['sfodm']
                m=re.search(r'DC ([+-][\d.]+) Hz',line)
                if m: info['cfo'].append(float(m.group(1)))
            elif 'SENSING: GPU_PATH' in line:
                m=re.search(r'path=(\w+) tier=\w+ choice=(\w+)',line)
                if m: info['gp'][m.group(1)]=m.group(2)
            elif 'PDSCHQ queued' in line: info['pq']=ANSI.sub('',line)[ANSI.sub('',line).find('PDSCHQ'):][:200].strip()
            elif 'SENSING: TIMEMUT' in line: info['timemut']+=1
            elif 'LMX2572' in line: info['lmx']+=1
            elif 'SENSING: ADCCLASS' in line: info['adc']=ANSI.sub('',line)[:700]
            elif 'CELL_LOST' in line or 'ACQ_STATE' in line and 'LOST' in line: info['lost']+=1
    dl.close(); ul.close()
    json.dump(info,open(OUT+name+'.json','w'))
