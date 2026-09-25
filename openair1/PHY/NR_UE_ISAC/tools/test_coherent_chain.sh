#!/bin/bash
# Offline end-to-end: make_coherent_scene -> isac_replay (lockstep) -> coherent JSONL checks.
set -euo pipefail
BUILD=${BUILD:?set BUILD}; T=$(cd "$(dirname "$0")" && pwd); O=${O:-/tmp/isac_coherent}
REPO=$(cd "$T/../../../.." && pwd); CONF_T=$REPO/tests/passive_rx/ota/sensing_coherent_synth.conf
rm -rf "$O"; mkdir -p "$O"
python3 "$T/make_coherent_scene.py" --out "$O/rows.bin" --truth "$O/truth.json" --survey-out "$O/survey.json" --seconds ${SECONDS_RUN:-6}
python3 "$REPO/tests/passive_rx/survey.py" "$O/survey.json" --geometry "$O/g.json" --apply "$CONF_T" "$O/coherent.conf" --report-path "$O/reports.jsonl"
# The long-dwell slow-target CPI (coherent_long_dwell) is on by default and scored; LONG_DWELL=0 turns it off.
if [ "${LONG_DWELL:-1}" = 0 ]; then sed -i 's/^\(\s*\)coherent_enable = 1;/\1coherent_enable = 1;\n\1coherent_long_dwell = 0;/' "$O/coherent.conf"; grep -q "coherent_long_dwell = 0" "$O/coherent.conf"; fi
(cd "$BUILD" && ./isac_replay -O "$O/coherent.conf" --rows "$O/rows.bin") > "$O/replay.txt" 2>&1
python3 - "$O" <<'EOF'
import glob, json, sys, numpy as np
C = 299792458.0
O = sys.argv[1]; tr = json.load(open(f"{O}/truth.json"))
allrep = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherent_reports.*.jsonl")) for l in open(f)]
# Traffic event lines (flow gate, one per ~1 s while closed; isac_replay never opens the gate) are not CPIs.
reps = [r for r in allrep if r.get("dwell") != "long" and r.get("event") is None]; longs = [r for r in allrep if r.get("dwell") == "long"]
trk = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherent_tracks.*.jsonl")) for l in open(f)]
reps = [r for r in reps if r.get("event") is None]   # traffic events (flow-gate state) carry no CPI
trk = [r for r in trk if r.get("event") is None]
coh = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherence.*.jsonl")) for l in open(f)]
assert len(reps) > 20 and len(trk) == len(reps) and len(coh) == len(reps), (len(reps), len(trk), len(coh))
done = [r for r in reps if r["skipped_reason"] is None]
print(f"CPIs {len(reps)} ({len(reps) - len(done)} skipped: {sorted({r['skipped_reason'] for r in reps} - {None})})")
# Calibration phase vs the EXPECTED LOS-tap phase (plan ruling R2): the LOS-referenced tap keeps the
# carrier term e^{-j2pi fc (d_i/c + resid_i)}, which the calibration absorbs.
g = tr["geometry"]; gnb = np.array(g["gnb"]); rx = np.array(g["rx"]); fc = C / done[-1]["lambda_m"]
d = np.linalg.norm(rx - gnb, axis=1); resid = np.array(tr["resid_delay_s"]); ph = np.array(tr["phases_rad"])
expected = np.angle(np.exp(1j * ((ph - ph[0]) - 2 * np.pi * fc * ((d - d[0]) / C + (resid - resid[0])))))
last_coh = [c for c in coh if not c.get("skipped")][-1]
err = np.angle(np.exp(1j * (np.array(last_coh["phase"]) - expected)))
print("calibration phase error vs expected (rad):", np.round(err, 3), " los_found", last_coh["los_found"])
assert np.all(np.abs(err[1:]) < 0.3), ("phases", err)
assert last_coh["G"] > 3.0 and last_coh["rho"] > 0.6, ("coherence", last_coh["G"], last_coh["rho"])
# Targets: confirmation required only outside the zero-Doppler notch (controller ruling 6):
# notch half-width in path-rate = lambda * notch_half_bins * dopp_step_hz.
last_rep = done[-1]; res = last_rep["range_res_m"]; last = trk[-1]; t = last["t"]
notch = last_rep["lambda_m"] * last_rep["notch_half_bins"] * last_rep["dopp_step_hz"]
cen = rx.mean(axis=0)
unit = lambda v: v / np.linalg.norm(v)
conf = [x for x in last["tracks"] if x["confirmed"]]
import re
vol = [float(x) for x in re.search(r'coherent_volume_m\s*=\s*"([^"]+)"', open(f"{O}/coherent.conf").read()).group(1).split(":")]
if longs:   # long-dwell CPIs: hit rate per target within one range cell of truth at the CPI midpoint; false = near no target
    ok = [r for r in longs if r["skipped_reason"] is None]; nfalse = 0; hits = {tg["name"]: 0 for tg in tr["targets"]}; vis = dict(hits)
    for r in ok:
        tt = r["t"]; lres = r["range_res_m"]; pos = {tg["name"]: np.array(tg["p0"]) + np.array(tg["v"]) * tt for tg in tr["targets"]}
        for tg in tr["targets"]:
            p = pos[tg["name"]]; v = np.array(tg["v"]); rate = v @ (unit(p - gnb) + unit(p - cen))
            if abs(rate) <= r["rate_slow_mps"] and abs(rate) > r["lambda_m"] * r["notch_half_bins"] * r["dopp_step_hz"]:
                vis[tg["name"]] += 1; hits[tg["name"]] += any(np.linalg.norm(np.array(d["p"]) - p) < lres for d in r["detections"])
        for d in r["detections"]:
            if min(np.linalg.norm(np.array(d["p"]) - q) for q in pos.values()) >= lres: nfalse += 1
        print(f"  long t={tt:.2f} T_L={r['t_l_s']:.2f}s rows={r['rows']} res={lres:.2f}m dets={len(r['detections'])} " +
              " ".join(f"{d['p']} rr={d['rr']:+.2f}" for d in r["detections"]) + "  timing " + str({k: round(x) for k, x in r['timing_ms'].items()}))
    lt = np.array([r["timing_ms"]["total"] for r in ok]); cad = np.median([r["cadence_s"] for r in ok])
    print(f"long dwell: {len(ok)}/{len(longs)} CPIs, T_L p50 {np.median([r['t_l_s'] for r in ok]):.2f} s, cadence {cad:.2f} s, "
          f"total ms p50/p95/max {np.percentile(lt, 50):.0f}/{np.percentile(lt, 95):.0f}/{lt.max():.0f}")
    print("long dwell hit rate (CPIs with the target in the slow band):", {k: f"{hits[k]}/{vis[k]}" for k in hits}, " false detections:", nfalse)
    # The person sits inside the short CPI's notch for the whole run (the short CPI cannot see it at all):
    # the long dwell must place it within one range cell in most of its slow-band CPIs. False detections
    # are reported, not asserted: a strong target's split / partial-channel ghosts are a property of the
    # shared detect/refine chain (the short CPI shows the same class for the car), see longdwell-report.md.
    assert vis["person"] > 0 and hits["person"] > vis["person"] / 2, ("long dwell misses the person", hits, vis)
