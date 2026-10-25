#!/usr/bin/env python3
"""Lab only: score receiver DCI/TB outcomes against the gNB scheduler (validation ground truth, never receiver input).

  python3 groundtruth.py --rx DIR (lab export) (--gnb-log gnb.log | --gnb-csv sched.csv) [--out DIR/groundtruth]

gNB source 1: OAI MAC LOG_D "[DLSCH/PDSCH/PUCCH]" (gNB_scheduler_dlsch.c:1243; needs --log_config.nr_mac_log_level debug).
This is what the MAC SCHEDULED, not what the PHY transmitted. Source 2: CSV with columns frame,slot,rnti[,dir].
Join key (frame, slot, rnti) on SFN-unwrapped slots; the two clocks differ by an unknown whole number of SFN cycles
(the receiver's hyperframe is process-local), estimated as the modal offset over key-equal pairs.
"""
import argparse
import bisect
import collections
import csv
import datetime
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
            g = {"frame": int(r["frame"]), "slot": int(r["slot"]), "dir": (r.get("dir") or "dl").strip(),
                 "rnti": int(v, 16) if v.lower().startswith("0x") else int(v)}
            for k in ("kind", "t", "dci_frame", "dci_slot"):  # optional columns of the OCUDU SCHED export (match_clock)
                if r.get(k):
                    g[k] = r[k].strip()
            out.append(g)
    return out


def _utc(s):
    return datetime.datetime.fromisoformat(s).replace(tzinfo=datetime.timezone.utc).timestamp()


def match_clock(rx_dci, rx_grants, gnb, spf=20, window_s=2.0):
    """Per-RNTI DL/UL DCI recall/precision and PDSCH TB CRC, joined on (frame, slot, rnti) inside a wall-clock window.

    match() assumes ONE slot offset between the clocks for the whole run; the receiver's slot counter jumps at every
    RX recovery (stream restart + re-sync), after which match() scores every later DCI as a miss and a false alarm.
    Here a receiver DCI matches a gNB row with the same (frame mod 1024, slot, rnti) whose time stamp lies within
    window_s of the DCI's UTC once the modal receiver-gNB clock offset (5 ms bins) is removed: the 10.24 s SFN period
    makes the key unique inside the window. A UL grant is keyed by its DCI slot (dci_frame/dci_slot columns).
    Needs the gNB rows' "t" column (OCUDU SCHED export) and the receiver's utc_ns; returns None without them."""
    rows = []
    for x in gnb:
        if "t" not in x or x.get("kind") not in (None, "pdsch", "pusch"):
            continue
        d = x.get("dir", "dl")
        fr, sl = (int(x["dci_frame"]), int(x["dci_slot"])) if d == "ul" and "dci_frame" in x and "dci_slot" in x else (x["frame"], x["slot"])
        rows.append(((d, fr % 1024, sl, x["rnti"]), _utc(x["t"])))
    dci = [r for r in rx_dci if r.get("utc_ns") and r["rnti"] != 0xFFFF]
    if not rows or not dci:
        return None
    by = collections.defaultdict(list)
    for k, t in rows:
        by[k].append(t)
    for v in by.values():
        v.sort()
    key = lambda r: (r["dir"], r["frame"] % 1024, r["slot"], r["rnti"])
    votes = collections.Counter()
    for r in dci[:50000]:
        for t in by.get(key(r), ()):
            votes[round((r["utc_ns"] / 1e9 - t) * 200) / 200] += 1
    if not votes:
        return None
    off = votes.most_common(1)[0][0]
    t0 = max(min(r["utc_ns"] for r in dci) / 1e9 - off, min(t for _, t in rows))
    t1 = min(max(r["utc_ns"] for r in dci) / 1e9 - off, max(t for _, t in rows))
    used = collections.defaultdict(set)  # gNB row index (per key) already matched
    per = collections.defaultdict(lambda: {"gnb": 0, "tp": 0, "fp": 0, "tb_ok": 0, "tb_fail": 0})
    dci_of = {}  # (ext_slot, rnti, dir) of a receiver DCI -> matched gNB key, for the PDSCH outcome join
    for r in sorted(dci, key=lambda r: r["utc_ns"]):
        t = r["utc_ns"] / 1e9 - off
        if not t0 <= t <= t1:
            continue
        k = key(r)
        ts = by.get(k, [])
        i = bisect.bisect_left(ts, t - window_s)
        hit = None
        while i < len(ts) and ts[i] <= t + window_s:
            if i not in used[k]:
                hit = i
                break
            i += 1
        st = per[(r["dir"], r["rnti"])]
        if hit is None:
            st["fp"] += 1  # includes a duplicate accept of one scheduled grant
        else:
            used[k].add(hit)
            st["tp"] += 1
            dci_of[(r.get("ext_slot"), r["rnti"], r["dir"])] = k
    for k, t in rows:
        if t0 <= t <= t1:
            per[(k[0], k[3])]["gnb"] += 1
    for g in rx_grants:
        if g.get("dir", "dl") != "dl" or g.get("probe") or g.get("outcome") not in ("crc_ok", "crc_fail"):
            continue
        if (g.get("ext_slot"), g["rnti"], "dl") in dci_of:
            per[("dl", g["rnti"])]["tb_ok" if g["outcome"] == "crc_ok" else "tb_fail"] += 1
    out = {"clock_offset_s": off, "offset_votes": votes.most_common(1)[0][1], "window_s": window_s, "per_rnti": {}}
    for (d, rnti), st in sorted(per.items(), key=lambda kv: -kv[1]["gnb"]):
        if not st["gnb"] and st["tp"] + st["fp"] < 20:
            continue  # stray false alarms of unscheduled RNTIs are summed below, not listed
        st = dict(st)
        st["recall"] = st["tp"] / st["gnb"] if st["gnb"] else None
        st["precision"] = st["tp"] / (st["tp"] + st["fp"]) if st["tp"] + st["fp"] else None
        if d == "dl":
            n = st["tb_ok"] + st["tb_fail"]
            st["pdsch_crc"] = st["tb_ok"] / n if n else None
            st["tb_success_over_gnb_grants"] = st["tb_ok"] / st["gnb"] if st["gnb"] else None
        out["per_rnti"]["%s 0x%04x" % (d, rnti)] = st
    out["stray_fp"] = {d: sum(st["fp"] for (dd, r), st in per.items() if dd == d and not st["gnb"]) for d in ("dl", "ul")}
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
    clk = match_clock(store.read_dataset(a.rx, "dci_attempts"), store.read_dataset(a.rx, "rx_grants"), gnb,
                      man.get("slots_per_frame", 20))
    if clk is not None:
        res["clock_matched"] = clk
        for name, st in clk["per_rnti"].items():
            print("groundtruth[clock]: %s gnb=%d tp=%d fp=%d recall=%s precision=%s%s" % (
                name, st["gnb"], st["tp"], st["fp"], "%.3f" % st["recall"] if st["recall"] is not None else "-",
                "%.4f" % st["precision"] if st["precision"] is not None else "-",
                " pdsch_crc=%s (%d/%d)" % ("%.3f" % st["pdsch_crc"] if st.get("pdsch_crc") is not None else "-", st["tb_ok"],
                                             st["tb_ok"] + st["tb_fail"]) if name.startswith("dl") else ""))
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
