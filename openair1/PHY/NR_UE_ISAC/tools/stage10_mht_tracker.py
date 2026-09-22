#!/usr/bin/env python3
"""Stage 10 tracker C: multi-hypothesis / multi-object extension of tracker B (same Track filter,
same SPRT scoring, same RTS smoother).  Declared before evaluation (2026-09-20):

  1. ALL consistent stage-9 solutions of a CPI are measurements (unit-weight-scaled covariance);
     tracker B used only the top-ranked one, which made it single-object by construction.
  2. Association: greedy global nearest neighbour on the whitened innovation among gated pairs;
     one hypothesis per track, one track per hypothesis (mutual exclusion).
  3. Per-track look-ahead conditioning (declared lag LOOKAHEAD_CPIS): a track's own associated
     measurements are buffered and each filter update is applied LOOKAHEAD CPIs later with the
     robust local-fit value and the larger of reported / empirical covariance (same rule as B,
     applied per track instead of to the global top-1 series).  Gating uses the filter predicted
     to the current time.
  4. Birth: every unassigned hypothesis starts a tentative track UNLESS its xy lies within
     DUP_XY_CELLS range cells of an existing track (the weak-z two-root mirror of an object is not
     a new object; documented limitation: two objects vertically stacked within that xy distance).
  4b. Measurement exclusivity: the stage-8 blocks of a hypothesis assigned to a CONFIRMED track
     are consumed; an unassigned hypothesis that shares at least EXCL_SHARE of its blocks with
     them is explained by that object (a multipath/mirror solution built from the same
     measurements) and cannot start a track.  Two real objects never share receiver blocks
     within one CPI beyond coincidence.
  4c. False density for the SPRT: unassociated hypotheses per CPI over the EMPIRICAL 6-D volume
     occupied by all hypotheses seen so far (2-sigma ellipsoid of their running covariance),
     instead of the declared surveillance volume, which made log(pd/beta) ~ 27 per hit and the
     SPRT inert (any 3 hits confirmed).
  5. Confirmation/deletion by the SPRT log-likelihood ratio (unchanged); a tentative track whose
     xy comes within DUP_XY_CELLS of a confirmed track is deleted (the confirmed one keeps its ID);
     two confirmed tracks within DUP_XY_CELLS keep the older.  No moment-matching merges (the
     mixture was measured to pull the wrong root into the drone track).
No target ground truth is read.  Feature-off equivalence: with MHT_ALL_HYPS=0 the measurement set
collapses to the top hypothesis and the result is tracker B's logic with per-track conditioning."""
from __future__ import annotations
import argparse, json, math, os
import numpy as np
import stage10_state_tracker as B
from stage10_state_tracker import (Track, measurement_cov, scaled_hyp, RANGE_CELL_M, CPI_S, GATE_SIGMA,
                                   T_DELETE, MAX_COAST_S, MEAS_VOLUME, PD_PRIOR, Z_RESOLVED_SIGMA_M)

LOOKAHEAD_CPIS = B.LOOKAHEAD_CPIS
DUP_XY_CELLS = 2.0                       # resolution-based duplicate/mirror rule (2 range cells in xy)
ALL_HYPS = os.environ.get("MHT_ALL_HYPS", "1") == "1"
EXCL_SHARE = 0.5                          # fraction of shared blocks that makes a hypothesis "explained"
EXCLUSIVITY = os.environ.get("MHT_EXCLUSIVITY", "1") == "1"
EMPIRICAL_BETA = os.environ.get("MHT_EMPIRICAL_BETA", "1") == "1"
# 4d (2026-09-20, declared): a NEW track may only be born from a solution supported by both a DL
# block and at least one UL block (a physical object is illuminated by both legs; the recurring
# false solutions were single-leg leftovers).  Updates of existing tracks are not restricted.
DLUL_BIRTH = os.environ.get("MHT_DLUL_BIRTH", "1") == "1"


class RunningCov:
    def __init__(self, dim=6): self.n = 0; self.mean = np.zeros(dim); self.M2 = np.zeros((dim, dim))
    def add(self, x):
        self.n += 1; d = x - self.mean; self.mean += d / self.n; self.M2 += np.outer(d, x - self.mean)
    def volume_2sigma(self):
        if self.n < 8: return None
        cov = self.M2 / (self.n - 1); sign, logdet = np.linalg.slogdet(cov + 1e-9 * np.eye(len(self.mean)))
        # volume of the 6-D ellipsoid at 2 sigma: pi^3/6 * (2)^6 * sqrt(det)
        return (math.pi ** 3 / 6.0) * 64.0 * math.exp(0.5 * logdet)


