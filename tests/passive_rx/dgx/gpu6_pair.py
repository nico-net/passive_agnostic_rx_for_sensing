#!/usr/bin/env python3
"""GPU6: paired comparison of two offline replays (no receiver/device access).

Grant identity = every EVMGRANT field that describes WHAT was decoded (RNTI, frame.slot, MCS, table,
Qm, Nl, RBs, symbols, ports, CDM groups, DM-RS symbols, probe flag); outcome = every field the receiver
COMPUTED from the samples (CRC, EVM, SNR, per-branch SNR, noise, combining weights, ...), timing excluded.
Run-to-run scheduling changes which hypotheses each run tries and the receiver state they meet, so a
CPU-vs-CPU control pair is needed to read a CPU-vs-GPU pair.

usage: gpu6_pair.py A/run.log B/run.log
"""
import collections
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
ID = ("rnti", "mcs", "tbl", "Qm", "Nl", "rb", "nsym", "ports", "cdm", "ndmrs", "probe")
SKIP = ("dec_us", "tok")  # timing, per-run token


def read(path):
    rows = collections.defaultdict(list)
    dci = collections.Counter()
    crc = collections.Counter()
    snr = collections.defaultdict(list)
    with open(path, errors="replace") as f:
        for raw in f:
            line = ANSI.sub("", raw)
            if "DCIRAW " in line:
                m = re.search(r"rnti=(0x[0-9a-fA-F]+)", line)
                if m:
                    dci[m.group(1).lower()] += 1
            if "EVMGRANT " not in line:
                continue
            m = re.search(r"EVMGRANT rnti=(0x[0-9a-fA-F]+) (\d+)\.(\d+) (.*)", line)
            if not m:
                continue
            kv = dict(re.findall(r"(\w+)=(\S+)", m.group(4)))
            kv["rnti"] = m.group(1).lower()
            key = (m.group(2), m.group(3)) + tuple(kv.get(k, "") for k in ID)
            out = tuple(sorted((k, v) for k, v in kv.items() if k not in ID and k not in SKIP))
            rows[key].append(out)
            crc[kv["rnti"]] += int(kv.get("crc", "0"))
            try:
                snr[kv["rnti"]].append(float(kv["snr"]))
            except (KeyError, ValueError):
                pass
    return rows, dci, crc, snr


def main(a, b):
    ra, da, ca, sa = read(a)
    rb_, db, cb, sb = read(b)
    common = sorted(set(ra) & set(rb_))
    same = crc_diff = snr_diff = 0
    for k in common:
        oa, ob = sorted(ra[k]), sorted(rb_[k])
        if oa == ob:
            same += 1
            continue
        fa = [dict(o) for o in oa]
        fb = [dict(o) for o in ob]
        if sorted(x.get("crc") for x in fa) != sorted(x.get("crc") for x in fb):
            crc_diff += 1
        try:
            if abs(statistics.median(float(x["snr"]) for x in fa) - statistics.median(float(x["snr"]) for x in fb)) > 0.1:
                snr_diff += 1
        except (KeyError, ValueError):
            pass
    print(f"grants A={len(ra)} B={len(rb_)} common={len(common)} identical_outcome={same} "
          f"({100.0 * same / max(1, len(common)):.2f} %) crc_differs={crc_diff} snr_differs_gt_0.1dB={snr_diff}")
    print("RNTI      DCIRAW A/B    CRC-OK A/B    median SNR A/B")
    for r in sorted(set(da) | set(db) | set(ca) | set(cb)):
        ma = f"{statistics.median(sa[r]):.1f}" if sa[r] else "-"
        mb = f"{statistics.median(sb[r]):.1f}" if sb[r] else "-"
        print(f"{r:8s} {da[r]:6d}/{db[r]:<6d} {ca[r]:6d}/{cb[r]:<6d} {ma:>6s}/{mb:<6s}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
