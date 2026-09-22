#!/usr/bin/env python3
"""UE localiser (stage 9 helper): position of the UL illuminator from the UE->receiver direct-path
delays measured by stage 2 on the UL leg, using only inter-receiver DIFFERENCES (common X410
clock; the UE transmit time is an unknown common offset, never assumed).  Static or slow UE:
delays are accumulated over a window of CPIs (median per receiver) before solving.
Unknowns: (x, y, z, offset). Infrastructure inputs only: receiver positions, range cell.
Outputs the estimate, its Gauss-Newton covariance (whitened by the delay standard errors) and the
residual.  Ground truth is not read here."""
from __future__ import annotations
import argparse, json
import numpy as np

C = 299792458.0


def solve_ue(rx_positions, delays_m, sigma_m, seeds_z=(0.5, 1.5, 5.0), iters=50):
    """delays_m: measured direct-path range per receiver (absolute scale, unknown common offset)."""
    rx = np.asarray(rx_positions); d = np.asarray(delays_m); s = np.asarray(sigma_m)
    def resid(x):
        return (np.linalg.norm(rx - x[:3], axis=1) + x[3] - d) / s
    best = None
    centre = rx.mean(axis=0)
    for z0 in seeds_z:
        x = np.array([centre[0], centre[1], z0, 0.0]); lam = 1e-3; r = resid(x); c = 0.5 * r @ r
        for _ in range(iters):
            J = np.zeros((len(d), 4)); eps = 1e-5
            for k in range(4):
                dx = x.copy(); dx[k] += eps; J[:, k] = (resid(dx) - r) / eps
            step = np.linalg.solve(J.T @ J + lam * np.eye(4), -J.T @ r)
            xn = x + step; rn = resid(xn); cn = 0.5 * rn @ rn
            if cn < c: x, r, c, lam = xn, rn, cn, lam * 0.3
            else: lam *= 5.0
            if np.abs(step).max() < 1e-6: break
        if best is None or c < best[1]:
            best = (x, c, J)
    x, c, J = best
    cov = np.linalg.pinv(J.T @ J)
    return {"position_m": x[:3].tolist(), "offset_m": float(x[3]), "cost": float(c),
            "dof": len(d) - 4, "position_sigma_m": np.sqrt(np.diag(cov)[:3]).tolist()}


def run(report_path, rx_positions, range_res_m, window_cpis):
    reps = [json.loads(l) for l in open(report_path) if l.strip()]
    per_rx = {i: [] for i in range(len(rx_positions))}; per_rx_se = {i: [] for i in range(len(rx_positions))}
    out = []
    for r in reps:
        for i, sr in enumerate(r.get("spatial_receivers") or []):
            ul = sr.get("uplink_dtd_dfs") or {}; d = ul.get("direct_reference") or {}
            if ul.get("valid") and d.get("delay_bins") is not None:
                per_rx[i].append(d["delay_bins"] * range_res_m); per_rx_se[i].append(max(d.get("delay_standard_error_bins", 0.02), 1e-3) * range_res_m)
        if all(len(v) >= 1 for v in per_rx.values()):
            delays = [np.median(v[-window_cpis:]) for v in per_rx.values()]
            # sigma: the larger of the reported standard error and the empirical scatter in the window
            sig = [max(np.median(per_rx_se[i][-window_cpis:]), np.std(per_rx[i][-window_cpis:]) if len(per_rx[i]) > 1 else 0.0, range_res_m / np.sqrt(12.0)) for i in per_rx]
            est = solve_ue(rx_positions, delays, sig); est["time_s"] = r["midpoint_air_time_s"]; est["cpis_used"] = min(window_cpis, len(per_rx[0]))
            out.append(est)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True); ap.add_argument("--rx", nargs=3, type=float, action="append", required=True)
    ap.add_argument("--range-res-m", type=float, required=True); ap.add_argument("--window-cpis", type=int, default=40)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = run(a.report, [np.array(r) for r in a.rx], a.range_res_m, a.window_cpis)
    with open(a.out, "w") as f:
        for r in out: f.write(json.dumps(r) + "\n")
    print("wrote", a.out, len(out))


if __name__ == "__main__":
    main()
