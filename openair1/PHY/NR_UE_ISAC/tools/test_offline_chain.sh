#!/bin/bash
# Deterministic end-to-end offline test: synthetic rows -> isac_replay -> reports.jsonl.
#  1. isac_replay (fixed work, lockstep): recorder round trip, target detection on every receiver,
#     gate-close discard, no CPI inside the gap, zero loss.
#  2. isac_replay_rt --realtime (production engine, paced at the recorded timestamps): real time --
#     per-CPI processing p95 < the 75 ms CPI period, no shed work, zero loss, and the same CPIs as 1.
set -euo pipefail
BUILD=${BUILD:?set BUILD to the sensing build dir}; T=$(cd "$(dirname "$0")" && pwd); O=${O:-/tmp/isac_synth}
CONF=${CONF:-$T/../../../../tests/passive_rx/ota/sensing_synth.conf}   # its report_path must be $O/reports.jsonl
rm -rf $O/reports*.jsonl $O/rec; mkdir -p $O/rec
# A target < 1 range bin from the direct path (the generator default on this survey) is untestable.
python3 $T/make_synthetic_rows.py --survey $T/survey_synth.json --out $O/rows.bin --seconds 12 --gap 5 8 \
  --target 200 150 5 3 -2 0 --cable-delay-ns 0 35 80 120 > $O/truth.json
(cd $BUILD && NR_ISAC_DEBUG_DIR=$O/rec ./isac_replay -O $CONF --rows $O/rows.bin) | tee $O/replay_fixed.txt
mv $O/reports.jsonl $O/reports_fixed.jsonl
(cd $BUILD && ./isac_replay_rt -O $CONF --rows $O/rows.bin --realtime) | tee $O/replay_rt.txt
mv $O/reports.jsonl $O/reports_rt.jsonl
python3 - "$O" "$T/survey_synth.json" <<'EOF'
import json, re, struct, sys, numpy as np
O, survey = sys.argv[1], json.load(open(sys.argv[2]))
tr = json.load(open(f"{O}/truth.json")); p0, v = np.array(tr["target_start"]), np.array(tr["velocity"])
gnb = np.array(survey["gnb_m"])

def records(path):   # (kind, header without mono_ns, payload) per record, see nr_isac.cc's recorder
    b = open(path, "rb").read(); i = 0; out = []
    while i < len(b):
        assert b[i:i + 4] == b"CFR1", f"{path}: bad magic at byte {i}"
        kind, ant, re_ = struct.unpack_from("<I", b, i + 4)[0], *struct.unpack_from("<II", b, i + 40)
        n = 68 + (0 if kind else 8 * ant * re_ + 8 * re_)
        out.append((kind, b[i:i + 60], b[i + 68:i + n])); i += n
    return out
# The replay re-records every row it submits: the recorder must reproduce the rows it was fed.
src = [(h, p) for k, h, p in records(f"{O}/rows.bin") if k == 0]
rec = [(h, p) for k, h, p in records(f"{O}/rec/cfr_rows.bin")]
assert src == rec, f"recorder round trip differs: {len(src)} rows in, {len(rec)} recorded"
print(f"recorder round trip: {len(rec)} rows identical")

