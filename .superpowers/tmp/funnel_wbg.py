#!/usr/bin/env python3
"""funnel_wbg.py <run dir>: SENSE_WBG log lines (rate/cost) + wide-DL-grant funnel (rnti_class, crc, unsolved)."""
import sys, re, json, collections
R = sys.argv[1]
log = re.sub(r'\x1b\[[0-9;]*m', '', open(R + '/run.log', errors='replace').read())
L = [m.groupdict() for m in re.finditer(r'SENSE_WBG cell=0 offered_hz=(?P<oh>[\d.]+) cost_us=(?P<c>[\d.]+) frac=(?P<f>[\d.]+) rate_hz=(?P<r>[\d.]+) ring_ns=(?P<rn>\d+) offered=(?P<o>\d+) admitted=(?P<a>\d+) taps=(?P<t>\d+) records=(?P<rec>\d+)', log)]
mean = lambda k: sum(float(x[k]) for x in L) / max(len(L), 1)
last = L[-1] if L else {}
cost = [float(x['c']) for x in L if float(x['c']) > 0]
print(R.split('/')[-1], "SENSE_WBG lines=%d rate_hz mean=%.2f offered_hz mean=%.2f cost_us(mean of nonzero)=%.0f frac mean=%.2f | last: %s" % (len(L), mean('r'), mean('oh'), sum(cost) / max(len(cost), 1), mean('f'), last))
G = collections.Counter(); sl = []
for ln in open(R + '/sense_records_last.jsonl', errors='replace'):
    if '"type":"grant"' not in ln or '"dir":"DL"' not in ln: continue
    d = json.loads(ln)
    if (d.get('nb_rb') or 0) < 128: continue
    G['wide'] += 1; G['class%s' % d.get('rnti_class')] += 1; G['crc%s' % d.get('crc')] += 1
    if d.get('rnti_class') == 0 and d.get('crc') == 1: G['C_crc1'] += 1
    G['ndi_null'] += d.get('ndi') is None
    G['cov_' + str(d.get('dl_coverage'))] += 1
print("  wide DL grants:", dict(G))
