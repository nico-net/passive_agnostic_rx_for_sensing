#!/usr/bin/env python3
"""C-DASH: multi-PCI lines in the dashboard parser (MPCI_ADMIT/SPAN/FEED/RT/LIFE/CELL/CELL_SLOT, SFO reset, CORE_PLAN/CORE_POOL, SHMIQ_*).

Fixtures: tests/passive_rx/mpci/fixtures/ota_20261027/*.run.log.gz (DGX GB10 + X410 OTA excerpts; P1: c0 PCI 415 + c1 PCI 923,
Q1: c0 PCI 415 + c1 PCI 403; S1 and TN1 are N=1 and carry no tag). Every expected value is taken from the raw log with a plain
re/str pass written here (not from rxparse), so the parser is checked against an independent reading.
    python3 -m unittest test_mpci_dash      (run from this directory)
"""
import gzip
import json
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PR = os.path.dirname(HERE)
FIX = os.path.join(PR, "mpci", "fixtures", "ota_20261027")
sys.path.insert(0, HERE)
import rxparse  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")
NAMES = {"P1": "mp27_P1_1_115718.run.log.gz", "Q1": "mp27_Q1_1_121424.run.log.gz", "S1": "mp27_S1_1_115134.run.log.gz",
         "TN1": "me_TN1_1_220653.run.log.gz"}
_RAW, _CLEAN, _VIEW = {}, {}, {}


def raw(k):
    if k not in _RAW:
        with gzip.open(os.path.join(FIX, NAMES[k]), "rt", errors="replace") as f:
            _RAW[k] = f.read().split("\n")
    return _RAW[k]


def clean(k):
    if k not in _CLEAN:
        _CLEAN[k] = [ANSI.sub("", l) for l in raw(k)]
    return _CLEAN[k]


def state(k, strip_esc=False):
    st = rxparse.RunState()
    for l in raw(k):
        st.feed_line(l.replace("\x1b", "") if strip_esc else l)      # monitor.py feeds lines with the ESC byte removed
    return st


def view(k):
    if k not in _VIEW:
        _VIEW[k] = state(k).mpci_view()
    return _VIEW[k]


def grep(k, pat):
    rx = re.compile(pat)
    return [l for l in clean(k) if rx.search(l)]


def num(line, key):
    return int(re.search(r"\b%s=(-?\d+)" % key, line)[1])


EXPECT = {"P1": {"pci": [415, 923], "ids": ["pci415", "pci923"]}, "Q1": {"pci": [415, 403], "ids": ["pci415", "pci403"]}}


