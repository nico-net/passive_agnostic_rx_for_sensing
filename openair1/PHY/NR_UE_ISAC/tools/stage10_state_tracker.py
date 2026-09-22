#!/usr/bin/env python3
"""Stage 10 (B): 3-D tracker on stage-9 fused states.

Input: stage-9 JSONL (per CPI: 6-D state, whitened Gauss-Newton covariance, consistency flag).
Model: constant-velocity Kalman filter in 3-D.  Every constant is a declared physical/conventional
quantity, none fitted:
  ACCEL_SIGMA_MPS2   2.0   manoeuvre bound for pedestrian/bike/drone-class objects (process noise)
  GATE_SIGMA         2.0   conventional 2-sigma innovation gate (same as stages 8/9)
  CONFIRM_HITS       3     consecutive gated measurements to confirm a track (M-of-M birth)
  MAX_COAST_S        2.0   maximum propagation without measurement (same as the C++ tracker);
                           coast credit is additionally bounded by the track's observed lifetime
  COV_FLOOR          resolution-cell floor per axis (range cell / sqrt(12)) on the measurement cov
Only 'consistent' stage-9 states update a track; inconsistent ones coast it.  Ground truth is not read.
"""
from __future__ import annotations
import argparse, json, math
import numpy as np

# Process noise is ADAPTIVE (covariance matching, Myers-Tapley): each track scales its own Q from
# the running mean of its state corrections K*nu vs the model's predicted Q.  The only declared
# quantity is the start value, taken from the surveillance speed bound (a track may change its
# whole speed budget within 1 s) so that no manoeuvre sigma is chosen by hand.
ACCEL_SIGMA_MPS2 = 50.0        # initial value only = SURV_SPEED_MPS / 1 s; adapted per track
Q_EMA = 0.1                    # ~10-CPI memory, same horizon as the NIS estimate
Z_RESOLVED_SIGMA_M = 3.0503913105413107   # option (b): z is reported only when sigma_z <= one range cell
GATE_SIGMA = 2.0
CONFIRM_HITS = 3          # retained only as the minimum evidence before scoring can confirm
MAX_COAST_S = 2.0
# --- track scoring (SPRT on the log-likelihood ratio), all declared -----------------------
SPRT_ALPHA = 0.01         # false-confirmation error (conventional)
SPRT_BETA = 0.10          # missed-confirmation error (conventional)
PD_PRIOR = 0.9            # prior detection probability; replaced online by the confirmed-track hit ratio
SURV_RANGE_M = 312.2838104166667; SURV_HEIGHT_M = 30.0; SURV_SPEED_MPS = 50.0   # declared surveillance bounds
MEAS_VOLUME = (math.pi * SURV_RANGE_M ** 2 * SURV_HEIGHT_M) * (2 * SURV_SPEED_MPS) ** 3    # 6-D measurement volume
T_CONFIRM = math.log((1 - SPRT_BETA) / SPRT_ALPHA)
T_DELETE = math.log(SPRT_BETA / (1 - SPRT_ALPHA))
RANGE_CELL_M = 3.0503913105413107
RATE_CELL_MPS = 1.1422492665493145
CPI_S = 0.075
import os as _os
SMOOTH_LAG_CPIS = int(_os.environ.get("STAGE10_SMOOTH_LAG_CPIS", "10"))   # DEFAULT ON (user 2026-09-20: no noisy oscillations); 1 = off   # fixed-lag RTS; 1 = off (frozen 2026-09-19). 2026-09-20 user: run with the smoother on (declared lag 10 CPIs = 0.75 s) on top of the 5-CPI look-ahead
TRACK_MERGE = False
import os as _os2
LOOKAHEAD_CPIS = int(_os2.environ.get("STAGE10_LOOKAHEAD_CPIS", "5"))        # declared tracker latency: each measurement is conditioned on the next 5 CPIs (robust fit)


def cv_matrices(dt):
    F = np.eye(6); F[:3, 3:] = dt * np.eye(3)
    q = ACCEL_SIGMA_MPS2 ** 2
    Q = np.zeros((6, 6))
    Q[:3, :3] = q * dt ** 4 / 4 * np.eye(3); Q[:3, 3:] = q * dt ** 3 / 2 * np.eye(3)
    Q[3:, :3] = q * dt ** 3 / 2 * np.eye(3); Q[3:, 3:] = q * dt ** 2 * np.eye(3)
    return F, Q


