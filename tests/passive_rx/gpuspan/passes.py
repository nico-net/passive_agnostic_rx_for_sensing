#!/usr/bin/env python3
"""Per-pass reconstruction of DL GPU passes from an nsys sqlite (CUDA + graph-node trace).
A pass = consecutive cudaGraphLaunch calls of one passivePdsch* thread (C, D, L sub-graphs; C absent when skipped).
For every pass: own nodes (kernels/memcpy/memset of its launches), span (first node start -> last node end = gspan
equivalent), busy (union of own nodes), gaps (own idle inside the span) and, for every gap and every own kernel, which
other GPU work was running, classified by the ISSUING THREAD of that work (other DL pass instances, DL non-pass, UL,
discovery, other). Optional: NVTX marks 'gp' with payload = pass seq (merge19 instrumentation) join a pass to EVMGRANT.
usage: passes.py db.sqlite [--out passes.pkl] [--t0 s --t1 s]
"""
import sqlite3, sys, collections, bisect, pickle, argparse

ap = argparse.ArgumentParser()
ap.add_argument('db'); ap.add_argument('--out'); ap.add_argument('--t0', type=float, default=0); ap.add_argument('--t1', type=float, default=1e9); ap.add_argument('--threads', default='passivePdsch')
a = ap.parse_args()
db = sqlite3.connect(a.db); c = db.cursor()
S = {i: s for i, s in c.execute("select id,value from StringIds")}
TN = {g: S[n] for g, n in c.execute("select globalTid,nameId from ThreadNames")}
tbl = {r[0] for r in c.execute("select name from sqlite_master where type='table'")}
T0 = c.execute("select min(start) from CUPTI_ACTIVITY_KIND_KERNEL").fetchone()[0]
lo, hi = T0 + int(a.t0 * 1e9), T0 + int(a.t1 * 1e9)

def cls_thread(n):
    if n.startswith('passivePdsch'): return 'dl'
    if n.startswith('passivePusch'): return 'ul'
    if n.startswith('csirsDisc'): return 'disc_csirs'
    if n.startswith('pdschRecov'): return 'recov'
    return 'other:' + n

# runtime calls: correlationId -> (thread, api name, start, end)
GL = {i for i, s in S.items() if s.startswith('cudaGraphLaunch')}
ER = {i for i, s in S.items() if s.startswith('cudaEventRecord')}
QY = {i for i, s in S.items() if s.startswith('cudaEventQuery') or s.startswith('cudaEventSynchronize') or s.startswith('cuKernelGetName')}
cid_thr = {}
calls = collections.defaultdict(list)  # per DL thread: (start, end, kind, cid)
for s, e, n, cid, g in c.execute("select start,end,nameId,correlationId,globalTid from CUPTI_ACTIVITY_KIND_RUNTIME where start between ? and ?", (lo - 50_000_000, hi)):
    th = TN.get(g, '?')
    cid_thr[cid] = th
    if th.startswith(a.threads) and n not in QY:
        calls[g].append((s, e, 'G' if n in GL else 'E' if n in ER else 'o', cid))
# GPU activities
act = []  # (start, end, name, cid, stream, gctx, gridX)
gnode = []  # parallel to act: graph id (graphNodeId >> 32) or None
for s, e, n, cid, st, gc, gx, gn in c.execute("select start,end,shortName,correlationId,streamId,greenContextId,gridX,graphNodeId from CUPTI_ACTIVITY_KIND_KERNEL where start between ? and ?", (lo - 50_000_000, hi + 50_000_000)):
    act.append((s, e, S[n], cid, st, gc, gx)); gnode.append(gn >> 32 if gn else None)
for s, e, cid, st, gc, b, gn in c.execute("select start,end,correlationId,streamId,greenContextId,bytes,graphNodeId from CUPTI_ACTIVITY_KIND_MEMCPY where start between ? and ?", (lo - 50_000_000, hi + 50_000_000)):
    act.append((s, e, 'memcpy', cid, st, gc, b)); gnode.append(gn >> 32 if gn else None)
for s, e, cid, st, gc, b, gn in c.execute("select start,end,correlationId,streamId,greenContextId,bytes,graphNodeId from CUPTI_ACTIVITY_KIND_MEMSET where start between ? and ?", (lo - 50_000_000, hi + 50_000_000)):
    act.append((s, e, 'memset', cid, st, gc, b)); gnode.append(gn >> 32 if gn else None)
