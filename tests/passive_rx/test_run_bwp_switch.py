#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""Offline check of run_bwp_switch.sh: a fake runner stands in for run_passive_rx.sh and a fake
telnet server for the gNB's telnetsrv CI module. Run: python3 tests/passive_rx/test_run_bwp_switch.py"""
import os, socket, subprocess, tempfile, threading, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
WRAPPER = HERE / "run_bwp_switch.sh"

# Written before the trigger: an armed tracker and a first per-RNTI census.
PRE = """[PHY] SENSING: BWP tracking armed: base len=41 size=106 start=0 ind_bits=0, 4 candidate lengths
[PHY] SENSING: PDSCHQ per-rnti 0x4601:10/12(83%)
[PHY] pdsch_decode[try=12 crc_ok=10 (83.3%) skip_rv=0]
"""
# Written after the trigger: the receiver follows the switch and keeps decoding.
POST = """[PHY] SENSING: BWP NEW entry=2 len=37 from rnti 0x4601 (2 indicator-width hypotheses) -- x
[PHY] SENSING: BWP RESOLVED entry=2 len=37 size=24 start=70 ind_bits=1 after 9 grants
[PHY] SENSING: BWP SWITCH rnti=0x4601 -> entry 2 (len 37, start 70, size 24), switches=1
[PHY] SENSING: PDSCHQ per-rnti 0x4601:25/30(83%)
[PHY] pdsch_decode[try=30 crc_ok=25 (83.3%) skip_rv=0]
[PHY] SENSING: PDSCHQ queued=40 decoded=31 crc_ok=999 (99.0%) dropped[full=0 stale=0]
"""


class FakeTelnet(threading.Thread):
    """Accepts connections, records each command line, answers like telnetsrv's CI module."""

    def __init__(self, refuse=False):
        super().__init__(daemon=True)
        self.refuse = refuse
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.cmds = []

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                line = conn.makefile().readline().strip()
                self.cmds.append(line)
                if line.startswith("ci trigger_bwp_switch") and self.refuse:
                    conn.sendall(b"failed trigger BWP switch for UE 4601 BWP ID 2\nsoftmodem_ci> ")
                elif line.startswith("ci trigger_bwp_switch"):
                    conn.sendall(b"triggered BWP switch to BWP ID 2 for UE 4601\nsoftmodem_ci> ")
                else:
                    conn.sendall(b"UE 4601 DL BWP ID 1 UL BWP ID 1\nsoftmodem_ci> ")


def fake_runner(tmp, rc=0, straddle=False):
    """Pre lines, then (optionally) the first half of a line the trigger will split, a 3 s gap
    (the wrapper triggers at 1 s), the rest of that line, then post lines."""
    runner = Path(tmp) / "fake_runner.sh"
    head = "[PHY] SENSING: BWP SWITCH rnti=0x4601 -> entry 1" if straddle else ""
    tail = " (len 41, start 0, size 106), switches=1\n" if straddle else ""
    runner.write_text(
        "#!/bin/bash\n"
        "# args: duration out_dir -- same contract as run_passive_rx.sh\n"
        'echo "env GNB_CONF_OVERRIDE=$GNB_CONF_OVERRIDE CONF_TAG=$CONF_TAG ISAC_BWP_TRACK=$ISAC_BWP_TRACK GNB_EXTRA=$GNB_EXTRA" > "$2/env.txt"\n'
        f"cat > \"$2/ue_rx1.log\" <<'EOF'\n{PRE}EOF\n"
        f"printf '%s' '{head}' >> \"$2/ue_rx1.log\"\n"
        "sleep 3\n"
        f"printf '%s' '{tail}' >> \"$2/ue_rx1.log\"\n"
        f"cat >> \"$2/ue_rx1.log\" <<'EOF'\n{POST}EOF\n"
        f"exit {rc}\n")
    runner.chmod(0o755)
    return runner


