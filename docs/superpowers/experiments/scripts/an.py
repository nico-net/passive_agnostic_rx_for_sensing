#!/usr/bin/env python3
"""Per-run DL/UL by grant width + ADC census, streaming (low memory). usage: an.py RUNDIR [RUNDIR...]"""
import sys, re, glob, statistics as st
ANSI = re.compile(r'\x1b\[[0-9;]*m')
def kv(line):
    return dict(re.findall(r'(\w+)=([^ ]+)', line))
def wbin(n, edges):
    for lo, hi, name in edges:
        if lo <= n <= hi: return name
DLB = [(1,4,'1-4'),(5,32,'5-32'),(33,128,'33-128'),(129,999,'>128')]
ULB = [(1,8,'1-8'),(9,64,'9-64'),(65,999,'>64')]
for rd in sys.argv[1:]:
    logs = glob.glob(rd + '/*/run.log')
    if not logs: print(rd, 'no run.log'); continue
    dl = {}; ul = {}; adc = pq = pp = None; ulre = {}
    with open(logs[0], errors='replace') as f:
        for line in f:
            if 'EVMGRANT' in line:
                d = kv(ANSI.sub('', line))
                if d.get('probe') != '0' or d.get('rnti') == '0xffff' or 'rb' not in d: continue
                try: n = int(d['rb'].split('+')[1]); s = float(d['snr']); c = int(d['crc'])
                except Exception: continue
                b = (wbin(n, DLB), d.get('Nl'))
                dl.setdefault(b, []).append((c, s))
            elif 'UL_PUSCH_CRC' in line:
                d = kv(ANSI.sub('', line))
                if d.get('probe') != '0': continue
                try: n = int(d['prb'].split('+')[1]); s = float(d['snr_db']); c = int(d['ok'])
                except Exception: continue
                b = wbin(n, ULB); ul.setdefault(b, []).append((c, s))
                k = (b, d.get('uci_hint'), d.get('uci_re') != '0'); ulre[k] = ulre.get(k, 0) + 1
            elif 'ADCCLASS' in line: adc = ANSI.sub('', line).strip()
            elif 'PDSCHQ queued' in line: pq = ANSI.sub('', line).strip()
            elif 'pusch_passive[' in line: pp = ANSI.sub('', line).strip()
    print('=' * 100); print(rd)
    if adc:
        m = re.search(r'DL\[(.*?)\]', adc)
        if m:
            ch = re.findall(r'(-?[\d.]+)dBFS w:clip=([\d.]+)% hot=([\d.]+)%', m.group(1))
            print('ADC DL mean dBFS/clip%%/hot%% per ch: %s' % ' '.join('%s/%s/%s' % c for c in ch))
    if pq: print(pq[pq.find('PDSCHQ'):][:140])
    if pp: print(pp[pp.find('pusch_passive'):][:110])
    print('DL non-probe grants by width,Nl:   n  crc  med_snr')
    for b in sorted(dl, key=lambda x: (x[1] or '', [e[2] for e in DLB].index(x[0]) if x[0] else 9)):
        v = dl[b]; print('   %-8s Nl=%s %6d %5.3f %6.1f' % (b[0], b[1], len(v), sum(c for c, _ in v) / len(v), st.median(s for _, s in v)))
    print('UL non-probe PUSCH by width:  n  crc  med_snr')
    for b in [e[2] for e in ULB]:
        v = ul.get(b)
        if v: print('   %-8s %6d %5.3f %6.1f' % (b, len(v), sum(c for c, _ in v) / len(v), st.median(s for _, s in v)))
    print('UL uci_hint/uci_re>0 counts:', {('%s h%s re%d' % (k[0], k[1], k[2])): n for k, n in sorted(ulre.items(), key=str)})
