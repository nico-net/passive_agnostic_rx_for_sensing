#!/usr/bin/env python3
"""Effective-config check (stage-2 cleanup). usage: effective_config_check.py <FROZEN.env> <DEFERRED.md> <build dir>
Prints, for every name of the frozen env: KEPT (still read from the environment, value = the env, else the code default) or
BAKED (no longer read; the no-env binary uses the frozen value as a constant). Then greps the binaries/libs for every removed
name (DEFERRED list + baked frozen names): any hit means a getenv for it survived."""
import re, subprocess, sys, glob, os
frozen_env, deferred, bd = sys.argv[1:4]
frozen = dict(kv.split('=', 1) for kv in open(frozen_env).read().split())
dfl = set(re.findall(r'^\| (\S+) \|', open(deferred).read(), re.M)) - {'flag'}
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
src = subprocess.run(['git', '-C', root, 'grep', '-ohE', r'"[A-Z][A-Z0-9_]+"', '--', '*.c', '*.cpp', '*.cu', '*.h', '*.cuh', ':!*/tests/*', ':!openair1/SIMULATION'],
                     capture_output=True, text=True).stdout.split()
live = {s.strip('"') for s in src}
kept = sorted(n for n in frozen if n in live)
baked = sorted(n for n in frozen if n not in live)
print(f'frozen names: {len(frozen)}; kept as env read (site/hardware): {len(kept)}; baked constants: {len(baked)}')
for n in kept: print(f'KEPT  {n}={frozen[n]}  (env, code default if unset)')
for n in baked: print(f'BAKED {n}={frozen[n]}')
removed = sorted((dfl | set(baked)) - live)
bins = [f for f in [bd + '/nr-uesoftmodem'] + glob.glob(bd + '/*.so') if os.path.isfile(f)]
blob = b''.join(subprocess.run(['strings', '-a', f], capture_output=True).stdout for f in bins).decode(errors='replace')
words = set(re.findall(r'[A-Z][A-Z0-9_]{3,}', blob))
LOGTAGS = {'ISAC_METRICS', 'ISAC_SEGV_BT', 'OAI_RNGSEED'}  # log-line tags / simulator seed, not env reads
hits = sorted(n for n in removed if n in words and n not in LOGTAGS)
print(f'removed names checked: {len(removed)} in {len(bins)} binaries; surviving string hits: {len(hits)}')
for n in hits: print('HIT', n)
sys.exit(1 if hits else 0)
