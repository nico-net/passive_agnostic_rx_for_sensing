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

if __name__ == '__main__': unittest.main()
