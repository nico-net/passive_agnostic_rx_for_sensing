#!/usr/bin/env python3
"""Stage 9 (cross-receiver association), DL + UL legs.

Per CPI: enumerate one-DL-block-per-receiver combinations (>= 3 receivers), solve the 6-D state,
then on every receiver admit the UL block (UE-illuminated, DTD/DFS relative to the UE->rx direct
path) that is consistent with that state within the conventional GATE_SIGMA using the block's own
sigma -- or none -- and refit with all admitted blocks.  Rank by residual per degree of freedom
and emit the best with its equation count / dof.  UE position is infrastructure metadata (static
UE) or comes from the UE localiser.  Ground truth is not read here."""
from __future__ import annotations
import argparse, itertools, json, math
import numpy as np

GATE_SIGMA = 2.0
import os as _os
# 2026-09-20 exclusive block partition (OUR ADAPTATION, declared before evaluation): after all
# consistent fits of a CPI are known, objects are selected greedily in the frozen ranking order
# (dof, then residual) with the constraint that a stage-8 block belongs to ONE object; any fit
# sharing a block with an already selected object is explained by it (mirror roots use the same
# blocks as the true root, so they vanish by construction).  Two objects may be within one range
# cell in xy (a crossing) as long as their blocks are disjoint.  PARTITION=0 restores the
# position-only deduplication of 2026-09-19.
PARTITION = _os.environ.get("STAGE9_PARTITION", "1") == "1"
# selection order inside the partition: "dof" = frozen ranking (most blocks explained first);
# "rms" = best whitened residual per dof first (a mixed fit of two crossing objects has more
# blocks but a worse residual than either object alone)
PARTITION_RANK = _os.environ.get("STAGE9_PARTITION_RANK", "dof")
# 2026-09-20 ADAPTIVE BLOCK SIGMA (user requirement: sigma calibrates online, no fixed floors).
# Per (leg, receiver) the range and rate residuals of the blocks used by the emitted objects of
# PAST CPIs are accumulated (causal); their robust scale (1.4826*MAD over a sliding window) is
# the block sigma used to whiten the NEXT CPIs, combined with the block's own reported range
# sigma (max).  Before MIN_SAMPLES residuals exist the declared priors apply: range cell/sqrt(12)
# (the previous constant) and rate cell/sqrt(12) (previously the full rate cell was used as
# sigma, 3.5x larger than the measured rate error).  ADAPTIVE_SIGMA=0 restores the constants.
ADAPTIVE_SIGMA = _os.environ.get("STAGE9_ADAPTIVE_SIGMA", "0") == "1"
# 2026-09-20 PREDICTION-SEEDED ASSOCIATION (OUR ADAPTATION, "track-before-associate", declared before
# evaluation).  Objects emitted in previous CPIs are carried forward with a constant-velocity
# prediction (covariance = last solution covariance + adaptive process noise, covariance matching
# as in the tracker).  Per CPI, each carried object claims, per (leg, receiver), the block with the
# smallest whitened innovation inside the conventional GATE_SIGMA and is re-solved from those
# blocks (seed = prediction); consistent objects consume their blocks.  Only the RESIDUE is
# enumerated exhaustively for new objects (the existing search + partition).  This replaces the
# arbitrary partition order by measurement-to-object likelihood and removes most of the
# combinatorics.  An object not re-solved for SEEDED_COAST_S is dropped.  STAGE9_SEEDED=0 -> off.
SEEDED = _os.environ.get("STAGE9_SEEDED", "1") == "1"
# 2026-09-20 CONSISTENCY GATE (declared): a fit is consistent iff chi2 <= chi2 quantile(dof) at the
# 2-sigma-equivalent probability (0.9545), the standard test.  The previous rule rms_per_dof <= 2
# is chi2 <= 4*dof, which is far looser than the quantile for dof > 4 (dof 18: 72 vs 29) and is
# what let 5-block fits from a low-quality leg pass.  Meaningful only with honest block sigma
# (STAGE9_ADAPTIVE_SIGMA).  STAGE9_CHI2_GATE=0 restores the rms rule.
CHI2_GATE = _os.environ.get("STAGE9_CHI2_GATE", "0") == "1"   # default OFF: REJECTED 2026-09-20 -- true fits are not chi2-consistent (cell-quantised block errors are not Gaussian): frozen runner emitted 81/139 CPIs, bike 110/145
from scipy.stats import chi2 as _chi2
_CHI2_Q = {}
def chi2_quantile(dof):
    if dof not in _CHI2_Q: _CHI2_Q[dof] = float(_chi2.ppf(0.9545, dof))
    return _CHI2_Q[dof]


MAX_SPEED_MPS = 50.0     # declared kinematic bound, same constant stage 8 applies to components


def is_admissible(fit, rx_positions):
    """Physical admissibility of an EXACTLY-DETERMINED (dof = 0) ground fit: there is no residual
    left to test, so the only per-CPI evidence is that the state is physically possible. Repetition
    across CPIs is then tested by the tracker's look-ahead confirmation, not here."""
    st = fit["state"]
    if not np.all(np.isfinite(st)):
        return False
    if float(np.hypot(st[3], st[4])) > MAX_SPEED_MPS:
        return False
    values = rx_positions.values() if hasattr(rx_positions, "values") else rx_positions
    centre = np.mean(np.array(list(values)), axis=0)
    return bool(np.linalg.norm(st[:2] - centre[:2]) <= MAX_RANGE_M)


MAX_RANGE_M = 625.0      # detector range support (c * subcarriers / bandwidth), a transport bound


def is_consistent(fit, calibrated=True):
    """calibrated=False (bootstrap: the online sigma of some block's (leg, receiver) still on its
    declared prior) keeps the previous rms rule; the chi-square test needs measured sigma.
    Measured without this: the gate rejected true fits on the priors, no residuals were collected,
    and the sigma never left the prior (frozen runner emitted in 81/139 CPIs)."""
    if fit["dof"] <= 0:
        return False
    if CHI2_GATE and calibrated:
        return bool(2.0 * fit["cost"] <= chi2_quantile(fit["dof"]))
    return bool(fit["rms_per_dof"] <= GATE_SIGMA)
SEEDED_COAST_S = 2.0            # same coast horizon as the tracker
SEEDED_ACCEL0 = 50.0            # initial process-noise acceleration (m/s^2), adapted per object
SEEDED_Q_EMA = 0.1
# Carried objects must satisfy the same physical evidence rule as tracker births (a real object is
# illuminated by DL and UL) and must be above ground (z >= -1 range cell, a declared physical bound
# applied to EMITTED solutions, not inside the solver).  Measured before these rules (33 m drone,
# tracker C): two 120-CPI carried ghosts 30-40 m away at z = -8 m built from UL-only blocks.
SEEDED_REQUIRE_DLUL = _os.environ.get("STAGE9_SEEDED_DLUL", "1") == "1"
# "dl": carry any fit that includes a DL block (UL-only fits, the measured ghost source, are still not carried);
# with the partial update this lets a DL-only object keep its identity across CPIs with < 3 receivers.
SEEDED_REQUIRE_DL = _os.environ.get("STAGE9_SEEDED_DLUL", "1") == "dl"
Z_MIN_M = -3.0503913105413107   # default OFF: freeze-set A-B 2026-09-20 mixed (runner +23, person -20, two-object B scenes -25..-44 points); re-measure on the 33 m set
SIGMA_WINDOW = 200          # residual samples kept per (leg, receiver) (~15-50 CPIs)
SIGMA_MIN_SAMPLES = 20


