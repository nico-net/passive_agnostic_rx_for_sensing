#!/usr/bin/env python3
"""Moving-UE localiser (stage-9 helper, OUR ADAPTATION): joint position + velocity of the UL
illuminator from the per-receiver UL direct-path DELAY and DIRECT-PATH DOPPLER (DFS) measured
by the receivers over a sliding window of CPIs.  Only inter-receiver structure is informative:
the UE transmit time (offset + drift) and the UE-vs-X410 carrier offset are unknown common terms
and are estimated jointly, never assumed zero.

Model over a window of K CPIs (times t_k, window-centred), constant velocity:
  d_ik = |p0 + v t_k - r_i| + o0 + o1 t_k              (direct-path range, m)
  f_ik = u_ik . v + c                                  (direct-path range rate, m/s; u_ik unit
                                                        vector from receiver i to the UE)
Unknowns x = (p0[3], vx, vy, o0, o1, c); vz = 0 is declared for a ground UE.
Infrastructure inputs only: receiver positions, resolution cells.  No ground truth is read."""
from __future__ import annotations
import argparse, json, os
import numpy as np


def _env_float(name, default):
    import os
    v = os.environ.get(name)
    return default if v in (None, "") else float(v)
from ue_localiser import solve_ue


def predict(x, rx, t):
    """rx (4,3), t (K,) -> d (K,4), f (K,4)."""
    p = x[:3][None, :] + np.stack([x[3] * t, x[4] * t, np.zeros_like(t)], axis=1)     # (K,3)
    rel = p[:, None, :] - rx[None, :, :]                                              # (K,4,3)
    dist = np.linalg.norm(rel, axis=2)
    u = rel / dist[..., None]
    d = dist + x[5] + x[6] * t[:, None]
    f = u[..., 0] * x[3] + u[..., 1] * x[4] + x[7]
    return d, f


# (2026-09-20) Physical admissibility of the TDOA solution (declared, infrastructure-class knowledge,
# no target information): a handheld/vehicle UE antenna sits between UE_Z_MIN and UE_Z_MAX above
# ground.  With four receivers and an unknown transmit offset the delay-only problem has a
# far-field branch (measured at the 33 m-gNB set: (-15, 87, -16) m with offset -85 m, rms 0.02 --
# the same residual as the true solution); solutions outside the admissible height band are
# discarded before ranking.  UE_Z_BOUND=0 disables the check.
UE_Z_MIN, UE_Z_MAX = 0.0, 3.0
UE_Z_BOUND = os.environ.get("UE_Z_BOUND", "1") == "1"
# UE antenna height is NOT observable with a 3 m receiver height spread at 20+ m (sigma_z 3.6 m):
# left free it absorbs the per-receiver delay biases and pushes the horizontal fix by ~10 m
# (measured, 33 m-gNB set: z clamped at 0, y error 8-12 m).  It is therefore fixed at the declared
# handheld antenna height (1.5 m, the same infrastructure-class value the scene declares for UEs);
# UE_Z_FIXED="" frees it again.
UE_Z_FIXED = float(os.environ.get("UE_Z_FIXED", "1.5")) if os.environ.get("UE_Z_FIXED", "1.5") != "" else None


# Declared common-offset prior (2026-09-21). With the receiver's UL window-advance sidecar the UE->RX
# delays are ABSOLUTE up to the UE's commanded timing advance N_TA (a per-session constant). Over
# RFsim transport N_TA is 0 (no propagation delay on the serving link), which is radio-transport
# metadata, not target truth; the prior sigma is one range cell (declared numerical regulariser).
# OTA: N_TA is carried in the RAR / TA MAC CE that the passive receiver decodes -- decode it and
# set the prior from it; never leave the offset free when absolute delays are available (the
# 10 m aperture cannot separate range from a common offset: measured 24-40 m errors).
OFFSET_PRIOR_M = _env_float("UE_OFFSET_PRIOR_M", None)
ANALYTIC_JAC = os.environ.get("UE_ANALYTIC_JAC", "1") == "1"    # exact derivatives (2026-09-21, real-time); 0 = finite differences
WARM_ONLY = os.environ.get("UE_WARM_ONLY", "1") == "1"          # single warm-start seed after the first solution (real-time)
OFFSET_PRIOR_SIGMA_M = _env_float("UE_OFFSET_PRIOR_SIGMA_M", 3.0503913105413107)


