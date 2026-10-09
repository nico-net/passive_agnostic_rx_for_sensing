#!/usr/bin/env python3
"""CSI-FULL: low-MCS class A/B/C CRC table from receiver logs (EVMGRANT probe=0 lines; QAMFIX.md classes).
A: abs_slot%160 in {15,16,20,21} (TRS occasions) and first RB < 52; B: first RB < 52, other slots; C: first RB >= 52.
One entry per (rnti, frame.slot): a grant passes if any logged full decode of it passed (recovery arms included).
Usage: csifull_classes.py run.log [--mcs-max 4] [--snr-min 10] [--by-slot]"""
import argparse, re, sys
from collections import defaultdict

PAT = re.compile(r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) rb=(\d+)\+(\d+) "
                 r".*?snr=(-?[\d.]+).*?probe=(\d)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--mcs-max", type=int, default=4)
    ap.add_argument("--snr-min", type=float, default=10.0)
    ap.add_argument("--by-slot", action="store_true")
    a = ap.parse_args()
    for f in a.logs:
        g = {}
        for line in open(f, errors="replace"):
            if "EVMGRANT" not in line:
                continue
            m = PAT.search(line)
            if not m:
                continue
            rnti, fr, sl, crc, mcs, tbl, qm, nl, rb0, nrb, snr, probe = m.groups()
            if probe != "0" or rnti in ("ffff",) or int(mcs) > a.mcs_max or int(qm) != 2:
                continue
            key = (rnti, int(fr), int(sl))
            e = g.get(key)
            if e is None:
                g[key] = dict(crc=int(crc), nl=int(nl), nrb=int(nrb), rb0=int(rb0), snr=float(snr), abs=int(fr) * 20 + int(sl), slot=int(sl))
            else:
                e["crc"] = max(e["crc"], int(crc))
                e["snr"] = max(e["snr"], float(snr))
        tab = defaultdict(lambda: [0, 0])
        slot = defaultdict(lambda: [0, 0])
        for e in g.values():
            if e["snr"] < a.snr_min:
                continue
            if e["rb0"] >= 52:  # QAMFIX.md classes: by the grant's first RB (A+B+C counts reproduce its table)
                c = "C"
            elif e["abs"] % 160 in (15, 16, 20, 21):
                c = "A"
            else:
                c = "B"
            nl = e["nl"]
            for k in ((nl, c), (nl, "all")):
                tab[k][0] += e["crc"]
                tab[k][1] += 1
            if c == "B":
                slot[(nl, e["slot"])][0] += e["crc"]
                slot[(nl, e["slot"])][1] += 1
        print(f"== {f}  (MCS0-{a.mcs_max} QPSK, snr>={a.snr_min} dB, dedicated, probe=0)")
        for nl in sorted({k[0] for k in tab}):
            row = []
            for c in ("A", "B", "C", "all"):
                ok, n = tab[(nl, c)]
                row.append(f"{c}: {ok}/{n}" + (f" ({100.0 * ok / n:.1f} %)" if n else ""))
            print(f"  Nl={nl}  " + "  ".join(row))
        if a.by_slot:
            for nl in sorted({k[0] for k in slot}):
                print(f"  B by slot Nl={nl}: " + " ".join(f"{s}:{slot[(nl, s)][0]}/{slot[(nl, s)][1]}"
                                                      for s in range(20) if slot[(nl, s)][1]))


if __name__ == "__main__":
    main()