class RunBwpSwitchTest(unittest.TestCase):
    def run_wrapper(self, mode, rc=0, straddle=False, refuse=False):
        tmp = tempfile.mkdtemp()
        tel = FakeTelnet(refuse)
        tel.start()
        out = Path(tmp) / "out"
        env = dict(os.environ, RUNNER=str(fake_runner(tmp, rc, straddle)), TELNET_PORT=str(tel.port),
                   TRIGGER_AT="1")
        r = subprocess.run(["bash", str(WRAPPER), mode, "3", str(out)], env=env, capture_output=True,
                           text=True, timeout=30)
        tel.sock.close()
        return r, out, tel.cmds

    def test_switch_triggers_and_scores_only_post_trigger_evidence(self):
        r, out, cmds = self.run_wrapper("switch")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ci trigger_bwp_switch 2", cmds)
        env = (out / "env.txt").read_text()
        self.assertIn("GNB_CONF_OVERRIDE=gnb.sa.rfsim.bwp.conf", env)
        self.assertIn("CONF_TAG=.bwp.agn", env)
        self.assertIn("ISAC_BWP_TRACK=1", env)  # the receiver feature under test
        self.assertIn("--telnetsrv", env)
        self.assertIn("--telnetsrv.shrmod ci", env)
        self.assertIn("triggered BWP switch", (out / "telnet.log").read_text())
        s = (out / "bwp_summary.txt").read_text()
        self.assertIn("switch_post=1", s)
        self.assertIn("resolved_post=1", s)
        self.assertIn("new_post=1", s)
        self.assertIn("crc_ok_pre=10", s)
        self.assertIn("crc_ok_post=25", s)
        self.assertIn("rnti_census_post=0x4601:25/30", s)

    def test_baseline_never_triggers(self):
        r, out, cmds = self.run_wrapper("baseline")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse([c for c in cmds if "trigger_bwp_switch" in c], cmds)
        self.assertIn("ci get_current_bwp", cmds)
        s = (out / "bwp_summary.txt").read_text()
        self.assertIn("mode=baseline", s)
        self.assertIn("switch_post=1", s)  # counted, not injected: scoring is mode-independent

    def test_line_straddling_the_trigger_is_counted_on_neither_side(self):
        r, out, _ = self.run_wrapper("switch", straddle=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        s = (out / "bwp_summary.txt").read_text()
        self.assertIn("switch_post=1", s)  # the split line is dropped, only the real post line counts
        self.assertIn("switch_pre=0", s)
        self.assertIn("crc_ok_pre=10", s)

    def test_runner_failure_propagates(self):
        r, out, _ = self.run_wrapper("baseline", rc=3)
        self.assertEqual(r.returncode, 3)

    def test_refused_trigger_fails_the_switch_arm(self):
        r, out, _ = self.run_wrapper("switch", refuse=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("trigger_ok=0", (out / "bwp_summary.txt").read_text())

    def test_missing_receiver_log_fails(self):
        tmp = tempfile.mkdtemp()
        runner = Path(tmp) / "r.sh"
        runner.write_text("#!/bin/bash\nexit 0\n")
        runner.chmod(0o755)
        tel = FakeTelnet()
        tel.start()
        r = subprocess.run(["bash", str(WRAPPER), "baseline", "3", str(Path(tmp) / "o")], capture_output=True,
                           text=True, timeout=30, env=dict(os.environ, RUNNER=str(runner),
                                                           TELNET_PORT=str(tel.port), TRIGGER_AT="1"))
        tel.sock.close()
        self.assertNotEqual(r.returncode, 0)

    def test_conf_enables_the_pdsch_queue(self):
        # The per-RNTI CRC census (PDSCHQ per-rnti) and the BWP tracker's TB-CRC feedback exist only
        # when the deferred decode queue runs: field 6 (consumer threads) of pdcch_blind_monitor_pdsch.
        conf = (HERE / "ue.passive.bwp.agn.conf").read_text()
        line = [l for l in conf.splitlines() if l.strip().startswith("pdcch_blind_monitor_pdsch")][0]
        fields = line.split('"')[1].split(":")
        self.assertGreaterEqual(len(fields), 6, line)
        self.assertGreater(int(fields[5]), 0, line)

    def test_rejects_unknown_mode(self):
        r = subprocess.run(["bash", str(WRAPPER), "bogus"], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