class PerCell(unittest.TestCase):
    def test_cell_labels_and_pci(self):
        for k, e in EXPECT.items():
            v = view(k)
            self.assertEqual([c["pci"] for c in v["cells"]], e["pci"], k)
            self.assertEqual([c["id"] for c in v["cells"]], e["ids"], k)
            self.assertEqual([c["label"] for c in v["cells"]], ["PCI %d (c%d)" % (p, i) for i, p in enumerate(e["pci"])], k)

    def test_cell_lines_slot_and_events(self):
        for k in EXPECT:
            v = view(k)
            ev = {num(l, "slot"): l for l in grep(k, r"^MPCI_CELL id=")}
            for c in v["cells"]:
                l = ev[c["cell"]]
                self.assertEqual(c["events"][-1]["event"], re.search(r"event=(\w+)", l)[1])
                self.assertEqual(c["events"][-1]["centre_hz"], float(num(l, "centre_hz")))
                self.assertEqual(c["events"][-1]["cfo_hz"], float(num(l, "cfo_hz")))
            for l in grep(k, r"MPCI_CELL_SLOT cell="):
                c = v["cells"][num(l, "cell")]
                for f in ("n_rb", "ssb", "nb_rx"):
                    self.assertEqual(c["slot"][f], num(l, f), (k, f))
                self.assertEqual(c["slot"]["centre_hz"], float(num(l, "centre_hz")))
                self.assertEqual(c["slot"]["ring"], re.search(r"ring=(\S+)", l)[1])

    def test_ring_assignment(self):
        for k in EXPECT:
            v = view(k)
            for c, i in zip(v["cells"], range(2)):
                self.assertEqual(c["ring"]["name"], "/nrcell_" + c["id"], (k, i))

    def test_rt_and_life(self):
        for k in EXPECT:
            v = view(k)
            for l in grep(k, r"MPCI_RT cell="):
                self.assertEqual(v["cells"][num(l, "cell")]["rt_core"], num(l, "core"))
            l = grep(k, r"MPCI_LIFE activated")[0]
            self.assertEqual((v["life"]["cells"], v["life"]["prepared"], v["life"]["nmax"]), (num(l, "cells"), num(l, "prepared"), num(l, "nmax")))
            self.assertEqual(v["life"]["primary"], re.search(r"primary=(\S+)", l)[1])

    def test_sfo_resets_per_cell(self):
        for k in EXPECT:
            v = view(k)
            for c in v["cells"]:
                tag = "[c%d] " % c["cell"]
                by = {}
                for l in grep(k, r"SFO reset \("):
                    if l.startswith(tag):
                        r = re.search(r"SFO reset \(([^)]*)\)", l)[1]
                        by[r] = by.get(r, 0) + 1
                self.assertEqual(c["sfo"]["by_reason"], by, (k, c["cell"]))
                self.assertEqual(c["sfo"]["n"], sum(by.values()))
            self.assertEqual(sum(c["sfo"]["n"] for c in v["cells"]), len(grep(k, r"SFO reset \(")))
        self.assertGreater(view("P1")["cells"][0]["sfo"]["by_reason"].get("stream discontinuity", 0), 0)

    def test_core_pool_per_cell(self):
        for k in EXPECT:
            v = view(k)
            for c in v["cells"]:
                last = [l for l in grep(k, r"^\[c%d\] .*CORE_POOL n=" % c["cell"])][-1]
                for f in ("n", "yields", "gpu_gives", "nested", "enforced"):
                    self.assertEqual(c["pool"][f], num(last, f), (k, c["cell"], f))
                m = re.search(r"cell%d\[n=(\d+) waited=(\d+) hold_ms=(\d+)" % c["cell"], [l for l in grep(k, r"CORE_POOL n=")][-1])
                self.assertEqual((c["pool_cell"]["n"], c["pool_cell"]["waited"], c["pool_cell"]["hold_ms"]), tuple(int(x) for x in m.groups()))
            leases = grep(k, r"^SENSING: CORE_POOL long lease")
            self.assertEqual((v["core"]["long_lease"] or {"n": 0})["n"], len(leases))
            self.assertEqual((v["core"]["long_wait"] or {"n": 0})["n"], len(grep(k, r"^SENSING: CORE_POOL long wait")))
            self.assertEqual(v["core"]["pool_cores"], [int(x) for x in re.search(r"CORE_POOL: \d+ cores:((?: \d+)+)", "\n".join(clean(k)))[1].split()])


