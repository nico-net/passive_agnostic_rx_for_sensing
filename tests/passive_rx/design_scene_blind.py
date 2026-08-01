#!/usr/bin/env python3
"""Design + VALIDATE a sensing_channel scene against the measured CPI geometry, then emit the
`objects = "..."` string for the .conf files.

WHY THIS EXISTS: the previous passive_rx scene had a target moving at 0.1 m/s. Its bistatic range
rate (~0.17 m/s) fell INSIDE the zero-Doppler clutter notch (zero_doppler_guard * vel_res) in 70% of
CPIs, so it was undetectable by construction -- no amount of fusion tuning could recover it, and the
poor full-passive numbers were partly measuring that. A target's detectability is a property of the
scene AND the achieved CPI geometry together, so it has to be checked, not assumed.

Constraints checked per receiver, at every sampled instant of the trajectory:
  1. dR > zero_range_guard * range_res          -- else buried in the direct-path notch
  2. dR < CIR span (SENS_MAX_TAPS * c/fs)        -- else the tap falls off the end of the channel
  3. |dR/dt| > zero_doppler_guard * vel_res      -- else buried in the clutter (zero-Doppler) notch
  4. |dR/dt| < vel_max                           -- else it aliases in Doppler
  5. bearing inside the estimator's scan window  -- a linear array only scans broadside +/- 90 deg

Usage: design_scene.py [--emit]
"""
import argparse, math

C = 299792458.0

# ---- measured CPI geometry: 100 MHz cell, sources="csi_rs" ONLY ----
# Using a SINGLE fixed-period source is what makes this axis deterministic. Measured over /tmp/val4:
# vel_max = 0.500 m/s in EVERY CPI (0th..100th percentile identical), because CSI-RS arrives on a
# strict 160-slot (80 ms) lattice, so the slow-time row spacing cannot drift.
# Contrast the mixed csi_rs+pdsch_dmrs_blind config, where vel_max swung 0.7..27 m/s CPI-to-CPI:
# blind-PDCCH rows arrive with DL grants, so how many land in a CPI depends on traffic AND on how
# starved the receiver is -- there is no single target speed detectable across both regimes.
RANGE_RES = 3.05
VEL_MAX_WORST = 13.80     # measured worst case, sources="pdsch_dmrs_blind" (val2: 13.8-27.2)
VEL_RES_WORST = 1.70      # measured worst case (widest zero-Doppler notch)
ZERO_RANGE_GUARD = 3
ZERO_DOPPLER_GUARD = 3
FS = 122.88e6
CIR_MAX_M = 255 * C / FS  # SENS_MAX_TAPS

TX = (0.0, 0.0)
RX = {"rx1": (100.0, 0.0), "rx2": (-100.0, 0.0)}
# A linear array scans only broadside +/- 90 deg; these are the ULA confs' aoa_broadside_deg.
# (The 2x2 UPA confs scan the full circle, so this check is the strictest of the two.)
BROADSIDE = {"rx1": 122.0, "rx2": 30.0}

DURATION_S = 2000.0       # cover a long run: ~6.7% of real time -> 1200 s sim ~ 5 h wall
SPEED = 6.0               # m/s, world frame

# Patrol legs: each object bounces between two waypoints forever. A piecewise-linear reversal means
# the range rate flips sign instantly rather than dwelling near zero, so the notch is only crossed
# where the GEOMETRY makes dR/dt small -- which is exactly what check 3 verifies.
OBJECTS = [
    {"refl": 1.0, "a": (0.0, 50.0),   "b": (0.0, 140.0)},
    {"refl": 0.8, "a": (50.0, 160.0), "b": (50.0, 70.0)},
]


def leg_waypoints(a, b, speed, duration):
    """Zig-zag between a and b at `speed` until `duration`, as (t, x, y)."""
    d = math.dist(a, b)
    leg = d / speed
    wps, t, cur, nxt = [], 0.0, a, b
    wps.append((0.0, a[0], a[1]))
    while t < duration:
        t += leg
        wps.append((round(t, 2), nxt[0], nxt[1]))
        cur, nxt = nxt, cur
    return wps, leg


