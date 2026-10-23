#!/usr/bin/env python3
"""Lab only: score receiver DCI/TB outcomes against the gNB scheduler (validation ground truth, never receiver input).

  python3 groundtruth.py --rx DIR (lab export) (--gnb-log gnb.log | --gnb-csv sched.csv) [--out DIR/groundtruth]

gNB source 1: OAI MAC LOG_D "[DLSCH/PDSCH/PUCCH]" (gNB_scheduler_dlsch.c:1243; needs --log_config.nr_mac_log_level debug).
This is what the MAC SCHEDULED, not what the PHY transmitted. Source 2: CSV with columns frame,slot,rnti[,dir].
Join key (frame, slot, rnti) on SFN-unwrapped slots; the two clocks differ by an unknown whole number of SFN cycles
(the receiver's hyperframe is process-local), estimated as the modal offset over key-equal pairs.
"""
import argparse
import collections
import csv
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse  # noqa: E402
import store  # noqa: E402

# "%4d.%2d [DLSCH/PDSCH/PUCCH] RNTI %04x DCI L %d start %3d RBs %3d ... MCS %2d nrOfLayers %d TBS %4d HARQ PID %2d round %d RV %d NDI %d"
_GNB = re.compile(r"(\d+)\.\s*(\d+)\s+\[DLSCH/PDSCH/PUCCH\] RNTI ([0-9a-fA-F]+) DCI L (\d+) start\s+(\d+) RBs\s+(\d+) "
                  r"startSymbol\s+(\d+) nb_symbol\s+(\d+) dmrspos\s+\w+ MCS\s+(\d+) nrOfLayers (\d+) TBS\s+(\d+) "
                  r"HARQ PID\s+(\d+) round (\d+)")


def parse_gnb_line(line):
    m = _GNB.search(line)
    if not m:
        return None
    return {"frame": int(m.group(1)), "slot": int(m.group(2)), "rnti": int(m.group(3), 16), "dir": "dl",
            "mcs": int(m.group(9)), "rb_n": int(m.group(6)), "tbs": int(m.group(11)), "round": int(m.group(13))}


def read_gnb_log(path):
    with open(path, errors="replace") as f:
        return [g for g in map(parse_gnb_line, f) if g]


def read_gnb_csv(path):
    out = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            v = r["rnti"].strip()
            out.append({"frame": int(r["frame"]), "slot": int(r["slot"]), "dir": (r.get("dir") or "dl").strip(),
                        "rnti": int(v, 16) if v.lower().startswith("0x") else int(v)})
    return out


def wilson(k, n, z=1.96):
    if not n:
        return (None, None)
    p = k / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return ((c - h) / d, (c + h) / d)