class Capture(unittest.TestCase):
    def test_admit(self):
        for k in EXPECT:
            a = view(k)["admit"]
            l = grep(k, r"^MPCI_ADMIT cells=")[0]
            self.assertEqual((a["cells"], a["n"], a["refuse"]), (2, 2, 0))
            for f in ("host_budget_mb", "host_used_mb", "gpu_budget_mb", "gpu_used_mb", "nid_window"):
                self.assertEqual(a[f], num(l, f), (k, f))
            self.assertEqual(a["reason"], "none")
            self.assertEqual(a["unserved"], [])

    def test_span(self):
        for k, (centre, rate, dec) in {"P1": (3720000000, 245760000, [2, 2]), "Q1": (3669945000, 122880000, [1, 4])}.items():
            l = grep(k, r"MPCI_SPAN ok=")[0]
            sp = view(k)["capture"]["span"]
            self.assertEqual((sp["centre_hz"], sp["rate"]), (float(num(l, "centre_hz")), float(num(l, "rate"))))
            self.assertEqual((sp["centre_hz"], sp["rate"]), (float(centre), float(rate)))
            self.assertEqual((sp["served"], sp["n_cells"], sp["n_rx"], sp["unserved"]), (2, 2, 4, []))
            self.assertEqual([c["decim"] for c in sp["cells"]], dec)
            shifts = [float(x) for x in re.findall(r":d\d+:([+-][\d.]+)", l)]
            self.assertEqual([c["shift_mhz"] for c in sp["cells"]], shifts)
            lo, hi = re.search(r"span_mhz=\[([\d.]+)\.\.([\d.]+)\]", l).groups()
            self.assertEqual((sp["span_lo_mhz"], sp["span_hi_mhz"]), (float(lo), float(hi)))
            rd = view(k)["capture"]["radio"]
            self.assertEqual((rd["device_rates"], rd["rx_channels"]), (7, 4))
            self.assertEqual(rd["analog_bw_mhz"], 400.0)

    def test_feed(self):
        for k, (ov, backend, thr) in {"P1": (147, "cpu", 7), "Q1": (24, "cpu", 3)}.items():
            cp = view(k)["capture"]
            lines = grep(k, r"MPCI_FEED rate=")
            self.assertEqual(cp["feed_n"], len(lines))
            self.assertEqual(len(cp["feed_series"]), len(lines))
            self.assertEqual(cp["feed_peak"]["chan_overruns"], max(num(l, "chan_overruns") for l in lines))
            self.assertEqual(cp["feed_peak"]["chan_overruns"], ov)       # equals SHMIQ_END discont, see below
            self.assertEqual(cp["feed_peak"]["chan_lag_max"], max(num(l, "chan_lag_max") for l in lines))
            self.assertEqual(cp["feed_peak"]["read_err"], max(num(l, "read_err") for l in lines))
            last = lines[-1]
            for f in ("reads", "samples", "gaps", "frozen", "short", "chan_discont"):
                self.assertEqual(cp["feed"][f], num(last, f), (k, f))
            self.assertEqual((cp["feed"]["backend"], cp["feed"]["threads_run"], cp["feed"]["threads_need"]), (backend, thr, thr))
            st = grep(k, r"MPCI_FEED started")[0]
            self.assertEqual((cp["feed_started"]["threads"], cp["feed_started"]["reader_cpu"], cp["feed_started"]["capacity_msps"]),
                             (num(st, "threads"), num(st, "reader_cpu"), float(re.search(r"capacity_msps=([\d.]+)", st)[1])))
            peaks = {}
            for l in lines:
                for tag, a, b, c in re.findall(r"(pci\d+):ov=(\d+),drop=(\d+),lag=(\d+)", l):
                    p = peaks.setdefault(tag, [0, 0, 0])
                    p[:] = [max(p[0], int(a)), max(p[1], int(b)), max(p[2], int(c))]
            for c in view(k)["cells"]:
                self.assertEqual(list(c["feed_peak"].values()), peaks[c["id"]], (k, c["id"]))
                self.assertEqual(c["feed"]["drop"], int(re.findall(r"%s:ov=\d+,drop=(\d+)" % c["id"], last)[0]))
        # P1: the mid-run line carries nonzero per-cell drop / lag that the shutdown snapshot has reset
        self.assertGreater(view("P1")["cells"][0]["feed_peak"]["drop"], 0)

    def test_rings(self):
        for k in EXPECT:
            cp = view(k)["capture"]
            ready, ends = grep(k, r"^SHMIQ_READY"), grep(k, r"^SHMIQ_END")
            self.assertEqual([r["name"] for r in cp["rings"]], [re.search(r"ring=(\S+)", l)[1] for l in ready])
            for r, l, e in zip(cp["rings"], ready, ends):
                self.assertEqual((r["ring_rate"], r["decim"], r["channels"], r["capacity"]), (float(num(l, "ring_rate")), num(l, "decim"), "4/4", num(l, "capacity")))
                self.assertEqual(r["end"], {f: num(e, f) for f in ("reads", "overruns", "dropped_in", "discont", "clipped")})
            disc = grep(k, r"^SHMIQ_DISCONT")
            self.assertEqual(cp["discont"]["n"], len(disc))
            self.assertEqual(cp["discont"]["skipped_in"], sum(num(l, "skipped_in") for l in disc))
            self.assertEqual(sum(r["end"]["discont"] for r in cp["rings"]), len(disc))      # per-ring END counts add up to the DISCONT lines
            tunes = grep(k, r"^SHMIQ_TUNE")
            for i, t in enumerate(cp["tunes"]):
                mine = [l for l in tunes if abs(float(re.search(r"ring_center_hz=([\d.]+)", l)[1]) - t["centre_hz"]) < 1]
                self.assertEqual(t["n"], len(mine))
                self.assertEqual(t["last"]["shift_hz"], float(re.search(r"shift_hz=(-?[\d.]+)", mine[-1])[1]))
                self.assertEqual(t["ring"], i)
            self.assertEqual(len(cp["tunes"]), 2)

    def test_checks_flag_overruns(self):
        for k in EXPECT:
            txt = " ".join(c["text"] for c in view(k)["checks"])
            self.assertIn("channelizer overruns", txt)


