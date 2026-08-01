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

# ---- CPI geometry ----
# HISTORY, because it explains why the shipped scene was undetectable (2026-07-30). These constants
# were measured with sources="csi_rs" ONLY, where the axis is deterministic: CSI-RS arrives on a
# strict 160-slot (80 ms) lattice, so vel_max was 0.500 m/s in EVERY CPI. A 0.15 m/s target is the
# only thing detectable in that regime, and that is what the confs encode.
#
# But the confs RUN a mixed source set (csi_rs + pdsch_dmrs_blind [+ pdsch_data]), where slow-time
# rows arrive with DL grants. Measured there: vel_max p10..p90 = 17.7..26.4 m/s, vel_res median
# 1.45 m/s -- so the zero-Doppler notch is ~4.3 m/s and the 0.15 m/s target sits deep inside it.
# Measured consequence: 1/54 CPIs detected the target. The scene was designed for one regime and
# deployed in another.
#
# The fix is not a different constant, it is a different QUESTION. With a traffic-driven slow-time
# lattice there is no single worst case -- "do all constraints pass?" is unanswerable and the honest
# metric is "in what FRACTION of real CPIs would this target be detectable?". Pass --from-reports
# with a real reports.jsonl and this script scores every sampled trajectory instant against every
# CPI's OWN measured geometry. The constants below remain the csi_rs-only fallback.
RANGE_RES = 3.05
VEL_MAX_WORST = 0.50      # measured, deterministic, csi_rs-only
VEL_RES_WORST = VEL_MAX_WORST / 16.0   # = vel_max / (cpi_slots/2), cpi_slots=32
ZERO_RANGE_GUARD = 3
ZERO_DOPPLER_GUARD = 3
FS = 122.88e6
CIR_MAX_M = 255 * C / FS  # SENS_MAX_TAPS

TX = (0.0, 0.0)
RX = {"rx1": (100.0, 0.0), "rx2": (-100.0, 0.0)}
# A linear array scans only broadside +/- 90 deg; these are the ULA confs' aoa_broadside_deg.
# (The 2x2 UPA confs scan the full circle, so this check is the strictest of the two.)
BROADSIDE = {"rx1": 122.0, "rx2": 30.0}

# Cover a long capture WITHOUT slowing the targets down. The previous scene stretched a single
# 180 m leg over 1200 s to "cover a 5 h wall-clock run", which silently set the speed to 0.15 m/s and
# put both targets in the zero-Doppler notch. Duration is bought with MORE PATROL LEGS at a fixed
# speed, never by making the leg longer in time.
DURATION_S = 2000.0
SPEED = 6.0               # m/s, world frame

