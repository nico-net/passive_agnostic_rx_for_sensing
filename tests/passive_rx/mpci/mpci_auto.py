#!/usr/bin/env python3
"""mpci_auto.py : blind multi-cell SUPERVISOR -- cells.json (fastscan) -> one radio window -> N cell receivers (mpci_launch.sh) -> pass 2 -> watch.
  mpci_auto.py plan CELLS.json --cores 4-19 [--max-span-hz 110e6] [--rate 122.88e6] [--pub-cores 0-1] [--ring isac_iq] [--out plan.json]
  mpci_auto.py launch PLAN.json --out DIR [--pub-cmd CMD] [--base-cfg CFG] [--build DIR]      (DRY=1 env: print only)
  mpci_auto.py pass2 PLAN.json --out DIR [--wait 180]    per cell: sync+MIB+SIB1 in ue.log -> autofront refine + retune_args auto; restart ONLY a cell whose grid changes
  mpci_auto.py watch PLAN.json --out DIR [--period 10] [--lost-s 30] [--max-restarts 5] [--backoff-s 10] [--rescan CAPDIR...]
  mpci_auto.py auto (--cells CELLS.json | --capdirs DIR...) --out DIR --cores 4-19 [plan/launch/watch options]
Window rule: a cell occupies the interval C +- BW/2. BW = SIB1 carrier width (sib1_nrb, pass-2 data in cells.json) else the pass-1 grid: the smallest
receiver-supported 30 kHz grid whose half width reaches autofront.RB_GRID_SC (the --ssb 1171 first-pass rule puts the grid centre there) = 217 PRB.
No operator constants: everything is the scanner's output or a receiver rule. The window centre covers the most cells within --max-span-hz; the
rest are `unserved` (M4 input). plan_window() is a separate function so a later step can call it repeatedly for several windows.
Stop/restart = SIGINT only (X410 claim hygiene), never SIGKILL/pkill. Polling, no daemons.
shortcut: one window, report-only rescan, 30 kHz grids (supported-grid list copied from retune_args.py), pass-2 interval uses the carrier width not the grid."""
import sys, os, re, json, time, subprocess, argparse
H = os.path.dirname(os.path.abspath(__file__)); AF = os.path.join(H, '..', 'autofront'); SURVEY = os.path.join(H, '..', 'survey')
sys.path.insert(0, AF)
import autofront as A

SUPPORTED = (51, 106, 133, 162, 189, 217, 273)         # get_samplerate_and_bw(mu=1), same list as survey/retune_args.py
RB_HZ = 12 * A.SCS_HZ
R1 = next(r for r in SUPPORTED if r * RB_HZ / 2 >= A.RB_GRID_SC * A.SCS_HZ)   # pass-1 grid (see docstring)

def grid_for(nrb): return next((r for r in SUPPORTED if r >= nrb), SUPPORTED[-1])

def expand_cores(s):
    out = []
    for part in s.split(','):
        a, _, b = part.partition('-'); out += range(int(a), int(b or a) + 1)
    return out

def fmt_cores(l):
    r = []; i = 0
    while i < len(l):
        j = i
        while j + 1 < len(l) and l[j + 1] == l[j] + 1: j += 1
        r.append(str(l[i]) if i == j else f'{l[i]}-{l[j]}'); i = j + 1
    return ','.join(r)

def cell_entry(c):
    """receiver geometry + occupied interval of one cells.json entry (pass 2 if the scanner/receiver facts are there)."""
    p2 = c.get('sib1_nrb') and c.get('k_ssb') is not None
    t = A.tune(c, c['k_ssb'] if p2 else None); r = grid_for(c['sib1_nrb']) if p2 else R1
    bw = (c['sib1_nrb'] if p2 else r) * RB_HZ
    return dict(tag=f"p{c['pci']}_{c['gscn']}", pci=c['pci'], gscn=c['gscn'], C_hz=t['C_hz'], r=r, ssb=t['ssb'], initial_fo=t['initial_fo'],
                bw_hz=bw, lo=t['C_hz'] - bw / 2, hi=t['C_hz'] + bw / 2, pass_=t['pass_'], n_cells=c.get('n_cells', 1), env={})

