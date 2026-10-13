import sys,collections,statistics
sys.path.insert(0,'/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad')
from evm import *
# per run: comb=3 share of Nl=1 grants after an RNTI's 40th decode; CRC by bucket at snr>=10 (Nl=1), all receivers and by receiver
for f in sys.argv[1:]:
    rows=[r for r in parse(f+'/run.log') if r['probe']==0 and r['rnti']!='0xffff' and r['nl']==1]
    cnt=collections.Counter(); steady=[]
    for r in rows:
        cnt[r['rnti']]+=1
        if cnt[r['rnti']]>40: steady.append(r)
    sh=100*sum(1 for r in steady if r['comb']==3)/len(steady) if steady else float('nan')
    out=[]
    for b in ['0-4','5-9','10-16','17+']:
        s=[r for r in rows if bucket(r['mcs'])==b and r['snr']>=10]
        out.append(f'{b}: {100*statistics.mean(r["crc"] for r in s):5.1f}% ({len(s)})' if s else f'{b}: --')
    name=f.split('/')[-1][7:]
    print(f'{name:28s} Nl1={len(rows):5d} rntis={len(cnt):3d} steady comb3 share={sh:5.1f}% ({len(steady)}) | CRC@snr>=10 '+' | '.join(out))