class OnlineSigma:
    def __init__(self, range_prior, rate_prior):
        self.r = {}; self.v = {}; self.range_prior = range_prior; self.rate_prior = rate_prior
    def add(self, key, dr, dv):
        self.r.setdefault(key, []).append(float(dr)); self.v.setdefault(key, []).append(float(dv))
        if len(self.r[key]) > SIGMA_WINDOW: self.r[key].pop(0); self.v[key].pop(0)
    @staticmethod
    def _scale(x, prior):
        if len(x) < SIGMA_MIN_SAMPLES: return prior
        a = np.asarray(x); return max(1.4826 * float(np.median(np.abs(a - np.median(a)))), 1e-3)
    def sigmas(self, key):
        return self._scale(self.r.get(key, []), self.range_prior), self._scale(self.v.get(key, []), self.rate_prior)
    def calibrated(self, blocks):
        return all(len(self.r.get((b["leg"], b["receiver"]), [])) >= SIGMA_MIN_SAMPLES for b in blocks)
Z_MIN_M = 0.0        # terrain height (scene ground plane); a declared physical bound, not a prior on the object
Z_PRIOR = _os.environ.get("STAGE9_Z_PRIOR", "0") == "1"
# (2026-09-21) Two-hypothesis height model (STAGE9_HEIGHT_MODEL=1): H_ground = z fixed at the ground-object
# hub height (5 free parameters), H_air = z free (6). Both are solved on the same blocks; H_air is kept only
# when the extra parameter is justified by the standard model-selection penalty on the whitened residuals
# (BIC: 2*(c_ground - c_air) > ln(n_eq)) and its z lies above the terrain. No class label, no truth: the
# data decide per CPI. Reported as height_model in every hypothesis.
HEIGHT_MODEL = _os.environ.get("STAGE9_HEIGHT_MODEL", "0") == "1"
HEIGHT_GROUND_M = float(_os.environ.get("STAGE9_HEIGHT_GROUND_M", "1.0"))
HEIGHT_SIGMA_GROUND_M = 3.0503913105413107 / math.sqrt(12.0)
Z_IDENT_SIGMA_M = float(_os.environ.get("STAGE9_Z_IDENT_SIGMA_M", "3.0503913105413107"))   # one range cell   # hub-height uncertainty carried by H_ground: one cell / sqrt(12)
Z_PRIOR_M = float(_os.environ.get("STAGE9_Z_PRIOR_M", "1.0"))          # terrain + hub height of a ground object
Z_PRIOR_SIGMA_M = float(_os.environ.get("STAGE9_Z_PRIOR_SIGMA_M", "3.0503913105413107"))   # one range cell


def forward(state, tx, rx, tx_vel=None):
    """Bistatic excess range/rate for a (possibly moving) illuminator: the rate of |p-tx| includes
    the illuminator's own velocity (a walking UE shifts it by ~1 rate cell)."""
    p, v = state[:3], state[3:]
    tl, rl = p - tx, p - rx
    tr, rr = np.linalg.norm(tl), np.linalg.norm(rl)
    vt = v if tx_vel is None else v - np.asarray(tx_vel)
    return tr + rr - np.linalg.norm(tx - rx), float(np.dot(tl / tr, vt) + np.dot(rl / rr, v))


def _ill(b, tx, ue):
    """Illuminator position/velocity of a block: per-block (moving UE, from the localiser) when
    present, else the fixed UE for 'ul' legs or the gNB for 'dl'."""
    if b.get("ill_pos") is not None:
        return np.asarray(b["ill_pos"]), np.asarray(b.get("ill_vel") or (0.0, 0.0, 0.0))
    return (ue if b["leg"].startswith("ul") else tx), np.zeros(3)


def residuals(state, blocks, tx, ue, rx_positions, rate_scale):
    out = []
    for b in blocks:
        ill, ill_v = _ill(b, tx, ue)
        rng, rate = forward(state, ill, rx_positions[b["receiver"]], ill_v)
        out.append((rng - b["range_m"]) / max(b["range_sigma_m"], 1e-6))
        out.append((rate - b["rate_mps"]) / b.get("rate_sigma_mps", rate_scale))
    return out


def _forward_batch(states, ills, rxs, ill_vels=None):
    """states [N,6]; ills/rxs [N,M,3] -> ranges [N,M], rates [N,M] (bistatic excess range & rate)."""
    p = states[:, None, :3]; v = states[:, None, 3:]
    tl = p - ills; rl = p - rxs
    tr = np.linalg.norm(tl, axis=2); rr = np.linalg.norm(rl, axis=2)
    rng = tr + rr - np.linalg.norm(ills - rxs, axis=2)
    vt = v if ill_vels is None else v - ill_vels
    rate = np.einsum("nmk,nmk->nm", tl / tr[..., None], vt) + np.einsum("nmk,nmk->nm", rl / rr[..., None], v)
    return rng, rate


def _jacobian_batch(states, ills, rxs, ill_vels=None):
    """Analytic Jacobian of _forward_batch: returns dR [N,M,6], dV [N,M,6] (d range / d state, d rate / d state).
    range = |p-T| + |p-R| - |T-R|;  rate = u_T.(v - v_T) + u_R.v  with u = unit vectors from T/R to p."""
    p = states[:, None, :3]; v = states[:, None, 3:]
    tl = p - ills; rl = p - rxs
    tr = np.linalg.norm(tl, axis=2)[..., None]; rr = np.linalg.norm(rl, axis=2)[..., None]
    uT = tl / tr; uR = rl / rr
    vt = v if ill_vels is None else v - ill_vels
    N, M = tr.shape[0], tr.shape[1]
    dR = np.zeros((N, M, 6)); dV = np.zeros((N, M, 6))
    dR[..., :3] = uT + uR
    # d(u.w)/dp = (w - (u.w) u) / |p - X|  for u = (p - X)/|p - X|
    dV[..., :3] = (vt - np.sum(uT * vt, axis=2, keepdims=True) * uT) / tr + (v - np.sum(uR * v, axis=2, keepdims=True) * uR) / rr
    dV[..., 3:] = uT + uR
    return dR, dV


ANALYTIC_JAC = _os.environ.get("STAGE9_ANALYTIC_JAC", "1") == "1"   # exact derivatives (2026-09-21, real-time); 0 = finite differences


