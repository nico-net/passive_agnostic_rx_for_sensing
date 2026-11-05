#!/usr/bin/env python3
"""Despread a 32-port NZP CSI-RS (given id, n_s, first slot, period, symbols, k bars, row hypothesis) over a raw capture.
Saves H[occ, rb, port, rx] (per RB, port channel incl. 1/CDM-size averaging) + slot index of each occasion."""
import argparse, sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import csilib as L
ap = argparse.ArgumentParser()
ap.add_argument("cap"); ap.add_argument("coh"); ap.add_argument("--id", type=int, required=True); ap.add_argument("--ns", type=int, required=True)
ap.add_argument("--j0", type=int, required=True); ap.add_argument("--period", type=int, required=True)
ap.add_argument("--l0", type=int, default=0); ap.add_argument("--l1", type=int, default=0); ap.add_argument("--symbols", default="")
ap.add_argument("--kbars", default="4,6,8,10"); ap.add_argument("--row", type=int, default=16); ap.add_argument("--out", required=True)
ap.add_argument("--conv", default="A"); ap.add_argument("--max-occ", type=int, default=1000)
a = ap.parse_args()
sl, meta = L.open_slotter(a.cap, a.coh)
kb = [int(x) for x in a.kbars.split(",")]
syms = [int(x) for x in a.symbols.split(",")] if a.symbols else sorted({a.l0, a.l0 + 1, a.l1, a.l1 + 1})
Hs, js = [], []
j = a.j0
while len(Hs) < a.max_occ:
    Y = sl.slot(j, range(14))
    if Y is None: break
    Ys = {l: Y[:, l] for l in syms}
    Hs.append(L.despread_groups(Ys, a.ns, a.id, syms, kb, conv=a.conv) if a.symbols else L.despread(Ys, a.ns, a.id, a.row, kb, a.l0, a.l1, conv=a.conv)); js.append(j); j += a.period
np.savez_compressed(a.out, H=np.array(Hs), j=np.array(js), meta=str(meta))
print("occasions", len(Hs))
