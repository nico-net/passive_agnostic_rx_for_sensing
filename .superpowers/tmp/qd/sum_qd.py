#!/usr/bin/env python3
"""sum_qd.py <campaign dir>: per-arm table for Q (rr-single1q) vs D (rr-single1d) from <run>/m.json and last ISAC_METRICS."""
import sys,json,re
C=sys.argv[1]; rows=[]
for ln in open(C+'/runs.tsv'):
    t,r,w,ok,arm=ln.rstrip('\n').split('\t')[:5]
    m=json.load(open(r+'/m.json')); log=re.sub(r'\x1b\[[0-9;]*m','',open(r+'/run.log',errors='replace').read())
    met={}
    for l in log.splitlines():
        i=l.find('ISAC_METRICS {')
        if i>=0:
            try: met=json.loads(l[i+13:])
            except ValueError: pass
    d=m['drops']; bs=m['by_layer']
    def v(src,th):
        k='v%d_s'%th; return sum(x.get(k,0) for n,x in bs.items() if n.startswith(src+'/'))
    wu=met.get('sense_write_us',0); wc=met.get('sense_write_count',1)
    rows.append((t,w,'Q' if arm=='base' else 'D',ok,m['dur'],met.get('sense_cfr_drop',0),met.get('sense_cfr_drop_csirs',0),met.get('sense_cfr_drop_pdsch_data',0),met.get('sense_cfr_drop_pdsch_dmrs',0),
      {k:met[k] for k in met if k.startswith('sense_cfr_drop_') and met[k]},sum(m['noqual'].values()),m['noqual'],
      v('pdsch_data',51),v('pdsch_data',128),v('pdsch_dmrs',51),v('pdsch_dmrs',128),m['wide128_dec_s'],m['wide128_crc_s'],m['wide128_part_s'],m['wide51_dec_s'],
      wu/1e6/m['dur'],wu/max(wc,1),m['cpu'],d['sense_rec_drop']))
print('| arm | cell | build | dur s | cfr_drop total | csirs | pdsch_data | pdsch_dmrs | other | rec w/o quality | data v51/s | data v128/s | dmrs v51/s | dmrs v128/s | wide128 dec/s | CRC-ok/s | partial/s | wide51 dec/s | writer cores | writer us/rec | rx cores | rec_drop |')
print('|'+'---|'*22)
for r in rows:
    oth={k[15:]:x for k,x in r[9].items() if k[15:] not in('csirs','pdsch_data','pdsch_dmrs')}
    print('| %s | %s | %s | %.0f | %d | %d | %d | %d | %s | %d | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.2f | %.1f | %.3f | %.1f | %s | %d |'%(r[0],r[1],r[2],r[4],r[5],r[6],r[7],r[8],oth or '-',r[10],r[12],r[13],r[14],r[15],r[16],r[17],r[18],r[19],r[20],r[21],r[22],r[23]))