class Attribution(unittest.TestCase):
    def test_ansi_prefixed_tag_is_detected(self):
        l = [x for x in raw("P1") if "[c1]" in x and "MPCI_RT" in x][0]
        self.assertTrue(l.startswith("\x1b[0m[c1] "))
        self.assertEqual(rxparse.split_cell(l)[0], 1)
        self.assertEqual(rxparse.split_cell(l.replace("\x1b", ""))[0], 1)         # monitor.py removes only the ESC byte
        self.assertEqual(rxparse.split_cell("12.5 \x1b[0m[c1] \x1b[32m[PHY] x")[0], 1)    # colour code between timestamp and tag
        self.assertEqual(rxparse.split_cell("12.5 [0m[c2] [PHY] x")[0], 2)

    def test_esc_stripped_feed_gives_same_view(self):
        a = json.dumps(view("Q1"), sort_keys=True)
        b = json.dumps(state("Q1", strip_esc=True).mpci_view(), sort_keys=True)
        self.assertEqual(a, b)

    def test_two_cell_states(self):
        st = state("P1")
        self.assertEqual(sorted(st.children), [1])
        self.assertTrue(st.attributed)
        sn = st.snapshot_for(None)
        self.assertEqual(sn["mpci"]["cells"][1]["pci"], 923)
        self.assertEqual(st.snapshot_for("923")["mpci"]["cells"][0]["pci"], 415)        # same receiver-wide view whichever cell is selected

    def test_core_plan_line(self):
        st = rxparse.RunState()
        st.feed_line("SENSING: CORE_PLAN host=sens6 cpus=16 online=0-15 allowed=0-15 isolated=- nohz_full=- cap=- smt=0 llc=1 numa=1 gpu_node=0 "
                     "rt_cap=0 radio=0 dpdk=- -> mode=SEP W=12 rt=2 uhd=- ring=-1 pool=4,5,6 n=3 lane=7 home=8 hk=0 dashboard=-1 gpu=9 uss=10 nid=11 "
                     "check=- recov=- src=auto cells=2 rt_cells=0,1 max_cells=2")
        v = st.mpci_view()
        self.assertEqual(v, None)          # a plan line alone does not switch the multi-cell panels on (N=1 has the line too)
        st.feed_line("[c0] [PHY]    SENSING: MPCI_LIFE activated cells=2 prepared=2 of nmax=2 primary=pci1")
        p = st.mpci_view()["core"]["plan"]
        self.assertEqual((p["host"], p["mode"], p["W"], p["cells"], p["rt_cells"], p["max_cells"]), ("sens6", "SEP", 12, 2, [0, 1], 2))

    def test_unserved_and_refuse(self):
        st = rxparse.RunState()
        st.feed_line("MPCI_ADMIT cells=1 of 2 reason=host_mem refuse=0 host_budget_mb=100 host_used_mb=90 gpu_budget_mb=-1 gpu_used_mb=0 nid_window=0 unserved=[1:host_mem]")
        st.feed_line("[PHY]    SENSING: MPCI_SPAN ok=1 centre_hz=3700000000 rate=122880000 usable_mhz=98.304 span_mhz=[3650.0..3750.0] n_rx=4 served=1/2 "
                     "cells=pci1:d1:+0.000 unserved=pci2:outside_span device_rates=7 analog_mhz=400.0")
        v = st.mpci_view()
        self.assertEqual(v["admit"]["unserved"], [{"cell": "1", "reason": "host_mem"}])
        self.assertEqual(v["capture"]["span"]["unserved"], [{"id": "pci2", "reason": "outside_span"}])
        self.assertTrue(any("unserved" in c["text"] for c in v["checks"]))


class SingleCell(unittest.TestCase):
    def test_n1_has_no_multicell_view(self):
        for k in ("S1", "TN1"):
            st = state(k)
            self.assertIsNone(st.mpci_view(), k)
            self.assertFalse(st.attributed)
            self.assertEqual(st.children, {})
            self.assertIsNone(st.snapshot()["mpci"])


