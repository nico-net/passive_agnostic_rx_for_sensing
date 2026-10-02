#!/usr/bin/env python3
"""BC9 simulator-part campaign (certified k0 evidence for levers P/C). [SIMULATED] nr_td_sim.
usage: bc9_campaign.py SIM OUTDIR [--jobs 8] [--acq 500] -> OUTDIR/raw_<arm>.jsonl, OUTDIR/summary.md
Cold = first two RNTIs of an acquisition, steady = later; undecidable (capped) RNTIs are censored (excluded from means/medians)."""
import json, os, statistics, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

SLOT = ["--slot-model", "1", "--oracle", "0", "--dci-false", "1e-3"]
LEV = ["--crc-accept", "1", "--geom-pin", "1"]
TDD = ["--tdd", "DDDSU"]
ARMS = {"base": [], "CP": LEV, "CP_fb2": LEV + ["--fieldbook", "2"]}


def cells(rho_list=("0.9",)):
    out = []
    for rx in (1, 4):
        for miss in ("0.01", "0.1"):
            for occ in ("0", "0.1"):
                for arm in ARMS:
                    out.append((rx, "rho0.9", "noTDD", miss, occ, arm, SLOT + ["--persist", "0.9", "--dci-miss", miss, "--other-ue-occ", occ] + ARMS[arm]))
    for rx in (1, 4):  # TDD: receiver without TDD knowledge (base), with it (excl), levers with it
        pre = SLOT + ["--persist", "0.9", "--dci-miss", "0.1", "--other-ue-occ", "0.1"] + TDD
        out.append((rx, "rho0.9", "TDD", "0.1", "0.1", "base", pre + ["--tdd-exclude", "0"]))
        out.append((rx, "rho0.9", "TDD", "0.1", "0.1", "excl", pre))
        out.append((rx, "rho0.9", "TDD", "0.1", "0.1", "CP", pre + LEV))
        out.append((rx, "rho0.9", "TDD", "0.1", "0.1", "CP_fb2", pre + LEV + ["--fieldbook", "2"]))
    for rx in (1, 4):  # low persistence: the lever fast path can fire
        pre = SLOT + ["--persist", "0.0", "--dci-miss", "0.1", "--other-ue-occ", "0.1"]
        for arm in ARMS:
            out.append((rx, "rho0.0", "noTDD", "0.1", "0.1", arm, pre + ARMS[arm]))
    return out


def run(sim, acq, c):
    rx, rho, tdd, miss, occ, arm, fl = c
    p = subprocess.run([sim, "--acq", str(acq), "--seed", "1", "--n-rx", str(rx)] + fl, capture_output=True, text=True, check=True)
    return [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]


def main():
    sim, out = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 8
    acq = int(sys.argv[sys.argv.index("--acq") + 1]) if "--acq" in sys.argv else 500
    os.makedirs(out, exist_ok=True)
    cs = sorted(cells(), key=lambda c: (c[0] != 1, c[1] != "rho0.9"))  # 1 RX first (slowest)
    with ThreadPoolExecutor(jobs) as ex:
        res = list(ex.map(lambda c: run(sim, acq, c), cs))
    rows = ["| rx | rho | tdd | miss | occ | arm | cold mean/median s | steady mean/median s | wrong | wrong_pins | undec | crc_accepts | geom_pins | cert share (explore feeds) | cert share (passes) | tdd_excl_removed |", "|" + "---|" * 16]
    for c, recs in zip(cs, res):
        s = next(r["summary"] for r in recs if "summary" in r)
        rr = [r for r in recs if "summary" not in r]
        with open(os.path.join(out, "raw_%d_%s_%s_m%s_o%s_%s.jsonl" % (c[0], c[1], c[2], c[3], c[4], c[5])), "w") as f:
            for r in recs: f.write(json.dumps(r) + "\n")
        ok = [r for r in rr if not r["undecidable"]]
        cold = [r["seconds"] for r in ok if r["rnti_rank"] < 2]; st = [r["seconds"] for r in ok if r["rnti_rank"] >= 2]
        f = lambda v: "%.1f/%.1f" % (statistics.mean(v), statistics.median(v)) if v else "-"
        sh = lambda a, b: "%.3f" % (s.get(a, 0) / s[b]) if s.get(b) else "-"
        rows.append("| %d | %s | %s | %s | %s | %s | %s | %s | %d | %d | %d | %d | %d | %s | %s | %d |" % (
            c[0], c[1], c[2], c[3], c[4], c[5], f(cold), f(st), s["wrong"], s.get("wrong_pins", 0), s["undecidable"], s.get("crc_accepts", 0),
            s.get("geom_pins", 0), sh("cert_fed", "fed_all"), sh("cert_pass", "pass_all"), s.get("tdd_excl_removed", 0)))
    open(os.path.join(out, "summary.md"), "w").write("\n".join(rows) + "\n")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
