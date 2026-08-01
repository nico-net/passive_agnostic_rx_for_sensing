#!/usr/bin/env python3
"""Generate a HIGH-RADIAL-ACCELERATION scene, to try to produce ORPHANED Doppler harmonics.

Why this exists (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md 7.7 item 2): Phase B's kinematic gate only
catches a harmonic whose FUNDAMENTAL was not detected in the same CPI. Every scene tried so far
produces ZERO of those. Halving target reflectivity did NOT work -- measured 2026-07-30, obj3 coverage
fell 74 % -> 59 % but orphaned stayed 0 -- because SNR suppression scales the fundamental and its
harmonic together, so they vanish as a pair.

Radial ACCELERATION attacks it differently, and this is the whole idea being tested: if the target's
bistatic range-rate changes by several Doppler bins WITHIN one CPI, the fundamental's energy smears
across those bins and its peak drops, potentially below CFAR -- while a harmonic replica can remain
concentrated. That decouples the two in a way an SNR change cannot.

**This is a hypothesis, not a known result.** It is Agent-1's proposed mechanism and it has not been
demonstrated on this pipeline. The census (score_ghost_census.py) is the test; if orphaned stays 0,
the hypothesis is wrong on this harness and should be recorded as such rather than retried blindly.

## Sizing (all numbers from the measured capture, not guessed)

Measured on /tmp/ghostkin/weak_trial (cpi_slots=128, 273 PRB): vel_res ~ 1.39 m/s, CPI ~ 64 ms,
vel_max ~ 87 m/s, zero_doppler_guard=3 (a notch of roughly +/-4.2 m/s).

To smear the fundamental across ~3 Doppler bins inside one CPI:
    |d(range_rate)/dt| * T_cpi  >  3 * vel_res
    |d(range_rate)/dt|          >  3 * 1.39 / 0.064  ~=  65 m/s^2

A constant 65 m/s^2 is not usable (it would reach ~1300 m/s over a 20 s scene), so the acceleration is
made OSCILLATORY: position amplitude stays small while acceleration stays large, since for
r(t) = A*sin(w t) the velocity amplitude is A*w but the acceleration amplitude is A*w^2. A 1 Hz
oscillation of under a metre delivers the target acceleration at a very modest speed.

Geometry: TX (0,0), RX (100,0). The target sits on the perpendicular bisector (x = 50) and moves in
y, where the bistatic range R = 2*sqrt(50^2 + y^2) gives dR/dy ~ 1.9-2.0 -- so bistatic range-rate is
about twice the y-velocity, and bistatic acceleration about twice the y-acceleration.

    y(t) = Y0 + DRIFT*t + A*sin(2*pi*F*t)

- DRIFT keeps the bulk Doppler clear of the zero-Doppler notch at all times (y-velocity never dips
  near 0, so the target is never notched out -- which would confound "smeared below CFAR" with
  "clutter-notched").
- A is solved from the acceleration target above.
- The dR range is kept under the simulator's ~622 m CIR tap cap (CLAUDE.md 12); exceeding it silently
  drops the target off the end of the channel.

Object 1 is a plain CONSTANT-VELOCITY control in the same scene: if orphaned harmonics appear only
around the accelerating target, the mechanism is confirmed; if they appear around both, something
else is responsible.
"""
import math

TX = (0.0, 0.0)
RX = (100.0, 0.0)
DUR_S = 20.0
DT = 0.05          # waypoint step; 20 samples per cycle at 1 Hz (piecewise-linear interpolation)
REFL = 0.3         # < LOS gain 1.0, so the reflector cannot hijack the UE's own time sync

# --- accelerating target ---------------------------------------------------------------------
X_ACC = 50.0       # on the perpendicular bisector: bistatic range-rate ~= 2 x y-velocity
Y0 = 150.0
DRIFT = 8.0        # m/s in y -> ~15 m/s bistatic, comfortably outside the ~4.2 m/s notch
F_HZ = 1.0
TARGET_ACC = 65.0  # bistatic m/s^2, from the smear derivation above


def dR_dy(x, y):
    """d(bistatic range)/dy for a target at (x, y)."""
    rt = math.hypot(x - TX[0], y - TX[1])
    rr = math.hypot(x - RX[0], y - RX[1])
    return (y - TX[1]) / rt + (y - RX[1]) / rr


def bistatic_dr(x, y):
    return math.hypot(x - TX[0], y - TX[1]) + math.hypot(x - RX[0], y - RX[1]) - math.hypot(
        RX[0] - TX[0], RX[1] - TX[1]
    )


def main():
    w = 2.0 * math.pi * F_HZ
    # Solve the sine amplitude from the desired BISTATIC acceleration, via the local dR/dy gain.
    gain = dR_dy(X_ACC, Y0 + DRIFT * DUR_S / 2.0)  # mid-run gain, ~1.95
    amp = TARGET_ACC / (gain * w * w)

    acc_wp, ctl_wp = [], []
    n = int(DUR_S / DT) + 1
    ymin = ymax = None
    vmin, vmax = 1e9, -1e9
    for i in range(n):
        t = i * DT
        y = Y0 + DRIFT * t + amp * math.sin(w * t)
        acc_wp.append(f"{t:.2f},{X_ACC:.1f},{y:.2f}")
        # diagnostics
        vy = DRIFT + amp * w * math.cos(w * t)
        rate = dR_dy(X_ACC, y) * vy
        vmin, vmax = min(vmin, rate), max(vmax, rate)
        dr = bistatic_dr(X_ACC, y)
        ymin = dr if ymin is None else min(ymin, dr)
        ymax = dr if ymax is None else max(ymax, dr)
        # constant-velocity control, well separated in range from the accelerating one
        ctl_wp.append(f"{t:.2f},{-140.0 - 6.0 * t:.2f},{-90.0 - 3.0 * t:.2f}")

    objects = f"{REFL}; " + "; ".join(acc_wp) + f" | {REFL}; " + "; ".join(ctl_wp)
    print(f"# accelerating target: amp={amp:.3f} m @ {F_HZ} Hz, drift={DRIFT} m/s")
    print(f"# bistatic range-rate spans {vmin:+.1f} .. {vmax:+.1f} m/s "
          f"(zero-Doppler notch ~ +/-4.2, vel_max ~ 87)")
    print(f"# bistatic dR spans {ymin:.0f} .. {ymax:.0f} m (CIR tap cap ~ 622)")
    print(f"# peak bistatic acceleration ~ {amp * gain * w * w:.0f} m/s^2 "
          f"-> ~{amp * gain * w * w * 0.064 / 1.39:.1f} Doppler bins of smear per 64 ms CPI")
    print()
    print(f'  objects = "{objects}";')


if __name__ == "__main__":
    main()