def _proj_state(v):
    v = np.array(v, float)
    if UE_Z_FIXED is not None: v[2] = UE_Z_FIXED
    elif UE_Z_BOUND: v[2] = min(max(v[2], UE_Z_MIN), UE_Z_MAX)
    return v


def solve_window(rx, t, d_meas, f_meas, sd, sf, x0=None, iters=60):
    """Levenberg-damped Gauss-Newton on the whitened residual.  Missing measurements are NaN."""
    rx = np.asarray(rx, float); t = np.asarray(t, float)
    d_meas = np.asarray(d_meas, float); f_meas = np.asarray(f_meas, float)
    sd = np.broadcast_to(np.asarray(sd, float), d_meas.shape); sf = np.broadcast_to(np.asarray(sf, float), f_meas.shape)
    md, mf = np.isfinite(d_meas), np.isfinite(f_meas)

    def resid_raw(x):
        d, f = predict(x, rx, t)
        parts = [((d - d_meas) / sd)[md], ((f - f_meas) / sf)[mf]]
        if OFFSET_PRIOR_M is not None:
            parts.append(np.array([(x[5] - OFFSET_PRIOR_M) / OFFSET_PRIOR_SIGMA_M, x[6] / OFFSET_PRIOR_SIGMA_M]))
        return np.concatenate(parts)

    # (2026-09-20) Huber-robust whitened residual (conventional k = 3 sigma, declared).  A
    # redescending (Tukey) loss was tried and REJECTED: with a far seed it zeroes every residual and
    # "converges" anywhere (synthetic static case 1.2 m -> 17 m).  Gross wrong-peak rows are removed
    # at the data level (per-receiver window MAD gate in run()) before the solve.
    HUBER_K = 3.0
    def resid(x):
        r = resid_raw(x)
        a = np.abs(r)
        w = np.where(a > HUBER_K, np.sqrt(HUBER_K * (2 * np.maximum(a, HUBER_K) - HUBER_K)) / np.maximum(a, 1e-12), 1.0)
        return r * w

    def jac_raw(x):
        """Analytic Jacobian of resid_raw (2026-09-21, real-time): d = |p+vt-rx| + off + drift t;
        f = u.v_xy + cfo, u the unit vector from the receiver to the UE at time t."""
        p = x[:3][None, :] + np.stack([x[3] * t, x[4] * t, np.zeros_like(t)], axis=1)
        rel = p[:, None, :] - rx[None, :, :]; dist = np.linalg.norm(rel, axis=2); u = rel / dist[..., None]   # (K,4,3)
        K, n = dist.shape; Jd = np.zeros((K, n, 8)); Jf = np.zeros((K, n, 8))
        Jd[..., :3] = u; Jd[..., 3] = u[..., 0] * t[:, None]; Jd[..., 4] = u[..., 1] * t[:, None]; Jd[..., 5] = 1.0; Jd[..., 6] = t[:, None]
        vxy = np.array([x[3], x[4], 0.0]); udotv = u @ vxy                      # (K,4)
        dfdp = (vxy[None, None, :] - udotv[..., None] * u) / dist[..., None]    # (K,4,3)
        Jf[..., :3] = dfdp; Jf[..., 3] = u[..., 0] + dfdp[..., 0] * t[:, None]; Jf[..., 4] = u[..., 1] + dfdp[..., 1] * t[:, None]; Jf[..., 7] = 1.0
        parts = [(Jd / sd[..., None])[md], (Jf / sf[..., None])[mf]]
        if OFFSET_PRIOR_M is not None:
            pr = np.zeros((2, 8)); pr[0, 5] = 1.0 / OFFSET_PRIOR_SIGMA_M; pr[1, 6] = 1.0 / OFFSET_PRIOR_SIGMA_M; parts.append(pr)
        return np.concatenate(parts, axis=0)

    def descend(x):
        x = np.array(x, float); lam = 1e-3; r = resid(x); c = 0.5 * r @ r; J = None
        for _ in range(iters):
            if ANALYTIC_JAC:
                rr = resid_raw(x); a = np.abs(rr)
                w = np.where(a > HUBER_K, np.sqrt(HUBER_K * (2 * np.maximum(a, HUBER_K) - HUBER_K)) / np.maximum(a, 1e-12), 1.0)
                J = jac_raw(x) * w[:, None]          # IRLS form: weights held fixed within the step
            else:
                J = np.zeros((r.size, 8)); eps = 1e-6
                for k in range(8):
                    dx = x.copy(); dx[k] += eps; J[:, k] = (resid(dx) - r) / eps
            H = J.T @ J
            step = np.linalg.solve(H + lam * np.diag(np.diag(H) + 1e-12), -J.T @ r)
            if UE_Z_FIXED is not None:
                step[2] = 0.0
            xn = x + step
            if UE_Z_FIXED is not None:
                xn[2] = UE_Z_FIXED
            elif UE_Z_BOUND:   # projected step: the antenna height stays inside the admissible band
                xn[2] = min(max(xn[2], UE_Z_MIN), UE_Z_MAX)
            rn = resid(xn); cn = 0.5 * rn @ rn
            if cn < c:
                x, r, c, lam = xn, rn, cn, lam * 0.3
            else:
                lam *= 5.0
            if np.abs(step).max() < 1e-7:
                break
        return x, r, c, J

    seeds = []
    if x0 is not None:
        seeds.append(np.array(x0, float))
        if WARM_ONLY:
            # (2026-09-21, real-time) warm start only: the previous window solution is the single seed;
            # the TDOA seed + displaced ring are used on cold start or when no previous solution exists.
            cand = descend(_proj_state(np.array(x0, float)))
            if UE_Z_BOUND and not (UE_Z_MIN <= cand[0][2] <= UE_Z_MAX): cand = None
            if cand is not None:
                x, r, c, J = cand
                cov = np.linalg.pinv(J.T @ J); dof = r.size - 8
                return {"position_m": x[:3].tolist(), "velocity_mps": [x[3], x[4], 0.0], "offset_m": float(x[5]),
                        "offset_drift_mps": float(x[6]), "cfo_mps": float(x[7]), "cost": float(c), "dof": int(dof),
                        "rms_whitened": float(np.sqrt(2.0 * c / max(r.size, 1))),
                        "position_sigma_m": np.sqrt(np.diag(cov)[:3]).tolist(), "velocity_sigma_mps": np.sqrt(np.diag(cov)[3:5]).tolist(),
                        "ambiguous": False, "alternative": None, "x": x}
    def _proj(v):
        return _proj_state(v)
    # TDOA solve on the window-median delays (static assumption) plus a declared ring of
    # displaced seeds (the delay-only problem is near-degenerate along the receiver-square
    # normal, so a single seed can settle in the wrong branch); lowest whitened cost wins.
    seed = solve_ue(rx, np.nanmedian(d_meas, axis=0), np.nanmedian(sd, axis=0))
    base = np.array(seed["position_m"] + [0.0, 0.0, seed["offset_m"], 0.0, float(np.nanmedian(f_meas))])
    seeds.append(_proj(base))
    for dx, dy in ((10, 0), (-10, 0), (0, 10), (0, -10), (10, 10), (-10, -10), (10, -10), (-10, 10)):
        s2 = base.copy(); s2[0] += dx; s2[1] += dy; seeds.append(_proj(s2))
    seeds = [_proj(v) for v in seeds]
    minima = []
    for s0 in seeds:
        try:
            cand = descend(s0)
        except np.linalg.LinAlgError:
            continue
        minima.append(cand)
    if UE_Z_BOUND:
        admissible = [m for m in minima if UE_Z_MIN <= m[0][2] <= UE_Z_MAX]
        if admissible:
            minima = admissible
    minima.sort(key=lambda m: m[2])
    x, r, c, J = minima[0]
    # Ambiguity flag: a distinct minimum (> 5 m away) whose chi-square (2*cost) is within the
    # conventional 3-sigma-equivalent gate (9) of the best is reported as an alternative; the
    # geometry alone then cannot decide and a downstream prior (TA, served-area bound) is needed.
    alternative = None
    for m in minima[1:]:
        if np.linalg.norm(m[0][:3] - x[:3]) > 5.0 and 2.0 * (m[2] - c) < 9.0:
            alternative = {"position_m": m[0][:3].tolist(), "velocity_mps": [m[0][3], m[0][4], 0.0], "delta_chi2": float(2.0 * (m[2] - c))}
            break
    cov = np.linalg.pinv(J.T @ J)
    dof = r.size - 8
    return {"position_m": x[:3].tolist(), "velocity_mps": [x[3], x[4], 0.0], "offset_m": float(x[5]),
            "offset_drift_mps": float(x[6]), "cfo_mps": float(x[7]), "cost": float(c), "dof": int(dof),
            "rms_whitened": float(np.sqrt(2.0 * c / max(r.size, 1))),
            "position_sigma_m": np.sqrt(np.diag(cov)[:3]).tolist(), "velocity_sigma_mps": np.sqrt(np.diag(cov)[3:5]).tolist(),
            "ambiguous": alternative is not None, "alternative": alternative, "x": x}


