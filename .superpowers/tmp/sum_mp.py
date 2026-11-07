#!/usr/bin/env python3
"""sum_mp.py <campaign dir>: markdown tables per (mode, cell) from <run>/mp.json (m_mp.py): solo arm vs pair arms, one column per arm. Never pools cells."""
import sys, json, re, collections
C = sys.argv[1]; rows = []
for ln in open(C + '/runs.tsv'):
    t, r, want, ok, mode, wall = ln.rstrip('\n').split('\t')
    try: m = json.load(open(r + '/mp.json'))
    except Exception: continue
    rows.append(dict(tag=t, run=r, want=want, ok=ok, mode=mode, m=m))
f1 = lambda v, d=1: '-' if v is None else ('%.' + str(d) + 'f') % v
ms = lambda us: '-' if us is None else '%.2f' % (us / 1000.)
def D(o, k): return (o['drops'].get(k) or 0)
def pc(a, b): return '-' if not b else '%.1f' % (100. * a / b)
def table(arms, pci):
    hdr = '| metric | ' + ' | '.join(a['tag'] + ('' if a['ok'] == 'OK' else ' (' + a['ok'] + ')') for a in arms) + ' |\n|---|' + '---|' * len(arms)
    print(hdr)
    def cellof(a):
        for c, o in a['m']['cells'].items():
            if str(o.get('pci')) == str(pci): return o
        return None
    def row(name, fn):
        out = []
        for a in arms:
            o = cellof(a)
            try: out.append(fn(o, a) if o else 'n/a')
            except Exception as e: out.append('err')
        print('| %s | %s |' % (name, ' | '.join(out)))
    row('active window s', lambda o, a: f1(o['dur_act'], 0)); row('t PBCH lock s', lambda o, a: f1(o['t_pbch_s'])); row('t SIB1 s', lambda o, a: f1(o['t_sib1_s'])); row('t first record s', lambda o, a: f1(o['t_first_rec_s']))
    row('sync losses / pbch locks', lambda o, a: '%s/%s' % (o['sync_losses'], o['pbch_locks']))
    row('DCI accepts/s (1_0 / 1_1 / 0_0 / 0_1)', lambda o, a: '%s (%s / %s / %s / %s)' % tuple(f1(o['dci_s'][k], 1) for k in ('accepts', 'd10', 'd11', 'd00', 'd01')))
    row('distinct RNTI (top)', lambda o, a: '%d (%s)' % (o['rntis_n'], ' '.join('%s:%d' % tuple(x) for x in o['rntis_top'][:3])))
    row('grants decoded/s', lambda o, a: f1(o['grants_s'])); row('CRC-ok % (all)', lambda o, a: f1(o['crc_pct']))
    row('dec>=51 /s (crc-ok/s)', lambda o, a: '%s (%s)' % (f1(o['dec51_s']), f1(o['crc51_s']))); row('dec>=128 /s (crc-ok/s)', lambda o, a: '%s (%s)' % (f1(o['dec128_s']), f1(o['crc128_s'])))
    row('dec_us p50/p99 ms all', lambda o, a: '%s/%s (n=%d)' % (ms(o['lat_us_0'][0]), ms(o['lat_us_0'][1]), o['lat_us_0'][2])); row('dec_us p50/p99 ms >=128', lambda o, a: '%s/%s (n=%d)' % (ms(o['lat_us_128'][0]), ms(o['lat_us_128'][1]), o['lat_us_128'][2]))
    row('scanq full/stale/epoch % of queued', lambda o, a: '%s/%s/%s (q=%d)' % (pc(D(o, 'scanq_drop_full'), D(o, 'scanq_queued')), pc(D(o, 'scanq_drop_stale'), D(o, 'scanq_queued')), pc(D(o, 'scanq_drop_epoch'), D(o, 'scanq_queued')), D(o, 'scanq_queued')))
    row('pdschq full/stale % of queued', lambda o, a: '%s/%s (q=%d)' % (pc(D(o, 'pdschq_drop_full'), D(o, 'pdschq_queued')), pc(D(o, 'pdschq_drop_stale'), D(o, 'pdschq_queued')), D(o, 'pdschq_queued')))
    row('DA async drop % data / dmrs', lambda o, a: '%s / %s' % (pc(D(o, 'sense_async_drop_pdsch_data'), D(o, 'sense_async_queued_pdsch_data')), pc(D(o, 'sense_async_drop_pdsch_dmrs'), D(o, 'sense_async_queued_pdsch_dmrs'))))
    row('cfr_drop / rec_drop / obs_dropped', lambda o, a: '%d / %d / %d' % (D(o, 'sense_cfr_drop'), D(o, 'sense_rec_drop'), D(o, 'obs_dropped')))
    row('records total / cfr exported dmrs+data', lambda o, a: '%d / %d' % (D(o, 'sense_rec_count'), D(o, 'sense_cfr_exported_pdsch_dmrs') + D(o, 'sense_cfr_exported_pdsch_data')))
    if any(a['mode'] == 'S' for a in arms):
        for src in ('pdsch_dmrs', 'pdsch_data', 'ssb', 'csirs'):
            row('rec/s %s n | v51 | v128' % src, lambda o, a, s=src: ('%s | %s | %s (noqual %d)' % (f1(o['rec'][s]['n_s']), f1(o['rec'][s]['v51_s']), f1(o['rec'][s]['v128_s']), o['rec'][s]['noqual'])) if s in o['rec'] else '0')
        row('SENSE_SCHED lines (cell)', lambda o, a: str(o['sched_n']))
