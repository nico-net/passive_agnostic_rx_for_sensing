import sqlite3,sys,statistics
db=sqlite3.connect(sys.argv[1])
tids=[r[0] for r in db.execute("select t.globalTid%16777216 from ThreadNames t join StringIds s on s.id=t.nameId where s.value like 'passivePdsch%' order by 1")]
for kn in ('k_fir','k_ls','k_delay_exact'):
  print("==",kn)
  res={'new_hwm':[], 'not':[]}
  for tid in tids:
    hwm=0
    for blocks,ks,ke in db.execute("select blocks,ks,ke from kx where kname=? and tid=? order by ks",(kn,tid)):
        d=(ke-ks)/1e3
        key='new_hwm' if blocks>hwm else 'not'
        res[key].append((blocks,d))
        hwm=max(hwm,blocks)
  for key,v in res.items():
    ds=[d for b,d in v]
    if not ds: continue
    ds.sort()
    print(f"  {key:8s} n={len(ds):6d} p50={ds[len(ds)//2]:8.0f} p90={ds[int(.9*len(ds))]:8.0f} max={ds[-1]:8.0f} us; >5ms: {sum(d>5000 for d in ds)}  ; >1ms: {sum(d>1000 for d in ds)}")
  # the slow ones that are NOT new hwm: list
  sl=[(b,d) for b,d in res['not'] if d>5000]
  print("  slow-not-hwm (blocks,us):",sorted(sl)[:20])
  nh=[(b,d) for b,d in res['new_hwm']]
  print("  new_hwm (blocks,us):",[(b,round(d)) for b,d in nh][:60])
