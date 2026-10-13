#!/usr/bin/env python3
"""Dashboard self-checks: log/JSON parsers, alerts, tiles, empty states, file readers, replay, HTTP API.

Every log line below is formatted from the receiver's own format string (file named in the comment), so a format
change in the C code that breaks the dashboard fails here once this line is updated to the new format.

    python3 -m unittest test_monitor.py      (or: python3 test_monitor.py)
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_fixtures  # noqa: E402
import monitor  # noqa: E402
import rxparse  # noqa: E402
from rxparse import RunState, alerts, cell_rows, kpis, latency_view, summary_md, tiles, ul_reason  # noqa: E402


def feed(lines, t=None):
    s = RunState()
    for i, l in enumerate(lines):
        s.feed_line(l, t if t is not None else float(i))
    return s


class LineParsers(unittest.TestCase):
    """One test per log prefix the dashboard depends on (PROJECT_MEMORY §11)."""

    def test_sync_chain_and_cell_facts(self):
        s = feed([
            "[NR_PHY]   Cell Detected with GSCN: 7971, SSB SC offset: 1171, SSB Ref: 3679680000.000000, PSS Corr peak: 92 dB, PSS Corr Average: 71",  # nr_initial_sync.c
            "[PHY]   [UE 0] Measured Carrier Frequency offset -4849 Hz",                                                                           # nr_initial_sync.c
            "[PHY]   Initial sync successful, PCI: 415",                                                                                          # nr_initial_sync.c
            "[NR_MAC]   SENSING: MIB fields sfn=12 k_ssb=14 scs_common=1 coreset0=12 ss0=5 dmrs_typeA=2 barred=1 intraFreqResel=1",              # nr_ue_procedures.c
            "[NR_RRC]   SIB1 decoded",                                                                                                         # rrc_UE.c
            "[NR_RRC]   SENSING: SIB1 CELL mcc=222 mnc=10 cell_identity=0x007ed8847 tac=-1 n_plmn=1",                                          # rrc_UE.c
            "[NR_MAC]   PASSIVE: cell config extracted from SIB1 -- PCI=415 DL: 217 PRB @ SCS=1 offsetToPointA=0 UL: 217 PRB @ SCS=1 absoluteFrequencyPointA=640000",  # config_ue.c
            "[PHY]   SENSING: TDD from SIB1 DERIVED: p1 period=10 slots dl=7+6sym ul=2+4sym | p2 period=0 dl=0+0sym ul=0+0sym",              # nr_passive_acq_state.c
            "[PHY]   SENSING: ACQ carrier CONFIRMED from SIB1: 217 PRB mu=1, Point A at grid subcarrier 0, carrier ends at 2604 = grid end "
            "(started with 217 PRB mu=1, SSB found at subcarrier 1171); derived carrier centre 3680.010000 MHz (started 3680.010000 MHz)",  # nr_passive_acq_state.c
            "[PHY]   SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(groups=8 dur=1 bundle=6 interleaver=2 shift=415 scramb=415) bwp=[16..64) "
            "(cset_start_rb = ssb_offset_point_a 97 - rb_offset 16) ss(period=20 offset=0 dur=2 symb=0) dci10(mux=1 sib1=1)",            # nr_pdcch_blind_monitor.c
            "[PHY]   SENSING: multi-CORESET bank add index=0 offset=0 span=216 symbol=0 mapping=0/0/0 len=47",                                # nr_pdcch_coreset_bank.c
            "[PHY]   SENSING: SEARCH_SPACE INFERRED [USS] by CRC-recovered grants: monitored AL={1,2} confirmed/examined[AL1=4/9 AL2=3/5 "
            "AL4=0/2 AL8=0/0] total_confirmed=7 (accepts per candidate EXAMINED; ...)",                                                    # nr_pdcch_blind_monitor_rt.c
            "[PHY]   SENSING: PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)] n_id=415 (assumed = PCI 415) by CRC-recovered RNTI 0x4ac6 -- x",  # nr_pdcch_blind_monitor_rt.c
            "[PHY]   SENSING: DCI 1_1 length locked coreset=0 rnti=0x4ac6 len=47 occasions=9",                                                # nr_pdcch_blind_monitor_rt.c
            '[PHY]   SENSING: CSIRS_BLIND CONFIRMED #0 after 217 slots -- csirs_monitor = "1:0:52:1:4:0:0:3:415:4:0" (search continues)', # nr_csirs_blind_rt.c
            "[PHY]   SENSING: Technique D CONVERGED rnti=0x4ac6 tda=1 S=2 L=12 mask=0x884 table=1 k0=0 map=A dci=1_1 sib1_row=none",       # §11.8
        ])
        snap = s.snapshot()
        f = snap["cells"]["415"]["facts"]
        self.assertEqual(f["pci"]["value"], 415)
        self.assertEqual(f["gscn"]["value"], 7971)                 # detection logged before the PCI is attached at sync
        self.assertAlmostEqual(f["ssb_freq_mhz"]["value"], 3679.68)
        self.assertEqual(f["cfo_hz"]["value"], -4849)
        self.assertEqual(f["k_ssb"]["value"], 14)
        self.assertEqual(f["plmn"]["value"], "222-10")
        self.assertEqual(f["tac"]["state"], "unknown")
        self.assertEqual(f["dl_nrb"]["value"], 217)
        self.assertEqual(f["carrier_verdict"]["value"], "CONFIRMED")
        self.assertIn("period 10 slots", f["tdd"]["value"])
        self.assertEqual(snap["cells"]["415"]["coresets"]["0"]["dci_len"], 47)
        self.assertEqual(snap["cells"]["415"]["scrambling"]["USS(dedicated)"]["n_id"], 415)
        self.assertEqual(snap["cells"]["415"]["csirs"]["0"]["resource"], "1:0:52:1:4:0:0:3:415:4:0")
        self.assertEqual(snap["td_conv"]["0x4ac6"]["mcs_table"], 1)
        self.assertNotIn("?", snap["cells"])                       # nothing learned before the PCI leaks into a "?" cell
        done = [st["done"] for st in snap["sync"]["stages"]]
        self.assertEqual(done, [True, True, True, True, True, True, True, False])
        items = {r["item"]: r for r in cell_rows(snap, "415")}
        self.assertEqual(items["Operator code (PLMN, MCC-MNC)"]["how"], "SIB1")
        self.assertEqual(items["Search space USS"]["state"], "provisional")
        self.assertEqual(items["Uplink BWP (PUSCH layout)"]["state"], "unknown")

    def test_carrier_mismatch_and_tdd_absent(self):
        s = feed(["[PHY]   Initial sync successful, PCI: 508",
                  "[PHY]   SENSING: ACQ carrier MISMATCH: SIB1 says 162 PRB mu=1 (Point A at grid subcarrier 972, carrier end 2916) x",
                  "[PHY]   SENSING: TDD from SIB1 ABSENT (tdd-UL-DL-ConfigurationCommon not in SIB1 -> FDD or pattern unknown)"])
        f = s.snapshot()["cells"]["508"]["facts"]
        self.assertEqual(f["carrier_verdict"]["value"], "MISMATCH")
        self.assertEqual(f["carrier_nrb"]["value"], 162)
        self.assertEqual(f["tdd"]["state"], "unknown")
        self.assertTrue(any(e["kind"] == "carrier" for e in s.events))

    def test_pdcch_summary_and_queues(self):
        s = feed([
            '[PHY]   SENSING: blind PDCCH monitor summary: occasions=100 candidates=5000 accepts=30 dci10[accepts=10 C=4 TC=0 SI=6 RA=0 P=0] '
            'dci01[accepts=5 rejects=1] dci00[accepts=0 rejects=0] ulscan[sched=1 crc_hit=0 disc=0] held[energy=1 dmrs=0 persist=2 snr=3 '
            'mismatch=4 rnti_set=5] efloor=1.00 cfr_submits=0 pdsch_decode[try=0 crc_ok=0 (0.0%) skip_rv=0 unsup=0 over_cap=0 data_submits=0 '
            'k0_wait=0] scanq[queued=100 done=98 drop_full=2 drop_stale=1 maxlag=3] dci_wide_probes=0 last_reject="x" last_reject_rnti=0x0',  # nr_pdcch_blind_monitor_rt.c
            "[PHY]   SENSING: PDSCHQ queued=50 decoded=40 crc_ok=36 (90.0%) dropped[full=1 stale=9] max_lag_slots=6/20 slot_groups=0/0 "
            "multi-grant slots dropped_narrow=0",                                                                                     # nr_pdcch_blind_monitor_rt.c
            "[PHY]   SENSING: PDSCHQ per-rnti 0x4ac6:40/50(80%) 0x4e32:9/60(15%)",                                                       # nr_pdsch_passive_queue.c
            "[PHY]   SENSING: PUSCHQ queued=9 decoded=8 crc_ok=7 dropped[full=0 stale=1] max_lag_slots=2/20",                           # nr_pusch_passive_monitor_rt.c
            "[PHY]   SENSING: LDPCDIAG ok=36 seg_fail=4 tb_fail=0 zero_tb=1 iface_err=0 segs_decoded=0/0 (0.0%)",                        # nr_pdsch_passive_decode.c
            "[PHY]   SENSING: pusch_passive[try=8 crc_ok=7 (87.5%) seg_fail=1 zero_tb=0 (0.0%) ta_refined=0 uci[trials=3 rescued=1 "
            "unconfirmed=0] health=99.0% unsup=0 setup_fail=0] init_tx[hit=0 evict=0] ul_cfr[submits=0 re=0]",                          # nr_pusch_passive_decode.c
        ])
        snap = s.snapshot()
        self.assertEqual(snap["pdcch"]["dci10"]["SI"], 6)
        self.assertEqual(snap["pdcch"]["held"]["rnti_set"], 5)
        self.assertEqual(snap["scanq"]["drop_full"], 2)
        self.assertEqual(snap["pdschq"]["drop_stale"], 9)
        self.assertEqual(snap["pdschq_rnti"]["4e32"], {"ok": 9, "decoded": 60})
        self.assertEqual(snap["puschq"]["crc_ok"], 7)
        self.assertAlmostEqual(snap["ldpc"]["tb_rate"], 90.0)
        self.assertEqual(snap["pusch"]["uci_rescued"], 1)

    def test_radio_lines(self):
        s = feed([
            "[HW]   RFCHAN ch0 subdev=A:0 port=RX1 gain=38.00 range=[0.0..60.0] freq=3680.010000 MHz rate=122.880000 Msps bw=100.000000 MHz",  # usrp_lib.cpp
            "[HW]   SENSING: CHPOW sample=12345 dBFS ch0=-43.10 ch1=-44.00 ch2=-60.70 ch3=-44.60",                                        # usrp_lib.cpp
            "[HW]   SENSING: RFSTALL USRP_RX_READ reason=x received=0 requested=30720 metadata=ERROR_CODE_OVERFLOW (Overflow)",           # usrp_lib.cpp
            "[PHY]   SENSING: RFCENSUS slots=2000 ssb_slots=50 pbch_ok=50 pbch_fail=0 rf_pow=1.00 ref=1.00 bad=0 shiftForNextFrame=0 max_pos_acc=10 frame=1",  # nr-ue.c
        ])
        snap = s.snapshot()
        self.assertEqual(snap["radio"]["rfchan"]["ch0"]["gain_db"], 38.0)
        self.assertEqual(snap["radio"]["chpow"]["ch2"], -60.7)
        self.assertEqual(snap["radio"]["rfstall_n"], 1)
        self.assertEqual(snap["radio"]["rfcensus"]["pbch_ok"], 50)
        self.assertEqual(snap["tuned_mhz"], 3680.01)

    def test_timing_lines(self):
        s = feed([
            "[PHY]   SENSING: PDTIM calls=200 fep[n=200 mean=91.0us cpu=88.0us max=900.0us tot=1.00s sd=10.0us] ldpc[n=200 mean=160.0us cpu=158.0us max=3000.0us tot=1.00s sd=30.0us]",
            "[PHY]   SENSING: PDTIMPCT us fep[n=200 p50=85 p90=125 p99=305] sum[n=200 p50=320 p90=560 p99=1100] ldpc_tb=200 ldpc_cb=200",
            "[PHY]   SENSING: PDTIMCLS us real[n=160 mean=380.0 p50=315 p90=545 p99=4005 max=8000.0 gt5ms=2 gt20ms=0 share_gt1/5/20ms=0.10/0.05/0.01] "
            "retry[n=20 mean=400.0 p50=380 p90=600 p99=900 max=1200.0 gt5ms=0 gt20ms=0 share_gt1/5/20ms=0.00/0.00/0.00]",
            "[PHY]   SENSING: PDTIMCLSST ms(<1/1-5/5-20/>20ms bodies) real[fep=1/2/3/4 ] ",
        ])  # nr_pdsch_passive_decode.c
        snap = s.snapshot()
        self.assertEqual(snap["timing"]["pdtim"]["fep"]["mean"], 91.0)
        self.assertEqual(snap["timing"]["pdtimpct"]["sum"]["p99"], 1100)
        lat = latency_view(snap)
        self.assertEqual((lat["p50"], lat["p99"]), (315, 4005))
        self.assertIn("PDTIMCLS", lat["src"])
        s2 = feed(["[PHY]   SENSING: PDTIMPCT us sum[n=9 p50=300 p90=400 p99=700] ldpc_tb=9 ldpc_cb=9"])
        self.assertIn("PDTIMPCT", latency_view(s2.snapshot())["src"])

    def test_evmgrant_line(self):
        line = ("[PHY]   SENSING: EVMGRANT rnti=0x4ac6 12.7 crc=1 mcs=13 tbl=1 Qm=4 Nl=2 rb=10+24 nsym=12 evm=20.0 snr=14.0 "
                "br_snr=1.0,2.0,3.0,4.0 nbrx=4 tok=1 snr_cpe=14.0 probe=0 kappa=0.90 alpha=1.00,1.00,1.00,1.00 nvar_db=36.0,36.0,36.0,36.0 "
                "rho=0.10 comb=3 ports=0x3 cdm=1 ndmrs=1 eq=0 occ=0 it=6 dec_us=420 rv=0 ndi=1 pid=3 tbs=8456 snr_l=14.0,12.0,-99.0,-99.0 l2=0 zabs=1000")  # nr_pdsch_passive_queue.c
        s = feed([line, "[PHY]   Initial sync successful, PCI: 1"], t=5.0)
        g = s.snapshot()["grants"]["?"]["DL"]
        self.assertEqual(g["grants"], 1)
        self.assertEqual(g["by_mcs"]["10-16"]["ok"], 1)
        self.assertEqual(g["by_nl"]["2"]["n"], 1)
        self.assertEqual(g["dec_us"]["p50"], 420.0)
        self.assertEqual(g["rntis"][0]["modulation"], "16QAM")

    def test_ul_lock_lines_and_reasons(self):
        base = ["[PHY]   Initial sync successful, PCI: 1"]
        s = feed(base)
        self.assertIn("No UL DCI", ul_reason(s.snapshot()))
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT STATS bwp=searching 0@0 probes[booked=3 fed=2] votes=12 ctl_votes=0 discarded=0 tied=0 hyps=24 tally[ ]"])
        self.assertIn("12 votes over 24 hypotheses", ul_reason(s.snapshot()))
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT STATS bwp=PROVISIONAL 107@53 probes[booked=3 fed=2] votes=140 ctl_votes=0 discarded=0 tied=0 hyps=12 tally[ ]"])
        self.assertIn("provisional", ul_reason(s.snapshot()))
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT LOCK len=37 bwp=216@0 riv=1 bwp_ind=0 tda=2(k2=4 S0 L14 typeA) fh=0 dmrs_add_pos=1 tail[-] runner[-]"])
        self.assertEqual(s.ul["state"], "LOCKED")
        self.assertIn("216 RB @ start 0", ul_reason(s.snapshot()))
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT BWP_LOCK bwp=107@53 votes=9 runner=107@54:3 rntis=2 others[ -] ctl_votes=0"])
        self.assertEqual(s.ul["bwp"], "107 RB @ start 53")
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT_CRC_LOCK len=37 bwp=216@1 riv=1 bwp_ind=0 tda=2(k2=4) fh=0 tail[-] crc_ok=3 trials=9 abs=1"])
        self.assertIn("CRC lock", s.ul["how"])
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT PRIOR_CONFIRM bwp=216@0 by=crc crc_expected_n=3 crc_ok=3 dec=3 demotions=0 slot=1"])
        self.assertEqual(s.ul["state"], "LOCKED")
        s = feed(base + ["[PHY]   SENSING: ULLAYOUT LOCK len=37 bwp=216@0 riv=1", "[PHY]   SENSING: ULLAYOUT reset: cell changed (PCI 1->2, carrier 1->2)"])
        self.assertIsNone(s.ul["state"])
        s = feed(base + ["UL automatic DCI length locked: 37 rnti=0x4ac6 (occasions=9 polar_decodes=99)"])
        self.assertEqual(s.ul["dci01_len"], 37)

    def test_state_lost_resets_stages_and_epoch_event(self):
        s = feed(["[PHY]   Initial sync successful, PCI: 1",
                  "[PHY]   SENSING: ACQ_STATE PBCH_LOCKED -> LOST (receive-stream discontinuity, no hysteresis; losses=1 updates=5 time_in_prev=4)",
                  "[PHY]   SENSING: CONFIG_EPOCH 1 -> 2 class=SOFT cause=CSIRS_MAP_CHANGE scope=x"])  # nr_passive_cfg_epoch.c
        snap = s.snapshot()
        self.assertEqual(snap["sync"]["acq_state"], "LOST")
        self.assertEqual(snap["sync"]["current"], 0)
        self.assertEqual(snap["sync"]["sync_losses"], 1)
        self.assertTrue(any(e["kind"] == "reconfig" for e in snap["events"]))
        self.assertIn("sync_lost", [a["id"] for a in alerts(snap)])

    def test_metrics_line_in_log_and_dedupe_with_file(self):
        m1 = {"schema": 1, "t_mono_ns": 10**12, "abs_slot": 1, "pci": 0, "acq_state": "SEARCHING", "pdschq_decoded": 0}
        s = RunState()
        s.feed_line("[PHY]   SENSING: ISAC_METRICS " + json.dumps(m1))
        s.feed_metrics(dict(m1))
        self.assertEqual(s.metrics_n, 1)
        self.assertEqual(s.snapshot()["metrics"]["acq_state"], "SEARCHING")

    def test_unknown_and_garbage_lines_are_harmless(self):
        s = feed(["", "SENSING: ", "SENSING: EVMGRANT broken", "\x1b[0m random text", "SENSING: ISAC_METRICS {not json",
                  "SENSING: PDTIMCLS us real[garbage]", "SENSING: ULLAYOUT FIT len=3 dcis=1 not yet"])
        json.dumps(s.snapshot(), allow_nan=False)


class Windows(unittest.TestCase):
    def test_window_rates_and_counter_reset(self):
        a = {"t_mono_ns": 0, "pdschq_queued": 100, "pdschq_decoded": 90, "pdschq_crc_ok": 80, "pdschq_drop_stale": 10,
             "scanq_queued": 1000, "scanq_drop_full": 10, "pusch_try": 10, "pusch_crc_ok": 5}
        b = dict(a, t_mono_ns=20 * 10**9, pdschq_queued=300, pdschq_decoded=270, pdschq_crc_ok=260, pdschq_drop_stale=30,
                 scanq_queued=3000, scanq_drop_full=60, pusch_try=20, pusch_crc_ok=15)
        w = rxparse.window(a, b, 20.0)
        self.assertAlmostEqual(w["crc_pct"], 100 * 180 / 180)
        self.assertAlmostEqual(w["stale_pct"], 10.0)
        self.assertAlmostEqual(w["drop_full_pct"], 2.5)
        self.assertAlmostEqual(w["decoded_per_s"], 9.0)
        self.assertAlmostEqual(w["pusch_crc_pct"], 100.0)
        r = rxparse.window(b, dict(b, t_mono_ns=40 * 10**9, pdschq_decoded=5, pdschq_crc_ok=5), 40.0)
        self.assertNotIn("crc_pct", r)                       # negative delta = receiver restart: no rate

    def test_hist_percentiles(self):
        self.assertEqual(rxparse.hist_percentiles([0, 0, 10, 0, 0, 0, 0, 0, 0]), [500, 500, 500])
        self.assertEqual(rxparse.hist_percentiles([0] * 9), [None, None, None])
        self.assertEqual(rxparse.hist_percentiles([0, 0, 0, 0, 0, 0, 0, 0, 1])[0], float("inf"))


class AlertsAndTiles(unittest.TestCase):
    def base(self):
        return RunState().snapshot()

    def test_empty_state_tiles_explain(self):
        t = {x["id"]: x for x in tiles(self.base())}
        self.assertEqual(t["dl"]["level"], "idle")
        self.assertIn("waiting for an RNTI", t["dl"]["sentence"])
        self.assertEqual(t["radio"]["level"], "idle")
        self.assertEqual(t["decode"]["level"], "idle")
        self.assertEqual(alerts(self.base()), [])
        self.assertIn("not synchronised", ul_reason(self.base()))

    def test_thresholds_are_honoured(self):
        s = feed(["[PHY]   SENSING: PDTIMCLS us real[n=10 mean=600.0 p50=500 p90=700 p99=900 max=1000.0 gt5ms=0 gt20ms=0 share_gt1/5/20ms=0/0/0]"])
        snap = s.snapshot()
        self.assertIn("p99", [a["id"] for a in alerts(snap)])
        self.assertNotIn("p99", [a["id"] for a in alerts(snap, {"p99_target_us": 1000})])
        snap["window"] = {"stale_pct": 12.0, "drop_full_pct": 3.0}
        ids = [a["id"] for a in alerts(snap)]
        self.assertIn("stale", ids)
        self.assertIn("drop_full", ids)
        self.assertNotIn("stale", [a["id"] for a in alerts(snap, {"stale_pct": 20})])
        snap["metrics"] = {"gpu_budget_used_pct": 97.0, "ldpc_cuda_breaker_trips": 2}
        ids = [a["id"] for a in alerts(snap)]
        self.assertIn("gpu", ids)
        self.assertIn("ldpc_breaker", ids)

    def test_host_alerts_live_only(self):
        snap = self.base()
        host = {"live": True, "silent_s": 120.0, "mem_avail_gb": 10.0, "temp_c": 85.0}
        ids = [a["id"] for a in alerts(snap, None, host)]
        self.assertEqual(sorted(ids), ["ram", "silent", "temp"])
        self.assertEqual(alerts(snap, None, dict(host, live=False, mem_avail_gb=None, temp_c=None)), [])
        comp = {x["id"]: x for x in tiles(snap, None, host)}["compute"]
        self.assertEqual(comp["level"], "bad")

    def test_dl_tile_levels(self):
        snap = self.base()
        snap["grants"] = {"1": {"src": "x", "DL": {"grants": 100, "crc_ok": 95, "crc_pct": 95.0, "grants_per_s_recent": 5.0,
                                                    "tput_bps_recent": 1.0}}}
        self.assertEqual({x["id"]: x for x in tiles(snap)}["dl"]["level"], "ok")
        snap["window"] = {"crc_pct": 60.0}
        self.assertEqual({x["id"]: x for x in tiles(snap)}["dl"]["level"], "warn")
        snap["window"] = {"crc_pct": 20.0}
        self.assertEqual({x["id"]: x for x in tiles(snap)}["dl"]["level"], "bad")


class Fixtures(unittest.TestCase):
    """The five synthetic run directories through the real readers (make_fixtures.py)."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="rxdash_")
        make_fixtures.build(cls.dir)
        cls.runs = {}
        for n in ("healthy", "sync_loss", "stale_tails", "ul_unlocked", "multi_cell"):
            r = monitor.Run(n, monitor.discover(os.path.join(cls.dir, n)), live=False)
            r.advance()
            cls.runs[n] = r

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def snap(self, n):
        return self.runs[n].state.snapshot()

    def test_healthy_is_all_green(self):
        t = {x["id"]: x["level"] for x in tiles(self.snap("healthy"))}
        self.assertEqual(t, {"radio": "ok", "sync": "ok", "dl": "ok", "ul": "ok", "decode": "ok", "compute": "ok"})
        self.assertEqual(alerts(self.snap("healthy")), [])
        s = self.snap("healthy")
        self.assertEqual(s["cfr"]["by_source"].get("csirs"), 3)
        self.assertEqual(s["sense"]["by_type"]["session"], 1)
        self.assertEqual(s["config"]["env"]["ISAC_RX_MRC_MODE"], "1")
        self.assertIn("--ssb", s["config"]["cmdline"])

    def test_sync_loss(self):
        s = self.snap("sync_loss")
        ids = [a["id"] for a in alerts(s)]
        self.assertIn("rfstall", ids)
        self.assertEqual(s["sync"]["sync_losses"], 1)
        self.assertEqual(s["sync"]["sync_count"], 2)
        kinds = [e["kind"] for e in s["events"]]
        self.assertLess(kinds.index("rfstall"), kinds.index("sync_lost"))

    def test_stale_tails(self):
        s = self.snap("stale_tails")
        ids = [a["id"] for a in alerts(s)]
        for a in ("p99", "stale", "gpu", "ldpc_breaker", "drop_full"):
            self.assertIn(a, ids)
        self.assertEqual({x["id"]: x for x in tiles(s)}["decode"]["level"], "bad")
        self.assertEqual(s["ul"]["state"], "PROVISIONAL")

    def test_ul_unlocked(self):
        s = self.snap("ul_unlocked")
        self.assertIn("Searching the UL layout", ul_reason(s))
        self.assertEqual({x["id"]: x for x in tiles(s)}["ul"]["level"], "idle")
        self.assertIsNone(s["pusch"].get("try") or None)

    def test_multi_cell_never_pooled(self):
        s = self.snap("multi_cell")
        self.assertEqual(sorted(s["cells"]), ["4", "508"])
        self.assertEqual(sorted(s["grants"]), ["4", "508"])
        self.assertIn("plmn", s["cells"]["508"]["facts"])
        self.assertNotIn("plmn", s["cells"]["4"]["facts"])           # PCI 4 has no SIB1
        self.assertEqual({v["pci"] for v in s["td_conv"].values()}, {4, 508})
        k4, k508 = kpis(s, "4"), kpis(s, "508")
        self.assertNotEqual(k4["by_mcs"]["0-4"]["n"], k508["by_mcs"]["0-4"]["n"])
        self.assertEqual(kpis(s)["by_mcs"], k4["by_mcs"])           # default = the current cell, not a pool
        self.assertEqual(len(monitor.scan_cells([os.path.join(self.dir, "multi_cell", "cells.json")])), 3)

    def test_times_from_anchors(self):
        s = self.snap("healthy")
        ts = [w["t"] for w in s["series"]]
        self.assertTrue(all(b > a for a, b in zip(ts, ts[1:])))
        early = [e for e in s["events"] if e["kind"] == "sync"]
        self.assertIsNone(early[0]["t"])                              # before the first anchor: no clock, not a guess
        ul = [e for e in s["events"] if e["kind"] == "ul_lock"][0]
        self.assertGreater(ul["t"], 15.0)

    def test_summary_markdown_carries_label(self):
        r = self.runs["stale_tails"]
        md = summary_md(r.state.snapshot(), monitor.derive_label(r), r.name)
        self.assertIn(make_fixtures.LABEL, md)
        self.assertIn("| 0-4 |", md)
        self.assertIn("Decode p99", md)

    def test_replay_matches_full_run_and_holds_future_lines(self):
        files = monitor.discover(os.path.join(self.dir, "healthy"))
        r = monitor.Run("x", files, live=False)
        r.advance(60.0)
        s60 = r.state.snapshot()
        self.assertLessEqual(max(w["t"] for w in s60["series"]), 60.0)
        self.assertLess(s60["grants"]["415"]["DL"]["grants"], self.snap("healthy")["grants"]["415"]["DL"]["grants"])
        r.advance(None)
        full = r.state.snapshot()
        ref = self.snap("healthy")
        self.assertEqual(full["grants"]["415"]["DL"]["grants"], ref["grants"]["415"]["DL"]["grants"])
        self.assertEqual(full["lines"], ref["lines"])
        self.assertEqual(full["metrics_n"], ref["metrics_n"])

    def test_live_tail_partial_lines(self):
        d = tempfile.mkdtemp(prefix="rxlive_")
        try:
            log = os.path.join(d, "run.log")
            open(log, "w").write("[PHY]   Initial sync successful, PCI: 7\n[PHY]   SENSING: PDSCHQ queued=5 decoded=4 crc_ok=")
            r = monitor.Run("live", monitor.discover(d), live=True)
            r.advance()
            self.assertEqual(r.state.cur_pci, 7)
            self.assertEqual(r.state.pdschq, {})                     # half a line is not parsed
            open(log, "a").write("3 (75.0%) dropped[full=0 stale=1] max_lag_slots=2/20\n")
            r.advance()
            self.assertEqual(r.state.pdschq["crc_ok"], 3)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_read_only(self):
        d = os.path.join(self.dir, "ul_unlocked")
        before = {f: os.path.getmtime(os.path.join(d, f)) for f in os.listdir(d)}
        r = monitor.Run("ro", monitor.discover(d), live=False)
        r.advance()
        monitor.log_lines(r.files["log"], "EVMGRANT", False, 10)
        self.assertEqual(before, {f: os.path.getmtime(os.path.join(d, f)) for f in os.listdir(d)})

    def test_cfr_reader_rejects_bad_framing(self):
        d = tempfile.mkdtemp(prefix="rxcfr_")
        try:
            p = os.path.join(d, "cfr.bin")
            open(p, "wb").write(b"XXXX" + b"\0" * 60)
            st = RunState()
            monitor.CfrReader(p, False).read(st)
            self.assertEqual(st.cfr["frames"], 0)
            hdr = struct.pack("<4sHHQBBHIQffII", b"NRCF", 1, 48, 0, 1, 2, 0, 4, 9, 0.0, 0.1, 0, 0)
            body = struct.pack("<4H", 1, 2, 3, 4) + bytes(4) + struct.pack("<16f", *([1.0] * 16))
            open(p, "wb").write(hdr + body + hdr[:10])                    # a complete frame + a partial one
            st = RunState()
            monitor.CfrReader(p, True).read(st)
            self.assertEqual(st.cfr["frames"], 1)
            self.assertEqual(st.cfr["last"]["csirs"]["k"], [1, 2, 3, 4])
            self.assertAlmostEqual(st.cfr["last"]["csirs"]["mag"][0][0], 3.01, places=2)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_env_parsing(self):
        self.assertEqual(monitor.parse_env_text("ISAC_A=1 ISAC_B=x=y\nNOT a=b\nISAC_C=\n"), {"ISAC_A": "1", "ISAC_B": "x=y", "ISAC_C": ""})
        self.assertEqual(monitor.flag_area("ISAC_PDSCH_GPU_HOST_PIN"), "PDSCH decode")
        self.assertEqual(monitor.flag_area("ISAC_ZZZ"), "Other")