def plan_window(entries, max_span_hz):
    """interval cover, one window: left edge = some cell's lo; serve every cell inside [L, L+span]; most cells wins (tie: tighter, then lower)."""
    best = None
    for e in entries:
        s = [x for x in entries if x['lo'] >= e['lo'] and x['hi'] <= e['lo'] + max_span_hz]
        key = (-len(s), max(x['hi'] for x in s) - e['lo'], e['lo'])
        if best is None or key < best[0]: best = (key, s)
    served = best[1] if best else []
    ids = {id(x) for x in served}
    centre = int(round((min(x['lo'] for x in served) + max(x['hi'] for x in served)) / 2)) if served else None
    return centre, served, [x for x in entries if id(x) not in ids]

def make_plan(cells, cores, max_span_hz=110e6, rate=122.88e6, pub_cores=None, ring='isac_iq'):
    entries = [cell_entry(c) for c in cells]
    centre, served, unserved = plan_window(entries, max_span_hz)
    if not served: sys.exit('no cells to serve')
    cl = expand_cores(cores)
    if len(cl) < len(served): sys.exit(f'{len(cl)} cores for {len(served)} cells')
    n = len(cl) // len(served)
    gs = [x['gscn'] for x in served]
    for i, x in enumerate(sorted(served, key=lambda x: x['lo'])):
        x['cores'] = fmt_cores(cl[i * n:(i + 1) * n]); x['idx'] = i
        if x['n_cells'] > 1 or gs.count(x['gscn']) > 1: x['env'] = dict(ISAC_TARGET_PCI=str(x['pci']), ISAC_SSS_MULTI='2.5')
    keep = ('tag', 'pci', 'gscn', 'C_hz', 'r', 'ssb', 'initial_fo', 'bw_hz', 'pass_', 'env', 'cores', 'idx')
    return dict(ring=ring, centre_hz=centre, rate_hz=int(rate), span_hz=int(max_span_hz), pub_cores=pub_cores, cores=cores,
                cells=sorted(({k: x[k] for k in keep} for x in served), key=lambda x: x['idx']),
                unserved=[dict(tag=x['tag'], pci=x['pci'], gscn=x['gscn'], C_hz=x['C_hz'], lo_hz=int(x['lo']), hi_hz=int(x['hi'])) for x in unserved])

def spec(c):
    s = f"{c['tag']}:{c['C_hz']}:{c['r']}:{c['ssb']}:{c['initial_fo'] if c.get('initial_fo') is not None else '-'}:{c['cores']}"
    return s + (':' + ','.join(f'{k}={v}' for k, v in sorted(c['env'].items())) if c['env'] else '')

def launcher_env(plan, out, extra=None):
    e = dict(os.environ, OUT=out, RING=plan['ring']); e.update(extra or {})
    for k, pk in (('BASE_CFG', 'base_cfg'), ('BUILD', 'build')):
        if plan.get(pk): e.setdefault(k, plan[pk])
    return e

def launch(plan, out, pub_cmd=None, cells=None, append=False, run=subprocess.run):
    cells = cells or plan['cells']; extra = {}
    if pub_cmd: extra['PUB_CMD'] = pub_cmd
    if plan.get('pub_cores') and pub_cmd: extra['PUB_CORES'] = plan['pub_cores']
    if append: extra.update(APPEND='1', BASE_PORT=str(int(os.environ.get('BASE_PORT', 5556)) + cells[0]['idx']))
    cmd = ['bash', os.path.join(H, 'mpci_launch.sh'), 'start'] + [spec(c) for c in cells]
    r = run(cmd, env=launcher_env(plan, out, extra), capture_output=True, text=True)
    if r.stdout: print(r.stdout, end='')
    if r.returncode: sys.exit(f'mpci_launch.sh failed: {r.stderr}')

