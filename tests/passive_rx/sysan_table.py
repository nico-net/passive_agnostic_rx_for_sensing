#!/usr/bin/env python3
"""SYSAN: one row per replay dir (glob), e2e + p99_stats essentials. usage: sysan_table.py 'replays/replay_m1_*'"""
import glob, json, os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import sysan_e2e
print('%-22s %5s %5s | %-17s | %-17s | %-17s | %-17s | %5s %5s | %-17s | %s' % ('run', 'n', 'crcok', 'arr2enq p50/p99', 'first p50/p99', 'dec p50/p99', 'dec_ok p50/p99', 'bod', 'rty%', 'UL wall p50/p99', 'scan_e2e'))
for d in sorted(glob.glob(sys.argv[1]), key=lambda x: re.sub(r'_\d{6}$', '', x)):
    f = d + '/run.log'
    if not os.path.exists(f):
        continue
    o = sysan_e2e.run(f)
    if not o['dl_n']:
        continue
    g = lambda k: '%s/%s' % (o[k]['p50'], o[k]['p99'])
    sc = ''
    for l in open(f, errors='replace'):
        if 'SCAN_E2E' in l:
            sc = re.sub(r'.*SCAN_E2E ', '', l.strip())
    print('%-22s %5d %5d | %-17s | %-17s | %-17s | %-17s | %5s %5s | %-17s | %s' % (
        os.path.basename(d)[7:-7], o['dl_n'], o['dl_crc_ok'], g('dl_arr2enq'), g('dl_first'), g('dl_dec'), g('dl_dec_crc_ok'),
        o['dl_bodies_mean'], o['dl_retry_time_pct'], g('ul_wall'), sc[:70]))
