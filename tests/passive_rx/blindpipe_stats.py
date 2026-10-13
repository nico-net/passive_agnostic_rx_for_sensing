#!/usr/bin/env python3
"""blindpipe_stats.py LOG_OR_DIR... : BLINDPIPE (2026-10-13) per-run summary of a passive receiver log.
Prints, per log: sync slot, first VERIFIED bank add (slots after sync), bank adds, dwell windows, C-RNTI DCI 1_1
accepts per 1000 slots inside vs outside dwells (the dwell-starvation metric), candidate ladders seen, PDSCHQ
crc_ok/decoded, CRC-OK RNTIs, PDCCH_NID_GPU passes (stage times, records by duration), ACQ carrier lines, the last
TRAFFIC and POINTA_SWEEP lines. Slot stamps are sfn*20+slot unwrapped at 1024 frames; a DIR resolves to DIR/run.log."""
import os, re, sys
from collections import Counter, defaultdict

ANSI = re.compile(r"\x1b\[[0-9;]*m")
R_STAMP = re.compile(r"sfn=(\d+) slot=(\d+)")
R_RNTI = re.compile(r"blind PDCCH rnti_seen .* rnti=(0x[0-9a-f]+) sfn=(\d+) slot=(\d+) fmt=(\S+)")
R_ABS = re.compile(r'"abs_slot":(\d+),')
R_METR = re.compile(r'"pdschq_decoded":(\d+),"pdschq_crc_ok":(\d+)')
R_EVM = re.compile(r"EVMGRANT rnti=(0x[0-9a-f]+) .* crc=(\d) mcs=(\d+) .* probe=(\d)")
R_NID = re.compile(r"PDCCH_NID_GPU pass (\d+): stage1 ([\d.]+) ms stage2 ([\d.]+) ms .*-> (\d+) CORESETs:(.*)")
R_REC = re.compile(r"nid=(\d+) sym=(\d+) dur=(\d+) grid_rb=(\d+) groups=(\d+) L=(\d+) R=(\d+) shift=(\d+) ties=(\d+)")
R_LADDER = re.compile(r"ladder: num_cces=(\d+) ncand=(\d+)/(\d+) .*\((AL1=\d+ AL2=\d+ AL4=\d+ AL8=\d+)\)")


