import os, re, subprocess, tempfile, unittest
H = os.path.dirname(os.path.abspath(__file__))
SH = os.path.join(H, "mpci_launch.sh")
BASE = '''sensing = {
  enable = 1;
  records_path = "/tmp/rec.bin";
  cfr_path = "/tmp/cfr.bin";
  report_endpoint = "tcp://*:5556";
};
'''

def dry(out, *cells, **kw):
    env = dict(os.environ, DRY="1", OUT=out, RING="ringX", BUILD="/b", **kw)
    return subprocess.run(["bash", SH, *cells], env=env, capture_output=True, text=True)

class T(unittest.TestCase):
    def test_distinct_per_cell(self):
        with tempfile.TemporaryDirectory() as d:
            r = dry(d, "A:3619200000:106:516:-:4-7", "B:3649440000:51:200:-5000:8-11", PUB_CMD="radio -x", PUB_CORES="0-1")
            self.assertEqual(r.returncode, 0, r.stderr)
            cells = [l for l in r.stdout.splitlines() if l.startswith("CELL ")]
            self.assertEqual(len(cells), 2)
            self.assertIn("ISAC_SHM_IQ_PUB=ringX taskset -c 0-1 radio -x", r.stdout)
            for tag, cores in (("A", "4-7"), ("B", "8-11")):
                l = next(x for x in cells if x.startswith("CELL %s " % tag))
                for k in ("ISAC_CELL_INSTANCE=%s" % tag, "ISAC_SHM_IQ_CELL=%s" % tag, "ISAC_SHM_IQ_NAME=ringX",
                          "ISAC_METRICS_PATH=%s/%s/metrics.jsonl" % (d, tag), "cwd=%s/%s" % (d, tag),
                          "taskset -c %s " % cores):
                    self.assertIn(k, l)
            self.assertNotIn("--initial-fo", cells[0])
            self.assertIn("--initial-fo -5000", cells[1])
            for key in ("ISAC_METRICS_PATH", "ISAC_UECTX_PATH", "ISAC_OBS_PATH"):
                v = re.findall(key + r"=(\S+)", r.stdout)
                self.assertEqual(len(set(v)), 2)

    def test_cfg_generation(self):
        with tempfile.TemporaryDirectory() as d:
            base = os.path.join(d, "base.cfg"); open(base, "w").write(BASE)
            env = dict(os.environ, OUT=os.path.join(d, "o"), BUILD="/b", BASE_CFG=base, BASE_PORT="6000")
            # real (non-dry) run with a fake nr-uesoftmodem so we also exercise start/stop
            b = os.path.join(d, "bin"); os.mkdir(b)
            open(os.path.join(b, "nr-uesoftmodem"), "w").write("#!/usr/bin/env python3\nimport signal,time\nsignal.signal(signal.SIGINT,lambda *a:exit(0))\ntime.sleep(30)\n")
            os.chmod(os.path.join(b, "nr-uesoftmodem"), 0o755)
            env["BUILD"] = b
            r = subprocess.run(["bash", SH, "A:1:2:3:-:0", "B:1:2:3:-:0"], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            ca = open(os.path.join(d, "o/A/cell.cfg")).read(); cb = open(os.path.join(d, "o/B/cell.cfg")).read()
            self.assertIn('records_path = "/tmp/rec.bin.A"', ca); self.assertIn('cfr_path = "/tmp/cfr.bin.B"', cb)
            self.assertIn("tcp://*:6000", ca); self.assertIn("tcp://*:6001", cb)
            self.assertIn('name = "shm_iq"', ca); self.assertIn('shlibpath = "%s"' % b, ca)
            s = subprocess.run(["bash", SH, "stop"], env=env, capture_output=True, text=True)
            self.assertEqual(s.stdout.count("SIGINT"), 2)

    def test_per_cell_ring_nant_pub_env(self):
        with tempfile.TemporaryDirectory() as d:
            r = dry(d, "A:1:2:3:-:0:ISAC_SHM_IQ_NAME=ringY,UE_NB_ANT_RX=2,K=V", "B:1:2:3:-:1", PUB_CMD="radio", PUB_ENV="ISAC_SHM_IQ_PUB=ringX:0-1,ringY:2 ISAC_SHM_IQ_PUB_FREQ=ringY=1e9")
            self.assertEqual(r.returncode, 0, r.stderr); L = r.stdout.splitlines()
            self.assertIn("PUB env ISAC_SHM_IQ_PUB=ringX:0-1,ringY:2 ISAC_SHM_IQ_PUB_FREQ=ringY=1e9 radio", L)
            a = next(x for x in L if x.startswith("CELL A ")); b = next(x for x in L if x.startswith("CELL B "))
            self.assertIn("ISAC_SHM_IQ_NAME=ringY K=V", a); self.assertIn("--ue-nb-ant-rx 2", a); self.assertNotIn("UE_NB_ANT_RX", a)
            self.assertNotIn("--ue-nb-ant-rx", b)

    def test_cell_args_and_core_lists(self):
        with tempfile.TemporaryDirectory() as d:
            r = dry(d, "A:1:2:3:-:0:ISAC_PDSCH_CORES=8+9+12,K=V", CELL_ARGS="--passive-rx --band 78")
            self.assertEqual(r.returncode, 0, r.stderr)
            a = next(x for x in r.stdout.splitlines() if x.startswith("CELL A "))
            self.assertIn("ISAC_PDSCH_CORES=8,9,12 K=V", a)
            self.assertTrue(a.endswith("--passive-rx --band 78"), a)

    def test_chan_exit_blocks_cells(self):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, OUT=d, BUILD="/b", CHAN_CMD="false", CHAN_RINGS="mpcit_never_%d" % os.getpid())
            r = subprocess.run(["bash", SH, "A:1:2:3:-:0"], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 3, r.stderr); self.assertIn("channelizer exited", r.stderr)
            self.assertEqual(open(os.path.join(d, "pids")).read().split()[0], "chan")      # no cell was started

    def test_rejects_bad_spec(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(dry(d, "bad tag:1:2:3:-:0").returncode, 2)
            self.assertEqual(dry(d, "A:1:2:3:-:0", "A:1:2:3:-:1").returncode, 2)

if __name__ == "__main__":
    unittest.main()
