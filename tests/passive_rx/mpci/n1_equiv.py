#!/usr/bin/env python3
"""N=1 equivalence of a candidate build against a reference build (multi-PCI plan Task A9, gates V2a / V3 post-merge check).

Log level only: parses the run logs of an N=1 replay or rfsim run (several runs per arm) and compares candidate against reference.
  extract RUN... -o ref.json --host-class H [--label L]    store the parsed reference runs (a main / mpci-main binary, same host class)
  compare --ref RUN|ref.json... --cand RUN... --host-class H [--bed replay|rfsim] [--min-n 2] [--out PREFIX]
RUN is a run directory (run.log | rx.log | base_r1/rx/rx.log | rx/rx.log, optional time.txt = /usr/bin/time -v, optional gate_rc) or a log file.
A reference or candidate on another host class than --host-class is refused (exit 3): results are never merged across hosts.

Two kinds of difference, kept apart (task A9):
  STRUCTURAL  a line template (numbers, hex ids, timestamps, [cK] tags normalised) present in EVERY candidate run and in NO reference run
              (or the reverse); CORE_PLAN lines; the CORE_POOL pool set ("CORE_POOL: N cores: ..."), the grammar of the CORE_POOL lines
              (their number-stripped signature: key set and order of the statistics line, lease / wait / cell lines).
  METRIC      CRC ok %, decoded grants, ok RNTI count, scan-queue drop %, check_bad, PDTIMPCT p50/p99, DMRS_SCAN lines, peak RSS, gate
              result: candidate must stay within the run-to-run spread of the reference (tolerance = max(range ref, range cand, floor)).
              Only a worse candidate is DRIFT; a better one is reported. Needs n >= --min-n runs per arm (else exit 3, "INSUFFICIENT").
SYNC_SCAN ms is MEASURED and reported (never hidden), WARN above 1.5x the reference mean, never DRIFT by itself.
Intended N=1 changes are whitelisted in WHITELIST below, each with its reason; a rule that was not exercised is listed as unused.
Exit: 0 equivalent, 1 drift, 3 refused / insufficient data."""
import argparse
import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import cellsel  # noqa: E402
import mpci_runsum  # noqa: E402

# --- known intended N=1 changes (each with its reason); extend only with an explained diff --------------------------------------------
WHITELIST = [
    dict(id="A10-pin-log", kind="template", side="cand", rx=r"SENSING: MPCI_SYNC cell=N want=N got=N",
         reason="A10: the cell-identity pin logs 'MPCI_SYNC ... locked' on every lock when the replay carries a cells.json descriptor PCI "
                "(a receiver without inventory prints none). Pure logging."),
    dict(id="A10-pin-notfound", kind="template", side="cand", rx=r"SENSING: MPCI_SYNC cell=N want=N not found",
         reason="A10: rate-limited (1 per 10 s) 'not found in this search' while the pinned cell is not yet seen. Pure logging."),
    dict(id="A10-sync-scan-ms", kind="measured", key="sync_scan_ms",
         reason="A10: the pinned PSS search keeps up to 3 peaks, only on already-pinned paths (RX_RECOVER full fallback, MAC resync): a cheap SSS "
                "on one N_ID_1 per candidate. Must be MEASURED (SYNC_SCAN ms table row), not hidden; WARN above 1.5x the reference mean."),
    dict(id="B6-admit-pool", kind="template", side="cand", rx=r"MPCI_ADMIT_POOL",
         reason="B6: pool-demand admission line (N cells); at N=1 it is informational and changes no decision."),
    dict(id="B6-dmrs-token", kind="template", side="cand", rx=r"CORE_POOL.*dmrs",
         reason="B6: the DMRS_SCAN token offer is active at N=1 (extra pool statistics fields / lease names); it changes no decision "
                "(checked: dmrs_scan_lines within spread)."),
    dict(id="AUDIT-8MB-rss", kind="rss_allow", mb=8.0,
         reason="audit cleanup: the DCI layout resolvers (8 MB per cell) are allocated earlier (at cell prealloc, not at first use): peak RSS may "
                "rise by up to 8 MB x 1 cell, on top of the run-to-run spread."),
]

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TS = re.compile(r"^\d+(?:\.\d+)? ")
HEX = re.compile(r"0x[0-9a-fA-F]+")
NUM = re.compile(r"(?:(?<=[\s=(\[:])[-+]|(?<!\d)-)?\d+(?:\.\d+)?")  # a leading sign is data, not structure ("a+b" keeps its +)
NLIST = re.compile(r"(?:N[ ,]){2,}N")
MAXT = 150
ARGV = re.compile(r'^\S*"\s*$|^\S*" "')
# build identity that differs between any two builds and says nothing about behaviour: worktree/run paths, branch name and hash of the Version line
BUILDID = re.compile(r"/home/\S*?/(?:wt|ota_runs)/\S+|Branch: \S+|Hash: \S+")
# lines whose field set depends on the data (which counters are non-zero): one template per prefix
FIELDSET = re.compile(r"^(.*?SENSING: (?:PDSCH_SOLVER why|SEGIDXC?|PDSCHQ per-rnti|SEARCH_SPACE INFERRED|BOOTTABLE))\b.*")


