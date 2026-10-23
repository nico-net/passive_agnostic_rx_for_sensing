#!/usr/bin/env python3
"""mpci_auto.py : blind multi-cell SUPERVISOR -- cells.json (fastscan) -> one radio window -> N cell receivers (mpci_launch.sh) -> pass 2 -> watch.
  mpci_auto.py plan CELLS.json --cores 4-19 [--single-window] [--channels 4 --min-ch-per-window 1 --dwell-min-ms 40 --dwell-max-ms 200] [--max-span-hz 110e6] [--rate 122.88e6] [--pub-cores 0-1] [--ring isac_iq] [--out plan.json]
  mpci_auto.py launch PLAN.json --out DIR [--pub-cmd CMD] [--base-cfg CFG] [--build DIR]      (DRY=1 env: print only)
  mpci_auto.py pass2 PLAN.json --out DIR [--wait 180]    per cell: sync+MIB+SIB1 in ue.log -> autofront refine + retune_args auto; restart ONLY a cell whose grid changes
  mpci_auto.py watch PLAN.json --out DIR [--period 10] [--lost-s 30] [--max-restarts 5] [--backoff-s 10] [--rescan CAPDIR...]
  mpci_auto.py plan CELLS.json --cores ... --uhd-args ARGS --gain G [--rate 245.76e6] [--channels 4] [--antenna RX1] [--ibw-hz HZ] [--multi-window] [--fallback-dwell --radio-channels 4]
      DEFAULT whenever --uhd-args is given (--wideband = no-op alias; --rate defaults to the widest ring rate (245.76 then 122.88 MS/s) whose MEASURED GPU channelizer capacity for the served cells is >= 3x real time, see choose_rate); --multi-window = M4 fallback plan.
      --no-chan / --chan-backend gpu|cpu / --chan-cores L: wideband plans add the shared channelizer (shm_iq_chan -> one narrow ring <ring>_<tag> per cell, receivers decim 1) unless --no-chan
      wideband mode: ONE window on ALL channels (standalone UHD publisher shm_iq_uhd_pub), consumers decimate by rate/cell_rate in 1,2,4,8,16
  mpci_auto.py auto (--cells CELLS.json | --capdirs DIR...) --out DIR --cores 4-19 [plan/launch/watch options]
Window rule: a cell occupies the interval C +- BW/2. BW = SIB1 carrier width (sib1_nrb, pass-2 data in cells.json) else the pass-1 grid: the smallest
receiver-supported 30 kHz grid whose half width reaches autofront.RB_GRID_SC (the --ssb 1171 first-pass rule puts the grid centre there) = 217 PRB.
No operator constants: everything is the scanner's output or a receiver rule. The window centre covers the most cells within --max-span-hz; the
rest are `unserved` with --single-window. Default = M4: cover_windows() greedy cover of ALL cells, assign_channels() maps windows to radio channel subsets / dwell groups (rules in its docstring), plan.json v2.
Stop/restart = SIGINT only (X410 claim hygiene), never SIGKILL/pkill. Polling, no daemons.
shortcut: one window, report-only rescan, 30 kHz grids (supported-grid list copied from retune_args.py), pass-2 interval uses the carrier width not the grid."""
import sys, os, re, json, time, subprocess, argparse, shlex
H = os.path.dirname(os.path.abspath(__file__)); AF = os.path.join(H, '..', 'autofront'); SURVEY = os.path.join(H, '..', 'survey')
sys.path.insert(0, AF)
import autofront as A

SUPPORTED = (51, 106, 133, 162, 189, 217, 273)         # get_samplerate_and_bw(mu=1), same list as survey/retune_args.py
RB_HZ = 12 * A.SCS_HZ
R1 = next(r for r in SUPPORTED if r * RB_HZ / 2 >= A.RB_GRID_SC * A.SCS_HZ)   # pass-1 grid (see docstring)

RATE_OF = {51: 30.72e6, 106: 61.44e6, 133: 61.44e6, 162: 61.44e6, 189: 122.88e6, 217: 122.88e6, 273: 122.88e6}   # get_samplerate_and_bw(mu=1), no 3/4 fs

def decim_of(ring_rate, grid):
    """consumer decimation ring_rate / cell rate of a supported grid; None unless a power of two in 1..16 (iq_ddc)."""
    d = ring_rate / RATE_OF[grid]
    return int(round(d)) if abs(d - round(d)) < 1e-9 and 1 <= round(d) <= 16 and not int(round(d)) & (int(round(d)) - 1) else None

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

def cover_windows(entries, max_span_hz):
    """greedy interval cover (optimal): open a window at the leftmost uncovered lo, take every cell ending within lo+span.
    A cell wider than the span gets a window of its own (flagged oversize by the caller)."""
    rest = sorted(entries, key=lambda x: (x['lo'], x['hi'])); out = []
    while rest:
        L = rest[0]['lo']; w = [x for x in rest if x['hi'] <= L + max_span_hz] or [rest[0]]
        out.append(w); rest = [x for x in rest if x not in w]
    return out

def subset_str(l):
    """channel list -> publisher syntax (ISAC_SHM_IQ_PUB): 'a-b' for a run, '+' joins runs/singles."""
    r = []; i = 0
    while i < len(l):
        j = i
        while j + 1 < len(l) and l[j + 1] == l[j] + 1: j += 1
        r.append(str(l[i]) if i == j else f'{l[i]}-{l[j]}'); i = j + 1
    return '+'.join(r)

