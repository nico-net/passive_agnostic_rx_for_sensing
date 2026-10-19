import sqlite3,pickle,sys
c=sqlite3.connect(sys.argv[1]).cursor(); P=pickle.load(open(sys.argv[2],'rb'))['passes']
S={i:s for i,s in c.execute("select id,value from StringIds")}
for p in P:
    subs=p['subs']
    if len(subs)<2: continue
    for a in range(len(subs)-1):
        g=subs[a+1][3]-subs[a][4]
        if g>20_000_000:
            t0=subs[a][4]
            print(f"--- {p['thr']} seq {p['seq']}: {subs[a][0]} GPU end 0 ; next {subs[a+1][0]} host launch call {(subs[a+1][1]-t0)/1e3:.0f}..{(subs[a+1][2]-t0)/1e3:.0f} us, its first node {(subs[a+1][3]-t0)/1e3:.0f} us")
            for s,e,n,st in c.execute("select start,end,shortName,streamId from CUPTI_ACTIVITY_KIND_KERNEL where end>? and start<? and end-start>1000000",(t0,subs[a+1][3])):
                print(f"     long kernel {S[n]} {(s-t0)/1e3:.0f}..{(e-t0)/1e3:.0f} us stream {st}")
            gt=[g for g,in c.execute("select globalTid from ThreadNames t join StringIds s on s.id=t.nameId where s.value=?",(p['thr'],))]
            for s,e,n in c.execute(f"select start,end,nameId from CUPTI_ACTIVITY_KIND_RUNTIME where globalTid in ({','.join(map(str,gt))}) and end>? and start<? and end-start>200000",(t0,subs[a+1][3])):
                print(f"     host API {S[n]} {(s-t0)/1e3:.0f}..{(e-t0)/1e3:.0f} us")