def proc(arms):
    print('| arm | exit | cpu cores | GPU util mean/p95 | chan overruns / discont | chan_work_max / gap_max ms | SHMIQ ov (per ring) | UHD ovf / RFSTALL / RXDISCONT | asserts | SENSE_SCHED fail/started | pthread fail |\n|---|---|---|---|---|---|---|---|---|---|---|')
    for a in arms:
        g = a['m']['glob']; fe = g.get('feed', {})
        so = ' '.join(re.findall(r'cell=(\S+) .*?overruns=(\d+)', ' '.join(g['shmiq_end'])) and ['%s:%s' % x for x in re.findall(r'cell=(\S+) reads=\d+ overruns=(\d+)', ' '.join(g['shmiq_end']))])
        print('| %s | %s | %s | %s/%s | %s / %s | %s / %s | %s | %d / %d / %d | %d | %d/%d | %d |' % (a['tag'], g['exit'], g['cpu_cores'], f1(g.get('gpu_util_mean'), 0), f1(g.get('gpu_util_p95'), 0), fe.get('chan_overruns', '-'), fe.get('chan_discont', '-'), fe.get('chan_work_max_ms', '-'), fe.get('chan_gap_max_ms', '-'), so or '-', g['uhd_ovf'], g['rfstall'], g['rxdiscont'], g['asserts'], g['sched_fail'], g['sched_started'], g['pthread_fail']))
for mode in ('T', 'S'):
    A = [r for r in rows if r['mode'] == mode and r['tag'].startswith('mp2')]
    if not A: continue
    print('\n### 200 MHz (245.76 MS/s), mode', mode, '\n'); proc(A)
    for pci in (415, 923):
        arms = [r for r in A if r['want'] == str(pci)] + [r for r in A if r['want'] == '415 923']
        print('\n#### cell', pci, '\n'); table(arms, pci)
S1 = [r for r in rows if r['tag'].startswith('m4')]
for a in S1:
    print('\n### step 1 attempt', a['tag'], a['run'], '\n'); g = a['m']['glob']; print('span:', g['span']); print('unserved:', g['span_unserved']); print('admit:', g['admit']); proc([a])
    for c, o in sorted(a['m']['cells'].items()): print('cell', c, 'pci', o.get('pci'), 'state', o.get('acq_state'), 't_pbch', o.get('t_pbch_s'), 'dec/s', o.get('dec0_s'))