class LocaliserStream:
    """Streaming UE localiser (2026-09-21): feed(report) -> window estimate or None. The body is the
    offline per-report loop verbatim; the window buffers and the previous solution are the state."""
    def __init__(self, rx_positions, range_res_m, rate_res_mps, window_cpis, ul_session=None, tx_position=None,
                 max_range_m=312.2838104166667, ul_advance=None, samples_per_bin=4096.0 / 3276.0):
        self.rx = np.asarray(rx_positions, float); self.n = len(self.rx)
        self.args = (range_res_m, rate_res_mps, window_cpis, ul_session, tx_position, max_range_m, ul_advance, samples_per_bin)
        self.T, self.D, self.F, self.SD, self.SF = [], [], [], [], []; self.x_prev = None

    def _save(self, x_prev):
        self.x_prev = x_prev

    def feed(self, r):
        rx = self.rx; n = self.n
        range_res_m, rate_res_mps, window_cpis, ul_session, tx_position, max_range_m, ul_advance, samples_per_bin = self.args
        T, D, F, SD, SF = self.T, self.D, self.F, self.SD, self.SF; x_prev = self.x_prev; out = []
        t = r["midpoint_air_time_s"]; d = np.full(n, np.nan); f = np.full(n, np.nan); sd = np.full(n, range_res_m / np.sqrt(12.0)); sf = np.full(n, rate_res_mps / np.sqrt(12.0))
        if ul_session is None:
            blocks = [(sr.get("uplink_dtd_dfs") or {}) for sr in (r.get("spatial_receivers") or [])]
        else:
            sess = [x for x in (r.get("uplink_sessions") or []) if x.get("pusch_session_id") == ul_session]
            blocks = list(sess[0]["receivers"]) if sess else []
        # OAI real captures (2026-09-21): the passive PUSCH path re-places its FFT window per grant
        # on the measured direct path, so direct_reference.delay_bins is only the residual (~0).
        # With the receiver's own window-advance sidecar (per slot: nominal N_TA_offset advance,
        # applied advance), the absolute direct-path delay relative to the DL frame origin is
        # (nominal - applied) samples + residual; the UE's commanded N_TA is a per-session constant
        # absorbed by the model's common offset. Median over the CPI's slots; skipped if none.
        advance_bins = 0.0
        if ul_advance is not None:
            s0 = int(round(r["midpoint_air_time_s"] / 0.0005 - 75)); s1 = s0 + 150
            seg = ul_advance[max(s0, 0):max(s1, 0)]
            ok = np.isfinite(seg[:, 1])
            if not ok.any():
                T.append(t); D.append(d); F.append(f); SD.append(sd); SF.append(sf); self._save(x_prev); return None
            advance_bins = float(np.median(seg[ok, 0] - seg[ok, 1])) / samples_per_bin
        for i, ul in enumerate(blocks[:n]):
            ref = ul.get("direct_reference") or {}
            if not ul.get("valid") or ref.get("delay_bins") is None:
                continue
            delay_bins = ref["delay_bins"] + advance_bins
            # Per-receiver chain calibration from the DL direct path (infrastructure metadata only):
            # the gNB->RX_i baseline is surveyed, so DL los_bins - baseline_bins is this receiver's
            # STO + common clock offset/drift for the same CPI.  Subtracting it from the UL delay
            # removes the per-channel STO (up to 4 samples = 10 m here); what remains is
            # |UE - RX_i| + a common offset, which is the model.  Without it a 4-sample STO spread
            # broke the solve (28 m error on the static UE, car scene, 2026-09-19).
            if tx_position is not None:
                dl_sync = ((r.get("spatial_receivers") or [{}] * n)[i].get("sync") or {})
                if dl_sync.get("los_bins") is None or not np.isfinite(dl_sync["los_bins"]):
                    continue
                delay_bins = delay_bins - dl_sync["los_bins"] + np.linalg.norm(np.asarray(tx_position) - rx[i]) / range_res_m
                # physical support: a calibrated UE->RX delay must lie in (0, max_range]; the UL
                # direct-path search occasionally locks a pre-LOS artefact (negative delay) and
                # such rows are dropped, not fitted.
                if not (0.0 < delay_bins * range_res_m <= max_range_m):
                    continue
            d[i] = delay_bins * range_res_m
            sd[i] = max(ref.get("delay_standard_error_bins", 0.0) * range_res_m, range_res_m / np.sqrt(12.0))
            if ref.get("range_rate_valid") and ref.get("range_rate_mps") is not None:
                f[i] = ref["range_rate_mps"]
                sf[i] = max(np.sqrt(max(ref.get("range_rate_variance_mps2", 0.0), 0.0)), rate_res_mps / np.sqrt(12.0))
        T.append(t); D.append(d); F.append(f); SD.append(sd); SF.append(sf)
        if len(T) < 3:
            self._save(x_prev); return None
        k = slice(-window_cpis, None)
        tt = np.array(T[k]); tc = tt.mean()
        Dw = np.array(D[k]); Fw = np.array(F[k])
        # per-receiver window outlier gate (declared 5 x MAD, floored at one range cell): wrong-peak
        # direct-path rows (measured +-100..400 m on one receiver at the 33 m-gNB set) are removed
        # before the solve; for a moving UE the delay drifts by < 1 cell over the window
        for i in range(n):
            col = Dw[:, i]; ok = np.isfinite(col)
            if ok.sum() >= 5:
                med = np.median(col[ok]); mad = 1.4826 * np.median(np.abs(col[ok] - med))
                gate = 5.0 * max(mad, range_res_m)
                bad = ok & (np.abs(col - med) > gate); Dw[bad, i] = np.nan; Fw[bad, i] = np.nan
        if np.isfinite(Dw).sum(axis=0).min() < 2:
            self._save(x_prev); return None
        try:
            est = solve_window(rx, tt - tc, Dw, Fw, np.array(SD[k]), np.array(SF[k]), x0=x_prev)
        except Exception:
            self._save(x_prev); return None
        x_prev = est.pop("x")
        est["time_s"] = float(tc); est["report_time_s"] = t; est["cpis_used"] = int(len(T[k])); est["ul_session"] = ul_session
        out.append(est)
        self._save(x_prev)
        return out[-1] if out else None