def robust_fit(win, t):
    """Robust per-axis linear fit of (t, z) pairs; returns (value at t, empirical diag covariance)."""
    T = np.array([w[0] for w in win]) - t; Z = np.array([w[1] for w in win])
    A = np.column_stack([np.ones_like(T), T]); w = np.ones(len(T))
    for _ in range(3):
        coef, *_ = np.linalg.lstsq(A * w[:, None], Z * w[:, None], rcond=None)
        resid = Z - A @ coef
        mad = np.median(np.abs(resid - np.median(resid, axis=0)), axis=0) * 1.4826 + 1e-9
        w_new = (np.max(np.abs(resid) / mad, axis=1) <= GATE_SIGMA).astype(float)
        if w_new.sum() < 3 or np.array_equal(w_new, w): break
        w = w_new
    resid = (Z - A @ coef)[w > 0]
    mad = np.median(np.abs(resid - np.median(resid, axis=0)), axis=0) * 1.4826
    return coef[0], np.diag(mad ** 2)


class MTrack:
    """A Track (filter) plus the per-track look-ahead buffer of raw associated measurements."""
    def __init__(self, t, z, R):
        self.filt = Track(t, z, R); self.pending = [(t, z, R)]; self.applied = 0
        self.raw_last = (t, z); self.hits = 1; self.last_assoc = t; self.born = t
    @property
    def id(self): return self.filt.id
    @property
    def confirmed(self): return self.filt.confirmed
    def gate_state(self):
        return self.filt.x, self.filt.P
    def associate(self, t, z, R):
        self.pending.append((t, z, R)); self.raw_last = (t, z); self.hits += 1; self.last_assoc = t
    def flush(self, now, pd, beta):
        """Apply buffered measurements older than the look-ahead horizon, conditioned on the buffer."""
        while self.applied < len(self.pending) and self.pending[self.applied][0] <= now - LOOKAHEAD_CPIS * CPI_S + 1e-9:
            t, z, R = self.pending[self.applied]
            win = [(tt, zz) for tt, zz, _ in self.pending[self.applied:] if tt <= t + LOOKAHEAD_CPIS * CPI_S + 1e-9]
            if len(win) >= 3:
                zc, emp = robust_fit(win, t); R2 = R.copy(); d = np.diag_indices(6); R2[d] = np.maximum(R[d], emp[d])
            else:
                zc, R2 = z, R
            if self.applied > 0:
                self.filt.predict(t)
                self.filt.update(zc, R2, t, pd, beta)
            else:   # first measurement: re-seed the filter at the conditioned value
                self.filt.x = zc.copy(); self.filt.P = R2.copy(); self.filt.hist[-1] = (t, zc.copy(), R2.copy(), zc.copy(), R2.copy(), np.eye(6))
            self.applied += 1
        # a coasting track is still propagated to the lagged horizon (now - LOOKAHEAD), so its
        # reported/smoothed state is a prediction, never a stale copy
        horizon = now - LOOKAHEAD_CPIS * CPI_S
        if self.applied > 0 and self.filt.t < horizon - 1e-9:
            self.filt.predict(horizon); self.filt._record_filtered()
        # drop buffer entries no longer needed for any window
        keep_from = max(0, self.applied - 1)
        if keep_from > 0:
            self.pending = self.pending[keep_from:]; self.applied -= keep_from