def norm(line):
    """Line template: colour, harness timestamp and [cK] tag stripped, hex ids -> H, numbers -> N, number lists -> N+, cut at MAXT."""
    _, l = cellsel.split(line)
    l = ANSI.sub("", l)
    if ARGV.match(l):  # a wrapped fragment of the shutdown echo of the command line: where it wraps depends on the build path length
        return ""
    l = TS.sub("", l, count=1).strip()
    l = BUILDID.sub("B", l)
    l = HEX.sub("H", l)
    l = NUM.sub("N", l)
    l = NLIST.sub("N+", l)
    l = re.sub(r"\s+", " ", l)
    l = FIELDSET.sub(r"\1 ...", l)
    return l[:MAXT]


def _num(s):
    return float(s)


def parse_log(lines):
    """-> profile dict of one run (all JSON-able)."""
    tpl, plan, pool_sigs, pool_cores, lease_names = {}, [], {}, None, set()
    stats_keys = None
    sync_scan, dmrs = [], 0
    for raw in lines:
        l = ANSI.sub("", raw.rstrip("\n"))
        if "CORE_PLAN" not in l and "CORE_POOL" not in l:      # those two have their own sections (exact lines / grammar)
            t = norm(l)
            if t:
                tpl[t] = tpl.get(t, 0) + 1
        if "SENSING:" not in l:
            continue
        if "CORE_PLAN" in l:
            plan.append(re.sub(r"host=\S+", "host=H", l[l.find("SENSING: CORE_PLAN"):]).strip())
        elif "SENSING: CORE_POOL" in l:
            s = l[l.find("SENSING: CORE_POOL"):]
            m = re.match(r"SENSING: CORE_POOL: (\d+) cores:((?: \d+)*)\s*$", s)
            if m:
                pool_cores = sorted(int(x) for x in m.group(2).split())
                continue
            sig = NUM.sub("N", s)
            pool_sigs[sig] = pool_sigs.get(sig, 0) + 1
            m = re.match(r"SENSING: CORE_POOL long lease (\S+) ", s)
            if m:
                lease_names.add(m.group(1))
            elif s.startswith("SENSING: CORE_POOL n="):
                stats_keys = re.findall(r"([A-Za-z_]+\d*)(?:=|\[)", s)    # header keys + class blocks, in order
        elif "SENSING: SYNC_SCAN" in l:
            m = re.search(r"SYNC_SCAN ms=([\d.]+)", l)
            if m:
                sync_scan.append(float(m.group(1)))
        elif "SENSING: DMRS_SCAN" in l:
            dmrs += 1
    return dict(templates=tpl, core_plan=plan, pool_sigs=pool_sigs, pool_cores=pool_cores, lease_names=sorted(lease_names),
                stats_keys=stats_keys, sync_scan_ms=sync_scan, dmrs_scan_lines=dmrs)


