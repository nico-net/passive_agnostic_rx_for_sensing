#!/usr/bin/env python3
"""Design + VERIFY + emit sensing_sim scenes, and write the per-receiver .conf files.

Every scene is checked against the limits this project measured the hard way, because violating any of
them fails SILENTLY (the target is simply never detected, with no error message):

  * CIR tap cap    -- dR must stay under ~622 m at 100 MHz / 273 PRB. The `~1244 m` in
                      sensing_channel.c is the 40 MHz figure; the cap is 255 TAPS, so it halves when
                      fs doubles. A scene at 650-930 m lost both targets before this was understood.
  * Doppler alias  -- |bistatic range-rate| must stay under vel_max = c/(2*fc*t_slow). A target of
                      speed v generates a rate up to 2v, so v is limited to ~vel_max/2 in the worst
                      geometry. With subslot_symbols=6 vel_max is ~+/-89 m/s.
  * Clutter notch  -- |rate| must clear zero_doppler_guard (~2.3 m/s) or the target is notched out
                      along with the direct path. An early MOT scene lost a target to this 70% of the
                      time.
  * Separation     -- targets must be resolvable in (dR, rate) except where a crossing is the POINT
                      of the scene.

Usage: make_scenes.py [--write]   (without --write it only prints the verification report)
"""
import sys, math
import numpy as np

TX = np.array([0.0, 0.0])
RXS = {"rx1": np.array([100.0, 0.0]), "rx2": np.array([0.0, 200.0]), "rx3": np.array([-50.0, -50.0])}
VMAX = 89.0        # subslot_symbols=6 -> ~+/-89 m/s unambiguous bistatic rate
NOTCH = 3 * 0.755  # zero_doppler_guard = 3 bins
TAPCAP = 622.0     # 100 MHz / 273 PRB
T_OBS = 10.0       # the harness captures ~8-10 s of trajectory per run
CROSS_MAX_FRAC = 0.20  # a crossing may overlap at most this fraction of the run
REFL = 0.30        # < LOS gain 1.0, so no reflector hijacks the UE's own time sync

def rate_of(rx, p, v):
    utx = (p - TX) / np.maximum(np.linalg.norm(p - TX, axis=-1, keepdims=True), 1e-9)
    urx = (p - rx) / np.maximum(np.linalg.norm(p - rx, axis=-1, keepdims=True), 1e-9)
    return np.sum(v * (utx + urx), axis=-1)

def dR_of(rx, p):
    return (np.linalg.norm(p - TX, axis=-1) + np.linalg.norm(p - rx, axis=-1)
            - float(np.linalg.norm(TX - rx)))

def sample(traj, t):
    """traj(t) -> (Nx2 positions, Nx2 velocities) by finite difference."""
    p = traj(t)
    h = 1e-3
    v = (traj(t + h) - traj(t - h)) / (2 * h)
    return p, v

def verify(name, trajs, allow_crossing=False):
    t = np.linspace(0, T_OBS, 201)
    ok, notes = True, []
    curves = {}
    for rxn, rx in RXS.items():
        for i, tr in enumerate(trajs):
            p, v = sample(tr, t)
            r = rate_of(rx, p, v)
            dR = dR_of(rx, p)
            a = np.abs(r)
            curves[(rxn, i)] = (dR, r)
            if dR.max() > TAPCAP:
                ok = False; notes.append(f"  !! {rxn} obj{i}: dR max {dR.max():.0f} m > tap cap {TAPCAP:.0f}")
            if dR.min() < 40:
                ok = False; notes.append(f"  !! {rxn} obj{i}: dR min {dR.min():.0f} m too close to LOS")
            if a.max() > VMAX:
                ok = False; notes.append(f"  !! {rxn} obj{i}: |rate| max {a.max():.1f} > vel_max {VMAX}")
            frac_notch = float((a < NOTCH).mean())
            if frac_notch > 0.05:
                ok = False; notes.append(f"  !! {rxn} obj{i}: in clutter notch {100*frac_notch:.0f}% of the time")
        # resolvability between objects at this receiver
        for i in range(len(trajs)):
            for j in range(i + 1, len(trajs)):
                dRi, ri = curves[(rxn, i)]; dRj, rj = curves[(rxn, j)]
                close = (np.abs(dRi - dRj) < 15.0) & (np.abs(ri - rj) < 3.0)
                frac = float(close.mean())
                if not allow_crossing:
                    if close.any():
                        ok = False
                        notes.append(f"  !! {rxn} obj{i}/obj{j}: unresolvable for {100*frac:.0f}% of the run")
                # A crossing scene is allowed -- indeed required -- to go briefly unresolvable, but a
                # PERSISTENT overlap is a degenerate scene, not a crossing: the two targets are then one
                # indistinguishable object at that receiver and no associator can do anything about it.
                # This bound exists because the first crossing design mirrored the velocities about the
                # bistatic bisector, which (the bisector being the iso-dR ellipse's normal) keeps both
                # targets on the SAME ellipse for the whole run -- 100% unresolvable at rx1, measured.
                elif frac > CROSS_MAX_FRAC:
                    ok = False
                    notes.append(f"  !! {rxn} obj{i}/obj{j}: overlap {100*frac:.0f}% > {100*CROSS_MAX_FRAC:.0f}%"
                                 f" -- degenerate, not a crossing")
    print(f"[{'OK  ' if ok else 'FAIL'}] {name}")
    for n in notes[:8]:
        print(n)
    # compact per-receiver summary
    for rxn in RXS:
        s = []
        for i in range(len(trajs)):
            dR, r = curves[(rxn, i)]
            s.append(f"obj{i} dR {dR.min():3.0f}-{dR.max():3.0f}m rate {np.abs(r).min():4.1f}-{np.abs(r).max():4.1f}")
        print(f"       {rxn}: " + " | ".join(s))
    return ok