def solve_batch(block_sets, tx, ue, rx_positions, rate_scale, seeds=None, iters=25, ground_only=False):
    """Levenberg-damped Gauss-Newton, vectorised over hypotheses of equal size.
    Returns a list of dicts (state, cost, n_eq, dof, rms_per_dof, cov)."""
    N = len(block_sets); M = len(block_sets[0])
    ills = np.array([[_ill(b, tx, ue)[0] for b in bs] for bs in block_sets]); ill_vels = np.array([[_ill(b, tx, ue)[1] for b in bs] for bs in block_sets])
    rxs = np.array([[rx_positions[b["receiver"]] for b in bs] for bs in block_sets])
    zr = np.array([[b["range_m"] for b in bs] for bs in block_sets]); zv = np.array([[b["rate_mps"] for b in bs] for bs in block_sets])
    sr = np.array([[max(b["range_sigma_m"], 1e-6) for b in bs] for bs in block_sets]); sv = np.array([[b.get("rate_sigma_mps", rate_scale) for b in bs] for bs in block_sets])
    centre = rxs.mean(axis=1)
    seed_list = [np.asarray(seeds)] if seeds is not None else [np.column_stack([centre[:, 0], centre[:, 1], np.full(N, z0), np.zeros((N, 3))]) for z0 in (1.0, 10.0, 25.0)]
    def resid(st):
        rng, rate = _forward_batch(st, ills, rxs, ill_vels)
        parts = [(rng - zr) / sr, (rate - zv) / sv]
        if Z_PRIOR:
            # (2026-09-21) MAP regularisation of the weakly observable height. At low illuminator
            # elevation (macro gNB at 300 m: ~6 deg) the bistatic ranges of a ground object are
            # nearly insensitive to z, so the unconstrained root drifts along that null direction
            # (measured: car +8..+12 m, runner +7.5 m) and drags xy with it. Prior: hub height of a
            # ground object (terrain + 1 m) with sigma = one range cell (declared regularisation
            # scale, as in the UE localiser). For an airborne object the RX-leg sensitivity
            # (dR/dz ~ dz/d, e.g. 0.3 at 19 m / 60 m) makes the data term dominate the prior.
            parts.append(((st[:, 2] - Z_PRIOR_M) / Z_PRIOR_SIGMA_M)[:, None])
        return np.concatenate(parts, axis=1)
    NE = 2 * M + (1 if Z_PRIOR else 0)
    def gauss_newton(seed_list, fix_z, fix_vz=False):
        best_x = best_c = best_J = None
        for x in seed_list:
            x = x.copy(); lam = np.full(N, 1e-3); r = resid(x); c = 0.5 * np.sum(r * r, axis=1); J = np.zeros((N, NE, 6))
            for _ in range(iters):
                if ANALYTIC_JAC:
                    dR, dV = _jacobian_batch(x, ills, rxs, ill_vels)
                    J[:, :M, :] = dR / sr[..., None]; J[:, M:2 * M, :] = dV / sv[..., None]
                    if Z_PRIOR: J[:, 2 * M, :] = 0.0; J[:, 2 * M, 2] = 1.0 / Z_PRIOR_SIGMA_M
                    if fix_z: J[:, :, 2] = 0.0
                    if fix_vz: J[:, :, 5] = 0.0
                else:
                    eps = 1e-5
                    for k in range(6):
                        if fix_z and k == 2: continue
                        if fix_vz and k == 5: continue
                        dx = x.copy(); dx[:, k] += eps; J[:, :, k] = (resid(dx) - r) / eps
                JTJ = np.einsum("nik,nil->nkl", J, J); g = np.einsum("nik,ni->nk", J, r)
                A = JTJ + lam[:, None, None] * np.eye(6)[None]
                if fix_z: A[:, 2, 2] = 1.0; g[:, 2] = 0.0
                if fix_vz: A[:, 5, 5] = 1.0; g[:, 5] = 0.0
                try:
                    step = np.linalg.solve(A, -g[..., None])[..., 0]
                except np.linalg.LinAlgError:
                    step = np.zeros_like(x)
                if fix_z: step[:, 2] = 0.0
                if fix_vz: step[:, 5] = 0.0
                xn = x + step
                rn = resid(xn); cn = 0.5 * np.sum(rn * rn, axis=1)
                better = cn < c
                x = np.where(better[:, None], xn, x); r = np.where(better[:, None], rn, r); c = np.where(better, cn, c)
                lam = np.where(better, lam * 0.3, lam * 5.0)
                if np.all(np.abs(step) < 1e-4): break
            if best_x is None: best_x, best_c, best_J = x, c, J
            else:
                sel = c < best_c; best_x = np.where(sel[:, None], x, best_x); best_c = np.where(sel, c, best_c); best_J = np.where(sel[:, None, None], J, best_J)
        return best_x, best_c, best_J
    # NOTE (2026-09-19): a projected z >= 0 bound was measured to bias z upward and collapse sigma_z
    # at the boundary; the unconstrained roots are the honest representation of H_air.
    if ground_only:
        # Exactly-determined ground state: z = hub height, vz = 0, 4 free parameters.
        g_seed = [np.column_stack([sd[:, 0], sd[:, 1], np.full(N, HEIGHT_GROUND_M),
                                   sd[:, 3], sd[:, 4], np.zeros(N)]) for sd in seed_list[:1]]
        gx, gc, gJ = gauss_newton(g_seed, fix_z=True, fix_vz=True)
        n = 2 * M; out = []
        for i in range(N):
            cov = np.linalg.pinv(gJ[i].T @ gJ[i])
            cov[2, :] = 0.0; cov[:, 2] = 0.0; cov[2, 2] = HEIGHT_SIGMA_GROUND_M ** 2
            cov[5, :] = 0.0; cov[:, 5] = 0.0
            dof = n - 4
            out.append({"state": gx[i], "cost": float(gc[i]), "n_eq": n, "dof": dof,
                        "rms_per_dof": math.sqrt(2.0 * gc[i] / max(dof, 1)), "cov": cov,
                        "height_model": "ground"})
        return out
    best_x, best_c, best_J = gauss_newton(seed_list, fix_z=False)
    height_model = ["air"] * N
    if HEIGHT_MODEL:
        g_seed = [np.column_stack([sd[:, 0], sd[:, 1], np.full(N, HEIGHT_GROUND_M), sd[:, 3], sd[:, 4], np.zeros(N)]) for sd in seed_list[:1]]
        gx, gc, gJ = gauss_newton(g_seed, fix_z=True)
        n_eq = 2 * M
        air_justified = (2.0 * (gc - best_c) > math.log(max(n_eq, 2))) & (best_x[:, 2] >= Z_MIN_M)
        # (2026-09-21) Identifiability: H_air is only meaningful when the data actually constrain z.
        # At low illuminator elevation the free-z root of a ground object lands a few metres off in xy
        # with z = 8-14 m and a slightly lower cost (measured: duplicate 'air' twins of the car in
        # car+bike and drone+car, 84-121 false states). The Fisher posterior sigma_z of the air fit
        # must be within one range cell (declared scale); otherwise z is unidentified and the ground
        # model is the honest description. A drone at 19 m / 60 m has dR/dz ~ 0.3 -> sigma_z ~ 1-2 m.
        for i in range(N):
            if air_justified[i]:
                cov_air = np.linalg.pinv(best_J[i].T @ best_J[i])
                if not (np.isfinite(cov_air[2, 2]) and math.sqrt(max(cov_air[2, 2], 0.0)) <= Z_IDENT_SIGMA_M):
                    air_justified[i] = False
            if not air_justified[i]:
                best_x[i] = gx[i]; best_c[i] = gc[i]; best_J[i] = gJ[i]; height_model[i] = "ground"
    n = 2 * M; dof = max(n - 6, 1); out = []
    for i in range(N):
        cov = np.linalg.pinv(best_J[i].T @ best_J[i])
        k_free = 5 if height_model[i] == "ground" else 6
        if height_model[i] == "ground":
            cov[2, :] = 0.0; cov[:, 2] = 0.0; cov[2, 2] = HEIGHT_SIGMA_GROUND_M ** 2
        dof_i = max(n - k_free, 1)
        out.append({"state": best_x[i], "cost": float(best_c[i]), "n_eq": n, "dof": n - k_free, "rms_per_dof": math.sqrt(2.0 * best_c[i] / dof_i), "cov": cov, "height_model": height_model[i]})
    return out


