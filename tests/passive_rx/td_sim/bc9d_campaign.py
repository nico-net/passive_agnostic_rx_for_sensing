#!/usr/bin/env python3
"""BC9d simulator measurement: hard TDD exclusion from CONFIRMED DCIs (default) vs every accepted DCI (--excl-unconfirmed 1,
the pre-BC9d hazard) vs no exclusion. [SIMULATED] nr_td_sim, TDD DDDSU, 4 RX, blind (oracle 0), dci-miss 0.1, other-UE 0.1.
usage: bc9d_campaign.py SIM OUTDIR [--jobs 8] [--acq 300] -> OUTDIR/raw_<cell>.jsonl, OUTDIR/summary.md
Means/medians over decided RNTIs (undecidable = capped or truth lost, reported separately); cold = first two RNTIs of an acquisition."""
import json, os, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BASE = ["--slot-model", "1", "--oracle", "0", "--n-rx", "4", "--tdd", "DDDSU", "--dci-miss", "0.1", "--other-ue-occ", "0.1", "--seed", "1"]
EXCL = {"confirmed": [], "unconfirmed": ["--excl-unconfirmed", "1"], "none": ["--tdd-exclude", "0"]}
LEV = ["--crc-accept", "1", "--geom-pin", "1", "--sib-pmin", "0"]  # levers with certified evidence alone (guard off: the fast path fires)


def cells():
    out = []
    for rho in ("0.5", "0.9"):
        for df in ("0", "1e-2"):
            for e, fl in EXCL.items():
                out.append((f"rho{rho}_df{df}_{e}", BASE + ["--persist", rho, "--dci-false", df] + fl))
    for cc in ("0", "1"):  # ISAC_TD_CERT_CONFIRMED cost: levers C/P (certified evidence alone) at rho 0.5, dci-false 1e-2
        out.append((f"rho0.5_df1e-2_CPnoguard_certconf{cc}", BASE + ["--persist", "0.5", "--dci-false", "1e-2", "--cert-confirmed", cc] + LEV))
    return out


def run(sim, acq, outdir, c):
    name, fl = c
    p = subprocess.run([sim, "--acq", str(acq)] + fl, capture_output=True, text=True, check=True)
    with open(os.path.join(outdir, f"raw_{name}.jsonl"), "w") as f:
        f.write(p.stdout)
    rows = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    s = rows[-1]["summary"]
    rn = [r for r in rows[:-1] if "rnti_rank" in r]
    dec = [r for r in rn if not r["undecidable"]]
    cold = [r["seconds"] for r in dec if r["rnti_rank"] < 2]
    f = lambda v, fn: f"{fn(v):.1f}" if v else "-"
    return (name, len(rn), s["wrong"], s["undecidable"], s.get("truth_excluded"), f([r["seconds"] for r in dec], statistics.mean),
            f([r["seconds"] for r in dec], statistics.median), f(cold, statistics.mean), f(cold, statistics.median),
            s.get("tdd_excl_removed"), s.get("spur_excl_dcis"), s.get("certified_wrong"), s.get("wrong_pins", "-"),
            s.get("crc_accepts", "-"), s.get("geom_pins", "-"), s.get("cert_fed", "-"))


def main():
    sim, outdir = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 8
    acq = int(sys.argv[sys.argv.index("--acq") + 1]) if "--acq" in sys.argv else 300
    os.makedirs(outdir, exist_ok=True)
    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda c: run(sim, acq, outdir, c), cells()))
    hdr = ("cell", "RNTIs", "wrong", "undecidable", "truth_excluded", "mean_s", "median_s", "cold_mean_s", "cold_median_s",
           "tdd_excl_removed", "spur_excl_dcis", "certified_wrong", "wrong_pins", "crc_accepts", "geom_pins", "cert_fed")
    lines = ["| " + " | ".join(hdr) + " |", "|" + "---|" * len(hdr)] + ["| " + " | ".join(str(x) for x in r) + " |" for r in res]
    with open(os.path.join(outdir, "summary.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
