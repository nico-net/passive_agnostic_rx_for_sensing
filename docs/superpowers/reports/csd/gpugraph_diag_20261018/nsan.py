#!/usr/bin/env python3
"""DIAG 2026-10-18: per-grant end-to-end decomposition from an nsys sqlite of an instrumented replay (DIAGTMP NVTX:
G marks with e2e, dec/body ranges, pprep/pwait in the pass, scan ranges, p2wait). usage: nsan.py trace.sqlite [label]"""
import sqlite3, sys, re, bisect, json
from collections import defaultdict
db = sqlite3.connect(sys.argv[1]); lab = sys.argv[2] if len(sys.argv) > 2 else ''
S = dict(db.execute('select id, value from StringIds'))
nv = defaultdict(list); marks = []
for s, e, t, tid, tidx in db.execute('select start, end, text, globalTid, textId from NVTX_EVENTS'):
    t = t if t is not None else S.get(tidx, '')
    if not t: continue
    if e is None or e == s:
        if t.startswith('G '): marks.append((s, tid, t))
    else: nv[tid].append((s, e, t))
for v in nv.values(): v.sort()
rt = defaultdict(list); corr_api = {}
for s, e, tid, c, n in db.execute('select start, end, globalTid, correlationId, nameId from CUPTI_ACTIVITY_KIND_RUNTIME'):
    nm = S.get(n, '')
    if 'GraphLaunch' in nm or 'LaunchKernel' in nm or 'Memcpy' in nm or 'Memset' in nm:
        rt[tid].append((s, e, c, nm)); corr_api[c] = (s, e, tid, nm)
for v in rt.values(): v.sort()
K = []  # all kernels
kc = defaultdict(list)
for s, e, c, n, st in db.execute('select start, end, correlationId, shortName, streamId from CUPTI_ACTIVITY_KIND_KERNEL'):
    nm = re.sub(r'<.*', '', S.get(n, '?'))[:24]
    K.append((s, e, st, nm, c)); kc[c].append((s, e, nm, st))
K.sort(); Ks = [k[0] for k in K]
# GPU busy union, per-thread origin
def union(iv):
    iv = sorted(iv); tot = 0; cur = None
    for a, b in iv:
        if cur is None: cur = [a, b]
        elif a <= cur[1]: cur[1] = max(cur[1], b)
        else: tot += cur[1] - cur[0]; cur = [a, b]
    return tot + (cur[1] - cur[0] if cur else 0)
def kin(a, b):  # kernels overlapping [a,b]
    i = bisect.bisect_left(Ks, a - 10_000_000)
    out = []
    for k in K[i:]:
        if k[0] > b: break
        if k[1] > a: out.append(k)
    return out
def rin(tid, a, b, pref):
    return [x for x in nv.get(tid, []) if x[0] >= a - 1000 and x[1] <= b + 1000 and x[2].startswith(pref)]
scan = defaultdict(list)
for tid, v in nv.items():
    for s, e, t in v:
        if t.startswith('scan a='): scan[int(t[7:])].append((s, e, tid))
