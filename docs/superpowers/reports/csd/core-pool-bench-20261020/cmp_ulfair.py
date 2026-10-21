# appends pool-before / pool-dlcap / pool-now / cfs comparison to summary_table_ulfair.txt (run after: python3 summ.py . raw-ulfair ulfair)
import csv
rd=lambda f:{(r['mode'],int(r['N']),float(r['scale'])):r for r in csv.DictReader(open(f))}
B=rd('summary_medians.csv');D=rd('summary_medians_dlcap.csv');U=rd('summary_medians_ulfair.csv')
f=lambda r,k:('%.1f' if k.endswith('miss') else '%d')%float(r[k])
o=['','[Bench, cloud x86] before (18810db, DL cap N-2) / dlcap (95e8458) / now (UL-fair DL cap) / cfs (this run). medians, us; miss in %. token = CORE_POOL histogram bin upper bound.']
for k,t in(('dl_w50','DL wait p50'),('dl_w99','DL wait p99'),('ul_w50','UL wait p50'),('ul_w99','UL wait p99'),('scan_w50','scan wait p50'),('scan_w99','scan wait p99'),('td99','decode token wait p99<='),('ts99','scan token wait p99<='),('dl_miss','DL miss %'),('ul_miss','UL miss %'),('scan_miss','scan miss %')):
  o+=['','%s: N scale | before / dlcap / now / cfs'%t]
  for N in(2,3):
    for s in(0.3,0.5,1.0,1.5):
      rs=[B[('pool',N,s)],D[('pool',N,s)],U[('pool',N,s)],U[('cfs',N,s)]]
      o.append('  %d %.1f | %s'%(N,s,' / '.join(f(r,k) if k in r else '-' for r in rs)))
open('summary_table_ulfair.txt','a').write('\n'.join(o)+'\n');print('\n'.join(o))
