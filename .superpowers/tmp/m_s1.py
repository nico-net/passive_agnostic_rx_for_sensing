#!/usr/bin/env python3
"""m_s1.py <run dir>: single1 A/B metrics -> <run>/m.json (+ one-line text). Items 1-4,5,7 of the brief.
Wide = nb_rb>=128. EVMGRANT (run.log, probe=0) gives decode/CRC/CB; grant/rs jsonl records give records. Matching EVMGRANT<->rs by (rnti, abs_slot mod 20480)
(EVMGRANT prints frame.slot only; 20480 slots = 10.24 s wrap -> rare false match possible)."""
import sys, re, json, math, collections, bisect
R = sys.argv[1]
log = re.sub(r'\x1b\[[0-9;]*m', '', open(R + '/run.log', errors='replace').read())
met = {}
for ln in log.splitlines():
    i = ln.find('ISAC_METRICS {')
    if i >= 0:
        try: met = json.loads(ln[i + 13:])
        except ValueError: pass
def pct(v, p):
    s = sorted(v)
    if not s: return None
    k = (len(s) - 1) * p; f = int(k); c = min(f + 1, len(s) - 1); return s[f] + (s[c] - s[f]) * (k - f)
rs = collections.defaultdict(list); tm = []
for ln in open(R + '/sense_records_last.jsonl', errors='replace'):
    if '"type":"rs"' not in ln: continue
    try: d = json.loads(ln)
    except ValueError: continue
    rs[d['source']].append(d); tm.append(d['t_mono_ns'])
t0, t1 = min(tm), max(tm); dur = (t1 - t0) / 1e9
vp = lambda x: x.get('valid_cfr_prbs') or 0
# EVMGRANT
ev = {}
for m in re.finditer(r'EVMGRANT rnti=(0x[0-9a-f]+) (\d+)\.(\d+) crc=(\d) .*? rb=(\d+)\+(\d+) .*? probe=(\d) .*? dec_us=(\d+) .*? cb_ok=(\d+) cb_tried=(\d+) cb_total=(\d+)', log):
    rnti = int(m.group(1), 16); fr, sl, crc, st, nb, pr, dus, cbo, cbt, cbn = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8)), int(m.group(9)), int(m.group(10)), int(m.group(11))
    if pr: continue
    k = (rnti, fr * 20 + sl)
    e = dict(crc=crc, nb=nb, dus=dus, cbo=cbo, cbn=cbn)
    o = ev.get(k)
    if o is None or (e['crc'], e['cbo']) > (o['crc'], o['cbo']): ev[k] = e
W = {k: e for k, e in ev.items() if e['nb'] >= 128}
okw = [k for k, e in W.items() if e['crc'] == 1]
part = [k for k, e in W.items() if e['crc'] == 0 and e['cbn'] > 1 and e['cbo'] >= 1]
proven = okw + part
out = dict(run=R.split('/')[-1], dur=dur)
out['wide_dec_s'] = len(W) / dur; out['wide_crc_s'] = len(okw) / dur; out['wide_part_s'] = len(part) / dur; out['wide_proven_s'] = len(proven) / dur
out['n'] = dict(wide=len(W), crc=len(okw), part=len(part))
# rs
pd, pdat = rs['pdsch_dmrs'], rs['pdsch_data']
out['dmrs_s'] = len(pd) / dur; out['dmrs_nb128_s'] = sum(1 for x in pd if (x.get('nb_rb') or 0) >= 128) / dur; out['dmrs_v128_s'] = sum(1 for x in pd if vp(x) >= 128) / dur
out['cfr_est'] = dict(collections.Counter(x.get('cfr_est', 'none') for x in pd))
out['cfr_est_v128'] = dict(collections.Counter(x.get('cfr_est', 'none') for x in pd if vp(x) >= 128))
out['dmrs_partial'] = sum(1 for x in pd if x.get('partial_tb')); out['dmrs_partial_v128'] = sum(1 for x in pd if x.get('partial_tb') and vp(x) >= 128)
out['data_s'] = len(pdat) / dur; out['data_whole_s'] = sum(1 for x in pdat if not x.get('partial_tb')) / dur; out['data_part_s'] = sum(1 for x in pdat if x.get('partial_tb')) / dur
out['data_v128_s'] = sum(1 for x in pdat if vp(x) >= 128) / dur
mod = lambda s: s % 20480
RSK = collections.defaultdict(list)
for x in pd + pdat: RSK[(x['rnti'], mod(x['abs_slot']))].append(x)
def cls(k):
    v = RSK.get(k, [])
    return 'none' if not v else ('v128' if any(vp(x) >= 128 for x in v) else 'lt128')
for nm, ks in (('proven', proven), ('crc', okw)):
    c = collections.Counter(cls(k) for k in ks); n = max(len(ks), 1)
    out['match_' + nm] = dict(n=len(ks), any=(c['v128'] + c['lt128']) / n, v128=c['v128'] / n, c=dict(c))
# funnel: proven wide w/o v128 record: no record / lt128 record (no drop info per grant; drops are counters)
out['funnel'] = dict(collections.Counter(cls(k) for k in proven))
# availability windows
def avail(recs, w):
    ts = sorted(x['t_mono_ns'] for x in recs if vp(x) >= 128); nw = int((t1 - t0) / (w * 1e9)); 
    if nw <= 0: return None
    hit = {int((t - t0) / (w * 1e9)) for t in ts}; return len([h for h in hit if h < nw]) / nw
P = pd + pdat; A = [x for v in rs.values() for x in v]
out['avail_pdsch'] = [avail(P, w) for w in (.025, .1, 1)]; out['avail_all'] = [avail(A, w) for w in (.025, .1, 1)]
out['avail_dmrs_only'] = [avail(pd, w) for w in (.025, .1, 1)]
# latency
dl = [e['dus'] for e in ev.values()]; dw = [e['dus'] for e in W.values()]
out['lat_us_all'] = [pct(dl, .5), pct(dl, .99), len(dl)]; out['lat_us_wide'] = [pct(dw, .5), pct(dw, .99), len(dw)]
# drops
g = met.get
out['drops'] = {k: g(k) for k in ('scanq_queued', 'scanq_drop_full', 'scanq_drop_stale', 'scanq_drop_epoch', 'pdschq_queued', 'pdschq_drop_full', 'pdschq_drop_stale', 'pdschq_drop_epoch', 'pdschq_drop_narrow', 'sense_rec_drop', 'sense_cfr_drop', 'sense_cfr_drop_pdsch_dmrs', 'sense_cfr_drop_pdsch_data', 'sense_async_drop_pdsch_dmrs', 'sense_async_drop_pdsch_data', 'sense_async_queued_pdsch_dmrs', 'sense_async_queued_pdsch_data', 'sense_cfr_exported_pdsch_dmrs', 'sense_cfr_exported_pdsch_data', 'sense_rec_count')}
out['cpu'] = open(R + '/cpu_cores.txt').read().strip() if __import__('os').path.exists(R + '/cpu_cores.txt') else None
tap = [l for l in log.splitlines() if 'DMRS_TAP_EST' in l]; out['tap'] = tap[-1].split('DMRS_TAP_EST')[1].strip() if tap else None
out['exit'] = open(R + '/rx_exit.txt').read().strip(); out['pcis'] = sorted(set(re.findall(r'Initial sync successful, PCI: (\d+)', log)))
out['sync_losses'] = met.get('acq_sync_losses')
json.dump(out, open(R + '/m.json', 'w'), indent=0)
print(json.dumps({k: (round(v, 3) if isinstance(v, float) else v) for k, v in out.items() if k not in ('tap', 'drops')}))