class Payload(unittest.TestCase):
    def test_state_payload_reads_gz_and_carries_mpci(self):
        import argparse
        import monitor
        d = tempfile.mkdtemp()
        shutil.copy(os.path.join(FIX, NAMES["P1"]), os.path.join(d, "run.log.gz"))
        ns = argparse.Namespace(run=d, follow=None, log=None, metrics=None, obs=None, records=None, cfr=None, runs_root=None, scan=None,
                                offline=True, port=0, host="127.0.0.1", label=None, flags=None, replay=None)
        mgr = monitor.Manager(ns)
        run = mgr.resolve("primary")
        run.advance()
        p = monitor.state_payload(mgr, run, {})
        m = p["snap"]["mpci"]
        self.assertEqual([c["pci"] for c in m["cells"]], [415, 923])
        self.assertEqual(m["capture"]["feed_peak"]["chan_overruns"], 147)
        json.dumps(p)
        d1 = tempfile.mkdtemp()
        shutil.copy(os.path.join(FIX, NAMES["S1"]), os.path.join(d1, "run.log.gz"))
        ns.run = d1
        mgr = monitor.Manager(ns)
        run = mgr.resolve("primary")
        run.advance()
        self.assertIsNone(monitor.state_payload(mgr, run, {})["snap"]["mpci"])

    def test_html_registers_the_page(self):
        with open(os.path.join(HERE, "monitor.html")) as f:
            h = f.read()
        self.assertIn("RENDER.mpci", h)
        self.assertIn('"mpci"', h)
        self.assertIn("navMpci", h)


SYNTH = os.path.join(PR, "mpci", "fixtures", "synth_dash_2cell.txt")


def synth_state():
    st = rxparse.RunState()
    with open(SYNTH, newline="") as f:
        for l in f.read().split("\n"):
            st.feed_line(l)
    return st


