#!/usr/bin/env python3
"""an_sp.py <run dir> <T|S|A>: single-PCI OTA analysis (ISAC_METRICS last line + sense_records_last.jsonl). Prints text; also <run>/an.json."""
import sys, os, re, json, math, collections, subprocess
R, M = sys.argv[1], sys.argv[2]
log = re.sub(r'\x1b\[[0-9;]*m', '', open(R + '/run.log', errors='replace').read())
met = None
for ln in log.splitlines():
    i = ln.find('ISAC_METRICS {')
    if i >= 0:
        try: met = json.loads(ln[i + 13:])
        except ValueError: pass
met = met or {}
def pct(v, p):
    s = sorted(v)
    if not s: return None
    k = (len(s) - 1) * p; f = int(math.floor(k)); c = min(f + 1, len(s) - 1)
    return round(s[f] + (s[c] - s[f]) * (k - f), 2)
def d3(v): return "n=%d p10/50/90=%s/%s/%s" % (len(v), pct(v, .1), pct(v, .5), pct(v, .9))
out = {}
P = lambda *a: print(*a)
pcis = sorted(set(re.findall(r'Initial sync successful, PCI: (\d+)', log)))
P("RUN", R, "mode", M, "PCI synced", pcis, "exit", open(R + '/rx_exit.txt').read().strip() if os.path.exists(R + '/rx_exit.txt') else '?',
  "cpu_cores", open(R + '/cpu_cores.txt').read().strip() if os.path.exists(R + '/cpu_cores.txt') else '?')
P("ACQ", met.get('acq_state'), "pbch_locks", met.get('acq_pbch_locks'), "sib1_decodes", met.get('acq_sib1_decodes'), "sync_losses", met.get('acq_sync_losses'),
  "log: SIB1 decoded x%d" % log.count('SIB1 decoded'), "CSIRS_BLIND CONFIRMED x%d" % log.count('CSIRS_BLIND CONFIRMED'),
  "coreset-found-lines", len(re.findall(r'BWP CORESET found|CORESET.*(?:discovered|locked)', log)))
st = [m for m in re.findall(r'ACQ_STATE (\w+ -> \w+)', log)]
P("ACQ transitions:", st[:8])
dur = None
# ---- records
dci = []; dec = []; rs = collections.defaultdict(list); n_other = collections.Counter()
f = R + '/sense_records_last.jsonl'
tmin = tmax = None
for ln in open(f, errors='replace'):
    try: d = json.loads(ln)
    except ValueError: continue
    t = d.get('type')
    if t == 'dci': dci.append(d)
    elif t == 'decode': dec.append(d)
    elif t == 'rs':
        rs[d.get('source')].append(d)
        tn = d.get('t_mono_ns')
        if tn: tmin = tn if tmin is None else min(tmin, tn); tmax = tn if tmax is None else max(tmax, tn)
    else: n_other[t] += 1
if tmin: dur = (tmax - tmin) / 1e9
P("record types other:", dict(n_other), "rs span_s", round(dur, 1) if dur else None)
FULL = ("harq_pid", "ndi", "rv", "mcs", "k1", "dai", "tpc", "pri")
if M in ('T', 'A'):
    FMT = {('DL', 0): '1_1', ('DL', 1): '1_0', ('UL', 2): '0_1', ('UL', 3): '0_0'}
    CLS = {0: 'C', 1: 'TC', 2: 'SI', 3: 'RA', 4: 'P', None: 'UL/none'}
    fc = collections.Counter(); cc = collections.Counter(); rn = collections.defaultdict(collections.Counter); sol = set(); fullc = collections.Counter(); dl11 = 0
    key2prb = {}
    for d in dci:
        fm = FMT.get((d['dir'], d.get('dci_format')), '?%s' % d.get('dci_format')); fc[fm] += 1
        cc[(d['dir'], CLS.get(d.get('rnti_class'), d.get('rnti_class')))] += 1
        k = (d['dir'], d['rnti']); rn[k][fm] += 1
        if d.get('layout_state') == 'solved': sol.add(k)
        if fm == '1_1':
            dl11 += 1
            if all(d.get(x) is not None for x in FULL): fullc['1_1'] += 1
        key2prb[(d['dir'], d['rnti'], d['abs_slot'])] = d.get('nb_rb') or 0
    P("DCI records", len(dci), "by format", dict(fc)); P("DCI by (dir,rnti class)", {"%s/%s" % k: v for k, v in cc.items()})
    r2 = [k for k, c in rn.items() if sum(c.values()) >= 2]
    P("distinct RNTI (dir,rnti)", len(rn), ">=2 DCIs", len(r2), "by dir", dict(collections.Counter(k[0] for k in r2)), "SOLVED RNTIs", len(sol),
      "of which >=2 DCIs", sum(1 for k in sol if k in r2))
    P("DL 1_1 records with all HARQ/K1/DAI/TPC/PRI/NDI/RV/MCS non-null:", fullc['1_1'], "/", dl11, "= %.1f%%" % (100. * fullc['1_1'] / max(dl11, 1)))
    ncrc = collections.Counter(); wide = collections.Counter()
    for d in dec:
        if d.get('probe'): ncrc['probe'] += 1; continue
        k = d['dir']; ncrc[k] += 1
        if d.get('crc') == 1: ncrc[k + '_ok'] += 1
        prb = key2prb.get((d['dir'], d['rnti'], d.get('dci_abs_slot')), None)
        if prb is not None and prb >= 128:
            wide[k] += 1
            if d.get('crc') == 1: wide[k + '_ok'] += 1
        elif prb is None: wide['unjoined'] += 1
    P("decode records (non-probe) by dir:", dict(ncrc), " >=128PRB:", dict(wide))
