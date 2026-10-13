import sqlite3,sys
db=sqlite3.connect(sys.argv[1])
TH=int(float(sys.argv[2])*1e6) if len(sys.argv)>2 else 20_000_000
tids=[r[0] for r in db.execute("select t.globalTid%16777216 from ThreadNames t join StringIds s on s.id=t.nameId where s.value like 'passivePdsch%' order by 1")]
tn={r[0]:r[1] for r in db.execute("select t.globalTid%16777216,s.value from ThreadNames t join StringIds s on s.id=t.nameId")}
print("pdsch tids",tids)
q=",".join(map(str,tids))
rows=db.execute(f"select cid,kname,tid,gc,lae,ks,ke from kx where tid in ({q}) and (ks-lae>{TH} or ke-ks>{TH}) order by lae").fetchall()
eps=[];cur=None
for r in rows:
    s,e=r[4],r[6]
    if cur and s<cur[1]+5_000_000: cur[1]=max(cur[1],e);cur[2].append(r)
    else: cur=[s,e,[r]];eps.append(cur)
print("episodes",len(eps),"victims",len(rows))
t00=db.execute("select min(las) from kx").fetchone()[0]
def union(iv,a,b):
    t=0;cur=a
    for s,e in sorted(iv):
        s=max(s,cur)
        if e>s: t+=min(e,b)-s;cur=max(cur,min(e,b))
    return t
for s,e,v in eps:
    span=e-s
    print(f"\n=== episode t={(s-t00)/1e9:.3f}s span={span/1e6:.1f}ms victims={len(v)} vtids={sorted(set(x[2] for x in v))} first_victim={v[0][1]} ks-lae={(v[0][5]-v[0][4])/1e6:.1f}ms exec={(v[0][6]-v[0][5])/1e6:.1f}ms")
    ks=db.execute("select kname,tid,gc,ks,ke,blocks from kx where ke>? and ks<?",(s,e)).fetchall()
    busy=union([(max(k[3],s),min(k[4],e)) for k in ks],s,e)
    print(f"  GPU busy {busy/span*100:.0f}% of window; kernels in window {len(ks)}")
    agg={}
    for k in ks:
        key=(k[0],tn.get(k[1],k[1]),k[2]); a=agg.setdefault(key,[0,0,0]); a[0]+=1; a[1]+=min(k[4],e)-max(k[3],s); a[2]=max(a[2],k[4]-k[3])
    for key,a in sorted(agg.items(),key=lambda kv:-kv[1][1])[:6]: print(f"   {key[0]:28s} {str(key[1]):22s} gc={key[2]} n={a[0]:5d} cover_ms={a[1]/1e6:7.1f} maxexec_ms={a[2]/1e6:7.1f}")
    print("  cuda api >1ms (any thread):")
    for r in db.execute("select s.value,r.globalTid%16777216,(r.start-?)/1e6,(r.end-r.start)/1e6 from CUPTI_ACTIVITY_KIND_RUNTIME r join StringIds s on s.id=r.nameId where r.end>? and r.start<? and r.end-r.start>1000000 order by r.start",(s,s,e)): print(f"   {r[0]:40s} {tn.get(r[1],r[1])!s:22s} +{r[2]:7.1f}ms dur {r[3]:7.1f}ms")
    print("  osrt >2ms (any thread, excluding waits):")
    try:
      for r in db.execute("select s.value,o.globalTid%16777216,(o.start-?)/1e6,(o.end-o.start)/1e6 from OSRT_API o join StringIds s on s.id=o.nameId where o.end>? and o.start<? and o.end-o.start>2000000 and s.value not in ('sem_wait','pthread_cond_wait','pthread_cond_timedwait','sleep','nanosleep','usleep','epoll_wait','poll','select','pselect6','read','recvmsg','accept','waitpid') order by o.start limit 25",(s,s,e)): print(f"   {r[0]:40s} {tn.get(r[1],r[1])!s:22s} +{r[2]:7.1f}ms dur {r[3]:7.1f}ms")
    except Exception as ex: print("   (no osrt)",ex)
    print("  memcpy/memset in window:",db.execute("select count(*),coalesce(sum(end-start),0)/1e6 from CUPTI_ACTIVITY_KIND_MEMCPY where end>? and start<?",(s,e)).fetchone(), db.execute("select count(*) from CUPTI_ACTIVITY_KIND_MEMSET where end>? and start<?",(s,e)).fetchone())
