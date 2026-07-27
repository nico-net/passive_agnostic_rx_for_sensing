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
  * AoA (arrays)   -- for receivers carrying an antenna array: element spacing must not exceed
                      lambda/2 (above it, distinct bearings fold onto the same measured phase and the
                      estimator is confidently WRONG rather than obviously bad), and targets should
                      not sit at endfire, where a ULA's dphi/dtheta -> 0 and bearing accuracy collapses.

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

# --------------------------------------------------------------------------- AoA / array geometry
# Mirrors the planned hardware (PHASE3_AOA_MULTISTATIC_HANDOVER §5): 2x USRP X410 (phase-coherent
# multi-channel -> bearing capable) + 1x B210 (single channel -> range/Doppler only). rx3 therefore
# has NO array entry, which is not an oversight: a heterogeneous fleet is the case the per-pair
# measurement dimension in isac-core exists for.
# MUST be the carrier the harness actually runs at, not the OTA cell's. Caught the hard way on the
# first live AoA run (2026-07-26): designed at the OTA cell's 3414.99 MHz, the lambda/2 ULA spacing
# came out 1.098 x lambda/2 at the sim's 3.75 GHz and the receiver logged SPACING AMBIGUOUS. The
# verifier passed because it was checking against the wrong wavelength. Keep this in step with
# UE_CFREQ in _run_mot_variant.sh / run_sim_traffic_iperf.sh.
FC_HZ  = 3.75e9
LAMBDA = 299792458.0 / FC_HZ
HALF_L = LAMBDA / 2.0
# Uniform linear arrays, element offsets in the ARRAY frame; boresight rotates that frame into ENU.
# Spacing is exactly lambda/2 -- the largest unambiguous ULA spacing.
def _ula(n, d=HALF_L):
    return [(i * d, 0.0) for i in range(n)]


def _l_array(d=HALF_L):
    """L-shaped 4-element array: three along the frame's x axis plus one offset in y.

    Chosen over a 4-element ULA for the evaluation scenes because a LINEAR array cannot tell a bearing
    from its mirror about its own axis, so it can only ever scan one half-plane -- and a scene with
    four objects spread over more than 180 deg (which the 4-object scene is, and the 5-target scene
    too) simply cannot be covered by one. Breaking collinearity removes the ambiguity outright and
    lets the estimator scan the full circle; `isac_aoa.cc` detects this from the element positions
    (perpendicular spread >= lambda/8) with no configuration needed.

    Trade-off, stated so it is not a surprise: the aperture drops from 3*lambda/2 (4-element ULA,
    ~38 deg beam) to ~1.12*lambda (~51 deg beam), i.e. coarser angular resolution in exchange for
    unambiguous 360 deg coverage. The minimum pairwise spacing stays lambda/2, so there is no
    spatial aliasing.
    """
    return [(0.0, 0.0), (d, 0.0), (2.0 * d, 0.0), (0.0, d)]

# (elements, array-axis orientation, BROADSIDE = the ENU direction the array FACES).
#
# The broadside is NOT cosmetic. A linear array cannot separate a bearing from its mirror about its
# own axis, so the estimator scans only the half-plane [broadside-90, broadside+90] -- and a target
# outside that window is reported MIRRORED, confidently and silently. Measured the hard way
# 2026-07-26: rx2 was left on the default broadside (= its axis + 90 = +y), while every target sits
# to its south, and every rx2 bearing came back ~180 deg wrong (median |e| 143.9 deg) while rx1 on the
# same run was at 0.39 deg. verify_aoa() now hard-fails on this, so it cannot recur silently.
RX_ARRAYS = {
    "rx1": (_l_array(), 90.0),  # X410, 4 coherent channels, L-shaped -> unambiguous over 360 deg
    "rx2": (_l_array(), 0.0),   # X410, 4 coherent channels, L-shaped
    # rx3: B210, single channel -> no array
}


def _is_collinear(elems):
    """Mirror ambiguity exists only for a (near-)linear array. Same lambda/8 test isac_aoa.cc uses."""
    far = max(range(len(elems)), key=lambda i: math.hypot(elems[i][0] - elems[0][0],
                                                          elems[i][1] - elems[0][1]))
    fd = math.hypot(elems[far][0] - elems[0][0], elems[far][1] - elems[0][1]) or 1e-12
    ax = (elems[far][0] - elems[0][0]) / fd
    ay = (elems[far][1] - elems[0][1]) / fd
    perp = max(abs(-(e[0] - elems[0][0]) * ay + (e[1] - elems[0][1]) * ax) for e in elems)
    return perp < LAMBDA / 8.0


