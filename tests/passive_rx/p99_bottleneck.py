#!/usr/bin/env python3
"""P99 bottleneck analysis of ISAC_E2E_TRACE=1 OTA run.logs (one run at a time, never pooled).
usage: p99_bottleneck.py run.log [...]   (text report on stdout; --json for machine output)
Sections: decomposition of slow real DL grants, mechanism tests (probe load, big-grant occupancy, consumer overlap),
DECSLOW/PDTIMSLOW (wall vs cpu, ctx switches), UL slow decodes. Slot axis: unwrapped frame*20+slot (30 kHz) * 0.5 ms."""
import re, sys, json, collections, bisect
R = re.compile(r'EVMGRANT rnti=0x(\w+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) .*? Nl=(\d) rb=(\d+)\+(\d+) .*? probe=(\d) .*? dec_us=(\d+) rv=(\d+) ndi=(\d) pid=(\d+) tbs=(\d+).*? e2e=(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(-?\d+),(-?\d+)')
U = re.compile(r'UL_E2E rnti=0x(\w+) st=(\d+) prb=(\d+) mcs=(\d+) pick=(\d) arr2deq=(-?\d+) wall=(\d+) tot=(-?\d+) cpu_us=(-?\d+)')
DS = re.compile(r'DECSLOW t=([\d.]+) rnti=0x(\w+) mcs=(\d+) Nl=(\d) rb=(\d+)\+(\d+) probe=(\d) st=(\d+) wall_us=(\d+) bodies=(\d+) body_us=(\d+) stage_us=(\d+) cpu_us=(\d+) vcsw=(\d+) ivcsw=(\d+) minflt=(\d+) majflt=(\d+) core=(\d+)->(\d+)')
PS = re.compile(r'PDTIMSLOW t=([\d.]+) class=(\w+) us=(\d+) fep=(\d+)/(\d+) chest=(\d+)/(\d+) demod=(\d+)/(\d+) ldpc=(\d+)/(\d+)')
def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))] if v else None
class G: pass
def load(path):
    dl, ul, ds, ps = [], [], [], []
    last = None; wrap = 0; idx = 0
    for l in open(path, errors='replace'):
        if 'EVMGRANT' in l:
            m = R.search(l)
            if not m: continue
            g = G(); (r, f, s, g.crc, g.mcs, g.nl, g.rb0, g.nrb, g.probe, g.dec_us, g.rv, g.ndi, g.pid, g.tbs, g.a2e, g.qw, g.pre, g.dec, g.first, g.bod, g.tot, g.lag) = m.groups()
            for k in ('crc','mcs','nl','rb0','nrb','probe','dec_us','rv','ndi','pid','tbs','a2e','qw','pre','dec','first','bod','tot','lag'): setattr(g, k, int(getattr(g, k)))
            g.rnti = r; a = int(f) * 20 + int(s)
            a = a if last is None else last + ((a - last + 10240) % 20480) - 10240   # nearest unwrapped (frame 0..1023 * 20 slots)
            last = a if last is None else max(last, a) if a - last < 2000 else a
            wrap = 0
            g.slot = a + wrap; g.t = g.slot * 0.0005; g.idx = idx; idx += 1
            g.extra = max(0, g.dec - g.first)
            dl.append(g); continue
        if 'UL_E2E' in l:
            m = U.search(l)
            if m:
                g = G(); (g.rnti, g.st, g.prb, g.mcs, g.pick, g.a2d, g.wall, g.tot, g.cpu) = m.groups()
                for k in ('st','prb','mcs','pick','a2d','wall','tot','cpu'): setattr(g, k, int(getattr(g, k)))
                g.idx = idx; idx += 1; g.t = None; ul.append(g)
            continue
        if 'UL_PUSCH_CRC' in l and ul and ul[-1].t is None:
            m = re.search(r'UL_PUSCH_CRC rnti=0x(\w+) .*? t=(\d+) ', l)
            if m and m.group(1) == ul[-1].rnti: ul[-1].t = int(m.group(2)) * 0.5   # ms
            continue
        if 'DECSLOW' in l:
            m = DS.search(l)
            if m:
                g = G(); v = m.groups(); g.t = float(v[0]); g.rnti = v[1]
                (g.mcs, g.nl, g.rb0, g.nrb, g.probe, g.st, g.wall, g.bodies, g.body, g.stage, g.cpu, g.vcsw, g.ivcsw, g.minflt, g.majflt, g.c0, g.c1) = map(int, v[2:])
                g.idx = idx; idx += 1; ds.append(g)
            continue
        if 'PDTIMSLOW' in l:
            m = PS.search(l)
            if m:
                g = G(); v = m.groups(); g.t = float(v[0]); g.cls = v[1]; g.us = int(v[2]); g.st = list(map(int, v[3:])); ps.append(g)
    return dl, ul, ds, ps

