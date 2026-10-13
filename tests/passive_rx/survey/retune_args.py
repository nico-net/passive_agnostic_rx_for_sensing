#!/usr/bin/env python3
"""retune_args.py RUN.LOG [N_RB_GRID] : from a run that logged 'ACQ carrier MISMATCH' (SIB1 carrier != started grid), print the
-C / -r / --ssb that put the SIB1 carrier fully inside an RB-aligned grid, centred on the carrier. General (SIB1 + KSSB_GRID lines only).
Why: 508 (WindTre): carrier 162 PRB, Point A at grid sc 972 of a 217-PRB grid => carrier end RB 243 > 217, top 26 RB not received.
N_RB_GRID may be 'auto': smallest receiver-supported 30 kHz grid >= the SIB1 carrier (51/106/133/162/189/217/273; the grid then equals the
carrier when SIB1 gives one of those). Exit 0 and one line 'C=.. R=.. SSB=..' ; exit 3 'no mismatch'; exit 4 'carrier wider than grid'."""
import re, sys
t = re.sub(r'\x1b\[[0-9;]*m', '', open(sys.argv[1], errors='replace').read())
m = re.search(r'SIB1 says (\d+) PRB mu=(\d) \(Point A at grid subcarrier (-?\d+).*?PHY started with (\d+) PRB.*?receiver started at ([\d.]+) MHz', t)
if not m: print('no mismatch'); sys.exit(3)
cr, mu, pa, nr, c0 = int(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5]) * 1e6
scs = 15e3 * 2 ** mu
k = re.search(r'KSSB_GRID k_ssb=(\d+) ssb_sc=(\d+).*?aligned: --ssb (\d+) -C (\d+)', t)
# SSB first subcarrier relative to Point A, on the CRB grid (aligned if KSSB_GRID reported an offset)
if k is None:
    print('no KSSB_GRID/ssb line'); sys.exit(5)
ssb_from_pa = int(k[3]) - pa
SUPPORTED = (51, 106, 133, 162, 189, 217, 273)  # get_samplerate_and_bw(mu=1) in common/utils/nr/nr_common.c
if len(sys.argv) > 2 and sys.argv[2] == 'auto':
    cands = [r for r in SUPPORTED if r >= cr]
    if not cands: print('carrier wider than 273'); sys.exit(4)
    grid_rb = cands[0]
else:
    grid_rb = int(sys.argv[2]) if len(sys.argv) > 2 else nr
if cr > grid_rb: print('carrier wider than grid: use -r 273'); sys.exit(4)
mg = (grid_rb - cr) // 2                              # whole RBs below Point A keeps the RB grid aligned
pa_hz = c0 - nr * 6 * scs + pa * scs
c = pa_hz - mg * 12 * scs + grid_rb * 6 * scs
print(f'C={c:.0f} R={grid_rb} SSB={mg * 12 + ssb_from_pa}  (carrier {cr} PRB, Point A {pa_hz / 1e6:.3f} MHz, grid {(c - grid_rb * 6 * scs) / 1e6:.3f}..{(c + grid_rb * 6 * scs) / 1e6:.3f} MHz)')