def waypoints(traj, dur=20.0, step=0.5):
    pts = []
    for tt in np.arange(0, dur + 1e-9, step):
        p = traj(np.array([tt]))[0]
        pts.append(f"{tt:.1f},{p[0]:.1f},{p[1]:.1f}")
    return f"{REFL}; " + "; ".join(pts)

# ---------------------------------------------------------------- scenes
def lin(x0, y0, sp, hd):
    v = sp * np.array([math.cos(math.radians(hd)), math.sin(math.radians(hd))])
    return lambda t: np.stack([x0 + v[0] * np.asarray(t), y0 + v[1] * np.asarray(t)], -1)

def _ok_single(traj):
    """All hard limits for ONE target across all receivers (no inter-target checks)."""
    t = np.linspace(0, T_OBS, 121)
    p, v = sample(traj, t)
    for rx in RXS.values():
        a = np.abs(rate_of(rx, p, v)); dR = dR_of(rx, p)
        if dR.max() > TAPCAP - 20 or dR.min() < 60: return False
        if a.max() > VMAX - 5 or (a < NOTCH * 1.6).mean() > 0.02: return False
    return True

def _resolved(t1, t2, margin_m=25.0, margin_v=4.0):
    """True if the two targets are separable in (dR, rate) at EVERY receiver, all the time."""
    t = np.linspace(0, T_OBS, 121)
    p1, v1 = sample(t1, t); p2, v2 = sample(t2, t)
    for rx in RXS.values():
        d1, d2 = dR_of(rx, p1), dR_of(rx, p2)
        r1, r2 = rate_of(rx, p1, v1), rate_of(rx, p2, v2)
        if ((np.abs(d1 - d2) < margin_m) & (np.abs(r1 - r2) < margin_v)).any():
            return False
    return True

# (a) CROSSING -- constructed, not searched. Both targets pass through the SAME point at t=5 s, and
#     their velocities are mirror images about the bistatic bisector at rx1, so at that instant they
#     share BOTH dR and range-rate: a genuine collision in the (range, Doppler) cell rx1 measures,
#     while their world headings differ. That is precisely the case where a bistatic-domain associator
#     can swap identities, and the reason the handover has wanted this scene since Part 1.
def _crossing_pair():
    """Two targets whose bistatic-range tracks cross TRANSVERSALLY at rx1.

    Design note (learned by getting it wrong): you cannot build a clean *transient* collision in both
    range AND Doppler at once. Equal dR and equal range-rate at the same instant means the two dR(t)
    curves are TANGENT there, so they stay within a bin of each other for a long stretch -- a permanent
    degeneracy, not a crossing. The first attempt did exactly that (mirroring the velocities about the
    bistatic bisector, which is the iso-dR ellipse's normal, so both targets rode the same ellipse for
    the entire run: measured 100% unresolvable at rx1, and obj1 was correctly never reported).

    So the useful test is a transversal crossing: the targets occupy the SAME range bin for ~1 s while
    having clearly DIFFERENT range-rates, which is the association ambiguity that actually occurs in
    practice. They are searched for, and the verifier now bounds the overlap so a tangency can't slip
    back in.
    """
    rng = np.random.default_rng(3)
    best = None
    rx = RXS["rx1"]
    t = np.linspace(0, T_OBS, 201)
    for _ in range(300000):
        a = lin(rng.uniform(-200, 200), rng.uniform(-200, 260), rng.uniform(7, 14), rng.uniform(0, 360))
        b = lin(rng.uniform(-200, 200), rng.uniform(-200, 260), rng.uniform(7, 14), rng.uniform(0, 360))
        if not (_ok_single(a) and _ok_single(b)):
            continue
        pa, va = sample(a, t); pb, vb = sample(b, t)
        dA, dB = dR_of(rx, pa), dR_of(rx, pb)
        rA, rB = rate_of(rx, pa, va), rate_of(rx, pb, vb)
        ddr = dA - dB
        if not (ddr.min() < 0 < ddr.max()):
            continue                                  # require a real sign change: a transversal crossing
        same_bin = np.abs(ddr) < 15.0                  # within ~5 range bins
        if not (0.02 <= same_bin.mean() <= CROSS_MAX_FRAC):
            continue                                   # must happen, must not persist
        drate = np.abs(rA - rB)[same_bin].min()
        if drate < 6.0:
            continue                                   # clearly separated in Doppler while co-range
        unres = float((same_bin & (np.abs(rA - rB) < 3.0)).mean())
        if unres > 0.0:
            continue                                   # never simultaneously ambiguous in BOTH axes
        # Prefer the SMALLEST Doppler separation that still clears the floor: that is the hardest
        # crossing the association layer can be asked to survive without the scene being degenerate.
        # (Maximising it instead gave a 54 m/s separation -- co-range, but trivially separable.)
        score = -drate
        if best is None or score > best[0]:
            best = (score, a, b, same_bin.mean())
    if best is None:
        raise RuntimeError("no valid transversal crossing pair")
    print(f"       crossing: co-range for {100*best[3]:.0f}% of the run, "
          f"min |drate| while co-range = {-best[0]:.1f} m/s")
    return [best[1], best[2]]

