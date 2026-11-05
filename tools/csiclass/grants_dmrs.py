#!/usr/bin/env python3
"""Offline H_eff of CRC-OK single-layer grants of a replay (rec.jsonl from tools/csiclass/replay_sense.sh), from the raw capture.
Grant fields (abs_slot, rnti, start_rb, nb_rb, dmrs_symb_pos, dmrs_scrambling_id, nl, crc) come from the receiver's own decode (CRC-OK); the DM-RS channel is re-estimated here:
LS at the DM-RS REs of port 1000 (type 1, comb offset 0 or 1 tested), sequence index m = k/2 (CRB0-relative) or allocation-relative (both tested), n_SCID 0/1 tested.
The capture slot j is not taken from rx_ts (its offset to the slot start is not a whole number of slots): j = abs_slot - c, c found per capture by maximising the DM-RS match over
c = c0 + 20 i (slot-in-frame is exact). Output npz: for each accepted grant slot, rb range, h_eff[rb] (rx), the match statistic and the chosen hypothesis."""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__)); import csilib as L, dmrs_scan as D

ap = argparse.ArgumentParser(); ap.add_argument("cap"); ap.add_argument("coh"); ap.add_argument("rec"); ap.add_argument("--c0", type=int, default=0)
ap.add_argument("--span", type=int, default=400); ap.add_argument("--out", required=True); ap.add_argument("--max", type=int, default=60); ap.add_argument("--all-rnti", action="store_true"); ap.add_argument("--nl", type=int, default=1)
a = ap.parse_args()
sl, meta = L.open_slotter(a.cap, a.coh)
G = [json.loads(l) for l in open(a.rec)]; G = [g for g in G if g["type"] == "grant" and g["crc"] == 1 and g["nl"] == a.nl and g["dmrs_scrambling_id"] is not None]
print("CRC-OK grants with Nl=%d: %d" % (a.nl, len(G)))
def syms(g): return [i for i in range(14) if (g["dmrs_symb_pos"] >> i) & 1]
def est(Y, g, delta, mode, scid):
    """returns (h[rb,rx] per RB averaged over DM-RS REs, stat)"""
    ns = g["abs_slot"] % 20; rb0, nrb = g["start_rb"], g["nb_rb"]; hs = []; st = 0
    for l in syms(g):
        nre = 6 * (rb0 + nrb) + 4
        r = D.dmrs_seq(np.array([g["dmrs_scrambling_id"]]), scid, ns, l, nre)[0]
        ks = np.array([12 * rb + 2 * q + delta for rb in range(rb0, rb0 + nrb) for q in range(6)])  # DM-RS REs of the allocation: 6 per RB, spacing 2 (comb)
        ks = np.array([12 * rb + 4 * q + delta for rb in range(rb0, rb0 + nrb) for q in range(3)] ) if False else np.array([12 * rb + 2 * (2 * q + kp) + 0 for rb in range(rb0, rb0 + nrb) for q in range(3) for kp in range(2)]) + delta
        # type 1: k = 4n + 2k' + delta  ->  per RB: 6 REs: 12rb + {0,2,4,6,8,10}+delta ... wait comb is every 2nd sc: 6 per RB at 12rb+2i+delta
        ks = np.array([12 * rb + 2 * i + delta for rb in range(rb0, rb0 + nrb) for i in range(6)])
        m = ks // 2 if mode == "crb" else (ks - 12 * rb0) // 2
        rr = r[m]
        # type-1 DM-RS: delta (comb) is the CDM group; sequence index runs over the REs of that comb only -> m = (k - delta)/2 ; k even-spaced so (k-delta)//2
        y = Y[l][:, ks]
        z = y * np.conj(rr)[None, :]  # rx x RE
        zz = z.sum(0); q = zz[1:] * np.conj(zz[:-1]); st += abs(q.sum()) / np.maximum(np.abs(q).sum(), 1e-30)
        hs.append(z.reshape(4, nrb, 6).mean(2).T)  # rb x rx
    return np.mean(hs, 0), st / len(syms(g))
# slot index: rx_ts is the sample index of the slot start (+-130 samples), so j = abs_slot - c with c = abs_slot - round((rx_ts - t0)/61440) (constant; verified by TRS phase)
t0 = L.M.SLOT * 0 + sl.t0
cs = np.array([g["abs_slot"] - round((g["rx_ts"] - t0) / L.M.SLOT) for g in G if g["rx_ts"]]); c = int(np.median(cs)); print("c =", c, "spread", cs.min(), cs.max())
out = []
Gs = [g for g in G if g["rnti"] != 65535 or a.all_rnti][: a.max]
for g in Gs:
    j = g["abs_slot"] - c; Y = sl.slot(j)
    if Y is None: continue
    Yd = {l: Y[:, l] for l in range(14)}
    res = []
    for delta in (0, 1):
        for mode in ("crb", "alloc"):
            for scid in (0, 1):
                h, st = est(Yd, g, delta, mode, scid); res.append((st, delta, mode, scid, h))
    res.sort(key=lambda r: -r[0])
    print("abs", g["abs_slot"], "rnti", g["rnti"], "rb", g["start_rb"], g["nb_rb"], "nl", g["nl"], "best stat %.2f (delta %d %s scid %d) 2nd %.2f" % (res[0][0], res[0][1], res[0][2], res[0][3], res[1][0]))
    out.append(dict(j=j, abs=g["abs_slot"], rnti=g["rnti"], rb0=g["start_rb"], nrb=g["nb_rb"], stat=res[0][0], stat2=res[1][0], hyp=(res[0][1], res[0][2], res[0][3]), h=res[0][4]))
import pickle; pickle.dump(out, open(a.out, "wb"))
