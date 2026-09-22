"""Synthetic validation of ue_localiser_dfs on the real pinned footprint: walking UE at several
headings + a static UE; unknown clock offset/drift and CFO injected; delay/DFS noise at the
resolution-cell level.  Compares against TDOA-only (delay medians, static assumption)."""
import json, numpy as np, sys
sys.path.insert(0, "/data/sionna/isac_microdoppler_src/tools")
from ue_localiser_dfs import solve_window, predict
from ue_localiser import solve_ue
g = json.load(open("/data/sionna/four_rx_wide_aperture_20260918/case_05/infrastructure_geometry.json"))
RX = np.array([g["receiver_positions_m"][f"rx{i}"] for i in range(4)])
RANGE_RES = 3.0503913105413107; RATE_RES = 1.1422492665493145
CPI = 0.075; K = 60   # 4.5 s window (declared; CV model holds for a walker)
rng = np.random.default_rng(1)
rows = []
for case, (p0, speed, heading) in {"static": ((5.0, 5.0, 1.5), 0.0, 0.0), "walk_E": ((5.0, 5.0, 1.5), 1.3, 0.0), "walk_N": ((5.0, 5.0, 1.5), 1.3, np.pi / 2),
                                   "walk_SW": ((0.0, 8.0, 1.5), 1.3, -3 * np.pi / 4), "walk_far": ((-8.0, 12.0, 1.5), 1.5, np.pi / 4), "run_S": ((12.0, 6.0, 1.5), 3.0, -np.pi / 2)}.items():
    errs_p, errs_v, errs_tdoa, sig = [], [], [], []
    for trial in range(30):
        o0, o1, cfo = rng.uniform(-50, 50), rng.uniform(-0.05, 0.05), rng.uniform(-20, 20)   # m, m/s (UE drift), m/s
        t = (np.arange(K) - (K - 1) / 2) * CPI
        v = np.array([speed * np.cos(heading), speed * np.sin(heading), 0.0])
        x_true = np.array([*p0, v[0], v[1], o0, o1, cfo])
        d, f = predict(x_true, RX, t)
        sd = 0.3; sf = 0.3   # m, m/s: sub-cell interpolation noise level (declared for the synthetic test)
        dm = d + rng.normal(0, sd, d.shape); fm = f + rng.normal(0, sf, f.shape)
        est = solve_window(RX, t, dm, fm, sd, sf)
        tdoa = solve_ue(RX, np.median(dm, axis=0), np.full(4, sd))
        pc = np.array(p0) + v * 0.0   # window centre truth
        errs_p.append(np.linalg.norm(np.array(est["position_m"])[:2] - pc[:2])); errs_v.append(np.linalg.norm(np.array(est["velocity_mps"])[:2] - v[:2]))
        errs_tdoa.append(np.linalg.norm(np.array(tdoa["position_m"])[:2] - pc[:2])); sig.append(est["position_sigma_m"][1])
    print(f"{case:8s} xy err DFS+delay median {np.median(errs_p):5.2f} p90 {np.percentile(errs_p, 90):5.2f} m | TDOA-only median {np.median(errs_tdoa):5.2f} p90 {np.percentile(errs_tdoa, 90):5.2f} m | v err median {np.median(errs_v):.2f} m/s | reported sigma_y median {np.median(sig):.2f} m")
