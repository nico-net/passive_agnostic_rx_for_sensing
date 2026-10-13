import json, os, subprocess, sys, tempfile, time, unittest
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, H)
import mpci_auto as M
CELLS = os.path.join(H, '../../../docs/superpowers/reports/csd/frontlive_data/cells.json')
SH = os.path.join(H, 'mpci_launch.sh')
# receiver log lines (formats from nr_passive_acq_state.c / nr_initial_sync.c / SIB1 extraction)
MISMATCH = ('SENSING: ACQ carrier MISMATCH: SIB1 says 162 PRB mu=1 (Point A at grid subcarrier 972, offsetToCarrier 0, carrier end 2916) but PHY started '
            'with 217 PRB mu=1, grid end 2604; bw_match=0 mu_match=1 grid_match=0 -- x; the cell\'s carrier centre is 3590.010000 MHz, receiver started at 3570.570000 MHz (retune target)\n'
            'SENSING: KSSB_GRID k_ssb=0 ssb_sc=516: receiver RB grid off (aligned: --ssb 516 -C 3570570000)\n')
SYNC = ('[PHY] Initial sync successful, PCI: 508\nSENSING: MIB fields sfn=3 k_ssb=0\nextracted from SIB1 -- PCI=508 DL: 162 PRB\noffsetToPointA=40\n')

def cells(): return json.load(open(CELLS))