by_cid = collections.defaultdict(list)
for x in act: by_cid[x[3]].append(x)
# conditional-node bodies run device-launched: correlationId 0, but their graphNodeId carries the parent graph's id. Index them by graph id.
body = collections.defaultdict(list)
for x, g in zip(act, gnode):
    if x[3] == 0 and g is not None: body[g].append(x)
for g in body: body[g].sort()
gid_of = {}  # cid -> graph id of its graph-launched nodes
for x, g in zip(act, gnode):
    if x[3] and g is not None: gid_of[x[3]] = g
# NVTX join marks (merge19+): payload = pass seq, on the launching thread just before the launches
marks = collections.defaultdict(list)
if 'NVTX_EVENTS' in tbl:
    cols = [r[1] for r in c.execute("pragma table_info(NVTX_EVENTS)")]
    pcol = 'coalesce(uint64Value,int64Value)' if 'uint64Value' in cols else 'int64Value' if 'int64Value' in cols else None
    if pcol:
        for s, g, v, txt, tid in c.execute(f"select start,globalTid,{pcol},text,textId from NVTX_EVENTS where start between ? and ?", (lo - 50_000_000, hi)):
            t = txt if txt is not None else S.get(tid, '')
            if t == 'gp' and v is not None: marks[g].append((s, v))
for g in marks: marks[g].sort()

# passes
passes = []
for g, L in calls.items():
    L.sort()
    cur = None
    prev = None
    M = [m[0] for m in marks.get(g, [])]  # merge19+: an NVTX 'gp' mark precedes every pass's launches (C, D, L with event markers between them)
    mi = 0
    newmark = False
    for s, e, k, cid in L:
        while mi < len(M) and M[mi] <= s:
            mi += 1; newmark = True  # pending until the next graph launch (the ev0 record comes first)
        if k == 'G':
            if (prev != 'G' and not M) or newmark or cur is None or len(cur['launches']) >= 3:  # a pass is at most C, D, L
                newmark = False
                cur = dict(thr=TN[g], gtid=g, t_launch=s, launches=[])
                passes.append(cur)
            cur['launches'].append((s, e, cid))
        elif k == 'E' and cur is not None and prev == 'G':
            cur['t_done_rec'] = s
        prev = k
passes = [p for p in passes if lo <= p['t_launch'] <= hi]

def kind_of(nodes):
    ns = [n[2] for n in nodes if n[2] not in ('memcpy', 'memset')]
    if not ns: return '?'
    f = ns[0]
    if f.startswith('k_fep_exact') or f.startswith('k_delay_exact'): return 'C'
    if f.startswith('k_chest_post') or f.startswith('k_gate'): return 'D'
    if f.startswith('k_rdm') or 'ldpc' in f: return 'L'
    if f.startswith('k_ul'): return 'U'
    return '?' + f

own_cids = set()
# per graph id: the first-node start of every launch, to bound a launch's body by the next launch of the same exec
starts = collections.defaultdict(list)
for cid, g in gid_of.items():  # every launch of every exec, whatever thread launched it
    if by_cid.get(cid): starts[g].append(min(n[0] for n in by_cid[cid]))
next_launch = {}
body_owner = {}
for g, L in starts.items():
    L.sort()
    for a_, b_ in zip(L, L[1:] + [1 << 62]): next_launch[(g, a_)] = b_
own_cids = set()
for p in passes:
    subs = []
    pos = {3: 'CDL', 2: 'DL', 1: 'F'}.get(len(p['launches']), 'X' * len(p['launches']))  # split pass: C, D, L in launch order (C skipped on a held instance)
    for i, (s, e, cid) in enumerate(p['launches']):
        nodes = by_cid.get(cid, [])
        own_cids.add(cid)
        g = gid_of.get(cid)
        if g is not None and nodes and body.get(g):  # the IF body of this launch: device-launched nodes of the same graph after its first node, before the exec's next launch
            B = body[g]; t_a = min(n[0] for n in nodes)
            nxt = next_launch.get((g, t_a), 1 << 62)
            i0 = bisect.bisect_left(B, (t_a,))
            j = i0
            while j < len(B) and B[j][0] < nxt: j += 1
            nodes = nodes + [n[:3] + (('body', g),) + n[4:] for n in B[i0:j]]
            for n in B[i0:j]: body_owner[(n[0], n[4])] = p['thr']
        nodes = sorted(nodes)
        subs.append(dict(kind=pos[i], call=(s, e), nodes=nodes))
    p['subs'] = subs
    alln = sorted(n for sb in subs for n in sb['nodes'])
    p['nodes'] = alln
    if g in marks: pass