# 2026-09-20 adaptive reported covariance (user requirement: sigma must calibrate online, no fixed
# floors).  Three GT-free parts, each switchable for A/B (all default ON after validation):
#  COV_UNITWEIGHT : scale each stage-9 covariance by its own whitened residual variance
#                   max(1, rms_per_dof^2) (unit-weight variance; the GN covariance is a CRLB that
#                   assumes the model fits with rms 1);
#  MERGE_FIRST    : moment-match the z-twin roots BEFORE the look-ahead picks its top hypothesis
#                   (previously the mixture was computed after, i.e. never used with look-ahead);
#  NIS_PER_AXIS   : the online NIS inflation of R is applied per axis, so a z-only inconsistency
#                   widens sigma_z without loosening xy.
COV_UNITWEIGHT = _os.environ.get("STAGE10_COV_UNITWEIGHT", "1") == "1"
MERGE_FIRST = _os.environ.get("STAGE10_MERGE_FIRST", "0") == "1"   # REJECTED by A-B 2026-09-20 (frozen drone 100% -> 54%: the mixture pulls the wrong root in); switchable only
NIS_PER_AXIS = _os.environ.get("STAGE10_NIS_PER_AXIS", "1") == "1"


def scaled_hyp(h):
    """Unit-weight-variance scaling of one hypothesis' covariance (no-op when the fit has rms<=1)."""
    if not COV_UNITWEIGHT or h.get("rms_per_dof") is None:
        return h
    k = max(1.0, float(h["rms_per_dof"]) ** 2)
    return dict(h, covariance=(np.array(h["covariance"], dtype=float) * k).tolist())


def measurement_cov(cov):
    R = np.array(cov, dtype=float)
    if not np.all(np.isfinite(R)):
        R = np.diag([RANGE_CELL_M ** 2] * 3 + [RATE_CELL_MPS ** 2] * 3)
    floor = np.diag([RANGE_CELL_M ** 2 / 12] * 3 + [RATE_CELL_MPS ** 2 / 12] * 3)
    return R + floor



def merge_ambiguous(hyps):
    """Moment-match hypotheses that are the same object seen through the weak-z two-root
    ambiguity: cluster on xy within 2 range cells (resolution), weight by likelihood
    exp(-dof*rms^2/2), and return one measurement per cluster whose covariance includes the
    spread between the roots.  Measured before: roots at z=+5 and z=-2.5 (true 1 m) each with
    reported sigma_z ~1 m alternated as the top hypothesis and split the bike track in two."""
    if len(hyps) <= 1:
        return hyps
    items = [(np.array(h["state"]), np.array(h["covariance"], dtype=float), math.exp(-0.5 * h.get("dof", 1) * h.get("rms_per_dof", 0.0) ** 2), h) for h in hyps]
    clusters = []
    for st, cov, w, h in items:
        for cl in clusters:
            if np.linalg.norm(st[:2] - cl[0][0][:2]) <= 2 * RANGE_CELL_M:
                cl.append((st, cov, w, h)); break
        else:
            clusters.append([(st, cov, w, h)])
    out = []
    for cl in clusters:
        ws = np.array([c[2] for c in cl]); ws = ws / ws.sum() if ws.sum() > 0 else np.full(len(cl), 1.0 / len(cl))
        mean = sum(w * c[0] for w, c in zip(ws, cl))
        cov = sum(w * (c[1] + np.outer(c[0] - mean, c[0] - mean)) for w, c in zip(ws, cl))
        lead = cl[0][3]
        out.append({"state": mean.tolist(), "covariance": cov.tolist(), "n_eq": lead.get("n_eq"), "dof": lead.get("dof"), "rms_per_dof": lead.get("rms_per_dof"), "members": len(cl)})
    return out



