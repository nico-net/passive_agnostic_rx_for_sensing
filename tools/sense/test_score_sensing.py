import io, json, os, sys, tempfile, unittest, contextlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_sensing as S

T0 = 10**12  # a realistic CLOCK_MONOTONIC offset; only differences matter
def ns(sec): return T0 + round(sec * 1e9)

def rec(t, pci=10, dir="DL", tx="auto", valid=128, layer=None, n_layers=None, prg=None,
        snr=20.0, resid=-30.0, recon=None, first=0, mask="auto", ssb_beam=None, port_mask=None, t_ns=None):
    """One rs line in the real nr_sense_rs_to_json format (schema 2, quality block present)."""
    if tx == "auto": tx = pci if dir == "DL" else "rnti:1a2b"
    if mask == "auto":
        m = 0
        for p in range(valid): m |= 1 << p
        mask = m.to_bytes((valid + 7) // 8, "little").hex() if valid else ""
    d = {"schema": 2, "type": "rs", "pci": pci, "t_mono_ns": ns(t) if t_ns is None else t_ns, "source": "pdsch_dmrs", "dir": dir,
         "rnti": 4660, "allocated_prbs": valid, "estimated_usable_prbs": valid, "valid_cfr_prbs": valid,
         "valid_mask": mask, "first_prb": first, "snr_db_p50": snr, "snr_db_p10": None if snr is None else snr - 3,
         "resid_db": resid, "sym_consist": None, "recon_conf": recon, "wideband": valid >= 128,
         "tx": tx, "ssb_beam": ssb_beam, "cfg_ref": None, "prg_size": prg}
    if port_mask is not None: d["port_mask"] = port_mask
    if layer is not None: d["layer"] = layer; d["n_layers"] = n_layers
    return json.dumps(d)

class Base(unittest.TestCase):
    def run_files(self, *contents, min_prb=128, **kw):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        paths = []
        for i, c in enumerate(contents):
            p = os.path.join(tmp.name, f"rec{i}.jsonl")
            with open(p, "w") as f: f.write("\n".join(c) + ("\n" if c else ""))
            paths.append(p)
        return S.score(paths, min_prb, **kw)
    def link(self, res, cell=0, i=0):
        return res["cells"][cell]["links"][i]

class Continuity(Base):
    def test_gaps(self):
        r = self.run_files([rec(0), rec(0.1), rec(0.5)])
        g = self.link(r)["gap_s"]
        self.assertEqual(g["n"], 2)
        self.assertAlmostEqual(g["p50"], 0.25)   # linear-interpolated median of [0.1, 0.4]
        self.assertAlmostEqual(g["p95"], 0.385)  # 0.1 + 0.95 * 0.3
        self.assertAlmostEqual(g["max"], 0.4)
    def test_not_wideband_excluded(self):
        r = self.run_files([rec(0), rec(0.2, valid=100), rec(0.5)])
        l = self.link(r)
        self.assertEqual(l["n_wideband"], 2); self.assertEqual(l["n_produced"], 3)
        self.assertEqual(l["gap_s"]["n"], 1); self.assertAlmostEqual(l["gap_s"]["max"], 0.5)
        self.assertEqual(l["valid_cfr_prbs_hist"], {"96": 1, "128": 2})
    def test_min_prb_option(self):
        r = self.run_files([rec(0), rec(0.2, valid=100), rec(0.5)], min_prb=100)
        self.assertEqual(self.link(r)["n_wideband"], 3)
    def test_out_of_order(self):
        r = self.run_files([rec(0.5), rec(0), rec(0.1)])
        g = self.link(r)["gap_s"]
        self.assertAlmostEqual(g["p50"], 0.25); self.assertAlmostEqual(g["max"], 0.4)
    def test_same_instant_is_one_event(self):
        r = self.run_files([rec(0), rec(0), rec(0.5)])
        self.assertEqual(self.link(r)["gap_s"]["n"], 1)

class Availability(Base):
    def test_availability_windows(self):
        r = self.run_files([rec(0), rec(0.05), rec(0.95), rec(2.5)])
        a = r["cells"][0]["availability"]
        self.assertEqual((a["w100ms"]["windows"], a["w100ms"]["covered"]), (26, 3))  # idx 0, 9, 25 of 0..25
        self.assertAlmostEqual(a["w100ms"]["fraction"], 3 / 26)
        self.assertEqual((a["w1s"]["windows"], a["w1s"]["covered"]), (3, 2))         # idx 0 and 2 of 0..2
        self.assertAlmostEqual(a["w1s"]["fraction"], 2 / 3)
        self.assertEqual(r["aggregate"]["availability"]["w1s"]["covered"], 2)
    def test_prbs_per_s(self):
        r = self.run_files([rec(0, valid=128), rec(0.25, valid=100), rec(0.5, valid=200)])
        b = self.link(r)
        self.assertAlmostEqual(b["valid_prbs_per_s_wideband"], 328 / 0.5)
        self.assertAlmostEqual(b["valid_prbs_per_s_all"], 428 / 0.5)
    def test_occupancy_map(self):
        r = self.run_files([rec(0, valid=8, first=0), rec(1.5, valid=8, first=4)], min_prb=4)
        o = self.link(r)["occupancy"]
        self.assertEqual(o["window_s"], 1.0)
        self.assertEqual(o["rows"][0], [1] * 8 + [0] * 4)   # PRB 0..7
        self.assertEqual(o["rows"][1], [0] * 4 + [1] * 8)   # PRB 4..11

class Quality(Base):
    def test_quality_and_rates(self):
        r = self.run_files([rec(0, snr=10, resid=-20, recon=0.5), rec(0.1, snr=20, resid=-30, recon=0.75),
                            rec(0.2, snr=30, resid=-40, recon=None), rec(0.3, valid=0, snr=None, resid=None)])
        l = self.link(r)
        self.assertEqual(l["snr_db_p50"]["n"], 3); self.assertAlmostEqual(l["snr_db_p50"]["p50"], 20)
        self.assertAlmostEqual(l["resid_db"]["p50"], -30)
        self.assertEqual(l["recon_conf"]["n"], 2); self.assertAlmostEqual(l["recon_conf"]["p50"], 0.625)
        self.assertAlmostEqual(l["valid_cfr_rate"], 3 / 4)    # valid_cfr_prbs > 0 / produced
        self.assertAlmostEqual(l["wideband_rate"], 3 / 4)

class Links(Base):
    def test_links_not_pooled(self):
        r = self.run_files([rec(0), rec(0.5), rec(0.1, dir="UL"), rec(0.2, dir="UL"),
                            rec(0.3, layer=1, n_layers=2), rec(0.4, tx=99)])
        ls = r["cells"][0]["links"]
        self.assertEqual(len(ls), 4)
        by = {(l["dir"], l["tx"], l["layer"]): l for l in ls}
        self.assertEqual(by[("DL", 10, None)]["n_wideband"], 2)
        self.assertAlmostEqual(by[("DL", 10, None)]["gap_s"]["max"], 0.5)
        self.assertAlmostEqual(by[("UL", "rnti:1a2b", None)]["gap_s"]["max"], 0.1)
        self.assertEqual(by[("DL", 10, 1)]["n_wideband"], 1); self.assertEqual(by[("DL", 99, None)]["n_wideband"], 1)
    def test_prg_change_splits(self):
        r = self.run_files([rec(0, prg=2), rec(0.1, prg=2), rec(0.2, prg=4), rec(0.9, prg=4), rec(1.0, prg=None)])
        ls = {l["prg_size"]: l for l in r["cells"][0]["links"]}
        self.assertEqual(set(ls), {2, 4, None})
        self.assertAlmostEqual(ls[2]["gap_s"]["max"], 0.1); self.assertAlmostEqual(ls[4]["gap_s"]["max"], 0.7)
        self.assertEqual(ls[None]["gap_s"]["n"], 0)
    def test_same_rnti_two_cells(self):
        r = self.run_files([rec(0, pci=10, dir="UL"), rec(0.1, pci=10, dir="UL")], [rec(0.05, pci=20, dir="UL")])
        self.assertEqual(len(r["cells"]), 2)
        a, b = r["cells"]
        self.assertEqual((a["pci"], b["pci"]), (10, 20))
        self.assertEqual(a["links"][0]["n_wideband"], 2); self.assertEqual(b["links"][0]["n_wideband"], 1)
        self.assertEqual(a["links"][0]["tx"], b["links"][0]["tx"])

class Guards(Base):
    def test_ssb_beam_and_port_mask_split_links(self):
        r = self.run_files([rec(0, ssb_beam=1), rec(0.1, ssb_beam=2), rec(0.2, ssb_beam=1), rec(0.3, port_mask=3), rec(0.4)])
        ls = r["cells"][0]["links"]
        self.assertEqual(len(ls), 4)
        b1 = [l for l in ls if l["ssb_beam"] == 1][0]
        self.assertEqual(b1["n_wideband"], 2); self.assertAlmostEqual(b1["gap_s"]["max"], 0.2)
    def test_mixed_clocks_refused(self):
        far_t = T0 + 30_000 * 10**9 - 2 * 10**9
        far = [rec(0, pci=20, t_ns=T0 + 30_000 * 10**9 - 10**9), rec(0.1, pci=20, t_ns=T0 + 30_000 * 10**9)]
        with self.assertRaises(S.ScoreError): self.run_files([rec(0), rec(1)], [rec(0, pci=20, t_ns=far_t)] + far)
        r = self.run_files([rec(0), rec(1)], [rec(0, pci=20, t_ns=far_t)] + far, allow_mixed_clocks=True)
        self.assertEqual(len(r["cells"]), 2)
    def test_overlapping_or_close_ranges_accepted(self):
        self.run_files([rec(0), rec(5)], [rec(3, pci=20), rec(9, pci=20)])          # chained
        self.run_files([rec(0), rec(10)], [rec(12, pci=20), rec(14, pci=20)])       # disjoint, span 14 <= 2 x 10
    def test_cli_exit_codes(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        ps = []
        for i, c in enumerate(([rec(0), rec(1)], [rec(0, pci=20, t_ns=T0 + 30_000 * 10**9)])):
            ps.append(os.path.join(tmp.name, f"{i}.jsonl")); open(ps[-1], "w").write("\n".join(c) + "\n")
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(S.main(ps), 2)
            self.assertEqual(S.main(ps + ["--allow-mixed-clocks"]), 0)   # 30000 s < 24 h: override works
        self.assertIn("share a clock", err.getvalue())
    def test_corrupt_timestamp(self):
        r = self.run_files([rec(0), rec(0, t_ns=10**18), rec(0.1)])
        self.assertEqual(r["malformed_lines"], 1); self.assertEqual(r["cells"][0]["n_wideband"], 2)
        with self.assertRaises(S.ScoreError):   # plausible per record, but > 24 h combined span
            self.run_files([rec(0), rec(0, t_ns=T0 + 100_000 * 10**9)])
    def test_short_mask(self):
        r = self.run_files([rec(0, valid=128, mask="ff")])   # vp says 128, mask carries 8 bits
        l = self.link(r)
        self.assertEqual(l["n_wideband"], 1); self.assertEqual(l["occupancy"]["rows"], [[1] * 8])
    def test_window_boundary(self):
        r = self.run_files([rec(0), rec(0, t_ns=ns(0.1) - 1)])   # 99.999999 ms: still window 0
        self.assertEqual(r["cells"][0]["availability"]["w100ms"]["windows"], 1)
        r = self.run_files([rec(0), rec(0.1)])                    # exactly 100 ms: window 1
        a = r["cells"][0]["availability"]["w100ms"]
        self.assertEqual((a["windows"], a["covered"]), (2, 2))

class MultiPci(Base):
    def test_rescue_fraction(self):
        r = self.run_files([rec(0, pci=10), rec(0.2, pci=10)], [rec(0.1, pci=20), rec(0.3, pci=20)])
        ag = r["aggregate"]
        self.assertAlmostEqual(ag["rescue_fraction"], 1.0)
        self.assertAlmostEqual(ag["rescue_split"]["10"], 0.5); self.assertAlmostEqual(ag["rescue_split"]["20"], 0.5)
        self.assertAlmostEqual(ag["availability"]["w100ms"]["fraction"], 1.0)
        self.assertAlmostEqual(r["cells"][0]["availability"]["w100ms"]["fraction"], 0.5)
    def test_rescue_with_overlap_and_shares(self):
        r = self.run_files([rec(0, pci=10), rec(0.1, pci=10)], [rec(0.1, pci=20)])
        ag = r["aggregate"]   # window 0: only 10; window 1: both
        self.assertAlmostEqual(ag["rescue_fraction"], 0.5)
        self.assertAlmostEqual(ag["rescue_split"]["10"], 1.0); self.assertAlmostEqual(ag["rescue_split"]["20"], 0.0)
        self.assertAlmostEqual(ag["wideband_share"]["10"], 2 / 3); self.assertAlmostEqual(ag["wideband_share"]["20"], 1 / 3)
    def test_single_pci_has_no_rescue(self):
        self.assertIsNone(self.run_files([rec(0)])["aggregate"]["rescue_fraction"])

class EdgeCases(Base):
    def test_empty_file(self):
        r = self.run_files([])
        self.assertEqual(r["cells"], []); self.assertEqual(r["malformed_lines"], 0)
        self.assertIsNone(r["aggregate"]["availability"]["w1s"]["fraction"])
        self.assertIsNone(r["aggregate"]["rescue_fraction"])
    def test_cell_without_wideband(self):
        r = self.run_files([rec(0, valid=100), rec(0.5, valid=100)], [rec(0, pci=20), rec(0.1, pci=20)])
        c = r["cells"][0]
        self.assertEqual(c["pci"], 10); self.assertEqual(c["n_wideband"], 0)
        self.assertEqual(c["availability"]["w100ms"]["covered"], 0)
        self.assertIsNone(c["links"][0]["gap_s"]["p50"]); self.assertEqual(c["links"][0]["gap_s"]["n"], 0)
        self.assertEqual(r["aggregate"]["rescue_split"]["20"], 1.0)
    def test_malformed_skipped_and_counted(self):
        good = rec(0)
        r = self.run_files([good, "not json", good[:30], '{"type":"rs","valid_cfr_prbs":5,"t_mono_ns":"x"}',
                            '[1,2]', json.dumps({"type": "grant", "pci": 1}),
                            json.dumps({"type": "rs", "pci": 10, "t_mono_ns": ns(1), "dir": "DL"}), "", rec(0.5)])
        self.assertEqual(r["malformed_lines"], 4)
        self.assertEqual(r["ignored_non_rs"], 1); self.assertEqual(r["rs_without_quality"], 1)
        self.assertEqual(r["cells"][0]["n_wideband"], 2)
    def test_bad_mask_is_malformed(self):
        r = self.run_files([rec(0, mask="zz"), rec(0.1)])
        self.assertEqual(r["malformed_lines"], 1)

class LogAndCli(Base):
    def test_log_stability(self):
        lines = ['x\x1b[0m ISAC_METRICS {"cell":0,"sense_units_enabled":1,"sense_violations":0,"sense_disabled_skips":3}',
                 'ISAC_METRICS {"cell":0,"sense_units_enabled":2,"sense_violations":1,"sense_disabled_skips":9}',
                 'ISAC_METRICS {"cell":1}', 'ISAC_METRICS {"cell":0,"sense_vio', 'noise']
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f: f.write("\n".join(lines))
        self.addCleanup(os.unlink, f.name)
        st = S.parse_log(f.name)
        self.assertEqual(st[0]["violations_last"], 1); self.assertEqual(st[0]["disabled_skips_last"], 9)
        self.assertEqual(st[0]["units_enabled_series"], [1, 2])
        self.assertIsNone(st[1]["violations_last"]); self.assertEqual(st[1]["units_enabled_series"], [])
    def test_cli_on_fixture(self):
        fx = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
        out = tempfile.NamedTemporaryFile(suffix=".json", delete=False); out.close(); self.addCleanup(os.unlink, out.name)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = S.main([os.path.join(fx, "cell0.jsonl"), os.path.join(fx, "cell1.jsonl"), "--json", out.name])
        self.assertEqual(rc, 0)
        d = json.load(open(out.name))
        self.assertEqual([c["pci"] for c in d["cells"]], [421, 422])
        self.assertIn("pci 421", buf.getvalue()); self.assertIn("AGGREGATE", buf.getvalue())
        self.assertEqual(d["malformed_lines"], 0)

if __name__ == "__main__":
    unittest.main()