PARTIAL_UPDATE = _os.environ.get("STAGE9_PARTIAL_UPDATE", "0") == "1"   # carried objects update from fewer receivers (MAP with the predicted state as prior)
PARTIAL_MIN_RX = int(_os.environ.get("STAGE9_PARTIAL_MIN_RX", "2"))      # minimum receivers for a partial update (1 receiver cannot test the object: measured runner ghosts)


# (2026-09-22) TWO-RECEIVER GROUND SOLVE (STAGE9_MIN_RX_GROUND=2).
# The >= 3 receiver rule exists because the free-z state has 6 unknowns and each receiver supplies 2
# equations. For a GROUND object z is not estimated at all (measured: sigma_z 5-15 m at 6 deg
# illuminator elevation, so H_air is never identifiable), and a ground object on a horizontal plane
# also has vz = 0. That leaves 4 unknowns (x, y, vx, vy) against 2 equations per receiver, so TWO
# receivers determine the state exactly. Redundancy is then zero, i.e. there is no per-CPI residual
# to test, so a 2-receiver block set is admitted only under the ground model and must still pass the
# physical admissibility bounds and the tracker's multi-CPI confirmation. Air hypotheses continue to
# require >= 3 receivers: with 2 they would be underdetermined.
MIN_RX_GROUND = int(_os.environ.get("STAGE9_MIN_RX_GROUND", "3"))

# (2026-09-22) TEMPORAL REDUNDANCY (STAGE9_TEMPORAL=1).
# Measured need: the four receivers miss the target INDEPENDENTLY. Per-receiver detection
# probability on the target is 0.67-0.74 (person), 0.48-0.77 (runner), 0.77-0.92 (car), and the
# empirical P(>= 3 receivers in the same CPI) matches the independent-receiver model to within 0.04
# (person 0.69 vs 0.66, runner 0.64 vs 0.60, car 0.84 vs 0.89). Requiring three SIMULTANEOUS
# receivers therefore discards 30-70% of the CPIs in which the object was actually observed
# (person: 9% of all CPIs on 1-2 receivers only; runner: 28%).
#
# A BIRTH still needs the full spatial redundancy: an unknown object must be over-determined before
# it is allowed to exist. But once an object has been born that way, its predicted state carries the
# information the missing receivers would have supplied, so the update is over-determined in the
# Bayesian sense even with two receivers -- redundancy in TIME replacing redundancy in SPACE.
#
# The earlier attempt at this (2-RX exactly-determined ground solve accepted on physical
# admissibility alone) quadrupled the false states (26 -> 99) because a dof-0 fit has no residual to
# test. The test used here is not the residual: it is the INNOVATION chi-square of the gated blocks
# against the object's own prediction, whitened by the predicted covariance (innovation_z already
# computes exactly that per block). That statistic exists no matter how few receivers there are, and
# it is the same quantity the tracker gates on. Threshold: the declared chi2 quantile of the
# measurement count, as everywhere else in this module.
# (2026-09-22) CARRY RETIREMENT (STAGE9_CARRY_RETIRE=1).
# A carried object was retired ONLY by elapsed time (SEEDED_COAST_S = 2 s, ~27 CPIs). Nothing
# checked whether it was still supported, so a stale prediction kept claiming whatever passed its
# widening innovation gate -- worst exactly when the target sits in the +-0.57 m/s clutter notch and
# clutter residue is nearest. Measured: enabling DL-only carry costs the runner (34% of CPIs notched)
# 8.6 points of coverage and 11 of precision, while costing the continuously-observed car nothing.
# Retirement now also requires evidence: an object is dropped after MAX_UNSUPPORTED consecutive CPIs
# without a gated update, and when its predicted position sigma exceeds the association gate (at
# which point the prediction can no longer discriminate between candidates anyway). Both are derived
# from quantities already computed -- the gate radius and the propagated covariance.
CARRY_RETIRE = _os.environ.get("STAGE9_CARRY_RETIRE", "0") == "1"
CARRY_MAX_UNSUPPORTED = int(_os.environ.get("STAGE9_CARRY_MAX_UNSUPPORTED", "3"))
CARRY_MAX_SIGMA_M = float(_os.environ.get("STAGE9_CARRY_MAX_SIGMA_M", str(3.0503913105413107)))  # one range cell

TEMPORAL = _os.environ.get("STAGE9_TEMPORAL", "0") == "1"
TEMPORAL_MIN_RX = int(_os.environ.get("STAGE9_TEMPORAL_MIN_RX", "2"))   # 1 receiver cannot test the object

WINDOW_BIRTH = _os.environ.get("STAGE9_WINDOW_BIRTH", "0") == "1"   # multi-CPI birth from single-receiver looks (2026-09-22)
WINDOW_BIRTH_CPIS = int(_os.environ.get("STAGE9_WINDOW_BIRTH_CPIS", "5"))   # = tracker look-ahead window
WINDOW_BIRTH_MAX_PER_RX = 4


