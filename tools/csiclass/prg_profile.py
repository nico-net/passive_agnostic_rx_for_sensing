#!/usr/bin/env python3
"""Precoder granularity from the DM-RS channel alone (no CSI-RS needed): adjacent-RB direction coherence c(rb) = |<h(rb),h(rb+1)>|^2 / (|h(rb)|^2 |h(rb+1)|^2) of the 4-RX DM-RS
estimates of CRC-OK single-layer grants, as a function of (absolute RB index mod G). A precoder that changes at PRG boundaries (size G) produces a dip of c at rb = G-1 mod G (pair crossing
the boundary); a wideband precoder gives a flat profile. Absolute RB parity matters because PRGs are defined from CRB 0."""
import sys, pickle, numpy as np
G = [g for g in pickle.load(open(sys.argv[1], "rb")) if g["stat"] >= 0.8 and g["nrb"] >= 24]
print("grants", len(G), "RBs", sum(g["nrb"] for g in G))
cs = []; ab = []
for g in G:
    h = g["h"]; c = np.abs((np.conj(h[:-1]) * h[1:]).sum(1)) ** 2 / ((np.abs(h[:-1]) ** 2).sum(1) * (np.abs(h[1:]) ** 2).sum(1)); cs.append(c); ab.append(g["rb0"] + np.arange(len(c)))
cs = np.concatenate(cs); ab = np.concatenate(ab); print("mean adjacent-RB direction coherence %.3f (4-dim random: 0.25)" % cs.mean())
for Gz in (2, 4, 8, 16):
    prof = [cs[ab % Gz == r].mean() for r in range(Gz)]
    print("mod %2d profile (pair rb,rb+1; index = rb mod G):" % Gz, " ".join("%.3f" % p for p in prof), " | min at", int(np.argmin(prof)), "dip %.3f" % (np.mean(prof) - min(prof)))
