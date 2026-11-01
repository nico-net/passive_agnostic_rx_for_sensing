# pred1101/periodic -- periodic resource tracking and occasion prediction (SRS integrated)

Base: mpci-int-1031 (a6e8f12e52). Evidence labels per PROJECT_MEMORY §0.1. Nothing here was run on air or in a replay.

## What changed
- `openair1/PHY/NR_UE_TRANSPORT/nr_periodic_track.{h,c}` (new, pure state machine, no globals, so nothing for the per-cell static audit):
  - `nr_ptrack_estimate`: period/offset from a detection-slot list. Period sets per type from TS 38.331 (SRS 1..2560, CSI-RS 4..640, PUCCH 1..640, CG/SPS 1..640 incl. 128).
    The largest legal period under which >= 4 and >= 80 % of the last 16 detections share one offset and >= 75 % of the occasions between the first and last matching
    detection were detected. Tolerates missed occasions and stray detections; rejects TDD-gap patterns and illegal gaps.
  - `nr_ptrack_hit` / `nr_ptrack_miss` / `nr_ptrack_due` / `nr_ptrack_next`: confirmation, predicted-occasion bookkeeping, re-learning on contradicting detections (period refinement 20 -> 10 tested), stale restart (10240 slots).
    Demotion on evidence: 4 consecutive missed predicted occasions, or a full 16-occasion window with <= 4 hits.
  - Counters per type (`nr_ptrack_counters_t`): predicted, hit, miss, demoted, confirmed, audit_hit.
- `nr_srs_blind.c/.h`, `nr_srs_blind_internal.h`: `srs_track_t` uses `nr_ptrack_t` instead of the single-gap rule (3 identical gaps, any miss in the gap chain broke it).
  New `nr_srs_predicted_search` (one sequence correlation on the tracked group/cs, no 30-group search on a miss, no result cache so the +-400 sample offsets can be retried),
  `nr_srs_periodic_counters`; `nr_srs_group_cs_search` split into a `hint_only` internal variant.
- `nr_srs_passive.c/.h`: with confirmed tracks, due occasions are tested directly (comb detection skipped); the blind search (pregate, comb detect, group search) now runs as a sparse audit every 16th
  eligible slot instead of never (before: any confirmed track switched blind search off for all new resources). Export happens on every predicted occasion that validates (same `nr_srs_export` path).
  Misses are recorded only when the slot was fully examined (no budget truncation).
- Metrics: `nr_passive_metrics.{h,c}`, `nr_passive_metrics_json.c` tail keys `periodic_{srs,csirs,pucch,cg}_{predicted,hit,miss,demoted,confirmed,audit_hit}` and `srs_audits` (ISAC_METRICS json).
- CMake: new source in PHY_NR_PASSIVE_UL_SRC, `test_nr_srs_blind`, `cmake_targets/mpci_cloud.cmake` (cellfeat srs test), `tools/mpci/mpci_audit_files.txt`; new target `test_nr_periodic_track`.

## Tests (written first)
`tests/nr_periodic_track_test.cc` (14 cases: clean/missed/stray/largest-period/illegal gaps, refinement 20 -> 10, counters, demotion by 4 misses, sparse-window demotion, occasional misses do not demote,
stale restart, 3000 noisy trials with 15 % misses + 3 % strays at 7 periods, 2000 random detection sets never confirm), plus `nr_srs_blind_test.cc` (missed-occasion period, counters, predicted search = 1 correlation
on hit and on miss) and `nr_passive_metrics_test.cc` (per-type keys). Results `[OFFLINE VERIFIED, DGX aarch64 unit]`: test_nr_periodic_track 13/13, test_nr_srs_blind 33/33 (incl. all pre-existing period/miss/stale tests), test_nr_passive_metrics 15/15,
test_mpci_cellfeat_srs/sense, static_audit_check + test_static_audit_scanner pass (table rows added for the const period arrays, stale `periods` rows dropped).
nr-uesoftmodem, pdsch_gpu, ldpc_cuda build clean. Unrelated: nr_fapi_srs_* ctest entries were not built.
Design note: with only 4 detections one stray detection can fake a finer period for a few occasions (e.g. 19,29,34,39 reads as period 5); the next detections relearn the true period (tested statistically, >= 95 % final-state correct in 1400 noisy trials).
Re-learning: a detection that fits no pattern keeps the learned one; real changes are caught by miss-based demotion.

## Not done / not verified
- CSI-RS/TRS, PUCCH, SPS/CG: the tracker, period sets and counters exist and are exported, but only SRS feeds them. CSI-RS discovery already confirms period/offset in its own code and was not rewired;
  PUCCH and CG have no passive detector on this base. Their counters stay 0.
- "Integrate periodic SRS over periods" (averaging records of one track) is NOT implemented: phase coherence between occasions (CFO/TA drift) needs a measured model, not assumed here.
- Everything on air is `[HYPOTHESIS]`: more regular SRS records (each due occasion is now tested with a 1-correlation cost and a hint-free blind fallback only by audit) and lower blind-search CPU
  (comb detect + up to 30 sequence correlations per tracked occasion replaced by one). Risk: a periodic resource whose sequence changes (group hopping v=1 is unsupported anyway) is demoted after 4 misses.
- Audit period 16 is a constant (cost/discovery latency trade, not cell fitted); new resources appear within <= 16 eligible slots + 4 detections.

## Where to look on air
ISAC_METRICS: `periodic_srs_predicted/hit/miss/demoted/confirmed/audit_hit`, `srs_audits`, `srs_us` (CPU), `srs_pregate_skip`, `sense_cfr_exported_srs`. Expect hit/predicted -> 1 for a stable SRS; misses followed by demoted when the UE leaves.

## Overlap risks
Touches `nr_srs_passive.c` process_srs, `nr_passive_metrics*` (tail keys only, additive), CMake lists next to nr_srs_blind.c. Other pred1101 branches adding metrics fields will conflict textually in nr_passive_metrics.h/json tail.