class T(unittest.TestCase):
    def plan(self, cs=None, cores='4-19'): return M.make_plan(cs or cells(), cores, pub_cores='0-1')

    def test_plan_default_and_pin(self):
        p = self.plan(); tags = [c['tag'] for c in p['cells']]
        self.assertEqual(tags, ['p508_7895', 'p4_7895'] if tags[0] == 'p508_7895' else tags)
        self.assertEqual(set(tags), {'p508_7895', 'p4_7895'})
        a, b = p['cells']; self.assertEqual(a['C_hz'], b['C_hz'])
        for c in p['cells']: self.assertEqual(c['env'], dict(ISAC_TARGET_PCI=str(c['pci']), ISAC_SSS_MULTI='2.5'))
        self.assertEqual({u['pci'] for u in p['unserved']}, {675, 403, 415, 923})
        self.assertEqual(p['centre_hz'], 3570570000); self.assertEqual(a['cores'], '4-11'); self.assertEqual(b['cores'], '12-19')
        self.assertNotEqual(a['cores'], b['cores'])

    def test_plan_with_pass2_widths(self):
        cs = cells()
        for c in cs:                                   # carrier widths: 508 162 PRB, 403 51 PRB (GRIDCARRIER), 415 51
            if c['pci'] in (508, 4, 403, 415) and c.get('k_ssb') is None: c['k_ssb'] = 0
            c['sib1_nrb'] = {508: 162, 4: 162, 403: 51, 415: 51}.get(c['pci'], 217)
        p = self.plan(cs); got = {c['pci'] for c in p['cells']}
        self.assertTrue({508, 4, 403} <= got, got)     # 3541.4..3599.7 and 3615.9..3634.6 fit 110 MHz
        c403 = next(c for c in p['cells'] if c['pci'] == 403); self.assertEqual(c403['env'], {})
        self.assertNotIn(675, got)

    def test_launch_dry(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.plan(); pp = os.path.join(d, 'plan.json'); M.save(p, pp)
            r = subprocess.run([sys.executable, os.path.join(H, 'mpci_auto.py'), 'launch', pp, '--out', d, '--pub-cmd', 'radio -x'],
                               env=dict(os.environ, DRY='1', BUILD='/b'), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('ISAC_SHM_IQ_PUB=isac_iq taskset -c 0-1 radio -x', r.stdout)
            l = [x for x in r.stdout.splitlines() if x.startswith('CELL p4_7895')][0]
            for k in ('ISAC_TARGET_PCI=4', 'ISAC_SSS_MULTI=2.5', 'taskset -c 12-19', '-r 217 --ssb 1171', 'ISAC_CELL_INSTANCE=p4_7895'): self.assertIn(k, l)
            self.assertEqual(M.spec(p['cells'][0]).count(':'), 6)

    def test_launcher_6_field_still_ok_and_bad_env(self):
        with tempfile.TemporaryDirectory() as d:
            e = dict(os.environ, DRY='1', OUT=d, BUILD='/b')
            self.assertEqual(subprocess.run(['bash', SH, 'A:1:2:3:-:0'], env=e, capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run(['bash', SH, 'A:1:2:3:-:0:X=1 ;rm'], env=e, capture_output=True).returncode, 2)

    def test_pass2_decision(self):
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, 'ue.log'); open(f, 'w').write(SYNC)
            self.assertIsNone(M.decide_retune(f))
            open(f, 'w').write(SYNC + MISMATCH); r = M.decide_retune(f)
            self.assertEqual((r['r'], r['env']), (162, {}))          # grid == carrier: no SIB1 override
            self.assertGreater(r['C_hz'], 3.57e9)

    def test_pass2_restart_only_that_cell(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.plan(); c = next(x for x in p['cells'] if x['pci'] == 508)
            os.makedirs(os.path.join(d, c['tag'])); open(os.path.join(d, c['tag'], 'ue.log'), 'w').write(SYNC + MISMATCH)
            calls = []; orig = M.restart_cell; M.restart_cell = lambda *a, **k: calls.append(a[2]['tag']) or True
            try: res = M.pass2_cell(p, d, c)
            finally: M.restart_cell = orig
            self.assertEqual((res, calls, c['r'], c['pass2']), ('restarted', ['p508_7895'], 162, 'retuned'))
            # window check: far-away retune target is not restarted
            c2 = dict(c, pass2=None); del c2['pass2']; p['centre_hz'] = 3000000000
            M.restart_cell = lambda *a, **k: calls.append('x'); 
            try: self.assertEqual(M.pass2_cell(p, d, c2), 'outside_window')
            finally: M.restart_cell = orig
            self.assertEqual(calls, ['p508_7895'])

    def test_watch_restart_backoff_sigint(self):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as d:
            b = os.path.join(d, 'bin'); os.mkdir(b); fake = os.path.join(b, 'nr-uesoftmodem')
            # exits by itself after 1 s (crash) unless FAKE_HANG: then traps SIGINT and records it
            open(fake, 'w').write('#!/usr/bin/env python3\nimport signal,time,os,sys\nm=os.environ["ISAC_CELL_INSTANCE"]\n'
                'signal.signal(signal.SIGINT,lambda *a:(open(os.environ["FAKE_DIR"]+"/sigint."+m,"a").write("x"),sys.exit(0)))\n'
                'time.sleep(1 if os.environ.get("FAKE_CRASH") else 60)\n'); os.chmod(fake, 0o755)
            cs = [c for c in cells() if c['pci'] in (508, 4)]; p = self.plan(cs, '0-3'); p['build'] = b; out = os.path.join(d, 'o')
            os.environ.update(FAKE_DIR=d, FAKE_CRASH='1')
            try:
                M.launch(p, out); self.assertEqual(open(os.path.join(out, 'pids')).read().count('\n'), 2)
                time.sleep(1.6); os.environ.pop('FAKE_CRASH'); rt = {}; t0 = 1000.0
                l = M.watch_step(p, out, rt, t0, backoff_s=10); self.assertTrue(all('RESTARTED' in x for x in l), l)
                self.assertEqual(open(os.path.join(out, 'pids')).read().count('\n'), 4)
                l = M.watch_step(p, out, rt, t0 + 1); self.assertTrue(all('DEAD' not in x and 'restarts=1' in x for x in l), l)
                time.sleep(0.8)  # let the fake install its handler
                # LOST cell: restarted only after lost_s, old process gets SIGINT, backoff (10*2^1 s after the 1st restart) respected
                tag = p['cells'][0]['tag']; open(os.path.join(out, tag, 'metrics.jsonl'), 'w').write('{"acq_state":"LOST"}\n')
                M.watch_step(p, out, rt, t0 + 2, lost_s=30); M.watch_step(p, out, rt, t0 + 25, lost_s=30)
                self.assertEqual(rt[tag]['restarts'], 1)
                l = M.watch_step(p, out, rt, t0 + 33, lost_s=30); self.assertEqual(rt[tag]['restarts'], 2, l)
                self.assertTrue(os.path.exists(os.path.join(d, 'sigint.' + tag)))
            finally:
                subprocess.run(['bash', SH, 'stop'], env=dict(os.environ, OUT=out), capture_output=True)

if __name__ == '__main__': unittest.main()
