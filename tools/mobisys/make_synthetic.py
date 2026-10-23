#!/usr/bin/env python3
"""SYNTHETIC receiver log + gNB MAC log + example datasets/plots. Nothing here is a measurement.

  python3 make_synthetic.py --work DIR                 # writes DIR/synthetic_rx.log, DIR/synthetic_gnb_mac.log
  python3 make_synthetic.py --work DIR --examples      # also runs export (lab, commercial), groundtruth, plots into examples/

Lines are formatted from the receiver's own C format strings (comments in parse.py name them); every line is
prefixed with a unix time like the repo's stamp.py harness, so the exporter runs with --line-time unix.
"""
import argparse
import json
import math
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UNIX0 = 1_790_000_000.0  # fixed so the fixture is reproducible
RX_START_S = 3.0         # receiver starts late: gNB lines before it must not count as misses
READY_S = 8.0            # first CRC-OK not before this: cold start


def _evm(rng, rnti, frame, slot, crc, mcs, nl, rb, pid, tbs, snr, e2e):
    dec = e2e[3]
    tail = ("rv=0 ndi=1 pid=%d tbs=%d snr_l=%.1f,%.1f,%.1f,%.1f l2=0 zabs=1000 dmrs_snr=%.1f,%.1f,%.1f,%.1f,%.1f dmrs_pw=1.0 "
            "dmrs_ref=1.0 dsp=0 skip=0 gp=0,0,0,0,0,0,0" % (pid, tbs, snr, snr, snr, snr, snr, snr, snr, snr, snr))
    return ("SENSING: EVMGRANT rnti=0x%04x %d.%d crc=%d mcs=%d tbl=1 Qm=%d Nl=%d rb=%d+%d nsym=12 evm=%.1f snr=%.1f "
            "br_snr=%.1f,%.1f,%.1f,%.1f nbrx=4 tok=0 snr_cpe=%.1f probe=0 kappa=1.00 alpha=1,1,1,1 nvar_db=-30,-30,-30,-30 "
            "rho=0.10 comb=3 ports=0x1 cdm=1 ndmrs=1 eq=1 occ=0 it=6 dec_us=%d %s e2e=%d,%d,%d,%d,%d,1,%d,%d"
            % (rnti, frame, slot, crc, mcs, 2 if mcs < 5 else 4 if mcs < 17 else 6, nl, rb[0], rb[1], 100 / (1 + 10 ** (snr / 20)),
               snr, snr, snr, snr, snr, snr, dec, tail, e2e[0], e2e[1], e2e[2], e2e[3], int(e2e[3] * 0.85),
               sum(e2e), 1))


