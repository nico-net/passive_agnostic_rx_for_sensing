#!/usr/bin/env python3
"""CB0 elimination-channel gate runner (matrix: gate_elim.json). [SIMULATED] nr_td_sim only.
usage: flock /tmp/td_measure.lock gate_elim.py --sim SIM --matrix gate_elim.json --out DIR [--jobs 8] [--acq N] [--only-supp]
Per cell: first = RNTI 1 of an acquisition, later = RNTIs 2-4; cold = RNTIs 1-2, steady = 3-4 (BC6 convention); seconds of decided RNTIs
(undecidable censored). Writes DIR/rows.json and DIR/summary.md (per-RNTI jsonl is not kept)."""
import argparse, json, os, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor


def med(v):
    return statistics.median(v) if v else float("nan")


def p95(v):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, max(0, -(-95 * len(v) // 100) - 1))]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def cmd_for(sim, seed, flags, acq, rx, tdd):
    c = [sim, "--acq", str(acq), "--seed", str(seed), "--n-rx", str(rx)]
    if tdd:
        c += ["--tdd", tdd]
    for k, v in flags.items():
        c += ["--" + k, str(v)]
    return c


def aggregate(recs):
    summ = next(r["summary"] for r in recs if "summary" in r)
    rn = [r for r in recs if "summary" not in r]
    ok = [r for r in rn if not r["undecidable"]]
    sel = lambda f: [r["seconds"] for r in ok if f(r["rnti_rank"])]
    first, later, cold, steady = sel(lambda k: k == 0), sel(lambda k: k >= 1), sel(lambda k: k < 2), sel(lambda k: k >= 2)
    return {
        "rntis": len(rn), "first_mean": mean(first), "first_median": med(first), "first_p95": p95(first), "later_mean": mean(later),
        "later_median": med(later), "later_p95": p95(later),
        "cold_mean": mean(cold), "cold_median": med(cold), "steady_mean": mean(steady), "steady_median": med(steady),
        "wrong": summ["wrong"], "wrong_pins": summ.get("wrong_pins", 0), "undecidable": summ["undecidable"],
        "cb0_per_grant": summ.get("cb0_per_grant", 0.0), "cb0_decodes": summ.get("cb0_decodes", 0), "cb0_grants": summ.get("cb0_grants", 0),
        "cb0_inadmissible": summ.get("cb0_inadmissible", 0), "truth_cb0_elim": summ.get("truth_cb0_elim", 0),
        "cb0_bound": summ.get("cb0_bound", 0.0), "false_passes": summ.get("false_passes", 0), "cb0_false_passes": summ.get("cb0_false_passes", 0),
        "fail_opens": summ.get("fail_opens", 0), "k0_trap_passes": summ.get("k0_trap_passes", 0), "n_full": summ["n_full"],
        "proc_grants": summ.get("proc_grants", 0), "cb0_alarms": summ.get("cb0_alarms", 0), "cb0_rejected": summ.get("cb0_rejected", 0),
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True); ap.add_argument("--matrix", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=8); ap.add_argument("--acq", type=int); ap.add_argument("--only-supp", action="store_true")
    a = ap.parse_args(argv)
    m = json.load(open(a.matrix))
    acq = a.acq or m["acq"]
    os.makedirs(a.out, exist_ok=True)

    def cell(arm, fb, tdd, rho, rx, orc, extra=None, name=None, snr_rho=0.0):
        flags = dict(m["common"], **m["arms"][arm], **m["fb"][fb], persist=rho, oracle=orc)
        flags["snr-rho"] = snr_rho
        flags.update(extra or {})
        tag = dict(cell=name or "main", arm=arm, fb=fb, tdd=tdd, rho=rho, snr_rho=flags["snr-rho"], rx=rx, oracle=orc, acq=acq)
        return tag, flags, m["tdd"][tdd]

    jobs = []
    if not a.only_supp:
        for arm in m["arms"]:
            for fb in m["fb"]:
                for tdd in m["tdd"]:
                    for rho in m["rho"]:
                        for rx in m["rx"]:
                            for orc in m["oracle"]:
                                for sr in m.get("snr_rho", [0.0]):
                                    jobs.append(cell(arm, fb, tdd, rho, rx, orc, snr_rho=sr))
    for name, s in m.get("supp", {}).items():
        jobs.append(cell(s["arm"], s["fb"], s["tdd"], s["rho"], s["rx"], s["oracle"], s.get("flags"), name, s.get("snr_rho", 0.0)))
    # slowest first (blind, 1 RX, cb0 off)
    jobs.sort(key=lambda j: (j[0]["oracle"], j[0]["arm"] != "off", -1 * (j[0]["rx"] == 1)))

    def run(j):
        tag, flags, tdd = j
        p = subprocess.run(cmd_for(a.sim, m.get("seed", 1), flags, tag["acq"], tag["rx"], tdd), capture_output=True, text=True, check=True)
        r = aggregate([json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")])
        with open(os.path.join(a.out, "cells.jsonl"), "a") as cf:  # incremental: a killed campaign keeps its finished cells
            cf.write(json.dumps(dict(tag, **r)) + "\n")
        return r

    ota = [l for l in subprocess.run(["pgrep", "-a", "-x", "nr-uesoftmodem"], capture_output=True, text=True).stdout.splitlines() if "--rfsim" not in l]
    if ota:  # an OTA receiver (X410) is running: never load the host under it
        sys.exit("OTA nr-uesoftmodem is running; refusing to start")
    with ThreadPoolExecutor(max(1, min(8, a.jobs))) as ex:
        res = list(ex.map(run, jobs))
    rows = [dict(t, **r) for (t, _, _), r in zip(jobs, res)]
    rows.sort(key=lambda r: (r["cell"] != "main", r["cell"], r["fb"], r["oracle"], r["tdd"], r["rho"], r["snr_rho"], -r["rx"], r["arm"]))
    json.dump(rows, open(os.path.join(a.out, "rows.json"), "w"), indent=1)
    cols = ["cell", "arm", "fb", "oracle", "tdd", "rho", "snr_rho", "rx", "acq", "first_mean", "first_median", "first_p95", "later_mean",
            "later_median", "later_p95", "cold_median", "steady_median", "wrong", "wrong_pins", "undecidable", "truth_cb0_elim", "cb0_alarms",
            "cb0_per_grant", "cb0_inadmissible", "cb0_rejected", "fail_opens", "cb0_bound"]
    f = lambda x: ("%.3g" % x) if isinstance(x, float) else str(x)
    with open(os.path.join(a.out, "summary.md"), "w") as sf:
        sf.write("[SIMULATED, nr_td_sim] first = RNTI 1 of an acquisition, later = RNTIs 2-4, cold = 1-2, steady = 3-4; seconds of decided RNTIs.\n\n")
        sf.write("| " + " | ".join(cols) + " |\n|" + "---|" * len(cols) + "\n")
        for r in rows:
            sf.write("| " + " | ".join(f(r[c]) for c in cols) + " |\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
