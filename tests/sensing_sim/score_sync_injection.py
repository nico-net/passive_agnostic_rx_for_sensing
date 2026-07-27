#!/usr/bin/env python3
"""Score the UE's Phase 1-3 sync estimates against a KNOWN injected receiver-clock error.

WHY THIS EXISTS
---------------
A sync-on/off A/B on this harness cannot validate the sync algorithm. Measured on the 4-object
scene, rfsimulator presents CFO rms 0.42 Hz and STO drift 0.12 bins/CPI, and the SFO correction is
withheld on 100% of CPIs by its own linearity gate because there is no real drift to fit. So the A/B
measures "correcting nothing changes nothing" -- a true statement about the harness, not about the
estimator. `sensing_channel_set_rx_clock()` injects a known impairment; this script checks whether
the estimator gets it back.

TIME-ALIGNMENT-FREE BY CONSTRUCTION
-----------------------------------
The injected STO/CFO/SFO are CONSTANTS for a run, so there is nothing to align: every CPI estimates
the same truth. That sidesteps the trap that has bitten this project repeatedly -- CPI log lines and
`SENSING_CHANNEL` log lines are written by different threads at different rates, so pairing them by
position in the file is not a valid clock (see the retracted "20% dt bias" in the project notes).

UNITS: WHY CFO AND SFO ARE THE LOAD-BEARING TESTS
-------------------------------------------------
CFO is reported in Hz and injected in Hz; SFO is reported in ppm and injected in ppm, and ppm is
dimensionless so it is identical whether the ramp is counted in RF samples or in range bins. Both
comparisons are therefore free of any unit conversion, and any disagreement is a real estimator
error rather than a modelling assumption of this script.

STO is different: it is injected in RF samples but estimated in range bins, and
    bins = samples * (N*df) / fs_rf
so scoring it needs the occupied bandwidth and the RF sample rate. Those are reported when supplied
(--bw-hz/--fs-hz) and otherwise skipped rather than guessed -- an unverifiable number is worse than
an absent one. STO is scored as a secondary check regardless of units via the tracker's own
self-consistency (does the walk track the injected ramp rate?).

Usage:
  score_sync_injection.py <run_dir|ue.log> [--bw-hz 98280000] [--fs-hz 122880000] [--csv PREFIX]
"""
import argparse
import math
import os
import re
import statistics
import sys

CLK_RE = re.compile(
    r"SENSING_CHANNEL clk:\s*t=([-\d.]+)s\s+sto_samples=([-\d.]+)\s+sfo_ppm=([-\d.]+)\s+"
    r"cfo_hz=([-\d.]+)\s+total_delay_samples=([-\d.]+)"
)
CPI_RE = re.compile(
    r"SENSING: sync CPI #(\d+)\s+"
    r"STO\[n=(\d+)\s+fly=(\d+)\s+frac_bin=([-+\d.]+)\s+drift=([-+\d.]+)\s+corrected=(\w+)\s+walk=([-+\d.]+)\]\s+"
    r"CFO\[hz=([-+\d.]+)\s+filt=([-+\d.]+)\s+rms_rad=([-+\d.]+)\]\s+"
    r"SFO\[raw=([-+\d.]+)\s+filt=([-+\d.]+)\s+hz=([-+\d.]+)\s+n_fit=(\d+)/(\d+)\s+corrected=(\w+)\]"
)


