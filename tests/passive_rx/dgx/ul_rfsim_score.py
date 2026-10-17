#!/usr/bin/env python3
"""Score one ul_rfsim_arm.sh run: passive UL CRC (UL_PUSCH_CRC, real vs probe) against the gNB's own UL CRC (ground truth).
usage: ul_rfsim_score.py <arm_dir>   -> one JSON line.  gNB CRC rate = 1 - rounds[1]/rounds[0] from the last
'UE 1234: ulsch_rounds r0/r1/r2/r3' line (HARQ round-1 failures = first-transmission CRC fails); gNB DTX reported separately."""
import json, re, sys, collections
d = sys.argv[1]
ansi = re.compile(r'\x1b\[[0-9;]*m')
rx = [ansi.sub('', l) for l in open(f'{d}/rx/rx.log', errors='replace')]
crc = [l for l in rx if 'UL_PUSCH_CRC' in l]
def f(l, k, cast=str):
    m = re.search(r'\b' + k + r'=(\S+)', l); return cast(m.group(1)) if m else None
rows = [dict(ok=f(l, 'ok', int), probe=f(l, 'probe', int) or 0, mcs=f(l, 'mcs', int), tbl=f(l, 'tbl', int), dmrs=f(l, 'dmrs_id', int),
             data=f(l, 'data_id', int), prb=f(l, 'prb'), snr=f(l, 'snr_db', float), t=f(l, 't', int)) for l in crc]
real = [r for r in rows if not r['probe']]
out = dict(n_crc_lines=len(rows), n_real=len(real), real_ok=sum(r['ok'] == 1 for r in real), n_probe=len(rows) - len(real),
           probe_ok=sum(r['ok'] == 1 for r in rows if r['probe']))
out['real_crc_pct'] = round(100 * out['real_ok'] / len(real), 2) if real else None
for k in ('dmrs', 'data', 'mcs', 'tbl', 'prb'):
    out[k + '_hist_real'] = dict(collections.Counter(str(r[k]) for r in real).most_common(4))
snr = sorted(r['snr'] for r in real if r['snr'] is not None)
out['snr_med'] = snr[len(snr) // 2] if snr else None
st = [ansi.sub('', l) for l in open(f'{d}/gnb/gnb.log', errors='replace') if 'ulsch_rounds' in l]
if st:
    m = re.search(r'ulsch_rounds (\d+)/(\d+)/(\d+)/(\d+), ulsch_errors (\d+), ulsch_DTX (\d+)', st[-1])
    r0, r1 = int(m.group(1)), int(m.group(2))
    out['gnb'] = dict(rounds=[int(m.group(i)) for i in range(1, 5)], errors=int(m.group(5)), dtx=int(m.group(6)),
                      crc_pct=round(100 * (1 - r1 / r0), 2) if r0 else None, mcs=re.search(r'MCS \((\d)\) (\d+)', st[-1]).groups())
m = [l for l in rx if 'pusch_passive[' in l]
if m: out['pusch_passive_last'] = re.search(r'pusch_passive\[[^\]]*\]', m[-1]).group(0)
# lock events with receiver time (s since rx start; the log prefix) and passive CRC after the last lock event
ev = [(float(l.split()[0]), re.sub(r'^\S+ .*?SENSING: ', '', l.strip())[:230]) for l in rx if re.search(r'ULLAYOUT(_CRC_LOCK| BWP_LOCK| SEED_LOCK| LOCK )', l)]
out['lock_events'] = [dict(t=e[0], ev=e[1]) for e in ev if 'ULLAYOUT LOCK' not in e[1]][:6]
tl = max([e[0] for e in ev], default=None)
tt = [(float(l.split()[0]), f(l, 'ok', int)) for l in crc if not (f(l, 'probe', int) or 0)]
post = [ok for t, ok in tt if tl is not None and t >= tl]
out['t_last_lock_s'] = tl
out['post_lock'] = dict(n_real=len(post), crc_pct=round(100 * sum(1 for o in post if o == 1) / len(post), 2) if post else None)
print(json.dumps(out))
