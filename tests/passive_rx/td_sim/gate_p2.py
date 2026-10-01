#!/usr/bin/env python3
"""P2 gate runner (levers Task 7, spec 2026-10-01 section 5.4). All numbers are SIMULATED (nr_td_sim).

  gate_p2.py run --sim nr_td_sim --matrix gate_p2.json --raw RAWDIR [-j 8] [--only-oracle 0|1]
  gate_p2.py summarize --matrix gate_p2.json --raw RAWDIR --out OUTDIR --label LABEL

Each (cell, rx, oracle, dims) config is split into chunks; a chunk gets one seed shared by both arms (paired channel
draws, truth and engine RNG). Raw output: RAWDIR/<arm>/<job>.jsonl (simulator stdout). summarize writes OUTDIR/summary.md
and OUTDIR/per_config.jsonl (one aggregate line per config and arm)."""
import argparse, itertools, json, os, statistics, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor


def jobs(m):
    d = m["dims"]
    keys = list(d)
    out, ci = [], 0
    for cell in m["cells"]:
        for rx in m["rx"]:
            for oracle, oc in sorted(m["oracle"].items()):
                for vals in itertools.product(*(d[k] for k in keys)):
                    ci += 1
                    dims = dict(zip(keys, vals))
                    n, chunk = oc["acq"], oc.get("chunk", oc["acq"])
                    for c in range((n + chunk - 1) // chunk):
                        acq = min(chunk, n - c * chunk)
                        name = "%s_rx%d_o%s_" % (cell, rx, oracle) + "_".join("%s%s" % (k, v) for k, v in dims.items()) + "_c%d" % c
                        out.append(dict(name=name, cell=cell, rx=rx, oracle=int(oracle), dims=dims, acq=acq, seed=ci * 1000 + c + 1))
    return out


def cmd(sim, m, arm, j):
    flags = dict(m.get("common", {}), **m["arms"][arm], **m["cells"][j["cell"]], **j["dims"], oracle=j["oracle"])
    c = [sim, "--acq", str(j["acq"]), "--seed", str(j["seed"]), "--n-rx", str(j["rx"])]
    for k, v in flags.items():
        c += ["--" + k.replace("_", "-"), str(v)]
    return c


def run(a, m):
    todo = []
    for j in jobs(m):
        if a.only_oracle is not None and j["oracle"] != a.only_oracle:
            continue
        for arm in m["arms"]:
            f = os.path.join(a.raw, arm, j["name"] + ".jsonl")
            if not os.path.exists(f):
                todo.append((arm, j, f))
    # heaviest first: blind, 1 RX, low SNR, big catalogue
    todo.sort(key=lambda t: (t[1]["oracle"], -t[1]["dims"]["catalog_tda"], t[1]["rx"], t[1]["dims"]["p_true_snr_mu"]))
    for arm in m["arms"]:
        os.makedirs(os.path.join(a.raw, arm), exist_ok=True)
    t0 = time.time()

    def one(t):
        arm, j, f = t
        p = subprocess.run(cmd(a.sim, m, arm, j), capture_output=True, text=True, check=True)
        with open(f + ".tmp", "w") as fh:
            fh.write(p.stdout)
        os.replace(f + ".tmp", f)
        return f

    with ThreadPoolExecutor(a.j) as ex:
        for i, f in enumerate(ex.map(one, todo)):
            if i % 50 == 0:
                print("%d/%d %.0f s %s" % (i + 1, len(todo), time.time() - t0, os.path.basename(f)), flush=True)
    print("done %d jobs in %.0f s" % (len(todo), time.time() - t0))


def med(v):
    return statistics.median(v) if v else float("nan")


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def summarize(a, m):
    arms = list(m["arms"])
    p1, p2 = arms
    groups = {}   # (cell, rx, oracle) -> list of paired records
    per_cfg = []
    for j in jobs(m):
        recs = {}
        for arm in arms:
            f = os.path.join(a.raw, arm, j["name"] + ".jsonl")
            lines = [json.loads(l) for l in open(f) if l.startswith("{")]
            recs[arm] = {(r["acq"], r["rnti_rank"]): r for r in lines if "summary" not in r}
            s = [r for r in lines if "summary" in r][0]["summary"]
            per_cfg.append(dict(job=j["name"], arm=arm, cell=j["cell"], rx=j["rx"], oracle=j["oracle"], seed=j["seed"], **j["dims"],
                                **{k: s[k] for k in ("rntis", "decided", "wrong", "undecidable", "mean_s", "median_s", "n_full", "n_probe",
                                                     "p2_admitted_fail", "truth_eliminated_by_probe", "twins_min")}))
        g = groups.setdefault((j["cell"], j["rx"], j["oracle"]), [])
        for k in recs[p1]:
            g.append((j, recs[p1][k], recs[p2][k]))
    with open(os.path.join(a.out, "per_config.jsonl"), "w") as fh:
        for r in per_cfg:
            fh.write(json.dumps(r) + "\n")

    L = []
    w = L.append
    w("# P2 failure-only probe evidence: simulator gate\n")
    w("Label: `%s`. Arms paired per RNTI (same seed, channel draws, truth, engine RNG seed; only `--p2` differs). "
      "Matrix: `tests/passive_rx/td_sim/gate_p2.json`; runner `gate_p2.py`. Undecidable = hit the 3600 s grant cap "
      "(the simulator does not model probation withdrawal). Paired-decided = RNTIs decided in BOTH arms; time/trial "
      "comparisons are on that set (uncensored).\n" % a.label)
    w("## Per (cell, rx, oracle)\n")
    w("| cell | rx | oracle | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | truth_elim P2 | same winner | differ (both decided) | P1 dec only | P2 dec only | paired-decided | median grants P1/P2 | mean s P1/P2 | median truth full-TB P1/P2 | n_full/RNTI P1/P2 | admitted probe FAILs P2 |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    verdict = {}
    for key in sorted(groups, key=lambda k: (k[0], -k[1], -k[2])):
        g = groups[key]
        n = len(g)
        wr1 = sum(x[1]["wrong"] for x in g); wr2 = sum(x[2]["wrong"] for x in g)
        u1 = sum(x[1]["undecidable"] for x in g); u2 = sum(x[2]["undecidable"] for x in g)
        te = sum(x[2]["truth_elim"] for x in g)
        same = sum(x[1]["winner"] == x[2]["winner"] for x in g)
        both = [x for x in g if not x[1]["undecidable"] and not x[2]["undecidable"]]
        diff = sum(x[1]["winner"] != x[2]["winner"] for x in both)
        o1 = sum(not x[1]["undecidable"] and x[2]["undecidable"] for x in g)
        o2 = sum(x[1]["undecidable"] and not x[2]["undecidable"] for x in g)
        mg1, mg2 = med([x[1]["grants"] for x in both]), med([x[2]["grants"] for x in both])
        ms1, ms2 = mean([x[1]["seconds"] for x in both]), mean([x[2]["seconds"] for x in both])
        tf1, tf2 = med([x[1]["truth_full"] for x in both]), med([x[2]["truth_full"] for x in both])
        nf1, nf2 = mean([x[1]["n_full"] for x in both]), mean([x[2]["n_full"] for x in both])
        adm = sum(x[2]["p2_admitted_fail"] for x in g)
        w("| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %.0f / %.0f | %.2f / %.2f | %.0f / %.0f | %.0f / %.0f | %d |" % (
            key[0], key[1], key[2], n, wr1, wr2, u1, u2, te, same, diff, o1, o2, len(both), mg1, mg2, ms1, ms2, tf1, tf2, nf1, nf2, adm))
        verdict[key] = dict(wrong=wr2 == 0 and wr1 == 0, truth_elim=te == 0, same_winner=diff == 0 and o1 == 0 and o2 == 0,
                            truth_trials=tf2 <= tf1 and mg2 <= mg1, reduced=nf2 < nf1 and ms2 < ms1, undec=u2 <= u1)
    w("\n## Per-criterion verdict per (cell, rx, oracle) (spec 5.4; PASS/FAIL)\n")
    w("Criteria: wrong = 0 in both arms; truth_eliminated_by_probe = 0; same winner for every paired RNTI (differ = 0 and no "
      "one-sided decision); true hypothesis not slower (median truth full-TB decodes and median grants-to-convergence P2 <= P1); "
      "full-TB decodes and convergence time reduced (mean, paired-decided); undecidable not increased.\n")
    w("| cell | rx | oracle | wrong=0 | truth_elim=0 | same winner | truth not slower | full-TB+time reduced | undecidable not up |")
    w("|---|---|---|---|---|---|---|---|---|")
    pf = lambda b: "PASS" if b else "FAIL"
    for key in sorted(verdict, key=lambda k: (k[0], -k[1], -k[2])):
        v = verdict[key]
        w("| %s | %d | %d | %s | %s | %s | %s | %s | %s |" % (key[0], key[1], key[2], pf(v["wrong"]), pf(v["truth_elim"]), pf(v["same_winner"]),
                                                       pf(v["truth_trials"]), pf(v["reduced"]), pf(v["undec"])))
    for o in sorted({k[2] for k in verdict}, reverse=True):
        ok = all(all(v.values()) for k, v in verdict.items() if k[2] == o)
        w("\n**oracle %d overall: %s**" % (o, "PASS" if ok else "FAIL"))
    # where do wrong winners / undecidables come from
    w("\n## Wrong winners and undecidables by dimension (summed over cells and rx)\n")
    for dim in m["dims"]:
        w("\n| oracle | %s | RNTIs | wrong P1 | wrong P2 | undec P1 | undec P2 | mean s P1/P2 (paired-decided) |" % dim)
        w("|---|---|---|---|---|---|---|---|")
        agg = {}
        for key, g in groups.items():
            for x in g:
                agg.setdefault((key[2], x[0]["dims"][dim]), []).append(x)
        for (o, v), g in sorted(agg.items(), key=lambda t: (-t[0][0], t[0][1])):
            both = [x for x in g if not x[1]["undecidable"] and not x[2]["undecidable"]]
            w("| %d | %s | %d | %d | %d | %d | %d | %.2f / %.2f |" % (o, v, len(g), sum(x[1]["wrong"] for x in g), sum(x[2]["wrong"] for x in g),
                                                         sum(x[1]["undecidable"] for x in g), sum(x[2]["undecidable"] for x in g),
                                                         mean([x[1]["seconds"] for x in both]), mean([x[2]["seconds"] for x in both])))
    # Wrong-winner diagnostics (P2 arm). Admitted-fail share of the truth = (truth KL trials - truth full-TB decodes) /
    # truth KL trials; valid without pruning (oracle 0, prior off because fieldbook 1). CORRELATIONAL, not a causal test.
    w("\n## Wrong P2 winners: identity and truth admitted-fail share (oracle 0, decided RNTIs; correlational)\n")
    pairs, shw, shc = {}, [], []
    for key, g in groups.items():
        if key[2] != 0:
            continue
        for x in g:
            r = x[2]
            if r["undecidable"] or x[0]["dims"].get("twins") != 2:
                continue
            sh = (r["truth_kl_trials"] - r["truth_full"]) / max(1, r["truth_kl_trials"])
            (shw if r["wrong"] else shc).append(sh)
            if r["wrong"]:
                k2 = (r["truth_table"], int(r["winner"].rsplit("/", 1)[1]))
                pairs[k2] = pairs.get(k2, 0) + 1
    w("| (truth table, winner table) | wrong P2 RNTIs |")
    w("|---|---|")
    for k2 in sorted(pairs):
        w("| %s | %d |" % (k2, pairs[k2]))
    w("\nMedian share of the truth's KL trials that are admitted probe FAILs (twins 2): wrong RNTIs %.2f (n %d), correct RNTIs "
      "%.2f (n %d)." % (med(shw), len(shw), med(shc), len(shc)))
    with open(os.path.join(a.out, "summary.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")
    print("\n".join(L))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["run", "summarize"])
    ap.add_argument("--sim")
    ap.add_argument("--matrix", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--out")
    ap.add_argument("--label", default="SIMULATED, nr_td_sim")
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--only-oracle", type=int)
    a = ap.parse_args(argv)
    m = json.load(open(a.matrix))
    if a.mode == "run":
        run(a, m)
    else:
        os.makedirs(a.out, exist_ok=True)
        summarize(a, m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