def solve_window_ground(block_sets, times_rel, tx, ue, rx_positions, rate_scale, iters=30):
    """(2026-09-22) Accumulated birth: solve a constant-velocity GROUND state (x, y, z = hub height, vx, vy)
    referenced to the current CPI from blocks taken at different CPIs (times_rel <= 0), each block seen by one
    receiver. Measured need: in the 3-4 object scenes most objects are seen by ONE receiver per CPI (persons3_bike:
    person0 67 CPIs, >= 3 receivers in 1), so a single-CPI fix never forms although the object is observed
    continuously. Same whitened residuals and consistency gate as the single-CPI solve; z is not free (the
    window is too short for the height model), the air alternative is left to later CPIs once carried."""
    N = len(block_sets); M = len(block_sets[0])
    ills = np.array([[_ill(b, tx, ue)[0] for b in bs] for bs in block_sets]); ill_vels = np.array([[_ill(b, tx, ue)[1] for b in bs] for bs in block_sets])
    rxs = np.array([[rx_positions[b["receiver"]] for b in bs] for bs in block_sets])
    zr = np.array([[b["range_m"] for b in bs] for bs in block_sets]); zv = np.array([[b["rate_mps"] for b in bs] for bs in block_sets])
    sr = np.array([[max(b["range_sigma_m"], 1e-6) for b in bs] for bs in block_sets]); sv = np.array([[b.get("rate_sigma_mps", rate_scale) for b in bs] for bs in block_sets])
    tau = np.asarray(times_rel, float)                     # [N,M] block time relative to the current CPI (<= 0)
    def resid(st):
        # propagate the state to each block's time: p_b = p + v * tau_b
        stb = np.repeat(st[:, None, :], M, axis=1).copy(); stb[:, :, :3] += stb[:, :, 3:] * tau[:, :, None]
        rng = np.zeros((N, M)); rate = np.zeros((N, M))
        for m in range(M):
            r_m, v_m = _forward_batch(stb[:, m, :], ills[:, m:m + 1, :], rxs[:, m:m + 1, :], ill_vels[:, m:m + 1, :])
            rng[:, m] = r_m[:, 0]; rate[:, m] = v_m[:, 0]
        return np.concatenate([(rng - zr) / sr, (rate - zv) / sv], axis=1)
    centre = rxs.mean(axis=1)
    x = np.column_stack([centre[:, 0], centre[:, 1], np.full(N, HEIGHT_GROUND_M), np.zeros((N, 3))])
    lam = np.full(N, 1e-3); r = resid(x); c = 0.5 * np.sum(r * r, axis=1); J = np.zeros((N, 2 * M, 6)); free = [0, 1, 3, 4]
    for _ in range(iters):
        for k in free:
            dx = x.copy(); dx[:, k] += 1e-5; J[:, :, k] = (resid(dx) - r) / 1e-5
        JTJ = np.einsum("nik,nil->nkl", J, J); g = np.einsum("nik,ni->nk", J, r)
        A = JTJ + lam[:, None, None] * np.eye(6)[None]
        for k in (2, 5): A[:, k, k] = 1.0; g[:, k] = 0.0
        try: step = np.linalg.solve(A, -g[..., None])[..., 0]
        except np.linalg.LinAlgError: step = np.zeros_like(x)
        step[:, 2] = 0.0; step[:, 5] = 0.0
        xn = x + step; rn = resid(xn); cn = 0.5 * np.sum(rn * rn, axis=1); better = cn < c
        x = np.where(better[:, None], xn, x); r = np.where(better[:, None], rn, r); c = np.where(better, cn, c); lam = np.where(better, lam * 0.3, lam * 5.0)
        if np.all(np.abs(step) < 1e-4): break
    n = 2 * M; out = []
    for i in range(N):
        cov = np.linalg.pinv(J[i].T @ J[i]); cov[2, :] = 0.0; cov[:, 2] = 0.0; cov[2, 2] = HEIGHT_SIGMA_GROUND_M ** 2; cov[5, :] = 0.0; cov[:, 5] = 0.0; cov[5, 5] = 1.0
        dof = max(n - 4, 1)
        out.append({"state": x[i], "cost": float(c[i]), "n_eq": n, "dof": n - 4, "rms_per_dof": math.sqrt(2.0 * c[i] / dof), "cov": cov, "height_model": "ground_window"})
    return out


def solve_with_prior(blocks, tx, ue, rx_positions, rate_scale, x_prior, P_prior, iters=25):
    """MAP refit of a carried object from any number of receivers (2026-09-21): Gauss-Newton on the
    whitened measurement residuals PLUS the whitened prior residual L (x - x_prior), L = chol(P_prior^-1).
    This is the Kalman update written as a least-squares problem; with the prior the problem is
    well-posed for a single receiver. Consistency is judged on the measurement part only."""
    ills = np.array([[_ill(b, tx, ue)[0] for b in blocks]]); ill_vels = np.array([[_ill(b, tx, ue)[1] for b in blocks]])
    rxs = np.array([[rx_positions[b["receiver"]] for b in blocks]])
    zr = np.array([[b["range_m"] for b in blocks]]); zv = np.array([[b["rate_mps"] for b in blocks]])
    sr = np.array([[max(b["range_sigma_m"], 1e-6) for b in blocks]]); sv = np.array([[b.get("rate_sigma_mps", rate_scale) for b in blocks]])
    M = len(blocks)
    try:
        L = np.linalg.cholesky(np.linalg.inv(P_prior + 1e-9 * np.eye(6))).T   # L^T L = P^-1
    except np.linalg.LinAlgError:
        return None
    x = np.asarray(x_prior, float).copy()[None, :]; lam = 1e-3
    def resid(st):
        rng, rate = _forward_batch(st, ills, rxs, ill_vels)
        meas = np.concatenate([(rng - zr) / sr, (rate - zv) / sv], axis=1)
        return np.concatenate([meas, (L @ (st[0] - x_prior))[None, :]], axis=1), meas
    r, rm = resid(x); c = 0.5 * float(np.sum(r * r))
    for _ in range(iters):
        dR, dV = _jacobian_batch(x, ills, rxs, ill_vels)
        J = np.concatenate([dR[0] / sr[0][:, None], dV[0] / sv[0][:, None], L], axis=0)
        A = J.T @ J + lam * np.eye(6); g = J.T @ r[0]
        try: step = np.linalg.solve(A, -g)
        except np.linalg.LinAlgError: break
        xn = x + step[None, :]; rn, rmn = resid(xn); cn = 0.5 * float(np.sum(rn * rn))
        if cn < c: x, r, rm, c = xn, rn, rmn, cn; lam *= 0.3
        else: lam *= 5.0
        if np.all(np.abs(step) < 1e-4): break
    dR, dV = _jacobian_batch(x, ills, rxs, ill_vels)
    J = np.concatenate([dR[0] / sr[0][:, None], dV[0] / sv[0][:, None], L], axis=0)
    cov = np.linalg.pinv(J.T @ J)
    n = 2 * M; c_meas = 0.5 * float(np.sum(rm * rm))
    return {"state": x[0], "cost": c_meas, "n_eq": n, "dof": n, "rms_per_dof": math.sqrt(2.0 * c_meas / max(n, 1)), "cov": cov,
            "height_model": "prior", "partial_update": True}


def solve(blocks, tx, ue, rx_positions, rate_scale, seed=None):
    return solve_batch([blocks], tx, ue, rx_positions, rate_scale, seeds=None if seed is None else [seed])[0]


def innovation_z(state, cov, block, tx, ue, rx_positions, rate_scale):
    """Whitened residual of one block including the state's own uncertainty: r / sqrt(1 + h C h^T)."""
    r = np.array(residuals(state, [block], tx, ue, rx_positions, rate_scale))
    if cov is None:
        return r
    H = np.zeros((2, 6)); eps = 1e-4
    for k in range(6):
        dp = state.copy(); dp[k] += eps
        H[:, k] = (np.array(residuals(dp, [block], tx, ue, rx_positions, rate_scale)) - r) / eps
    var = 1.0 + np.einsum("ij,jk,ik->i", H, cov, H)
    return r / np.sqrt(np.maximum(var, 1e-12))


def attach_ue_track(block, track):
    """Causal illuminator state for a UL block from the localiser output: the latest estimate whose
    report time is <= the block time, propagated with its own velocity.  None -> block dropped."""
    t = block["time_s"]; best = None
    for e in track:
        if e["report_time_s"] <= t + 1e-9:
            best = e
        else:
            break
    if best is None:
        return False
    dt = t - best["time_s"]; v = np.asarray(best["velocity_mps"])
    block["ill_pos"] = (np.asarray(best["position_m"]) + v * dt).tolist(); block["ill_vel"] = v.tolist()
    return True