def pid_of(out, tag):
    p = None
    try:
        for l in open(os.path.join(out, 'pids')):
            t = l.split()
            if len(t) == 2 and t[0] == tag: p = int(t[1])
    except OSError: pass
    return p

def alive(pid):
    try: return pid is not None and open(f'/proc/{pid}/stat').read().rsplit(')', 1)[1].split()[0] != 'Z'
    except OSError: return False

def sigint_wait(pid, wait_s=60):
    if alive(pid):
        try: os.kill(pid, 2)
        except OSError: pass
        t0 = time.time()
        while alive(pid) and time.time() - t0 < wait_s: time.sleep(0.2)
    return not alive(pid)

def restart_cell(plan, out, c, wait_s=60):
    """SIGINT the old process of ONE cell, keep its log, relaunch it with the (possibly updated) plan entry."""
    if not sigint_wait(pid_of(out, c['tag']), wait_s): print(f"{c['tag']}: old pid still alive after {wait_s} s SIGINT, not restarting"); return False
    log = os.path.join(out, c['tag'], 'ue.log')
    if os.path.exists(log): os.replace(log, f"{log}.{int(time.time())}")
    launch(plan, out, cells=[c], append=True); return True

def save(plan, path): json.dump(plan, open(path, 'w'), indent=1)

def decide_retune(log):
    """pass-2 restart decision from a run log: None (keep grid) or dict(C_hz, r, ssb, env) from survey/retune_args.py auto."""
    t = open(log, errors='replace').read()
    if not re.search(r'ISAC_ACQ_RETUNE|ACQ carrier MISMATCH', t): return None
    p = subprocess.run([sys.executable, '-I', os.path.join(SURVEY, 'retune_args.py'), log, 'auto'], capture_output=True, text=True)
    m = re.match(r'C=(\d+) R=(\d+) SSB=(-?\d+)\s+\(carrier (\d+) PRB', p.stdout)
    if p.returncode or not m: print(f'retune_args exit {p.returncode}: {p.stdout.strip()}'); return None
    d = dict(C_hz=int(m[1]), r=int(m[2]), ssb=int(m[3]), env={})
    if d['r'] > int(m[4]): d['env']['ISAC_CARRIER_FROM_SIB1'] = '1'      # GRIDCARRIER: grid wider than the carrier
    return d

def pass2_cell(plan, out, c, plan_path=None, cells_json=None):
    """returns 'waiting' | 'done' | 'restarted' | 'outside_window' | 'pci_mismatch'."""
    log = os.path.join(out, c['tag'], 'ue.log')
    t = re.sub(r'\x1b\[[0-9;]*m', '', open(log, errors='replace').read()) if os.path.exists(log) else ''
    info = A.refine_from_log(t)
    if not ('k_ssb' in info and 'pci' in info and 'sib1_nrb' in info): return 'waiting'
    if info['pci'] != c['pci']: return 'pci_mismatch'
    if cells_json and os.path.exists(cells_json):
        cells = json.load(open(cells_json)); json.dump(A.apply_refine(cells, c['pci'], info), open(cells_json, 'w'), indent=1)
    d = decide_retune(log); c['pass_'] = 2
    if d is None: c['pass2'] = 'keep'; res = 'done'
    elif abs(d['C_hz'] - plan['centre_hz']) + d['r'] * RB_HZ / 2 > plan['span_hz'] / 2: c['pass2'] = 'outside_window'; c['wanted'] = d; res = 'outside_window'
    else:
        c.update(C_hz=d['C_hz'], r=d['r'], ssb=d['ssb'], bw_hz=info['sib1_nrb'] * RB_HZ, pass2='retuned'); c['env'] = {**c['env'], **d['env']}
        res = 'restarted' if restart_cell(plan, out, c) else 'done'
    if plan_path: save(plan, plan_path)
    return res

