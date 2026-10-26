"""Unit tests: histogram, SFN unwrap, parsers, aggregation (retry attribution, overlap-safe spans), dataset schemas,
ground-truth matching, cold-start denominators. Run: cd tools/mobisys && python3 -m unittest -v"""
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agg  # noqa: E402
import groundtruth  # noqa: E402
import hist  # noqa: E402
import make_synthetic  # noqa: E402
import parse  # noqa: E402
import plots  # noqa: E402
import policy  # noqa: E402
import store  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def jload(p):
    with open(p) as f:
        return json.load(f)


def evm(rnti=0x4601, frame=10, slot=3, crc=1, pid=2, e2e=(100, 50, 60, 300), probe=0):
    line = make_synthetic._evm(None, rnti, frame, slot, crc, 12, 2, (5, 20), pid, 3000, 15.0, list(e2e))
    return line.replace("probe=0", "probe=%d" % probe)


def collect(mode="lab"):
    rows = []
    return agg.Aggregator(lambda d, r: rows.append((d, r)), mode), rows


class HistTest(unittest.TestCase):
    def test_accuracy(self):
        rng = random.Random(1)
        vals = [int(rng.lognormvariate(6, 1.2)) for _ in range(20000)]
        h = hist.Hist()
        for v in vals:
            h.add(v)
        s = sorted(vals)
        for q in (0.5, 0.9, 0.99):
            exact = s[max(0, math.ceil(q * len(s)) - 1)]
            self.assertLessEqual(abs(h.quantile(q) - exact), exact / 32 + 1, q)

    def test_merge_and_rows(self):
        a, b, both = hist.Hist(), hist.Hist(), hist.Hist()
        for v in range(1, 5000, 7):
            (a if v % 2 else b).add(v)
            both.add(v)
        m = hist.Hist().merge(a).merge(b)
        self.assertEqual(m.b, both.b)
        self.assertEqual(m.quantile(0.99), both.quantile(0.99))
        rt = hist.Hist.from_rows(m.to_rows())
        self.assertEqual(rt.b, m.b)
        self.assertEqual(rt.n, m.n)
        self.assertIsNone(hist.Hist().quantile(0.5))