def dR(p, rx):
    """Bistatic differential range: (tx->target->rx) minus the direct tx->rx path."""
    return math.dist(TX, p) + math.dist(p, rx) - math.dist(TX, rx)


def bearing_deg(p, rx):
    return math.degrees(math.atan2(p[1] - rx[1], p[0] - rx[0]))


def wrap180(a):
    return (a + 180.0) % 360.0 - 180.0


def pos_at(wps, t):
    if t <= wps[0][0]:
        return (wps[0][1], wps[0][2])
    for (t0, x0, y0), (t1, x1, y1) in zip(wps, wps[1:]):
        if t0 <= t <= t1:
            f = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
            return (x0 + f * (x1 - x0), y0 + f * (y1 - y0))
    return (wps[-1][1], wps[-1][2])


ap = argparse.ArgumentParser()
ap.add_argument("--emit", action="store_true")
args = ap.parse_args()

print(f"CPI geometry used (worst case of measured): range_res={RANGE_RES} m, "
      f"vel_res={VEL_RES_WORST} m/s, vel_max={VEL_MAX_WORST} m/s")
print(f"  zero-Doppler notch  : |dR/dt| must exceed {ZERO_DOPPLER_GUARD*VEL_RES_WORST:.2f} m/s")
print(f"  zero-range notch    : dR must exceed {ZERO_RANGE_GUARD*RANGE_RES:.2f} m")
print(f"  CIR span (255 taps) : dR must stay under {CIR_MAX_M:.0f} m\n")

specs = []
ok_all = True
for oi, o in enumerate(OBJECTS):
    wps, leg = leg_waypoints(o["a"], o["b"], SPEED, DURATION_S)
    specs.append(f"{o['refl']}; " + "; ".join(f"{t},{x},{y}" for t, x, y in wps))
    print(f"obj{oi}: {o['a']} <-> {o['b']} at {SPEED} m/s, leg {leg:.1f}s, {len(wps)} waypoints")
    # Sample one full leg densely (the pattern repeats, so one leg covers every geometry reached).
    dt = 0.05
    for name, rx in RX.items():
        rs, rates, bears = [], [], []
        prev = None
        t = 0.0
        while t <= leg:
            p = pos_at(wps, t)
            r = dR(p, rx)
            rs.append(r)
            bears.append(bearing_deg(p, rx))
            if prev is not None:
                rates.append((r - prev) / dt)
            prev = r
            t += dt
        amin, amax = min(abs(v) for v in rates), max(abs(v) for v in rates)
        bmin, bmax = min(bears), max(bears)
        # relative to this receiver's broadside (linear-array scan window is +/-90 deg)
        rel = [abs(wrap180(b - BROADSIDE[name])) for b in bears]
        c1 = min(rs) > ZERO_RANGE_GUARD * RANGE_RES
        c2 = max(rs) < CIR_MAX_M
        c3 = amin > ZERO_DOPPLER_GUARD * VEL_RES_WORST
        c4 = amax < VEL_MAX_WORST
        c5 = max(rel) < 90.0
        ok = all((c1, c2, c3, c4, c5))
        ok_all &= ok
        print(f"   {name}: dR {min(rs):6.1f}..{max(rs):6.1f} m [{'ok' if c1 and c2 else 'FAIL'}]   "
              f"|dR/dt| {amin:5.2f}..{amax:5.2f} m/s [{'ok' if c3 and c4 else 'FAIL'}]   "
              f"bearing {bmin:6.1f}..{bmax:6.1f} deg, max {max(rel):.0f} deg off broadside "
              f"[{'ok' if c5 else 'FAIL'}]")
print("\n" + ("ALL CONSTRAINTS PASS" if ok_all else "*** SOME CONSTRAINTS FAIL ***"))

if args.emit:
    print("\nobjects = \"" + " | ".join(specs) + "\";")
