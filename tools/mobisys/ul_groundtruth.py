#!/usr/bin/env python3
"""UL ground truth (lab only): the receiver's UL DCIs (DCI 0_x rows of a lab export, dci_attempts dir=ul) against the gNB's UL
grants, plus the receiver's PUSCH CRC per RNTI from its log. groundtruth.py scores DL only (match() keeps dir == "dl"); this is
its UL companion and leaves groundtruth.py unchanged.

gNB input: a CSV with frame,slot,rnti,dir and, for dir=ul rows, the DCI slot as dci_frame,dci_slot (ocudu_gt.py writes both; the
PUSCH slot is frame,slot). Without dci_frame/dci_slot the PUSCH slot is matched, which is wrong by k2: say so with --pusch-slot.
The UL DCI is matched at its PDCCH slot (the receiver's dci_attempts frame/slot is the PDCCH slot). The SFN offset is the modal
offset over key-equal pairs (the receiver's hyperframe is process-local), as in groundtruth.py.
gNB data is validation ground truth only; nothing here feeds the receiver.

usage: ul_groundtruth.py --rx LABDIR --gnb-csv sched.csv [--rx-log rx.log] [--pusch-slot] [--out DIR]
writes DIR/ul_summary.json (default LABDIR/groundtruth/ul_summary.json)."""
import argparse, collections, csv, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse  # noqa: E402
import store  # noqa: E402

_ULCRC = re.compile(r"UL_PUSCH_CRC rnti=0x([0-9a-fA-F]+) ok=(\d) mcs=(\d+) prb=(\d+)\+(\d+)")
_KV = re.compile(r"\b(probe|nl|layers|tbl)=(\d+)")


def read_gnb_ul(path, pusch_slot=False):
    out = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if (r.get("dir") or "dl").strip() != "ul":
                continue
            v = r["rnti"].strip()
            rnti = int(v, 16) if v.lower().startswith("0x") else int(v)
            fr, sl = r["frame"], r["slot"]
            if not pusch_slot:
                if r.get("dci_frame") in (None, "") or r.get("dci_slot") in (None, ""):
                    continue  # no DCI slot known for this row (msg3 / RAR-granted): not a DCI 0_x
                fr, sl = r["dci_frame"], r["dci_slot"]
            out.append({"frame": int(fr), "slot": int(sl), "rnti": rnti, "mcs": r.get("mcs") or None,
                        "layers": r.get("layers") or None, "rb": r.get("rb") or None})
    return out


def rx_ul_crc(path):
    per = collections.defaultdict(lambda: {"n": 0, "ok": 0, "probe": 0, "mcs": collections.Counter(), "prb_n": collections.Counter(),
                                           "layers": collections.Counter()})
    if not path:
        return {}
    with open(path, errors="replace") as f:
        for line in f:
            if "UL_PUSCH_CRC" not in line:
                continue
            m = _ULCRC.search(parse.ANSI.sub("", line))
            if not m:
                continue
            kv = dict(_KV.findall(line))
            p = per["0x%04x" % int(m.group(1), 16)]
            if kv.get("probe", "0") != "0":
                p["probe"] += 1
                continue
            p["n"] += 1
            p["ok"] += int(m.group(2))
            p["mcs"][m.group(3)] += 1
            p["prb_n"][m.group(5)] += 1
            p["layers"][kv.get("nl") or kv.get("layers") or "?"] += 1
    return {k: {"pusch_real": v["n"], "crc_ok": v["ok"], "crc_pct": round(100 * v["ok"] / v["n"], 2) if v["n"] else None,
                "probe": v["probe"], "mcs_top": dict(v["mcs"].most_common(5)), "prb_n_top": dict(v["prb_n"].most_common(5)),
                "layers": dict(v["layers"])} for k, v in sorted(per.items())}


def match_ul(rx_dci, gnb, spf=20):
    un = parse.SfnUnwrap(spf)
    g = [dict(x, ext=un(x["frame"], x["slot"])) for x in gnb]
    dci = [r for r in rx_dci if r["dir"] == "ul"]
    by_key = collections.defaultdict(list)
    for i, x in enumerate(g):
        by_key[(x["frame"], x["slot"], x["rnti"])].append(i)
    votes = collections.Counter(r["ext_slot"] - g[i]["ext"] for r in dci for i in by_key.get((r["frame"], r["slot"], r["rnti"]), ()))
    off, nv = votes.most_common(1)[0] if votes else (0, 0)
    res = {"offset_slots": off, "offset_votes": nv, "gnb_ul_grants": 0, "rx_ul_dci": len(dci), "tp": 0, "fp": 0, "fn": 0, "per_rnti": {}}
    if not g or not dci:
        return res
    lo = max(min(r["ext_slot"] for r in dci), min(x["ext"] for x in g) + off)
    hi = min(max(r["ext_slot"] for r in dci), max(x["ext"] for x in g) + off)
    free = collections.defaultdict(list)
    for i, x in enumerate(g):
        if lo <= x["ext"] + off <= hi:
            free[(x["ext"] + off, x["rnti"])].append(i)
    per = collections.defaultdict(lambda: {"gnb": 0, "tp": 0, "fp": 0})
    for x in g:
        if lo <= x["ext"] + off <= hi:
            per["0x%04x" % x["rnti"]]["gnb"] += 1
    for r in dci:
        if not lo <= r["ext_slot"] <= hi:
            continue
        k = "0x%04x" % r["rnti"]
        if free.get((r["ext_slot"], r["rnti"])):
            free[(r["ext_slot"], r["rnti"])].pop(0)
            per[k]["tp"] += 1
        else:
            per[k]["fp"] += 1
    tp = sum(v["tp"] for v in per.values()); fp = sum(v["fp"] for v in per.values()); n = sum(v["gnb"] for v in per.values())
    res.update(gnb_ul_grants=n, tp=tp, fp=fp, fn=n - tp, span_slots=hi - lo + 1,
               recall=round(tp / n, 4) if n else None, precision=round(tp / (tp + fp), 4) if tp + fp else None,
               per_rnti={k: dict(v, recall=round(v["tp"] / v["gnb"], 4) if v["gnb"] else None) for k, v in sorted(per.items())})
    return res


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rx", required=True)
    ap.add_argument("--gnb-csv", required=True)
    ap.add_argument("--rx-log")
    ap.add_argument("--pusch-slot", action="store_true", help="the CSV has no dci_frame/dci_slot: match at the PUSCH slot (off by k2)")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    with open(os.path.join(a.rx, "manifest.json")) as f:
        man = json.load(f)
    if man.get("mode") != "lab":
        sys.stderr.write("ul_groundtruth: needs a lab export\n")
        return 2
    res = match_ul(store.read_dataset(a.rx, "dci_attempts"), read_gnb_ul(a.gnb_csv, a.pusch_slot), man.get("slots_per_frame", 20))
    res["rx_pusch_crc"] = rx_ul_crc(a.rx_log)
    res["provenance"] = man.get("provenance")
    out = a.out or os.path.join(a.rx, "groundtruth")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "ul_summary.json"), "w") as f:
        json.dump(res, f, indent=1, sort_keys=True)
    print("ul_groundtruth: gnb_ul=%d tp=%d fp=%d fn=%d offset=%d" % (res["gnb_ul_grants"], res["tp"], res["fp"], res["fn"], res["offset_slots"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