def run(report_path, rx_positions, range_res_m, rate_res_mps, window_cpis, ul_session=None, tx_position=None,
        max_range_m=312.2838104166667, ul_advance=None, samples_per_bin=4096.0 / 3276.0):
    """Offline: identical to feeding LocaliserStream one report at a time."""
    reps = [json.loads(l) for l in open(report_path) if l.strip()]
    stream = LocaliserStream(rx_positions, range_res_m, rate_res_mps, window_cpis, ul_session, tx_position, max_range_m, ul_advance, samples_per_bin)
    out = []
    for r in reps:
        est = stream.feed(r)
        if est is not None: out.append(est)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True); ap.add_argument("--rx", nargs=3, type=float, action="append", required=True)
    ap.add_argument("--range-res-m", type=float, required=True); ap.add_argument("--rate-res-mps", type=float, required=True)
    ap.add_argument("--window-cpis", type=int, default=60); ap.add_argument("--ul-session", type=int, default=None); ap.add_argument("--out", required=True)
    ap.add_argument("--tx", nargs=3, type=float, default=None, help="gNB position: enables per-receiver STO calibration from the DL direct path")
    ap.add_argument("--ul-advance", default=None, help="per-slot UL window advance .npy [nominal, applied, refined] from the OAI passive receiver sidecar")
    ap.add_argument("--max-range-m", type=float, default=312.2838104166667)
    a = ap.parse_args()
    adv = np.load(a.ul_advance) if a.ul_advance else None
    out = run(a.report, a.rx, a.range_res_m, a.rate_res_mps, a.window_cpis, a.ul_session, tx_position=a.tx, max_range_m=a.max_range_m, ul_advance=adv)
    with open(a.out, "w") as f:
        for r in out: f.write(json.dumps(r) + "\n")
    print("wrote", a.out, len(out))


if __name__ == "__main__":
    main()
