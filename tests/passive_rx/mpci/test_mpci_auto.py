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

PUBC = os.path.join(H, '../../../radio/COMMON/shm_iq_pub.c')

def real_parser(d, nch, env):
    """compile the REAL parse_env/parse_freq_dwell (text-extracted from shm_iq_pub.c, file untouched) and dump what they parse; None if no gcc."""
    import re, shutil
    if not shutil.which('gcc'): return None
    t = open(PUBC).read()
    td = t[t.index('typedef struct {'):t.index('} pub_ring_t;') + 13]
    fn = t[t.index('static int ring_by_name'):t.index('void shm_iq_pub_maybe_wrap')]
    src = ('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <errno.h>\n#include "shm_iq_ring.h"\n' + td +
           'static pub_ring_t g_r[SHM_IQ_MAX_CH]; static int g_nr, g_nd; static int g_dwell_idx[SHM_IQ_MAX_CH];\n' + fn +
           'int main(int c, char **v){ parse_env(getenv("ISAC_SHM_IQ_PUB"), atoi(v[1])); parse_freq_dwell();'
           ' for(int i=0;i<g_nr;i++){ printf("%s freq=%.0f dwell=%d ch=", g_r[i].name, g_r[i].freq, g_r[i].dwell_ms);'
           ' for(unsigned k=0;k<g_r[i].nch;k++) printf("%u,", g_r[i].ch[k]); printf("\\n"); } printf("nd=%d\\n", g_nd); return 0; }\n')
    open(os.path.join(d, 'h.c'), 'w').write(src)
    p = subprocess.run(['gcc', '-w', '-I', os.path.join(H, '../../../radio/COMMON'), '-o', os.path.join(d, 'h'), os.path.join(d, 'h.c')], capture_output=True, text=True)
    if p.returncode: return None
    r = subprocess.run([os.path.join(d, 'h'), str(nch)], env=dict(os.environ, **env), capture_output=True, text=True)
    return r