def check(tag):
    print(f"--- {tag}")
    reps = [json.loads(l) for l in open(f"{O}/reports_{tag}.jsonl") if l.strip()]
    assert len(reps) >= 20, f"{tag}: too few CPIs: {len(reps)}"
    ok = n = ndet = 0
    for r in reps:
        t = r["midpoint_air_time_s"]; pt = p0 + v * t
        assert not 5.0 <= t < 8.0, f"{tag}: CPI inside the gap at t={t}"   # rows there are silence
        for sr in r["spatial_receivers"]:
            rxp = np.array(sr["receiver_position_enu_m"])
            # DL bistatic_range_m is EXCESS range: the engine places the measured direct path at zero
            # (dl_reference_measured_los), which is also what cancels per-channel cable delay.
            dR = np.linalg.norm(pt - gnb) + np.linalg.norm(rxp - pt) - np.linalg.norm(rxp - gnb)
            dets = sr["detections"]; n += 1; ndet += len(dets)
            if any(abs(d["bistatic_range_m"] - dR) <= sr["range_res_m"] for d in dets): ok += 1
    print(f"CPIs {len(reps)}; receiver-CPIs with the target within 1 range cell: {ok}/{n} "
          f"({100.0 * ok / n:.1f} %), mean detections per receiver-CPI {ndet / n:.2f}")
    fe = np.array([r["spatial_frontend_processing"]["wall_s"] for r in reps]) * 1e3
    mt = [r["multistatic_tracker_processing"] for r in reps]
    tk = np.array([m["elapsed_s"] for m in mt]) * 1e3
    total = fe + tk
    row = lambda name, x: print(f"  {name:<22} p50 {np.percentile(x, 50):7.2f}  p95 {np.percentile(x, 95):7.2f}  max {x.max():7.2f} ms")
    print(f"per-CPI engine processing (CPI period 75 ms), front-end peak concurrency "
          f"{max(r['spatial_frontend_processing']['peak_concurrency'] for r in reps)}:")
    row("spatial front ends", fe); row("multistatic tracker", tk)
    row("  tracker birth", np.array([m["birth_elapsed_s"] for m in mt]) * 1e3)
    row("  tracker UL", np.array([m["ul_elapsed_s"] for m in mt]) * 1e3)
    row("total", total)
    shed = sum(sr["detector_stop"]["reason"] == "next_cpi_processing_deadline"
               or sr["uplink_dtd_dfs"].get("detector_stop", {}).get("reason") == "next_cpi_processing_deadline"
               for r in reps for sr in r["spatial_receivers"])
    shed += sum(m["processing_deadline_exhausted"] or m["birth_search_deadline_exhausted"] for m in mt)
    m = re.search(r"gate_discarded_rows=(\d+) recorded_span_s=([\d.]+) replay_wall_s=([\d.]+)",
                  open(f"{O}/replay_{tag}.txt").read())
    gate_discarded, span, wall = int(m.group(1)), float(m.group(2)), float(m.group(3))
    print(f"replay wall {wall:.2f} s for {span:.2f} s of recorded air time")
    loss = {k: max(r[k] for r in reps) for k in ("dropped_submissions", "dropped_cpis", "discarded_pending_rows", "stale_submissions")}
    print(f"loss counters {loss}; deadline-shed stages {shed}; gate_discarded_rows {gate_discarded}")
    assert gate_discarded > (1 if tag == "fixed" else 0), f"{tag}: the gate close discarded {gate_discarded} pending rows"
    # The close must drop the pre-gap partial CPI, not emit it (and not the first post-gap row).
    short = [r["midpoint_air_time_s"] for r in reps
             if r["midpoint_air_time_s"] < 5.0 and r["dwell_s"] < r["cpi_plan"]["target_dwell_s"] - 0.001]
    assert not short, f"{tag}: pre-gap CPIs shorter than their target dwell at t={short}"
    assert not any(loss.values()), f"{tag}: rows or CPIs lost outside the gate-close discard: {loss}"
    assert ok >= 0.8 * n, f"{tag}: engine misses the synthetic target (or cable delay did not cancel)"
    return reps, total, shed

fixed, _, _ = check("fixed")
assert len(fixed) == 119, f"lockstep: {len(fixed)} CPIs, expected 119 (66 pre-gap + 53 post-gap)"
rt, total, shed = check("rt")
assert len(rt) == len(fixed), f"real time: {len(rt)} CPIs vs {len(fixed)} in lockstep (engine fell behind)"
assert shed == 0, f"real time: {shed} detector/tracker stages shed work at the CPI deadline"
assert np.percentile(total, 95) < 75.0, f"real time: per-CPI processing p95 {np.percentile(total, 95):.2f} ms >= 75 ms CPI period"
EOF
echo "test_offline_chain: PASS"