def groups_of(dl):
    """Slot groups: members of one slot dequeued together share taken = arr + a2e + qw. Returns [(enq_us, taken_us, members sorted by log order)]."""
    by_slot = collections.defaultdict(list)
    for g in dl:
        if g.a2e >= 0 and g.tot >= 0: by_slot[g.slot].append(g)
    out = []
    for sl, l in by_slot.items():
        l.sort(key=lambda g: g.a2e + g.qw); cur = None
        for g in l:
            k = g.a2e + g.qw
            if cur and k - cur[0] <= 2: cur[1].append(g)
            else: cur = [k, [g]]; out.append(cur)
            cur[0] = k
    res = []
    for k, m in out:
        m.sort(key=lambda g: g.idx)
        res.append((min(g.slot * 500 + g.a2e for g in m), m[0].slot * 500 + m[0].a2e + m[0].qw, m))
    return res
def simulate(groups, nserv, keep, service):
    """FIFO queue, nserv identical servers. keep(g) selects members that stay; service(members_kept, head_pre) -> us. Returns {id(g): qwait_us}."""
    import heapq
    jobs = []
    for enq, taken, m in groups:
        mk = [g for g in m if keep(g)]
        if mk: jobs.append((enq, service(mk, m[0].pre if m[0].pre >= 0 else 0), mk))
    jobs.sort(key=lambda j: j[0]); free = [0.0] * nserv; heapq.heapify(free); qw = {}
    for enq, svc, mk in jobs:
        t = heapq.heappop(free); start = max(t, enq)
        for g in mk: qw[id(g)] = start - enq
        heapq.heappush(free, start + svc)
    return qw
def whatif(dl, real, nserv=5):
    gr = groups_of(dl)
    svc = lambda mk, pre0: pre0 + sum(g.dec for g in mk)
    base = simulate(gr, nserv, lambda g: True, svc)
    o = ['', 'What-if replay (FIFO, %d identical consumers, measured group service = head pre + member decode walls; stale drops NOT modelled, so it is optimistic for saturated seconds):' % nserv]
    def line(lab, qw, keep_real):
        v = [g for g in real if id(g) in qw and keep_real(g)]
        tot = [g.a2e + qw[id(g)] + g.pre + g.dec for g in v]
        return '  %-34s real n=%6d  qwait p50=%6d p99=%6d | tot p50=%6d p90=%6d p99=%6d p99.9=%6d' % (lab, len(v), pct([qw[id(g)] for g in v], 50), pct([qw[id(g)] for g in v], 99), pct(tot, 50), pct(tot, 90), pct(tot, 99), pct(tot, 99.9))
    meas = [g for g in real if g.tot >= 0]
    o.append('  %-34s real n=%6d  qwait p50=%6d p99=%6d | tot p50=%6d p90=%6d p99=%6d p99.9=%6d' % ('MEASURED', len(meas), pct([g.qw for g in meas], 50), pct([g.qw for g in meas], 99), pct([g.tot for g in meas], 50), pct([g.tot for g in meas], 90), pct([g.tot for g in meas], 99), pct([g.tot for g in meas], 99.9)))
    o.append(line('replay, all jobs as measured', base, lambda g: True))
    o.append(line('replay, probes removed', simulate(gr, nserv, lambda g: g.probe == 0, svc), lambda g: True))
    o.append(line('replay, probes + tbs>20k removed*', simulate(gr, nserv, lambda g: g.probe == 0 and g.tbs <= 20000, svc), lambda g: g.tbs <= 20000))
    o.append(line('replay, tbs>20k real removed', simulate(gr, nserv, lambda g: g.probe or g.tbs <= 20000, svc), lambda g: g.tbs <= 20000))
    o.append(line('replay, 10 consumers', simulate(gr, 10, lambda g: True, svc), lambda g: True))
    o.append('  (* reported over the remaining light real grants only; their MEASURED tot p99 = %s, tbs<=20k)' % pct([g.tot for g in meas if g.tbs <= 20000], 99))
    return o
def row(name, v):
    v = [x for x in v if x >= 0]
    return '%-14s n=%6d p50=%7s p90=%7s p99=%7s p99.9=%7s max=%8s' % (name, len(v), pct(v, 50), pct(v, 90), pct(v, 99), pct(v, 99.9), max(v) if v else None)
def dominant(g):
    c = dict(arr2enq=g.a2e, qwait=g.qw, pre=g.pre, first_body=g.first, extra_bodies=g.extra)
    return max(c, key=lambda k: c[k])
def attribution(real, thr, key, out):
    sel = [g for g in real if getattr(g, key) > thr and g.tot >= 0 and g.a2e >= 0]
    if not sel: out.append('  %s > %d us: none' % (key, thr)); return sel
    tot = sum(g.tot for g in sel) or 1
    sh = {k: 100.0 * sum(max(0, getattr(g, a)) for g in sel) / tot for k, a in (('arr2enq', 'a2e'), ('qwait', 'qw'), ('pre', 'pre'), ('first_body', 'first'), ('extra_bodies', 'extra'))}
    dom = collections.Counter(dominant(g) for g in sel)
    out.append('  %s > %d us: n=%d (%.1f%% of real); time share %s; dominant component count %s' % (key, thr, len(sel), 100.0 * len(sel) / len(real),
               ' '.join('%s=%.0f%%' % kv for kv in sh.items()), dict(dom)))
    return sel