class T(unittest.TestCase):
    def plan(self, cs=None, cores='4-19', **kw): return M.make_plan(cs or cells(), cores, pub_cores='0-1', **kw)

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

    # ---- M4 multi-window
    def sub(self, pcis): return [c for c in cells() if c['pci'] in pcis]

    def test_multi_cover_fixture(self):
        p = self.plan(multi=True, cores='4-19'); w = p['windows']
        self.assertEqual([x['cells'] for x in w], [['p675_7795'], ['p508_7895', 'p4_7895'], ['p403_7933'], ['p415_7971'], ['p923_8007']])
        self.assertEqual(p['unserved'], []); self.assertEqual(len(p['cells']), 6)
        for x in w: self.assertLessEqual(x['span_hz'], 110e6)
        two = next(c for c in p['cells'] if c['pci'] == 4); self.assertEqual(two['env']['ISAC_TARGET_PCI'], '4')   # co-located still pinned
        self.assertEqual(next(c for c in p['cells'] if c['pci'] == 403)['env'], {})
        self.assertEqual(len({c['cores'] for c in p['cells']}), 6)

    def test_multi_dedicated_and_env(self):
        p = self.plan(self.sub((675, 508, 4, 923)), multi=True)               # 3 windows, 4 channels: 508+4 has most cells -> 2 ch
        self.assertEqual([x['channels'] for x in p['windows']], [[0], [1, 2], [3]]); self.assertTrue(all(x['mode'] == 'dedicated' for x in p['windows']))
        e = p['radio']['pub_env']; self.assertEqual(e['ISAC_SHM_IQ_PUB'], 'isac_iq_w0:0,isac_iq_w1:1-2,isac_iq_w2:3'); self.assertNotIn('ISAC_SHM_IQ_DWELL', e)
        self.assertEqual({c['tag']: c['nrx'] for c in p['cells']}, {'p675_7795': 1, 'p508_7895': 2, 'p4_7895': 2, 'p923_8007': 1})
        self.assertEqual({c['tag']: c['ring'] for c in p['cells']}['p4_7895'], 'isac_iq_w1')
        with tempfile.TemporaryDirectory() as d:
            r = real_parser(d, 4, e)
            if r is None: self.skipTest('no gcc / header')
            self.assertEqual(r.returncode, 0, r.stderr)
            f = {l.split()[0]: l for l in r.stdout.splitlines()}
            for w in p['windows']:
                self.assertIn(f"freq={w['centre_hz']} ", f[w['ring']]); self.assertIn('ch=' + ''.join(f'{c},' for c in w['channels']), f[w['ring']])

    def test_multi_dwell_grouping(self):
        p = self.plan(multi=True); w = p['windows']                          # 5 windows > 4 channels: 3 dedicated (most cells, then lowest), 2 share ch 3
        self.assertEqual([x['ring'] for x in w if x['mode'] == 'dwell'], ['isac_iq_w3', 'isac_iq_w4'])
        self.assertEqual({x['channels'][0] for x in w if x['mode'] == 'dedicated'}, {0, 1, 2}); self.assertEqual([x['dwell_ms'] for x in w if x['mode'] == 'dwell'], [40, 40])
        # proportional dwell: a 2-cell window in the shared group gets twice, clamped to the cap
        a = M.assign_channels([[1], [1], [1, 2], [1, 2, 3, 4, 5, 6]], 2, 1, 40, 200)
        self.assertEqual([x['dwell_ms'] for x in a if x['mode'] == 'dwell'], [40, 40, 80]); self.assertEqual(a[3]['mode'], 'dedicated')
        a = M.assign_channels([[1] * 7, [1], [1]], 1, 1, 40, 200); self.assertEqual([x['dwell_ms'] for x in a], [200, 40, 40])   # cap
        e = p['radio']['pub_env']
        self.assertEqual(e['ISAC_SHM_IQ_DWELL'], ','.join(f"{x['ring']}@{x['centre_hz']}:40" for x in w if x['mode'] == 'dwell'))
        self.assertEqual(e['ISAC_SHM_IQ_PUB'].count('isac_iq_w3:3'), 1); self.assertNotIn('isac_iq_w3=', e['ISAC_SHM_IQ_PUB_FREQ'])
        with tempfile.TemporaryDirectory() as d:
            r = real_parser(d, 4, e)
            if r is None: self.skipTest('no gcc / header')
            self.assertEqual(r.returncode, 0, r.stderr); self.assertIn('nd=2', r.stdout)
            self.assertIn('isac_iq_w3 freq=%d dwell=40 ch=3,' % w[3]['centre_hz'], r.stdout)

    def test_min_ch_and_single_window_form(self):
        p = self.plan(multi=True, channels=4, min_ch=2)                      # D=2: 1 dedicated (508+4, 2 ch), 4 windows share 2 ch
        self.assertEqual([x['mode'] for x in p['windows']], ['dwell', 'dedicated', 'dwell', 'dwell', 'dwell']); self.assertEqual(p['windows'][1]['channels'], [0, 1])
        self.assertEqual(p['windows'][0]['channels'], [2, 3])
        p = self.plan()                                                      # v1 behaviour: one window, rest unserved, plain publisher env
        self.assertEqual((len(p['windows']), p['radio']['pub_env']), (1, {'ISAC_SHM_IQ_PUB': 'isac_iq'}))

    def test_launch_dry_multi(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.plan(multi=True); pp = os.path.join(d, 'plan.json'); M.save(p, pp)
            r = subprocess.run([sys.executable, os.path.join(H, 'mpci_auto.py'), 'launch', pp, '--out', d, '--pub-cmd', 'radio -x'],
                               env=dict(os.environ, DRY='1', BUILD='/b'), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr); pub = [l for l in r.stdout.splitlines() if l.startswith('PUB ')]
            self.assertEqual(len(pub), 1)
            for k, v in p['radio']['pub_env'].items(): self.assertIn(f'{k}={v}', pub[0])
            for c in p['cells']:
                l = [x for x in r.stdout.splitlines() if x.startswith(f"CELL {c['tag']} ")][0]
                self.assertTrue(l.index(f"ISAC_SHM_IQ_NAME={c['ring']}") > l.index('ISAC_SHM_IQ_NAME=isac_iq '), l)   # per-cell value comes last: wins in env
                self.assertIn(f"--ue-nb-ant-rx {c['nrx']}", l); self.assertNotIn('UE_NB_ANT_RX', l)

    def test_outside_window_reported_multi(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.plan(multi=True); c = next(x for x in p['cells'] if x['pci'] == 508)
            os.makedirs(os.path.join(d, c['tag'])); open(os.path.join(d, c['tag'], 'ue.log'), 'w').write(SYNC + MISMATCH)
            p['windows'][c['window']]['centre_hz'] = 3000000000
            calls = []; orig = M.restart_cell; M.restart_cell = lambda *a, **k: calls.append(1)
            try: self.assertEqual(M.pass2_cell(p, d, c), 'outside_window')
            finally: M.restart_cell = orig
            self.assertEqual(calls, []); self.assertIn('wanted', c)
        w = p['windows'][0]; w['mode'] = 'dwell'; w['dwell_ms'] = 40
        with tempfile.TemporaryDirectory() as d:
            self.assertIn('dwell=40ms switches=0', M.window_status(p, d, w))


    # ---- wideband (one window, all channels, standalone UHD publisher)
    def wb(self, rate, **kw): return self.plan(wideband=True, rate=rate, uhd_args='type=x4xx,addr=1.2.3.4', gain=30, **kw)

    def test_wideband_245(self):
        p = self.wb(245.76e6); got = {c['pci'] for c in p['cells']}
        # 200 MHz X410 IBW at 245.76 MS/s with pass-1 (217 PRB) widths: 923 does not fit with 508/4
        self.assertEqual(got, {508, 4, 403, 415}); self.assertEqual(sorted(u['pci'] for u in p['unserved']), [675, 923])
        self.assertIn('M4', p['unserved'][0]['fallback']); self.assertEqual(len(p['windows']), 1)
        w = p['windows'][0]; self.assertEqual(w['channels'], [0, 1, 2, 3]); self.assertAlmostEqual(w['centre_hz'] / 1e6, 3625.29, delta=2)
        self.assertLessEqual(w['span_hz'], 200e6); self.assertEqual(p['radio']['ibw_hz'], 200000000)
        for c in p['cells']: self.assertEqual((c['nrx'], c['decim']), (4, 2))        # 217 PRB pass-1 grid = 122.88 MS/s
        self.assertEqual(p['radio']['pub_cmd'], f"shm_iq_uhd_pub --args type=x4xx,addr=1.2.3.4 --rate 245760000 --freq {w['centre_hz']} --gain 30 --channels 4 --antenna RX1 --ring isac_iq")
        pin = {c['pci'] for c in p['cells'] if c['env']}; self.assertEqual(pin, {508, 4})

    def test_wideband_491_all_and_decim(self):
        p = self.wb(491.52e6); self.assertEqual(len(p['cells']), 6); self.assertEqual(p['unserved'], [])
        self.assertEqual({c['decim'] for c in p['cells']}, {4})
        cs = cells()
        for c in cs: c['k_ssb'] = 0; c['sib1_nrb'] = {508: 162, 4: 162, 403: 51, 415: 51, 923: 106, 675: 273}[c['pci']]
        d = {c['pci']: c['decim'] for c in self.wb(491.52e6, cs=cs)['cells']}
        self.assertEqual(d, {508: 8, 4: 8, 403: 16, 415: 16, 923: 8, 675: 4})
        self.assertEqual(M.decim_of(245.76e6, 51), 8); self.assertIsNone(M.decim_of(184.32e6, 51)); self.assertIsNone(M.decim_of(61.44e6, 217))

    def test_wideband_needs_args_and_launch_dry(self):
        with self.assertRaises(SystemExit): self.plan(wideband=True, rate=245.76e6)
        with tempfile.TemporaryDirectory() as d:
            p = self.wb(245.76e6); pp = os.path.join(d, 'plan.json'); M.save(p, pp)
            r = subprocess.run([sys.executable, os.path.join(H, 'mpci_auto.py'), 'launch', pp, '--out', d],
                               env=dict(os.environ, DRY='1', BUILD='/b'), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr); pub = [l for l in r.stdout.splitlines() if l.startswith('PUB ')]
            self.assertEqual(len(pub), 1); self.assertIn('taskset -c 0-1 shm_iq_uhd_pub --args type=x4xx,addr=1.2.3.4 --rate 245760000', pub[0])
            cl = [l for l in r.stdout.splitlines() if l.startswith('CELL ')]; self.assertEqual(len(cl), 4)  # 200 MHz IBW: 508, 4, 403, 415
            for l in cl: self.assertIn('--ue-nb-ant-rx 4', l)

    def test_wideband_default_cli_and_fallback_dwell(self):
        with tempfile.TemporaryDirectory() as d:
            json.dump(cells(), open(os.path.join(d, 'c.json'), 'w')); base = [sys.executable, os.path.join(H, 'mpci_auto.py'), 'plan', os.path.join(d, 'c.json'), '--cores', '4-19', '--chan-cores', '20-21', '--out', os.path.join(d, 'p.json')]
            def run(*x): return subprocess.run(base + list(x), capture_output=True, text=True)
            r = run('--uhd-args', 'type=x4xx,addr=1.2.3.4', '--gain', '30'); self.assertEqual(r.returncode, 0, r.stderr)
            p = json.load(open(os.path.join(d, 'p.json'))); self.assertEqual((p['rate_hz'], len(p['windows']), p['radio']['wideband']), (245760000, 1, True))
            self.assertEqual({c['pci'] for c in p['cells']}, {508, 4, 403, 415}); self.assertEqual(sorted(u['pci'] for u in p['unserved']), [675, 923])
            self.assertIn('--rate 245760000 --freq', p['radio']['pub_cmd']); self.assertIn('--channels 4 --antenna RX1', p['radio']['pub_cmd'])
            r = run('--uhd-args', 'type=x4xx,addr=1.2.3.4', '--gain', '30', '--wideband'); self.assertEqual(r.returncode, 0, r.stderr)    # alias
            r = run('--uhd-args', 'type=x4xx,addr=1.2.3.4', '--gain', '30', '--multi-window'); self.assertEqual(r.returncode, 0, r.stderr)
            p = json.load(open(os.path.join(d, 'p.json'))); self.assertEqual((p['rate_hz'], len(p['windows'])), (122880000, 5)); self.assertFalse(p['radio'].get('wideband'))
            r = run(); self.assertEqual(r.returncode, 0, r.stderr); self.assertIn('needs --uhd-args', r.stderr); self.assertEqual(len(json.load(open(os.path.join(d, 'p.json')))['windows']), 5)
            r = run('--uhd-args', 'x', '--gain', '30', '--fallback-dwell'); self.assertNotEqual(r.returncode, 0); self.assertIn('share one LO', r.stderr)
            r = run('--uhd-args', 'x', '--gain', '30', '--fallback-dwell', '--channels', '2'); self.assertIn('spare channels (2)', r.stderr)
        with self.assertRaises(SystemExit): self.wb(245.76e6, fallback_dwell=True)

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
            c2 = dict(c, pass2=None); del c2['pass2']; p['centre_hz'] = 3000000000; p['windows'][0]['centre_hz'] = 3000000000   # v2: the cell's window centre decides
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

    # ---- shared channelizer (shm_iq_chan) in wideband plans
    def wbc(self, **kw):
        kw.setdefault('chan_cores', '20-22'); kw.setdefault('chan_backend', 'cpu')
        return self.wb(245.76e6, chan=True, **kw)

    def test_chan_plan_shape(self):
        p = self.wbc(chan_cells='/o/chan_cells.json'); r = p['radio']; cmd = r['chan_cmd'].split()
        self.assertEqual(cmd[:5], ['shm_iq_chan', '--ring', 'isac_iq', '--cells-json', '/o/chan_cells.json'])
        self.assertEqual(cmd[-4:], ['--threads', '3', '--cores', '20-22']); self.assertIn('real-time for about ONE cell', r['chan_warn'])
        cc = set(M.expand_cores('20-22')); pool = set(M.expand_cores('4-19'))
        self.assertFalse(cc & pool or cc & {0, 1}); self.assertEqual(r['chan_cores'], '20-22')
        for c in p['cells']:
            self.assertEqual((c['ring'], c['decim'], c['chan_decim'], c['nrx']), (f"isac_iq_{c['tag']}", 1, 2, 4))
            self.assertIn(f"ISAC_SHM_IQ_NAME=isac_iq_{c['tag']}", M.spec(c)); self.assertIn('UE_NB_ANT_RX=4', M.spec(c))
        self.assertEqual(p['windows'][0]['ring'], 'isac_iq')                      # the publisher's wideband ring is unchanged
        g = M.make_plan(cells(), '4-19', pub_cores='0-1', wideband=True, rate=245.76e6, uhd_args='x', gain=30, chan=True, chan_cores='20-21')['radio']
        self.assertIn('--gpu --cores 20-21', g['chan_cmd']); self.assertNotIn('chan_warn', g)          # default backend gpu
        with self.assertRaises(SystemExit): self.wb(245.76e6, chan=True)                              # no site default for the chan cores
        with self.assertRaises(SystemExit): self.wbc(chan_cores='1-2')                                # overlaps --pub-cores 0-1
        with self.assertRaises(SystemExit): self.wbc(chan_cores='19-20')                              # overlaps the cell cores
        self.assertNotIn('chan_cmd', self.wb(245.76e6)['radio'])

    def test_chan_cells_file_matches_real_parser(self):
        p = self.wbc(); b = os.path.join(H, '../../../cmake_targets/ran_build/build/shm_iq_chan')
        if not os.access(b, os.X_OK): self.skipTest('shm_iq_chan not built')
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, 'cc.json'); p['radio']['chan_cells'] = f; M.write_chan_cells(p)
            r = subprocess.run([b, '--bench', '0.3', '--nch', '4', '--in-rate', '245.76e6', '--cells-json', f, '--threads', '1'], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr); self.assertIn('cells=4 ch=4', r.stderr + r.stdout)
            for c in p['cells']: self.assertRegex(r.stderr, f"SHMIQ_CHAN_TUNE cell={c['tag']} .*cell_centre_hz={c['C_hz']}\\.000 .*decim=2")

    def test_chan_cli_default_and_no_chan(self):
        with tempfile.TemporaryDirectory() as d:
            json.dump(cells(), open(os.path.join(d, 'c.json'), 'w')); pj = os.path.join(d, 'p.json')
            base = [sys.executable, os.path.join(H, 'mpci_auto.py'), 'plan', os.path.join(d, 'c.json'), '--cores', '4-19', '--out', pj, '--uhd-args', 'x', '--gain', '30']
            r = subprocess.run(base, capture_output=True, text=True); self.assertNotEqual(r.returncode, 0); self.assertIn('--chan-cores', r.stderr)
            r = subprocess.run(base + ['--chan-cores', '20-21', '--chan-backend', 'cpu'], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stderr)
            p = json.load(open(pj)); self.assertIn('WARNING', r.stdout); self.assertEqual(p['radio']['chan_cells'], os.path.join(d, 'chan_cells.json'))
            self.assertEqual(json.load(open(os.path.join(d, 'chan_cells.json')))['cells'][0]['decim'], 2)
            r = subprocess.run(base + ['--no-chan'], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stderr)
            p = json.load(open(pj)); self.assertNotIn('chan_cmd', p['radio']); self.assertEqual({(c['ring'], c['decim']) for c in p['cells']}, {('isac_iq', 2)})

    def test_chan_launch_dry_order(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.wbc(chan_cells=os.path.join(d, 'cc.json')); pp = os.path.join(d, 'plan.json'); M.save(p, pp)
            r = subprocess.run([sys.executable, os.path.join(H, 'mpci_auto.py'), 'launch', pp, '--out', d], env=dict(os.environ, DRY='1', BUILD='/b'), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr); L = r.stdout.splitlines()
            kinds = [l.split()[0] for l in L if l.split()[0] in ('PUB', 'CHAN', 'WAIT', 'CELL')]
            self.assertEqual(kinds, ['PUB', 'CHAN', 'WAIT'] + ['CELL'] * 4)
            self.assertIn('shm_iq_chan --ring isac_iq --cells-json', next(l for l in L if l.startswith('CHAN ')))
            self.assertTrue(all(f"isac_iq_{c['tag']}" in next(l for l in L if l.startswith('WAIT ')) for c in p['cells']))
            self.assertFalse(os.path.exists(os.path.join(d, 'cc.json')))           # DRY writes nothing
            for c in p['cells']: self.assertIn(f"ISAC_SHM_IQ_NAME=isac_iq_{c['tag']}", next(l for l in L if l.startswith(f"CELL {c['tag']} ")))

    def test_chan_pass2_restarts_channelizer_with_new_cells(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.wbc(chan_cells=os.path.join(d, 'cc.json')); c = next(x for x in p['cells'] if x['pci'] == 508)
            os.makedirs(os.path.join(d, c['tag'])); open(os.path.join(d, c['tag'], 'ue.log'), 'w').write(SYNC + MISMATCH)
            calls = []; o1, o2 = M.restart_chan_and_cells, M.restart_cell
            M.restart_chan_and_cells = lambda pl, out: calls.append(json.dumps(M.chan_cells_doc(pl))) or True; M.restart_cell = lambda *a, **k: calls.append('cell')
            try: res = M.pass2_cell(p, d, c)
            finally: M.restart_chan_and_cells, M.restart_cell = o1, o2
            self.assertEqual((res, c['r'], c['decim'], c['chan_decim']), ('restarted', 162, 1, 4)); self.assertEqual(len(calls), 1)   # chan restart only; it restarts the cells
            e = next(x for x in json.loads(calls[0])['cells'] if x['tag'] == c['tag']); self.assertEqual((e['decim'], e['C_hz']), (4, c['C_hz']))

    def test_chan_watch_restart_and_cell_restart_leaves_chan(self):
        ring = f'mpcit{os.getpid()}'
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as d:
            b = os.path.join(d, 'bin'); os.mkdir(b); fake = os.path.join(b, 'nr-uesoftmodem'); fchan = os.path.join(b, 'shm_iq_chan')
            open(fake, 'w').write('#!/usr/bin/env python3\nimport signal,time,sys\nsignal.signal(signal.SIGINT,lambda *a:sys.exit(0))\ntime.sleep(60)\n')
            open(fchan, 'w').write('#!/usr/bin/env python3\nimport signal,sys,os\na=sys.argv; ring=a[a.index("--ring")+1]; doc=eval(open(a[a.index("--cells-json")+1]).read())\n'
                'fs=["/dev/shm/%s_%s"%(ring,c["tag"]) for c in doc["cells"]]\n[open(f,"w").close() for f in fs]\n'
                'def stop(*x):\n [os.unlink(f) for f in fs if os.path.exists(f)]; sys.exit(0)\nsignal.signal(signal.SIGINT,stop)\n'
                'sys.stderr.write("SHMIQ_CHAN_READY\\nSHMIQ_CHAN_OVERRUN skipped_in=1 new_pos=2\\n"); sys.stderr.flush()\nsignal.pause()\n')
            for x in (fake, fchan): os.chmod(x, 0o755)
            cs = [c for c in cells() if c['pci'] in (508, 4)]; out = os.path.join(d, 'o')
            p = M.make_plan(cs, '0-1', pub_cores=None, ring=ring, wideband=True, rate=245.76e6, uhd_args='x', gain=30, chan=True, chan_cores='2-3', chan_cells=os.path.join(d, 'cc.json'))
            p['build'] = b; p['radio']['pub_cmd'] = 'sleep 60'; p['radio']['chan_cmd'] = f"{fchan} --ring {ring} --cells-json {os.path.join(d, 'cc.json')}"
            rings = [f"/dev/shm/{c['ring']}" for c in p['cells']]
            try:
                M.launch(p, out); self.assertTrue(all(os.path.exists(r) for r in rings))              # cells started after the rings existed
                pids = lambda: {t: M.pid_of(out, t) for t in ['pub', 'chan'] + [c['tag'] for c in p['cells']]}
                p0 = pids(); self.assertTrue(all(M.alive(v) for v in p0.values()), p0)
                time.sleep(0.8)
                M.restart_cell(p, out, p['cells'][0])                                                  # single-cell restart: publisher and channelizer untouched
                p1 = pids(); self.assertEqual((p1['pub'], p1['chan']), (p0['pub'], p0['chan'])); self.assertTrue(M.alive(p1['chan']) and M.alive(p1['pub']))
                self.assertNotEqual(p1[p['cells'][0]['tag']], p0[p['cells'][0]['tag']]); self.assertEqual(p1[p['cells'][1]['tag']], p0[p['cells'][1]['tag']])
                time.sleep(0.8); rt = {}
                l = M.watch_step(p, out, rt, 1000.0); self.assertTrue(l[0].startswith('chan state=UP') and 'overrun=1' in l[0], l)
                os.kill(p1['chan'], 9); time.sleep(0.3)                                                # channelizer crash (SIGKILL by the test only)
                l = M.watch_step(p, out, rt, 1001.0); self.assertIn('RESTARTED', l[0], l); p2 = pids()
                self.assertNotEqual(p2['chan'], p1['chan']); self.assertTrue(M.alive(p2['chan'])); self.assertEqual(p2['pub'], p0['pub'])
                for c in p['cells']: self.assertNotEqual(p2[c['tag']], p1[c['tag']]); self.assertTrue(M.alive(p2[c['tag']]))   # orphaned cells re-attached
                self.assertTrue(os.path.exists(os.path.join(out, 'chan.log.' + next(x for x in os.listdir(out) if x.startswith('chan.log.')).split('.')[-1])))
            finally:
                subprocess.run(['bash', SH, 'stop'], env=dict(os.environ, OUT=out), capture_output=True); time.sleep(0.5)
                for r in rings:
                    if os.path.exists(r): os.unlink(r)

if __name__ == '__main__': unittest.main()