class DashLines(unittest.TestCase):
    """MPCI-DASH section 5 lines (P0 ring id, F1 split pool line, CELL_CPU/Q, LDPC_ITER, CELL_TIMING, CELL_GPU, CHAN_CELL, CELL_BUDGET, SLOT_CELL,
    CELL_SUM). Fixture: SYNTHETIC format-only (strings from the C format strings, invented numbers; no receiver run)."""

    @classmethod
    def setUpClass(cls):
        cls.v = synth_state().mpci_view()
        cls.c0, cls.c1 = cls.v["cells"]

    def test_cells_labelled_from_slot_lines(self):
        self.assertEqual([c["label"] for c in self.v["cells"]], ["PCI 415 (c0)", "PCI 923 (c1)"])

    def test_cpu(self):
        l0, l1 = self.c0["dash"]["cpu"]["last"], self.c1["dash"]["cpu"]["last"]
        self.assertEqual((l0["share_pct"], l0["tokens_avg"], l0["fair_pct"], l0["n"]), (31.5, 1.1, 50.0, 1800))
        self.assertEqual((l1["share_pct"], l1["wait_p99_us"], l1["ul.n"], l1["scan.hold_ms"]), (60.0, 16, 2, 2800))
        self.assertEqual(self.c0["dash"]["cpu"]["n"], 2)
        self.assertEqual([w["share_pct"] for w in self.c0["dash_series"]["cpu"]], [25.0, 31.5])

    def test_queues_totals_and_peaks(self):
        q = self.c1["dash"]["q"]
        self.assertEqual(q["last"]["scan.drop_full"], 60)
        self.assertEqual(q["last"]["pdsch.depth_now"], 3)
        self.assertEqual(q["tot"]["scan.drop_full"], 90)
        self.assertEqual(q["peak"]["scan.max_lag_win"], 12)

    def test_ldpc_hist(self):
        l = self.c1["dash"]["ldpc"]["last"]
        self.assertEqual(l["it_hist"], [2, 4, 6, 4, 2, 1, 1])
        self.assertEqual((l["tb"], l["cb"], l["at_cap_pct"], l["mean_it"]), (20, 80, 5.0, 3.1))

    def test_timing_gpu_slot_sum(self):
        t = self.c1["dash"]["timing"]["last"]
        self.assertEqual((t["sfo_eps_ppm"], t["cfo_hz"], t["shift_next"], t["discont"], t["resync"]), (-1.5, -120, -2, 4, 2))
        self.assertEqual(self.c1["dash"]["timing"]["tot"], {"discont": 6, "resync": 3})
        g = self.c1["dash"]["gpu"]["last"]
        self.assertEqual((g["active"], g["calls"], g["fallback"], g["fallback_pct"], g["gpu_mem_mb"], g["pin_mb"]), (1, 100, 10, 10.0, 4, 5))
        self.assertNotIn("gpu", self.c0["dash"])
        s = self.c1["dash"]["slot"]["last"]
        self.assertEqual((s["proc_p50"], s["proc_p99"], s["proc_max"], s["late_slots"], s["cpu"]), (256, 256, 3500, 3, 7))
        m = self.c1["dash"]["sum"]["last"]
        self.assertEqual((m["state"], m["decode_ok_pct"], m["grants"], m["cb_crc_pct"], m["drops"], m["disc"], m["tokens_avg"]),
                         ("TRACKING", 90.0, 100, 95.0, 12, 2, 0.5))
        self.assertEqual(self.c0["dash"]["sum"]["tot"]["grants"], 200)

    def test_chan_cell_and_budget(self):
        ch = self.c1["dash"]["chan"]["last"]
        self.assertEqual((ch["ring"], ch["lag_now"], ch["overruns_seen"], ch["paused"], ch["write_us_max"]), ("pci923", 1024, 1, 0, 240))
        b = self.c0["dash"]["budget"]["last"]
        self.assertEqual((b["ram_admit_mb"], b["gpu_admit_mb"], b["gpu_now_mb"], b["pin_now_mb"], b["tokens_fair"]), (1200, 800, 300, 20, 4.0))
        self.assertEqual(self.v["capture"]["budget_proc"], {"scope": "proc", "cells": 2, "host_budget_mb": 20000, "host_used_mb": 6000,
                                                          "headroom_mb": 14000, "avail_now_mb": 15000, "rss_mb": 5000})

    def test_f1_pool_split(self):
        self.assertTrue(self.v["dash"]["pool_cells_new_form"])
        pp = self.v["core"]["pool_proc"]
        self.assertEqual((pp["n"], pp["nested"], pp["split"]), (4, 200, True))
        self.assertEqual(sorted(pp["groups"]), ["decode", "disc", "house", "retry", "scan"])
        self.assertEqual((self.c0["pool_cell"]["n"], self.c0["pool_cell"]["pci"], self.c0["pool_cell"]["hold_ms"]), (4000, 415, 1800))
        self.assertEqual((self.c1["pool_cell"]["n"], self.c1["pool_cell"]["wait_p99"], self.c1["pool_cell"]["wait_max"]), (12000, 16, 1845))

    def test_p0_rings_attributed_by_id(self):
        r0, r1 = self.c0["ring"], self.c1["ring"]
        self.assertEqual((r0["id"], r0["by_id"], r1["id"]), ("pci415", True, "pci923"))
        self.assertEqual((r0["discont_n"], r1["discont_n"]), (2, 1))
        self.assertEqual((r0["tune_n"], r1["tune_n"]), (1, 2))
        self.assertEqual(r1["tune_last"]["shift_hz"], 240.0)
        # END lines are in reverse ring order in the fixture: attribution is by id, not by order
        self.assertEqual((r0["end"]["reads"], r1["end"]["reads"]), (1000, 900))
        self.assertEqual(self.v["capture"]["shmiq_unattributed"], {"discont": 0, "overrun": 0})
        self.assertEqual(self.v["capture"]["discont"]["n"], 3)

    def test_old_pid_form_falls_back(self):
        # old OTA fixture: cell=pid<n>: no per-ring attribution, no dash rows, END by order
        v = view("P1")
        self.assertTrue(all(not r.get("by_id") for r in v["capture"]["rings"]))
        self.assertTrue(all("discont_n" not in r for r in v["capture"]["rings"]))
        self.assertTrue(all(c["dash"] is None for c in v["cells"]))
        self.assertEqual(v["dash"]["n"], 0)
        self.assertFalse(v["dash"]["pool_cells_new_form"])
        self.assertTrue(all(r.get("end") for r in v["capture"]["rings"]))

    def test_n1_unchanged(self):
        for k in ("S1", "TN1"):
            self.assertIsNone(state(k).mpci_view(), k)

    def test_html_has_dash_panel(self):
        with open(os.path.join(HERE, "monitor.html")) as f:
            h = f.read()
        for tok in ("dashRows", "CELL_BUDGET scope=proc", "Per-cell load side by side"):
            self.assertIn(tok, h)


if __name__ == "__main__":
    unittest.main()