class SfnTest(unittest.TestCase):
    def test_wrap_and_late_lines(self):
        u = parse.SfnUnwrap(20)
        seq = [u(f, 0) for f in (1020, 1022, 1023, 0, 1, 1023, 2)]  # 1023 after 1 = a late line from before the wrap
        self.assertEqual(seq[3], seq[2] + 20)
        self.assertLess(seq[5], seq[4])
        self.assertGreater(seq[6], seq[4])
        t = parse.SfnUnwrap(20)
        last = -1
        for cyc in range(3):
            for f in range(0, 1024, 37):
                v = t(f, 5)
                self.assertGreater(v, last)
                last = v
        self.assertEqual(t(0, 0) // 20 // 1024, 3)  # frame 0 after frame ~999 of cycle 2 = next cycle


class ParseTest(unittest.TestCase):
    def test_evmgrant_fields_and_missing_timestamps(self):
        k, d = parse.parse_line("[PHY]   " + evm(e2e=(100, 50, 60, 300)))
        self.assertEqual((k, d["rnti"], d["frame"], d["slot"], d["crc"]), ("evmgrant", 0x4601, 10, 3, 1))
        self.assertEqual(d["e2e"]["tot"], 510)
        self.assertEqual(d["legacy_dec_us"], 300.0)
        d2 = parse.parse_line(evm(e2e=(-1, -1, 60, 300)))[1]
        self.assertIsNone(d2["e2e"]["arr2enq"])  # -1 = unavailable, never 0

    def test_ansi_and_unrelated(self):
        self.assertEqual(parse.parse_line("\x1b[32m" + evm() + "\x1b[0m")[0], "evmgrant")
        self.assertIsNone(parse.parse_line("nothing to see"))

    def test_ul_crc_drops_payload(self):
        line = ("SENSING: UL_PUSCH_CRC rnti=0x4601 ok=1 mcs=9 prb=3+20 ta=0 t=100 cpu_us=5 status=0 reason=- uci_hint=0 uci_re=0 "
                "dmrs_id=1 data_id=1 snr_db=11.5 delay=0 seg=1/1 tbs=100 cdm=2 tb=DEADBEEF probe=0")
        k, d = parse.parse_line(line)
        self.assertEqual(k, "ul_crc")
        self.assertNotIn("DEADBEEF", json.dumps(d))

    def test_lifecycle_config_health(self):
        self.assertEqual(parse.parse_line("SENSING: CONFIG_EPOCH 2 -> 3 class=HARD_RESET cause=MIB_CHANGE scope=cell_identity")[1]["epoch_to"], 3)
        self.assertEqual(parse.parse_line("SENSING: ACQ_STATE SEARCHING -> PBCH_LOCKED (updates=3)")[1]["state_to"], "PBCH_LOCKED")
        k, d = parse.parse_line("SENSING: RXDISCONT abs_slot=5 frame=1 slot=2 expected=10 actual=30 delta=20 prev_consumed=9 n=4 -> INVALIDATING SYNC")
        self.assertEqual((k, d["kind"], d["delta"], d["n"]), ("health", "rxdiscont", 20, 4))
        self.assertEqual(parse.parse_line("SENSING: RFTSDISC n=3 prev_raw=1 cur_raw=2 expected=3 delta=-5 requested=1 returned=1")[1]["delta"], -5)


class AggTest(unittest.TestCase):
    def test_success_and_failed_grants_are_terminal(self):
        a, rows = collect()
        a.feed_line(evm(crc=1), 1000.0, 0.0)
        a.feed_line(evm(crc=0, frame=11), 1000.1, 0.1)
        a.close()
        g = [r for d, r in rows if d == "rx_grants"]
        self.assertEqual([r["outcome"] for r in g], ["crc_ok", "crc_fail"])
        s = a.summary()
        self.assertEqual((s["grants"], s["crc_ok"], s["crc_fail"]), (2, 1, 1))

    def test_probe_decodes_not_counted_as_grants(self):
        a, _ = collect()
        a.feed_line(evm(probe=1), 1000.0, 0.0)
        self.assertEqual(a.summary()["grants"], 0)
        self.assertEqual(a.c["probe_decodes"], 1)

    def test_retry_attribution_without_double_counting(self):
        a, rows = collect()
        a.feed_line(evm(crc=0, frame=20, slot=4, pid=5), 1000.0, 0.0)
        a.feed_line(evm(crc=1, frame=21, slot=4, pid=5), 1000.0, 0.0)
        rec = "SENSING: RECOVLATE rnti=0x4601 20.4 abs=7 pid=5 ndi=1 tbs=3000 recovered_cfg=0 tot_us=4200"
        a.feed_line(rec, 1000.1, 0.1)
        a.feed_line(rec, 1000.2, 0.2)  # duplicate line for the same grant
        a.feed_line(rec.replace("20.4", "99.1"), 1000.3, 0.3)  # failure never seen
        s = a.summary()
        self.assertEqual((s["crc_ok"], s["crc_fail"], s["rescued_unique"]), (1, 1, 1))
        self.assertEqual(s["crc_ok_after_retry"], 2)  # each grant counted once
        self.assertEqual((a.c["retry_dup"], a.c["retry_orphan"]), (1, 1))
        att = [r["attributed"] for d, r in rows if d == "retry_events"]
        self.assertEqual(att, [True, False, False])

    def test_overlap_safe_durations(self):
        self.assertEqual(agg.union_us([(0, 10), (5, 15), (20, 30), (22, 25)]), 25)
        a, rows = collect()
        a.feed_line(evm(e2e=(100, 50, 60, 300)), 1000.0, 0.0)
        r = [r for d, r in rows if d == "rx_grants"][0]
        self.assertEqual(r["serial_us"], 510)  # arr2enq+qwait+pre+dec; first_body (nested in dec) is not added
        self.assertEqual(r["first_body_us"], 255)
        self.assertAlmostEqual(r["e2e_tot_us"], r["serial_us"], delta=agg.SPAN_TOL_US)
        self.assertFalse(r["span_inconsistent"])
        r2 = collect()
        r2[0].feed_line(evm(e2e=(-1, 50, 60, 300)), 1000.0, 0.0)
        row = [x for d, x in r2[1] if d == "rx_grants"][0]
        self.assertIsNone(row["serial_us"])
        self.assertIsNone(row["arr2enq_us"])

    def test_code_block_crc_columns(self):
        a, rows = collect()
        a.feed_line(evm().replace("probe=0", "probe=0 cb_ok=3 cb_tried=5 cb_total=30"), 1000.0, 0.0)
        a.feed_line(evm(), 1000.1, 0.1)  # a log from before 2026-10-26: no cb fields
        r = [x for d, x in rows if d == "rx_grants"]
        self.assertEqual((r[0]["cb_ok"], r[0]["cb_tried"], r[0]["cb_total"]), (3, 5, 30))
        self.assertEqual((r[1]["cb_ok"], r[1]["cb_tried"], r[1]["cb_total"]), (None, None, None))

    def test_legacy_names_kept(self):
        a, _ = collect()
        a.feed_line(evm(e2e=(100, 50, 60, 300)), 1000.0, 0.0)
        a.feed_line("SENSING: PDTIMCLS us real[n=5 mean=330.0 p50=325 p90=495 p99=705 max=1900.0 gt5ms=0 gt20ms=0]", 1000.0, 0.0)
        s = a.summary()
        self.assertIn("legacy_dec_us", s["latency_us"])
        self.assertIn("e2e_tot", s["latency_us"])
        self.assertEqual(s["legacy_pdtim_real"]["p99"], 705)

    def test_k_suppression_commercial(self):
        a, rows = collect("commercial")
        for i in range(agg.policy.K_COMMERCIAL - 1):
            a.feed_line(evm(frame=i), 1000.0, 0.0)
        a.close()
        self.assertEqual([d for d, _ in rows], [])
        self.assertGreaterEqual(a.c["suppressed_rows"], 1)
        b, rows = collect("commercial")
        for i in range(agg.policy.K_COMMERCIAL):
            b.feed_line(evm(frame=i), 1000.0, 0.0)
        b.close()
        self.assertIn("cell_aggregates", [d for d, _ in rows])

    def test_complementary_suppression(self):
        # one small combo next to one big one: totals minus the big row would reveal the small one, so both go
        a, rows = collect("commercial")
        for i in range(agg.policy.K_COMMERCIAL + 5):
            a.feed_line(evm(frame=i), 1000.0, 0.0)
        for i in range(3):
            a.feed_line(evm(frame=100 + i).replace("mcs=12", "mcs=27"), 1000.0, 0.0)
        a.close()
        self.assertNotIn("cell_aggregates", [d for d, _ in rows])

    def test_cold_start_failure_in_denominator(self):
        a, _ = collect()
        a.feed_line("Starting sync detection", 1000.0, 0.0)
        a.feed_line(evm(crc=0), 1000.0, 1.0)
        s_fail = a.summary()
        self.assertFalse(s_fail["cold_start"]["success"])
        b, _ = collect()
        b.feed_line(evm(crc=1), 1000.0, 1.0)
        st = plots.cold_start_stats([s_fail, b.summary()])
        self.assertEqual((st["sessions"], st["successes"], st["rate"]), (2, 1, 0.5))
        self.assertLess(st["ci95"][0], 0.5 < st["ci95"][1])


class DatasetSchemaTest(unittest.TestCase):
    """Run the exporter on the synthetic fixture, then validate every dataset's schema, units and ordering."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        rx, gnb = make_synthetic.build(secs=70)
        cls.rx_path = os.path.join(cls.tmp, "rx.log")
        with open(cls.rx_path, "w") as f:
            f.write("\n".join(rx) + "\n")
        cls.gnb_path = os.path.join(cls.tmp, "gnb.log")
        with open(cls.gnb_path, "w") as f:
            f.write("\n".join(gnb) + "\n")
        cls.out = {}
        for fmt in ("jsonl", "auto"):
            o = os.path.join(cls.tmp, "lab_" + fmt)
            r = subprocess.run([sys.executable, os.path.join(HERE, "export.py"), "--experiment-mode", "lab", "--out", o, "--input",
                                cls.rx_path, "--line-time", "unix", "--no-sampler", "--format", fmt, "--rotate-rows", "400"],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            cls.out[fmt] = o

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_every_dataset_validates(self):
        seen = set()
        for fmt, o in self.out.items():
            man = jload(os.path.join(o, "manifest.json"))
            if fmt == "jsonl":
                self.assertEqual(man["format"], "jsonl")
            for ds in policy.datasets_for("lab"):
                rows = store.read_dataset(o, ds)
                if not rows:
                    continue
                seen.add(ds)
                for r in rows:
                    policy.validate_row("lab", ds, r)
                    for k, v in r.items():
                        if k.endswith("_us") and v is not None:
                            self.assertTrue(0 <= v < 10 ** 9, (ds, k, v))
                        if k in ("frame",) and v is not None:
                            self.assertTrue(0 <= v < 1024)
                        if k == "slot" and v is not None:
                            self.assertTrue(0 <= v < 20)
                        if k == "window_start_s":
                            self.assertEqual(v % 60, 0)
                ts = [r["t_s"] for r in rows if r.get("t_s") is not None and ds != "system_telemetry"]
                self.assertEqual(ts, sorted(ts), ds)  # arrival-time ordering preserved
        for need in ("rx_grants", "dci_attempts", "discovery_events", "retry_events", "slot_health", "system_telemetry",
                     "latency_histograms", "cell_aggregates", "discovery_summary", "configuration_events"):
            self.assertIn(need, seen)

    def test_rotation_makes_part_files(self):
        man = jload(os.path.join(self.out["jsonl"], "manifest.json"))
        self.assertGreater(man["datasets"]["dci_attempts"], 1)
        self.assertGreater(len(store.read_dataset(self.out["jsonl"], "dci_attempts")), 400)

    def test_formats_agree(self):
        a, b = (store.read_dataset(o, "rx_grants") for o in (self.out["jsonl"], self.out["auto"]))
        self.assertEqual(len(a), len(b))

    def test_dynamic_and_new_metric_keys_allowed(self):
        rows = store.read_dataset(self.out["jsonl"], "system_telemetry")
        m = [r for r in rows if r.get("src") == "metrics"][0]
        for k in ("t_real_ns", "cfg_epoch", "proc_utime_us", "proc_maxrss_kb", "recov_attempts", "retry_yield_chains"):
            self.assertIn(k, m)

    def test_groundtruth_end_to_end(self):
        res, cls = groundtruth.match(store.read_dataset(self.out["jsonl"], "dci_attempts"),
                                     store.read_dataset(self.out["jsonl"], "rx_grants"), groundtruth.read_gnb_log(self.gnb_path))
        self.assertGreater(res["recall"], 0.9)
        self.assertGreater(res["precision"], 0.9)
        self.assertEqual(sum(res["classes"].values()), res["gnb_grants"])


class GroundTruthTest(unittest.TestCase):
    def test_collisions_wrap_and_classes(self):
        spf = 20
        # gNB schedule: same (frame,slot,rnti) key in two SFN cycles (collision), plus a grant across the wrap.
        g = [{"frame": 1020, "slot": 3, "rnti": 0x4601}, {"frame": 1023, "slot": 19, "rnti": 0x4601},
             {"frame": 5, "slot": 3, "rnti": 0x4601}, {"frame": 1020, "slot": 3, "rnti": 0x4601},  # same key one cycle later
             {"frame": 7, "slot": 1, "rnti": 0x4602}, {"frame": 8, "slot": 1, "rnti": 0x4602}, {"frame": 9, "slot": 1, "rnti": 0x4603}]
        un = parse.SfnUnwrap(spf)
        ext = [un(x["frame"], x["slot"]) for x in g]
        # the receiver's hyperframe differs: shifted by 3 whole SFN cycles; it misses g[2], decodes the rest as noted
        shift = 3 * 1024 * spf
        dci = [{"ext_slot": e + shift, "frame": x["frame"], "slot": x["slot"], "rnti": x["rnti"], "dir": "dl"}
               for i, (e, x) in enumerate(zip(ext, g)) if i != 2]
        dci.append({"ext_slot": ext[0] + shift + 1, "frame": 1020, "slot": 4, "rnti": 0x1234, "dir": "dl"})  # false alarm
        grants = {0: "crc_ok", 1: "crc_fail", 3: "crc_ok", 4: "crc_ok"}  # g[5] (idx5) has a DCI but no decode record
        rg = [{"ext_slot": ext[i] + shift, "rnti": g[i]["rnti"], "outcome": o, "dir": "dl", "probe": False} for i, o in grants.items()]
        res, cls = groundtruth.match(dci, rg, g, spf)
        self.assertEqual(res["offset_slots"], shift)
        self.assertEqual(res["classes"], {"correct_tb": 3, "dci_ok_tb_failed": 1, "missed_tb": 1, "dropped_undecidable": 2})
        self.assertEqual((res["tp_dci"], res["fp_dci"], res["fn_dci"]), (6, 1, 1))
        self.assertAlmostEqual(res["precision"], 6 / 7)
        self.assertAlmostEqual(res["recall"], 6 / 7)
        self.assertAlmostEqual(res["tb_success_over_gnb_grants"], 3 / 7)  # the miss stays in the denominator

    def test_parse_gnb_line_and_csv(self):
        line = ("  1000. 5 [DLSCH/PDSCH/PUCCH] RNTI 4601 DCI L 4 start  10 RBs  20 startSymbol  2 nb_symbol 12 dmrspos 4 MCS 13 "
                "nrOfLayers 2 TBS 3000 HARQ PID  3 round 0 RV 0 NDI 1 dl_data_to_ULACK 4 (0.0) PUCCH allocation 0 TPC 1")
        g = groundtruth.parse_gnb_line("[NR_MAC] " + line)
        self.assertEqual((g["frame"], g["slot"], g["rnti"], g["mcs"]), (1000, 5, 0x4601, 13))


class PolicyTest(unittest.TestCase):
    def test_commercial_datasets_have_no_identifying_columns(self):
        for ds in policy.datasets_for("commercial"):
            for c, (kind, cls) in policy.DATASETS[ds]["cols"].items():
                self.assertEqual(cls, "agg", (ds, c))
                self.assertNotIn("rnti", c)
        for ds in ("rx_grants", "dci_attempts", "discovery_events", "retry_events"):
            self.assertNotIn("commercial", policy.DATASETS[ds]["modes"])

    def test_fail_closed(self):
        v = policy.validate_row
        with self.assertRaises(policy.PolicyViolation):
            v("commercial", "cell_aggregates", {"rnti": 1})
        with self.assertRaises(policy.PolicyViolation):
            v("commercial", "cell_aggregates", {"dir": "PAYLOADMARKER"})
        with self.assertRaises(policy.PolicyViolation):
            v("commercial", "nope", {})
        with self.assertRaises(policy.PolicyViolation):
            v("lab", "cell_aggregates", {"n_grants": "x"})
        v("commercial", "cell_aggregates", {"dir": "dl", "n_grants": 10, "snr_db_mean": None})

    def test_lab_dataset_refused_in_commercial_sink_direct(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        s = store.Sink(d, "commercial", "jsonl")
        with self.assertRaises(policy.PolicyViolation):
            s.write("rx_grants", {"seq": 1})
        s.write("cell_aggregates", {"n_grants": 12})
        s.flush()
        self.assertFalse(os.path.exists(os.path.join(d, "rx_grants")))
        self.assertEqual((s.refused_rows, s.refused_fields), (1, 1))

    def test_atomic_parts(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        s = store.Sink(d, "lab", "jsonl", rotate_rows=3)
        for i in range(7):
            s.write("slot_health", {"seq": i, "kind": "rfcensus"})
        s.close()
        self.assertEqual(sorted(os.listdir(os.path.join(d, "slot_health"))), ["part-00000.jsonl", "part-00001.jsonl", "part-00002.jsonl"])
        self.assertFalse(os.path.exists(os.path.join(d, ".tmp")))


if __name__ == "__main__":
    unittest.main()