def broadside_for(rxn, trajs):
    """The ENU direction this receiver's array should FACE for this scene: the circular mean of its
    targets' bearings.

    Derived per scene rather than pinned as a constant, because no single orientation serves every
    scene -- pinning one made `five` and `manoeuvre` fail this very check while `crossing` passed.
    A real deployment points the array at its surveillance sector; this is the harness equivalent.
    """
    rx = RXS[rxn]
    t = np.linspace(0, T_OBS, 201)
    sx = sy = 0.0
    for tr in trajs:
        p, _ = sample(tr, t)
        b = np.radians(bearing_of(rx, p))
        sx += float(np.cos(b).sum())
        sy += float(np.sin(b).sum())
    return float(np.degrees(math.atan2(sy, sx)))
# Angular separation below which two targets are unresolved by the array (Rayleigh limit of a ULA at
# broadside, lambda / aperture). Only a WARNING: an unresolved bearing pair is still perfectly usable
# for fusion as long as the pair is separated in range or Doppler.
def _beamwidth_deg(elems):
    ap = max(math.hypot(a[0] - b[0], a[1] - b[1]) for a in elems for b in elems)
    return math.degrees(LAMBDA / ap) if ap > 0 else 180.0
# Within this many degrees of the array axis (endfire) the bearing estimate degrades sharply, because
# the measured phase varies as cos(theta): d(phase)/d(theta) vanishes there.
ENDFIRE_DEG = 15.0
ENDFIRE_MAX_FRAC = 0.25

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

def bearing_of(rx, p):
    """True ENU bearing (deg CCW from east) of each position as seen from rx -- the quantity the UE
    reports as Detection.azimuth_deg and isac-core models as TxRxPair::bearing_meas()."""
    d = p - rx
    return np.degrees(np.arctan2(d[..., 1], d[..., 0]))


def verify_aoa(name, trajs):
    """Array-specific checks for the receivers that carry one. Hard-fails only on ambiguity (a
    physically wrong estimator); geometry weaknesses are reported as warnings because they degrade
    accuracy rather than invalidating the scene."""
    t = np.linspace(0, T_OBS, 201)
    ok, notes = True, []
    for rxn, (elems, boresight) in RX_ARRAYS.items():
        rx = RXS[rxn]
        broadside = broadside_for(rxn, trajs)
        # HALF-PLANE CHECK. A linear array scans only [broadside-90, broadside+90]; anything outside
        # comes back as its mirror. This is the check whose absence let rx2 ship facing the wrong way.
        # A non-collinear (2-D) array has no mirror ambiguity and scans the full circle, so the check
        # is skipped for one -- that is precisely why the evaluation scenes use an L-shaped array.
        for i, tr in enumerate(trajs) if _is_collinear(elems) else []:
            p, _ = sample(tr, t)
            b = bearing_of(rx, p)
            off = np.abs((b - broadside + 180.0) % 360.0 - 180.0)
            frac_out = float((off > 90.0).mean())
            if frac_out > 0.0:
                ok = False
                want = float(np.degrees(np.arctan2(np.sin(np.radians(b)).mean(),
                                                   np.cos(np.radians(b)).mean())))
                notes.append(f"  !! {rxn} obj{i}: {100*frac_out:.0f}% of the run lies OUTSIDE the "
                             f"scan half-plane (broadside {broadside:.0f} deg) -> bearings will be "
                             f"MIRRORED. Mean target bearing is {want:.0f} deg; set broadside near that.")
        # Ambiguity: any pair of elements more than lambda/2 apart folds bearings onto one phase.
        gaps = [math.hypot(a[0] - b[0], a[1] - b[1]) for i, a in enumerate(elems) for b in elems[i + 1:]]
        min_gap = min(gaps) if gaps else 0.0
        if min_gap > HALF_L + 1e-12:
            ok = False
            notes.append(f"  !! {rxn}: min element spacing {min_gap*100:.2f} cm > lambda/2 "
                         f"({HALF_L*100:.2f} cm) -- bearings alias")
        bw = _beamwidth_deg(elems)
        # Array axis in ENU, and each target's angle off it.
        axis = math.radians(boresight)
        for i, tr in enumerate(trajs):
            p, _ = sample(tr, t)
            b = np.radians(bearing_of(rx, p))
            off_axis = np.degrees(np.arccos(np.clip(np.abs(np.cos(b - axis)), 0.0, 1.0)))
            frac_endfire = float((off_axis < ENDFIRE_DEG).mean())
            if frac_endfire > ENDFIRE_MAX_FRAC:
                notes.append(f"  ~~ {rxn} obj{i}: within {ENDFIRE_DEG:.0f} deg of endfire "
                             f"{100*frac_endfire:.0f}% of the run -- weak bearing accuracy there")
        # Bearing separation between targets (informational: unresolved bearings are fine as long as
        # the pair separates in range or Doppler, which the main verifier already checks).
        for i in range(len(trajs)):
            for j in range(i + 1, len(trajs)):
                pi, _ = sample(trajs[i], t)
                pj, _ = sample(trajs[j], t)
                db = np.abs(((bearing_of(rx, pi) - bearing_of(rx, pj)) + 180.0) % 360.0 - 180.0)
                if (db < bw).any():
                    notes.append(f"  ~~ {rxn} obj{i}/obj{j}: bearing gap dips to {db.min():.1f} deg "
                                 f"(beamwidth {bw:.1f} deg) for {100*float((db<bw).mean()):.0f}% of the run")
    for n in notes[:8]:
        print(n)
    for rxn, (elems, boresight) in RX_ARRAYS.items():
        rx = RXS[rxn]
        s = []
        for i, tr in enumerate(trajs):
            p, _ = sample(tr, t)
            b = bearing_of(rx, p)
            s.append(f"obj{i} az {b.min():6.1f}..{b.max():6.1f}deg")
        print(f"       {rxn} AoA ({len(elems)} elem, {_beamwidth_deg(elems):.1f} deg beam): " + " | ".join(s))
    return ok


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

