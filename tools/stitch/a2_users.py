#!/usr/bin/env python3
"""a2_users.py <run dir>...: boundaries between different users in the same slot (same DM-RS symbol, adjacent comb REs, k gap <= 4)
vs the same-user PRB boundaries (a1). Reports jump stats and counts; also the number of same-slot multi-user pairs by PRB gap."""
import sys, numpy as np, collections, json
from load import *
def rob(x): x = np.array(x); return float(np.median(np.abs(x - np.median(x))) * 1.4826)
def run(R):
    D = load_pdsch(R, 1); by = collections.defaultdict(list)
    for d in D: by[(d['slot'], d['l'])].append(d)
    gaps = collections.Counter(); J = []; n_multi = 0; EC = []
    for (s, l), L in by.items():
        users = collections.defaultdict(list)
        for d in L: users[d['rnti']].append(d)
        if len(users) < 2: continue
        n_multi += 1
        segs = []   # one segment per user = union of its records (merge by concatenation)
        for r, ds in users.items():
            segs.append((np.concatenate([d['k'] for d in ds]), np.concatenate([d['h'] for d in ds], axis=1), r, ds[0]['nl']))
        for ka, ha, ra, na in segs:   # edge control: coherence of a user's own edge RE with the RE one PRB inside (edge-estimate quality, no user change)
            o = np.argsort(ka); kk, hh = ka[o], ha[:, o]
            for e, inner in ((0, 6), (-1, -7)):
                if len(kk) > 12 and abs(kk[inner] - kk[e]) == 12: EC.append(float(jump(hh, np.array([e]), np.array([inner]))[1][0]))
        for a in range(len(segs)):
            for b in range(len(segs)):
                if a == b: continue
                ka, ha, ra, na = segs[a]; kb, hb, rb, nb = segs[b]
                if ka.max() >= kb.min() or ka.max() < 0: continue   # a below b
                ia = np.argmax(ka); ib = np.argmin(kb); gap_prb = (kb[ib] - ka[ia]) / 12.0
                if kb.min() > ka.max(): gaps[int(round(gap_prb))] += 1
                if kb[ib] - ka[ia] <= 4:
                    h = np.stack([ha[:, ia], hb[:, ib]], 1); c, coh, ar = jump(h, np.array([0]), np.array([1]))
                    J.append((float(np.angle(c[0])), float(coh[0]), float(20*np.log10(ar[0])), na, nb, int(ka[ia]//12)))
    return n_multi, gaps, J, EC
if __name__ == '__main__':
    for R in sys.argv[1:]:
        n, gaps, J, EC = run(R); print(R.split('/')[-1], 'multi-user (slot,symbol) groups', n, 'gap histogram (PRB, between ordered user pairs, nearest):', dict(sorted(gaps.items())[:12]))
        print('  edge control (same user, edge RE vs RE one PRB inside): n=%d coh median %.3f frac<0.9 %.3f' % (len(EC), np.median(EC), np.mean(np.array(EC) < .9)))
        if J:
            J = np.array(J); print('  adjacent boundaries', len(J), 'dphi MAD', round(rob(J[:, 0]), 3), 'coh median', round(float(np.median(J[:, 1])), 3), 'frac coh<0.9', round(float(np.mean(J[:, 1] < .9)), 3), 'amp dB MAD', round(rob(J[:, 2]), 2), 'dphi uniform-null MAD would be 2.17')
