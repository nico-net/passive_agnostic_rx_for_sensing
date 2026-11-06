#!/usr/bin/env python3
"""agg_wbg.py <campaign dir>: one row per run from an.txt/inv.txt (written by an_sp_wbg.py / inv_wbg.py)."""
import sys, re, json, os
C = sys.argv[1]
for ln in open(C + '/runs.tsv'):
    t, r, pci, ok, arm, wall = ln.rstrip('\n').split('\t')
    a = open(r + '/an.txt').read(); i = open(r + '/inv.txt').read()
    f = lambda p, s=a: (re.search(p, s) or [None, None])[1]
    cpu = open(r + '/cpu_cores.txt').read().strip() if os.path.exists(r + '/cpu_cores.txt') else '?'
    ws = re.search(r"WBG counters: (\{.*\})", a); w = eval(ws.group(1)) if ws else {}
    print(t, arm, ok, wall, 'cpu', cpu, 'wide_gr/s', f(r'grants/s ([\d.]+)', i), 'cov>=128', f(r'valid>=128 rs=\d+ \(([\d.]+)%\)', i), 'anyrs', f(r'any pdsch rs=\d+ \(([\d.]+)%\)', i),
          'avail', f(r'avail25ms=([\d.]+)'), f(r'avail100ms=([\d.]+)'), f(r'avail1s=([\d.]+)'), 'wbg', list(w.values()))