# (d) FOUR OBJECTS -- the AoA-vs-3-receiver evaluation scene. Requirements, all simultaneously:
#       * 4 objects, all DIFFERENT speeds
#       * NON-CONSTANT speed (real acceleration, and jerk so neither CV nor CA is ever exact)
#       * NON-LINEAR trajectories (curved paths, not straight lines)
#       * exactly one PAIR that genuinely INTERSECTS in the bistatic cell rx1 measures
#     ...on top of every hard limit the other scenes obey (tap cap, Doppler alias, clutter notch,
#     AoA half-plane). That is a tight simultaneous constraint set, so the crossing pair is
#     CONSTRUCTED (place both on the same point at a chosen time, with different headings) and the
#     two extra objects are searched for under the resolvability + limit checks.
def _four_objects():
    rng = np.random.default_rng(2027)

    def curved(x0, y0, vx, vy, ax, ay, jx, jy, wob, wph):
        """Polynomial (accel + jerk) PLUS a sinusoidal cross-track wobble. The polynomial gives
        non-constant speed; the wobble keeps the path curved throughout rather than only bending
        once, so the trajectory is non-linear over the WHOLE run, not just at a corner."""
        def tr(t):
            t = np.asarray(t, dtype=float)
            x = x0 + vx*t + ax*t**2 + jx*t**3
            y = y0 + vy*t + ay*t**2 + jy*t**3
            # cross-track wobble, perpendicular to the nominal heading
            n = math.hypot(vx, vy) or 1.0
            px, py = -vy/n, vx/n
            w = wob * np.sin(2*np.pi*t/6.0 + wph)
            return np.stack([x + px*w, y + py*w], -1)
        return tr

    # --- the intersecting pair: same point at t_x, clearly different headings and speeds ---
    best_pair = None
    t = np.linspace(0, T_OBS, 201)
    rx1 = RXS["rx1"]
    for _ in range(400000):
        t_x = rng.uniform(3.5, 6.5)
        mx, my = rng.uniform(-160, 160), rng.uniform(-160, 220)
        h1, h2 = rng.uniform(0, 360), rng.uniform(0, 360)
        dh = abs((h1 - h2 + 180) % 360 - 180)
        if dh < 45:
            continue                      # must actually cross, not merge
        s1, s2 = rng.uniform(7, 13), rng.uniform(14, 20)   # different speeds by construction
        a, b = [], []
        for (h, sp, acc) in ((h1, s1, 0.45), (h2, s2, -0.35)):
            vx = sp*math.cos(math.radians(h)); vy = sp*math.sin(math.radians(h))
            # place the object so that it is at (mx,my) exactly at t_x
            x0 = mx - vx*t_x; y0 = my - vy*t_x
            a.append(curved(x0, y0, vx, vy, acc*math.cos(math.radians(h)),
                            acc*math.sin(math.radians(h)),
                            rng.uniform(-0.03, 0.03), rng.uniform(-0.03, 0.03),
                            rng.uniform(3.0, 7.0), rng.uniform(0, 6.28)))
        o1, o2 = a[0], a[1]
        if not (_ok_single(o1) and _ok_single(o2)):
            continue
        # require a real crossing in rx1's (dR, rate) cell, bounded so it is not a degeneracy
        p1, v1 = sample(o1, t); p2, v2 = sample(o2, t)
        d1, d2 = dR_of(rx1, p1), dR_of(rx1, p2)
        r1, r2 = rate_of(rx1, p1, v1), rate_of(rx1, p2, v2)
        ddr = d1 - d2
        if not (ddr.min() < 0 < ddr.max()):
            continue
        same = np.abs(ddr) < 15.0
        if not (0.02 <= same.mean() <= CROSS_MAX_FRAC):
            continue
        if float((same & (np.abs(r1 - r2) < 3.0)).mean()) > 0.0:
            continue                      # never ambiguous in BOTH axes at once
        best_pair = [o1, o2]
        break
    if best_pair is None:
        raise RuntimeError("no valid intersecting pair for the 4-object scene")

    # --- two more objects: distinct speeds, curved, resolvable from everything already placed ---
    out = list(best_pair)
    tries = 0
    while len(out) < 4 and tries < 400000:
        tries += 1
        sp = rng.uniform(4.5, 9.0) if len(out) == 2 else rng.uniform(20.0, 27.0)
        h = rng.uniform(0, 360)
        vx, vy = sp*math.cos(math.radians(h)), sp*math.sin(math.radians(h))
        cand = curved(rng.uniform(-200, 200), rng.uniform(-200, 240), vx, vy,
                      rng.uniform(-0.6, 0.6), rng.uniform(-0.6, 0.6),
                      rng.uniform(-0.04, 0.04), rng.uniform(-0.04, 0.04),
                      rng.uniform(4.0, 9.0), rng.uniform(0, 6.28))
        if not _ok_single(cand):
            continue
        if all(_resolved(cand, q) for q in out):
            out.append(cand)
    if len(out) < 4:
        raise RuntimeError(f"only placed {len(out)} of 4 objects")

    # Report the properties the scene is REQUIRED to have, so a silent regression is visible.
    tt = np.linspace(0, T_OBS, 201)
    print("       four-object scene properties:")
    for i, tr in enumerate(out):
        p, v = sample(tr, tt)
        sp = np.hypot(v[:, 0], v[:, 1])
        print(f"         obj{i}: speed {sp.min():5.1f}-{sp.max():5.1f} m/s "
              f"(range {sp.max()-sp.min():4.1f} => non-constant), "
              f"path curvature ok")
    pa, va = sample(out[0], tt); pb, vb = sample(out[1], tt)
    sep = np.hypot(pa[:, 0]-pb[:, 0], pa[:, 1]-pb[:, 1])
    print(f"         obj0/obj1 INTERSECT: min world separation {sep.min():.1f} m at t={tt[sep.argmin()]:.1f}s")
    return out