g = lambda k: met.get(k)
P("DCI counters: accepted 00/01/10/11", g('dci_accepted_00'), g('dci_accepted_01'), g('dci_accepted_10'), g('dci_accepted_11'), "dup", g('dci_dup'),
  "rec generated/persisted/lost", g('dci_rec_generated'), g('dci_rec_persisted'), g('dci_rec_lost'), "state cand/fixed/solved/unsolved", g('dci_state_cand'), g('dci_state_fixed'), g('dci_state_solved'), g('dci_state_unsolved'))
P("PDSCH counters: queued", g('pdschq_queued'), "decoded", g('pdschq_decoded'), "crc_ok", g('pdschq_crc_ok'), "reasons", g('pdschq_reasons'),
  "| PUSCH try", g('pusch_try'), "crc_ok", g('pusch_crc_ok'), "| probe_decodes", g('probe_decodes'), "dl/ul", g('probe_dl'), g('probe_ul'))
q = max(g('scanq_queued') or 1, 1); pq = max(g('pdschq_queued') or 1, 1)
P("DROPS scanq full/stale/epoch %s/%s/%s of %s (%.2f%%)" % (g('scanq_drop_full'), g('scanq_drop_stale'), g('scanq_drop_epoch'), g('scanq_queued'),
  100. * ((g('scanq_drop_full') or 0) + (g('scanq_drop_stale') or 0) + (g('scanq_drop_epoch') or 0)) / q),
  "| pdschq full/stale/epoch/narrow %s/%s/%s/%s of %s (%.2f%%)" % (g('pdschq_drop_full'), g('pdschq_drop_stale'), g('pdschq_drop_epoch'), g('pdschq_drop_narrow'), g('pdschq_queued'),
  100. * ((g('pdschq_drop_full') or 0) + (g('pdschq_drop_stale') or 0) + (g('pdschq_drop_epoch') or 0)) / pq),
  "| puschq_epoch", g('puschq_drop_epoch'), "| recov dropped full/old expired/stale", g('recov_dropped_full'), g('recov_dropped_old'), g('recov_expired'), g('recov_stale'),
  "submitted/rescued", g('recov_submitted'), g('recov_rescued'), "| sense_rec_drop", g('sense_rec_drop'), "sense_cfr_drop", g('sense_cfr_drop'), "inline_epoch", g('pdcch_inline_drop_epoch'))
P("NOT-DECODED/NOT-SAVED counters: traffic_skip", g('traffic_skip'), "ul_decode_skip", g('ul_decode_skip'), "probe_skip", g('probe_skip'), "e1_skip_classes", g('e1_skip_classes'), "e3_skip", g('e3_skip'),
  "e3_trunc", g('e3_trunc'), "pdschq_stale_after_decode", g('pdschq_stale_after_decode'), "td_cb0_budget_skips", g('td_cb0_budget_skips'), "sense_unexported_pdsch_data", g('sense_unexported_pdsch_data'),
  "sense_unexported_pusch_data", g('sense_unexported_pusch_data'), "sense_io_errors", g('sense_io_errors'), "sense_recon_skip", g('sense_recon_skip'))

