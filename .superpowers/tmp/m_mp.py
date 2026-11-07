#!/usr/bin/env python3
"""m_mp.py <run dir>: per-cell metrics of one multi-PCI (or solo) OTA arm -> <run>/mp.json. Cells: log lines tagged [cK] (untagged = cell 0, the solo case).
Rates are per second of the ACTIVE window of the cell (first PBCH lock seen in ISAC_METRICS .. last ISAC_METRICS of the cell); sync times are relative to the first session record.
EVMGRANT probe=0 gives decode/CRC/latency (dec_us); records come from sense_records_last[.cellK].jsonl."""
import sys, re, json, os, glob, collections, statistics
R = sys.argv[1]
log = re.sub(r'\x1b\[[0-9;]*m', '', open(R + '/run.log', errors='replace').read())
lines = log.splitlines()
def pct(v, p):
    s = sorted(v)
    if not s: return None
    k = (len(s) - 1) * p; f = int(k); c = min(f + 1, len(s) - 1); return s[f] + (s[c] - s[f]) * (k - f)
cell_of = lambda l: int(l[2:l.index(']')]) if re.match(r'\[c\d+\] ', l) else 0
strip = lambda l: re.sub(r'^\[c\d+\] ', '', l)
cells = collections.defaultdict(lambda: dict(met=[], ev=[], sched=[]))
glob_l = dict(span=None, admit=None, feed=[], shmiq_end=[], pools=[])
for l in lines:
    c = cell_of(l); s = strip(l)
    if 'ISAC_METRICS {' in s:
        try: cells[c]['met'].append(json.loads(s[s.index('ISAC_METRICS {') + 13:]))
        except ValueError: pass
    elif 'EVMGRANT ' in s: cells[c]['ev'].append(s)
    elif 'SENSE_SCHED' in s: cells[c]['sched'].append(s)
    if 'MPCI_SPAN ok=' in s and not glob_l['span']: glob_l['span'] = s.strip()
    if 'MPCI_ADMIT cells=' in s and not glob_l['admit']: glob_l['admit'] = s.strip()
    if 'MPCI_FEED rate=' in s: glob_l['feed'].append(s.strip())
    if s.strip().startswith('SHMIQ_END'): glob_l['shmiq_end'].append(s.strip())
# record files
recf = {}
for f in glob.glob(R + '/sense_records_last*.jsonl'):
    m = re.search(r'\.cell(\d+)\.jsonl$', f); recf[int(m.group(1)) if m else 0] = f
t0 = None; recs = {}
for c, f in recf.items():
    rs = collections.defaultdict(list); sess = []
    for ln in open(f, errors='replace'):
        if '"type":"rs"' not in ln and '"type":"session"' not in ln: continue
        try: d = json.loads(ln)
        except ValueError: continue
        if d['type'] == 'session': sess.append(d['t_mono_ns']); continue
        rs[d['source']].append(d)
    recs[c] = rs
    if sess: t0 = min(t0, min(sess)) if t0 else min(sess)
