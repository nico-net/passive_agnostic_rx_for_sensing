import json, os, sys, tempfile, time, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from monitor import JsonlTail, health_snapshot  # noqa: E402


class Health(unittest.TestCase):
    def test_missing_file_is_no_data_not_error(self):
        t = JsonlTail("/nonexistent/metrics.jsonl"); t.poll()
        self.assertIsNone(t.last); self.assertEqual(t.bad_lines, 0)

    def test_truncated_line_is_skipped_and_counted(self):
        p = tempfile.mktemp(); open(p, "w").write('{"schema":1,"pdschq_decoded":10}\n{"schema":1,"pdsch')
        t = JsonlTail(p); t.poll()
        self.assertEqual(t.last["pdschq_decoded"], 10); self.assertEqual(t.bad_lines, 0)  # partial tail is pending, not bad
        open(p, "a").write('q_decoded":20}\n'); t.poll()
        self.assertEqual(t.last["pdschq_decoded"], 20)

    def test_garbage_line_counts_bad(self):
        p = tempfile.mktemp(); open(p, "w").write('not json\n{"schema":1}\n')
        t = JsonlTail(p); t.poll()
        self.assertEqual(t.bad_lines, 1)

    def test_file_truncated_restarts_from_zero(self):
        p = tempfile.mktemp(); open(p, "w").write('{"a":1}\n{"a":2}\n'); t = JsonlTail(p); t.poll()
        open(p, "w").write('{"a":3}\n'); t.poll()
        self.assertEqual(t.last["a"], 3)

    def test_window_crc_rate_from_two_snapshots(self):
        m = JsonlTail(None); o = JsonlTail(None)
        m.history = [{"t_mono_ns": 0, "pdschq_decoded": 100, "pdschq_crc_ok": 90, "scanq_queued": 1000, "scanq_drop_full": 0},
                     {"t_mono_ns": 20_000_000_000, "pdschq_decoded": 300, "pdschq_crc_ok": 285, "scanq_queued": 3000, "scanq_drop_full": 10}]
        m.last = m.history[-1]
        h = health_snapshot(m, o)
        self.assertAlmostEqual(h["rates"]["crc_pct_window"], 97.5)
        self.assertAlmostEqual(h["rates"]["grants_per_s"], 10.0)
        self.assertAlmostEqual(h["rates"]["drop_full_pct"], 0.5)

    # --- additions (orchestrator rulings) ---
    def test_obs_with_null_nb_rb_does_not_crash(self):
        m = JsonlTail(None); o = JsonlTail(None)
        o.history = [{"t_mono_ns": 0, "dir": "DL", "rnti": 1, "nb_rb": None},
                     {"t_mono_ns": 10**9, "dir": "UL", "rnti": 1, "nb_rb": 106},
                     {"t_mono_ns": 2 * 10**9, "dir": "DL", "rnti": None, "nb_rb": 275}]
        o.last = o.history[-1]
        h = health_snapshot(m, o)
        self.assertEqual(sum(h["obs"]["prb_hist"]), 2)  # the null nb_rb grant is not binned
        self.assertEqual(h["obs"]["prb_hist"][9], 1)    # 275 clamps into the last bin
        json.dumps(h)

    def test_empty_sources_snapshot_is_valid_json(self):
        h = health_snapshot(JsonlTail(None), JsonlTail(None))
        self.assertIsNone(h["metrics"]); self.assertIsNone(h["metrics_age_s"])
        json.loads(json.dumps(h))

    def test_late_file_and_rotation_recover(self):
        p = tempfile.mktemp(); t = JsonlTail(p); t.poll()
        self.assertIsNone(t.last)                       # started before the receiver
        open(p, "w").write('{"a":1}\n'); t.poll()
        self.assertEqual(t.last["a"], 1)
        os.remove(p); t.poll()                          # rotated away: still last good value, no crash
        open(p, "w").write('{"a":2}\n'); t.poll()       # new file, same or smaller size
        self.assertEqual(t.last["a"], 2)

    def test_metrics_age_uses_file_mtime(self):
        p = tempfile.mktemp(); open(p, "w").write('{"t_mono_ns":1,"pdschq_decoded":1}\n')
        old = time.time() - 120; os.utime(p, (old, old))
        m = JsonlTail(p); m.poll()
        h = health_snapshot(m, JsonlTail(None))
        self.assertGreater(h["metrics_age_s"], 100)

    def test_huge_file_first_poll_is_bounded(self):
        p = tempfile.mktemp()
        with open(p, "w") as f:
            for i in range(200_000):                    # ~7 MB
                f.write(json.dumps({"i": i, "pad": "x" * 20}) + "\n")
        self.assertGreater(os.path.getsize(p), JsonlTail.MAX_CATCHUP_BYTES)
        t = JsonlTail(p, keep=100); new = t.poll()
        self.assertEqual(t.last["i"], 199_999)
        self.assertEqual(t.bad_lines, 0)                # first partial line dropped, not counted bad
        self.assertLess(len(new), 200_000)              # did not parse the whole file
        self.assertLessEqual(len(new) * 30, JsonlTail.MAX_CATCHUP_BYTES + 1000)
        self.assertEqual(len(t.history), 100)

    # --- fix round 1 ---
    def _snap(self, a, b):
        m = JsonlTail(None); m.history = [a, b]; m.last = b
        return health_snapshot(m, JsonlTail(None))

    def test_negative_delta_is_counter_reset(self):
        base = {"t_mono_ns": 0, "pdschq_decoded": 300, "pdschq_crc_ok": 285, "scanq_queued": 3000, "scanq_drop_full": 10}
        for k, v in (("pdschq_decoded", 5), ("pdschq_crc_ok", 5), ("scanq_queued", 5), ("scanq_drop_full", 0)):
            b = dict(base, t_mono_ns=10**9, pdschq_decoded=400, pdschq_crc_ok=380, scanq_queued=4000, scanq_drop_full=20)
            b[k] = v
            r = self._snap(base, b)["rates"]
            if k in ("pdschq_decoded", "pdschq_crc_ok"):
                self.assertIsNone(r["crc_pct_window"], k)
            if k == "pdschq_decoded":
                self.assertIsNone(r["grants_per_s"])
            if k in ("scanq_queued", "scanq_drop_full"):
                self.assertIsNone(r["drop_full_pct"], k)

    def test_truncation_clears_history(self):
        p = tempfile.mktemp(); open(p, "w").write('{"a":1}\n{"a":2}\n'); t = JsonlTail(p); t.poll()
        open(p, "w").write('{"a":3}\n'); t.poll()
        self.assertEqual(t.history, [{"a": 3}])

    def test_inode_change_same_or_larger_size_resets(self):
        p = tempfile.mktemp(); open(p, "w").write('{"a":1}\n'); t = JsonlTail(p); t.poll()
        for body in ('{"a":2}\n', '{"a":3}\n{"a":4}\n'):
            q = tempfile.mktemp(); open(q, "w").write(body); os.replace(q, p)
            t.poll()
            self.assertEqual(t.history, [json.loads(l) for l in body.split()])

    def test_nonfinite_values_never_emit_nan(self):
        a = {"t_mono_ns": 0, "pdschq_decoded": 1, "pdschq_crc_ok": 1, "scanq_queued": 1, "scanq_drop_full": 0}
        b = {"t_mono_ns": 10**9, "pdschq_decoded": float("inf"), "pdschq_crc_ok": float("nan"),
             "scanq_queued": float("inf"), "scanq_drop_full": float("nan")}
        h = self._snap(a, b)
        self.assertNotIn("NaN", json.dumps(h["rates"], allow_nan=True))
        self.assertTrue(all(v is None for v in h["rates"].values()))

    def test_old_schema_missing_keys_no_exception(self):
        m = JsonlTail(None); m.history = [{"schema": 0}, {"schema": 0, "x": 1}]; m.last = m.history[-1]
        o = JsonlTail(None); o.history = [{"t_mono_ns": 0}, {"t_mono_ns": 10**9}]; o.last = o.history[-1]
        h = health_snapshot(m, o); json.dumps(h)

    def test_non_dict_json_line_counts_bad(self):
        p = tempfile.mktemp(); open(p, "w").write('[1,2]\n42\n"s"\n{"a":1}\n')
        t = JsonlTail(p); t.poll()
        self.assertEqual(t.bad_lines, 3); self.assertEqual(t.last, {"a": 1})


if __name__ == "__main__":
    unittest.main()