def _lookahead_point(m):
    hyps = m.get("hypotheses")
    h = (hyps if hyps else ([{"state": m["state"], "covariance": m["covariance"], "rms_per_dof": m.get("rms_per_dof")}] if m.get("consistent", True) else []))
    h = [scaled_hyp(x) for x in h]
    h = (merge_ambiguous(h) if MERGE_FIRST else h)[:1]
    return (m["time_s"], np.array(h[0]["state"]) if h else None, measurement_cov(h[0]["covariance"]) if h else None)


def condition_one(meas_window):
    """Condition the FIRST measurement of meas_window (= [i, i+LOOKAHEAD] in the offline form) --
    robust per-axis linear fit over the window; sigma = max(reported, empirical MAD scatter)."""
    pts = [_lookahead_point(m) for m in meas_window]
    t, z, R = pts[0]
    if z is None:
        return dict(meas_window[0], hypotheses=[])
    win = [(tt, zz) for tt, zz, _ in pts if zz is not None]
    if len(win) >= 3:
        T = np.array([w[0] for w in win]) - t; Z = np.array([w[1] for w in win])
        A = np.column_stack([np.ones_like(T), T])
        # ROBUST per-axis linear fit: iteratively drop points beyond GATE_SIGMA x MAD of the
        # residual (conventional 2-sigma), so a single wrong-but-consistent stage-9 solution
        # in the window neither moves the conditioned measurement nor inflates its sigma.
        # (Plain least squares let such outliers pull the track by 5-10 m.)
        w = np.ones(len(T))
        for _ in range(3):
            Aw = A * w[:, None]; coef, *_ = np.linalg.lstsq(Aw, Z * w[:, None], rcond=None)
            resid = Z - A @ coef
            mad = np.median(np.abs(resid - np.median(resid, axis=0)), axis=0) * 1.4826 + 1e-9
            score = np.max(np.abs(resid) / mad, axis=1)
            w_new = (score <= GATE_SIGMA).astype(float)
            if w_new.sum() < 3 or np.array_equal(w_new, w): break
            w = w_new
        fit_t = coef[0]; resid = (Z - A @ coef)[w > 0]
        mad = np.median(np.abs(resid - np.median(resid, axis=0)), axis=0) * 1.4826
        emp = np.diag(mad ** 2)
        R2 = R.copy(); d = np.diag_indices(6); R2[d] = np.maximum(R[d], emp[d])
        zc = fit_t
    else:
        zc, R2 = z, R
    return dict(meas_window[0], hypotheses=[{"state": zc.tolist(), "covariance": R2.tolist()}])


def condition_lookahead(meas):
    """Look-ahead measurement conditioning (declared lag LOOKAHEAD_CPIS).  For the measurement
    at index i use the window [i, i+LOOKAHEAD]: robust linear fit per state axis over time; the
    conditioned measurement is the fit at t_i and its covariance is the larger of the reported
    one and the EMPIRICAL residual scatter about the fit (MAD-based, per axis).  A z that flips
    between mirror roots inside the window shows up as a large empirical sigma_z, which the
    per-CPI linearised covariance cannot see.  Only top hypotheses of consistent CPIs are used."""
    return [condition_one(meas[i:i + LOOKAHEAD_CPIS + 1]) for i in range(len(meas))]


