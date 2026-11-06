#!/usr/bin/env python3
"""inv_day.py <run dir>: wide (>=128 PRB) DL grants vs wideband rs records (same abs_slot+rnti), and gaps."""
import sys, json, collections
R = sys.argv[1]
G = {}; RS = collections.defaultdict(list); t0 = None
for ln in open(R + '/sense_records_last.jsonl', errors='replace'):
    try: d = json.loads(ln)
    except ValueError: continue
    t = d.get('type')
    if t == 'grant' and d['dir'] == 'DL' and (d.get('nb_rb') or 0) >= 128: G[(d['abs_slot'], d['rnti'])] = d
    elif t == 'rs' and d.get('source') in ('pdsch_dmrs', 'pdsch_data'): RS[(d['abs_slot'], d['rnti'])].append(d)
n = len(G); anyrs = sum(1 for k in G if k in RS); wb = sum(1 for k in G if any((x.get('valid_cfr_prbs') or 0) >= 128 for x in RS.get(k, [])))
sl = sorted(k[0] for k in G); gaps = [(b - a) / 2000. for a, b in zip(sl, sl[1:])]  # 30 kHz SCS: 2000 slots/s
print("%s wide DL grants=%d with any pdsch rs=%d (%.1f%%) with valid>=128 rs=%d (%.1f%%) | wide-grant span %.0fs, grants/s %.2f, max gap between wide grants %.1fs, p95 %.2fs" % (
    R.split('/')[-1], n, anyrs, 100. * anyrs / max(n, 1), wb, 100. * wb / max(n, 1), (sl[-1] - sl[0]) / 2000. if sl else 0, n / ((sl[-1] - sl[0]) / 2000.) if len(sl) > 1 else 0, max(gaps) if gaps else 0, sorted(gaps)[int(.95 * len(gaps))] if gaps else 0))
import re
log = re.sub(r'\x1b\[[0-9;]*m', '', open(R + '/run.log', errors='replace').read())
adm = collections.Counter()
for m in re.finditer(r'SENSE_SCHED decision=\d+ .*? admitted=\d+ \[([^\]]*)\]', log):
    for it in m.group(1).split(','):
        q = re.match(r'c\d+/(\d+)/(pdsch_\w+)=', it)
        if q: adm[int(q.group(1))] += 1
wr = collections.Counter(k[1] for k in G); tot = sum(wr.values())
print("  wide-grant RNTIs=%d top5(share): %s | RNTIs ever admitted as pdsch unit=%d, of which with wide grants=%d; share of wide grants whose RNTI was ever admitted=%.1f%%" % (
    len(wr), [(r, '%.0f%%' % (100. * c / tot)) for r, c in wr.most_common(5)], len(adm), sum(1 for r in adm if r in wr), 100. * sum(c for r, c in wr.items() if r in adm) / max(tot, 1)))
seq = []
for m in re.finditer(r'SENSE_SCHED decision=\d+ .*? admitted=\d+ \[([^\]]*)\]', log):
    seq.append({(int(q.group(1)), q.group(2)) for q in (re.match(r'c\d+/(\d+)/(\w+)=', it) for it in m.group(1).split(',')) if q and q.group(2).startswith('pdsch')})
allu = set().union(*seq) if seq else set()
res = []
for u in allu:
    run = 0
    for s in seq + [set()]:
        if u in s: run += 1
        elif run: res.append(run); run = 0
firstpd = next((i for i, s in enumerate(seq) if s), None)
print("  PDSCH admitted-set per decision (N=%d): decisions with >=1 pdsch unit admitted=%d (%.0f%%), first at decision #%s, distinct pdsch units admitted=%d, mean/max consecutive decisions a unit stays admitted=%.1f/%d, units admitted in exactly 1 decision=%d, mean pdsch units admitted per decision=%.1f" % (
    len(seq), sum(1 for s in seq if s), 100. * sum(1 for s in seq if s) / max(len(seq), 1), firstpd, len(allu), sum(res) / max(len(res), 1), max(res) if res else 0, sum(1 for r in res if r == 1), sum(len(s) for s in seq) / max(len(seq), 1)))
