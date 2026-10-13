#!/usr/bin/env python3
"""Synthetic run directories for the dashboard tests and screenshots (NOT receiver results).

Every line uses the exact format string of the receiver source it imitates (file named per line below), so a
format change in the C code shows up as a failing parser test once the fixture is updated from the new format.

  make_fixtures.py build OUT_DIR                 # writes OUT_DIR/{healthy,sync_loss,stale_tails,ul_unlocked,multi_cell}
  make_fixtures.py stream SRC_RUN DST_RUN [--speed 10]
        copies a fixture run into DST_RUN progressively (log, metrics, obs in time order) as a receiver would:
        feeds the dashboard's live mode without a radio.
"""
import argparse
import json
import math
import os
import random
import shutil
import struct
import sys
import time

T0_NS = 1_000_000_000_000   # arbitrary CLOCK_MONOTONIC origin of the fake receiver
LABEL = "[SYNTHETIC FIXTURE, make_fixtures.py, not a receiver result]"


class Run:
    def __init__(self, out, name, seed):
        self.dir = os.path.join(out, name)
        os.makedirs(self.dir, exist_ok=True)
        self.rng = random.Random(seed)
        self.log = []          # (t_s, line)
        self.metrics = []
        self.obs = []
        self.rec = []
        self.cfr = []          # (t_s, bytes)
        self.c = dict(occ=0, cand=0, acc=0, accc=0, sq=0, sp=0, sfull=0, sstale=0, pq=0, pdec=0, pok=0, pfull=0, pstale=0,
                      ptry=0, pusok=0, ldok=0, ldseg=0, si=0, pbch=0, sib1=0, losses=0, trips=0, mv=0)

    def L(self, t, line):
        self.log.append((t, line))

    def tick_metrics(self, t, state, pci, gpu=35.0, extra=None):
        c = self.c
        m = {"schema": 1, "t_mono_ns": T0_NS + int(t * 1e9), "abs_slot": int(t * 2000), "pci": pci, "acq_state": state,
             "acq_transitions": 5, "acq_sync_losses": c["losses"], "acq_pbch_locks": c["pbch"], "acq_sib1_decodes": c["sib1"],
             "pdcch_occasions": c["occ"], "pdcch_candidates": c["cand"], "pdcch_accepts": c["acc"], "pdcch_accepts_c": c["accc"],
             "scanq_queued": c["sq"], "scanq_processed": c["sp"], "scanq_drop_full": c["sfull"], "scanq_drop_stale": c["sstale"],
             "scanq_drop_epoch": 0, "scanq_max_lag": 3, "pdcch_inline_drop_epoch": 0,
             "pdschq_queued": c["pq"], "pdschq_decoded": c["pdec"], "pdschq_crc_ok": c["pok"], "pdschq_drop_full": c["pfull"],
             "pdschq_drop_stale": c["pstale"], "pdschq_drop_epoch": 0, "pdschq_stale_after_decode": 0, "pdschq_max_lag": 6,
             "puschq_drop_epoch": 0, "ldpc_ok": c["ldok"], "ldpc_seg_fail": c["ldseg"], "ldpc_tb_fail": 0, "ldpc_zero_tb": 0,
             "ldpc_cuda_errors": 0, "ldpc_cuda_fallbacks": 0, "ldpc_cuda_poisoned": 0, "ldpc_cuda_disabled": 0,
             "ldpc_cuda_breaker_trips": c["trips"], "ldpc_tb_cpu": c["ldok"], "ldpc_tb_cuda": 0,
             "pusch_try": c["ptry"], "pusch_crc_ok": c["pusok"], "obs_pushed": len(self.obs), "obs_written": len(self.obs),
             "obs_dropped": 0, "sense_cfr_drop": 0, "sense_rec_drop": 0, "sense_io_errors": 0, "sense_cfr_no_sink": 0,
             "prach_us": 0, "prach_drops": 0, "prach_skipped": 0, "srs_us": 0, "srs_jobs_dropped": 0,
             "csirs_candidates": 4, "csirs_confirmed": 1 if t > 60 else 0, "csirs_revoked": 0, "zp_exported": 0, "zp_revoked": 0,
             "gpu_budget_used_pct": gpu, "discovery_yields": 0, "rt_gpu_wait_max_us": 120, "rt_gpu_fallbacks": 0,
             "discovery_overruns": 0, "csirs_time_to_confirm_slots_last": 848, "csirs_time_to_confirm_slots_sum": 1065,
             "csirs_time_to_confirm_resources": [{"zp": 0, "row": 1, "ports": 1, "density": 3, "period": 20, "offset": 4,
                                                  "offset2": 0, "freq_domain": 1, "symb_l0": 4, "slots": 217}] if t > 60 else [],
             "pdschq_drop_narrow": 0, "pdschq_latency_us_bounds": [100, 250, 500, 1000, 2000, 4000, 8000, 16000],
             "pdschq_reasons": {"queue_expired": c["pstale"], "future_unready": 0, "copy_expired": 0, "unsupported": 0,
                                "decode_error": 0, "gpu_fallback": 0}}
        m.update(extra or {})
        self.metrics.append(m)
        self.L(t, "[PHY]   SENSING: ISAC_METRICS " + json.dumps(m, separators=(",", ":")))

    def grant(self, t, rnti, pci, mcs, nl, nb_rb, crc, dir_="DL", dec_us=None, snr=None):
        qm = 2 if mcs <= 9 else 4 if mcs <= 16 else 6
        tbs = int(nb_rb * 12 * 11 * qm * nl * (0.12 + 0.03 * mcs) / 8) * 8
        frame, slot = int(t * 100) % 1024, int(t * 2000) % 20
        snr = snr if snr is not None else 4 + mcs * 0.9 + self.rng.gauss(0, 1.5)
        o = {"schema": 1, "abs_slot": int(t * 2000), "t_mono_ns": T0_NS + int(t * 1e9), "frame": frame, "slot": slot,
             "pci": pci, "dir": dir_, "rnti": rnti, "rnti_class": 0, "start_rb": self.rng.randrange(0, 100), "nb_rb": nb_rb,
             "start_sym": 2, "nb_sym": 12, "mcs": mcs, "mcs_table": 1, "qm": qm, "nl": nl, "dmrs_symb_pos": 2180,
             "dmrs_scrambling_id": pci, "tbs": tbs, "harq_pid": self.rng.randrange(16), "rv": 0, "ndi": 1, "crc": int(crc),
             "nvar": 12.5, "snr_db": round(snr, 1), "fo_comp_hz": 3.1, "delay_samples": 1.2, "carrier_hz": 3680010000,
             "scs_khz": 30, "fs_hz": 122880000}
        self.obs.append(o)
        if dir_ == "DL":
            dec = dec_us if dec_us is not None else max(60, self.rng.lognormvariate(math.log(330), 0.35))
            # nr_pdsch_passive_queue.c EVMGRANT format
            self.L(t, "[PHY]   SENSING: EVMGRANT rnti=0x%04x %d.%d crc=%d mcs=%u tbl=%u Qm=%u Nl=%u rb=%u+%u nsym=%u evm=%.1f "
                      "snr=%.1f br_snr=%.1f,%.1f,%.1f,%.1f nbrx=%u tok=%u snr_cpe=%.1f probe=%d kappa=%.2f "
                      "alpha=%.2f,%.2f,%.2f,%.2f nvar_db=%.1f,%.1f,%.1f,%.1f rho=%.2f comb=%d ports=0x%x cdm=%u ndmrs=%u eq=%d "
                      "occ=%d it=%d dec_us=%.0f rv=%u ndi=%u pid=%u tbs=%u snr_l=%.1f,%.1f,%.1f,%.1f l2=%d zabs=%.0f"
                   % (rnti, frame, slot, int(crc), mcs, 1, qm, nl, o["start_rb"], nb_rb, 12, 30.0, snr, snr - 1, snr - 2, snr - 1,
                      snr - 3, 4, 1, snr, 0, 0.9, 1, 1, 1, 1, 36, 36, 36, 36, 0.1, 3, (1 << nl) - 1, 1, 1, 0, 0, 6, dec, 0, 1,
                      o["harq_pid"], tbs, snr, -99, -99, -99, 0, 1000))
            self.c["pq"] += 1
            self.c["pdec"] += 1
            self.c["pok"] += int(crc)
            self.c["ldok" if crc else "ldseg"] += 1
        else:
            self.c["ptry"] += 1
            self.c["pusok"] += int(crc)
        if self.rng.random() < 0.05:
            self.rec.append({"schema": 2, "type": "rs", "epoch": None, "identity_gen": None, "dedicated_epoch": None,
                             "abs_slot": o["abs_slot"], "frame": frame, "slot": slot, "t_mono_ns": o["t_mono_ns"], "rx_ts": 0,
                             "pci": pci, "source": "pdsch_dmrs" if dir_ == "DL" else "pusch_dmrs", "n_ant": 4, "n_re": 64})
            self.cfr_frame(t, 2 if dir_ == "DL" else 4, o["abs_slot"])

    def cfr_frame(self, t, src, abs_slot, n_re=96, n_ant=4):
        fid = len(self.cfr)
        hdr = struct.pack("<4sHHQBBHIQffII", b"NRCF", 1, 48, fid, src, n_ant, 0, n_re, abs_slot, 0.0, 0.01, 0, 0)
        k = struct.pack("<%dH" % n_re, *[600 + 6 * i for i in range(n_re)])
        l = bytes([2] * n_re)
        pad = b"\0" * (-(48 + 3 * n_re) % 4)
        iq = []
        for a in range(n_ant):
            for i in range(n_re):
                amp = 1.0 + 0.5 * math.cos(2 * math.pi * i / 40 + a) + 0.2 * math.sin(t)
                ph = 0.15 * i + a * 0.7
                iq += [amp * math.cos(ph), amp * math.sin(ph)]
        self.cfr.append((t, hdr + k + l + pad + struct.pack("<%df" % (2 * n_ant * n_re), *iq)))

    def write(self, label=LABEL):
        self.log.sort(key=lambda x: x[0])
        with open(os.path.join(self.dir, "run.log"), "w") as f:
            for _, line in self.log:
                f.write(line + "\n")
        for name, rows in (("metrics.jsonl", self.metrics), ("obs.jsonl", self.obs), ("records.jsonl", self.rec)):
            with open(os.path.join(self.dir, name), "w") as f:
                for r in rows:
                    f.write(json.dumps(r) + "\n")
        with open(os.path.join(self.dir, "cfr.bin"), "wb") as f:
            for _, b in self.cfr:
                f.write(b)
        with open(os.path.join(self.dir, "label.txt"), "w") as f:
            f.write(label + "\n")
        with open(os.path.join(self.dir, "env.txt"), "w") as f:
            f.write("ISAC_METRICS=1\nISAC_METRICS_PATH=%s/metrics.jsonl\nISAC_OBS_PATH=%s/obs.jsonl\nISAC_RX_MRC_MODE=1\n"
                    "ISAC_PDCCH_TIMING=1\nISAC_PDSCH_GPU_HOST_PIN=1\nISAC_UL_LAYOUT_LOCK=1\nISAC_NOT_A_REAL_FLAG=7\n" % (self.dir, self.dir))