class Http(unittest.TestCase):
    """The API end to end on a real server (127.0.0.1, ephemeral port)."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="rxhttp_")
        make_fixtures.build(cls.dir)
        args = monitor.argparse.Namespace(run=os.path.join(cls.dir, "healthy"), follow=None, log=None, metrics=None, obs=None,
                                          records=None, cfr=None, runs_root=[os.path.join(cls.dir, "*")],
                                          scan=[os.path.join(cls.dir, "multi_cell", "cells.json")], label=None, offline=True,
                                          poll=0.2, port=0, bind="127.0.0.1")
        cls.mgr = monitor.Manager(args)
        cls.mgr.primary.advance()
        cls.srv = monitor.ThreadingHTTPServer(("127.0.0.1", 0), monitor.make_handler(cls.mgr))
        cls.base = "http://127.0.0.1:%d" % cls.srv.server_address[1]
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        shutil.rmtree(cls.dir, ignore_errors=True)

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=30) as r:
            return r.status, r.read()

    def test_endpoints(self):
        code, body = self.get("/")
        self.assertIn(b"Receiver Dashboard", body)
        runs = json.loads(self.get("/api/runs")[1])
        ids = [r["id"] for r in runs["runs"]]
        self.assertEqual(ids[0], "primary")
        self.assertEqual(len(ids), 5)                          # primary + 4 other fixture dirs (no duplicate of the primary)
        st = json.loads(self.get("/api/state?thr=%7B%22p99_target_us%22%3A100%7D")[1])
        self.assertEqual(st["meta"]["cell"], "415")
        self.assertIn("p99", [a["id"] for a in st["alerts"]])  # threshold from the page settings is applied
        self.assertTrue(st["cell_rows"])
        other = [r["id"] for r in runs["runs"] if r["name"] == "multi_cell"][0]
        st2 = json.loads(self.get("/api/state?run=%s&cell=508" % other)[1])
        self.assertEqual(st2["meta"]["cell"], "508")
        rep = json.loads(self.get("/api/state?run=primary&t=40")[1])
        self.assertEqual(rep["meta"]["replay_t"], 40.0)
        self.assertGreater(rep["meta"]["duration"], 200)
        cmp_ = json.loads(self.get("/api/compare?runs=primary,%s" % other)[1])
        self.assertEqual(len(cmp_["runs"]), 2)
        self.assertTrue(all(r["label"] for r in cmp_["runs"]))
        md = self.get("/api/summary.md")[1].decode()
        self.assertIn("Evidence label", md)
        lg = json.loads(self.get("/api/log?q=Initial&key=1&n=5")[1])
        self.assertTrue(lg["lines"] and all("Initial" in l for l in lg["lines"]))
        self.assertEqual(len(json.loads(self.get("/api/scan")[1])["cells"]), 3)
        self.assertGreater(len(json.loads(self.get("/api/flags")[1])), 100)
        self.assertEqual(self.get("/health")[0], 200)

    def test_unknown_run_is_refused(self):
        with self.assertRaises(urllib.error.HTTPError) as cm:
            self.get("/api/state?run=h000000000000")
        self.assertEqual(cm.exception.code, 404)
        with self.assertRaises(urllib.error.HTTPError):
            self.get("/api/log/download?run=../../etc")


if __name__ == "__main__":
    unittest.main()
