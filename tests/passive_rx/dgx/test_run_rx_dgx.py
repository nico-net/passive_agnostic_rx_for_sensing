import os, subprocess, tempfile, unittest
H = os.path.dirname(os.path.abspath(__file__)); L = os.path.join(H, "run_rx_dgx.sh")
DGX = {"ONLINE_CPUS_OVERRIDE": "0-19"}  # emulate the GB10 topology; the cloud host has 4 cores

def dry(env=None, args=("--passive-rx",)):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([L, "--dry-run", "--", *args], capture_output=True, text=True, env=e)

def conf(line):
    f = tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False)
    f.write("sensing = {\n  %s\n};\n" % line); f.close()
    return f.name

class Launcher(unittest.TestCase):
    def test_instance_a_pins_rt_to_x925_core5(self):
        r = dry({"INSTANCE": "A", **DGX}); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ISAC_UE_RT_CORE=5", r.stdout); self.assertIn("--thread-pool 8,9,0,1", r.stdout)
        self.assertIn("COREMAP applied", r.stdout)

    def test_instance_b_is_cluster1(self):
        r = dry({"INSTANCE": "B", **DGX}); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ISAC_UE_RT_CORE=15", r.stdout); self.assertIn("--thread-pool 18,19,10,11", r.stdout)

    def test_refuses_offline_cpu(self):
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-3"}); self.assertNotEqual(r.returncode, 0)
        self.assertIn("not online", r.stderr)

    def test_refuses_on_this_host_without_override(self):
        with open("/sys/devices/system/cpu/online") as f: online = f.read().strip()
        if online == "0-19": self.skipTest("host actually has 20 CPUs")
        e = {k: v for k, v in os.environ.items() if k != "ONLINE_CPUS_OVERRIDE"}; e["INSTANCE"] = "A"
        r = subprocess.run([L, "--dry-run", "--", "--passive-rx"], capture_output=True, text=True, env=e)
        self.assertNotEqual(r.returncode, 0); self.assertIn("not online", r.stderr)

    def test_rfsim_arm_coremap_refuses_before_starting_anything(self):
        with open("/sys/devices/system/cpu/online") as f: online = f.read().strip()
        if online == "0-19": self.skipTest("host actually has 20 CPUs")
        e = {k: v for k, v in os.environ.items() if k != "ONLINE_CPUS_OVERRIDE"}; e["COREMAP"] = "1"
        out = tempfile.mkdtemp()
        r = subprocess.run([os.path.join(H, "rfsim_arm.sh"), os.path.join(out, "o"), "1"], capture_output=True, text=True, env=e, timeout=30)
        self.assertEqual(r.returncode, 3); self.assertIn("not online", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(out, "o")))

    def test_invalid_instance(self):
        r = dry({"INSTANCE": "C", **DGX}); self.assertNotEqual(r.returncode, 0)
        self.assertIn("INSTANCE must be A or B", r.stderr)

    def test_scan_thread_on_a725_warns(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:3";')
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("WARNING", r.stderr); self.assertIn("A725", r.stderr)

    def test_scan_thread_b_a725_range(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:12";')
        r = dry({"INSTANCE": "B", **DGX}, ("--passive-rx", "-O", c)); self.assertIn("WARNING", r.stderr)
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertNotIn("WARNING", r.stderr)

    def test_scan_thread_on_x925_no_warning(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:6";')
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertNotIn("WARNING", r.stderr)

    def test_scan_thread_unpinned_no_warning(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:-1";')
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 0)
        self.assertNotIn("WARNING", r.stderr)

    def test_missing_or_unreadable_conf_ok(self):
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", "/nonexistent.cfg")); self.assertEqual(r.returncode, 0)
        self.assertNotIn("WARNING", r.stderr)
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx",)); self.assertNotIn("WARNING", r.stderr)

    def test_shipped_dgx_cfg_has_no_warning(self):
        c = os.path.join(H, "ue.passive.auto.100mhz.dgx.cfg")
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("WARNING", r.stderr)

    def test_scan_thread_equal_rt_core_refused(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:5";')
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 3)
        self.assertIn("collides", r.stderr)

    def test_scan_thread_equal_uss_core_refused(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:7";')
        r = dry({"INSTANCE": "A", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 3)
        self.assertIn("collides", r.stderr)

    def test_middle_pdsch_core_offline_refused(self):
        # conf pdsch cores 11,12,13; core 12 offline (first and last online) -> must refuse, naming pdsch
        c = conf('pdcch_blind_monitor_pdsch = "1:1:0:1:16:3:20:11";')
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-11,13-19"}, ("--passive-rx", "-O", c))
        self.assertEqual(r.returncode, 3); self.assertIn("pdsch", r.stderr)

    def test_default_pdsch_middle_core_checked(self):
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-2,4-19"}); self.assertEqual(r.returncode, 3)

    def test_conf_scan_core_offline_refused(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:12";')
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-11,13-19"}, ("--passive-rx", "-O", c))
        self.assertEqual(r.returncode, 3); self.assertIn("scan_thread", r.stderr)

    def test_conf_ul_thread_core_offline_refused(self):
        c = conf('pdcch_blind_monitor_ul_thread = "2:32:12";')  # cores 12,13
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-12,14-19"}, ("--passive-rx", "-O", c))
        self.assertEqual(r.returncode, 3); self.assertIn("ul_thread", r.stderr)

    def test_instance_b_with_cluster0_pins_refused(self):
        c = os.path.join(H, "ue.passive.auto.100mhz.dgx.cfg")  # instance-A conf
        r = dry({"INSTANCE": "B", **DGX}, ("--passive-rx", "-O", c))
        self.assertEqual(r.returncode, 3); self.assertIn("cluster-0", r.stderr)

    def test_instance_b_with_cluster1_pins_ok(self):
        c = conf('pdcch_blind_monitor_scan_thread = "1:8:16";\n  pdcch_blind_monitor_pdsch = "1:1:0:1:16:3:20:12";\n  pdcch_blind_monitor_ul_thread = "2:32:12";')
        r = dry({"INSTANCE": "B", **DGX}, ("--passive-rx", "-O", c)); self.assertEqual(r.returncode, 0, r.stderr)

    def test_comma_list_online_mask(self):
        # "0-3,5,7-9" expands to 0,1,2,3,5,7,8,9: core 4 and 6 missing -> A map (core 5,6,7,...) refused on 6
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-3,5,7-9"}); self.assertEqual(r.returncode, 3)
        self.assertIn("core 6 is not online", r.stderr)
        r = dry({"INSTANCE": "A", "ONLINE_CPUS_OVERRIDE": "0-9,12,15-19"}); self.assertEqual(r.returncode, 0, r.stderr)

if __name__ == "__main__":
    unittest.main()