def metrics_of(path, prof):
    r = mpci_runsum.summ(path)
    q = r["q"] or (0, 0, 0, 0, 0)
    m = {}
    m["decoded"] = float(q[1])
    m["crc_ok_pct"] = 100.0 * q[2] / q[1] if q[1] else 0.0
    m["drop_full_pct"] = 100.0 * q[3] / q[0] if q[0] else 0.0
    m["rnti_ok"] = float(r["rnti_ok"])
    m["check_bad_max"] = float(max([v[1] for v in r["check_bad"].values()] or [0]))
    if r["pdtim"]:
        m["pdtim_sum_p50_us"], m["pdtim_sum_p99_us"] = float(r["pdtim"][1]), float(r["pdtim"][3])
    m["dmrs_scan_lines"] = float(prof["dmrs_scan_lines"])
    s = prof["sync_scan_ms"]
    if s:
        m["sync_scan_ms"] = sum(s) / len(s)
    return m


def find_log(p):
    if os.path.isfile(p):
        return p
    for rel in ("run.log", "rx.log", "base_r1/rx/rx.log", "rx/rx.log"):
        if os.path.exists(os.path.join(p, rel)):
            return os.path.join(p, rel)
    return None


def _up(lg, p):
    """directories that may hold time.txt / gate_rc of a run: the log's directory and its parents up to the run directory p."""
    d, out = os.path.dirname(os.path.abspath(lg)), []
    top = os.path.abspath(p if os.path.isdir(p) else os.path.dirname(p))
    while True:
        out.append(d)
        if d == top or d == os.path.dirname(d):
            return out
        d = os.path.dirname(d)


def rss_mb(dirs):
    for d in dirs:
        t = os.path.join(d, "time.txt")
        if os.path.exists(t):
            for l in open(t, errors="replace"):
                m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", l)
                if m:
                    return int(m.group(1)) / 1024.0
    return None


def load_run(p):
    """Run directory or log file -> run profile."""
    lg = find_log(p)
    if lg is None:
        raise SystemExit("n1_equiv: no run log under %s" % p)
    lines = cellsel.read_lines(lg, text=True)
    prof = parse_log(lines)
    prof["metrics"] = metrics_of(lg, prof)
    dirs = _up(lg, p)
    rss = rss_mb(dirs)
    if rss is not None:
        prof["metrics"]["peak_rss_mb"] = rss
    for d in dirs:
        g = os.path.join(d, "gate_rc")
        if os.path.exists(g):
            prof["metrics"]["gate_pass"] = 1.0 if open(g).read().strip() == "0" else 0.0
            break
    prof["path"] = p
    return prof


def load_arm(paths):
    runs, hc = [], None
    for p in paths:
        if p.endswith(".json") and os.path.isfile(p):
            d = json.load(open(p))
            if hc and d.get("host_class") != hc:
                raise SystemExit("n1_equiv: mixed host classes in one arm")
            hc = d.get("host_class")
            runs += d["runs"]
        else:
            runs.append(load_run(p))
    return runs, hc


# --- comparison -----------------------------------------------------------------------------------------------------------------
# key: (label, worse direction 'low'|'high'|'both'|'any', abs floor, rel floor)
METRICS = [
    ("gate_pass", "low", 0.0, 0.0),
    ("crc_ok_pct", "low", 0.5, 0.0),
    ("decoded", "low", 0.0, 0.05),
    ("rnti_ok", "low", 2.0, 0.10),
    ("drop_full_pct", "high", 0.5, 0.0),
    ("check_bad_max", "high", 0.0, 0.0),
    ("pdtim_sum_p50_us", "high", 100.0, 0.30),
    ("pdtim_sum_p99_us", "high", 200.0, 0.30),
    ("dmrs_scan_lines", "both", 5.0, 0.25),
    ("peak_rss_mb", "high", 0.0, 0.01),
]


