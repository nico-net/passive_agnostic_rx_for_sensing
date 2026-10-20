import sqlite3,sys,json
from collections import defaultdict
agg=defaultdict(lambda: defaultdict(list))
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<4: continue
    db=sqlite3.connect(p[3]+'/nsys_trace.sqlite')
    n=sum(1 for r in json.load(open(p[3]+'/nsys_trace.sqlite.rows.json')) if 'body' in r)
    for nm,tot in db.execute("""select s.value, sum(k.end-k.start) from CUPTI_ACTIVITY_KIND_KERNEL k join CUPTI_ACTIVITY_KIND_RUNTIME r on r.correlationId=k.correlationId join StringIds s on s.id=k.shortName
        where r.globalTid in (select globalTid from ThreadNames t join StringIds s2 on s2.id=t.nameId where s2.value like 'passivePdsch%' or s2.value='pdschRecov') group by 1"""):
        agg[(p[0],p[1])][nm.split('<')[0][:20]].append(tot/1000/n)
for cap in ('b','tim'):
    names=set()
    for arm in ('OFF','ON','FP32'): names|=set(agg[(cap,arm)])
    tot={arm:sum(sum(v)/len(v) for v in agg[(cap,arm)].values()) for arm in ('OFF','ON','FP32')}
    print(cap,'us per real grant (mean of 2 runs): total',{k:round(v) for k,v in tot.items()})
    rows=[(nm,*[sum(agg[(cap,a)][nm])/max(1,len(agg[(cap,a)][nm])) for a in ('OFF','ON','FP32')]) for nm in names]
    for r in sorted(rows,key=lambda r:-max(r[1:]))[:16]: print('  %-20s OFF %5.0f ON %5.0f FP32 %5.0f'%r)