def assign_channels(wins, channels, min_ch, dwell_min_ms, dwell_max_ms):
    """wins: list of cell lists (cover order). Returns per window dict(ch=[..], mode='dedicated'|'dwell', dwell_ms).
    Rule: D = channels // min_ch dedicated slots. W <= D: every window dedicated, channels split as evenly as possible, the
    remainder going one each to the windows with MORE cells (tie: lower frequency) -- more RX per cell where more cells need it.
    W > D: the D-1 windows with the most cells (tie: lower frequency) get min_ch channels each, all the others time-share the
    remaining channels (>= min_ch) with dwell = dwell_min_ms * cells in the window, clamped to [min, max]: dwell_min_ms >= 2 SSB
    periods (2 x 20 ms) so a return always sees an SSB, and the cap bounds the time any window is away."""
    W = len(wins); D = max(1, channels // min_ch); order = sorted(range(W), key=lambda k: (-len(wins[k]), k)); res = [None] * W
    if W <= D:
        n = [channels // W] * W
        for k in order[:channels % W]: n[k] += 1
        groups = {k: n[k] for k in range(W)}; shared = []
    else:
        groups = {k: min_ch for k in order[:D - 1]}; shared = [k for k in range(W) if k not in groups]
    nxt = 0; ch = {}
    for k in sorted(groups): ch[k] = list(range(nxt, nxt + groups[k])); nxt += groups[k]
    for k in range(W):
        if k in ch: res[k] = dict(ch=ch[k], mode='dedicated', dwell_ms=None)
    for k in shared:
        res[k] = dict(ch=list(range(nxt, channels)), mode='dwell', dwell_ms=int(min(dwell_max_ms, max(dwell_min_ms, dwell_min_ms * len(wins[k])))))
    return res

def pub_env(ring, windows):
    """publisher env strings in the exact syntax of radio/COMMON/shm_iq_pub.c (parse_env / parse_freq_dwell)."""
    if len(windows) == 1 and windows[0]['mode'] == 'dedicated': return {'ISAC_SHM_IQ_PUB': windows[0]['ring']}   # v1 form: one ring, all channels
    e = {'ISAC_SHM_IQ_PUB': ','.join(f"{w['ring']}:{subset_str(w['channels'])}" for w in windows)}
    f = [f"{w['ring']}={w['centre_hz']}" for w in windows if w['mode'] == 'dedicated']
    d = [f"{w['ring']}@{w['centre_hz']}:{w['dwell_ms']}" for w in windows if w['mode'] == 'dwell']
    if f: e['ISAC_SHM_IQ_PUB_FREQ'] = ','.join(f)
    if d: e['ISAC_SHM_IQ_DWELL'] = ','.join(d)
    return e


# ---- host resources (MPCIFIX): RAM admission, DGX GB10 core map, channelizer capacity ----------------------------------------------------------
RAM_FLOOR_GB = 16.0      # never let MemAvailable fall below this (user rule); --ram-floor-gb can only raise it
def cell_ram_gb(r):
    """host + GPU memory one receiver of a grid of r PRB takes, GB. The GB10 has ONE memory pool, so MemAvailable drops by both.
    [MEASURED, DGX replay + OTA 2026-10-15] anon RSS 13 GB / GPU 11 GB at 217 PRB; 3.6 / 9 GB at 51 PRB; linear in PRB in between and beyond
    (106..189, 273 are interpolated/extrapolated, not measured). x1.1: two 217-PRB cells + ring dropped MemAvailable 110 -> 55 GB on air."""
    host = 3.6 + (13.0 - 3.6) * (r - 51) / (217 - 51); gpu = 9.0 + 2.0 * (r - 51) / (217 - 51)
    return 1.1 * (host + gpu)

def ring_gb(channels, capacity=1 << 27):
    return (4096 + channels * capacity * 4) / 2**30      # /dev/shm ring (publisher default 2^27 samples)

def mem_available_gb():
    try:
        for l in open('/proc/meminfo'):
            if l.startswith('MemAvailable:'): return int(l.split()[1]) / 2**20
    except OSError: pass
    return None

def ram_limit(entries, avail_gb, floor_gb=RAM_FLOOR_GB, fixed_gb=0.0):
    """admit cells (cheapest first, so the most cells fit; ties keep the input order) while avail - fixed - sum(cost) >= floor.
    Returns (kept in the input order, dropped, info). avail_gb None = unknown host: nothing is dropped (info says so)."""
    floor_gb = max(RAM_FLOOR_GB, floor_gb)
    if avail_gb is None: return list(entries), [], dict(avail_gb=None, floor_gb=floor_gb, note='MemAvailable unknown: RAM not checked')
    budget = avail_gb - floor_gb - fixed_gb; kept = set(); used = 0.0
    for i in sorted(range(len(entries)), key=lambda i: (cell_ram_gb(entries[i]['r']), i)):
        cost = cell_ram_gb(entries[i]['r'])
        if used + cost <= budget: kept.add(i); used += cost
    return ([e for i, e in enumerate(entries) if i in kept], [e for i, e in enumerate(entries) if i not in kept],
            dict(avail_gb=round(avail_gb, 1), floor_gb=floor_gb, fixed_gb=round(fixed_gb, 1), planned_gb=round(used, 1), left_gb=round(avail_gb - fixed_gb - used, 1)))

# DGX Spark GB10 / t26f split (MPCI-DASH-REVIEW-20261015 section 4). Cores 15-19 belong to the radio side (uhd_ctrl 15, ring producer 16, DPDK 17-19),
# core 10 to the channelizer main thread. Per cell: mask = every core the cell's threads may use (USS/NID/GPU worker stay unpinned inside it);
# pdsch / scan / dmrs / ul = the pins of ISAC_PDSCH_CORES, pdcch_blind_monitor_scan_thread (cfg field), ISAC_DMRS_SCAN_CORE, ISAC_PUSCH_CORES+ISAC_UL_SNAP_CORE.
# N = 1: the whole t26f map of sd_t26ul. N = 2: the review's A/B split. N = 3: the third cell is a LIGHT cell sharing little cores with A and B (reduced
# capacity, flagged 'shared'); more than 3 cells have no pin map (plan refuses them with a core budget message).
T26F_MAP = {
    1: [dict(mask='0-9,11-14', pdsch='7+8+9+3+4', scan=6, dmrs=2, ul='0+1')],
    2: [dict(mask='0,2,3,5,6,7,12', pdsch='7+3', scan=5, dmrs=2, ul='0'), dict(mask='1,4,8,9,11,13,14', pdsch='4+11', scan=8, dmrs=13, ul='1')],
    3: [dict(mask='0,2,3,5,6,7,12', pdsch='7+3', scan=5, dmrs=2, ul='0'), dict(mask='1,4,8,9,11,13,14', pdsch='4+11', scan=8, dmrs=13, ul='1'),
        dict(mask='12,13,14', pdsch='12', scan=14, dmrs=13, ul='12', shared=True)],
}
T26F_CHAN_CORE = '10'; T26F_PUB_CORES = '15-19'; T26F_MAX_CELLS = 3

def t26f_cells_ok(cores, n):
    """True when the host core list contains every core of the n-cell map (+ channelizer core 10)."""
    if n not in T26F_MAP: return False
    need = {int(x) for m in T26F_MAP[n] for x in expand_cores(m['mask'])} | {int(T26F_CHAN_CORE)}
    return need <= set(expand_cores(cores))

def apply_core_map(cells, cores, mode='auto'):
    """cells: planned entries sorted by frequency (idx order). Sets cores (mask) and the pins in env (SCAN_CORE is a launcher pseudo-key).
    mode auto = t26f map when the host core list covers it, else the caller's even split stays; t26f = demand it; none = off. Returns a note or None."""
    n = len(cells)
    if mode == 'none': return None
    if not t26f_cells_ok(cores, n):
        if mode == 't26f': sys.exit(f'--core-map t26f: {n} cells need the t26f map cores (<= {T26F_MAX_CELLS} cells, host core list must contain them and core {T26F_CHAN_CORE}); --cores {cores}')
        return None
    for c, m in zip(cells, T26F_MAP[n]):
        c['cores'] = m['mask']; c['env'] = {**c['env'], 'ISAC_PDSCH_CORES': m['pdsch'], 'ISAC_DMRS_SCAN_CORE': str(m['dmrs']), 'ISAC_PUSCH_CORES': m['ul'],
                                            'ISAC_UL_SNAP_CORE': m['ul'].split('+')[0], 'SCAN_CORE': str(m['scan'])}
        if m.get('shared'): c['core_note'] = 'light cell: little cores shared with the other cells'
    return f't26f core map for {n} cell(s)'

# Channelizer capacity, x real time, [MEASURED, DGX GB10 idle, shm_iq_chan --bench 3 s, 4 channels, MPCIFIX 2026-10-15 09:03, 3 reps agree within 1 %, new binary].
# Key: (ring rate MS/s, cells). Lookup takes the first entry with cells >= n (conservative). GPU = pipelined, fused kernels, default 1M-sample block;
# cell rate 61.44 MS/s at 245.76 and 30.72 MS/s at 122.88 (lighter cells are not slower). Idle numbers: the margin below covers the time slicing with cell receivers.
CHAN_CAP_GPU = {(122.88, 1): 13.5, (122.88, 2): 8.7, (122.88, 4): 5.1, (245.76, 1): 6.7, (245.76, 2): 4.3, (245.76, 4): 2.55}
# CPU, ONE thread, one cell, worst measured cell rate per ring rate (decim 4 / 2): the CPU backend serves one cell per thread.
CHAN_CAP_CPU_1T = {(122.88, 1): 2.4, (245.76, 1): 1.19}
CHAN_MARGIN = 3.0     # the old 245.76 MS/s 2-cell channelizer measured 2.05x idle and ran at 0.85x on air (time-sliced with the cell receivers): x2.4 loss

def chan_capacity(backend, rate_hz, ncells, table=None):
    tab = table or (CHAN_CAP_GPU if backend == 'gpu' else CHAN_CAP_CPU_1T); mrate = round(rate_hz / 1e6, 2)
    ks = sorted(k for k in tab if k[0] == mrate and k[1] >= ncells)
    return tab[ks[0]] if ks else 0.0

def choose_rate(backend, ncells_at, table=None, margin=CHAN_MARGIN):
    """default ring rate = the widest window (245.76 then 122.88 MS/s) whose MEASURED channelizer capacity for the number of cells it would serve is
    >= margin x real time; ncells_at(rate) = cells that window serves. Returns (rate_hz, info)."""
    info = {}
    for rate in (245.76e6, 122.88e6):
        n = ncells_at(rate); cap = chan_capacity(backend, rate, n, table); info[f'{rate / 1e6:.2f}'] = dict(cells=n, capacity_x_rt=cap)
        if n and cap >= margin: return rate, info
    return 122.88e6, info

FALLBACK = 'M4 multi-window plan (plan without --wideband) for these cells; a dwell window cannot share the radio while the wideband window uses all channels'

def make_wideband(entries, cores, rate, pub_cores, ring, channels, uhd_args, gain, antenna, ibw_hz, fallback_dwell=False, radio_channels=4,
                  chan=False, chan_backend='gpu', chan_cores=None, chan_cells='chan_cells.json', auto_rate=False, mem_avail_gb=None, ram_floor_gb=RAM_FLOOR_GB,
                  core_map='auto', chan_table=None, ram_check=True):
    """ONE window, all channels, one LO. Usable span = the radio's instantaneous bandwidth ibw_hz (default rate * 200/245.76: the X410
    datasheet IBW, 200 MHz at 245.76 MS/s and 400 MHz at 491.52 MS/s; outside it the radio's own filter rolls off); a cell needs [C - BW/2, C + BW/2] inside it. The window covers the most cells
    (plan_window rule), its centre is the middle of the served extent. Cells whose rate does not divide the ring rate by 1,2,4,8,16 are unserved."""
    if not uhd_args or gain is None: sys.exit('wideband needs --uhd-args and --gain (no site defaults)')
    if fallback_dwell:    # shortcut: spare-channel M4 window (cover_windows/assign_channels on channels..radio_channels-1) not implemented, refusal only
        sys.exit('--fallback-dwell refused: in wideband mode all antennas share one LO, a dwell visit would retune them away from the wideband window; '
                 + (f'spare channels ({radio_channels - channels}) could host an M4 window but that is not implemented, use --multi-window' if channels < radio_channels
                    else f'--channels {channels} uses all {radio_channels} radio channels'))
    def feasible(rt):
        sp = ibw_hz or rt * 200 / 245.76; g = [dict(e, decim=decim_of(rt, e['r'])) for e in entries]
        return sp, [x for x in g if x['decim']], [x for x in g if not x['decim']]
    rate_info = None
    if auto_rate:    # the widest window the MEASURED channelizer capacity carries (see choose_rate); without a channelizer there is no such limit
        if chan: rate, rate_info = choose_rate(chan_backend, lambda rt: len(plan_window(feasible(rt)[1], feasible(rt)[0])[1]), chan_table)
        else: rate = 245.76e6
    span = ibw_hz or rate * 200 / 245.76; ok = []; bad = []
    for x in entries:
        x['decim'] = decim_of(rate, x['r'])
        (ok if x['decim'] else bad).append(x)
    centre, served, out = plan_window(ok, span)
    if not served: sys.exit('no cells to serve')
    ram_info = None; dropped = []
    if ram_check:    # RAM admission: the window loses the cells that would push MemAvailable under the floor
        served, ram_drop, ram_info = ram_limit(served, mem_available_gb() if mem_avail_gb is None else mem_avail_gb, ram_floor_gb,
                                               ring_gb(channels) + (0.2 if chan else 0.0))
        if not served: sys.exit(f'RAM: not even the cheapest cell fits ({ram_info}); free memory or lower --ram-floor-gb (never below {RAM_FLOOR_GB:g})')
        dropped += [(x, 'ram') for x in ram_drop]
    if core_map != 'none' and len(served) > T26F_MAX_CELLS and t26f_cells_ok(cores, min(len(served), T26F_MAX_CELLS)):
        served = sorted(served, key=lambda x: x['lo']); dropped += [(x, 'cores') for x in served[T26F_MAX_CELLS:]]; served = served[:T26F_MAX_CELLS]
    if dropped:
        ids = {id(x) for x, _ in dropped}; out = [x for x in out if id(x) not in ids]
        centre = int(round((min(x['lo'] for x in served) + max(x['hi'] for x in served)) / 2))
    cl = expand_cores(cores)
    mapped = core_map != 'none' and t26f_cells_ok(cores, len(served))
    if core_map == 't26f' and not mapped: apply_core_map(served, cores, 't26f')     # refuses with the reason
    if not mapped and len(cl) < len(served): sys.exit(f'{len(cl)} cores for {len(served)} cells')
    if mapped:
        pub_cores = pub_cores or T26F_PUB_CORES; chan_cores = chan_cores or (T26F_CHAN_CORE if chan else None)
        cl = sorted({int(c) for m in T26F_MAP[len(served)] for c in expand_cores(m['mask'])})     # the cores the cells actually use
    if chan:
        if not chan_cores: sys.exit('the channelizer needs --chan-cores (no site defaults; --no-chan = per-cell decimation in each receiver)')
        if chan_backend not in ('gpu', 'cpu'): sys.exit('--chan-backend gpu|cpu')
        cc = expand_cores(chan_cores)
        if set(cc) & (set(cl) | set(expand_cores(pub_cores) if pub_cores else ())): sys.exit(f'--chan-cores {chan_cores} overlap the cell cores {fmt_cores(cl)} or --pub-cores {pub_cores}')
    n = len(cl) // len(served); gs = [x['gscn'] for x in served]; keep = ('tag', 'pci', 'gscn', 'C_hz', 'r', 'ssb', 'initial_fo', 'bw_hz', 'pass_', 'env', 'cores', 'idx', 'ring', 'nrx', 'window', 'decim', 'chan_decim', 'core_note')
    pcells = []; core_note = None
    if mapped: core_note = apply_core_map(sorted(served, key=lambda x: x['lo']), cores, 't26f')
    for idx, x in enumerate(sorted(served, key=lambda x: x['lo'])):
        x.update(idx=idx, ring=ring, nrx=channels, window=0)
        if not mapped: x['cores'] = fmt_cores(cl[idx * n:(idx + 1) * n])
        if chan: x.update(chan_decim=x['decim'], decim=1, ring=f"{ring}_{x['tag']}")      # the receiver reads the channelizer's narrow ring as is
        if x['n_cells'] > 1 or gs.count(x['gscn']) > 1: x['env'] = {**x['env'], **dict(ISAC_TARGET_PCI=str(x['pci']), ISAC_SSS_MULTI='2.5')}
        pcells.append({q: x[q] for q in keep if q in x})
    pub = ['shm_iq_uhd_pub', '--args', uhd_args, '--rate', f'{rate:.0f}', '--freq', str(centre), '--gain', str(gain), '--channels', str(channels),
           '--antenna', antenna, '--ring', ring]
    win = dict(ring=ring, centre_hz=centre, channels=list(range(channels)), mode='dedicated', dwell_ms=None, wideband=True, cells=[x['tag'] for x in served],
               span_hz=int(max(x['hi'] for x in served) - min(x['lo'] for x in served)))
    un = [dict(tag=x['tag'], pci=x['pci'], gscn=x['gscn'], C_hz=x['C_hz'], lo_hz=int(x['lo']), hi_hz=int(x['hi']), fallback=FALLBACK,
               reason='decim' if x in bad else 'outside window') for x in out + bad]
    un += [dict(tag=x['tag'], pci=x['pci'], gscn=x['gscn'], C_hz=x['C_hz'], lo_hz=int(x['lo']), hi_hz=int(x['hi']), fallback=FALLBACK, reason=why) for x, why in dropped]
    rd = dict(channels=channels, min_ch_per_window=channels, pub_env={}, wideband=True, ibw_hz=int(span), pub_cmd=' '.join(shlex.quote(a) for a in pub), chan=bool(chan))
    if ram_info: rd['ram'] = ram_info
    if rate_info: rd['rate_choice'] = rate_info
    if core_note: rd['core_map'] = core_note
    if chan:
        # --cells-json reads a dedicated small file (chan_cells_doc), rewritten from the plan before every channelizer start: pass 2 may change centre/rate
        be = ['--gpu'] if chan_backend == 'gpu' else ['--threads', str(len(cc))]
        rd.update(chan_backend=chan_backend, chan_cores=chan_cores, chan_cells=chan_cells,
                  chan_cmd=' '.join(shlex.quote(a) for a in ['shm_iq_chan', '--ring', ring, '--cells-json', chan_cells, '--attach-s', '120'] + be + ['--cores', chan_cores]))
        if chan_backend == 'cpu': rd['chan_warn'] = 'CPU channelizer is real-time for about ONE cell only (MPCI-CHAN: 4 ch x 245.76 MS/s, 3 threads: 0.98x for 1 cell, 0.66x for 2) [MEASURED, cloud x86 4c]; use --chan-backend gpu'
    return dict(version=2, ring=ring, centre_hz=centre, rate_hz=int(rate), span_hz=int(span), pub_cores=pub_cores, cores=cores,
                radio=rd, windows=[win], cells=pcells, unserved=un)

def make_plan(cells, cores, max_span_hz=110e6, rate=122.88e6, pub_cores=None, ring='isac_iq', multi=False, channels=4, min_ch=1,
              dwell_min_ms=40, dwell_max_ms=200, wideband=False, uhd_args=None, gain=None, antenna='RX1', ibw_hz=None, fallback_dwell=False, radio_channels=4,
              chan=False, chan_backend='gpu', chan_cores=None, chan_cells='chan_cells.json', auto_rate=False, mem_avail_gb=None, ram_floor_gb=RAM_FLOOR_GB,
              core_map='auto', chan_table=None, ram_check=True):
    entries = [cell_entry(c) for c in cells]
    if wideband: return make_wideband(entries, cores, rate, pub_cores, ring, channels, uhd_args, gain, antenna, ibw_hz, fallback_dwell, radio_channels,
                                          chan, chan_backend, chan_cores, chan_cells, auto_rate, mem_avail_gb, ram_floor_gb, core_map, chan_table, ram_check)
    if multi: wins = cover_windows(entries, max_span_hz); unserved = []
    else:
        centre, served, unserved = plan_window(entries, max_span_hz)
        wins = [served] if served else []
    if not wins: sys.exit('no cells to serve')
    ram_dropped = []
    if ram_check:    # RAM admission (the M4 windows share one host): drop what would push MemAvailable under the floor
        flat = [x for w in wins for x in w]
        kept, ram_dropped, ram_info = ram_limit(flat, mem_available_gb() if mem_avail_gb is None else mem_avail_gb, ram_floor_gb, ring_gb(channels))
        if not kept: sys.exit(f'RAM: not even the cheapest cell fits ({ram_info})')
        ids = {id(x) for x in kept}; wins = [w for w in ([x for x in w if id(x) in ids] for w in wins) if w]; unserved = list(unserved) + [dict(x, _why='ram') for x in ram_dropped]
    cl = expand_cores(cores); nserved = sum(len(w) for w in wins)
    if len(cl) < nserved: sys.exit(f'{len(cl)} cores for {nserved} cells')
    if min_ch < 1 or channels < min_ch: sys.exit(f'--channels {channels} < --min-ch-per-window {min_ch}')
    n = len(cl) // nserved; asg = assign_channels(wins, channels, min_ch, dwell_min_ms, dwell_max_ms)
    keep = ('tag', 'pci', 'gscn', 'C_hz', 'r', 'ssb', 'initial_fo', 'bw_hz', 'pass_', 'env', 'cores', 'idx', 'ring', 'nrx', 'window')
    windows = []; pcells = []; idx = 0
    for k, (w, a) in enumerate(zip(wins, asg)):
        rn = ring if len(wins) == 1 else f'{ring}_w{k}'
        centre = int(round((min(x['lo'] for x in w) + max(x['hi'] for x in w)) / 2))
        gs = [x['gscn'] for x in w]
        for x in sorted(w, key=lambda x: x['lo']):
            x.update(cores=fmt_cores(cl[idx * n:(idx + 1) * n]), idx=idx, ring=rn, nrx=len(a['ch']), window=k); idx += 1
            if x['n_cells'] > 1 or gs.count(x['gscn']) > 1: x['env'] = dict(ISAC_TARGET_PCI=str(x['pci']), ISAC_SSS_MULTI='2.5')   # co-channel pin: per window
            pcells.append({q: x[q] for q in keep})
        windows.append(dict(ring=rn, centre_hz=centre, channels=a['ch'], mode=a['mode'], dwell_ms=a['dwell_ms'], cells=[x['tag'] for x in w],
                            span_hz=int(max(x['hi'] for x in w) - min(x['lo'] for x in w))))
    return dict(version=2, ring=ring, centre_hz=windows[0]['centre_hz'], rate_hz=int(rate), span_hz=int(max_span_hz), pub_cores=pub_cores, cores=cores,
                radio=dict(channels=channels, min_ch_per_window=min_ch, pub_env=pub_env(ring, windows)), windows=windows,
                cells=sorted(pcells, key=lambda x: x['idx']),
                unserved=[dict(tag=x['tag'], pci=x['pci'], gscn=x['gscn'], C_hz=x['C_hz'], lo_hz=int(x['lo']), hi_hz=int(x['hi']), reason=x.get('_why', 'outside window')) for x in unserved])

def spec(c):
    s = f"{c['tag']}:{c['C_hz']}:{c['r']}:{c['ssb']}:{c['initial_fo'] if c.get('initial_fo') is not None else '-'}:{c['cores']}"
    e = dict(c['env'])
    if c.get('ring'): e['ISAC_SHM_IQ_NAME'] = c['ring']            # per-cell ring (launcher default is the global RING)
    if c.get('nrx'): e['UE_NB_ANT_RX'] = str(c['nrx'])             # launcher turns this pseudo-key into --ue-nb-ant-rx
    return s + (':' + ','.join(f'{k}={v}' for k, v in sorted(e.items())) if e else '')

def launcher_env(plan, out, extra=None):
    e = dict(os.environ, OUT=out, RING=plan['ring']); e.update(extra or {})
    pe = (plan.get('radio') or {}).get('pub_env')
    if pe: e['PUB_ENV'] = ' '.join(f'{k}={v}' for k, v in pe.items())
    for k, pk in (('BASE_CFG', 'base_cfg'), ('BUILD', 'build')):
        if plan.get(pk): e.setdefault(k, plan[pk])
    return e

def chan_cells_doc(plan):
    """the file shm_iq_chan --cells-json reads (load_plan: ring, cells[].tag/C_hz/decim/bw_hz): the CURRENT plan, so pass-2 retunes are included."""
    return dict(ring=plan['ring'], cells=[dict(tag=c['tag'], C_hz=c['C_hz'], decim=c['chan_decim'], bw_hz=c['bw_hz']) for c in plan['cells']])

def write_chan_cells(plan):
    json.dump(chan_cells_doc(plan), open(plan['radio']['chan_cells'], 'w'), indent=1)

def chan_env(plan):
    r = plan['radio']; return dict(CHAN_CMD=r['chan_cmd'], CHAN_RINGS=' '.join(c['ring'] for c in plan['cells']))

def launch_chan(plan, out, run=subprocess.run):
    """(re)start ONLY the channelizer (launcher CHAN_ONLY=1, pid appended to OUT/pids); True when its output rings exist."""
    if os.environ.get('DRY') != '1': write_chan_cells(plan)
    e = launcher_env(plan, out, dict(chan_env(plan), CHAN_ONLY='1', APPEND='1'))
    r = run(['bash', os.path.join(H, 'mpci_launch.sh'), 'start'], env=e, capture_output=True, text=True)
    if r.returncode: print(f'channelizer start failed: {r.stderr.strip()}')
    return r.returncode == 0

def restart_chan(plan, out, wait_s=60):
    """SIGINT the channelizer (it unlinks its rings), keep its log, start it with the current cell list. Its cells are orphaned by this
    (their rings are new inodes): the caller restarts them."""
    if not sigint_wait(pid_of(out, 'chan'), wait_s): print(f'chan: old pid still alive after {wait_s} s SIGINT, not restarting'); return False
    log = os.path.join(out, 'chan.log')
    if os.path.exists(log): os.replace(log, f'{log}.{int(time.time())}')
    return launch_chan(plan, out)

def restart_chan_and_cells(plan, out):
    """policy for a changed cell list (pass 2) or a dead channelizer: channelizer first, then EVERY cell (all of them read rings it re-created)."""
    if not restart_chan(plan, out): return False
    return all([restart_cell(plan, out, c) for c in plan['cells']])

def launch(plan, out, pub_cmd=None, cells=None, append=False, run=subprocess.run):
    cells = cells or plan['cells']; extra = {}
    if not append and (plan.get('radio') or {}).get('chan_cmd'):
        if os.environ.get('DRY') != '1': write_chan_cells(plan)
        extra.update(chan_env(plan))
    if not append and not pub_cmd: pub_cmd = (plan.get('radio') or {}).get('pub_cmd')    # wideband: standalone UHD publisher
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

def win_centre(plan, c):
    w = plan.get('windows')
    return w[c['window']]['centre_hz'] if w and c.get('window') is not None else plan['centre_hz']

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
    elif abs(d['C_hz'] - win_centre(plan, c)) + d['r'] * RB_HZ / 2 > plan['span_hz'] / 2:
        c['pass2'] = 'outside_window'; c['wanted'] = d; res = 'outside_window'
        print(f"{c['tag']}: refined carrier {d['C_hz']} Hz (grid {d['r']} PRB) leaves window {c.get('ring', '')} centre {win_centre(plan, c)} Hz: NOT restarted, re-plan (mpci_auto.py plan/auto with the refined cells.json)")
    else:
        c.update(C_hz=d['C_hz'], r=d['r'], ssb=d['ssb'], bw_hz=info['sib1_nrb'] * RB_HZ, pass2='retuned')
        chan = (plan.get('radio') or {}).get('chan_cmd'); k = 'chan_decim' if chan else 'decim'
        if c.get(k): c[k] = decim_of(plan['rate_hz'], d['r']); c['env'] = {**c['env'], **d['env']}
        if chan and not c[k]: c['pass2'] = 'decim_unsupported'; res = 'done'; print(f"{c['tag']}: grid {d['r']} PRB has no channelizer decimation at {plan['rate_hz']} Hz: NOT restarted")
        elif chan: res = 'restarted' if restart_chan_and_cells(plan, out) else 'done'    # new centre/rate -> new channelizer cell list -> all cells re-attach
        else: res = 'restarted' if restart_cell(plan, out, c) else 'done'
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

def chan_status(out):
    try: t = open(os.path.join(out, 'chan.log'), errors='replace').read()
    except OSError: t = ''
    st = re.findall(r'SHMIQ_CHAN_STAT .*', t)
    return {k: t.count('SHMIQ_CHAN_' + k) for k in ('OVERRUN', 'DISCONT', 'PRODUCER_LOST', 'WARN', 'ERR')}, (st[-1] if st else '')

def watch_step(plan, out, rt, now, lost_s=30, max_restarts=5, backoff_s=10, restart=restart_cell, restart_ch=restart_chan):
    """one polling period; rt[tag] = dict(restarts, lost_since, next_ok); rt['chan'] likewise for the channelizer. Returns status lines."""
    lines = []
    if (plan.get('radio') or {}).get('chan_cmd'):
        r = rt.setdefault('chan', dict(restarts=0, next_ok=0)); up = alive(pid_of(out, 'chan')); state = 'UP' if up else 'DEAD'
        if not up and r['restarts'] >= max_restarts: state += ' GIVEUP'
        elif not up and now >= r['next_ok']:
            if restart_ch(plan, out):
                r['restarts'] += 1; state += ' RESTARTED'
                for c in plan['cells']: restart(plan, out, c)       # their narrow rings are new inodes
            r['next_ok'] = now + backoff_s * 2 ** r['restarts']
        elif not up: state += ' BACKOFF'
        n, last = chan_status(out)
        lines.append(f"chan state={state} restarts={r['restarts']} " + ' '.join(f'{k.lower()}={v}' for k, v in n.items()) + (f' | {last}' if last else ''))
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
    for w in plan.get('windows') or []:
        if len(plan['windows']) < 2 and w['mode'] == 'dedicated': continue
        lines.append(window_status(plan, out, w)); 
    return lines

def window_status(plan, out, w):
    """per-window line from logs only: dwell switches seen by the publisher (pub.log) and, per cell, discontinuities (return from away) / overruns."""
    try: pub = open(os.path.join(out, 'pub.log'), errors='replace').read()
    except OSError: pub = ''
    cs = [c for c in plan['cells'] if c.get('window') is not None and plan['windows'][c['window']] is w]
    dis = 0; ov = 0
    for c in cs:
        try: t = open(os.path.join(out, c['tag'], 'ue.log'), errors='replace').read()
        except OSError: continue
        dis += t.count('SHMIQ_DISCONT'); ov += t.count('SHMIQ_OVERRUN')
    last = re.findall(r'SHMIQ_PUB_DWELL from=/(\S+) to=/(\S+)', pub)
    where = ('' if w['mode'] == 'dedicated' else ' dwell=%dms switches=%d %s' % (w['dwell_ms'], len(last), 'AWAY' if last and last[-1][0] == w['ring'] else 'ON' if last and last[-1][1] == w['ring'] else '?'))
    return f"window {w['ring']} ch={subset_str(w['channels'])} {w['mode']}{where} returns_seen={dis} overruns={ov} cells={len(cs)}"

def rescan(plan, out, capdirs):
    d = os.path.join(out, 'rescan'); sh = os.path.join(AF, 'live_chain.sh')
    p = subprocess.run(['bash', sh, 'discover', d] + capdirs, capture_output=True, text=True)
    if p.returncode: return [f'rescan failed: {p.stderr[-200:]}']
    new = {(c['pci'], c['gscn']): cell_entry(c) for c in json.load(open(os.path.join(d, 'cells.json')))}
    have = {(c['pci'], c['gscn']) for c in plan['cells'] + plan['unserved']}
    half = plan['span_hz'] / 2; msgs = []
    for k, e in new.items():
        if k not in have: msgs.append(f"NEW {e['tag']} " + ('fits the window: rerun plan+launch' if any(abs(e['C_hz'] - w['centre_hz']) + e['bw_hz'] / 2 <= half for w in plan.get('windows') or [dict(centre_hz=plan['centre_hz'])]) else 'outside window'))
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

def mp_args(a):
    if a.multi_window and a.wideband: sys.exit('--wideband and --multi-window are exclusive')
    wb = bool(a.uhd_args) and not a.multi_window
    if not a.uhd_args and not a.multi_window: print('hint: wideband (one window, all antennas) is the default architecture and needs --uhd-args; planning M4 multi-window', file=sys.stderr)
    if a.wideband and not a.uhd_args: sys.exit('--wideband needs --uhd-args')
    cd = os.path.abspath(os.path.dirname(a.out) if a.cmd == 'plan' else a.out)
    return dict(chan=wb and not a.no_chan, chan_backend=a.chan_backend, chan_cores=a.chan_cores, chan_cells=os.path.join(cd, 'chan_cells.json'), max_span_hz=a.max_span_hz, rate=a.rate or (245.76e6 if wb else 122.88e6), auto_rate=bool(wb and not a.rate), mem_avail_gb=a.mem_avail_gb, ram_floor_gb=a.ram_floor_gb, core_map=a.core_map, ram_check=not a.no_ram_check, pub_cores=a.pub_cores, ring=a.ring, multi=not a.single_window, channels=a.channels,
                min_ch=a.min_ch_per_window, dwell_min_ms=a.dwell_min_ms, dwell_max_ms=a.dwell_max_ms,
                wideband=wb, uhd_args=a.uhd_args, gain=a.gain, antenna=a.antenna, ibw_hz=a.ibw_hz,
                fallback_dwell=a.fallback_dwell, radio_channels=a.radio_channels)

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); sp = ap.add_subparsers(dest='cmd', required=True)
    def common(p):
        p.add_argument('--cores'); p.add_argument('--max-span-hz', type=float, default=110e6); p.add_argument('--rate', type=float, default=None, help='default: wideband = chosen by measured channelizer capacity (choose_rate), 122.88e6 multi-window')
        p.add_argument('--pub-cores'); p.add_argument('--ring', default='isac_iq')
        p.add_argument('--single-window', action='store_true', help='v1: one window (most cells), the rest unserved')
        p.add_argument('--channels', type=int, default=4, help='radio RX channel count (host inventory)')
        p.add_argument('--min-ch-per-window', type=int, default=1)
        p.add_argument('--wideband', action='store_true', help='no-op alias: wideband is the default whenever --uhd-args is given')
        p.add_argument('--multi-window', action='store_true', help='explicit M4 multi-window plan (fallback architecture)')
        p.add_argument('--fallback-dwell', action='store_true', help='wideband: ask for dwell visits of unserved cells (refused, see message)')
        p.add_argument('--radio-channels', type=int, default=4, help='radio RX channel count for --fallback-dwell (default 4)')
        p.add_argument('--no-chan', action='store_true', help='wideband: no shared channelizer, every receiver decimates the wideband ring itself')
        p.add_argument('--chan-backend', choices=('gpu', 'cpu'), default='gpu'); p.add_argument('--chan-cores', help='channelizer cores (disjoint from --cores/--pub-cores; required unless --no-chan)')
        p.add_argument('--uhd-args'); p.add_argument('--gain', type=float); p.add_argument('--antenna', default='RX1')
        p.add_argument('--ibw-hz', type=float, default=None, help='wideband: radio instantaneous bandwidth (default rate*200/245.76, X410 IBW)')
        p.add_argument('--dwell-min-ms', type=int, default=40); p.add_argument('--dwell-max-ms', type=int, default=200)
        p.add_argument('--mem-avail-gb', type=float, default=None, help='free RAM to plan with (default: MemAvailable now); cells that would push it under the floor are left unserved (reason ram)')
        p.add_argument('--ram-floor-gb', type=float, default=RAM_FLOOR_GB, help=f'MemAvailable that must stay free (default and minimum {RAM_FLOOR_GB:g})')
        p.add_argument('--no-ram-check', action='store_true', help='do not limit cells by RAM (the OTA runner still aborts below 16 GB)')
        p.add_argument('--core-map', choices=('auto', 't26f', 'none'), default='auto', help='per-cell core pins of the DGX t26f split (<= 3 cells): auto = when --cores contains the map cores, else an even core split')
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
        plan = make_plan(json.load(open(a.cells)), a.cores, **mp_args(a)); plan['cells_json'] = os.path.abspath(a.cells)
        save(plan, a.out); print(f"{len(plan['windows'])} window(s): {len(plan['cells'])} served, {len(plan['unserved'])} unserved -> {a.out}")
        for w in plan['windows']: print(f"  window {w['ring']} centre {w['centre_hz']} Hz ch={subset_str(w['channels'])} {w['mode']}" + (f" dwell {w['dwell_ms']} ms" if w['dwell_ms'] else ''))
        for k, v in plan['radio']['pub_env'].items(): print(f'  {k}={v}')
        if plan['radio'].get('pub_cmd'): print('  publisher: ' + plan['radio']['pub_cmd'])
        if plan['radio'].get('chan_cmd'): print('  channelizer: ' + plan['radio']['chan_cmd']); write_chan_cells(plan)
        if plan['radio'].get('chan_warn'): print('  WARNING: ' + plan['radio']['chan_warn'])
        for c in plan['cells']: print('  ' + spec(c))
        for u in plan['unserved']: print(f"  unserved {u['tag']} [{u['lo_hz']}, {u['hi_hz']}]" + (f" ({u['reason']}; fallback: {u['fallback']})" if u.get('fallback') else ''))
        return
    if a.cmd == 'auto':
        os.makedirs(a.out, exist_ok=True); cj = a.cells
        if not cj:
            subprocess.run(['bash', os.path.join(AF, 'live_chain.sh'), 'discover', a.out] + (a.capdirs or []), check=True); cj = os.path.join(a.out, 'cells.json')
        plan = make_plan(json.load(open(cj)), a.cores, **mp_args(a)); plan['cells_json'] = os.path.abspath(cj); pp = os.path.join(a.out, 'plan.json')
        if plan['radio'].get('chan_warn'): print('WARNING: ' + plan['radio']['chan_warn'])
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