def stat(vals):
    return (sum(vals) / len(vals), min(vals), max(vals)) if vals else None


def whitelisted_template(t, side, used):
    for w in WHITELIST:
        if w["kind"] == "template" and w["side"] == side and re.search(w["rx"], t):
            used.add(w["id"])
            return w
    return None


def compare(ref, cand, min_n=2, bed="replay"):
    """-> dict(rows=[...], structural=[...], verdict, exit). Pure function on run profiles."""
    out = dict(structural=[], rows=[], info=[], whitelist_used=[], notes=[])
    used = set()
    n_r, n_c = len(ref), len(cand)
    # structure: templates present in every run of one arm and in no run of the other
    def tset(runs):
        return [set(r["templates"]) for r in runs]
    rs, cs = tset(ref), tset(cand)
    r_all, c_all = set.intersection(*rs), set.intersection(*cs)
    r_any, c_any = set.union(*rs), set.union(*cs)
    only_c, only_r = sorted(c_all - r_any), sorted(r_all - c_any)
    sporadic = len((c_any - c_all) | (r_any - r_all))
    out["info"].append("templates: ref %d (always %d), cand %d (always %d), sporadic (not in every run of an arm) %d" %
                       (len(r_any), len(r_all), len(c_any), len(c_all), sporadic))
    for side, lst in (("cand", only_c), ("ref", only_r)):
        for t in lst:
            w = whitelisted_template(t, side, used)
            if w:
                out["structural"].append(("WHITELISTED[%s]" % w["id"], "template only in %s: %s" % (side, t)))
            else:
                out["structural"].append(("DRIFT", "template only in %s (every run): %s" % (side, t)))
    # CORE_PLAN: identical line sets
    pr, pc = [sorted(set(r["core_plan"])) for r in ref], [sorted(set(r["core_plan"])) for r in cand]
    if any(x != pr[0] for x in pr[1:]):
        out["notes"].append("CORE_PLAN differs between reference runs")
    for tag, a, b in (("only in cand", pc[0], pr[0]), ("only in ref", pr[0], pc[0])):
        for l in sorted(set(a) - set(b)):
            out["structural"].append(("DRIFT", "CORE_PLAN line %s: %s" % (tag, l)))
    if any(x != pc[0] for x in pc[1:]):
        out["structural"].append(("DRIFT", "CORE_PLAN differs between candidate runs"))
    if not pr[0] and not pc[0]:
        out["info"].append("CORE_PLAN: no line in either arm")
    # CORE_POOL pool set, stats-line key list, grammar signatures
    pcr, pcc = {tuple(r["pool_cores"] or ()) for r in ref}, {tuple(r["pool_cores"] or ()) for r in cand}
    if len(pcr) > 1 or len(pcc) > 1 or pcr != pcc:
        out["structural"].append(("DRIFT", "CORE_POOL pool set ref %s vs cand %s" % (sorted(pcr), sorted(pcc))))
    else:
        out["info"].append("CORE_POOL pool set identical: %s" % (list(next(iter(pcr))),))
    skr, skc = {tuple(r["stats_keys"] or ()) for r in ref}, {tuple(r["stats_keys"] or ()) for r in cand}
    if skr != skc:
        extra = sorted(set(sum(map(list, skc), [])) - set(sum(map(list, skr), [])))
        miss = sorted(set(sum(map(list, skr), [])) - set(sum(map(list, skc), [])))
        w = None
        if extra and not miss:
            w = whitelisted_template("CORE_POOL " + " ".join(extra), "cand", used)
        if w:
            out["structural"].append(("WHITELISTED[%s]" % w["id"], "CORE_POOL statistics line gained fields %s" % extra))
        else:
            out["structural"].append(("DRIFT", "CORE_POOL statistics line key list differs (cand extra %s, missing %s)" % (extra, miss)))
    else:
        out["info"].append("CORE_POOL statistics line key list identical (%d keys)" % len(next(iter(skr))))
    stats_ok = skr == skc or any(o[0].startswith("WHITELISTED") and "statistics line" in o[1] for o in out["structural"])
    skip = (lambda s: s.startswith("SENSING: CORE_POOL n=")) if stats_ok else (lambda s: False)   # whitelisted gain: not compared twice
    sgr = {s for r in ref for s in r["pool_sigs"]}
    sgc = {s for r in cand for s in r["pool_sigs"]}
    # signatures that are always present in all runs of an arm and absent from the other (sporadic lease/wait lines ignored)
    s_all_r = set.intersection(*[set(r["pool_sigs"]) for r in ref])
    s_all_c = set.intersection(*[set(r["pool_sigs"]) for r in cand])
    if stats_ok and skr != skc:
        s_all_r, s_all_c = {s for s in s_all_r if not skip(s)}, {s for s in s_all_c if not skip(s)}
    for side, lst in (("cand", sorted(s_all_c - sgr)), ("ref", sorted(s_all_r - sgc))):
        for s in lst:
            w = whitelisted_template(s, side, used)
            out["structural"].append(("WHITELISTED[%s]" % w["id"], "CORE_POOL grammar only in %s: %s" % (side, s[:100])) if w
                                     else ("DRIFT", "CORE_POOL grammar only in %s: %s" % (side, s[:160])))
    # metrics
    insufficient = min(n_r, n_c) < min_n
    for key, bad, floor, rel in METRICS:
        a = [r["metrics"][key] for r in ref if key in r["metrics"]]
        b = [r["metrics"][key] for r in cand if key in r["metrics"]]
        if not a or not b:
            if a or b:
                out["rows"].append((key, stat(a), stat(b), None, "DRIFT" if (key != "peak_rss_mb") else "n/a", "metric present in one arm only"))
            continue
        sr, sc = stat(a), stat(b)
        tol = max(sr[2] - sr[1], sc[2] - sc[1], floor, rel * abs(sr[0]))
        note = "tol %.3g" % tol
        if key == "peak_rss_mb":
            allow = sum(w["mb"] for w in WHITELIST if w["kind"] == "rss_allow")
            if allow:
                used.update(w["id"] for w in WHITELIST if w["kind"] == "rss_allow")
                tol += allow
                note = "tol %.3g incl. AUDIT-8MB-rss allowance %.0f MB" % (tol, allow)
        d = sc[0] - sr[0]
        if bad == "low":
            worse = d < -tol
        elif bad == "high":
            worse = d > tol
        else:
            worse = abs(d) > tol
        if key == "gate_pass":
            worse = sc[0] < sr[0] - 1e-9
            note = "gate PASS fraction (rfsim_regress.sh --gate); both failing = baseline gate unreachable (plan R2), compare metrics"
        v = "DRIFT" if worse else ("better" if (bad == "low" and d > tol) or (bad == "high" and d < -tol) else "within spread")
        if insufficient and v != "DRIFT":
            v = "INSUFFICIENT n"
        out["rows"].append((key, sr, sc, d, v, note))
    # SYNC_SCAN: measured, never hidden
    a = [r["metrics"]["sync_scan_ms"] for r in ref if "sync_scan_ms" in r["metrics"]]
    b = [r["metrics"]["sync_scan_ms"] for r in cand if "sync_scan_ms" in r["metrics"]]
    if a and b:
        sr, sc = stat(a), stat(b)
        used.add("A10-sync-scan-ms")
        out["rows"].append(("sync_scan_ms (A10, MEASURED)", sr, sc, sc[0] - sr[0], "WARN" if sc[0] > 1.5 * sr[0] else "measured", "per-run mean of SYNC_SCAN ms"))
    else:
        out["rows"].append(("sync_scan_ms (A10, MEASURED)", stat(a), stat(b), None, "no data", "no SYNC_SCAN line in one arm"))
    out["whitelist_used"] = sorted(used)
    out["notes"].append("arms: ref n=%d, cand n=%d (min %d)" % (n_r, n_c, min_n))
    drift = any(s[0] == "DRIFT" for s in out["structural"]) or any(r[4] == "DRIFT" for r in out["rows"])
    insuf = insufficient or any(r[4] == "INSUFFICIENT n" for r in out["rows"])
    out["verdict"] = "DRIFT" if drift else ("INSUFFICIENT" if insuf else "EQUIVALENT")
    out["exit"] = 1 if drift else (3 if insuf else 0)
    return out