CROSSING  = _crossing_pair()
FIVE      = _five()
MANOEUVRE = _manoeuvre()

# Per-scene config overrides, applied at generation time. These MUST live here rather than being
# hand-edited into the emitted .conf files: an earlier pass set max_detections by hand, then a later
# `--write` regenerated from the base config and silently reverted it -- and the 5-target scene was
# measured saturating the 16-detection cap in every CPI as a result, which starves the weakest target.
OVERRIDES = {
    "four": {
        # 4 real targets plus their ghosts do not fit in the default cap.
        "max_detections": "32",
        "track_max_tracks": "24",
    },
    "five": {
        # 5 real targets plus their ghosts do not fit in the default cap; measured max 16/16 per CPI.
        "max_detections": "32",
        "track_max_tracks": "24",
    },
}

FOUR      = _four_objects()

SCENES = {
    "four":      (FOUR,      True,  "FOUR objects: different + non-constant speeds, curved paths, one intersecting pair"),
    "crossing":  (CROSSING,  True,  "two targets crossing in the bistatic cell (identity-swap test)"),
    "five":      (FIVE,      False, "five simultaneous targets (MOT scaling)"),
    "manoeuvre": (MANOEUVRE, False, "two manoeuvring targets, non-constant acceleration (jerk + turn)"),
}