class Stage9Stream:
    """Streaming stage 9 (2026-09-21): step(t, legs_at_t) -> per-CPI record or None. legs_at_t is
    {leg_name: {receiver: [blocks]}} for one CPI time (blocks as produced by stage 8, with 'leg' and
    'range_sigma_raw' set and UE tracks already attached). The body is the offline loop verbatim;
    carried objects, ids and the online sigma are the state."""
    def __init__(self, tx, ue, rx_positions, rate_scale, min_receivers=3):
        self.tx = tx; self.ue = ue; self.rx_positions = rx_positions; self.rate_scale = rate_scale; self.min_receivers = min_receivers
        self.carried = []; self.next_id = [1]
        self.online = OnlineSigma(3.0503913105413107 / math.sqrt(12.0), rate_scale / math.sqrt(12.0)) if ADAPTIVE_SIGMA else None
        self.window = []   # (t, block) unclaimed DL blocks of the last WINDOW_BIRTH_CPIS CPIs (accumulated birth)

    def step(self, t, legs_at_t):
        tx = self.tx; ue = self.ue; rx_positions = self.rx_positions; rate_scale = self.rate_scale; min_receivers = self.min_receivers
        carried = self.carried; next_id = self.next_id; online = self.online
        legs = {name: {t: tab} for name, tab in legs_at_t.items()}
        def cv_predict(obj, t):
            dt = max(t - obj["t"], 0.0); F = np.eye(6); F[:3, 3:] = dt * np.eye(3)
            a2 = (SEEDED_ACCEL0 ** 2) * obj["q_scale"]
            Q = np.zeros((6, 6)); Q[:3, :3] = np.eye(3) * a2 * dt ** 4 / 4; Q[:3, 3:] = Q[3:, :3] = np.eye(3) * a2 * dt ** 3 / 2; Q[3:, 3:] = np.eye(3) * a2 * dt ** 2
            return F @ obj["state"], F @ obj["cov"] @ F.T + Q
        return self._body(t, legs, tx, ue, rx_positions, rate_scale, min_receivers, carried, next_id, online, cv_predict)

    def _body(self, t, legs, tx, ue, rx_positions, rate_scale, min_receivers, carried, next_id, online, cv_predict):
        if online is not None:
            for leg, tab in legs.items():
                for r, bl in tab.get(t, {}).items():
                    sr_on, sv_on = online.sigmas((leg, r))
                    for b in bl:
                        b["range_sigma_m"] = max(b.get("range_sigma_raw", b["range_sigma_m"]), sr_on); b["rate_sigma_mps"] = sv_on
        # (2026-09-19) Seed symmetrically from DL or UL subsets (>= min_receivers, ALL subsets,
        # no early stop), then admit blocks of every other (receiver, leg) slot by the innovation
        # gate.  Seeding only from the largest DL subset forced a false DL block onto receivers
        # where the target is DL-blind (zero-Doppler bin) although UL saw it: person/runner sets
        # with 8-20 m xy error.
        best = None; hypotheses = 0; consistent_fits = []
        seeded_fits = []; consumed_ids = set()
        if SEEDED and carried:
            for obj in sorted(carried, key=lambda o: o["id"]):
                xp, Pp = cv_predict(obj, t)
                obj["_updated"] = False
                if CARRY_RETIRE:
                    # predicted position sigma of the propagated state (largest axis)
                    try:
                        obj["_pred_sigma_m"] = float(np.sqrt(max(np.max(np.diag(Pp)[:2]), 0.0)))
                    except Exception:
                        obj["_pred_sigma_m"] = float("inf")
                claimed = []
                for leg, tab in legs.items():
                    for r in sorted(tab.get(t, {})):
                        cands = []
                        for b in tab[t][r]:
                            bid = (leg, r, round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6))
                            if bid in consumed_ids: continue
                            res = innovation_z(xp, Pp, b, tx, ue, rx_positions, rate_scale)
                            if all(abs(x) <= GATE_SIGMA for x in res): cands.append((float(np.sum(res * res)), b))
                        if cands: claimed.append(min(cands, key=lambda c: c[0])[1])
                n_rx = len({b["receiver"] for b in claimed})
                if TEMPORAL and n_rx < min_receivers and n_rx >= TEMPORAL_MIN_RX and obj.get("born_full"):
                    # Temporal redundancy: MAP update of an object that was BORN over-determined,
                    # from fewer receivers than a stand-alone solve needs. Accepted on the
                    # innovation chi-square of the gated blocks against this object's own
                    # prediction -- a statistic that does not need measurement redundancy.
                    innov = np.concatenate([innovation_z(xp, Pp, b, tx, ue, rx_positions, rate_scale)
                                            for b in claimed])
                    chi2 = float(np.sum(innov * innov))
                    hypotheses += 1
                    if chi2 > chi2_quantile(len(innov)):
                        continue
                    fit = solve_with_prior(claimed, tx, ue, rx_positions, rate_scale, xp, Pp)
                    if fit is None:
                        continue
                    fit["blocks"] = claimed
                    fit["consistent"] = True          # the innovation test above IS the gate
                    fit["temporal_update"] = True
                    fit["innovation_chi2"] = chi2
                    fit["innovation_dof"] = len(innov)
                    obj["state"] = fit["state"].copy(); obj["cov"] = fit["cov"].copy(); obj["t"] = t
                    obj["_updated"] = True
                    fit["seeded_id"] = obj["id"]; seeded_fits.append(fit)
                    for b in claimed:
                        consumed_ids.add((b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)))
                    continue
                if n_rx < min_receivers and MIN_RX_GROUND <= n_rx and obj.get("height_model", "ground") == "ground":
                    # (2026-09-22) Exactly-determined ground continuation: z is not estimated for a
                    # ground object and vz = 0, so 4 unknowns are determined by 2 receivers. The
                    # object already passed a redundant birth; this only keeps it alive through CPIs
                    # where a third receiver is missing, and the blocks still had to pass the
                    # predicted-state innovation gate above.
                    fit = solve_batch([claimed], tx, ue, rx_positions, rate_scale,
                                      seeds=np.array([xp]), ground_only=True)[0]
                    fit["blocks"] = claimed; fit["consistent"] = is_admissible(fit, rx_positions)
                    hypotheses += 1
                    if not fit["consistent"]: continue
                    obj["state"] = fit["state"].copy(); obj["cov"] = fit["cov"].copy(); obj["t"] = t
                    obj["_updated"] = True
                    fit["seeded_id"] = obj["id"]; seeded_fits.append(fit)
                    for b in claimed: consumed_ids.add((b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)))
                    continue
                if n_rx < min_receivers:
                    if not (PARTIAL_UPDATE and n_rx >= PARTIAL_MIN_RX):
                        continue
                    # (2026-09-21) partial update: fewer receivers than a stand-alone solve needs, but the
                    # carried object's predicted state is a valid prior -> MAP update (measured need: the
                    # near-gNB drone was seen by >= 2 receivers in 70 CPIs but by >= 3 in only 18).
                    fit = solve_with_prior(claimed, tx, ue, rx_positions, rate_scale, xp, Pp)
                else:
                    fit = solve(claimed, tx, ue, rx_positions, rate_scale, seed=xp)
                if fit is None: continue
                fit["blocks"] = claimed; fit["consistent"] = is_consistent(fit, online is not None and online.calibrated(claimed))
                hypotheses += 1
                if not fit["consistent"]: continue
                # covariance matching (velocity innovation vs model), as in the tracker
                dt = max(t - obj["t"], 1e-3); dv = fit["state"][3:] - xp[3:]
                q_model = 3.0 * (SEEDED_ACCEL0 ** 2) * dt ** 2 * obj["q_scale"]; q_obs = float(dv @ dv)
                obj["q_scale"] = max(1e-6, (1 - SEEDED_Q_EMA) * obj["q_scale"] + SEEDED_Q_EMA * obj["q_scale"] * (q_obs / max(q_model, 1e-12)))
                obj["state"] = fit["state"].copy(); obj["cov"] = fit["cov"].copy(); obj["t"] = t
                obj["_updated"] = True
                fit["seeded_id"] = obj["id"]; seeded_fits.append(fit)
                for b in claimed: consumed_ids.add((b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)))
            if CARRY_RETIRE:
                for o in carried:
                    o["unsupported"] = 0 if o.get("_updated") else o.get("unsupported", 0) + 1
                carried[:] = [o for o in carried
                              if t - o["t"] <= SEEDED_COAST_S
                              and o.get("unsupported", 0) < CARRY_MAX_UNSUPPORTED
                              and o.get("_pred_sigma_m", 0.0) <= CARRY_MAX_SIGMA_M]
            else:
                carried[:] = [o for o in carried if t - o["t"] <= SEEDED_COAST_S]
        for leg_name, table in legs.items():
            per_rx = {r: [b for b in bl if (leg_name, r, round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) not in consumed_ids] for r, bl in table.get(t, {}).items()} if consumed_ids else table.get(t, {})
            per_rx = {r: bl for r, bl in per_rx.items() if bl}; receivers = sorted(per_rx)
            # Births still require >= min_receivers: an exactly-determined fit has no residual to
            # test, and admitting births on physical admissibility alone was measured to quadruple
            # the false states (26 -> 106 on drone_car_car_det). Two-receiver ground solves are used
            # only to CONTINUE an object that was already established (seeded path above).
            for k in range(len(receivers), min_receivers - 1, -1):
                ground_exact = False
                for subset in itertools.combinations(receivers, k):
                    combos = [list(c) for c in itertools.product(*(per_rx[r] for r in subset))]
                    if not combos:
                        continue
                    hypotheses += len(combos)
                    fits = solve_batch(combos, tx, ue, rx_positions, rate_scale,
                                       ground_only=ground_exact)
                    for combo, fit in zip(combos, fits):
                        blocks = list(combo); used = {(b["receiver"], b["leg"]) for b in blocks}
                        for leg, tab in legs.items():
                            for r in sorted(tab.get(t, {})):
                                if (r, leg) in used:
                                    continue
                                cands = []
                                for b in tab[t][r]:
                                    if (leg, r, round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) in consumed_ids: continue
                                    res = innovation_z(fit["state"], fit["cov"], b, tx, ue, rx_positions, rate_scale)
                                    if all(abs(x) <= GATE_SIGMA for x in res):
                                        cands.append((float(np.sum(res * res)), b))
                                if cands:
                                    blocks.append(min(cands, key=lambda c: c[0])[1])
                        if len(blocks) > len(combo):
                            refit = solve(blocks, tx, ue, rx_positions, rate_scale, seed=fit["state"])
                            if refit is not None:
                                fit = refit
                        fit["blocks"] = blocks
                        # An exactly-determined ground fit that attracted extra blocks became
                        # testable; only a still-redundancy-free one falls back to admissibility.
                        fit["consistent"] = (is_consistent(fit, online is not None and online.calibrated(blocks))
                                             if fit["dof"] > 0
                                             else (ground_exact and is_admissible(fit, rx_positions)))
                        if fit["consistent"]:
                            consistent_fits.append(fit)
                        key = (fit["consistent"], fit["dof"], -fit["rms_per_dof"])
                        if best is None or key > (best["consistent"], best["dof"], -best["rms_per_dof"]):
                            best = fit
        window_fits = []
        if WINDOW_BIRTH:
            # (2026-09-22) accumulated birth: unclaimed DL blocks of this and the previous CPIs, one block per
            # (receiver, CPI), sets spanning >= 3 distinct receivers, solved as one constant-velocity ground
            # state referenced to t. Accepted fits are emitted and carried like any other hypothesis.
            claimed_now = {(b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) for f in seeded_fits + consistent_fits for b in f["blocks"]}
            for r_id, bl in legs.get("dl", {}).get(t, {}).items():
                for b in bl:
                    if (b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) not in claimed_now:
                        self.window.append((t, b))
            self.window = [(tb, b) for tb, b in self.window if t - tb <= WINDOW_BIRTH_CPIS * 0.075 + 1e-6]
            by_rx = {}
            for tb, b in self.window:
                by_rx.setdefault(b["receiver"], []).append((tb, b))
            rxs_avail = [r for r in sorted(by_rx) if by_rx[r]]
            if len(rxs_avail) >= 3:
                # per receiver keep the most recent WINDOW_BIRTH_MAX_PER_RX blocks (bounded combinatorics)
                cand = {r: sorted(by_rx[r], key=lambda x: -x[0])[:WINDOW_BIRTH_MAX_PER_RX] for r in rxs_avail}
                sets = []; times = []
                for subset in itertools.combinations(rxs_avail, 3):
                    for combo in itertools.product(*(cand[r] for r in subset)):
                        if len({tb for tb, _ in combo}) == 1:
                            continue   # same-CPI triples are the ordinary single-CPI path
                        sets.append([b for _, b in combo]); times.append([tb - t for tb, _ in combo])
                if sets:
                    hypotheses += len(sets)
                    fits = solve_window_ground(sets, times, tx, ue, rx_positions, rate_scale)
                    accepted = []
                    for blocks, fit in zip(sets, fits):
                        fit["blocks"] = blocks; fit["consistent"] = is_consistent(fit, False)
                        if not fit["consistent"] or fit["state"][2] < Z_MIN_M: continue
                        if any(np.linalg.norm(fit["state"][:2] - h["state"][:2]) <= 3.0503913105413107 for h in seeded_fits + consistent_fits + accepted): continue
                        accepted.append(fit)
                    accepted.sort(key=lambda f: f["rms_per_dof"])
                    used_ids = set()
                    for f in accepted:
                        ids = {(b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) for b in f["blocks"]}
                        if ids & used_ids: continue
                        used_ids |= ids; f["window_birth"] = True; window_fits.append(f)
                    if used_ids:
                        self.window = [(tb, b) for tb, b in self.window if (b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) not in used_ids]
        if best is None and not seeded_fits and not window_fits:
            return None
        if best is None:
            best = (seeded_fits or window_fits)[0]
        if best is None or (seeded_fits and not best["consistent"]):
            best = seeded_fits[0] if seeded_fits else best
        best_gate = best["consistent"]
        # NOTE (2026-09-19): an explicit z-mirror re-solve (seed 2*h_gnb - z) added as a hypothesis
        # was measured to create a competing consistent root even where z is well observed
        # (drone confirmed precision 100% -> 60%) and did not help the bike (86% -> 84%). Removed.
        # (2026-09-19) Emit EVERY consistent hypothesis (deduplicated within one range cell in
        # position), ranked by dof then residual, so the tracker can associate by continuity.
        # Emitting only the argmax made the choice alternate between comparable hypotheses (true
        # vs multipath-consistent) from CPI to CPI, feeding a parallel ghost track (bike, 18 hits).
        consistent_fits.sort(key=lambda f: (-f["dof"], f["rms_per_dof"]))
        hyps = []
        hyps.extend(seeded_fits)   # carried objects first (already block-exclusive)
        if PARTITION:
            consumed = set()
            order = consistent_fits if PARTITION_RANK == "dof" else sorted(consistent_fits, key=lambda f: (f["rms_per_dof"], -f["dof"]))
            for f in order:
                ids = {(b["leg"], b["receiver"], round(float(b["range_m"]), 6), round(float(b["rate_mps"]), 6)) for b in f["blocks"]}
                if ids & consumed:
                    continue
                consumed |= ids; hyps.append(f)
        else:
            for f in consistent_fits:
                if any(np.linalg.norm(f["state"][:3] - h["state"][:3]) <= 3.0503913105413107 for h in hyps):
                    continue
                hyps.append(f)
        hyps.extend(window_fits)   # accumulated-birth fits (unclaimed blocks only, deduplicated above)
        # physical admissibility of emitted solutions: above ground
        hyps = [f for f in hyps if f["state"][2] >= Z_MIN_M]
        if best["state"][2] < Z_MIN_M:
            if not hyps:
                return None
            best = hyps[0]; best_gate = best["consistent"]
        if SEEDED:
            for f in hyps:
                if "seeded_id" not in f:
                    if SEEDED_REQUIRE_DLUL:
                        legs_seen = {b["leg"] for b in f["blocks"]}
                        if "dl" not in legs_seen or not any(l.startswith("ul") for l in legs_seen):
                            continue   # emitted, but not carried forward
                    elif SEEDED_REQUIRE_DL:
                        if "dl" not in {b["leg"] for b in f["blocks"]}:
                            continue   # UL-only fits are not carried
                    born_full = len({b["receiver"] for b in f["blocks"]}) >= min_receivers and f.get("dof", 0) > 0
                    carried.append({"state": f["state"].copy(), "cov": f["cov"].copy(), "t": t,
                                    "q_scale": 1.0, "id": next_id[0], "born_full": born_full,
                                    "height_model": f.get("height_model", "ground")})
                    f["seeded_id"] = next_id[0]; next_id[0] += 1
        if online is not None:
            # LEAVE-ONE-OUT residuals: each block is compared with the fit made WITHOUT it (batched
            # refit seeded at the full solution).  In-sample residuals are biased low (a fit with
            # dof ~ 0 has zero residual by construction; measured: rate sigma collapsed to 0.01-0.1
            # m/s and runner precision 99% -> 73%); prediction residuals are the honest scale.
            for h in hyps:
                blocks = h["blocks"]
                if len(blocks) < 4:
                    continue
                loo_sets = [[b for j, b in enumerate(blocks) if j != i] for i in range(len(blocks))]
                loo = solve_batch(loo_sets, tx, ue, rx_positions, rate_scale, seeds=[h["state"]] * len(loo_sets))
                for b, f in zip(blocks, loo):
                    ill, ill_v = _ill(b, tx, ue); rng, rate = forward(f["state"], ill, rx_positions[b["receiver"]], ill_v)
                    online.add((b["leg"], b["receiver"]), rng - b["range_m"], rate - b["rate_mps"])
        return ({"time_s": t, "state": best["state"].tolist(), "n_eq": best["n_eq"], "dof": best["dof"], "consistent": best_gate,
                        "covariance": (best["cov"] if best.get("cov") is not None else np.full((6, 6), np.nan)).tolist(),
                        "hypotheses": [{"state": h["state"].tolist(), "covariance": (h["cov"] if h.get("cov") is not None else np.full((6, 6), np.nan)).tolist(),
                                        "n_eq": h["n_eq"], "dof": h["dof"], "rms_per_dof": h["rms_per_dof"], "seeded_id": h.get("seeded_id"), "height_model": h.get("height_model"),
                                        # block identities (2026-09-20): lets the tracker apply measurement exclusivity
                                        "blocks": [[b["leg"], int(b["receiver"]), round(float(b["range_m"]), 3), round(float(b["rate_mps"]), 3)] for b in h["blocks"]]} for h in hyps],
                        "rms_per_dof": best["rms_per_dof"], "hypotheses_evaluated": hypotheses,
                        "blocks": [{"leg": b["leg"], "receiver": b["receiver"], "range_m": b["range_m"], "rate_mps": b["rate_mps"], "range_sigma_m": b["range_sigma_m"], "rate_sigma_mps": b.get("rate_sigma_mps", rate_scale)} for b in best["blocks"]]})


