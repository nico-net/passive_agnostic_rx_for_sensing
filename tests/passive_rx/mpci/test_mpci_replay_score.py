"""Unit tests of mpci_replay_score.py on synthetic log snippets (no receiver). Run: python3 -m unittest tests/passive_rx/mpci/test_mpci_replay_score.py"""
import json, os, sys, tempfile, unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpci_replay_score as S

TIME = """\tCommand being timed: "timeout"
\tUser time (seconds): %s
\tSystem time (seconds): %s
\tElapsed (wall clock) time (h:mm:ss or m:ss): %s
\tMaximum resident set size (kbytes): 9000000
"""


def ev(tag, crc, tot, mcs=10, i=0):
    return (f"{tag}[PHY]    SENSING: EVMGRANT rnti=0x1 1.{i} crc={crc} mcs={mcs} tbl=0 Qm=2 Nl=1 rb=0+5 nsym=12 "
            f"evm=1 snr=20.0 probe=0 rv=0 ndi=0 pid=0 tbs=96 snr_l=20.0,-99.0,-99.0,-99.0 l2=1 zabs=0 e2e=100,1,1,1,1,1,{tot},1 stg=fep:1/1/1 cb=1\n")


def cell_lines(tag, n, p50, p99, crc_frac, dropped=0, ul_p50=None, bad=0):
    L = []
    for i in range(n):
        tot = p99 if i >= int(n * 0.97) else p50
        L.append(ev(tag, 1 if i < int(n * crc_frac) else 0, tot, i=i))
    L.append(f"{tag}[PHY]    SENSING: PDSCHQ queued=1000 decoded=900 crc_ok=800 dropped[full={dropped} stale=0] dropped_narrow=0\n")
    if ul_p50 is not None:
        for i in range(40):
            L.append(f"{tag}[PHY]    SENSING: UL_E2E tot={ul_p50} probe=0\n")
            L.append(f"{tag}[PHY]    SENSING: UL_PUSCH_CRC ok=1 mcs=5 tbl=0 snr_db=15.0 probe=0\n")
    L.append(f"{tag}[PHY]    SENSING: GPU_EXACT_FEP calls=100 fallback=0 (late=0 busy=0) check_ok=99 check_bad={bad} direct=1\n")
    return L


def pool_line(wait_frac=0.0, p99=200):
    n = 1000
    return ("[PHY]    SENSING: CORE_POOL n=8 yields=1 gpu_gives=0 nested=0 enforced=0 scan[n=%d waited=%d promo=0 wait_us p50<=10 p99<=%d max=900] "
            "decode[n=%d waited=%d promo=0 wait_us p50<=10 p99<=%d max=900] chan[n=50 waited=0 promo=0 wait_us p50<=1 p99<=1 max=1]\n"
            % (n, int(n * wait_frac), p99, n, int(n * wait_frac), p99))


def feed(ov=0):
    return ("[PHY]    SENSING: MPCI_FEED rate=122880000 centre_hz=3600000000 reads=1000 samples=1 short=0 gaps=0 frozen=0 read_err=0 chan_overruns=%d "
            "chan_discont=0 chan_lag_max=3 threads=2/2 backend=gpu cells=pci508:ov=0,drop=0,lag=1;pci403:ov=0,drop=0,lag=1\n" % ov)


TICKET = "[GPU] DISC_TICKET active_cells=2 pct=30 rt_busy_pct=5 c0[grants=100 contended=%d wait_ms=%d wait_max_ms=9 held_ms=50 held_max_ms=5 pace_ms=%d]\n"