def report(path):
    dl, ul, ds, ps = load(path)
    lines_cache = [l for l in open(path, errors='replace') if 'GPU_DISCOVERY_SLICE' in l or 'kernel_overrun' in l]
    real = [g for g in dl if g.probe == 0]; pr = [g for g in dl if g.probe]
    o = ['## ' + path, '']
    if not real: return '\n'.join(o + ['no real grants'])
    ok = [g for g in real if g.crc]; bad = [g for g in real if not g.crc]
    o.append('DL real n=%d crc_ok=%.1f%%; probe(EVMGRANT probe=1) n=%d; span %.0f s (slot axis)' % (len(real), 100.0 * len(ok) / len(real), len(pr), real[-1].t - real[0].t))
    for k in ('a2e', 'qw', 'pre', 'first', 'extra', 'dec', 'tot'): o.append('  ' + row(k, [getattr(g, k) for g in real]))
    o.append('  ' + row('tot crc_ok', [g.tot for g in ok])); o.append('  ' + row('tot crc_fail', [g.tot for g in bad]))
    o.append('  ' + row('first crc_ok', [g.first for g in ok])); o.append('  ' + row('first crc_fail', [g.first for g in bad]))
    o.append('  bodies>1: %.1f%% of real; extra-body time share of dec = %.1f%%' % (100.0 * sum(g.bod > 1 for g in real) / len(real), 100.0 * sum(g.extra for g in real) / max(1, sum(g.dec for g in real))))
    p99 = pct([g.tot for g in real], 99)
    M = report.last = collections.OrderedDict()
    kn = [g for g in real if g.tot >= 0]
    M.update(n=len(real), crc=round(100.0 * len(ok) / len(real), 1), probes=len(pr), tot50=pct([g.tot for g in kn], 50), tot99=p99, tot999=pct([g.tot for g in kn], 99.9), a2e99=pct([g.a2e for g in kn], 99), qw99=pct([g.qw for g in real], 99), pre99=pct([g.pre for g in real], 99), first99=pct([g.first for g in real], 99), dec99=pct([g.dec for g in real], 99), multi=round(100.0 * sum(g.bod > 1 for g in real) / len(real), 1), heavy_pct=round(100.0 * sum(g.tbs > 20000 for g in real) / len(real), 1))
    o += ['', 'Slow-grant attribution (mean time share of tot; "dominant" = largest single component):']
    attribution(real, p99, 'tot', o); attribution(real, 5000, 'tot', o); attribution(real, 5000, 'dec', o)
    p99d = pct([g.dec for g in real], 99); attribution(real, p99d, 'dec', o)
    # what separates slow first bodies: size/layers/mcs/class
    o += ['', 'First-body time by Nl and PRB class (real):']
    for nl in sorted(set(g.nl for g in real)):
        for lab, f in (('nrb<=50', lambda g: g.nrb <= 50), ('50<nrb<=150', lambda g: 50 < g.nrb <= 150), ('nrb>150', lambda g: g.nrb > 150)):
            v = [g.first for g in real if g.nl == nl and f(g)]
            if len(v) >= 20: o.append('  Nl=%d %-12s n=%5d p50=%6d p90=%6d p99=%6d  share>5ms=%.1f%%  share of all tot>5ms grants=%.1f%%' % (nl, lab, len(v), pct(v, 50), pct(v, 90), pct(v, 99), 100.0 * sum(x > 5000 for x in v) / len(v), 100.0 * sum(1 for g in real if g.nl == nl and f(g) and g.tot > 5000) / max(1, sum(g.tot > 5000 for g in real))))
    # mechanism A: qwait vs concurrent work. Decode interval ~ [done-dec, done], done = slot_t + tot (slot axis ~ wall)
    iv = [((g.t * 1e6 + g.tot - g.dec), (g.t * 1e6 + g.tot), g) for g in dl if g.tot >= 0]
    iv.sort(key=lambda x: x[0])
    # per-second load
    sec = collections.defaultdict(lambda: dict(real=[], probe=0, fail=0, busy=0))
    for g in dl:
        s = int(g.t)
        if g.probe: sec[s]['probe'] += 1
        else:
            sec[s]['real'].append(g.tot); sec[s]['fail'] += (g.crc == 0)
        sec[s]['busy'] += g.dec
    rows = [(s, d) for s, d in sec.items() if len(d['real']) >= 20]
    o += ['', 'Per-second load vs real tot (seconds with >=20 real grants, n_sec=%d):' % len(rows)]
    if rows:
        def corr(x, y):
            n = len(x); mx = sum(x) / n; my = sum(y) / n
            sx = sum((a - mx) ** 2 for a in x) ** .5; sy = sum((b - my) ** 2 for b in y) ** .5
            return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx and sy else float('nan')
        p99s = [pct(d['real'], 99) for s, d in rows]
        for lab, xs in (('probes/s', [d['probe'] for s, d in rows]), ('real/s', [len(d['real']) for s, d in rows]), ('crcfail/s', [d['fail'] for s, d in rows]), ('decode-busy ms/s', [d['busy'] / 1000.0 for s, d in rows])):
            o.append('  corr(%s, per-second p99 tot) = %.2f' % (lab, corr(xs, p99s)))
        srt = sorted(rows, key=lambda r: r[1]['probe'])
        q = len(srt) // 4 or 1
        for lab, part in (('lowest-probe quartile', srt[:q]), ('highest-probe quartile', srt[-q:])):
            allv = [x for s, d in part for x in d['real']]
            o.append('  %s: probes/s mean=%.0f; real tot p50=%s p99=%s (n=%d)' % (lab, sum(d['probe'] for s, d in part) / len(part), pct(allv, 50), pct(allv, 99), len(allv)))
    # mechanism B: slow grants coincide with big/slow grants executing on the same time axis
    slow = [g for g in real if g.tot > 5000]
    o += ['', 'Who occupies the consumers while a slow real grant waits (qwait>2 ms, n=%d of %d slow):' % (sum(g.qw > 2000 for g in slow), len(slow))]
    import bisect
    starts = [x[0] for x in iv]
    cnt = collections.Counter(); tot_blk = 0
    for g in slow:
        if g.qw <= 2000: continue
        enq = g.t * 1e6 + g.a2e; deq = enq + g.qw
        # decodes running at any point of [enq,deq]
        run = [h for (s0, e0, h) in iv if s0 < deq and e0 > enq and h is not g]
        big = [h for h in run if h.dec > 5000]
        cnt['slow_with_gt5ms_decode_running'] += bool(big); cnt['slow_with_probe_gt5ms_running'] += any(h.probe for h in big); cnt['slow_with_real_gt5ms_running'] += any(not h.probe for h in big)
        cnt['n'] += 1
        cnt['avg_concurrent'] += len(run)
    if cnt['n']: o.append('  ' + ', '.join('%s=%s' % (k, v if k != 'avg_concurrent' else round(v / cnt['n'], 1)) for k, v in cnt.items()))
    # DECSLOW mechanism
    if ds:
        o += ['', 'DECSLOW (calls over the log threshold, n=%d; wall vs cpu and context switches):' % len(ds)]
        cpu_bound = [d for d in ds if d.cpu >= 0.7 * d.wall]; blocked = [d for d in ds if d.cpu < 0.3 * d.wall]
        o.append('  cpu>=70%% of wall (compute-bound): %d calls, wall sum %.0f ms; cpu<30%% of wall (blocked/preempted): %d calls, wall sum %.0f ms; in-between %d' % (len(cpu_bound), sum(d.wall for d in cpu_bound) / 1e3, len(blocked), sum(d.wall for d in blocked) / 1e3, len(ds) - len(cpu_bound) - len(blocked)))
        o.append('  classes: real=%d probe=%d; Nl>=3: %d; nrb>=150: %d; ivcsw>0: %d; vcsw>1000: %d (wall sum %.0f ms); majflt>0: %d; stage_us<0.2*body (time outside stages): %d (wall sum %.0f ms)' % (
            sum(d.probe == 0 for d in ds), sum(d.probe for d in ds), sum(d.nl >= 3 for d in ds), sum(d.nrb >= 150 for d in ds), sum(d.ivcsw > 0 for d in ds), sum(d.vcsw > 1000 for d in ds), sum(d.wall for d in ds if d.vcsw > 1000) / 1e3, sum(d.majflt > 0 for d in ds), sum(d.stage < 0.2 * d.body for d in ds), sum(d.wall for d in ds if d.stage < 0.2 * d.body) / 1e3))
        o.append('  wall us: p50=%s p90=%s max=%s; per-core counts %s' % (pct([d.wall for d in ds], 50), pct([d.wall for d in ds], 90), max(d.wall for d in ds), dict(collections.Counter(d.c1 for d in ds))))
    if ps:
        o += ['', 'PDTIMSLOW (bodies >20 ms, first %d logged; stage wall/cpu):' % len(ps)]
        for st, nm in enumerate(('fep', 'chest', 'demod', 'ldpc')):
            w = sum(p.st[2 * st] for p in ps); c = sum(p.st[2 * st + 1] for p in ps)
            o.append('  %s: wall %.0f ms, cpu %.0f ms (cpu/wall %.2f); dominant stage in %d of %d bodies' % (nm, w / 1e3, c / 1e3, c / max(1, w), sum(1 for p in ps if max(range(4), key=lambda k: p.st[2 * k]) == st), len(ps)))
    unk = [g for g in real if g.tot < 0]
    o += ['', 'Real grants whose slot samples left the 128-slot ring (tot=-1, i.e. > 64 ms old): n=%d (%.2f%%); their dec p50=%s p90=%s' % (len(unk), 100.0 * len(unk) / len(real), pct([g.dec for g in unk], 50), pct([g.dec for g in unk], 90))]
    # class split of the tail
    def cls(g):
        big = g.nrb > 150; hi = g.nl >= 3
        return ('big' if big else 'mid' if g.nrb > 50 else 'small') + ('+Nl>=3' if hi else '+Nl<=2')
    for lab, sel in (('tot>p99', [g for g in real if g.tot > p99]), ('tot>5ms', [g for g in real if g.tot > 5000]), ('first>5ms', [g for g in real if g.first > 5000]), ('all real', real)):
        c = collections.Counter(cls(g) for g in sel)
        o.append('  class mix of %-9s n=%5d: %s' % (lab, len(sel), ' '.join('%s=%.0f%%' % (k, 100.0 * v / len(sel)) for k, v in sorted(c.items(), key=lambda kv: -kv[1]))))
    # tail with the big/high-rank class removed
    rest = [g.tot for g in real if g.tot >= 0 and not (g.nrb > 150 and g.nl >= 3)]
    o.append('  real tot without (nrb>150 & Nl>=3): n=%d p50=%s p90=%s p99=%s p99.9=%s' % (len(rest), pct(rest, 50), pct(rest, 90), pct(rest, 99), pct(rest, 99.9)))
    rest2 = [g.tot for g in real if g.tot >= 0 and g.nrb <= 150 and g.nl <= 2]
    o.append('  real tot of (nrb<=150 & Nl<=2): n=%d p50=%s p99=%s' % (len(rest2), pct(rest2, 50), pct(rest2, 99)))
    # head-of-line: which decode finished right when this grant was dequeued
    o += ['', 'Head-of-line (qwait>2 ms): decode that finished within 400 us before this grant was dequeued, and pool occupancy at enqueue:']
    ends = sorted(((e0, h) for (s0, e0, h) in iv), key=lambda x: x[0]); ek = [e for e, h in ends]
    hol = collections.Counter(); occ = collections.Counter(); nq = 0
    for g in real:
        if g.qw <= 2000 or g.tot < 0: continue
        nq += 1
        deq = g.t * 1e6 + g.a2e + g.qw; enq = g.t * 1e6 + g.a2e
        i = bisect.bisect_left(ek, deq - 400); blk = None
        while i < len(ends) and ends[i][0] <= deq + 100:
            if ends[i][1] is not g: blk = ends[i][1]; break
            i += 1
        hol['none' if blk is None else ('probe' if blk.probe else 'real') + ('>5ms' if blk.dec > 5000 else '<=5ms')] += 1
        running = sum(1 for (s0, e0, h) in iv if s0 <= enq <= e0 and h is not g)
        occ['>=5' if running >= 5 else str(running)] += 1
    if nq: o.append('  n=%d blockers %s; decodes running at enqueue %s' % (nq, dict(hol), dict(sorted(occ.items()))))
    # pre >1 ms
    o.append('  pre>1ms: n=%d (%.1f%% of real), mean %.0f us; of them concurrent probe count (probes finishing within +-1 ms) mean %.1f vs overall %.1f' % (
        sum(g.pre > 1000 for g in real), 100.0 * sum(g.pre > 1000 for g in real) / len(real), sum(g.pre for g in real if g.pre > 1000) / max(1, sum(g.pre > 1000 for g in real)), 0.0, 0.0))
    # join DECSLOW and PDTIMSLOW (same ms timestamp) -> Nl, rb of >20 ms bodies and their stage split
    if ps and ds:
        dsl = sorted(ds, key=lambda d: d.t); tt = [d.t for d in dsl]
        jc = collections.Counter(); wsum = collections.defaultdict(float)
        for p in ps:
            i = bisect.bisect_left(tt, p.t - 0.003)
            d = dsl[i] if i < len(dsl) and abs(dsl[i].t - p.t) < 0.01 else None
            if d is None: jc['unjoined'] += 1; continue
            key = '%s Nl%s nrb%s' % ('probe' if d.probe else 'real', '>=3' if d.nl >= 3 else '<=2', '>150' if d.nrb > 150 else '<=150')
            jc[key] += 1; wsum[key] += p.us / 1e3
        o += ['', 'Bodies >20 ms (PDTIMSLOW joined to DECSLOW): ' + ', '.join('%s: n=%d (%.0f ms)' % (k, v, wsum[k]) for k, v in jc.most_common())]
    return_tail = None
    # UL detail
    # run-level aggregates printed by the UE
    last = {}
    for l in open(path, errors='replace'):
        for tag in ('PDTIMCLS us', 'PDTIMCLSST', 'PDTIMPCT', 'PDTIM calls'):
            if 'SENSING: ' + tag in l: last[tag] = l.split('SENSING: ', 1)[1].strip()
    o += ['', 'Final UE-side aggregates (cumulative, whole run):'] + ['  ' + v for v in last.values()]
    # tail buckets: each tail grant gets ONE cause = its largest component; first-body split by body duration
    def bucket(g):
        d = dominant(g)
        if d == 'first_body': return 'body>20ms' if g.first > 20000 else 'body5-20ms' if g.first > 5000 else 'body<5ms'
        return d
    o += ['', 'Tail grants by single largest component (tot known only):']
    for lab, sel in (('tot>p99', [g for g in real if g.tot > p99]), ('tot>5ms', [g for g in real if g.tot > 5000])):
        c = collections.Counter(bucket(g) for g in sel)
        o.append('  %-8s n=%5d: %s' % (lab, len(sel), ' '.join('%s=%.0f%%' % (k, 100.0 * v / len(sel)) for k, v in c.most_common())))
        c2 = collections.Counter('crc_fail' if not g.crc else 'crc_ok' for g in sel); o.append('           crc: %s; tbs>20k: %.0f%%; bodies>1: %.0f%%' % (dict(c2), 100.0 * sum(g.tbs > 20000 for g in sel) / len(sel), 100.0 * sum(g.bod > 1 for g in sel) / len(sel)))
    tb = collections.Counter(bucket(g) for g in real if g.tot > p99); tn = sum(tb.values()) or 1
    M.update(p99_body=round(100.0 * (tb['body5-20ms'] + tb['body>20ms'] + tb['body<5ms']) / tn), p99_qwait=round(100.0 * tb['qwait'] / tn), p99_extra=round(100.0 * tb['extra_bodies'] / tn), p99_pre=round(100.0 * tb['pre'] / tn), p99_a2e=round(100.0 * tb['arr2enq'] / tn), p99_heavy=round(100.0 * sum(g.tbs > 20000 for g in real if g.tot > p99) / tn), p99_crcfail=round(100.0 * sum(not g.crc for g in real if g.tot > p99) / tn))
    o.append('  first body >20 ms: %d (%.2f%%), >100 ms: %d; pre >100 ms: %d; qwait >6 ms: %d (%.1f%%)' % (sum(g.first > 20000 for g in real), 100.0 * sum(g.first > 20000 for g in real) / len(real), sum(g.first > 100000 for g in real), sum(g.pre > 100000 for g in real), sum(g.qw > 6000 for g in real), 100.0 * sum(g.qw > 6000 for g in real) / len(real)))
    # who consumes decode time
    dt = sum(g.dec for g in dl) or 1
    o.append('  decode wall by class (EVMGRANT lines): real crc_ok %.0f%%, real crc_fail %.0f%%, probes %.0f%%; real tbs>20k %.0f%% of ALL decode wall' % (100.0 * sum(g.dec for g in ok) / dt, 100.0 * sum(g.dec for g in bad) / dt, 100.0 * sum(g.dec for g in pr) / dt, 100.0 * sum(g.dec for g in real if g.tbs > 20000) / dt))
    # probe test restricted to the non-heavy real grants (tbs<=20k): does the probe load move their tail?
    lt = [g for g in real if g.tbs <= 20000 and g.tot >= 0]
    if lt and rows:
        ps_ = collections.defaultdict(list)
        for g in lt: ps_[int(g.t)].append(g.tot)
        pr_s = {s_: d['probe'] for s_, d in sec.items()}
        xs = sorted((pr_s.get(s_, 0), v) for s_, v in ps_.items() if len(v) >= 20)
        if len(xs) >= 8:
            q = len(xs) // 4
            for lab, part in (('lowest-probe quartile', xs[:q]), ('highest-probe quartile', xs[-q:])):
                allv = [x for _, v in part for x in v]
                o.append('  light grants (tbs<=20k) in %s seconds (probes/s %.0f): tot p50=%s p99=%s n=%d' % (lab, sum(p for p, _ in part) / len(part), pct(allv, 50), pct(allv, 99), len(allv)))
    # DL LDPC cost per code block from interval deltas of the cumulative GPU_EXACT_LDPC counters
    pts = []
    for l in open(path, errors='replace'):
        m = re.search(r'GPU_EXACT_LDPC tbs=(\d+) cbs=(\d+) .*?mean=(\d+) us \(gpu (\d+)\)', l)
        if m: pts.append(tuple(int(x) for x in m.groups()))
    tt = [(t, c, t * m, t * g) for t, c, m, g in pts]; xs, ys, zs = [], [], []
    for a, b in zip(tt, tt[1:]):
        if b[0] > a[0]: xs.append((b[1] - a[1]) / (b[0] - a[0])); ys.append((b[3] - a[3]) / (b[0] - a[0])); zs.append((b[2] - a[2]) / (b[0] - a[0]))
    if len(xs) >= 8 and max(xs) - min(xs) > 0.5:
        n = len(xs); mx = sum(xs) / n; sxx = sum((x - mx) ** 2 for x in xs)
        f = lambda v: (sum(v) / n - sum((x - mx) * (y - sum(v) / n) for x, y in zip(xs, v)) / sxx * mx, sum((x - mx) * (y - sum(v) / n) for x, y in zip(xs, v)) / sxx)
        o.append('  DL GPU LDPC cost (interval deltas, n=%d, cbs/TB %.1f-%.1f): gpu_us/TB = %.0f + %.0f x cbs/TB; call wall_us/TB = %.0f + %.0f x cbs/TB' % (n, min(xs), max(xs), f(ys)[0], f(ys)[1], f(zs)[0], f(zs)[1]))
    # global stalls: >=3 consumers released in the same ms, each held >=100 ms (DECSLOW log is capped at 300 lines/run)
    cl = collections.defaultdict(list)
    for d in ds: cl[round(d.t, 2)].append(d)
    big = [(t, v) for t, v in sorted(cl.items()) if len(v) >= 3 and min(d.wall for d in v) >= 100000]
    sl = []
    for i, l in enumerate(lines_cache):
        m2 = re.search(r'(GPU_DISCOVERY_SLICE|kernel_overrun).*?elapsed_ms=([\d.]+)', l)
        if m2: sl.append((i, float(m2.group(2))))
    hit = 0
    for t, v in big:
        w = max(d.wall for d in v) / 1000.0; i0 = min(d.idx for d in v)
        hit += any(0.7 * w <= e <= 1.15 * w for _, e in sl)   # coarse: any slice of matching duration in the run (see per-event check in the report text)
    M.update(stall_clusters=len(big), stall_s=round(sum(max(d.wall for d in v) for t, v in big) / 1e6, 1), disc_slices_gt100ms=sum(e > 100 for _, e in sl))
    o += ['', 'Global GPU/driver stalls (>=3 consumers released in the same ms, each held >=100 ms): %d clusters, %.1f s of stall (max member); durations ms: %s' % (len(big), M['stall_s'], [round(max(d.wall for d in v) / 1000) for t, v in big]),
          '  GPU_DISCOVERY_SLICE/kernel_overrun lines: %d, elapsed p50=%s ms max=%s ms, >100 ms: %d' % (len(sl), pct([e for _, e in sl], 50), max([e for _, e in sl] or [0]), M['disc_slices_gt100ms'])]
    # censoring: stale drops
    q = None
    for l in open(path, errors='replace'):
        if 'SENSING: PDSCHQ queued=' in l: q = l
        if 'scanq[queued=' in l: sq = l
    if q:
        m = re.search(r'queued=(\d+) decoded=(\d+) crc_ok=(\d+).*?dropped\[full=(\d+) stale=(\d+)\] max_lag_slots=(\d+)', q)
        if m: M['stale_pct'] = round(100.0 * int(m.group(5)) / max(1, int(m.group(1))), 1)
        if m: o.append('  PDSCHQ final: queued=%s decoded=%s dropped full=%s stale=%s (stale = %.1f%% of queued: never decoded, absent from every latency number above), max_lag_slots=%s' % (m.group(1), m.group(2), m.group(4), m.group(5), 100.0 * int(m.group(5)) / max(1, int(m.group(1))), m.group(6)))
    try:
        m = re.search(r'scanq\[queued=(\d+) done=(\d+) drop_full=(\d+) drop_stale=(\d+) maxlag=(\d+)', sq)
        if m: o.append('  PDCCH scanq final: queued=%s done=%s drop_full=%s drop_stale=%s maxlag=%s' % m.groups())
    except NameError: pass
    wi = whatif(dl, real)
    o += wi
    for ln in wi:
        mm = re.search(r"tot p50=\s*(\d+) p90=\s*(\d+) p99=\s*(\d+)", ln)
        if mm and 'replay, probes removed' in ln: M['sim_noprobe99'] = int(mm.group(3))
        if mm and 'tbs>20k real removed' in ln: M['sim_noheavy99'] = int(mm.group(3))
        if mm and '10 consumers' in ln: M['sim_10cons99'] = int(mm.group(3))
        if mm and 'all jobs as measured' in ln: M['sim_all99'] = int(mm.group(3))
    # UL
    ur = [u for u in ul if u.pick == 0]; up = [u for u in ul if u.pick == 1]
    o += ['', 'UL_E2E real(pick=0) n=%d, probe-ring(pick=1) n=%d' % (len(ur), len(up))]
    for k in ('a2d', 'wall', 'tot', 'cpu'): o.append('  ' + row('real ' + k, [getattr(u, k) for u in ur]))
    for k in ('a2d', 'wall', 'tot'): o.append('  ' + row('probe ' + k, [getattr(u, k) for u in up]))
    sl = [u for u in ur if u.tot > 5000]
    if sl: o.append('  real tot>5ms n=%d: a2d share %.0f%%, wall share %.0f%%; cpu/wall median %.2f' % (len(sl), 100.0 * sum(max(0, u.a2d) for u in sl) / sum(u.tot for u in sl), 100.0 * sum(u.wall for u in sl) / sum(u.tot for u in sl), pct([u.cpu / max(1, u.wall) for u in sl], 50)))
    # UL: who holds the PUSCH consumers (2) while a slow real job waits; slot axis from UL_PUSCH_CRC t= (abs slot)
    uj = [u for u in ul if u.t is not None and u.tot >= 0]
    if uj:
        ev = sorted(((u.t * 1000 + u.tot - u.wall, u.t * 1000 + u.tot, u) for u in uj), key=lambda x: x[0])
        sl = [u for u in uj if u.pick == 0 and u.a2d > 5000]
        c = collections.Counter(); busy_all = 0
        for u in sl:
            arr = u.t * 1000 + u.tot - u.tot; deq = arr + u.a2d
            run = [h for (s0, e0, h) in ev if s0 < deq and e0 > arr and h is not u]
            kind = 'real' if any(h.pick == 0 for h in run) else 'probe-only' if run else 'none'
            c[kind] += 1; c['sum_conc_probe_wall_ms'] += sum(h.wall for h in run if h.pick == 1) / 1e3 / max(1, len(sl))
            c['sum_conc_real_wall_ms'] += sum(h.wall for h in run if h.pick == 0) / 1e3 / max(1, len(sl))
        o.append('  UL real a2d>5ms n=%d: consumers busy meanwhile with %s (mean concurrent wall ms probe/real in [arr,deq] are the last two keys)' % (len(sl), {k: (round(v, 1) if 'sum' in k else v) for k, v in c.items()}))
        pw = [u.wall for u in uj if u.pick == 1]; rw = [u.wall for u in uj if u.pick == 0]
        span = (max(e[1] for e in ev) - min(e[0] for e in ev)) / 1e6
        o.append('  UL consumer load: probe wall sum %.1f s (%d jobs, p99 wall %s), real wall sum %.1f s (%d jobs); utilisation of 2 consumers %.2f over %.0f s' % (sum(pw) / 1e6, len(pw), pct(pw, 99), sum(rw) / 1e6, len(rw), (sum(pw) + sum(rw)) / 1e6 / span / 2, span))
        for lab, lo, hi in (('probe wall>5ms', 5000, 10**9),):
            sel = [u for u in uj if u.pick == 1 and lo < u.wall <= hi]
            o.append('  UL probe jobs with wall>5ms: n=%d, wall sum %.1f s = %.0f%% of probe wall; real jobs >5ms: n=%d' % (len(sel), sum(u.wall for u in sel) / 1e6, 100.0 * sum(u.wall for u in sel) / max(1, sum(pw)), sum(u.wall > 5000 for u in uj if u.pick == 0)))
    M.update(ul_real=len(ur), ul_ok=sum(u.st == 0 for u in ur), ul_wall99=pct([u.wall for u in ur], 99), ul_a2d99=pct([u.a2d for u in ur], 99), ul_tot99=pct([u.tot for u in ur], 99), ul_gt5=len([u for u in ur if u.tot > 5000]))
    for lab, f in (('prb<=20', lambda u: u.prb <= 20), ('20<prb<=60', lambda u: 20 < u.prb <= 60), ('prb>60', lambda u: u.prb > 60)):
        for st in (0, 2):
            v = [u for u in ur if f(u) and (u.st == 0) == (st == 0)]
            if len(v) >= 20: o.append('  real %-11s %s n=%5d wall p50=%6d p99=%6d cpu p99=%6d share of wall>5ms=%.1f%%' % (lab, 'ok  ' if st == 0 else 'fail', len(v), pct([u.wall for u in v], 50), pct([u.wall for u in v], 99), pct([u.cpu for u in v], 99), 100.0 * sum(u.wall > 5000 for u in v) / max(1, sum(u.wall > 5000 for u in ur))))
    o.append('METRICS ' + json.dumps(M))
    return '\n'.join(o)
if __name__ == '__main__':
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--table' in sys.argv:   # args are per-run report files (with a METRICS line) or run.logs
        rows = []
        for p in paths:
            if p.endswith('.txt'):
                m = [l for l in open(p) if l.startswith('METRICS ')][-1]; rows.append((p.split('/')[-1][:-4], json.loads(m[8:])))
            else:
                report(p); rows.append((p.split('/')[-2].split('_')[1] if '/' in p else p, dict(report.last)))
        keys = list(rows[0][1].keys())
        print('run   ' + ' '.join('%11s' % k for k in keys))
        for n, m in rows: print('%-5s ' % n + ' '.join('%11s' % m.get(k, '-') for k in keys))
    else:
        for p in paths:
            print(report(p)); print()
