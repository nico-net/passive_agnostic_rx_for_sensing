#!/usr/bin/env python3
"""Recorded-input hypotheses; no RF access. Run via heavy.sh.

Usage: decodefix_inputs.py LOG PDL1 OUTPUT [scale|tdra]
scale: full dedicated Nl=1 MCS0..4 SNR>=6; normalization, RV and scale A/B.
tdra: all full dedicated CPU decodes; baseline and alternate S=0/1 (S=0 uses symbol-0 erasures).
OUTPUT.json retains the original per-grant diagnostic beside each trial.
"""
import collections
import json
import math
import pathlib
import re
import struct
import sys
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]

def gold(n, seed):
    x1, x2 = 1, seed
    result = np.empty(n, dtype=np.int16)
    for i in range(n + 1600):
        if i >= 1600:
            result[i - 1600] = 1 - 2 * ((x1 ^ x2) & 1)
        x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1) << 30)
        x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1) << 30)
    return result

def records(log, dump):
    rows = collections.defaultdict(list)
    for line in open(log, errors="replace"):
        if "SENSING: DECODEDIAG " not in line:
            continue
        d = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if d["probe"] != "0":
            continue
        key = (int(d["rnti"], 16), d["fs"], *(int(d[k]) for k in ["G", "nl", "qm", "tbl"]), d["rb"], int(d["S"]), int(d["L"]))
        rows[key].append(d)
    with open(dump, "rb") as f:
        while magic := f.read(4):
            assert magic == b"PDL1"
            rnti, fr, sl, qm, nl, rb, nrb, s, l, mcs, nid, desc, g, tbl = struct.unpack("<14i", f.read(56))
            llr = np.frombuffer(f.read(2 * g), dtype="<i2").copy()
            key = (rnti, f"{fr}.{sl}", g, nl, qm, tbl, f"{rb}+{nrb}", s, l)
            if not rows[key]:
                continue
            d = rows[key].pop(0)
            if rnti <= 127 or rnti == 65535 or d["decoder"] != "1":
                continue
            yield d, llr, desc

def normalized(llr, a, bg):
    kcb = 3840 if bg == 2 else 8448
    c = max(1, math.ceil((a + 24) / (kcb - 24))) if a + 24 > kcb else 1
    span = (len(llr) + c - 1) // c
    mean = int(np.abs(llr[:span:16].astype(np.int32)).mean())
    shift = 0
    while shift < 8 and mean >> shift > 40:
        shift += 1
    return llr >> shift, shift

def tbs(d, length):
    # Mirror the in-tree integer arithmetic, obtaining the actual tables from it.
    src = (ROOT / "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c").read_text()
    name = ["Table_51311", "Table_51312", "Table_51313"][int(d["tbl"])]
    entries = re.search(name + r"\[32\]\[2\]\s*=\s*\{(.*?)\};", src, re.S)[1]
    qm, rate = [tuple(map(int, x)) for x in re.findall(r"\{(\d+),\s*(\d+)\}", entries)][int(d["mcs"])]
    nrb = int(d["rb"].split("+")[1])
    # G determines the original RE budget; these recorded trials require no other exclusions.
    dmrs = int(d["L"]) * 12 - int(d["G"]) // (nrb * qm * int(d["nl"]))
    ninfo = (min(156, length * 12 - dmrs - int(d["xoh"])) * nrb * (rate // 5) * qm * int(d["nl"])) >> (11 + int(d["scaling"]))
    if ninfo <= 3824:
        n = max(3, ninfo.bit_length() - 1 - 6)
        ni = max(24, (ninfo >> n) << n)
        table_src = (ROOT / "openair2/LAYER2/NR_MAC_COMMON/nr_compute_tbs_common.c").read_text()
        values = re.search(r"Tbstable_nr\[INDEX_MAX_TBS_TABLE\]\s*=\s*\{(.*?)\};", table_src, re.S)[1]
        a = next(v for v in map(int, re.findall(r"\d+", values)) if v >= ni)
    else:
        n = (ninfo - 24).bit_length() - 1 - 5
        ni = max(3840, ((ninfo - 24 + (1 << (n - 1))) >> n) << n)
        c = math.ceil((ni + 24) / 3816) if rate <= 2560 else math.ceil((ni + 24) / 8424) if ni > 8424 else 1
        a = 8 * c * math.ceil((ni + 24) / (8 * c)) - 24
    bg = 2 if a <= 292 or (a <= 3824 and rate <= 6666) or rate <= 2560 else 1
    return a, bg

def main():
    log, dump, out = sys.argv[1:4]
    mode = sys.argv[4] if len(sys.argv) > 4 else "scale"
    assert mode in ["scale", "tdra"]
    meta = []
    with open(out, "wb") as dst:
        for d, raw, desc in records(log, dump):
            if d.get("attempt", "0") != "0":
                continue
            if mode == "scale" and (int(d["nl"]) != 1 or int(d["mcs"]) > 4 or float(d["snr"]) < 6):
                continue
            seed = (int(d["rnti"], 16) << 15) + int(d["dataid"])
            a, bg, rv = int(d["tbs"]), int(d["BG"]), int(d["rv"])
            scrambled = raw * gold(len(raw), seed) if desc else raw
            base, shift = normalized(scrambled * gold(len(raw), seed), a, bg)
            trials = [("base", base, a, bg, rv, 1, 1, shift)]
            if mode == "scale":
                for num, den in [(1, 2), (1, 4), (1, 8), (1, 16), (2, 1), (4, 1), (8, 1), (16, 1)]:
                    trials.append((f"scale{num}/{den}", base, a, bg, rv, num, den, shift))
                for alt_rv in range(1, 4):
                    trials.append((f"rv{alt_rv}", base, a, bg, alt_rv, 1, 1, shift))
            elif int(d["S"]) <= 1 and int(d["S"]) + int(d["L"]) == 14 and all(d[k] == "0" for k in ["ptrs_re", "csi_re", "ssb_re"]):
                sym = int(d["rb"].split("+")[1]) * 12 * int(d["qm"]) * int(d["nl"])
                alt_s = 1 - int(d["S"])
                alt = scrambled[sym:] if alt_s else np.concatenate([np.zeros(sym, dtype=np.int16), scrambled])
                aa, abg = tbs(d, 14 - alt_s)
                alt, ashift = normalized(alt * gold(len(alt), seed), aa, abg)
                label = "S1_exact" if alt_s else "S0_erasures"
                trials.append((label, alt, aa, abg, rv, 1, 1, ashift))
            for label, llr, aa, abg, arv, num, den, ashift in trials:
                ix = len(meta)
                dst.write(struct.pack("<12I", ix, aa, len(llr), int(d["qm"]), int(d["nl"]), abg, arv, int(d["lbrm"]), 8, num, den, 0))
                dst.write(llr.astype("<i2").tobytes())
                meta.append(dict(d, trial=label, norm_shift=ashift, id=ix, alt_tbs=aa, alt_G=len(llr), alt_BG=abg))
    pathlib.Path(out + ".json").write_text(json.dumps(meta))
    print(f"{len(meta)} trials")

if __name__ == "__main__":
    main()