def run(stage9_path):
    meas = sorted([json.loads(l) for l in open(stage9_path) if l.strip()], key=lambda m: m["time_s"])
    tracks: list[MTrack] = []; out = []
    pd_hits = pd_opps = unassoc = cpis = 0; hyp_cov = RunningCov()
    for m in meas:
        t = m["time_s"]; cpis += 1
        hyps = m.get("hypotheses")
        if hyps is None:
            hyps = [{"state": m["state"], "covariance": m["covariance"], "rms_per_dof": m.get("rms_per_dof")}] if m.get("consistent", True) else []
        hyps = [scaled_hyp(h) for h in hyps]
        if not ALL_HYPS: hyps = hyps[:1]
        Z = [np.array(h["state"]) for h in hyps]; Rs = [measurement_cov(h["covariance"]) for h in hyps]
        Bl = [set(tuple(b) for b in (h.get("blocks") or [])) for h in hyps]
        for z in Z: hyp_cov.add(z)
        pd = (pd_hits / pd_opps) if pd_opps >= 10 else PD_PRIOR
        vol = hyp_cov.volume_2sigma() if EMPIRICAL_BETA else None
        beta = max(unassoc / max(cpis, 1), 1e-3) / (vol if vol else MEAS_VOLUME)
        # gating on the filter predicted to now (a copy: the real filter lags LOOKAHEAD CPIs)
        pairs = []
        for ti, tr in enumerate(tracks):
            dt = max(t - tr.filt.t, 0.0); F, Q = B.cv_matrices(dt); xp = F @ tr.filt.x; Pp = F @ tr.filt.P @ F.T + Q * tr.filt.q_scale
            for hi, (z, R) in enumerate(zip(Z, Rs)):
                S = Pp + tr.filt.inflated(R); d = z - xp
                try: m2 = float(d @ np.linalg.solve(S, d))
                except np.linalg.LinAlgError: continue
                if m2 <= GATE_SIGMA ** 2 * 6: pairs.append((m2, ti, hi))
        assigned = set(); used = set()
        for m2, ti, hi in sorted(pairs):
            if ti in assigned or hi in used: continue
            tracks[ti].associate(t, Z[hi], Rs[hi]); assigned.add(ti); used.add(hi)
        # exclusivity: blocks consumed by hypotheses assigned to confirmed tracks
        consumed = set()
        if EXCLUSIVITY:
            for m2, ti, hi in pairs:
                if ti in assigned and hi in used and tracks[ti].confirmed and tracks[ti].last_assoc == t:
                    consumed |= Bl[hi]
        # births: any unassigned hypothesis not within DUP_XY_CELLS of an existing track (mirror rule)
        # and not explained by consumed blocks
        for hi, z in enumerate(Z):
            if hi in used: continue
            if any(np.linalg.norm(z[:2] - tr.raw_last[1][:2]) <= DUP_XY_CELLS * RANGE_CELL_M for tr in tracks):
                unassoc += 1; continue
            if EXCLUSIVITY and Bl[hi] and len(Bl[hi] & consumed) >= EXCL_SHARE * len(Bl[hi]):
                unassoc += 1; continue
            if DLUL_BIRTH and Bl[hi]:
                legs_seen = {b[0] for b in Bl[hi]}
                if "dl" not in legs_seen or not any(l.startswith("ul") for l in legs_seen):
                    unassoc += 1; continue
            tracks.append(MTrack(t, z, Rs[hi]))
        # apply lagged updates / misses, scoring
        for ti, tr in enumerate(tracks):
            if ti not in assigned and tr.born < t:
                tr.filt.miss(pd) if tr.applied > 0 else None
            tr.flush(t, pd, beta)
        for ti, tr in enumerate(tracks):
            if tr.confirmed: pd_opps += 1; pd_hits += (ti in assigned)
        # deletion: SPRT, coast credit, duplicates
        tracks = [tr for tr in tracks if tr.filt.score >= T_DELETE or tr.confirmed]
        tracks = [tr for tr in tracks if (t - tr.last_assoc) <= min(MAX_COAST_S, tr.hits * CPI_S)]
        pruned = []
        for tr in sorted(tracks, key=lambda x: (not x.confirmed, x.born)):
            if any(np.linalg.norm(tr.raw_last[1][:2] - k.raw_last[1][:2]) <= DUP_XY_CELLS * RANGE_CELL_M for k in pruned):
                continue
            pruned.append(tr)
        tracks = sorted(pruned, key=lambda x: x.id)
        smooth = {tr.id: tr.filt.smoothed() for tr in tracks}
        out.append({"time_s": t, "measurement_consistent": bool(m.get("consistent", True)), "n_hypotheses": len(Z),
                    "smoothed": [{"track_id": tr.id, "confirmed": tr.confirmed, "time_s": smooth[tr.id][0], "position": smooth[tr.id][1][:3].tolist(),
                                  "velocity": smooth[tr.id][1][3:].tolist(), "position_sigma": np.sqrt(np.maximum(np.diag(smooth[tr.id][2])[:3], 0)).tolist(),
                                  "z_resolved": bool(math.sqrt(max(smooth[tr.id][2][2][2], 0)) <= Z_RESOLVED_SIGMA_M), "q_scale": tr.filt.q_scale} for tr in tracks],
                    "tracks": [{"track_id": tr.id, "confirmed": tr.confirmed, "position": tr.filt.x[:3].tolist(), "velocity": tr.filt.x[3:].tolist(),
                                "position_sigma": np.sqrt(np.diag(tr.filt.P)[:3]).tolist(), "hits": tr.hits, "score": tr.filt.score, "nis": tr.filt.nis,
                                "coasting": tr.last_assoc < t} for tr in tracks]})
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