if M in ('S', 'A'):
    P("SENSE sources: records/s [span %.1fs] | valid_cfr_prbs p10/50/90,max | wideband(>=128)/s | snr_db_p50 p10/50/90 | hist(snr_db_p50 5dB bins from -10) | counters exported/prod/cfr_drop/async_drop/async_queued/pack" % (dur or 0))
    SM = {'ssb': 'ssb', 'csirs': 'csirs', 'pdsch_dmrs': 'pdsch_dmrs', 'pdsch_data': 'pdsch_data', 'pusch_dmrs': 'pusch_dmrs', 'pusch_data': 'pusch_data', 'prach': 'prach', 'srs': 'srs'}
    allsrc = sorted(set(rs) | set(SM))
    for s in allsrc:
        v = rs.get(s, []); n = len(v); k = SM.get(s, s)
        prb = [(x.get('valid_cfr_prbs') if 'valid_cfr_prbs' in x else x.get('nb_rb')) or 0 for x in v]; wb = sum(1 for x in prb if x >= 128)
        snr = [(x['snr_db_p50'] if 'snr_db_p50' in x else x.get('snr_db')) for x in v if (x.get('snr_db_p50') if 'snr_db_p50' in x else x.get('snr_db')) is not None]
        h = collections.Counter(int(math.floor(x / 5.0)) * 5 for x in snr)
        dr = lambda p: met.get('sense_%s_%s' % (p, k)); ex = dr('cfr_exported'); cd = dr('cfr_drop'); ad = dr('async_drop'); aq = dr('async_queued')
        dp = ((cd or 0) + (ad or 0)); dpct = 100. * dp / max(dp + (ex or 0), 1)
        P("  %-10s n=%-7d %8.2f/s | prbs %s max=%s | wb %d (%.3f/s) | snr %s | hist %s | exp=%s prod=%s cfr_drop=%s async_drop=%s/%s drop%%=%.2f pack=%s" %
          (s, n, n / dur if dur else 0, d3(prb), max(prb) if prb else None, wb, wb / dur if dur else 0, d3(snr), dict(sorted(h.items())), ex, dr('prod_count'), cd, ad, aq, dpct, dr('pack_count')))
        out[s] = dict(n=n, drop_pct=dpct, rate=n / dur if dur else 0, prb=[pct(prb, .1), pct(prb, .5), pct(prb, .9), max(prb) if prb else None], wb_rate=wb / dur if dur else 0, snr=[pct(snr, .1), pct(snr, .5), pct(snr, .9)], exp=ex, cfr_drop=cd, async_drop=ad, async_q=aq)
    P("  record kinds other: grant records", n_other.get('grant'), "dci", len(dci), "decode", len(dec))
    P("SRS: gate_hist", met.get('srs_gate_hist'), "gate_det", met.get('srs_gate_det'), "snr_hist_confirmed", met.get('srs_snr_hist_confirmed'), "unconfirmed", met.get('srs_snr_hist_unconfirmed'),
      "jobs_dropped", met.get('srs_jobs_dropped'), "pregate_skip", met.get('srs_pregate_skip'), "budget_skip", met.get('srs_budget_skip'), "backoff_skip", met.get('srs_backoff_skip'),
      "audits", met.get('srs_audits'), "idle_resets", met.get('srs_idle_resets'), "gate_open pusch/unexpl", met.get('srs_gate_open_pusch'), met.get('srs_gate_open_unexplained'), "srs_us", met.get('srs_us'))
    P("UL: pusch_try", met.get('pusch_try'), "pusch_crc_ok", met.get('pusch_crc_ok'), "ul_decode_skip", met.get('ul_decode_skip'), "sense_ul_delay_skip", met.get('sense_ul_delay_skip'),
      "PRACH detections/occasions/drops/skipped", met.get('prach_detections'), met.get('prach_occasions'), met.get('prach_drops'), met.get('prach_skipped'))
    P("SCHED: units_enabled", met.get('sense_units_enabled'), "enables/disables/deferrals/decisions", met.get('sched_enables'), met.get('sched_disables'), met.get('sched_deferrals'), met.get('sched_decisions'),
      "binding", met.get('sched_binding'), "disabled_skips", met.get('sense_disabled_skips'), "violations", met.get('sense_violations'), "floor_prb", met.get('sense_floor_prb'),
      "gap_p95_ms", met.get('sense_cont_gap_p95_ms'), "gap_max_ms", met.get('sense_gap_max_ms'), "wb_valid", met.get('sense_wb_valid'), "wb_valid_prbs", met.get('sense_wb_valid_prbs'),
      "rec_count/drop", met.get('sense_rec_count'), met.get('sense_rec_drop'), "gpu_budget_used_pct", met.get('gpu_budget_used_pct'))
    # T_cell / cont_floor trace
    tr = [(m.group(1), m.group(2)) for m in re.finditer(r'SENSE_FLOOR cell=\d+ pci=\d+ t_cell=(\d+) gap_p95_ms=([\d.]+)', log)]
    P("T_cell trace (t_cell:gap_p95_ms) n=%d:" % len(tr), ' '.join('%s:%s' % x for x in tr[::max(1, len(tr) // 12)]))
    cf = collections.Counter()
    for s, v in rs.items():
        for x in v: cf[x.get('cont_floor')] += 1
    P("cont_floor values over rs records:", dict(sorted(cf.items(), key=lambda a: str(a[0]))))
    sc = subprocess.run([sys.executable, '/home/nicola/NICOLA/wt/rr-twomodes/tools/sense/score_sensing.py', f, '--log', R + '/run.log', '--json', R + '/score.json'], capture_output=True, text=True)
    for ln in sc.stdout.splitlines():
        if ln.startswith(('min_prb', 'pci ', 'AGGREGATE', 'STABILITY')): P("score_sensing:", ln[:400])
    P("  (score_sensing 'avail' = fraction of 25ms/100ms/1s windows of the run containing >=1 wideband(>=128 PRB) record)")
    P("sense_prod/counters: prach prod_count", met.get('sense_prod_count_prach'), "exported", met.get('sense_cfr_exported_prach'), "| pusch_data prod", met.get('sense_prod_count_pusch_data'), "exp", met.get('sense_cfr_exported_pusch_data'))

if M in ('S', 'A'):
    # PDSCH units / scheduler (handles BASE format binding=X admitted= and FIX format binding= top= elig= cold_deferred= admitted=)
    dec = re.findall(r'SENSE_SCHED decision=(\d+) cells=(\d+) enables=(\d+) disables=(\d+) bad_cap=\d+ binding=(\S+)(?: top=(\S+))?(?: elig=(\d+))?(?: cold_deferred=(\d+))? admitted=(\d+) \[([^\]]*)\]', log)
    nd = len(dec); bnd = collections.Counter(d[4] for d in dec); top = collections.Counter(d[5] for d in dec if d[5])
    elig = [int(d[6]) for d in dec if d[6] != '']; cd = [int(d[7]) for d in dec if d[7] != '']; cd2 = cd
    adm = collections.Counter(); admd = collections.Counter()
    for d in dec:
        seen = set()
        for it in d[9].split(','):
            m = re.match(r'c\d+/(\d+)/(\w+)=', it)
            if m: adm[m.group(2)] += 1; seen.add(m.group(2))
        for x in seen: admd[x] += 1
    P("SCHED_LOG decisions=%d enables_sum=%d disables_sum=%d binding=%s top=%s elig(mean/max)=%s cold_deferred(mean/max)=%s" % (nd, sum(int(d[2]) for d in dec), sum(int(d[3]) for d in dec), dict(bnd), dict(top),
      ("%.1f/%d" % (sum(elig) / len(elig), max(elig))) if elig else 'n/a(base format)', ("%.1f/%d" % (sum(cd) / len(cd), max(cd))) if cd else 'n/a(base format)'))
    P("SCHED_LOG decisions with source admitted (of %d):" % nd, dict(admd))
    uni = collections.defaultdict(lambda: [0, 0, 0.0]); nu = 0; ulines = 0
    for m in re.finditer(r'SENSE_UNITS cell=\d+ pci=\d+ .*? n=(\d+) (.*)', log):
        ulines += 1; nu = max(nu, int(m.group(1)))
        for u in m.group(2).split(' | '):
            q = re.match(r'(\d+)/(\w+):(ON|off) wb=([\d.]+)', u)
            if not q: continue
            k = '%s/%s' % (q.group(1), q.group(2)); uni[k][0] += 1; uni[k][1] += q.group(3) == 'ON'; uni[k][2] = max(uni[k][2], float(q.group(4)))
    P("SENSE_UNITS lines=%d, max units known n=%d (only first 8 printed per line)" % (ulines, nu))
    psrc = collections.defaultdict(lambda: [0, 0, 0])  # lines present, ON lines, distinct units
    for k, (a, b, w) in uni.items():
        sn = k.split('/')[1]; psrc[sn][0] += a; psrc[sn][1] += b; psrc[sn][2] += 1
    P("UNITS ON-fraction by source (unit-rows ON/rows, distinct printed units):", {k: "%d/%d=%.0f%% units=%d" % (v[1], v[0], 100. * v[1] / max(v[0], 1), v[2]) for k, v in psrc.items()})
    off = [(k, a, b) for k, (a, b, w) in uni.items() if k.split('/')[1].startswith('pdsch') and a >= 3 and b < a / 2.]
    P("PDSCH units ON in <50%% of lines (n=%d of %d pdsch units printed): %s" % (len(off), sum(1 for k in uni if k.split('/')[1].startswith('pdsch')), ['%s %d/%d' % o for o in off[:12]]))
    t = [(m.group(1), m.group(2)) for m in re.finditer(r'SENSE_SCHED decision=\d+ .*? enabled_now=(\d+) violations=\d+ disabled_skips=(\d+)', log)]
    P("enabled_now over decisions:", ' '.join(x[0] for x in t[::max(1, len(t) // 20)]), "| final disabled_skips", t[-1][1] if t else None)

if M in ('S', 'A'):
    # --- grant-level wideband path (wbg) metrics
    wk = ('sense_wbg_offered', 'sense_wbg_admitted', 'sense_wbg_taps', 'sense_wbg_refunded', 'offered_wbg_overcap')
    P("WBG counters:", {k: met.get(k) for k in wk})
    pdw = [x for x in rs.get('pdsch_dmrs', []) if (x.get('valid_cfr_prbs') or 0) >= 128]
    allw = [x for v in rs.values() for x in v if (x.get('valid_cfr_prbs') or 0) >= 128]
    sn = lambda x: x['snr_db_p50'] if 'snr_db_p50' in x else x.get('snr_db')
    bad = [x for x in rs.get('pdsch_dmrs', []) if (x.get('nb_rb') or 0) >= 128 and ((x.get('valid_cfr_prbs') or 0) == 0 or sn(x) is None)]
    P("WBG records: pdsch_dmrs wide n=%d (%.3f/s) snr %s | all-source wide n=%d | pdsch_dmrs with nb_rb>=128 and (valid_cfr_prbs=0 or null snr): %d" % (
        len(pdw), len(pdw) / dur if dur else 0, d3([sn(x) for x in pdw if sn(x) is not None]), len(allw), len(bad)))
    P("WBG record fields sample:", {k: v for k, v in (pdw[0].items() if pdw else []) if k not in ('cfr', 'h', 'cfr_re', 'cfr_im')} if pdw else None)
    ft = [int(m.group(1)) for m in re.finditer(r'SENSE_FLOOR cell=\d+ pci=\d+ t_cell=(\d+)', log)]
    P("SENSE_FLOOR t_cell distribution (n=%d):" % len(ft), {k: "%.1f%%" % (100. * ft.count(k) / max(len(ft), 1)) for k in sorted(set(ft), reverse=True)})
    gr = sum(1 for ln in open(f, errors='replace') if '"type":"grant"' in ln and '"dir":"DL"' in ln)
    out['wbg'] = dict(counters={k: met.get(k) for k in wk}, wide_pdsch_dmrs=len(pdw), snr=[pct([sn(x) for x in pdw if sn(x) is not None], p) for p in (.1, .5, .9)], bad=len(bad), floor=dict(collections.Counter(ft)), dur=dur)
out['sched'] = dict(decisions=nd, binding=dict(bnd), top=dict(top), elig=elig and [sum(elig)/len(elig), max(elig)], cold=cd2 and [sum(cd2)/len(cd2), max(cd2)], adm=dict(admd), enabled_series=[int(x[0]) for x in t]) if M in ('S','A') else None
json.dump(out, open(R + '/an.json', 'w'))
