# CSI-RS / TRS -> periodic tracker (fix1102/csirsptrack, base freeze-20261102)

## What changed
- `nr_periodic_track.c` `nr_ptrack_hit`: for `NR_PTRACK_CSIRS` a detection exactly one slot after the previous one is the trailing slot of a
  TRS burst (2 consecutive slots) and is ignored. Without it a burst splits the residues 50/50 and the estimator never confirms.
  The CSI-RS period set was checked against TS 38.331 (4,5,8,10,16,20,32,40,64,80,160,320,640): unchanged. TRS 10/20/40/80 ms = 10..160 slots, all in the set.
- `nr_csirs_observer.{h,c}`: every observer entry owns an `nr_ptrack_t`; `nr_csirs_observer_due()` (called with the scored rho of each
  occasion of a confirmed resource, ISAC_RECONF scoring path) feeds `nr_ptrack_hit/miss`. Per-observer (= per cell, `g_csirs_obs` is NR_CELL_STATIC)
  `nr_ptrack_counters_t periodic`, exported to `m->periodic[NR_PTRACK_CSIRS]` (already serialised by `nr_passive_metrics_json.c`).
  New advisory API `nr_csirs_observer_predicted_next/predicted_due`. Tracker reset on (re)confirmation. The four-miss revocation is untouched.
- `nr_csirs_blind_rt.c` (confirmed-NZP re-score loop): occasions predicted by the tracker are scored first; no occasion is skipped.
  Blind discovery (candidate rotation, probes, ZP maintenance) is not touched: a prediction never suppresses a detection.
- CMake: `nr_periodic_track.c` added next to every `nr_csirs_observer.c` (PHY_NR_UE_SRC, 4 test targets, mpci_cloud.cmake).
- No new flags/env, no new static state.

## Counters (JSON, per cell)
`periodic_csirs_predicted, _hit, _miss, _demoted, _confirmed, _audit_hit`.

## Tests
`test_nr_periodic_track` (PtrackCsirs.*: period set, TRS burst at 10/20/40/80/160, lost lead, stray, period change -> demotion -> relearn,
SRS adjacent not collapsed) and `test_nr_csirs_observer` (CsirsObserverPtrack.*).

## Not verified / caveats
- Counters are only fed where confirmed NZP occasions are scored, i.e. `ISAC_RECONF=1` (existing gate of nzp_observer_score). Without it they stay 0.
- Only NZP resources; ZP resources are not tracked.
- The ordering gain (predicted first) is not measured; no CPU saving is claimed. `[HYPOTHESIS]` negligible effect, the change is for observability.
- No on-air or replay run (rules). Expected on air: `periodic_csirs_confirmed` ~ number of confirmed resources, `miss` ~ fades.

## Follow-up 2026-11-02 (controller)
- Removed the "predicted occasions scored first" reordering in nr_csirs_blind_rt.c: it added observer lookups per slot and no benefit was measured.
- The per-entry nr_ptrack_t moved out of nr_csirs_observer_entry_t into a parallel array (nr_csirs_observer_t.pt[]), so the linear find()
  over entries keeps its pre-change density.
- Demotion churn explained [OFFLINE VERIFIED, DGX replay, temporary counters]: on raw_pci415_4s_1102_0951, all 545 demotions were gap > NR_PTRACK_EXPIRY
  (0 reorder, misses 3): the 4 s capture loops 25x and the receiver re-syncs each loop. Expected to be rare on air.
- A/B on raw_pci415_4s_1102_0951, 100 s, sensing on, n=3 each [OFFLINE VERIFIED, DGX replay]: frozen tot p99 15.4/11.0/14.3 ms, async drop 18.7/5.1/20.0 %;
  this branch 13.6/12.2/11.8 ms, 5.4/10.9/9.0 %. No regression; the high drops on this busy capture exist on the frozen build too.