allm = [m['t_mono_ns'] for c in cells.values() for m in c['met']]
if t0 is None and allm: t0 = min(allm)
out = dict(run=os.path.basename(R.rstrip('/')), cells={}, glob=dict(span=glob_l['span'], admit=glob_l['admit']))
vp = lambda x: x.get('valid_cfr_prbs') or 0
rx = re.compile(r'EVMGRANT rnti=(0x[0-9a-f]+) (\d+)\.(\d+) crc=(\d) .*? rb=(\d+)\+(\d+) .*? probe=(\d) .*? dec_us=(\d+) .*? cb_ok=(\d+) cb_tried=(\d+) cb_total=(\d+)')
for c in sorted(set(cells) | set(recs)):
    d = cells[c]; o = {}
    met = d['met']; last = met[-1] if met else {}
    lock = next((m for m in met if (m.get('acq_pbch_locks') or 0) >= 1), None)
    sib = next((m for m in met if (m.get('acq_sib1_decodes') or 0) >= 1), None)
    o['t_pbch_s'] = (lock['t_mono_ns'] - t0) / 1e9 if lock and t0 else None
    o['t_sib1_s'] = (sib['t_mono_ns'] - t0) / 1e9 if sib and t0 else None
    o['pci'] = last.get('pci'); o['acq_state'] = last.get('acq_state'); o['sync_losses'] = last.get('acq_sync_losses'); o['pbch_locks'] = last.get('acq_pbch_locks'); o['sib1'] = last.get('acq_sib1_decodes')
    ta = lock['t_mono_ns'] if lock else (met[0]['t_mono_ns'] if met else None)
    dur = (last['t_mono_ns'] - ta) / 1e9 if met and ta else None
    if not dur or dur < 5:  # no lock / too short: fall back to the record span
        tm = [x['t_mono_ns'] for v in recs.get(c, {}).values() for x in v]
        dur = (max(tm) - min(tm)) / 1e9 if len(tm) > 1 else None
    o['dur_act'] = dur
    D = lambda k: last.get(k) or 0
    # DCI
    if dur:
        o['dci_s'] = dict(accepts=D('pdcch_accepts') / dur, d10=D('dci_accepted_10') / dur, d11=D('dci_accepted_11') / dur, d00=D('dci_accepted_00') / dur, d01=D('dci_accepted_01') / dur)
    # EVMGRANT
    ev = {}; rn = collections.Counter()
    for s in d['ev']:
        m = rx.search(s)
        if not m: continue
        rnti = int(m.group(1), 16); fr, sl, crc, nb, pr, dus, cbo, cbt, cbn = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(7)), int(m.group(8)), int(m.group(9)), int(m.group(10)), int(m.group(11))
        pr = int(m.group(7)); nb = int(m.group(5))
        if pr: continue
        k = (rnti, fr * 20 + sl); e = dict(crc=crc, nb=nb, dus=dus, cbo=cbo, cbn=cbn, rnti=rnti)
        old = ev.get(k)
        if old is None or (e['crc'], e['cbo']) > (old['crc'], old['cbo']): ev[k] = e
    for e in ev.values():
        if e['rnti'] not in (0xffff,): rn[e['rnti']] += 1
    o['rntis_n'] = len(rn); o['rntis_top'] = [(hex(r), n) for r, n in rn.most_common(6)]
    dec = lambda T: [e for e in ev.values() if e['nb'] >= T]
    prov = lambda e: e['crc'] == 1 or (e['crc'] == 0 and e['cbn'] > 1 and e['cbo'] >= 1)
    if dur:
        o['grants_s'] = len(ev) / dur
        for T in (0, 51, 128):
            v = dec(T); o['dec%d_s' % T] = len(v) / dur; o['crc%d_s' % T] = sum(1 for e in v if e['crc'] == 1) / dur; o['proven%d_s' % T] = sum(1 for e in v if prov(e)) / dur
    o['crc_pct'] = 100. * sum(1 for e in ev.values() if e['crc'] == 1) / max(len(ev), 1)
    for T in (0, 128):
        v = [e['dus'] for e in dec(T)]
        o['lat_us_%d' % T] = [pct(v, .5), pct(v, .99), len(v)]
    # drops
    o['drops'] = {k: last.get(k) for k in ('scanq_queued', 'scanq_processed', 'scanq_drop_full', 'scanq_drop_stale', 'scanq_drop_epoch', 'pdschq_queued', 'pdschq_decoded', 'pdschq_crc_ok', 'pdschq_drop_full', 'pdschq_drop_stale', 'pdschq_drop_epoch',
                                  'pdschq_drop_narrow', 'sense_rec_drop', 'sense_cfr_drop', 'sense_async_queued_pdsch_dmrs', 'sense_async_drop_pdsch_dmrs', 'sense_async_queued_pdsch_data', 'sense_async_drop_pdsch_data', 'sense_cfr_exported_pdsch_dmrs', 'sense_cfr_exported_pdsch_data',
                                  'sense_cfr_exported_ssb', 'sense_cfr_exported_csirs', 'sense_cfr_exported_srs', 'sense_cfr_exported_prach', 'sense_rec_count', 'sense_unexported_pdsch_data', 'obs_dropped', 'sense_io_errors', 'csirs_disc_feed_drops', 'recov_dropped_full', 'discovery_overruns', 'proc_utime_us', 'proc_stime_us')}
    # records
    rs = recs.get(c, {}); rd = {}
    tl = ta
    for src, v in rs.items():
        if tl: v = [x for x in v if x['t_mono_ns'] >= tl]
        if not dur: continue
        rd[src] = dict(n_s=len(v) / dur, v51_s=sum(1 for x in v if vp(x) >= 51) / dur, v128_s=sum(1 for x in v if vp(x) >= 128) / dur, noqual=sum(1 for x in v if x.get('valid_cfr_prbs') is None), n=len(v))
    o['rec'] = rd
    o['sched_n'] = len(d['sched']); o['sched_last'] = d['sched'][-1][:900] if d['sched'] else None
    tm = [x['t_mono_ns'] for v in rs.values() for x in v]
    o['t_first_rec_s'] = (min(tm) - t0) / 1e9 if tm and t0 else None
    out['cells'][c] = o