def match(rx_dci, rx_grants, gnb, spf=20):
    """rx_dci: dci_attempts rows; rx_grants: rx_grants rows; gnb: dicts with frame/slot/rnti/dir (file order)."""
    un = parse.SfnUnwrap(spf)
    g = [dict(x, ext=un(x["frame"], x["slot"])) for x in gnb if x.get("dir", "dl") == "dl"]
    dci = [r for r in rx_dci if r["dir"] == "dl"]
    by_key = collections.defaultdict(list)
    for i, x in enumerate(g):
        by_key[(x["frame"], x["slot"], x["rnti"])].append(i)
    votes = collections.Counter(r["ext_slot"] - g[i]["ext"] for r in dci for i in by_key.get((r["frame"], r["slot"], r["rnti"]), ()))
    off = votes.most_common(1)[0][0] if votes else 0
    out = {"offset_slots": off, "offset_votes": votes.most_common(1)[0][1] if votes else 0}
    if not g or not dci:
        out.update(_metrics([], 0, 0, 0))
        return out, []
    lo = max(min(r["ext_slot"] for r in dci), min(x["ext"] for x in g) + off)  # span both sides observed
    hi = min(max(r["ext_slot"] for r in dci), max(x["ext"] for x in g) + off)
    free = collections.defaultdict(list)  # (ext, rnti) -> unmatched gNB indices
    for i, x in enumerate(g):
        if lo <= x["ext"] + off <= hi:
            free[(x["ext"] + off, x["rnti"])].append(i)
    grants = collections.defaultdict(list)
    for r in rx_grants:
        if r.get("dir", "dl") == "dl" and not r.get("probe"):
            grants[(r["ext_slot"], r["rnti"])].append(r["outcome"])
    cls, fp = [], 0
    matched = set()
    for r in sorted((r for r in dci if lo <= r["ext_slot"] <= hi), key=lambda r: r["ext_slot"]):
        cand = free.get((r["ext_slot"], r["rnti"]))
        if cand:
            matched.add(cand.pop(0))
        else:
            fp += 1  # includes a duplicate accept of one scheduled grant
    for i, x in enumerate(g):
        if not lo <= x["ext"] + off <= hi:
            continue
        key = (x["ext"] + off, x["rnti"])
        if i not in matched:
            c = "missed_tb"
        else:
            o = grants.get(key)
            c = "dropped_undecidable" if not o else ("correct_tb" if o.pop(0) == "crc_ok" else "dci_ok_tb_failed")
        cls.append({"frame": x["frame"], "slot": x["slot"], "rnti": x["rnti"], "class": c})
    out.update(_metrics(cls, fp, hi - lo + 1, len(matched)))
    return out, cls


def _metrics(cls, fp, span_slots, tp):
    n = len(cls)
    cnt = collections.Counter(c["class"] for c in cls)
    fn = n - tp
    prec = tp / (tp + fp) if tp + fp else None
    rec = tp / n if n else None
    f1 = 2 * prec * rec / (prec + rec) if prec and rec else (0.0 if prec is not None and rec is not None else None)
    return {"gnb_grants": n, "tp_dci": tp, "fp_dci": fp, "fn_dci": fn, "precision": prec, "recall": rec, "f1": f1,
            "false_alarms_per_1000_slots": 1000.0 * fp / span_slots if span_slots else None,
            "classes": {k: cnt.get(k, 0) for k in ("correct_tb", "dci_ok_tb_failed", "missed_tb", "dropped_undecidable")},
            "tb_success_over_gnb_grants": cnt.get("correct_tb", 0) / n if n else None,  # misses stay in the denominator
            "recall_ci95": wilson(tp, n), "span_slots": span_slots,
            "label": "gNB MAC log = scheduled grants (not PHY-emitted); mismatches may be gNB-side non-transmissions"}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rx", required=True)
    ap.add_argument("--gnb-log")
    ap.add_argument("--gnb-csv")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    with open(os.path.join(a.rx, "manifest.json")) as f:
        man = json.load(f)
    if man.get("mode") != "lab":
        sys.stderr.write("groundtruth: needs a lab export (per-grant rows do not exist in commercial mode)\n")
        return 2
    gnb = read_gnb_log(a.gnb_log) if a.gnb_log else read_gnb_csv(a.gnb_csv)
    res, cls = match(store.read_dataset(a.rx, "dci_attempts"), store.read_dataset(a.rx, "rx_grants"), gnb,
                     man.get("slots_per_frame", 20))
    out = a.out or os.path.join(a.rx, "groundtruth")
    os.makedirs(out, exist_ok=True)
    res["provenance"] = man.get("provenance")
    for name, write in (("summary.json", lambda f: json.dump(res, f, indent=1, sort_keys=True)),
                        ("matches.csv", lambda f: _csv(f, cls))):
        tmp = os.path.join(out, "." + name + ".tmp")
        with open(tmp, "w", newline="") as f:
            write(f)
        os.replace(tmp, os.path.join(out, name))
    print("groundtruth: gnb_grants=%d tp=%d fp=%d fn=%d" % (res["gnb_grants"], res["tp_dci"], res["fp_dci"], res["fn_dci"]))
    return 0


def _csv(f, rows):
    w = csv.DictWriter(f, fieldnames=["frame", "slot", "rnti", "class"])
    w.writeheader()
    w.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
