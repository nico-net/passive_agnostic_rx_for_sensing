#!/usr/bin/env python3
"""Traffic-mode field/capability scorer. Usage: score_fields.py [--json] [--agg] <campaign dir | run dir>...
Per run and cell (records: sense_records_last[.cellN].jsonl; cell -> PCI from the last ISAC_METRICS in run.log) and per
direction: distinct RNTIs (>=2 DCIs), DCI records per format / RNTI class, layout_state, layout/fields evidence,
share of records with each field non-null, nb_rb buckets, MCS histogram + table, layers, decode CRC-OK per probe flag.
Counts only (flat Counter), so runs of the SAME cell/arm may be summed (--agg); never sum across cells/hosts."""
import sys, os, json, re, collections

FIELDS = ("harq_pid", "ndi", "rv", "mcs", "mcs_table", "qm", "tbs", "k1", "dai", "tpc", "pri", "start_rb", "nb_rb",
          "start_sym", "nb_sym", "tda_index", "nl", "dmrs_symb_pos", "dmrs_scrambling_id", "dmrs_type", "dmrs_add_pos",
          "dmrs_n_scid", "k2", "k0", "srs_req", "csi_req")
FMT = {("DL", 0): "1_1", ("DL", 1): "1_0", ("UL", 2): "0_1", ("UL", 3): "0_0"}  # nr_traffic_record.h
CLS = {0: "C", 1: "TC", 2: "SI", 3: "RA", 4: "P"}

def bucket(n):
    return "<16" if n < 16 else "<64" if n < 64 else "<128" if n < 128 else ">=128"

def score_records(lines):
    """-> flat dict of counts (key 'DIR|what|value'); 'max|...' keys are maxima, 'rnti2|DIR' distinct RNTIs with >=2 DCIs."""
    c = collections.Counter(); mx = {}; per = collections.defaultdict(collections.Counter)
    for ln in lines:
        if '"type":"dci"' not in ln and '"type":"decode"' not in ln: continue
        try: d = json.loads(ln)
        except ValueError: continue
        D = d.get("dir")
        if d.get("type") == "decode":
            c["%s|dec|%s|%s" % (D, "probe" if d.get("probe") else "check", "ok" if d.get("crc") == 1 else "fail")] += 1
            continue
        c[D + "|dci"] += 1; per[D][d.get("rnti")] += 1
        f = FMT.get((D, d.get("dci_format")), "fmt%s" % d.get("dci_format"))
        c["%s|fmt|%s" % (D, f)] += 1
        cl = d.get("rnti_class")
        if cl is not None: c["%s|cls|%s" % (D, CLS.get(cl, cl))] += 1
        c["%s|state|%s" % (D, d.get("layout_state"))] += 1
        c["%s|levid|%s" % (D, d.get("layout_evidence"))] += 1
        c["%s|fev|%s" % (D, d.get("fields_evidence"))] += 1
        c["%s|tev|%s" % (D, d.get("table_evidence"))] += 1
        for k in FIELDS:
            if d.get(k) is not None: c["%s|nn.%s|%s" % (D, f, k)] += 1
        nb = d.get("nb_rb")
        if nb is not None:
            c["%s|nbrb|%s" % (D, bucket(nb))] += 1; mx["max|%s|nbrb" % D] = max(mx.get("max|%s|nbrb" % D, 0), nb)
        if d.get("mcs") is not None: c["%s|mcs|%d" % (D, d["mcs"])] += 1
        if d.get("mcs_table") is not None: c["%s|mcstab|%s" % (D, d["mcs_table"])] += 1
        if d.get("nl") is not None: c["%s|nl|%s" % (D, d["nl"])] += 1
    out = dict(c); out.update(mx)
    for D, cnt in per.items():
        out["rnti1|" + D] = len(cnt); out["rnti2|" + D] = sum(1 for v in cnt.values() if v >= 2)
    return out

def merge(a, b):
    for k, v in b.items(): a[k] = max(a.get(k, 0), v) if k.startswith("max|") else a.get(k, 0) + v
    return a

