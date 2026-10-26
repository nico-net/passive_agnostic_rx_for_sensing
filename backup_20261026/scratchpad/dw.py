import sys,json,glob,collections
d=sys.argv[1]
c=collections.defaultdict(lambda: collections.Counter())
for f in sorted(glob.glob(d+'/export/dci_attempts/part-*.jsonl')):
    for l in open(f):
        try:r=json.loads(l)
        except: continue
        w=int(r['t_s']//20)*20
        c[w][(r['dir'],r['fmt'])]+=1
ks=sorted({k for v in c.values() for k in v})
print('win',ks)
for w in sorted(c): print(w,[c[w][k] for k in ks])