def analyse(path):
    if os.path.isdir(path):
        path = os.path.join(path, "run.log")
    slot = None  # unwrapped receiver slot from the latest stamp
    base = 0
    last_raw = None
    sync_slot = None
    first_bank = None
    bank_adds = 0
    dwells = []  # [start_slot, end_slot or None, text]
    crnti = []  # (slot, rnti)
    ladders = Counter()
    metr = (0, 0)
    crc_rntis = set()
    all_rntis = set()
    mcs_pass = defaultdict(lambda: [0, 0])
    nid = {"passes": 0, "s1": 0.0, "s2": 0.0, "dur": Counter(), "ties": Counter()}
    acq, traffic, pointa, misc = [], None, None, []
    with open(path, errors="replace") as f:
        for line in f:
            line = ANSI.sub("", line)
            m = R_STAMP.search(line)
            if m:
                raw = int(m.group(1)) * 20 + int(m.group(2))
                if last_raw is not None and raw < last_raw - 10000:
                    base += 20480
                last_raw = raw
                slot = base + raw
            if "Initial sync successful" in line and sync_slot is None:
                sync_slot = slot if slot is not None else 0
            if "VERIFIED -> bank add" in line:
                bank_adds += 1
                if first_bank is None:
                    first_bank = slot
                if dwells and dwells[-1][1] is None:
                    dwells[-1][1] = slot
            if "dwell start" in line:
                dwells.append([slot, None, line.split("SENSING: ")[-1].strip()])
            if "rejected (no fresh CRC evidence" in line and dwells and dwells[-1][1] is None:
                dwells[-1][1] = slot
            m = R_RNTI.search(line)
            if m and m.group(4) == "1_1":
                crnti.append((slot, m.group(1)))
            m = R_LADDER.search(line)
            if m:
                ladders[f"cces={m.group(1)} ncand={m.group(2)}/{m.group(3)} {m.group(4)}"] += 1
            m = R_METR.search(line)
            if m:
                metr = (int(m.group(1)), int(m.group(2)))
            m = R_EVM.search(line)
            if m:
                all_rntis.add(m.group(1))
                if m.group(4) == "0":
                    b = mcs_pass[int(m.group(3)) // 5 * 5]
                    b[1] += 1
                    if m.group(2) == "1":
                        b[0] += 1
                if m.group(2) == "1":
                    crc_rntis.add(m.group(1))
            m = R_NID.search(line)
            if m:
                nid["passes"] += 1
                nid["s1"] += float(m.group(2))
                nid["s2"] += float(m.group(3))
                for r in R_REC.finditer(m.group(5)):
                    nid["dur"][int(r.group(3))] += 1
                    nid["ties"][int(r.group(9))] += 1
            if "ACQ carrier" in line:
                acq.append(line.split("SENSING: ")[-1].strip()[:200])
            if "TRAFFIC slots=" in line:
                traffic = line.split("SENSING: ")[-1].strip()
            if "POINTA_SWEEP" in line:
                pointa = line.split("SENSING: ")[-1].strip()
            if "DL_TRAFFIC_PROBE on" in line or "Segmentation" in line or "Assertion" in line:
                misc.append(line.strip()[:160])
    # dwell-starvation metric: C-RNTI accepts per 1000 slots inside vs outside dwell windows, after the first bank add
    inside = outside = 0
    in_slots = out_slots = 0
    if first_bank is not None and slot is not None:
        windows = [(a, b if b is not None else slot) for a, b, _ in dwells if a is not None and a >= first_bank]
        for s, _ in crnti:
            if s is None or s < first_bank:
                continue
            if any(a <= s < b for a, b in windows):
                inside += 1
            else:
                outside += 1
        in_slots = sum(b - a for a, b in windows)
        out_slots = max(0, slot - first_bank - in_slots)
    print(f"== {path}")
    print(f"  sync_slot={sync_slot} first_bank_after_sync={None if first_bank is None or sync_slot is None else first_bank - sync_slot}"
          f" bank_adds={bank_adds} dwells={len(dwells)} last_slot={slot}")
    for a, b, t in dwells[:12]:
        print(f"    dwell [{a}, {b}) {t[:110]}")
    print(f"  C-RNTI DCI 1_1 accepts after first bank: inside dwells {inside} over {in_slots} slots"
          f" ({1000.0 * inside / in_slots if in_slots else float('nan'):.0f}/1000), outside {outside} over {out_slots} slots"
          f" ({1000.0 * outside / out_slots if out_slots else float('nan'):.0f}/1000)")
    for k, v in sorted(ladders.items()):
        print(f"  ladder x{v}: {k}")
    print(f"  PDSCHQ decoded={metr[0]} crc_ok={metr[1]} CRC-OK RNTIs={len(crc_rntis)} of {len(all_rntis)} EVMGRANT RNTIs")
    print("  CRC by MCS bucket (non-probe): " + " ".join(f"{k}-{k + 4}:{v[0]}/{v[1]}" for k, v in sorted(mcs_pass.items())))
    if nid["passes"]:
        print(f"  NID_GPU passes={nid['passes']} stage1 mean {nid['s1'] / nid['passes']:.2f} ms stage2 mean {nid['s2'] / nid['passes']:.2f} ms"
              f" records by dur {dict(nid['dur'])} ties {dict(sorted(nid['ties'].items()))}")
    for a in acq[:3]:
        print(f"  {a}")
    if traffic:
        print(f"  {traffic}")
    if pointa:
        print(f"  {pointa}")
    for mline in misc[:4]:
        print(f"  {mline}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        analyse(p)