def metrics_last(log):
    last = {}
    for ln in open(log, errors="replace"):
        i = ln.find("ISAC_METRICS {")
        if i < 0: continue
        try: d = json.loads(ln[i + 13:])
        except ValueError: continue
        last[d.get("cell", 0)] = d
    return last

def score_run(rd):
    """-> {pci: stats + run-level metric keys (m|...)}"""
    res = {}
    for cell, m in sorted(metrics_last(os.path.join(rd, "run.log")).items()):
        f = os.path.join(rd, "sense_records_last.jsonl" if cell == 0 else "sense_records_last.cell%d.jsonl" % cell)
        if not os.path.exists(f): continue
        s = score_records(open(f, errors="replace"))
        q = m.get("scanq_queued", 0)
        s["m|drop_pct"] = 100.0 * sum(m.get("scanq_drop_" + k, 0) for k in ("full", "stale", "epoch")) / max(q, 1)
        for k in ("pdschq_decoded", "pdschq_crc_ok", "pusch_try", "pusch_crc_ok", "e3_phys_solved", "e3_fam_accept", "e3_scored"):
            s["m|" + k] = m.get(k, 0)
        try: s["m|cpu"] = float(open(os.path.join(rd, "cpu_cores.txt")).read())
        except (OSError, ValueError): pass
        s["runs"] = 1
        res[m.get("pci", cell)] = s
    return res

def runs_of(path):
    tsv = os.path.join(path, "runs.tsv")
    if not os.path.exists(tsv): return [(os.path.basename(path), path)]
    out = []
    for ln in open(tsv):
        p = ln.rstrip("\n").split("\t")
        if len(p) >= 4 and p[3] == "OK": out.append((p[0], p[1]))
    return out

def show(tag, pci, s):
    print("== %s PCI %s" % (tag, pci))
    for D in ("DL", "UL"):
        n = s.get(D + "|dci", 0)
        print("  %s dci=%d rnti>=2=%d rnti(all)=%d max_nb_rb=%s" % (D, n, s.get("rnti2|" + D, 0), s.get("rnti1|" + D, 0), s.get("max|%s|nbrb" % D)))
        for w in ("fmt", "cls", "state", "levid", "fev", "nbrb", "nl", "mcstab", "dec"):
            items = sorted((k.split("|", 2)[2], v) for k, v in s.items() if k.startswith("%s|%s|" % (D, w)))
            if items: print("    %-6s %s" % (w, " ".join("%s=%d" % kv for kv in items)))
        for f in sorted({k.split("|")[1][3:] for k in s if k.startswith(D + "|nn.")}):
            nf = s.get("%s|fmt|%s" % (D, f), 0)
            print("    nonnull%% %s (n=%d) %s" % (f, nf, " ".join("%s=%.0f" % (k, 100.0 * s.get("%s|nn.%s|%s" % (D, f, k), 0) / max(nf, 1)) for k in FIELDS)))
        mc = sorted((int(k.split("|")[2]), v) for k, v in s.items() if k.startswith(D + "|mcs|"))
        if mc: print("    mcs    %s" % " ".join("%d:%d" % kv for kv in mc))
    print("  " + " ".join("%s=%.2f" % (k[2:], v) for k, v in sorted(s.items()) if k.startswith("m|")))

def main(argv):
    js, agg = "--json" in argv, "--agg" in argv
    paths = [a for a in argv if not a.startswith("--")]
    allr = collections.OrderedDict()
    for p in paths:
        for tag, rd in runs_of(p):
            for pci, s in score_run(rd).items():
                key = (os.path.basename(p.rstrip("/")), re.sub(r"\d+", "", tag), pci) if agg else (tag, rd, pci)
                allr[key] = merge(allr.get(key, {}), s)
    for key, s in allr.items():
        if js: print(json.dumps({"key": list(key), **s}))
        else: show(" ".join(map(str, key[:-1])), key[-1], s)

if __name__ == "__main__":
    main(sys.argv[1:])