def preamble(r, pci, ssb_sc=1171, gscn=7971, mhz=3680.01, kssb=14, sib1=True, t=0.0):
    r.L(t, 'CMDLINE: "nr-uesoftmodem" "-O" "ue.passive.ota.cfg" "-r" "217" "--numerology" "1" "--band" "78" "-C" "%d" '
           '"--ssb" "%d" "--ue-rxgain" "38" "--ue-nb-ant-rx" "4" "--passive-rx" "--initial-fo" "-4800"' % (int(mhz * 1e6), ssb_sc))
    for ch in range(4):   # radio/USRP/usrp_lib.cpp RFCHAN
        r.L(t + 0.01, "[HW]   RFCHAN ch%d subdev=A:0 port=RX1 gain=38.00 range=[0.0..60.0] freq=%.6f MHz rate=122.880000 Msps "
                      "bw=100.000000 MHz" % (ch, mhz))
    # nr_initial_sync.c
    r.L(t + 1.2, "[NR_PHY]   Cell Detected with GSCN: %d, SSB SC offset: %d, SSB Ref: %.6f, PSS Corr peak: 92 dB, PSS Corr Average: 71"
        % (gscn, ssb_sc, 3679.68e6))
    r.L(t + 1.3, "[PHY]   [UE 0] Measured Carrier Frequency offset -4849 Hz")
    r.L(t + 1.3, "[PHY]   Initial sync successful, PCI: %d" % pci)
    r.c["pbch"] += 1
    r.L(t + 1.4, "[PHY]   SENSING: ACQ_STATE SEARCHING -> PBCH_LOCKED (updates=1 time_in_prev=0 regressions=0) evidence[len_found=0]")
    r.L(t + 1.4, "[NR_MAC]   SENSING: MIB fields sfn=12 k_ssb=%d scs_common=1 coreset0=12 ss0=5 dmrs_typeA=2 barred=1 intraFreqResel=1" % kssb)
    r.L(t + 1.5, "[PHY]   SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(groups=8 dur=1 bundle=6 interleaver=2 shift=%d scramb=%d) "
                 "bwp=[16..64) (cset_start_rb = ssb_offset_point_a 97 - rb_offset 16) ss(period=20 offset=0 dur=2 symb=0) "
                 "dci10(mux=1 sib1=1)" % (pci, pci))
    r.L(t + 1.6, "[PHY]   SENSING: PDCCH_SCRAMBLING_ID CONFIRMED [CSS0(common)] n_id=%d (assumed = PCI %d) by CRC-recovered RNTI "
                 "0xffff -- a wrong identity yields zero accepts, so this is proof of the value in use" % (pci, pci))
    if sib1:
        r.c["sib1"] += 1
        r.L(t + 2.0, "[NR_RRC]   SIB1 decoded")
        r.L(t + 2.0, "[NR_RRC]   SENSING: SIB1 CELL mcc=222 mnc=10 cell_identity=0x007ed8847 tac=-1 n_plmn=1")
        r.L(t + 2.1, "[PHY]   SENSING: ACQ_STATE PBCH_LOCKED -> SIB1_DECODED (updates=40 time_in_prev=39 regressions=0)")
        r.L(t + 2.1, "[PHY]   SENSING: TDD from SIB1 DERIVED: p1 period=10 slots dl=7+6sym ul=2+4sym | p2 period=0 dl=0+0sym ul=0+0sym")
        r.L(t + 2.2, "[PHY]   SENSING: ACQ carrier CONFIRMED from SIB1: 217 PRB mu=1, Point A at grid subcarrier 0, carrier ends at "
                     "2604 = grid end (started with 217 PRB mu=1, SSB found at subcarrier 1171); derived carrier centre %.6f MHz "
                     "(started %.6f MHz)" % (mhz, mhz))
    else:
        r.L(t + 2.1, "[PHY]   SENSING: TDD from SIB1 ABSENT (tdd-UL-DL-ConfigurationCommon not in SIB1 -> FDD or pattern unknown)")