print(f"t={t:.3f} s  range_res={res:.2f} m  notch |dL/dt| < {notch:.3f} m/s")
for tg in tr["targets"]:
    p = np.array(tg["p0"]) + np.array(tg["v"]) * t; v = np.array(tg["v"])
    rate = v @ (unit(p - gnb) + unit(p - cen))
    per_ch = [v @ (unit(p - gnb) + unit(p - r)) for r in rx]
    dmin = min((np.linalg.norm(np.array(x["p"]) - p) for x in conf), default=1e9)
    inside = all(vol[2 * k] <= p[k] <= vol[2 * k + 1] for k in range(3))   # the tracker only covers the volume
    exempt = abs(rate) <= notch or not inside
    print(f"  {tg['name']:<7} dL/dt {rate:+.3f} m/s (per ch {np.round(per_ch, 3)})  nearest confirmed {dmin:.2f} m"
          + ("  EXEMPT (in zero-Doppler notch)" if abs(rate) <= notch else "") + ("" if inside else "  EXEMPT (left the volume)"))
    if not exempt: assert dmin < res, (tg["name"], dmin, res)
assert all(x["p"][2] >= -1e-6 for r in trk for x in r["tracks"]), "below-ground track"
assert len(conf) <= len(tr["targets"]) + 1, ("ghost tracks", len(conf))
tm = {k: np.array([r["timing_ms"][k] for r in done]) for k in done[-1]["timing_ms"]}
print("per-stage CPU ms (p50 / p95 / max):  " + "  ".join(f"{k} {np.percentile(x, 50):.1f}/{np.percentile(x, 95):.1f}/{x.max():.1f}" for k, x in tm.items()))
print(f"t_cpi p50 {np.percentile([r['t_cpi_s'] for r in done], 50) * 1e3:.1f} ms; stats {reps[-1]['stats']}")
print(f"coherent chain: {len(reps)} CPIs, G={last_coh['G']:.2f}, rho={last_coh['rho']:.2f}, confirmed={len(conf)}")
EOF
echo "test_coherent_chain: PASS"