g = out['glob']
g['exit'] = open(R + '/rx_exit.txt').read().strip() if os.path.exists(R + '/rx_exit.txt') else None
g['pcis_sync'] = sorted(set(re.findall(r'Initial sync successful, PCI: (\d+)', log)))
g['mpci_sync'] = re.findall(r'MPCI_SYNC cell=\d+ want=\d+ got=\d+ \w+', log)
cnt = lambda p: len(re.findall(p, log))
g['sched_fail'] = cnt(r'SENSE_SCHED thread not started'); g['pthread_fail'] = cnt(r'pthread_create'); g['sched_started'] = cnt(r'SENSE_SCHED started')
g['rfstall'] = cnt(r'RFSTALL'); g['reinit'] = cnt(r'recovered by full device re-init'); g['rxdiscont'] = cnt(r'RXDISCONT abs_slot'); g['uhd_ovf'] = cnt(r'ERROR_CODE_OVERFLOW|reason=[^ ]*overrun|\bOOO\b')
g['asserts'] = cnt(r'Assertion|AssertFatal|SEGV|Segmentation|core dumped|\bERROR\b.*thread'); g['assert_lines'] = [l.strip()[:240] for l in lines if re.search(r'Assertion|AssertFatal|SEGV|Segmentation|core dumped', l)][:6]
g['errors_top'] = collections.Counter(re.sub(r'\d+', 'N', strip(l))[:110] for l in lines if re.search(r'\[(PHY|HW|NR_PHY|UTIL)\]\s+(ERROR|\S*ERROR)', l) or ' ERROR ' in l).most_common(8)
g['feed_last'] = glob_l['feed'][-1] if glob_l['feed'] else None
fe = glob_l['feed'][-1] if glob_l['feed'] else ''
g['feed'] = {k: v for k, v in re.findall(r'(chan_overruns|chan_discont|short|gaps|frozen|read_err|chan_work_max_ms|chan_gap_max_ms)=([\d.]+)', fe)}
g['feed_cells'] = (re.search(r'cells=(\S+)$', fe) or [None, None])[1] if fe else None
g['shmiq_end'] = glob_l['shmiq_end']
g['span_unserved'] = re.findall(r'MPCI_SPAN cell=\d+ id=\S+ unserved reason=\S+', log)
g['cpu_cores'] = open(R + '/cpu_cores.txt').read().strip() if os.path.exists(R + '/cpu_cores.txt') else None
if os.path.exists(R + '/gpu.tsv'):
    u = [int(x.split(',')[0].replace('%', '').strip()) for x in open(R + '/gpu.tsv') if x.strip() and x.split(',')[0].replace('%', '').strip().isdigit()]
    g['gpu_util_mean'] = sum(u) / len(u) if u else None; g['gpu_util_p95'] = pct(u, .95) if u else None
json.dump(out, open(R + '/mp.json', 'w'), indent=0)
print(out['run'], 'exit', g['exit'], 'cells', {c: (o.get('pci'), round(o['t_pbch_s'], 1) if o.get('t_pbch_s') is not None else None, round(o.get('dec0_s') or 0, 1), round(o.get('dec128_s') or 0, 2)) for c, o in out['cells'].items()})