def dedicated(r, t, pci):
    r.L(t, "[PHY]   SENSING: multi-CORESET bank add index=0 offset=0 span=216 symbol=0 mapping=0/0/0 len=47")
    r.L(t + 0.1, "[PHY]   SENSING: ACQ_STATE SIB1_DECODED -> CORESET_VERIFIED (updates=200 time_in_prev=160 regressions=0)")
    r.L(t + 0.2, "[PHY]   SENSING: DCI 1_1 length locked coreset=0 rnti=0x4ac6 len=47 occasions=120")
    r.L(t + 0.5, "[PHY]   SENSING: PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)] n_id=%d (assumed = PCI %d) by CRC-recovered RNTI "
                 "0x4ac6 -- a wrong identity yields zero accepts, so this is proof of the value in use" % (pci, pci))
    r.L(t + 3, "[PHY]   SENSING: SEARCH_SPACE INFERRED [USS] by CRC-recovered grants: monitored AL={1,2,4} undersampled=8 "
               "confirmed/examined[AL1=40/900 AL2=30/500 AL4=8/200 AL8=0/20] total_confirmed=78 (accepts per candidate EXAMINED)")


def common_loop(r, t0, t1, pci, rntis, crc_by_mcs, stale=0.02, scan_full=0.005, dec_scale=1.0, gpu=35.0, ul=True,
                metrics_state="TRACKING", tail_ms=None, rate=60, p99_ms=1.1):
    t = t0
    nxt_m = (int(t0 / 20) + 1) * 20.0
    nxt_s = t0 + 5
    conv = set()
    while t < t1:
        t += 1.0 / rate
        rnti = r.rng.choice(rntis)
        mcs = r.rng.choice([0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 16, 19, 22, 27])
        nl = r.rng.choice([1, 1, 1, 2, 2, 3, 4])
        b = next(k for k, lo, hi in (("0-4", 0, 4), ("5-9", 5, 9), ("10-16", 10, 16), ("17-27", 17, 27)) if lo <= mcs <= hi)
        p = crc_by_mcs[b] * (1.0 if nl <= 2 else 0.8)
        dec = max(60, r.rng.lognormvariate(math.log(330 * dec_scale), 0.35))
        if tail_ms and r.rng.random() < 0.03:
            dec = tail_ms * 1000 * r.rng.uniform(0.5, 1.5)
        r.grant(t, rnti, pci, mcs, nl, r.rng.choice([4, 8, 16, 24, 51, 106]), r.rng.random() < p, dec_us=dec)
        if rnti not in conv:
            conv.add(rnti)
            r.L(t, "SWEEP: new per-RNTI context rnti=0x%04x" % rnti)
            r.L(t + 0.3, "[PHY]   SENSING: Technique D CONVERGED rnti=0x%04x tda=1 S=2 L=12 mask=0x884 table=1 k0=0 map=A dci=1_1 "
                         "sib1_row=none deftab=match" % rnti)
        if ul and r.rng.random() < 0.15:
            r.grant(t, rnti, pci, r.rng.choice([2, 5, 9]), 1, r.rng.choice([4, 10, 20]), r.rng.random() < 0.92, "UL")
        c = r.c
        c["occ"] += 40; c["cand"] += 2000; c["acc"] += 3; c["accc"] += 2
        c["sq"] += 40; c["sp"] += 40
        nf = scan_full * 40
        c["sfull"] += int(nf) + (r.rng.random() < nf - int(nf))
        if r.rng.random() < stale / (1 - stale):
            c["pq"] += 1; c["pstale"] += 1
        if t >= nxt_s:
            nxt_s += 5
            r.L(t, "[HW]   SENSING: CHPOW sample=%d dBFS ch0=%.2f ch1=%.2f ch2=%.2f ch3=%.2f"
                % (int(t * 122.88e6), -43.1 + r.rng.gauss(0, .3), -44.0 + r.rng.gauss(0, .3), -42.8 + r.rng.gauss(0, .3),
                   -44.6 + r.rng.gauss(0, .3)))
            # nr_pdcch_blind_monitor_rt.c summary + PDSCHQ lines
            r.L(t, "[PHY]   SENSING: blind PDCCH monitor summary: occasions=%d candidates=%d accepts=%d dci10[accepts=%d C=%d TC=0 "
                   "SI=%d RA=4 P=0] dci01[accepts=%d rejects=12] dci00[accepts=0 rejects=0] ulscan[sched=10 crc_hit=3 disc=1] "
                   "held[energy=10 dmrs=0 persist=33 snr=2 mismatch=5 rnti_set=1] efloor=1.20 cfr_submits=0 "
                   "pdsch_decode[try=0 crc_ok=0 (0.0%%) skip_rv=0 unsup=0 over_cap=0 data_submits=0 k0_wait=0] "
                   "scanq[queued=%d done=%d drop_full=%d drop_stale=%d maxlag=3] dci_wide_probes=0 last_reject=\"(none yet)\" "
                   "last_reject_rnti=0x0" % (c["occ"], c["cand"], c["acc"], c["acc"] // 3, c["accc"] // 4, int(t * 20),
                                            (c["acc"] // 5) if ul else 0, c["sq"], c["sp"], c["sfull"], c["sstale"]))
            r.L(t, "[PHY]   SENSING: PDSCHQ queued=%d decoded=%d crc_ok=%d (%.1f%%) dropped[full=%d stale=%d] max_lag_slots=6/20 "
                   "slot_groups=0/0 multi-grant slots dropped_narrow=0" % (c["pq"], c["pdec"], c["pok"], 100.0 * c["pok"] / max(1, c["pdec"]),
                                                                          c["pfull"], c["pstale"]))
            r.L(t, "[PHY]   SENSING: PDSCHQ per-rnti" + "".join(" 0x%x:%d/%d(%d%%)" % (x, 40, 50, 80) for x in rntis[:3]))
            r.L(t, "[PHY]   SENSING: LDPCDIAG ok=%d seg_fail=%d tb_fail=0 zero_tb=0 iface_err=0 segs_decoded=0/0 (0.0%%)" % (c["ldok"], c["ldseg"]))
            if ul:
                r.L(t, "[PHY]   SENSING: pusch_passive[try=%d crc_ok=%d (%.1f%%) seg_fail=0 zero_tb=0 (0.0%%) ta_refined=0 "
                       "uci[trials=4 rescued=1 unconfirmed=0] health=99.0%% unsup=0 setup_fail=0] init_tx[hit=0 evict=0] "
                       "ul_cfr[submits=0 re=0]" % (c["ptry"], c["pusok"], 100.0 * c["pusok"] / max(1, c["ptry"])))
            p50, p99 = int(320 * dec_scale), int(((tail_ms or p99_ms) * 1000) * dec_scale)
            p90 = (p50 + p99) // 2
            # nr_pdsch_passive_decode.c PDTIM / PDTIMPCT / PDTIMCLS
            r.L(t, "[PHY]   SENSING: PDTIM calls=%d fep[n=%d mean=91.0us cpu=88.0us max=900.0us tot=1.00s sd=10.0us] "
                   "chest[n=%d mean=70.0us cpu=69.0us max=700.0us tot=1.00s sd=9.0us] alloc[n=0 mean=0.0us cpu=0.0us max=0.0us tot=0.00s sd=0.0us] "
                   "demod[n=%d mean=%.1fus cpu=150.0us max=4000.0us tot=1.00s sd=40.0us] ldpc[n=%d mean=160.0us cpu=158.0us "
                   "max=3000.0us tot=1.00s sd=30.0us]" % (c["pdec"], c["pdec"], c["pdec"], c["pdec"], 155 * dec_scale, c["pdec"]))
            r.L(t, "[PHY]   SENSING: PDTIMPCT us fep[n=%d p50=85 p90=125 p99=305] chest[n=%d p50=65 p90=95 p99=255] "
                   "demod[n=%d p50=145 p90=205 p99=905] ldpc[n=%d p50=155 p90=225 p99=705] sum[n=%d p50=%d p90=%d p99=%d] "
                   "ldpc_tb=%d ldpc_cb=%d" % (c["pdec"], c["pdec"], c["pdec"], c["pdec"], c["pdec"], p50, p90, p99, c["pdec"], c["pdec"]))
            r.L(t, "[PHY]   SENSING: PDTIMCLS us real[n=%d mean=%.1f p50=%d p90=%d p99=%d max=%.1f gt5ms=%d gt20ms=%d "
                   "share_gt1/5/20ms=0.10/0.05/0.01] retry[n=%d mean=400.0 p50=380 p90=600 p99=900 max=1200.0 gt5ms=0 gt20ms=0 "
                   "share_gt1/5/20ms=0.00/0.00/0.00] probe[n=%d mean=300.0 p50=290 p90=400 p99=700 max=900.0 gt5ms=0 gt20ms=0 "
                   "share_gt1/5/20ms=0.00/0.00/0.00]" % (int(c["pdec"] * .8), 380 * dec_scale, p50, p90, p99, 4 * p99,
                                                         int(c["pdec"] * (0.02 if tail_ms else 0)), int(c["pdec"] * (0.005 if tail_ms else 0)),
                                                         int(c["pdec"] * .1), int(c["pdec"] * .1)))
        if t >= nxt_m:
            r.tick_metrics(nxt_m, metrics_state, pci, gpu)
            nxt_m += 20.0
    return t


def build(out):
    os.makedirs(out, exist_ok=True)
    good = {"0-4": 0.97, "5-9": 0.85, "10-16": 0.62, "17-27": 0.15}

    r = Run(out, "healthy", 1)
    preamble(r, 415)
    dedicated(r, 9.0, 415)
    r.L(30.0, "[PHY]   SENSING: ULLAYOUT LOCK len=37 bwp=216@0 riv=1200 bwp_ind=0 tda=2(k2=4 S0 L14 typeA) fh=0 dmrs_add_pos=1 "
              "tail[-] runner[-] cdm_ev[cdm1=3 cdm2=0] gold=1 mcs_fit=0.92/0.40 dcis=200 first_abs=1000 lock_abs=60000 t_conv_ms=29000")
    r.L(30.1, "[PHY]   SENSING: ACQ_STATE CORESET_VERIFIED -> UL_CONVERGED (updates=900 time_in_prev=700 regressions=0)")
    r.L(62.0, '[PHY]   SENSING: CSIRS_BLIND CONFIRMED #0 after 217 slots -- csirs_monitor = "1:0:52:1:4:0:0:3:415:4:0" (search continues)')
    for i in range(3):
        r.cfr_frame(5 + i, 0, 10000 + 40 * i, n_re=240)
        r.cfr_frame(62 + i, 1, 124000 + 40 * i, n_re=52 * 3)
    r.rec.append({"schema": 2, "type": "session", "abs_slot": 0, "t_mono_ns": T0_NS, "pci": 415, "rx_id": "x410-fixture",
                  "n_rx": 4, "rx_gain_db": 38, "carrier_dl_hz": 3680010000, "fs_hz": 122880000, "scs_khz": 30, "n_rb_dl": 217})
    common_loop(r, 10.0, 300.0, 415, [0x4ac6, 0x4e32, 0x51a0, 0x5c11, 0x60fe],
                {"0-4": 0.995, "5-9": 0.97, "10-16": 0.9, "17-27": 0.7}, p99_ms=0.45, dec_scale=0.9)
    r.write()

    r = Run(out, "sync_loss", 2)
    preamble(r, 415)
    dedicated(r, 9.0, 415)
    common_loop(r, 10.0, 140.0, 415, [0x4ac6, 0x4e32], good)
    r.L(141.0, "[HW]   SENSING: RFSTALL USRP_RX_READ reason=UHD metadata error received=0 requested=30720 metadata=ERROR_CODE_OVERFLOW (Overflow)")
    r.L(141.1, "[PHY]   SENSING: ACQ_STATE TRACKING -> LOST (receive-stream discontinuity, no hysteresis; losses=1 updates=5000 time_in_prev=4000)")
    r.c["losses"] += 1
    r.tick_metrics(160.0, "LOST", 415)
    r.L(170.0, "[PHY]   Initial sync successful, PCI: 415")
    r.L(170.1, "[PHY]   SENSING: ACQ_STATE LOST -> PBCH_LOCKED (updates=5001 time_in_prev=29 regressions=0)")
    common_loop(r, 180.0, 240.0, 415, [0x4ac6], good, metrics_state="PBCH_LOCKED")
    r.write()

    r = Run(out, "stale_tails", 3)
    preamble(r, 923, ssb_sc=233, gscn=8007, mhz=3759.99, kssb=10)
    dedicated(r, 9.0, 923)
    r.L(30.0, "[PHY]   SENSING: ULLAYOUT STATS bwp=PROVISIONAL 107@53 probes[booked=900 fed=880] votes=140 ctl_votes=0 discarded=3 "
              "tied=0 hyps=12 tally[ 107@53:7 107@54:6 ]")
    common_loop(r, 10.0, 240.0, 923, [0x4e32, 0x4b10, 0x4c77], {"0-4": 0.62, "5-9": 0.40, "10-16": 0.12, "17-27": 0.02},
                stale=0.38, scan_full=0.06, dec_scale=2.4, gpu=98.0, tail_ms=8.0)
    r.c["trips"] = 1
    r.tick_metrics(241.0, "TRACKING", 923, 99.0)
    r.write()

    r = Run(out, "ul_unlocked", 4)
    preamble(r, 508, ssb_sc=517, gscn=7895, mhz=3590.19, kssb=2)
    dedicated(r, 9.0, 508)
    r.L(40.0, "[PHY]   SENSING: ULLAYOUT STATS bwp=searching 0@0 probes[booked=300 fed=280] votes=12 ctl_votes=0 discarded=40 "
              "tied=3 hyps=24 tally[ 216@0:5 216@1:5 213@2:2 ]")
    common_loop(r, 10.0, 180.0, 508, [0x3301, 0x3302], good, ul=False)
    r.write()

    r = Run(out, "multi_cell", 5)
    preamble(r, 508, ssb_sc=517, gscn=7895, mhz=3590.19, kssb=2)
    common_loop(r, 3.0, 90.0, 508, [0x3301], good, ul=False, metrics_state="SIB1_DECODED")
    r.L(91.0, "[PHY]   SENSING: ACQ_STATE SIB1_DECODED -> LOST (receive-stream discontinuity, no hysteresis; losses=1 updates=10 time_in_prev=9)")
    r.c["losses"] += 1
    preamble(r, 4, ssb_sc=517, gscn=7895, mhz=3590.19, kssb=2, sib1=False, t=95.0)
    common_loop(r, 100.0, 160.0, 4, [0x1101], {"0-4": 0.9, "5-9": 0.5, "10-16": 0.2, "17-27": 0.0}, ul=False,
                metrics_state="PBCH_LOCKED")
    r.write()
    with open(os.path.join(r.dir, "cells.json"), "w") as f:   # fastscan.py output shape (docs/.../frontlive_data/cells.json)
        json.dump([{"gscn": 7895, "ssb_mhz": 3570.24, "pci": 508, "metric_db": 16.4, "sss_ratio": 3.89, "dmrs_ratio": 4.18,
                    "i_ssb": 1, "scs_khz": 30, "cfo_hz": -4761, "n_cells": 2, "capture": "synthetic"},
                   {"gscn": 7895, "ssb_mhz": 3570.24, "pci": 4, "metric_db": 9.1, "sss_ratio": 2.15, "dmrs_ratio": 2.24,
                    "i_ssb": 0, "scs_khz": 30, "cfo_hz": -5297, "n_cells": 2, "capture": "synthetic"},
                   {"gscn": 7971, "ssb_mhz": 3679.68, "pci": 415, "metric_db": 18.0, "sss_ratio": 4.47, "dmrs_ratio": 3.74,
                    "i_ssb": 4, "scs_khz": 30, "cfo_hz": -4849, "n_cells": 1, "capture": "synthetic"}], f, indent=1)
    return out


def stream(src, dst, speed):
    """Write src's files into dst in run-time order at `speed` x real time (a fake live receiver)."""
    os.makedirs(dst, exist_ok=True)
    for f in ("label.txt", "env.txt"):
        if os.path.exists(os.path.join(src, f)):
            shutil.copy(os.path.join(src, f), dst)
    items = []
    t_log = 0.0
    anchor = None
    for line in open(os.path.join(src, "run.log")):
        if "ISAC_METRICS {" in line:
            anchor = (json.loads(line.split("ISAC_METRICS ", 1)[1])["t_mono_ns"] - T0_NS) / 1e9
        items.append((anchor if anchor is not None else t_log, "run.log", line))
    # log lines between anchors get their anchor time; good enough for a feed
    for name in ("metrics.jsonl", "obs.jsonl"):
        for line in open(os.path.join(src, name)):
            items.append(((json.loads(line)["t_mono_ns"] - T0_NS) / 1e9, name, line))
    items.sort(key=lambda x: x[0])
    fh = {n: open(os.path.join(dst, n), "a") for n in ("run.log", "metrics.jsonl", "obs.jsonl")}
    start = time.time()
    for t, name, line in items:
        delay = start + t / speed - time.time()
        if delay > 0:
            for h in fh.values():
                h.flush()
            time.sleep(delay)
        fh[name].write(line)
    for h in fh.values():
        h.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build"); b.add_argument("out")
    s = sub.add_parser("stream"); s.add_argument("src"); s.add_argument("dst"); s.add_argument("--speed", type=float, default=10.0)
    a = ap.parse_args()
    if a.cmd == "build":
        print(build(a.out))
    else:
        stream(a.src, a.dst, a.speed)
