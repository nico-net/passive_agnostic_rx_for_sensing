#!/usr/bin/env python3
"""Offline re-read of traffic-mode DCI 1_0 / 0_0 bits using the "config" records (cfg_ref -> bwp_size).
shortcut: 1_1 offline re-parse not implemented, add when the pre_ant split is solvable offline."""
import json
import math
import sys

# TS 38.212 7.3.1.2.1 (DL 1_0, C-RNTI) after the FDRA: name, width
_DL10 = [("tda_index", 4), ("vrb_prb", 1), ("mcs", 5), ("ndi", 1), ("rv", 2), ("harq_pid", 4),
         ("dai", 2), ("tpc", 2), ("pri", 3), ("k1", 3)]
# TS 38.212 7.3.1.1.1 (UL 0_0, C-RNTI, no SUL) after the FDRA
_UL00 = [("tda_index", 4), ("freq_hop", 1), ("mcs", 5), ("ndi", 1), ("rv", 2), ("harq_pid", 4), ("tpc", 2)]


def _riv(riv, n):
    length = riv // n + 1
    start = riv % n
    if start + length > n:  # RIV encoding for (L-1) > N/2
        length = n - (riv // n) + 1
        start = n - 1 - (riv % n)
    return start, length


def reinterpret(rec, bwp_size):
    """Return the fields re-read from rec['dci_bits'] with the given bwp_size, or None if the bits do not fit the format."""
    n = int(rec["dci_len"])
    bits = bin(int(rec["dci_bits"], 16))[2:].zfill(len(rec["dci_bits"]) * 4)[:n]
    fdra = math.ceil(math.log2(bwp_size * (bwp_size + 1) / 2))
    tail = _UL00 if rec.get("dir") == "UL" else _DL10
    need = 1 + fdra + sum(w for _, w in tail)
    # shortcut: UL 0_0 only on exact length (its dci_format code is not fixed in-tree), add the code when it is
    if (rec.get("dir") == "UL" and n != need) or n < need:
        return None
    pos = 1  # identifier bit
    out = {}
    out["start_rb_bwp"], out["nb_rb"] = _riv(int(bits[pos:pos + fdra], 2), bwp_size)
    pos += fdra
    for name, w in tail:
        out[name] = int(bits[pos:pos + w], 2)
        pos += w
    return out


def reinterpret_file(path):
    """Yield (dci_record, fields) for every 1_0/0_0 DCI whose cfg_ref names a config record seen earlier."""
    cfg = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("type") == "config":
                cfg[(r["rnti"], r["cfg_ref"])] = r
            elif r.get("type") == "dci" and r.get("cfg_ref") is not None and (r.get("dci_format") == 1 or r.get("dir") == "UL"):
                c = cfg.get((r["rnti"], r["cfg_ref"])) or cfg.get((0, r["cfg_ref"]))
                if c and c["bwp_size"] is not None:
                    yield r, reinterpret(r, c["bwp_size"])


if __name__ == "__main__":
    for rec, fields in reinterpret_file(sys.argv[1]):
        print(json.dumps({"rnti": rec["rnti"], "cfg_ref": rec["cfg_ref"], "fields": fields}))
