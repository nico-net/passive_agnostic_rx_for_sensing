import sqlite3,sys,statistics
db=sqlite3.connect(sys.argv[1]+'.sqlite')
db.executescript("""create index if not exists ix_rt_corr on CUPTI_ACTIVITY_KIND_RUNTIME(correlationId);create index if not exists ix_k_corr on CUPTI_ACTIVITY_KIND_KERNEL(correlationId);
drop table if exists kx;create table kx as select k.correlationId cid, k.streamId sid, k.greenContextId gc, s.value kname, r.globalTid%16777216 tid, r.start las, r.end lae, k.start ks, k.end ke, k.gridX*k.gridY*k.gridZ blocks from CUPTI_ACTIVITY_KIND_KERNEL k join CUPTI_ACTIVITY_KIND_RUNTIME r on r.correlationId=k.correlationId join StringIds s on s.id=k.shortName;""")
tids=[r[0] for r in db.execute("select t.globalTid%16777216 from ThreadNames t join StringIds s on s.id=t.nameId where s.value like 'passivePdsch%' order by 1")]
print(tids)
allk=db.execute("select kname,tid,gc,lae,ks,ke from kx order by ks").fetchall()
rtk=[k for k in allk if k[1] in tids];dk=[k for k in allk if k[1] not in tids]
def union(iv,a,b):
    t=0;cur=a
    for s,e in sorted(iv):
        s=max(s,cur)
        if e>s: t+=min(e,b)-s;cur=max(cur,min(e,b))
    return t
med={};medq={}
for n in set(k[0] for k in rtk):
    med[n]=statistics.median([k[5]-k[4] for k in rtk if k[0]==n]);medq[n]=statistics.median([k[4]-k[3] for k in rtk if k[0]==n])
tot={};cnt={}
for k in rtk:
    n,tid,gc,lae,ks,ke=k
    ex=max(0,(ke-ks)-med[n]*3)+max(0,(ks-lae)-medq[n]*3-200000)
    if ex<1_000_000: continue
    a,b=lae,ke;win=b-a
    nz=union([(max(x[4],a),min(x[5],b)) for x in dk if x[0]=='normalize' and x[4]<b and x[5]>a],a,b)
    dis=union([(max(x[4],a),min(x[5],b)) for x in dk if x[4]<b and x[5]>a],a,b)
    oth=union([(max(x[4],a),min(x[5],b)) for x in rtk if x[4]<b and x[5]>a and x is not k],a,b)
    if nz>0.5*win: c='csirs normalize'
    elif dis>0.5*win: c='other discovery/NID kernel'
    elif oth>0.3*win: c='RT-vs-RT'
    else: c='GPU idle (host/driver)'
    tot[c]=tot.get(c,0)+ex;cnt[c]=cnt.get(c,0)+1
T=sum(tot.values())
for c,v in sorted(tot.items(),key=lambda kv:-kv[1]): print(f"{c:32s} kernels={cnt[c]:4d} excess_ms={v/1e6:8.1f} share={v/T:.2f}")
print("total excess ms",round(T/1e6,1),"RT kernels",len(rtk))