def run(dl_path, ul_path, tx, ue, rx_positions, rate_scale, min_receivers=3, use_ul=True, ul_legs=None):
    """ul_legs: optional list of (leg_name, blocks_path, ue_track_or_None).  A UE track is the
    localiser JSONL (per-CPI position/velocity, no mobility prior); None uses the fixed `ue`.
    Offline: identical to Stage9Stream.step() per CPI time."""
    def load(path, leg, track=None):
        out = {}
        for line in open(path):
            if line.strip():
                o = json.loads(line); o["leg"] = leg; o["range_sigma_raw"] = o["range_sigma_m"]
                if track is not None and not attach_ue_track(o, track):
                    continue
                out.setdefault(round(o["time_s"], 4), {}).setdefault(o["receiver"], []).append(o)
        return out
    dl = load(dl_path, "dl"); ul = load(ul_path, "ul") if (use_ul and ul_path) else {}
    legs = {"dl": dl, "ul": ul}
    for name, path, track in (ul_legs or []):
        legs[name] = load(path, name, track)
    stream = Stage9Stream(tx, ue, rx_positions, rate_scale, min_receivers)
    results = []
    for t in sorted(set().union(*[set(v) for v in legs.values()])):
        rec = stream.step(t, {name: tab.get(t, {}) for name, tab in legs.items()})
        if rec is not None: results.append(rec)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dl", required=True); ap.add_argument("--ul")
    ap.add_argument("--tx", nargs=3, type=float, required=True); ap.add_argument("--ue", nargs=3, type=float)
    ap.add_argument("--rx", nargs=3, type=float, action="append", required=True)
    ap.add_argument("--rate-res-mps", type=float, required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = run(a.dl, a.ul, np.array(a.tx), np.array(a.ue) if a.ue else None, [np.array(r) for r in a.rx], a.rate_res_mps, use_ul=bool(a.ul))
    with open(a.out, "w") as f:
        for r in out: f.write(json.dumps(r) + "\n")
    print("wrote", a.out, len(out))


if __name__ == "__main__":
    main()