def write_run(root, name, arm, cells, p50, p99, crc, drops=0, ul=None, busy=(100.0, 2.0, "1:00.00"), bad=0, wait_frac=0.0, ov=0, ticket=(1, 1, 0)):
    """cells: PCIs in cell order. Pair: tagged log with MPCI_CELL lines; solo: untagged."""
    d = os.path.join(root, name)
    os.makedirs(d)
    with open(os.path.join(d, "cells.json"), "w") as f:
        json.dump({"cells": [{"id": "pci%d" % p, "pci": p} for p in cells]}, f)
    with open(os.path.join(d, "arm.txt"), "w") as f:
        f.write(arm + "\n")
    L = []
    if len(cells) == 1:
        L += cell_lines("", 600, p50[0], p99[0], crc[0], drops[0] if isinstance(drops, list) else drops, ul[0] if ul else None, bad)
    else:
        for k, p in enumerate(cells):
            L.append("[PHY]    SENSING: MPCI_CELL id=pci%d slot=%d event=add pci=%d\n" % (p, k, p))
            L += cell_lines("[c%d] " % k, 600, p50[k], p99[k], crc[k], drops[k] if isinstance(drops, list) else drops, ul[k] if ul else None, bad)
        L += [feed(ov), pool_line(wait_frac), TICKET % ticket]
        L.append("[PHY]    SENSING: CORE_POOL cell=0 pci=508 cell0[n=1 waited=0 hold_ms=1 wait_us p50<=1 p99<=1 max=1]\n")
        L.append("[PHY]    SENSING: CELL_CPU cell=0 pci=508 win_ms=10000 tokens_avg=1.50 share_pct=50.0 fair_pct=50.0 wait_p99_us=5 n=9\n")
        L.append("[PHY]    SENSING: CELL_CPU cell=1 pci=403 win_ms=10000 tokens_avg=0.50 share_pct=50.0 fair_pct=50.0 wait_p99_us=5 n=9\n")
    with open(os.path.join(d, "rx.log"), "w") as f:
        f.writelines(L)
    with open(os.path.join(d, "time.txt"), "w") as f:
        f.write(TIME % (busy[0], busy[1], busy[2]))
    s = S.score_run(d)
    with open(os.path.join(d, "summary.json"), "w") as f:
        json.dump(s, f)
    return d


def campaign(root, pair_p50=(1000, 1000), pair_p99=(2000, 2000), pair_crc=(0.9, 0.9), pair_busy=(70.0, 2.0, "1:00.00"), **kw):
    for i in range(3):
        write_run(root, "s508_%d" % i, "solo508", [508], [1000], [2000], [0.9], ul=[3000], busy=(60.0, 2.0, "1:00.00"))
        write_run(root, "s403_%d" % i, "solo403", [403], [900], [1800], [0.9], ul=[3000], busy=(40.0, 2.0, "1:00.00"))
        write_run(root, "pair_%d" % i, "pair", [508, 403], list(pair_p50), list(pair_p99), list(pair_crc), ul=[3000, 3000], busy=pair_busy, **kw)
    return S.verdict(S.find_runs(root))