class Track:
    _next = 1
    def __init__(self, t, z, R):
        self.id = Track._next; Track._next += 1
        self.t = t; self.x = z.copy(); self.P = R.copy(); self.hits = 1; self.misses = 0; self.confirmed = False; self.last_update = t
        self.score = 0.0; self.opportunities = 0
        self.hist = [(t, z.copy(), R.copy(), z.copy(), R.copy(), np.eye(6))]   # (t, x_pred, P_pred, x_filt, P_filt, F)
        self.nis = 1.0   # running normalised innovation (chi2/dim); >1 => reported covariance too small
        self.nis_axis = np.ones(6)   # per-axis version (innovation_i^2 / S_ii), EMA
        self.q_scale = 1.0   # adaptive process-noise scale (covariance matching)

    def predict(self, t):
        dt = max(t - self.t, 0.0); F, Q = cv_matrices(dt); Q = Q * self.q_scale
        self.x = F @ self.x; self.P = F @ self.P @ F.T + Q; self.t = t
        # bound the adaptive Q from below with the measurement floor (cell/sqrt(12)) so that
        # covariance matching cannot collapse P to ~0 (measured: q_scale -> 0.00 split the drone
        # track into 3 IDs on 1-2 m innovations)
        pf = np.diag([RANGE_CELL_M ** 2 / 12] * 3 + [RATE_CELL_MPS ** 2 / 12] * 3); d = np.diag_indices(6)
        self.P[d] = np.maximum(self.P[d], pf[d])
        self._F = F; self.hist.append((t, self.x.copy(), self.P.copy(), self.x.copy(), self.P.copy(), F))

    def _record_filtered(self):
        t, xp, Pp, _, _, F = self.hist[-1]; self.hist[-1] = (t, xp, Pp, self.x.copy(), self.P.copy(), F)
        if len(self.hist) > SMOOTH_LAG_CPIS + 1: self.hist.pop(0)

    def smoothed(self):
        """Fixed-lag RTS: run the backward pass over the stored window and return the state at
        its oldest entry (lag = SMOOTH_LAG_CPIS) with its smoothed covariance."""
        h = self.hist
        xs, Ps = h[-1][3].copy(), h[-1][4].copy()
        for k in range(len(h) - 2, -1, -1):
            t, xp_next, Pp_next, xf, Pf, F_next = h[k + 1][0], h[k + 1][1], h[k + 1][2], h[k][3], h[k][4], h[k + 1][5]
            try:
                C = Pf @ F_next.T @ np.linalg.inv(Pp_next)
            except np.linalg.LinAlgError:
                C = np.zeros((6, 6))
            xs = xf + C @ (xs - xp_next); Ps = Pf + C @ (Ps - Pp_next) @ C.T
        return h[0][0], xs, Ps

    def inflated(self, R):
        # ADAPTIVE sigma (user requirement): scale the measurement covariance by the track's own
        # measured NIS ratio so the gate/update reflect the observed scatter, not the reported
        # (overconfident) sigma.  Measured before: 2-sigma covered 31-56% of errors; 3-4 m jitter
        # split the bike track into two alternating tracks (ghost, 73% precision).
        if NIS_PER_AXIS:
            d = np.sqrt(np.maximum(self.nis_axis, 1.0))
            return R * np.outer(d, d)
        return R * max(1.0, self.nis)

    def gate(self, z, R):
        R = self.inflated(R)
        S = self.P + R; d = z - self.x
        try:
            m2 = float(d @ np.linalg.solve(S, d))
        except np.linalg.LinAlgError:
            return None
        # per-dimension conventional gate: chi2 with 6 dof at the 2-sigma-equivalent quantile
        return m2 if m2 <= GATE_SIGMA ** 2 * 6 else None

    def update(self, z, R, t, pd, beta):
        raw_S = self.P + R; d = z - self.x
        raw_d2 = float(d @ np.linalg.solve(raw_S, d))
        self.nis = 0.9 * self.nis + 0.1 * (raw_d2 / 6.0)      # EMA over ~10 CPIs, chi2 per dimension
        self.nis_axis = 0.9 * self.nis_axis + 0.1 * (d ** 2 / np.maximum(np.diag(raw_S), 1e-12))
        R = self.inflated(R)
        S = self.P + R; d2 = float(d @ np.linalg.solve(S, d))
        K = self.P @ np.linalg.inv(S)
        corr = K @ d
        self.x = self.x + corr; self.P = (np.eye(6) - K) @ self.P
        # covariance matching: the process noise actually experienced is the state correction
        # energy; compare its velocity part with the model's predicted velocity noise (Q_vv trace)
        dt = max(t - self.hist[-1][0], CPI_S) if len(self.hist) > 1 else CPI_S
        q_model = 3.0 * (ACCEL_SIGMA_MPS2 ** 2) * dt ** 2 * self.q_scale
        q_obs = float(corr[3:] @ corr[3:])
        self.q_scale = max(1e-6, (1 - Q_EMA) * self.q_scale + Q_EMA * self.q_scale * (q_obs / max(q_model, 1e-12)))
        self.hits += 1; self.misses = 0; self.last_update = t; self.opportunities += 1
        # LLR increment: Gaussian innovation likelihood vs false density beta (Blackman)
        sign, logdet = np.linalg.slogdet(2 * math.pi * S)
        self.score += math.log(pd / beta) - 0.5 * logdet - 0.5 * d2
        if self.hits >= CONFIRM_HITS and self.score >= T_CONFIRM: self.confirmed = True
        self._record_filtered()

    def miss(self, pd):
        self.misses += 1; self.opportunities += 1
        self.score += math.log(max(1.0 - pd, 1e-6))
        self._record_filtered()