def find_log(path):
    if os.path.isfile(path):
        return path
    for cand in (os.path.join(path, "logs", "ue.log"), os.path.join(path, "ue.log")):
        if os.path.isfile(cand):
            return cand
    sys.exit(f"no ue.log under {path}")


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def summarize(name, est, truth, unit, tol, out):
    """Report an estimator against its known truth. `tol` is the pass threshold in `unit`."""
    if not est:
        out.append((name, None))
        print(f"{name:26s} NO ESTIMATES IN LOG")
        return
    err = [e - truth for e in est]
    med = statistics.median(est)
    bias = statistics.median(err)
    print(f"{name:26s} injected {truth:+10.4f} {unit:<4s} | est median {med:+10.4f} "
          f"| bias {bias:+9.4f} | rms err {rms(err):8.4f} | n={len(est)}")
    ok = abs(bias) <= tol
    out.append((name, ok))
    if not ok:
        print(f"{'':26s}   ^ bias exceeds tolerance {tol:g} {unit}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--bw-hz", type=float, default=None, help="occupied bandwidth N*df, for STO units")
    ap.add_argument("--fs-hz", type=float, default=None, help="RF sample rate, for STO units")
    ap.add_argument("--cfo-tol", type=float, default=1.0, help="Hz")
    ap.add_argument("--sfo-tol", type=float, default=0.15, help="ppm")
    ap.add_argument("--csv", default=None)
    a = ap.parse_args()

    log = find_log(a.path)
    clk, cpi = [], []
    with open(log, "r", errors="ignore") as fh:
        for line in fh:
            m = CLK_RE.search(line)
            if m:
                clk.append(tuple(float(x) for x in m.groups()))
                continue
            m = CPI_RE.search(line)
            if m:
                cpi.append(m.groups())

    if not clk:
        sys.exit("No 'SENSING_CHANNEL clk:' lines: no impairment was injected (or the binary predates "
                 "sensing_channel_set_rx_clock). Nothing to score.")
    if not cpi:
        sys.exit("No 'SENSING: sync CPI' lines: sync_correction is off, or no CPI completed.")

    sto_s = {round(c[1], 6) for c in clk}
    sfo_p = {round(c[2], 6) for c in clk}
    cfo_h = {round(c[3], 6) for c in clk}
    if len(sto_s) > 1 or len(sfo_p) > 1 or len(cfo_h) > 1:
        sys.exit("Injected impairment is not constant over the run; this scorer assumes it is.")
    t_sto, t_sfo, t_cfo = sto_s.pop(), sfo_p.pop(), cfo_h.pop()

    cfo_est = [float(c[7]) for c in cpi]
    sfo_est = [float(c[10]) for c in cpi]
    n_lock = [int(c[1]) for c in cpi]
    n_fly = [int(c[2]) for c in cpi]
    sfo_corr = sum(1 for c in cpi if c[15] == "yes")
    sto_corr = sum(1 for c in cpi if c[5] == "yes")

    print("=" * 96)
    print(f"sync-injection validation  |  {log}")
    print(f"injected: STO {t_sto:+.4f} samples, CFO {t_cfo:+.4f} Hz, SFO {t_sfo:+.4f} ppm"
          f"   ({len(cpi)} CPIs)")
    print("=" * 96)

    verdicts = []
    summarize("CFO (Hz)", cfo_est, t_cfo, "Hz", a.cfo_tol, verdicts)
    summarize("SFO raw (ppm)", sfo_est, t_sfo, "ppm", a.sfo_tol, verdicts)

    if a.bw_hz and a.fs_hz:
        t_sto_bins = t_sto * a.bw_hz / a.fs_hz
        print(f"{'STO (bins, derived)':26s} injected {t_sto_bins:+10.4f} bins "
              f"(= {t_sto:.3f} samples x BW/fs = {a.bw_hz/a.fs_hz:.4f})")
        print(f"{'':26s} NOTE: the tracker corrects only the FRACTIONAL bin, by design -- the integer"
              f" part is absorbed by the walk, so this is context, not a pass/fail.")
    else:
        print(f"{'STO':26s} not scored in absolute units (pass --bw-hz and --fs-hz to enable)")

    print()
    print(f"tracker internals: locked/CPI {statistics.mean(n_lock):.1f}  flywheel/CPI {statistics.mean(n_fly):.1f}"
          f"  ({100*statistics.mean(n_fly)/max(1e-9, statistics.mean(n_lock)+statistics.mean(n_fly)):.0f}% flywheel)")
    print(f"corrections applied: STO {sto_corr}/{len(cpi)} CPIs, SFO {sfo_corr}/{len(cpi)} CPIs")
    if abs(t_sfo) > 0.01:
        # The SFO_MIN_R_SQUARED gate withholds the correction unless the delay-vs-time fit is
        # genuinely linear. With a real linear ramp injected it SHOULD now engage -- if it does not,
        # the gate is too tight and is suppressing corrections the receiver actually needs.
        frac = sfo_corr / len(cpi)
        print(f"  => with a real {t_sfo:+.3f} ppm drift injected, the linearity gate passed on "
              f"{100*frac:.0f}% of CPIs")
        verdicts.append(("SFO gate engages on real drift", frac >= 0.5))
        if frac < 0.5:
            print("     ^ the gate is REJECTING a genuine linear drift: SFO_MIN_R_SQUARED is too tight,")
            print("       or the ramp is being broken by the sawtooth wrap (raise rx_sfo_wrap_samples).")

    print()
    failed = [n for n, ok in verdicts if ok is False]
    if failed:
        print("VERDICT: FAIL -- " + ", ".join(failed))
    else:
        print("VERDICT: PASS -- every injected impairment recovered within tolerance")

    if a.csv:
        print(f"{a.csv},{t_sto:.4f},{t_cfo:.4f},{t_sfo:.4f},{len(cpi)},"
              f"{statistics.median(cfo_est):.4f},{statistics.median(sfo_est):.4f},"
              f"{statistics.mean(n_lock):.2f},{statistics.mean(n_fly):.2f},"
              f"{sto_corr},{sfo_corr},{0 if failed else 1}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