class Tests(unittest.TestCase):
    def tmp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        return d.name

    def test_parse_time(self):
        t = S.parse_time(TIME % ("12.5", "1.5", "4:10.50"))
        self.assertEqual((t["user"], t["sys"], t["wall"], t["rss_kb"]), (12.5, 1.5, 250.5, 9000000.0))
        self.assertEqual(S.parse_time(TIME % ("1", "1", "1:02:03.5"))["wall"], 3723.5)

    def test_order_interleaved(self):
        o = S.order(3, ["pair", "solo508", "solo403"])
        self.assertEqual(o, ["pair", "solo508", "solo403", "solo403", "pair", "solo508", "solo508", "solo403", "pair"])
        self.assertEqual(S.order(4, ["a", "b"]), list("abbaabba") and ["a", "b", "b", "a", "a", "b", "b", "a"])

    def test_cli_and_src(self):
        self.assertEqual(S.cli_args(508), "-C 3590010000 --ssb 193 -r 162 --initial-fo -4868")
        self.assertEqual(S.cli_args(403), "-C 3630000000 --ssb 18 -r 51 --initial-fo 2178")
        d = self.tmp()
        json.dump({"samples_per_channel": 983040000, "first_sample_tick": 5, "channels": 4, "sample_rate_hz": 122880000, "rf_channels": [{"frequency_hz": 3610000000}]},
                  open(os.path.join(d, "capture_info.json"), "w"))
        self.assertEqual(S.pub_src("g5wide", d), "g5wide=%s,983040000,5,4,122880000,3610000000,2" % d)

    def test_make_cells_checks_inventory(self):
        inv = os.path.join(os.path.dirname(os.path.abspath(__file__)), "inventory", "live_tuned_20261023.json")
        cap = "/home/nicola/NICOLA/ota_runs/newcells_tiled/l0942_c3610x8"
        if not os.path.exists(cap):
            self.skipTest("capture not on this host")
        j = S.make_cells(inv, cap, [403])
        self.assertEqual([c["pci"] for c in j["cells"]], [403])
        self.assertEqual(j["window"], {"centre_hz": 3610000000.0, "rate_msps": 122.88, "n_rx": 4})
        j2 = S.make_cells(inv, cap, [508, 403])
        self.assertEqual([c["id"] for c in j2["cells"]], ["pci508", "pci403"])

    def test_pass(self):
        v = campaign(self.tmp())
        self.assertTrue(v["ok"], S.text(v))
        self.assertEqual(v["arms"], {"pair": 3, "solo403": 3, "solo508": 3})
        self.assertAlmostEqual(v["p3"]["ratio"], 72.0 / 62.0, places=3)   # pair (70+2)/60 s vs the heavier solo (60+2)/60 s
        self.assertEqual(v["cells"]["508"]["n_pair"], 3)

    def test_each_cell_labelled_separately(self):
        v = campaign(self.tmp(), pair_p99=(2000, 4000))   # 403 p99 2.2x its own solo; 508 unchanged
        self.assertTrue(v["cells"]["508"]["ok"])
        self.assertFalse(v["cells"]["403"]["ok"])
        self.assertFalse(v["ok"])
        bad = [c for c in v["cells"]["403"]["checks"] if not c["ok"]]
        self.assertEqual([c["check"].split()[0] for c in bad], ["dl_p99"])

    def test_crc_and_drops_and_ul(self):
        root = self.tmp()
        v = campaign(root, pair_crc=(0.9, 0.8))   # 0.8/0.9 = 0.889 < 0.95
        self.assertFalse(v["cells"]["403"]["ok"])
        self.assertTrue(v["cells"]["508"]["ok"])
        v = campaign(self.tmp(), drops=2.0)       # pair drops 2 % > solo (0) + 1
        self.assertFalse(v["p1_ok"])

    def test_p3_fails_and_names_cores(self):
        v = campaign(self.tmp(), pair_busy=(100.0, 2.0, "0:40.00"), wait_frac=0.4)   # 2.55 cores vs 1.0
        self.assertFalse(v["p3"]["ok"])
        self.assertEqual(v["binding"]["binding"], "cores")
        self.assertIn("CORE_POOL", v["binding"]["evidence"][0])

    def test_check_bad_and_channelizer(self):
        v = campaign(self.tmp(), bad=2)
        self.assertFalse(v["gpu"]["ok"])
        self.assertFalse(v["ok"])
        v = campaign(self.tmp(), ov=3)
        self.assertFalse(v["overruns"]["ok"])
        self.assertFalse(v["ok"])
        self.assertEqual(v["binding"]["binding"], "channelizer")

    def test_discovery_binding(self):
        v = campaign(self.tmp(), pair_p99=(4000, 4000), ticket=(80, 500, 120))
        self.assertFalse(v["ok"])
        self.assertIn("discovery", v["binding"]["candidates"])

    def test_missing_ul_in_pair_fails(self):
        root = self.tmp()
        for i in range(3):
            write_run(root, "s508_%d" % i, "solo508", [508], [1000], [2000], [0.9], ul=[3000])
            write_run(root, "s403_%d" % i, "solo403", [403], [900], [1800], [0.9])
            write_run(root, "pair_%d" % i, "pair", [508, 403], [1000, 900], [2000, 1800], [0.9, 0.9], ul=None)
        v = S.verdict(S.find_runs(root))
        self.assertFalse(v["cells"]["508"]["ok"])
        self.assertTrue(v["cells"]["403"]["ok"])

    def test_thin_data_and_n_lt_3(self):
        root = self.tmp()
        write_run(root, "s508_0", "solo508", [508], [1000], [2000], [0.9])
        write_run(root, "s403_0", "solo403", [403], [900], [1800], [0.9])
        write_run(root, "pair_0", "pair", [508, 403], [1000, 900], [2000, 1800], [0.9, 0.9])
        v = S.verdict(S.find_runs(root), min_dl=100000)
        self.assertFalse(v["ok"])
        self.assertTrue(any("n=1" in p for p in v["problems"]))
        self.assertFalse(v["cells"]["508"]["ok"])

    def test_pci_mismatch_flagged(self):
        root = self.tmp()
        d = write_run(root, "pair_0", "pair", [508, 403], [1000, 900], [2000, 1800], [0.9, 0.9])
        s = S.score_run(d, expected=[403, 508])    # the log says slot 0 = 508
        self.assertEqual(s["cells"]["508"]["pci_mismatch_expected"], 403)

    def test_text_runs(self):
        t = S.text(campaign(self.tmp(), pair_p99=(2000, 4000)))
        self.assertIn("P1 cell PCI 403", t)
        self.assertIn("V3 OVERALL: FAIL", t)


if __name__ == "__main__":
    unittest.main()