class TrackerStream:
    """Streaming tracker B (2026-09-21): feed(stage9_record) -> output record for the CPI that left
    the look-ahead buffer (latency LOOKAHEAD_CPIS), or None; flush() drains the buffer at the end with
    the shorter windows the offline form also uses. The per-CPI body is the offline loop verbatim."""
    def __init__(self):
        self.tracks = []; self.buffer = []
        self.pd_hits = 0; self.pd_opps = 0; self.unassoc = 0; self.cpis = 0

    def feed(self, m):
        if LOOKAHEAD_CPIS <= 0:
            return self._process(m)
        self.buffer.append(m)
        if len(self.buffer) > LOOKAHEAD_CPIS:
            first = condition_one(self.buffer[:LOOKAHEAD_CPIS + 1]); self.buffer.pop(0)
            return self._process(first)
        return None

    def flush(self):
        out = []
        while self.buffer:
            first = condition_one(self.buffer[:LOOKAHEAD_CPIS + 1]); self.buffer.pop(0)
            rec = self._process(first)
            if rec is not None: out.append(rec)
        return out

    def _process(self, m):
        tracks = self.tracks; pd_hits = self.pd_hits; pd_opps = self.pd_opps; unassoc = self.unassoc; cpis = self.cpis
        t = m["time_s"]
        for tr in tracks: tr.predict(t)
        # Hypotheses: all consistent stage-9 solutions (ranked); fall back to the single state.
        # (2026-09-19) Only the TOP-ranked consistent hypothesis is used.  Feeding secondary
        # hypotheses (to all tracks, or to confirmed tracks only) was measured to lower confirmed
        # precision on every class (drone 100 -> 83-88%, runner 98 -> 74-82%): recurring multipath
        # mirrors keep ghost tracks alive.  Multi-hypothesis use needs track scoring (LLR/MHT).
        hyps = m.get("hypotheses")
        if hyps is None:
            hyps = [{"state": m["state"], "covariance": m["covariance"], "rms_per_dof": m.get("rms_per_dof")}] if m.get("consistent", True) else []
        hyps = merge_ambiguous([scaled_hyp(x) for x in hyps])[:1]
        assigned = set(); used = set(); cpis += 1
        pd = (pd_hits / pd_opps) if pd_opps >= 10 else PD_PRIOR
        beta = max(unassoc / max(cpis, 1), 1e-3) / MEAS_VOLUME       # unassociated consistent hypotheses per CPI per unit volume
        # 1) existing tracks pick their best gated hypothesis (greedy by Mahalanobis, one each)
        pairs = []
        for ti, tr in enumerate(tracks):
            for hi, h in enumerate(hyps):
                # secondary hypotheses are available only to CONFIRMED tracks (earned continuity);
                # tentative tracks see the top-ranked one only, so ghosts are not fed by mirrors
                if hi > 0 and not tr.confirmed: continue
                g = tr.gate(np.array(h["state"]), measurement_cov(h["covariance"]))
                if g is not None: pairs.append((g, ti, hi))
        for g, ti, hi in sorted(pairs):
            if ti in assigned or hi in used: continue
            tracks[ti].update(np.array(hyps[hi]["state"]), measurement_cov(hyps[hi]["covariance"]), t, pd, beta); assigned.add(ti); used.add(hi)
        # 2) only the TOP-ranked hypothesis may start a new track, and only if no track took it.
        #    Letting any secondary consistent hypothesis birth (they are recurring multipath
        #    mirrors) confirmed ghosts every few CPIs: drone confirmed precision 100% -> 32%.
        if hyps and 0 not in used:
            h = hyps[0]
            tracks.append(Track(t, np.array(h["state"]), measurement_cov(h["covariance"]))); assigned.add(len(tracks) - 1)
        for ti, tr in enumerate(tracks):
            if ti not in assigned: tr.miss(pd)
        for ti, tr in enumerate(tracks):
            if tr.confirmed: pd_opps += 1; pd_hits += (ti in assigned)
        unassoc += sum(1 for hi in range(len(hyps)) if hi not in used)
        # SPRT delete
        tracks[:] = [tr for tr in tracks if tr.score >= T_DELETE or tr.confirmed]
        # (2026-09-19) z-root merge: two tracks whose xy agree within one range cell are the same
        # object seen through the weak-z two-root ambiguity (measured: drone at the gNB height
        # split into z=7.5 and z=12.5 tracks alternating hits, confirmed precision 60%).  Merge by
        # moment matching (z -> mean, sigma_z includes the spread); the older track keeps its ID.
        # Documented limitation: two real objects vertically stacked within one cell in xy merge.
        # Track merge DISABLED (measured 2026-09-19: merging pulled a wrong root into the drone
        # track, z 10 -> 5 m).  Kept for reference; enable only with a per-class held-out check.
        merged = True
        while merged and TRACK_MERGE:
            merged = False
            for i in range(len(tracks)):
                for j in range(i + 1, len(tracks)):
                    a, b = tracks[i], tracks[j]
                    if np.linalg.norm(a.x[:2] - b.x[:2]) <= RANGE_CELL_M:
                        wa = a.hits / (a.hits + b.hits); wb = 1.0 - wa
                        xm = wa * a.x + wb * b.x
                        a.P = wa * (a.P + np.outer(a.x - xm, a.x - xm)) + wb * (b.P + np.outer(b.x - xm, b.x - xm)); a.x = xm
                        a.hits += b.hits; a.confirmed = a.confirmed or b.confirmed; a.score = max(a.score, b.score)
                        a.last_update = max(a.last_update, b.last_update)
                        tracks.pop(j); merged = True; break
                if merged: break
        # Coast credit: a track may coast at most as long as it has been observed (hits x CPI),
        # capped at MAX_COAST_S.  Without it a track confirmed on 3 outliers coasted 2 s while
        # reported as confirmed (bike: 4 ghosts, confirmed precision 49%).
        tracks[:] = [tr for tr in tracks if (t - tr.last_update) <= min(MAX_COAST_S, tr.hits * CPI_S)]
        smooth = {tr.id: tr.smoothed() for tr in tracks}
        def z_field(sig_z):
            return bool(sig_z <= Z_RESOLVED_SIGMA_M)
        self.pd_hits, self.pd_opps, self.unassoc, self.cpis = pd_hits, pd_opps, unassoc, cpis
        return ({"time_s": t, "measurement_consistent": bool(m.get("consistent", True)),
                    "smoothed": [{"track_id": tr.id, "confirmed": tr.confirmed, "time_s": smooth[tr.id][0], "position": smooth[tr.id][1][:3].tolist(),
                                  "velocity": smooth[tr.id][1][3:].tolist(), "position_sigma": np.sqrt(np.maximum(np.diag(smooth[tr.id][2])[:3], 0)).tolist(),
                                  "z_resolved": z_field(math.sqrt(max(smooth[tr.id][2][2][2], 0))), "q_scale": tr.q_scale} for tr in tracks],
                    "tracks": [{"track_id": tr.id, "confirmed": tr.confirmed, "position": tr.x[:3].tolist(), "velocity": tr.x[3:].tolist(),
                                "position_sigma": np.sqrt(np.diag(tr.P)[:3]).tolist(), "hits": tr.hits, "score": tr.score, "nis": tr.nis, "coasting": tr.last_update < t} for tr in tracks]})

def run(stage9_path):
    meas = [json.loads(l) for l in open(stage9_path) if l.strip()]
    meas = sorted(meas, key=lambda m: m["time_s"])
    stream = TrackerStream(); out = []
    for m in meas:
        rec = stream.feed(m)
        if rec is not None: out.append(rec)
    out.extend(stream.flush())
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage9", required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = run(a.stage9)
    with open(a.out, "w") as f:
        for r in out: f.write(json.dumps(r) + "\n")
    print("wrote", a.out, len(out))


if __name__ == "__main__":
    main()