def cmd_pass2(plan, out, plan_path, wait=180, poll=5):
    t0 = time.time(); todo = [c for c in plan['cells'] if 'pass2' not in c]
    while todo:
        for c in list(todo):
            r = pass2_cell(plan, out, c, plan_path, plan.get('cells_json'))
            if r != 'waiting': print(f"{c['tag']}: pass2 {r}"); todo.remove(c)
        if todo and time.time() - t0 > wait: print('pass2 timeout, still waiting: ' + ' '.join(c['tag'] for c in todo)); break
        if todo: time.sleep(poll)

def cell_status(out, c):
    d = os.path.join(out, c['tag']); st = dict(state='NOMETRICS', crc=None, losses=None, overruns=0, producer_lost=False)
    try:
        with open(os.path.join(d, 'metrics.jsonl'), 'rb') as f:
            f.seek(0, 2); f.seek(max(0, f.tell() - 65536)); m = json.loads(f.read().decode(errors='replace').strip().splitlines()[-1])
        st.update(state=m.get('acq_state', '?'), losses=m.get('acq_sync_losses'), crc=(m['pdschq_crc_ok'] / m['pdschq_decoded']) if m.get('pdschq_decoded') else None)
    except (OSError, ValueError, IndexError, KeyError): pass
    try:
        t = open(os.path.join(d, 'ue.log'), errors='replace').read()
        st['overruns'] = t.count('SHMIQ_OVERRUN'); st['producer_lost'] = 'SHMIQ_PRODUCER_LOST' in t
    except OSError: pass
    return st

def watch_step(plan, out, rt, now, lost_s=30, max_restarts=5, backoff_s=10, restart=restart_cell):
    """one polling period; rt[tag] = dict(restarts, lost_since, next_ok). Returns status lines."""
    lines = []
    for c in plan['cells']:
        r = rt.setdefault(c['tag'], dict(restarts=0, lost_since=None, next_ok=0)); st = cell_status(out, c)
        up = alive(pid_of(out, c['tag']))
        r['lost_since'] = (r['lost_since'] or now) if up and st['state'] == 'LOST' else None
        why = 'exited' if not up else ('lost' if r['lost_since'] and now - r['lost_since'] > lost_s else None)
        state = st['state'] if up else 'DEAD'
        if why and r['restarts'] >= max_restarts: state += ' GIVEUP'
        elif why and now >= r['next_ok']:
            if restart(plan, out, c): r['restarts'] += 1; r['lost_since'] = None; state += ' RESTARTED'
            r['next_ok'] = now + backoff_s * 2 ** r['restarts']
        elif why: state += ' BACKOFF'
        if st['producer_lost']: state += ' PRODUCER_LOST'
        crc = '-' if st['crc'] is None else f"{100 * st['crc']:.1f}%"
        lines.append(f"{c['tag']} pci={c['pci']} state={state} crc={crc} overruns={st['overruns']} restarts={r['restarts']}")
    return lines

def rescan(plan, out, capdirs):
    d = os.path.join(out, 'rescan'); sh = os.path.join(AF, 'live_chain.sh')
    p = subprocess.run(['bash', sh, 'discover', d] + capdirs, capture_output=True, text=True)
    if p.returncode: return [f'rescan failed: {p.stderr[-200:]}']
    new = {(c['pci'], c['gscn']): cell_entry(c) for c in json.load(open(os.path.join(d, 'cells.json')))}
    have = {(c['pci'], c['gscn']) for c in plan['cells'] + plan['unserved']}
    half = plan['span_hz'] / 2; msgs = []
    for k, e in new.items():
        if k not in have: msgs.append(f"NEW {e['tag']} " + ('fits the window: rerun plan+launch' if abs(e['C_hz'] - plan['centre_hz']) + e['bw_hz'] / 2 <= half else 'outside window'))
    msgs += [f'VANISHED p{p}_{g}' for p, g in sorted(have - set(new))]
    return msgs

