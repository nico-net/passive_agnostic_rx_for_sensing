#!/usr/bin/env python3
"""Per-CCE catch rate -> tests whether the dedicated CORESET's low yield is FREQUENCY DEPENDENT.

Hypothesis under test (handover section 10.5, cause #1): CORESET#0 (48 RB, immediately adjacent to
the SSB the receiver syncs on) is caught at ~90 %, while the dedicated CORESET (270 RB, spanning the
whole BWP) is caught at 2-3 %. A residual sub-bin timing offset produces a phase ramp across
subcarriers, so the channel estimate degrades with distance from the sync reference. If that is the
cause, catch rate must FALL with CCE index (CCE n occupies REGs at increasing frequency). A flat
profile refutes it and points at cause #2 (LLR autoscaling on a sparse wideband CORESET) instead.

Usage:  score_cce.py <run_dir>      # reads runN.log + runN_gnb_cce.txt written by _run_catchrate.sh
"""
import re, sys, glob, os, collections

def hist(path):
    d = collections.Counter()
    for line in open(path):
        m = re.match(r"\s*(\d+)\s+cce=(\d+)", line)
        if m:
            d[int(m.group(2))] += int(m.group(1))
    return d

def main(run_dir):
    gnb, ours = collections.Counter(), collections.Counter()
    for gf in sorted(glob.glob(os.path.join(run_dir, "run*_gnb_cce.txt"))):
        gnb += hist(gf)
        log = gf.replace("_gnb_cce.txt", ".log")
        if not os.path.exists(log):
            continue
        crnti = None
        for line in open(log, errors="ignore"):
            if "DCIGT" in line:
                m = re.search(r"rnti=(0x[0-9a-f]+).*cce=(\d+) al=2", line)
                if m:
                    ours[int(m.group(2))] += 1
    if not gnb:
        print("no gNB CCE data in", run_dir); return
    print(f"{'cce':>4} {'offered':>8} {'caught':>7} {'catch%':>7}")
    tot_o = tot_c = 0
    for c in sorted(gnb):
        if gnb[c] < 100:      # ignore CCEs the scheduler barely uses -- ratios there are noise
            continue
        tot_o += gnb[c]; tot_c += ours[c]
        print(f"{c:>4} {gnb[c]:>8} {ours[c]:>7} {100*ours[c]/gnb[c]:>6.1f}%")
    print(f"{'ALL':>4} {tot_o:>8} {tot_c:>7} {100*tot_c/tot_o if tot_o else 0:>6.1f}%")
    print("\nRising/flat with cce -> refutes the timing-ramp hypothesis; falling -> supports it.")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