# Patrol legs: each object bounces between two waypoints forever. A piecewise-linear reversal means
# the range rate flips sign instantly rather than dwelling near zero, so the notch is only crossed
# where the GEOMETRY makes dR/dt small -- which is exactly what check 3 verifies.
# Placed so |dR/dt| lands inside [7, 15] m/s -- above the mixed regime's ~4.3-4.9 m/s zero-Doppler
# notch and well under its p10 vel_max of 17.7 m/s. Both patrol in y at an x offset, which is what
# makes dR/dt large: for a target at (x, y) the bistatic rate is ~ y_dot * (y/|P| + y/|P-RX|), i.e.
# it grows with |y| and is near zero for motion perpendicular to that. They move in OPPOSITE
# directions so they separate in Doppler even where their ranges cross -- a real association test.
OBJECTS = [
    {"refl": 1.0, "a": (50.0, 60.0),  "b": (50.0, 250.0)},
    {"refl": 0.8, "a": (20.0, 260.0), "b": (20.0, 80.0)},
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
ap.add_argument("--from-reports", metavar="JSONL",
                help="score against the per-CPI geometry MEASURED in a real reports.jsonl instead of "
                     "the csi_rs-only constants -- required for any mixed source set")
ap.add_argument("--speed", type=float, default=SPEED, help=f"target speed m/s (default {SPEED})")
args = ap.parse_args()
SPEED = args.speed

CPIS = None
if args.from_reports:
    import json
    CPIS = []
    for line in open(args.from_reports):
        if not line.strip():
            continue
        r = json.loads(line)
        if "vel_res_mps" in r and "vel_max_mps" in r and "range_res_m" in r:
            CPIS.append((r["range_res_m"], r["vel_res_mps"], r["vel_max_mps"]))
    if not CPIS:
        raise SystemExit(f"no usable CPI geometry in {args.from_reports}")


def detectable_fraction(r, rate):
    """Fraction of MEASURED CPIs in which a target at differential range r with rate `rate` clears
    every notch. This replaces the binary pass/fail: with a traffic-driven slow-time lattice the
    geometry differs CPI to CPI, so detectability is a probability, not a property."""
    n = 0
    for rres, vres, vmax in CPIS:
        if (r > ZERO_RANGE_GUARD * rres and r < CIR_MAX_M
                and abs(rate) > ZERO_DOPPLER_GUARD * vres and abs(rate) < vmax):
            n += 1
    return n / len(CPIS)

if CPIS:
    import statistics as _st
    _vr = sorted(c[1] for c in CPIS); _vm = sorted(c[2] for c in CPIS)
    print(f"CPI geometry MEASURED over {len(CPIS)} real CPIs ({args.from_reports}):")
    print(f"  vel_res  median {_st.median(_vr):.3f} m/s  -> zero-Doppler notch median "
          f"{ZERO_DOPPLER_GUARD*_st.median(_vr):.2f} m/s")
    print(f"  vel_max  p10 {_vm[len(_vm)//10]:.2f}  median {_st.median(_vm):.2f} m/s")
    print("  scoring = fraction of those CPIs in which the target clears every notch\n")
print(f"CPI geometry fallback (csi_rs-only worst case): range_res={RANGE_RES} m, "
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
        c5 = max(rel) < 90.0
        if CPIS:
            # Score every sampled instant against every measured CPI. `worst` is the least
            # detectable moment of the patrol -- the number that decides whether a track survives.
            fr = [detectable_fraction(r, rt) for r, rt in zip(rs[1:], rates)]
            worst, mean = min(fr), sum(fr) / len(fr)
            ok = c1 and c2 and c5 and worst >= 0.5
            ok_all &= ok
            print(f"   {name}: dR {min(rs):6.1f}..{max(rs):6.1f} m [{'ok' if c1 and c2 else 'FAIL'}]   "
                  f"|dR/dt| {amin:5.2f}..{amax:5.2f} m/s   "
                  f"detectable in {100*mean:.0f}% of CPIs on average, {100*worst:.0f}% at the worst "
                  f"instant [{'ok' if worst >= 0.5 else 'FAIL'}]   "
                  f"bearing {bmin:6.1f}..{bmax:6.1f} deg, max {max(rel):.0f} deg off broadside "
                  f"[{'ok' if c5 else 'FAIL'}]")
        else:
            c3 = amin > ZERO_DOPPLER_GUARD * VEL_RES_WORST
            c4 = amax < VEL_MAX_WORST
            ok = all((c1, c2, c3, c4, c5))
            ok_all &= ok
            print(f"   {name}: dR {min(rs):6.1f}..{max(rs):6.1f} m [{'ok' if c1 and c2 else 'FAIL'}]   "
                  f"|dR/dt| {amin:5.2f}..{amax:5.2f} m/s [{'ok' if c3 and c4 else 'FAIL'}]   "
                  f"bearing {bmin:6.1f}..{bmax:6.1f} deg, max {max(rel):.0f} deg off broadside "
                  f"[{'ok' if c5 else 'FAIL'}]")
print("\n" + ("ALL CONSTRAINTS PASS" if ok_all else "*** SOME CONSTRAINTS FAIL ***"))

if args.emit:
    print("\nobjects = \"" + " | ".join(specs) + "\";")