# other activity classified by issuing thread
cat_iv = collections.defaultdict(list)
for x in act:
    th = cid_thr.get(x[3], '?')
    if x[3] == 0 and (x[0], x[4]) in body_owner: k = 'dl_pass@' + body_owner[(x[0], x[4])]
    else:
        k = cls_thread(th)
        if k == 'dl':
            k = 'dl_pass@' + th if x[3] in own_cids else 'dl_nonpass'
    cat_iv[k].append((x[0], x[1], x[3]))
CATS = sorted(cat_iv)

class Cov:
    """union coverage of a set of intervals, queried as covered length within [a, b]"""
    def __init__(s, iv):
        iv = sorted((a, b) for a, b, _ in iv)
        m = []
        for a_, b_ in iv:
            if m and a_ <= m[-1][1]: m[-1][1] = max(m[-1][1], b_)
            else: m.append([a_, b_])
        s.st = [x[0] for x in m]; s.en = [x[1] for x in m]
        s.cum = [0]
        for x in m: s.cum.append(s.cum[-1] + x[1] - x[0])
    def cov(s, a, b):
        def upto(t):
            i = bisect.bisect_right(s.st, t)
            if i == 0: return 0
            return s.cum[i - 1] + min(t, s.en[i - 1]) - s.st[i - 1]
        return max(0, upto(b) - upto(a))
COV = {k: Cov(v) for k, v in cat_iv.items()}
COV_ALL = Cov([x for v in cat_iv.values() for x in v])
# per-cid coverage for dl_pass excluding own: coverage(dl_pass) minus own covered part (approximated by excluding own via separate cov)

def merge(iv):
    m = []
    for a_, b_ in sorted(iv):
        if m and a_ <= m[-1][1]: m[-1][1] = max(m[-1][1], b_)
        else: m.append([a_, b_])
    return m

out = []
for p in passes:
    N = p['nodes']
    if not N: continue
    st0, en = N[0][0], max(n[1] for n in N)
    busy_iv = merge([(n[0], n[1]) for n in N])
    busy = sum(b - a_ for a_, b in busy_iv)
    gaps = [(busy_iv[i][1], busy_iv[i + 1][0]) for i in range(len(busy_iv) - 1)]
    own = Cov([(n[0], n[1], 0) for n in N])
    def others(a_, b_):
        d = {}
        for k in CATS:
            if k == 'dl_pass@' + p['thr']: continue  # own thread: one pass at a time, so this is the pass itself
            v = COV[k].cov(a_, b_)
            if k.startswith('dl_pass@'): d['dl_pass_other'] = max(d.get('dl_pass_other', 0), v)  # max over threads: lower bound of their union
            else: d[k] = v
        return d
    gap_rec = []
    for a_, b_ in gaps:
        # which sub-graph boundary? gap between last node of sub i and first of sub i+1
        o = others(a_, b_); o['ANY'] = COV_ALL.cov(a_, b_)
        gap_rec.append((a_, b_, o))
    sub_rec = []
    for sb in p['subs']:
        if not sb['nodes']: continue
        sub_rec.append((sb['kind'], sb['call'][0], sb['call'][1], sb['nodes'][0][0], max(n[1] for n in sb['nodes']), len(sb['nodes'])))
    node_rec = [(n[0], n[1], n[2], n[4], n[5], n[6], others(n[0], n[1])) for n in N]
    seq = None
    if p['gtid'] in marks:
        L = marks[p['gtid']]
        i = bisect.bisect_right(L, (p['t_launch'], 1 << 62)) - 1
        if i >= 0 and p['t_launch'] - L[i][0] < 2_000_000: seq = L[i][1]
    out.append(dict(thr=p['thr'], t_launch=p['t_launch'], t0=st0, t1=en, span=en - st0, busy=busy, l2s=st0 - p['t_launch'], subs=sub_rec, gaps=gap_rec, nodes=node_rec, seq=seq, isbody=[isinstance(n[3], tuple) for n in N],
                    ncb=max([n[6] for n in N if n[2].startswith('k_ldpc')] or [0])))
CATS = sorted({k for k in CATS if not k.startswith('dl_pass@')} | {'dl_pass_other'})
print('passes', len(out), 'with seq', sum(1 for o in out if o['seq'] is not None), 'cats', CATS, file=sys.stderr)
if a.out:
    pickle.dump(dict(T0=T0, passes=out, cats=CATS), open(a.out, 'wb'))