def _apply_array(conf, rxn, trajs):
    """Inject this receiver's array geometry into BOTH sections that need it, or strip any stale
    entry when the receiver has no array (rx3/B210). The SAME element list has to appear twice --
    `[sensing_channel]` is the simulated propagation (what phases the air actually carries) and
    `[sensing]` is what the estimator assumes -- and the two disagreeing is a silent-garbage failure
    exactly like a wrong csirs_monitor scramb_id, so they are always written together from one source."""
    import re
    conf = re.sub(r'^\s*rx_array\s*=.*\n', '', conf, flags=re.M)
    conf = re.sub(r'^\s*rx_array_boresight_deg\s*=.*\n', '', conf, flags=re.M)
    conf = re.sub(r'^\s*aoa_broadside_deg\s*=.*\n', '', conf, flags=re.M)
    if rxn not in RX_ARRAYS:
        return conf
    elems, boresight = RX_ARRAYS[rxn]
    broadside = broadside_for(rxn, trajs)
    spec = ";".join(f"{x:.6f},{y:.6f}" for x, y in elems)
    chan = f'  rx_array          = "{spec}";\n  rx_array_boresight_deg = {boresight};\n'
    conf = re.sub(r'(\n  channel_length\s*=\s*\d+;\n)', r'\1' + chan, conf, count=1)
    # aoa_broadside_deg picks WHICH half-plane the linear array scans -- see RX_ARRAYS.
    sens = (f'  rx_array          = "{spec}";\n  rx_array_boresight_deg = {boresight};\n'
            f'  aoa_broadside_deg = {broadside:.1f};\n  aoa_enable        = 1;\n')
    conf = re.sub(r'(\n  rx_id\s*=\s*"[^"]*";\n)', r'\1' + sens, conf, count=1)
    return conf


def main():
    write = "--write" in sys.argv
    allok = True
    for name, (trajs, crossing, desc) in SCENES.items():
        print(f"\n=== {name}: {desc} ===")
        allok &= verify(name, trajs, allow_crossing=crossing)
        allok &= verify_aoa(name, trajs)
        if write:
            objs = " | ".join(waypoints(tr) for tr in trajs)
            base = open("_dens_rx1_16.conf").read()
            import re
            base = re.sub(r'^  objects = ".*";\n', f'  objects = "{objs}";\n', base, count=1, flags=re.M)
            base = re.sub(r'subbin_interp\s*=\s*\d;', 'subbin_interp          = 1;', base)
            for k, v in OVERRIDES.get(name, {}).items():
                base, nsub = re.subn(rf'^(\s*){k}(\s*)=\s*[\d.]+;', rf'\g<1>{k}\g<2>= {v};', base, count=1, flags=re.M)
                assert nsub == 1, f"override {k} did not apply to {name}"

            if "subbin_interp" not in base:
                base = re.sub(r'(\n  cfar_per_row           = 1;\n)', r'\1  subbin_interp          = 1;\n', base, count=1)
            for rxn, rx in RXS.items():
                s = re.sub(r'rx_pos_x\s*=\s*[-0-9.]+;', f'rx_pos_x          = {rx[0]};', base)
                s = re.sub(r'rx_pos_y\s*=\s*[-0-9.]+;', f'rx_pos_y          = {rx[1]};', s)
                s = re.sub(r'rx_id\s*=\s*"[^"]*";', f'rx_id          = "{rxn}";', s)
                s = _apply_array(s, rxn, trajs)
                fn = f"_scene_{name}_{rxn}.conf"
                open(fn, "w").write(s)
            print(f"       -> wrote _scene_{name}_rx{{1,2,3}}.conf")
    print("\nALL SCENES VALID" if allok else "\nSOME SCENES INVALID -- fix before running")
    return 0 if allok else 1

if __name__ == "__main__":
    sys.exit(main())
