#!/usr/bin/env python3
"""Report on passes.pkl: span decomposition (own kernel busy vs gaps; per sub-graph; inter-sub-graph gaps), what ran in the
gaps, per-kernel stretch vs solo, and per-pass timelines of typical (p45-p55 span) and tail (>= p99) passes.
usage: passrep.py passes.pkl [--log run.log] [--n 3]
With --log (merge19 instrumentation) passes are joined to EVMGRANT by seq (gp 11th value) and split real/probe/crc."""
import pickle, sys, statistics as st, collections, argparse, re
ap = argparse.ArgumentParser(); ap.add_argument('pkl'); ap.add_argument('--log'); ap.add_argument('--n', type=int, default=3); ap.add_argument('--select', default='all')
a = ap.parse_args()
D = pickle.load(open(a.pkl, 'rb')); P = D['passes']; CATS = D['cats']
def q(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')
us = lambda ns: ns / 1e3
EV = {}
if a.log:
    KV = re.compile(r'(\w+)=(\S+)')
    for l in open(a.log, errors='replace'):
        if 'EVMGRANT' not in l: continue
        k = dict(KV.findall(re.sub(r'\x1b\[[0-9;]*m', '', l)))
        if 'gq' in k:
            gq = [int(x) for x in k['gq'].split(',')]
            if gq[3]: EV[gq[3]] = k
    for p in P:
        p['ev'] = EV.get(p['seq'])
    print(f"joined {sum(1 for p in P if p.get('ev'))} of {len(P)} passes to EVMGRANT lines ({len(EV)} lines with seq)")
def sel(p):
    if a.select == 'all': return True
    e = p.get('ev')
    if a.select == 'real': return bool(e) and e.get('probe', '0') == '0'
    if a.select == 'probe': return bool(e) and e.get('probe', '0') != '0'
    if a.select == 'crcfail': return bool(e) and e.get('probe', '0') == '0' and e.get('crc') == '0'
    if a.select == 'crcok': return bool(e) and e.get('probe', '0') == '0' and e.get('crc') == '1'
    return True
P = [p for p in P if sel(p)]
full = [p for p in P if [s[0] for s in p['subs']] == ['C', 'D', 'L']]
print(f"passes {len(P)}; C+D+L {len(full)}; kinds {collections.Counter(''.join(s[0] for s in p['subs']) for p in P).most_common(6)}")
def decomp(p):
    subs = {s[0]: s for s in p['subs']}
    r = dict(span=us(p['span']), busy=us(p['busy']), gap=us(p['span'] - p['busy']), l2s=us(p['l2s']), ngap=len(p['gaps']), ncb=p['ncb'])
    for k in 'CDL':
        if k in subs: r['sub' + k] = us(subs[k][4] - subs[k][3])
    if 'C' in subs and 'D' in subs: r['gCD'] = us(subs['D'][3] - subs['C'][4])
    if 'D' in subs and 'L' in subs: r['gDL'] = us(subs['L'][3] - subs['D'][4])
    gc = collections.Counter()
    for g0, g1, o in p['gaps']:
        d = g1 - g0
        gc['gap_GPUidle'] += d - o.get('ANY', 0)
        for k, v in o.items(): gc['gap_' + k] += v
    for k, v in gc.items(): r[k] = us(v)
    nd = p['nodes']
    for gname, tag in (('k_gate_d', 'D'), ('k_gate_l', 'L')):
        gi = [i for i, n in enumerate(nd) if n[2].startswith(gname)]
        if not gi: continue
        g = nd[gi[0]]
        bod = [n for i, n in enumerate(nd) if p['isbody'][i] and n[0] >= g[1] and (tag == 'L' or not any(m[2].startswith('k_gate_l') and m[0] <= n[0] for m in nd))]
        if bod:
            r['if' + tag] = us(bod[0][0] - g[1])           # gate kernel end -> first IF-body node start
            r['body' + tag] = us(max(n[1] for n in bod) - bod[0][0])
            r['bodybusy' + tag] = us(sum(n[1] - n[0] for n in bod))
        else:
            r['skip' + tag] = 1
    kd = collections.Counter()
    for s, e, n, stv, gcx, gx, o in p['nodes']:
        b = n.split('<')[0]
        kd['k_' + ('ldpc' if 'ldpc' in b else b)] += e - s
    for k, v in kd.items(): r[k] = us(v)
    return r
R = [decomp(p) for p in full]
keys = ['span', 'busy', 'gap', 'l2s', 'subC', 'subD', 'subL', 'gCD', 'gDL', 'ifD', 'bodyD', 'ifL', 'bodyL', 'ngap', 'ncb']
print('\n== C+D+L passes: p10/p50/p90/p99 (us)')
for k in keys:
    v = [r[k] for r in R if k in r]; print(f"  {k:6s} n={len(v):6d} {q(v,.1):8.1f} {q(v,.5):8.1f} {q(v,.9):8.1f} {q(v,.99):8.1f}")
spans = sorted(r['span'] for r in R)
p50 = q(spans, .5); p99 = q(spans, .99)
for name, lo_, hi_ in (('TYPICAL (span p45-p55)', q(spans, .45), q(spans, .55)), ('TAIL (span >= p99)', p99, 1e18)):
    G = [r for r in R if lo_ <= r['span'] <= hi_]
    print(f"\n== {name}: n={len(G)}  mean decomposition (us)")
    allk = sorted({k for r in G for k in r})
    for k in allk:
        v = [r.get(k, 0) for r in G]
        print(f"  {k:22s} mean {st.mean(v):8.1f}  p50 {q(v,.5):8.1f}")
# per-kernel stretch: duration when no other non-own kernel overlapped vs when it did
print('\n== own-kernel duration: solo (no other GPU work overlapping) vs shared, by kernel family + grid (us p50, n)')
fam = collections.defaultdict(lambda: ([], []))
for p in full:
    for s, e, n, stv, gcx, gx, o in p['nodes']:
        if n in ('memcpy', 'memset'): continue
        b = n.split('<')[0]
        other = max([v for k, v in o.items() if k != 'ANY'] or [0])
        fam[(b, gx)][1 if other > 0.2 * (e - s) else 0].append(us(e - s))
for (b, gx), (so, sh) in sorted(fam.items(), key=lambda t: -sum(t[1][0]) - sum(t[1][1]))[:25]:
    print(f"  {b:22s} grid {gx:6d}  solo {q(so,.5):8.1f} (n={len(so):6d})  shared {q(sh,.5):8.1f} (n={len(sh):6d})")
def timeline(p):
    print(f"\n-- pass thr={p['thr']} span={us(p['span']):.1f} busy={us(p['busy']):.1f} launch->first={us(p['l2s']):.1f} seq={p['seq']} " + (f"ev: crc={p['ev'].get('crc')} probe={p['ev'].get('probe')} tbs={p['ev'].get('tbs')} rb={p['ev'].get('rb')} mcs={p['ev'].get('mcs')} gp={p['ev'].get('gp')} gq={p['ev'].get('gq')} li={p['ev'].get('li')} e2e={p['ev'].get('e2e')}" if p.get('ev') else ''))
    t0 = p['t0']; prev = None
    for s, e, n, stv, gcx, gx, o in p['nodes']:
        gap = '' if prev is None else f"gap {us(s - prev):7.1f}"
        oth = ' '.join(f"{k}={us(v):.0f}" for k, v in o.items() if v > 500)
        print(f"   {us(s - t0):8.1f} +{us(e - s):7.1f}  {n[:28]:28s} grid={gx:<6} strm={stv:<5} {gap:14s} other: {oth}")
        prev = e if prev is None else max(prev, e)
    for g0, g1, o in p['gaps']:
        if g1 - g0 > 20000:
            print(f"   GAP {us(g0 - t0):8.1f}..{us(g1 - t0):8.1f} ({us(g1 - g0):.1f}) other: " + ' '.join(f"{k}={us(v):.0f}" for k, v in o.items() if v > 0))
typ = [p for p in full if q(spans, .48) <= us(p['span']) <= q(spans, .52)]
tail = [p for p in full if us(p['span']) >= p99]
for p in typ[:: max(1, len(typ) // a.n)][:a.n]: timeline(p)
for p in tail[:: max(1, len(tail) // a.n)][:a.n]: timeline(p)
# stretch of own kernels by WHO overlapped them (categories overlapping > 20 % of the kernel)
print('\n== own-kernel stretch = duration / solo p50 of the same kernel+grid, by the set of overlapping categories (p50 ratio, share of kernel time)')
solo = {k: (sorted(v[0])[len(v[0]) // 2] if v[0] else None) for k, v in fam.items()}
grp = collections.defaultdict(lambda: [[], 0.0])
tot = 0.0
for p in full:
    for s, e, n, stv, gcx, gx, o in p['nodes']:
        if n in ('memcpy', 'memset'): continue
        b = n.split('<')[0]; sm = solo.get((b, gx))
        if not sm: continue
        d = us(e - s)
        key = '+'.join(sorted(k for k, v in o.items() if k != 'ANY' and v > 0.2 * (e - s))) or 'solo'
        grp[key][0].append(d / sm); grp[key][1] += d; tot += d
for k, (v, t) in sorted(grp.items(), key=lambda x: -x[1][1])[:14]:
    print(f"  {k:45s} ratio p50 {q(v,.5):5.2f} p90 {q(v,.9):5.2f}  time share {100*t/tot:5.1f} %  n={len(v)}")
# ---------------- queueing (needs --log): who waits behind whom ----------------
if a.log:
    allp = pickle.load(open(a.pkl, 'rb'))['passes']
    for p in allp: p['ev'] = EV.get(p['seq'])
    byth = collections.defaultdict(list)
    for p in allp: byth[p['thr']].append(p)
    for v in byth.values(): v.sort(key=lambda p: p['t_launch'])
    def info(p):
        e = p.get('ev') or {}
        return f"seq={p['seq']} rb={e.get('rb')} tbs={e.get('tbs')} crc={e.get('crc')} probe={e.get('probe')} span={us(p['span']):.0f}"
    J = []
    for p in allp:
        e = p.get('ev')
        if not e or e.get('probe', '0') != '0' or 'e2e' not in e: continue
        ee = [int(x) for x in e['e2e'].split(',')]; gp = [int(x) for x in e['gp'].split(',')]
        if min(ee[1], ee[2]) < 0: continue
        t_entry = p['t_launch'] - 1000 * (gp[3] + gp[1])          # dl_run entry (the decode body's host work before it is not stamped)
        t_taken = t_entry - 1000 * ee[2]; t_enq = t_taken - 1000 * ee[1]
        mates = [q for q in byth[p['thr']] if t_taken <= q['t_launch'] < p['t_launch']]  # this thread's earlier passes after the dequeue (slot group)
        others = []
        for th, L in byth.items():
            if th == p['thr']: continue
            busy = sum(max(0, min(q['t1'], t_taken) - max(q['t_launch'], t_enq)) for q in L if q['t_launch'] < t_taken and q['t1'] > t_enq)
            others.append(busy / max(1, t_taken - t_enq))
        J.append(dict(p=p, tot=ee[6], qwait=ee[1], pre=ee[2], dec=ee[3], mates=mates, thr_busy=sum(others), nthr=len(others)))
    if J:
        tots = sorted(j['tot'] for j in J); p99 = q(tots, .99)
        for name, G in (('ALL real', J), ('slot->result >= p99', [j for j in J if j['tot'] >= p99])):
            print(f"\n== queue ({name}, n={len(G)}): qwait p50 {q([j['qwait'] for j in G],.5)} us, pre p50 {q([j['pre'] for j in G],.5)} us; "
                  f"during qwait, other decode threads inside a GPU pass (mean count of {G[0]['nthr']}): {st.mean(j['thr_busy'] for j in G):.2f}; "
                  f"grants decoded after same-thread passes (slot group): {sum(1 for j in G if j['mates'])} (mates' span sum p50 {q([sum(us(m['span']) for m in j['mates']) for j in G if j['mates']],.5)} us)")
        for j in sorted(J, key=lambda j: -j['tot'])[:a.n * 3]:
            print(f"  tot {j['tot']} qwait {j['qwait']} pre {j['pre']} dec {j['dec']}: {info(j['p'])} thr={j['p']['thr']} | ahead on own thread: " + '; '.join(info(m) for m in j['mates'][:4]) + f" | others in pass {j['thr_busy']:.2f}")