def cmd_watch(plan, out, period=10, rescan_dirs=None, rescan_period=300, iters=None, **kw):
    rt = {}; n = 0; last = time.time()
    while iters is None or n < iters:
        for l in watch_step(plan, out, rt, time.time(), **kw): print(l, flush=True)
        if rescan_dirs and time.time() - last > rescan_period:
            last = time.time(); [print('rescan:', m, flush=True) for m in rescan(plan, out, rescan_dirs)]
        n += 1
        if iters is None or n < iters: time.sleep(period)

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); sp = ap.add_subparsers(dest='cmd', required=True)
    def common(p):
        p.add_argument('--cores'); p.add_argument('--max-span-hz', type=float, default=110e6); p.add_argument('--rate', type=float, default=122.88e6)
        p.add_argument('--pub-cores'); p.add_argument('--ring', default='isac_iq')
    def lw(p):
        p.add_argument('--out', required=True); p.add_argument('--pub-cmd'); p.add_argument('--base-cfg'); p.add_argument('--build')
        p.add_argument('--period', type=float, default=10); p.add_argument('--lost-s', type=float, default=30)
        p.add_argument('--max-restarts', type=int, default=5); p.add_argument('--backoff-s', type=float, default=10)
        p.add_argument('--rescan', nargs='*'); p.add_argument('--wait', type=float, default=180)
    p = sp.add_parser('plan'); p.add_argument('cells'); common(p); p.add_argument('--out', default='plan.json')
    for n in ('launch', 'pass2', 'watch'): p = sp.add_parser(n); p.add_argument('plan'); lw(p)
    p = sp.add_parser('auto'); p.add_argument('--cells'); p.add_argument('--capdirs', nargs='*'); common(p); lw(p)
    a = ap.parse_args(argv)
    if a.cmd == 'plan':
        if not a.cores: sys.exit('--cores required (host core list, e.g. 4-19)')
        plan = make_plan(json.load(open(a.cells)), a.cores, a.max_span_hz, a.rate, a.pub_cores, a.ring); plan['cells_json'] = os.path.abspath(a.cells)
        save(plan, a.out); print(f"window centre {plan['centre_hz']} Hz: {len(plan['cells'])} served, {len(plan['unserved'])} unserved -> {a.out}")
        for c in plan['cells']: print('  ' + spec(c))
        for u in plan['unserved']: print(f"  unserved {u['tag']} [{u['lo_hz']}, {u['hi_hz']}]")
        return
    if a.cmd == 'auto':
        os.makedirs(a.out, exist_ok=True); cj = a.cells
        if not cj:
            subprocess.run(['bash', os.path.join(AF, 'live_chain.sh'), 'discover', a.out] + (a.capdirs or []), check=True); cj = os.path.join(a.out, 'cells.json')
        plan = make_plan(json.load(open(cj)), a.cores, a.max_span_hz, a.rate, a.pub_cores, a.ring); plan['cells_json'] = os.path.abspath(cj); pp = os.path.join(a.out, 'plan.json')
    else:
        pp = a.plan; plan = json.load(open(pp))
    for k in ('base_cfg', 'build'):
        if getattr(a, k): plan[k] = os.path.abspath(getattr(a, k))
    if a.cmd in ('launch', 'auto'):
        save(plan, pp); launch(plan, a.out, a.pub_cmd)
        if os.environ.get('DRY') == '1': return
    if a.cmd in ('pass2', 'auto'): cmd_pass2(plan, a.out, pp, a.wait)
    if a.cmd in ('watch', 'auto'): cmd_watch(plan, a.out, a.period, a.rescan, lost_s=a.lost_s, max_restarts=a.max_restarts, backoff_s=a.backoff_s)

if __name__ == '__main__': main(sys.argv[1:])