# (b) FIVE targets -- randomised search under the hard limits plus pairwise resolvability.
def _five():
    rng = np.random.default_rng(7)
    picked = []
    tries = 0
    while len(picked) < 5 and tries < 200000:
        tries += 1
        cand = lin(rng.uniform(-220, 220), rng.uniform(-220, 260), rng.uniform(7, 14), rng.uniform(0, 360))
        if not _ok_single(cand):
            continue
        if all(_resolved(cand, q) for q in picked):
            picked.append(cand)
    if len(picked) < 5:
        raise RuntimeError(f"only found {len(picked)} of 5 targets")
    return picked

# (c) MANOEUVRING, NON-CONSTANT acceleration: quadratic + cubic terms give a continuously changing
#     acceleration (non-zero jerk) plus a turn, so neither the CV nor the CA model is ever exactly
#     right and the adaptive-q path is genuinely exercised. The earlier sinusoid scene only produced
#     constant-MAGNITUDE curvature, which a CA model handles almost exactly.
def _manoeuvre():
    rng = np.random.default_rng(11)
    out = []
    tries = 0
    while len(out) < 2 and tries < 200000:
        tries += 1
        x0, y0 = rng.uniform(-180, 180), rng.uniform(-180, 220)
        vx, vy = rng.uniform(-11, 11), rng.uniform(-11, 11)
        ax, ay = rng.uniform(-0.7, 0.7), rng.uniform(-0.7, 0.7)
        jx, jy = rng.uniform(-0.05, 0.05), rng.uniform(-0.05, 0.05)
        def tr(t, x0=x0, y0=y0, vx=vx, vy=vy, ax=ax, ay=ay, jx=jx, jy=jy):
            t = np.asarray(t, dtype=float)
            return np.stack([x0 + vx*t + ax*t**2 + jx*t**3, y0 + vy*t + ay*t**2 + jy*t**3], -1)
        if abs(jx) < 0.015 and abs(jy) < 0.015:
            continue      # insist on a real jerk, otherwise it is just a CA target
        if not _ok_single(tr):
            continue
        if all(_resolved(tr, q) for q in out):
            out.append(tr)
    if len(out) < 2:
        raise RuntimeError("no valid manoeuvring pair")
    return out

CROSSING  = _crossing_pair()
FIVE      = _five()
MANOEUVRE = _manoeuvre()

SCENES = {
    "crossing":  (CROSSING,  True,  "two targets crossing in the bistatic cell (identity-swap test)"),
    "five":      (FIVE,      False, "five simultaneous targets (MOT scaling)"),
    "manoeuvre": (MANOEUVRE, False, "two manoeuvring targets, non-constant acceleration (jerk + turn)"),
}

def main():
    write = "--write" in sys.argv
    allok = True
    for name, (trajs, crossing, desc) in SCENES.items():
        print(f"\n=== {name}: {desc} ===")
        allok &= verify(name, trajs, allow_crossing=crossing)
        if write:
            objs = " | ".join(waypoints(tr) for tr in trajs)
            base = open("_dens_rx1_16.conf").read()
            import re
            base = re.sub(r'^  objects = ".*";\n', f'  objects = "{objs}";\n', base, count=1, flags=re.M)
            base = re.sub(r'subbin_interp\s*=\s*\d;', 'subbin_interp          = 1;', base)
            if "subbin_interp" not in base:
                base = re.sub(r'(\n  cfar_per_row           = 1;\n)', r'\1  subbin_interp          = 1;\n', base, count=1)
            for rxn, rx in RXS.items():
                s = re.sub(r'rx_pos_x\s*=\s*[-0-9.]+;', f'rx_pos_x          = {rx[0]};', base)
                s = re.sub(r'rx_pos_y\s*=\s*[-0-9.]+;', f'rx_pos_y          = {rx[1]};', s)
                s = re.sub(r'rx_id\s*=\s*"[^"]*";', f'rx_id          = "{rxn}";', s)
                fn = f"_scene_{name}_{rxn}.conf"
                open(fn, "w").write(s)
            print(f"       -> wrote _scene_{name}_rx{{1,2,3}}.conf")
    print("\nALL SCENES VALID" if allok else "\nSOME SCENES INVALID -- fix before running")
    return 0 if allok else 1

if __name__ == "__main__":
    sys.exit(main())
