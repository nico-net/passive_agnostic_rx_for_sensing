# Task 13 report: Blind DM-RS and data scrambling identities (DL and UL)

Lane: scr, worktree `sens6:/home/sens/NICOLA/agn-wt/scr`, branch `sdd/agn-scr`.
Commit: `6db31e2cec` — "Passive DL/UL: discover DM-RS (0..65535) and data scrambling identities
instead of assuming the PCI".

## What changed

### DM-RS scrambling identity (`nr_dmrs_id_estimate.{h,c}`)
- Removed the `crc &&` precondition on the DL accumulate call site (`nr_pdsch_passive_queue.c`).
  The estimator never needed a CRC — the DM-RS sequence is fixed by `(nid, slot, symbol)` regardless
  of whether the payload decodes — so gating it on CRC was circular: under a wrong id the CRC never
  passes, which is exactly the situation the estimator exists to diagnose.
- Widened the fixed 1024-candidate window to a resizable one (`nr_dmrs_id_set_range(first, count)`;
  arrays are now `malloc`'d to `range_count`, not a compile-time 1024). Default range unchanged
  (0..1023); stage 2 is `NR_DMRS_ID_CANDIDATES..NR_DMRS_ID_SPACE` (1024..65535).
- Staging in the DL wiring: stage 2 triggers only after stage 1 accumulates ≥ 64 grants without
  deciding (not merely "not enough evidence yet" — `nr_dmrs_id_decide`'s own ≥16-grant gate already
  covers that). Once escalated, stage-2 `accumulate()` calls are throttled to 1-in-2048 eligible
  jobs.
- **Measured**: one stage-2 accumulate over 64,512 candidates took **170,360 µs** (test output,
  `DmrsId.FindsAnIdAboveTheOldRange`), vs ~2 ms for a 1024-candidate one at similar geometry — ~64x,
  matching the candidate-count ratio. At 1-in-2048 throttling this amortises to ~83 µs/job.
- One estimator state per nSCID on the DL side (`g_dl_dmrs_id[2]`) — scramblingID0/1 are independent
  RRC fields; a shared state would average two different true identities into neither.
- UL DM-RS decision is now actually **applied** (previously accumulated but never read back):
  `nr_pdcch_blind_monitor.c`'s UL fallback construction uses `nr_pusch_passive_ul_dmrs_id()->decided`
  in place of the flat PCI default, with an explicit `opts` override still taking priority.

### Data (PDSCH/PUSCH) scrambling identity — new `nr_scrambling_id_sweep.{h,c}`
No coherence statistic exists for this identity (TS 38.211 scrambling is per-RE random-looking
regardless of candidate correctness) — the TB CRC is the only oracle. Implemented as an ordered
walk: PCI first, then the decided DM-RS id if it's in the 0..1023 data-id range, then the rest of
0..1023, latching permanently on the first CRC pass (bounded at 1024 LDPC decodes worst case).
- **DL**: per-RNTI (`rnti_dec_t.data_id` in `nr_pdsch_passive_decode.c`), wired at
  `nr_pdcch_blind_monitor_rt.c`'s `dlsch_pdu.dlDataScramblingId` construction. Only allowed to
  advance past PCI when `nr_pdsch_config_sweep_rnti_prior_get()` (Technique D converged) **and**
  `nr_pdsch_passive_rnti_crc_stalled(rnti, 20)` (CRC rate 0 over ≥20 TBs, reusing the existing
  `g_rnti_dec`/`g_rnti_ok` census arrays — no new counters). This is the gate the brief asked for:
  a config mismatch is never misread as a scrambling mismatch, and a healthy/unconverged RNTI never
  burns an LDPC decode on the sweep.
- **UL**: cell-wide, not per-RNTI (this deployment has one UL BWP and, unlike DL, no per-RNTI
  Technique-D-equivalent signal to gate on) — gated on `nr_pusch_passive_ul_crc_stalled(20)`, reusing
  the existing `g_try`/`g_crc_ok` UL globals.
- Feedback is gated on a `data_id_advance` flag threaded through the job (DL:
  `nr_pdsch_passive_job_t.data_id_advance`; UL: `nr_pdcch_blind_ul_result_t.data_id_advance`), set
  only when the sweep's own candidate was actually used for that grant — a PCI-fallback attempt never
  perturbs a sweep it did not use.

## Tests
- `test_nr_dmrs_id_estimate`: **8/8** (6 pre-existing + 2 new: `FindsAnIdAboveTheOldRange` — N_ID
  40000 synthesized, stage-2 range, accumulates and decides with no CRC input;
  `WrongRangeDoesNotDecide` — same N_ID, default 0..1023 range, correctly refuses to decide).
- `test_nr_scrambling_id_sweep` (new target): **4/4** — `OrderIsPciThenDmrsIdThenRest`,
  `LatchesOnFirstCrcPass`, `DmrsIdAboveDataRangeIsSkipped`, plus one extra
  (`NoDmrsDecisionYet`, `dmrs_id = -1`).
- `test_nr_pdsch_config_sweep`, `test_nr_pdsch_prb_set`, `test_nr_pdsch_ptrs_unav`,
  `test_nr_pdsch_qm_oracle`, `test_nr_pdsch_xoverhead`: all still pass (untouched by this change,
  run as part of the brief's `nr_pdsch` regex).
- `test_nr_pdcch_blind_monitor`: **could not be verified** — link fails with undefined references to
  `nr_pdcch_blind_parse_lane_als`, `nr_pdcch_blind_monitor_bank_has_geometry`, `nr_tdd_config_init`,
  `nr_tdd_slot_has_downlink`. Confirmed via `git stash` (this task's changes fully removed, rebuilt)
  that this link failure is **pre-existing on this branch**, from other lanes' concurrent merges
  (Task 14/15 work touching `nr_passive_acq_state.c` / CORESET lane code) — nothing this task touched.
  `nr-uesoftmodem` itself — which does statically link `nr_pusch_passive_decode.c` and every new
  symbol this task added (`nr_pusch_passive_ul_dmrs_id`, `nr_pusch_passive_data_id_current`,
  `nr_pusch_passive_ul_crc_stalled`) — builds clean, so the same UL call path this task added is
  exercised and resolves correctly in the real binary; only the standalone unit-test binary's stale
  CMake source list is broken, and fixing that is outside this task's scope.
- Full `nr-uesoftmodem` executable: builds clean (only two pre-existing, unrelated warnings).

## Known limitations / out of scope
- DM-RS type 2 remains unsupported (`nr_dmrs_id_accumulate` is type-1 only, matching the pre-existing
  DL `dmrsConfigType == 0` guard it's called behind).
- ~~UL's DM-RS `accumulate()` call is still only reached after a CRC-OK decode~~ — **fixed in review
  fix round 1, finding 2 below; no longer a limitation.**
- `test_nr_pdcch_blind_monitor` needs its CMake source list reconciled with the other lanes' recent
  merges before it can build again; not attempted here (would require pulling in symbols owned by
  other in-flight lanes).

---

## Review fix round 1 (controller ruling R24)

Commit: `5d0836e53a` — "Task 13 review fix round 1: latch-vs-eligibility bug (critical) + UL DM-RS
CRC-gating (finding 2)".

### Finding 1 (CRITICAL): latched result discarded the moment the sweep succeeds

**Bug.** `nr_pdsch_passive_data_id_current()` / `nr_pusch_passive_data_id_current()` checked
`if (!advance_ok) return pci;` *before* ever looking at whether the sweep had already latched a
result. `advance_ok` was computed from a **lifetime** "stalled" measure
(`nr_pdsch_passive_rnti_crc_stalled` / `nr_pusch_passive_ul_crc_stalled`: total attempts ≥ N *and*
total CRC-OK == 0). The moment the sweep found the correct id and got its first CRC pass under it,
the lifetime CRC-OK counter went from 0 to 1 **forever**, so `advance_ok` became `false` on every
subsequent call — and the very next grant for that RNTI (or, on UL, the very next grant overall)
discarded the just-latched correct id and reverted to PCI permanently. This broke the feature on
exactly the deployment it targets (a cell whose data-scrambling id ≠ PCI).

**Fix, file:line:**
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:377` (function start) —
  `nr_pdsch_passive_data_id_current()` rewritten to check `r->data_id.latched >= 0` **first** and
  return it unconditionally; `advance_ok` now only gates the `else` branch (creating/advancing an
  unlatched sweep).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:130-151` — same restructuring for the UL
  cell-wide sweep (`nr_pusch_passive_data_id_current()`).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:329-336,349-356` — new
  `g_rnti_fails_since_ok[65536]`, a **windowed, resettable** measure (CRC fails since this RNTI's
  last pass) replacing the lifetime `g_rnti_dec`/`g_rnti_ok`-based check inside
  `nr_pdsch_passive_rnti_crc_stalled()`; reset to 0 on every `CRC_OK`, incremented on every
  non-`CRC_OK` outcome at the existing census update site (`nr_pdsch_passive_queue.c:980-986`).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:113-125` — new cell-wide
  `g_ul_fails_since_ok`, same windowed semantics, replacing the lifetime `g_try`/`g_crc_ok`-based
  check inside `nr_pusch_passive_ul_crc_stalled()`; reset on the `CRC_OK` success path
  (`nr_pusch_passive_decode.c:~1355`), incremented on the `hp_crc_failed()` path
  (`nr_pusch_passive_decode.c:~1322`).

**Why both changes were needed, not just one:** the latch-first fix alone stops a *found* id from
being discarded, but without the windowed measure the sweep could never even *start* walking again
after a transient success reset eligibility, and more importantly the *original* report's own
recommended two-part fix ("always return latched" + "windowed/resettable measure") addresses two
distinct failure modes — the first is about honoring a result already found, the second is about not
poisoning the ability to *look for* one.

**Test evidence:**
```
openair1/PHY/NR_UE_TRANSPORT/tests/nr_scrambling_id_sweep_test.cc — new TEST(ScramblingIdSweep,
LatchedIdSurvivesFurtherPassesAndFailures): walks with 2 fails, latches on the 3rd feed (a pass),
then asserts current()==latched_id and s.latched unchanged across 5 further feed(pass) calls AND
5 further feed(fail) calls.
```
```
$ ssh sens6 ctest -R test_nr_scrambling_id_sweep --output-on-failure
    Start 23: test_nr_scrambling_id_sweep
1/1 Test #23: test_nr_scrambling_id_sweep ......   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```
Note on scope of this test: it pins the invariant the fix depends on (`nr_scrambling_id_sweep_t`
itself always returns a latched result under any further `feed()` sequence — this was already true
pre-fix and is not what broke). The actual bug lived in the *wrapper* functions
(`nr_pdsch_passive_data_id_current`/`nr_pusch_passive_data_id_current`), which are private statics
deep inside PHY-coupled files with no existing standalone gtest harness (no `test_nr_pdsch_passive_decode`
target exists in this tree). No new harness was stood up for this fix round — the wrapper-level fix
is verified by code inspection (the `latched`-first branch is now structurally unreachable to skip)
plus the full `nr-uesoftmodem` link succeeding with the new counters wired at every call site. Flagged
here rather than silently claimed as test-covered.

### Finding 2: UL DM-RS accumulation still CRC-gated

**Bug.** The UL DM-RS accumulate block lived inside `nr_pusch_passive_decode_inner()`'s **CRC-OK-only
success path** (originally right after `atomic_fetch_add_explicit(&g_crc_ok, ...)`), i.e. structurally
identical to the circularity Task 13's own brief already fixed on the DL side: on a cell whose UL
DM-RS id ≠ PCI, the CRC never passes, so the accumulate call it needs to *discover that* is never
reached.

**Fix, file:line:**
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:~1015` (right before the "G is needed by
  the LLR probe" comment, immediately after the existing PUSCH_DMRS CFR block) — the whole DM-RS
  accumulate block moved here from its old post-`nr_ulsch_decoding()` location. This is right after
  the delay-refined FEP and before any LDPC decode or CRC check, so it runs on every attempted grant
  regardless of outcome. The `nr_pusch_passive_queue_running() && dmrs_config_type==0 &&
  !transform_precoding` scope guard is unchanged.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:98-111` — DM-RS state widened from a single
  `g_ul_dmrs_id` to `g_ul_dmrs_id[NR_UL_DMRS_NSCID]` (2), mirroring the DL `g_dl_dmrs_id[2]`; added
  `NR_UL_DMRS_STAGE1_GRANTS`/`NR_UL_DMRS_STAGE2_THROTTLE` constants (same values as DL: 64 and 2048)
  and the same staged-range escalation logic (0..1023 → 1024..65535 once stage 1 exhausts 64 grants
  undecided; stage-2 `accumulate()` throttled 1-in-2048). The margin gate itself
  (`nr_dmrs_id_decide(dst, 16, 10.0)`) is untouched — an undecided id is never applied.
  `nr_pusch_passive_ul_dmrs_id()` gained an `int nscid` parameter to select the right state.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:4363-4390,4571-4576,4654-4657` —
  `blind_ul_finish()`'s inline identity-fallback computation extracted into a new
  `blind_ul_apply_scrambling_ids(opts, out)` (needs `out->nscid`, which is not yet known inside
  `blind_ul_finish()` — the DCI 0_1 extractor sets `out->nscid` *before* calling `blind_ul_finish()`,
  the DCI 0_0 extractor sets it *after*, so the shared `blind_ul_finish()` itself can never assume
  either ordering). Both extractors now call the new helper immediately after their own `out->nscid`
  assignment.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6555` — diagnostic log line's
  `nr_pusch_passive_ul_dmrs_id()` call updated to `nr_pusch_passive_ul_dmrs_id(0)` for the new
  signature.

**Test evidence:** no new dedicated test (the UL decode path has no standalone gtest harness —
`nr_ulsch_decoding`/`gNB_config` dependencies make it PHY-integration-only in this tree, matching the
pre-existing lack of a `test_nr_pdsch_passive_decode`-equivalent for UL). Verified by:
1. Full clean rebuild of `nr-uesoftmodem` with the relocated block and the nscid-parameterized getter
   used consistently across `nr_pusch_passive_decode.c`, `nr_pdcch_blind_monitor.c`, and
   `nr_pdcch_blind_monitor_rt.c` (no undefined/mismatched-signature errors).
2. Code-level check that the new insertion point has every value the old one used already in scope
   (`fp`, `gnb`, `slot`, `slot_off`, `g->{start_symbol,num_symbols,ul_dmrs_symb_pos,bwp_start,
   start_rb,num_rb,nscid,dmrs_config_type,transform_precoding}`) — no new state threading was needed.

### Full re-test summary after this round
```
test_nr_dmrs_id_estimate ......... 8/8 Passed  (0.20s)
test_nr_scrambling_id_sweep ...... 5/5 Passed  (0.00s)   [was 4/4; +1 new]
test_nr_pdsch_config_sweep ....... Passed      (36.23s)
test_nr_pdsch_prb_set ............ Passed      (0.00s)
test_nr_pdsch_ptrs_unav .......... Passed      (0.00s)
test_nr_pdsch_qm_oracle .......... Passed      (0.00s)
test_nr_pdsch_xoverhead .......... Passed      (0.01s)
nr-uesoftmodem .................... builds clean
test_nr_pdcch_blind_monitor ....... still unbuildable, same pre-existing unrelated link errors as
                                     recorded in the original report (undefined
                                     nr_pdcch_blind_parse_lane_als /
                                     nr_pdcch_blind_monitor_bank_has_geometry / nr_tdd_config_init /
                                     nr_tdd_slot_has_downlink, owned by other lanes) — unchanged by
                                     this fix round, not re-verified via git stash a second time
                                     since nothing in this round touches those symbols.
```

### Remaining known gap (not part of either finding, flagged for transparency)
`nr_pdsch_passive_rnti_crc_stalled()`'s new windowed counter, like the census counters it replaces,
is only updated by `nr_pdsch_passive_queue.c`'s **deferred** consumer path
(`nr_pdsch_passive_queue.c:980-986`). A grant decoded via `nr_pdcch_blind_monitor_rt.c`'s **in-line**
(non-deferred) path never touches it, so for an RNTI whose grants are never deferred, the DL data-ID
sweep can never see `advance_ok == true` and will stay on PCI even if genuinely mismatched. This is
pre-existing (queue-deferred is the primary/default decode path per
`nr_pdsch_passive_queue.h`'s own design doc) and out of scope for both of this round's findings, but
recorded here rather than left implicit.

## Files touched
- `openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.{h,c}`, `tests/nr_dmrs_id_estimate_test.cc`
- `openair1/PHY/NR_UE_TRANSPORT/nr_scrambling_id_sweep.{h,c}` (new),
  `tests/nr_scrambling_id_sweep_test.cc` (new)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.{c,h}`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{c,h}`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.{c,h}`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.{c,h}`
- `CMakeLists.txt` (new source file + new test target registration)