def build(seed=7, secs=150, spf=20, n_ue=3, rate=3.0):
    rng = random.Random(seed)
    rntis = [0x4601 + 7 * i for i in range(n_ue)]
    rx, gnb = [], []

    def put(t, text):
        rx.append((t, "%.6f %s" % (UNIX0 + t, text)))

    for t, text in ((RX_START_S, "Starting sync detection"), (4.1, "Cell Detected with GSCN: 7929, SSB SC offset: 0"),
                    (4.3, "Initial sync successful, PCI: 101"), (4.5, "Initial sync: pbch decoded sucessfully, ssb index 1"),
                    (4.6, "SENSING: ACQ_STATE SEARCHING -> PBCH_LOCKED (updates=3 time_in_prev=3 regressions=0)"),
                    (5.4, "SENSING: ACQ_EVENT sib1_decoded (n=1, state=PBCH_LOCKED)"),
                    (5.5, "SENSING: ACQ_STATE PBCH_LOCKED -> SIB1_DECODED (updates=9 time_in_prev=9 regressions=0)"),
                    (6.2, "SENSING: CORESET VERIFIED by fresh dedicated DCI: offset=0 observed_span=48"),
                    (6.3, "SENSING: ACQ_STATE SIB1_DECODED -> CORESET_VERIFIED (updates=14 time_in_prev=5 regressions=0)"),
                    (7.0, "SENSING: DCI 1_1 length locked coreset=1 rnti=0x%x len=41" % rntis[0]),
                    (READY_S - 0.2, "SENSING: Technique D CONVERGED rnti=0x%x tda=2 S=2 L=12 mask=0x1 table=1" % rntis[0]),
                    (READY_S - 0.1, "SENSING: ACQ_STATE CORESET_VERIFIED -> DL_CONVERGED (updates=30 time_in_prev=16 regressions=0)"),
                    (90.0, "SENSING: CONFIG_EPOCH 0 -> 1 class=SOFT cause=SIB1_CHANGE scope=cell"),
                    (100.0, "SENSING: RXDISCONT abs_slot=%d frame=%d slot=0 expected=1000 actual=3000 delta=2000 prev_consumed=900 n=1 "
                            "-> INVALIDATING SYNC" % (100 * 2000, 0)),
                    (100.0, "SENSING: CONFIG_EPOCH continuity gap_samples=2000 gap_ms=0.016")):
        put(t, text)

    pend_fail = []
    for ue, rnti in enumerate(rntis):
        t = rng.uniform(0, 0.3)
        while t < secs:
            t += rng.expovariate(rate)
            fr, sl = int(t / 0.01) % 1024, int((t % 0.01) / 0.01 * spf)
            gnb.append((t, "%4d.%2d [DLSCH/PDSCH/PUCCH] RNTI %04x DCI L 4 start %3d RBs %3d startSymbol  2 nb_symbol 12 dmrspos 4 MCS %2d "
                           "nrOfLayers 2 TBS %4d HARQ PID %2d round 0 RV 0 NDI 1 dl_data_to_ULACK 4 (0.0) PUCCH allocation 0 TPC 1"
                        % (fr, sl, rnti, 10, 20, 4 + 9 * ue, 3000, rng.randrange(16))))
            if t < RX_START_S + 3.5 or rng.random() < 0.03:
                continue  # receiver not ready / DCI missed
            ts = t + 0.0004
            put(ts, "SENSING: blind PDCCH rnti_seen utc_ns=%d rnti=0x%x sfn=%d slot=%d fmt=1_1 cce=%d al=4"
                % (int((UNIX0 + ts) * 1e9), rnti, fr, sl, rng.randrange(0, 40, 4)))
            if rng.random() < 0.01:
                continue  # accepted but never decoded
            e2e = [int(rng.lognormvariate(math.log(150), 0.4)), int(rng.lognormvariate(math.log(80), 0.5)),
                   int(rng.lognormvariate(math.log(120), 0.4)), int(rng.lognormvariate(math.log(330), 0.35))]
            snr = rng.gauss(18 - 4 * ue, 3)
            ok = (t >= READY_S) and rng.random() < 1 / (1 + math.exp(-(snr - 6) / 2))
            pid = rng.randrange(16)
            put(ts + sum(e2e) / 1e6, _evm(rng, rnti, fr, sl, int(ok), 4 + 9 * ue, 2, (10, 20), pid, 3000, snr, e2e))
            if not ok and rng.random() < 0.25:
                pend_fail.append((ts + 0.004, rnti, fr, sl, pid))
    for ts, rnti, fr, sl, pid in pend_fail:
        put(ts, "SENSING: RECOVLATE rnti=0x%04x %d.%d abs=%d pid=%d ndi=1 tbs=3000 recovered_cfg=0 tot_us=%d" % (rnti, fr, sl, 0, pid, 4200))
    t = rng.uniform(0, 1)
    while t < secs:  # false-alarm DCI
        t += rng.expovariate(0.15)
        if t > RX_START_S + 3.5:
            put(t, "SENSING: blind PDCCH rnti_seen utc_ns=%d rnti=0x%x sfn=%d slot=%d fmt=1_1 cce=8 al=2"
                % (int((UNIX0 + t) * 1e9), rng.randrange(0x1000, 0xfff0), int(t / 0.01) % 1024, rng.randrange(spf)))
    for k in range(1, int(secs // 10) + 1):  # periodic lines
        t = 10.0 * k
        mono = int((1000 + t) * 1e9)
        m = {"schema": 1, "t_mono_ns": mono, "t_real_ns": int((UNIX0 + t) * 1e9), "abs_slot": int(t * 2000), "pci": 101,
             "acq_state": "DL_CONVERGED", "pdcch_accepts_c": int(t * 9), "scanq_queued": int(t * 400), "scanq_drop_full": 0,
             "pdschq_queued": int(t * 9), "pdschq_decoded": int(t * 9), "pdschq_crc_ok": int(t * 8), "cfg_epoch": int(t > 90),
             "cfg_identity_gen": 0, "proc_utime_us": int(t * 3.1e5), "proc_stime_us": int(t * 4e4), "proc_maxrss_kb": 900000 + 50 * k,
             "proc_minflt": 1000 * k, "proc_majflt": 0, "recov_attempts": k, "retry_yield_chains": 2 * k}
        put(t, "SENSING: ISAC_METRICS " + json.dumps(m))
        put(t, "SENSING: PDTIMCLS us real[n=%d mean=330.0 p50=325 p90=495 p99=705 max=1900.0 gt5ms=0 gt20ms=0 ns1=0.9 ns5=0.1 ns20=0.0]" % (int(t * 9)))
        put(t, "SENSING: RFCENSUS slots=%d ssb_slots=100 pbch_ok=%d pbch_fail=0 rf_pow=60.00 ref=60.00 bad=0 shiftForNextFrame=0 max_pos_acc=0 frame=%d"
            % (int(t * 2000), 50 * k, int(t * 100) % 1024))
    rx = [x for _, x in sorted(rx, key=lambda p: p[0])]
    gnb = ["%.6f %s" % (UNIX0 + t, text) for t, text in sorted(gnb)]
    return rx, gnb


def run(cmd):
    r = subprocess.run([sys.executable] + cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("failed: %s\n%s" % (cmd[0], r.stderr[-500:]))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--examples", action="store_true")
    ap.add_argument("--secs", type=int, default=150)
    a = ap.parse_args(argv)
    os.makedirs(a.work, exist_ok=True)
    rx, gnb = build(secs=a.secs)
    rxp, gp = os.path.join(a.work, "synthetic_rx.log"), os.path.join(a.work, "synthetic_gnb_mac.log")
    open(rxp, "w").write("\n".join(rx) + "\n")
    open(gp, "w").write("\n".join(gnb) + "\n")
    if a.examples:
        ex = os.path.join(HERE, "examples")
        shutil.rmtree(ex, ignore_errors=True)
        common = ["--provenance", "synthetic", "--host-label", "synthetic (cloud x86 generator)", "--line-time", "unix",
                  "--no-sampler", "--flush-s", "1"]
        run([os.path.join(HERE, "export.py"), "--experiment-mode", "lab", "--out", os.path.join(ex, "lab"), "--input", rxp] + common)
        with open(rxp, "rb") as f:  # commercial: through a real pipe, as in deployment
            p = subprocess.run([sys.executable, os.path.join(HERE, "export.py"), "--experiment-mode", "commercial", "--out",
                                os.path.join(ex, "commercial")] + common, input=f.read(), capture_output=True)
            if p.returncode:
                sys.exit("commercial export failed")
        run([os.path.join(HERE, "groundtruth.py"), "--rx", os.path.join(ex, "lab"), "--gnb-log", gp,
             "--out", os.path.join(ex, "lab", "groundtruth")])
        run([os.path.join(HERE, "plots.py"), "--lab", os.path.join(ex, "lab"), "--commercial", os.path.join(ex, "commercial"),
             "--out", os.path.join(ex, "plots")])
    print("ok")


if __name__ == "__main__":
    main()
