#!/usr/bin/env python3
"""ab_sum.py <name> <replay dir>: drops per source, records without quality, >=51/>=128 valid-PRB records per source, writer cost."""
import sys,re,json,collections
name,R=sys.argv[1],sys.argv[2]
log=re.sub(r'\x1b\[[0-9;]*m','',open(R+'/run.log',errors='replace').read())
met={}
for ln in log.splitlines():
    i=ln.find('ISAC_METRICS {')
    if i>=0:
        try: met=json.loads(ln[i+13:])
        except ValueError: pass
src=['ssb','csirs','pdsch_dmrs','pdsch_data','pusch_dmrs','pusch_data','prach','srs']
drop={s:met.get('sense_cfr_drop_'+s,0) for s in src}
noq=collections.Counter(); n51=collections.Counter(); n128=collections.Counter(); tot=collections.Counter()
for ln in open('/home/nicola/NICOLA/wt/rr-single1d/.superpowers/tmp/ab/%s.records.jsonl'%name,errors='replace'):
    if '"type":"rs"' not in ln: continue
    s=re.search(r'"source":"(\w+)"',ln).group(1); tot[s]+=1
    if re.search(r'"cfr_frame_id":(null|0),',ln): noq[s]+=1
    m=re.search(r'"valid_cfr_prbs":(\d+)',ln)
    if m:
        v=int(m.group(1)); n51[s]+= v>=51; n128[s]+= v>=128
rec=met.get('sense_rec_count',0); ru=met.get('sense_rec_us',0)
print(json.dumps({'arm':name,'cfr_drop':met.get('sense_cfr_drop'),'drop_by_src':{k:v for k,v in drop.items() if v},'records_no_cfr':dict(noq),'ge51_per_src':{k:v for k,v in n51.items() if v},'ge128_per_src':{k:v for k,v in n128.items() if v},'records':{k:v for k,v in tot.items()},'writer_rec_us_per_rec':round(ru/rec,2) if rec else None,'sense_write_us':met.get('sense_write_us'),'exported':{s:met.get('sense_cfr_exported_'+s) for s in ('csirs','pdsch_dmrs','pdsch_data','pusch_dmrs')}}))
