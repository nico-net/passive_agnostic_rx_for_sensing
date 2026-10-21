# appends the pool-before / pool-now / cfs comparison to summary_table_dlcap.txt (pool-before = summary_medians.csv, 95e8458^ ; now + cfs = summary_medians_dlcap.csv)
import csv
rd=lambda f:{(r['mode'],int(r['N']),float(r['scale'])):r for r in csv.DictReader(open(f))}
B=rd('summary_medians.csv');A=rd('summary_medians_dlcap.csv')
def f(r,k):
    v=float(r[k]);return '%d'%v if k[-4:]!='miss' else '%.1f'%v
o=['','[Bench, cloud x86] DL/scan/UL comparison, medians. before = pool at 18810db (DL cap N-2), now = pool at 95e8458, cfs = this run (cfs of the earlier run in summary_table.txt). us; miss in %.',
   'N scale | DL w50 (before/now/cfs) | DL w99 | DL token w99<= | DL miss% | scan w99 | UL w99 | UL miss%%']
for N in(2,3):
  for s in(0.3,0.5,1.0,1.5):
    b,n,c=B[('pool',N,s)],A[('pool',N,s)],A[('cfs',N,s)]
    t=lambda k,tk=None:' / '.join(f(r,k) for r in(b,n,c)) if k!='td99' else ' / '.join(f(r,k) for r in(b,n))
    o.append('%d %.1f | %s | %s | %s | %s | %s | %s | %s'%(N,s,t('dl_w50'),t('dl_w99'),t('td99'),t('dl_miss'),t('scan_w99'),t('ul_w99'),t('ul_miss')))
open('summary_table_dlcap.txt','a').write('\n'.join(o)+'\n');print('\n'.join(o))
