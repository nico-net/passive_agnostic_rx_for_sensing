#!/usr/bin/env python3
"""BC6 reduced blind-convergence gate runner (matrix: gate_bc.json). [SIMULATED] nr_td_sim only.
usage: gate_bc.py --sim SIM --matrix gate_bc.json --out DIR [--jobs 8] [--acq N] [--oracle1-acq N] [--skip-rx1-none]
Writes DIR/rows.json (per-cell aggregates), DIR/summary.md, DIR/results.jsonl (per-RNTI, tagged)."""
import argparse, json, os, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor


def med(v):
    return statistics.median(v) if v else float("nan")


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def cmd_for(sim, m, flags, acq, rx, tdd):
    c = [sim, "--acq", str(acq), "--seed", str(m.get("seed", 1)), "--n-rx", str(rx)]
    if tdd:
        c += ["--tdd", tdd]
    for k, v in flags.items():
        c += ["--" + k.replace("_", "-"), str(v)]
    return c


def aggregate(recs):
    summ = next(r["summary"] for r in recs if "summary" in r)
    rn = [r for r in recs if "summary" not in r]
    ok = [r for r in rn if not r["undecidable"]]
    cold = [r["seconds"] for r in ok if r["rnti_rank"] < 2]
    st = [r["seconds"] for r in ok if r["rnti_rank"] >= 2]
    return {
        "rntis": len(rn), "cold_mean": mean(cold), "cold_median": med(cold), "steady_mean": mean(st), "steady_median": med(st),
        "wrong": summ["wrong"], "wrong_pins": summ.get("wrong_pins", 0), "undecidable": summ["undecidable"],
        "crc_accepts": summ.get("crc_accepts", 0), "geom_pins": summ.get("geom_pins", 0), "sib_blocks": summ.get("sib_blocks", 0),
        "geom_blocks": summ.get("geom_blocks", 0), "crc_wrong": summ.get("crc_wrong", 0),
        "crc_bound": summ.get("crc_bound", 0), "geom_bound": summ.get("geom_bound", 0),
        "fail_opens": summ.get("fail_opens", 0), "recovery_grants": summ.get("recovery_grants", -1),
        "recovery_rntis": summ.get("recovery_rntis", -1), "recovery_never": summ.get("recovery_never", 0),
        "injected": summ.get("injected", 0), "inject_skipped": summ.get("inject_skipped", 0),
        "cert_pass": summ.get("cert_pass", 0), "pass_all": summ.get("pass_all", 0),
        "cert_share": summ["cert_pass"] / summ["pass_all"] if summ.get("pass_all") else float("nan"),
        "tdd_excl_removed": summ.get("tdd_excl_removed", 0), "truth_excluded": summ.get("truth_excluded", 0),
        "k0_trap_passes": summ.get("k0_trap_passes", 0), "false_passes": summ.get("false_passes", 0),
        "certified_wrong": summ.get("certified_wrong", 0),
        "n_full": summ["n_full"], "n_probe": summ.get("n_probe", 0),
    }, rn


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True); ap.add_argument("--matrix", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=8); ap.add_argument("--acq", type=int); ap.add_argument("--oracle1-acq", type=int)
    ap.add_argument("--skip-rx1-none", action="store_true")
    a = ap.parse_args(argv)
    m = json.load(open(a.matrix))
    acq = a.acq or m["acq"]
    os.makedirs(a.out, exist_ok=True)
    jobs = []
    for arm, af in m["arms"].items():
        for tn, tdd in m["tdd"].items():
            for rx in m["rx"]:
                if a.skip_rx1_none and rx == 1 and tn == "none":
                    continue
                for orc in m["oracle"]:
                    n = a.oracle1_acq if (orc == 1 and a.oracle1_acq) else acq
                    jobs.append((dict(arm=arm, tdd=tn, rx=rx, oracle=orc, acq=n), dict(m["common"], **af, oracle=orc), tdd))
    for arm, af in m.get("stress", {}).items():
        af = dict(af); bound = af.pop("bound")
        jobs.append((dict(arm=arm, tdd="none", rx=m.get("stress_rx", 4), oracle=af["oracle"], acq=acq, bound=bound), dict(m["common"], **af), None))

    def run(j):
        tag, flags, tdd = j
        p = subprocess.run(cmd_for(a.sim, m, flags, tag["acq"], tag["rx"], tdd), capture_output=True, text=True, check=True)
        return [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]

    if subprocess.run(["pgrep", "-x", "nr-uesoftmodem"], capture_output=True).returncode == 0:
        sys.exit("nr-uesoftmodem is running; refusing to start")
    with ThreadPoolExecutor(max(1, a.jobs)) as ex:
        res = list(ex.map(run, jobs))
    rows = []
    with open(os.path.join(a.out, "results.jsonl"), "w") as rf:
        for (tag, _, _), recs in zip(jobs, res):
            agg, rn = aggregate(recs)
            rows.append(dict(tag, **agg))
            for r in rn:
                rf.write(json.dumps(dict(r, arm=tag["arm"], tdd=tag["tdd"], rx=tag["rx"], oracle=tag["oracle"])) + "\n")
    json.dump(rows, open(os.path.join(a.out, "rows.json"), "w"), indent=1)
    cols = ["arm", "tdd", "rx", "oracle", "acq", "rntis", "cold_mean", "cold_median", "steady_mean", "steady_median", "wrong", "wrong_pins",
            "undecidable", "crc_accepts", "geom_pins", "sib_blocks", "fail_opens", "recovery_grants", "recovery_rntis", "cert_share",
            "tdd_excl_removed", "truth_excluded", "crc_wrong", "crc_bound", "geom_bound"]
    f = lambda x: ("%.3g" % x) if isinstance(x, float) else str(x)
    with open(os.path.join(a.out, "summary.md"), "w") as sf:
        sf.write("[SIMULATED, nr_td_sim] cold = first two RNTIs per acquisition, steady = later; seconds; undecidable RNTIs censored.\n\n")
        sf.write("| " + " | ".join(cols) + " |\n|" + "---|" * len(cols) + "\n")
        for r in rows:
            sf.write("| " + " | ".join(f(r[c]) for c in cols) + " |\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
