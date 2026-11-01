#!/usr/bin/env python3
"""Offline: the production time-domain comb gate statistic (nr_srs_td_comb_gate) per uplink slot of a raw capture, plus which symbols/RBs carry the periodic energy.
usage: srs_gate_stats.py <capture dir> [seconds] -- reuses the alignment of srs_physics_check.py"""
import sys, numpy as np
D = sys.argv[1]; secs = sys.argv[2] if len(sys.argv) > 2 else "4.0"
src = open(__file__.replace("srs_gate_stats.py", "srs_physics_check.py")).read().split("rows = []")[0]
sys.argv = [sys.argv[0], D, secs, "0.0"]
exec(src)
nslots = (n - start0) // SLOT - 1
def stat(win_syms):
    k = 0; S = 0.0
    for X in win_syms:                     # X: [ant, N] time samples of one symbol window
        lag = N // 2; m = lag // 2
        a = X[:, lag // 4: lag // 4 + m]; b = X[:, lag // 4 + lag: lag // 4 + lag + m]
        a = a - a.mean(1, keepdims=True); b = b - b.mean(1, keepdims=True)
        c = (a * np.conj(b)).sum(1); v1 = (np.abs(a) ** 2).sum(1); v2 = (np.abs(b) ** 2).sum(1)
        S += (m * np.abs(c) ** 2 / (v1 * v2)).sum(); k += X.shape[0]
    return S, k
res = []
for g in range(nslots):
    si = g % 10
    if si not in (7, 8, 9): continue
    ss = start0 + g * SLOT
    for l in range(max(8, 10 if si == 7 else 8), 14):
        s0 = ss + sym_off[l] + cp(l) // 2
        if s0 + N > n: continue
        S, k = stat([x[:, s0:s0 + N]])
        res.append((g, si, l, S, k))
res = np.array(res)
perslot = {}
for g, si, l, S, k in res: perslot.setdefault(g, [0, 0]); perslot[g][0] += S; perslot[g][1] += k
st = np.array([(g, v[0], v[1]) for g, v in perslot.items()])
thr = st[:, 2] + 8 * np.sqrt(st[:, 2])
op = st[:, 1] > thr
print(f"slots {len(st)}; gate stat / K: median {np.median(st[:,1]/st[:,2]):.2f} (white noise: 1.00) p99 {np.percentile(st[:,1]/st[:,2],99):.2f}; open {op.sum()} = {100*op.mean():.1f} %")
for si in (7, 8, 9):
    sel = np.array([int(g) % 10 == si for g in st[:, 0]])
    print(f"  slot-in-period {si}: open {op[sel].sum()}/{sel.sum()}")
for l in range(8, 14):
    sel = res[:, 2] == l
    print(f"  symbol {l}: per-(ant,symbol) stat median {np.median(res[sel,3]/res[sel,4]):.2f} p99 {np.percentile(res[sel,3]/res[sel,4],99):.2f}")
# ---- what are the strongest periodic symbols? frequency-domain look + periodicity ----
order = np.argsort(-(res[:, 3] / res[:, 4]))
print("strongest (g, slot-in-period, symbol, stat per ant-symbol):")
seen = []
for i in order[:400]:
    g, si, l, S, k = res[i]
    seen.append((int(g), int(si), int(l), S / k))
print("  ", [(a, b, c, round(d, 1)) for a, b, c, d in seen[:12]])
gs = sorted({a for a, b, c, d in seen[:150]})
print(f"  top-150 symbols fall on {len(gs)} distinct slots; slot-in-period histogram:", {si: sum(1 for a in gs if a % 10 == si) for si in (7, 8, 9)})
for P in (10, 20, 40, 80, 160, 320, 640):
    c = np.bincount([a % P for a in gs], minlength=P).max()
    print(f"  period {P:4d} slots: {c}/{len(gs)} in the best phase")
for g, si, l, st_ in seen[:4]:
    X = np.fft.fft(x[:, start0 + g * SLOT + sym_off[l] + cp(l) // 2:][:, :N], axis=1)
    Pw = (np.abs(X) ** 2).sum(0)[bins].reshape(NRB, 12)
    rb = Pw.mean(1); ref = np.median(rb)
    occ = np.where(rb > 3 * ref)[0]
    print(f"  slot {g} (si {si}) symbol {l}: RBs 3x above the median RB: {len(occ)} " + (f"range {occ.min()}..{occ.max()}" if len(occ) else ""))
    for K in (2, 4):
        for off in range(K):
            on = Pw[occ][:, off::K].sum(1) / (12 // K) if len(occ) else np.array([0.0]); allp = Pw[occ].mean(1) if len(occ) else np.array([1.0])
            print(f"     comb {K} offset {off}: on-comb share of occupied-RB power {np.mean(on*(12//K)/(12*allp)):.2f} (1/{K} = {1/K:.2f} if flat)")
