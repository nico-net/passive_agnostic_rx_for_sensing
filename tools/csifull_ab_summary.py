#!/usr/bin/env python3
"""CSI-FULL: pool CSIFULLAB paired lines (same samples, several CSI-RS hypotheses) of one capture's replays.
A hypothesis passes a grant when either S variant decoded on the hypothesis itself ('1'). SNR/MCS filter from the
grant's EVMGRANT lines (max DD-SNR over its decodes). Classes as csifull_classes.py (first RB < 52 -> A at TRS slots)."""
import re, sys, collections
AB = re.compile(r"CSIFULLAB rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) ph=(\d+) nl=(\d+) mcs=(\d+) rb=(\d+)\+(\d+) .*?pop=(\d+) .*?n_own=(\d+) n_range=(\d+) (.*)")
EV = re.compile(r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=\d mcs=\d+ tbl=\d+ Qm=(\d) .*?snr=(-?[\d.]+)")
H = ["none", "own", "range", "all", "own16", "range_noerase"]
import os
snr_min = float(os.environ.get("SNR_MIN", "10"))
tab = collections.defaultdict(lambda: collections.Counter())
for f in sys.argv[1:]:
    snr = {}
    lines = []
    for line in open(f, errors="replace"):
        m = EV.search(line)
        if m:
            k = (m.group(1), m.group(2), m.group(3))
            snr[k] = max(snr.get(k, -99), float(m.group(5)))
            continue
        m = AB.search(line)
        if m:
            lines.append(m)
    for m in lines:
        k = (m.group(1), m.group(2), m.group(3))
        if int(m.group(6)) > 4 or snr.get(k, -99) < snr_min:
            continue
        res = dict(x.split("=") for x in m.group(12).split())
        c = "C" if int(m.group(7)) >= 52 else "A"
        key = (f"pop{m.group(9)}", f"Nl{m.group(5)}", c)
        tab[key]["grants"] += 1
        for h in H:
            if h in res:
                tab[key]["n_" + h] += 1
                tab[key][h] += "1" in res[h]
        if "1" in res.get("range", "") and "1" not in res.get("none", ""):
            tab[key]["range_only_gain"] += 1
        if "1" in res.get("none", "") and "1" not in res.get("range", ""):
            tab[key]["range_loss"] += 1
for key in sorted(tab):
    t = tab[key]
    print(" ".join(key), f"grants={t['grants']}", " ".join(f"{h}={t[h]}/{t['n_' + h]}" for h in H), f"gain={t['range_only_gain']} loss={t['range_loss']}")