decs = {tid: [x for x in v if x[2].startswith('dec ')] for tid, v in nv.items()}
decs = {k: v for k, v in decs.items() if v}
decs_s = {k: [y[0] for y in v] for k, v in decs.items()}
E2E = ('a2e', 'qw', 'pre', 'dec', 'first', 'bodies', 'tot', 'lag')
rows = []
for ms, tid, t in marks:
    m = re.search(r'a=(\d+) rb=(\d+)\+(\d+) nl=(\d+) off=(\d+) e2e=([-\d,]+)', t)
    if not m: continue
    a, nrb, nl, off = int(m.group(1)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    e = dict(zip(E2E, (int(x) for x in m.group(6).split(','))))
    if e['tot'] < 0: continue
    stg = dict(re.findall(r'(fep|chest|demod|ldpc):(\d+)', t))
    e['stage'] = sum(int(v) for v in stg.values()) > 200
    tend = ms - off; tarr = tend - e['tot'] * 1000; tenq = tarr + e['a2e'] * 1000; td0 = tend - e['dec'] * 1000
    dr = [x for x in nv.get(tid, []) if x[2].startswith('dec ') and abs(x[1] - tend) < 200_000]
    if not dr or not dr[-1][2].endswith('p=0'): continue
    r = dict(e, a=a, nrb=nrb, nl=nl, tid=tid, tarr=tarr, tend=tend)
    mw = re.search(r'wq:(\d+)', t); r['wq'] = int(mw.group(1)) if mw else None
    tq = tend - (e['dec'] + e['pre']) * 1000
    r['busy_cons'] = sum(1 for tt, v in decs.items() if tt != tid and any(x[0] <= tenq <= x[1] for x in v[bisect.bisect_left(decs_s[tt], tenq - 30_000_000):bisect.bisect_right(decs_s[tt], tenq)]))
    # a2e split: the scan occasion of slot a that started before the enqueue
    sc = [x for x in scan.get(a, []) if x[0] <= tenq + 1000]
    if sc:
        s0, s1, stid = max(sc)
        r['scan_wait'] = (s0 - tarr) / 1000; r['scan_to_enq'] = (tenq - s0) / 1000
        r['p2wait'] = sum(x[1] - x[0] for x in rin(stid, s0, min(s1, tenq), 'p2wait')) / 1000
        r['scan_run'] = (s1 - s0) / 1000
    bodies = [x for x in rin(tid, td0, tend, 'body:')]
    if not bodies: rows.append(r); continue
    b0, b1, bt = bodies[0]
    r['body'] = (b1 - b0) / 1000
    pp = rin(tid, b0, b1, 'pprep'); pw = rin(tid, b0, b1, 'pwait')
    api = [x for x in rt.get(tid, []) if b0 <= x[0] <= b1]
    gls = [x for x in api if 'GraphLaunch' in x[3]]
    r['path'] = 'pass' if (pw and gls) else 'stage'
    own = [k for x in api for k in kc.get(x[2], [])]
    if not own: rows.append(r); continue
    ks0 = min(k[0] for k in own); ke1 = max(k[1] for k in own)
    r['own_busy'] = union([(k[0], k[1]) for k in own]) / 1000
    r['own_span'] = (ke1 - ks0) / 1000
    owns = set((k[0], k[1], k[3]) for k in own)
    oth = [k for k in kin(ks0, ke1) if (k[0], k[1], k[3]) not in owns]
    r['oth_busy'] = union([(max(k[0], ks0), min(k[1], ke1)) for k in oth]) / 1000
    r['oth_names'] = defaultdict(float)
    for k in oth: r['oth_names'][k[3]] += (min(k[1], ke1) - max(k[0], ks0)) / 1000
    if r['path'] == 'pass':
        first_api = min(x[0] for x in gls)
        r['host_pre'] = (first_api - b0) / 1000
        r['pprep'] = sum(x[1] - x[0] for x in pp) / 1000
        # sub-graphs: graph launches of the first pass in this body (C,D,L or D,L)
        w0 = pw[0]
        pe = pp[0][1] if pp else b0
        g1 = [x for x in gls if pe - 1000 <= x[0] <= w0[0]][-3:]
        names = ['C', 'D', 'L'][-len(g1):] if len(g1) <= 3 else ['?'] * len(g1)
        prev_end = 0
        for nmx, g in zip(names, g1):
            kk = kc.get(g[2], [])
            if not kk: continue
            s_ = min(k[0] for k in kk); e_ = max(k[1] for k in kk)
            ready = max(g[0], prev_end); r['api_' + nmx] = (g[1] - g[0]) / 1000
            r['q_' + nmx] = (s_ - ready) / 1000  # queue wait: launch called (or previous sub-graph done) -> first kernel
            r['x_' + nmx] = (e_ - s_) / 1000     # exec span
            r['b_' + nmx] = union([(k[0], k[1]) for k in kk]) / 1000
            prev_end = e_
        r['poll_lat'] = (w0[1] - prev_end) / 1000 if prev_end else None  # last kernel end -> wait loop exit
        r['wait'] = (w0[1] - w0[0]) / 1000
        r['host_post'] = (b1 - w0[1]) / 1000
        r['npass'] = len(pw)
    else:
        r['host_gap'] = r['body'] - r['own_busy']
    # concurrency at the body: other bodies in flight (any thread)
    rows.append(r)
# GPU busy timeline: per 500 us bin, union busy fraction
t0 = K[0][0] if K else 0
def busy_frac(a, b):
    return union([(max(k[0], a), min(k[1], b)) for k in kin(a, b)]) / max(1, b - a)
for r in rows:
    if 'tarr' in r: r['gpu_busy_life'] = busy_frac(r['tarr'], r['tend'])
json.dump([{k: (dict(v) if isinstance(v, defaultdict) else v) for k, v in r.items()} for r in rows], open(sys.argv[1] + '.rows.json', 'w'))
# whole-run GPU busy %
if K:
    span = K[-1][1] - K[0][0]
    allb = union([(k[0], k[1]) for k in K])
    bins = defaultdict(list)
    for k in K: bins[k[2]].append((k[0], k[1]))
    print(f'{lab} GPU busy (any kernel) {100*allb/span:.1f} % of {span/1e9:.1f} s; per stream top:', ', '.join(f'{s}:{100*union(v)/span:.1f}%' for s, v in sorted(bins.items(), key=lambda z: -union(z[1]))[:8]))
print(lab, 'grants', len(rows), 'with body', sum('body' in r for r in rows))