def fmt_stat(s):
    return "-" if s is None else "%.4g [%.4g..%.4g]" % s


def render(res, ref_name="ref", cand_name="cand"):
    L = ["N=1 EQUIVALENCE  %s (reference) vs %s (candidate)  bed/host: see header" % (ref_name, cand_name), ""]
    L += ["STRUCTURE"] + (["  %-24s %s" % x for x in res["structural"]] or ["  none: template set, CORE_PLAN, CORE_POOL grammar and pool set agree"])
    L += ["  " + i for i in res["info"]] + [""]
    L += ["METRICS (ref mean [min..max] | cand | delta | verdict | note)"]
    for key, sr, sc, d, v, note in res["rows"]:
        L.append("  %-28s %-26s %-26s %-10s %-14s %s" % (key, fmt_stat(sr), fmt_stat(sc), "-" if d is None else "%+.4g" % d, v, note))
    L += ["", "WHITELIST (intended N=1 changes)"]
    for w in WHITELIST:
        L.append("  %-18s %-8s %s%s" % (w["id"], "USED" if w["id"] in res["whitelist_used"] else "unused", w["reason"], ""))
    L += [""] + ["  " + n for n in res["notes"]] + ["VERDICT: %s (exit %d)" % (res["verdict"], res["exit"])]
    return "\n".join(L)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract")
    e.add_argument("runs", nargs="+"); e.add_argument("-o", required=True); e.add_argument("--host-class", required=True)
    e.add_argument("--label", default="")
    c = sub.add_parser("compare")
    c.add_argument("--ref", nargs="+", required=True); c.add_argument("--cand", nargs="+", required=True)
    c.add_argument("--host-class", required=True); c.add_argument("--bed", default="replay", choices=("replay", "rfsim"))
    c.add_argument("--min-n", type=int, default=2); c.add_argument("--out", default="")
    a = ap.parse_args(argv)
    if a.cmd == "extract":
        runs = [load_run(p) for p in a.runs]
        for r in runs:
            r.pop("path", None)
        with open(a.o, "w") as f:
            json.dump(dict(host_class=a.host_class, label=a.label, runs=runs), f)
        print("n1_equiv: stored %d reference run(s) -> %s" % (len(runs), a.o))
        return 0
    ref, hr = load_arm(a.ref)
    cand, hc = load_arm(a.cand)
    for who, h in (("reference", hr), ("candidate", hc)):
        if h is not None and h != a.host_class:
            print("n1_equiv: REFUSED: %s is host class %r, this comparison is %r (never merged across hosts)" % (who, h, a.host_class), file=sys.stderr)
            return 3
    res = compare(ref, cand, a.min_n, a.bed)
    txt = render(res)
    print("bed=%s host_class=%s" % (a.bed, a.host_class)); print(txt)
    if a.out:
        open(a.out + ".txt", "w").write("bed=%s host_class=%s\n%s\n" % (a.bed, a.host_class, txt))
        json.dump(res, open(a.out + ".json", "w"), indent=1, default=str)
    return res["exit"]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
