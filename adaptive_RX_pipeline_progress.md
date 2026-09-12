# Adaptive RX pipeline — progress and evidence ledger

Created: **2026-09-10**, Europe/Zurich.
Plan: [adaptive_RX_pipeline.md](adaptive_RX_pipeline.md).
Canonical location: `sens6:/home/sens/NICOLA/adaptive-rx-sensing/adaptive_RX_pipeline_progress.md`.

## Current status

**Documentation and read-only inventory complete; P01 (baseline manifest / acceptance profile) IN_PROGRESS as of 2026-09-10 (see session below); P02 (valid DL/UL replay fixtures) PASSED as of 2026-09-11 after two controller-ruled fix rounds to the passive-replay recorder (see P02 session + Fix round 1/2 below) -- the first implementation task under this plan to reach PASS, and the first receiver source changes made under this plan (two small, ruling-scoped gate fixes in `nr_passive_replay_capture.c`/`nr_pdcch_blind_monitor_rt.c`). G0 remains IN_PROGRESS overall, blocked only on P01's outstanding survey/acceptance-limit work. P03 (branch abstraction) IN_PROGRESS as of 2026-09-11: foundation delivered (`docs/passive_branch_globals_audit.md`, `nr_rx_branch.{h,c}`, `[sensing] rx_branches`/`rx_branch_phys_map`, 14/14 gtest) but deliberately not wired into the RT read loop (controller-scoped to P04/P05). P04 (immutable buffer delivery) IN_PROGRESS as of 2026-09-11: standalone `nr_rx_span_pool.{h,c}` delivered (refcounted per-branch spans, no sample copies, per-branch drop policy), 5/5 gtest, also deliberately not wired into the RT read loop (same controller ruling -- `nr-ue.c` is dirty with another session's edits). P07 (independent DL decoding) PASS as of 2026-09-11 in REPLAY form (single-branch view of the DL decode chain, `dl_branch_view_replay` ctest on the P02 fixture: legacy 34/38 unchanged, views 0/1/2/3 = 34/31/34/34 of 38 with data-aided submissions == own CRC-OK; live independent mode gated on P06 job tagging and the P10 CFR ABI; G2 IN_PROGRESS). P05 (independent digital correction/recovery) IN_PROGRESS as of 2026-09-11: standalone `nr_rx_branch_sync.{h,c}` delivered (per-branch CFO accumulator, timing offset, frame-wrap/slot-continuity bookkeeping, lock/acq epoch staleness snapshot), 14/14 gtest including G1 test 4 in pure form; a checked-in hardware-isolation guard (`tests/passive_rx/check_branch_hw_isolation.sh`, registered as two ctest entries, with its own self-test proving the grep both catches a planted violation and does not false-positive on a legitimate `rf_`-substring symbol name); and `docs/passive_branch_wiring_plan.md`, a file:line map of exactly which `nr-ue.c`/`nr-ue-ru.c` statements belong to AcquisitionOwner vs. become per-branch. Still deliberately not wired into the RT read loop (same controller ruling) -- G1 remains IN_PROGRESS: tests 1/3/4 exist in pure form (P04/P05) and test 2 exists in pure form via P03/P04, but test 5 (standalone-replay comparison) and the full G1 exit criterion (including "no unauthorized hardware operation from branch WORKERS" -- there are no live workers yet) cannot pass until the read-loop wiring lands.**

| Baseline fact | Value |
|---|---|
| Host / tree | `sens6:/home/sens/NICOLA/adaptive-rx-sensing` |
| Branch | `merge/adaptive-sensing` |
| Inspected commit | `50c8312fa46f17850aeee579abfb0a745f8465ff` (unchanged as of the P01 session) |
| Existing modified/untracked files (2026-09-10 plan baseline) | `M tests/passive_rx/run_adaptive_receive_test.sh`, `?? tests/passive_rx/aoa_track_dl.conf` |
| **Measured dirty state at P01 session time (2026-09-10, differs from the row above -- see P01 session notes)** | `git status --short` additionally shows `M executables/nr-ue-ru.c`, `M executables/nr-ue.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`, `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`, and untracked `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`, `tests/passive_rx/run_manual_x410.sh`, beyond the two rows above. None of it was reset/clean/stashed/touched by P01; recorded as a measured fact only (full list in the P01 manifest's `git.status_short`) |
| Build cache | Sensing ON, tests ON, CUDA OFF, RelWithDebInfo (re-measured 2026-09-10 P01 session: unchanged) |
| Newly written files | `adaptive_RX_pipeline.md`, `adaptive_RX_pipeline_progress.md`, plus P01's `tests/passive_rx/baseline_manifest.sh`, `tests/passive_rx/check_manifest.py`, `tests/passive_rx/acceptance_four_rx.json`, `tests/passive_rx/geometry_four_rx.json`, `tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` (+ sibling `_tracked.patch`; superseded the review-round-1 regeneration of the original `manifest_20260910_50c8312-dirty.json` -- see "Fix round 1" addendum below) |
| Actual new pipeline code | None |
| Compilation / receiver tests / OTA | Not run |
| Separated-antenna survey | Not provided or verified (unchanged; `tests/passive_rx/geometry_four_rx.json` now formalizes this as `surveyed:false`) |
| Acceptance profile | v1 created 2026-09-10, NOT frozen except for the plan's exact invariants; every deployment-dependent limit UNSET by construction (`tests/passive_rx/acceptance_four_rx.json`) |
| Four-branch launcher | Proposed; not implemented |
| Optional UL localization | Specification only; default-off feature not implemented |

## Task checklist and accomplishment dates

Dates use `YYYY-MM-DD` in Europe/Zurich. Check an implementation item only when its corresponding evidence gate passes. Source presence is not an accomplishment under this new plan. Use `—` until complete. Keep the plan's initial checklist synchronized when statuses change.

| Check | Task | Required result | Status | Accomplished date | Evidence |
|---|---|---|---|---|---|
| [x] | D01 | Code/config/branch inventory | PASS | 2026-09-10 | Session 2026-09-10; plan section 2 |
| [x] | D02 | Detailed plan and progress ledger | PASS | 2026-09-10 | These two documents |
| [ ] | P01 | Baseline manifest / acceptance profile | IN_PROGRESS | — | Session 2026-09-10 (P01) below + "Fix round 1" addendum; current manifest `tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` (commit `e4c8cadd5f`), `acceptance_four_rx.json`, `geometry_four_rx.json`. NOT checked/PASS: geometry unsurveyed and every deployment-dependent acceptance limit UNSET are BLOCKED sub-items |
| [x] | P02 | Valid DL/UL replay fixtures | PASS | 2026-09-11 | Session 2026-09-11 (P02) + Fix round 1 + Fix round 2 below. Audit found all 22 pre-existing `replay.bin` captures INADMISSIBLE (binary mismatch + full_auto=1). Two structural code defects (both coupling the passive-replay recorder's arm condition to full-auto-only paths) fixed under controller ruling: DL gate in `nr_passive_replay_capture.c`'s `nr_passive_replay_dl()` (round 1) and UL gate in `nr_pdcch_blind_monitor_rt.c`'s UL candidate loop (round 2, root-caused via live instrumentation). With both fixes, capture `sensing_manual_fixed.UtvBT7` (binary `c3810019...`) produced `REPLAY READY ... slots=320 UL=24 DL-controls=38` and replay-verified `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0). Registered in `tests/passive_rx/baselines/fixtures.json` (`status:"PASS"`, 1 admissible fixture); `check_manifest.py --fixtures` PASSES; negative selftest cases still correctly FAIL |
| [ ] | P03 | Branch ownership / lifecycle | IN_PROGRESS | — | Session 2026-09-11 (P03) below. Foundation delivered: `docs/passive_branch_globals_audit.md` (globals audit), `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.{h,c}` (pure branch identity/epoch/lifecycle, 14/14 gtest), `[sensing] rx_branches`/`rx_branch_phys_map` config surface in `nr_isac.cc`/`.h`. NOT done: nothing wired into the `nr-ue.c` RT read loop (deliberately out of scope, see P03's controller ruling) and the `nb_antennas_rx` antenna-count cross-check is unwired (setter added, no live caller — see audit doc's "nb_antennas_rx reachability" section) — so G1 is not testable end-to-end from this task alone. **UPDATED 2026-09-11 (P06a): both gaps are now CLOSED IN CODE** — the lifecycle calls (`nr_rx_branch_lock`/`lose_lock`/`set_rf_discontinuity`) are wired into `UE_thread()`'s sync/RXDISCONT/RFSTALL/CFOTRK sites and `nr_isac_set_nb_antennas_rx()` is called at `nr-uesoftmodem.c` before `nr_isac_init()` (the check is now fatal). Verified offline only (replay + a deliberate 4-branch/2-antenna abort); NO live capture — X410 unreachable 2026-09-11 |
| [ ] | P04 | Immutable channel-buffer delivery | IN_PROGRESS | — | Session 2026-09-11 (P04) below. Standalone module delivered: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.{h,c}` (refcounted per-branch span pool, no sample copies, per-branch drop policy), 5/5 gtest (G1 test 1 and test 3 in pure form, plus refcount and epoch-carry cases). NOT done: nothing wired into the nr-ue.c RT read loop (deliberately out of scope, see P04's controller ruling -- that file is dirty with another session's uncommitted edits) -- so G1 stays NOT_STARTED end-to-end. |
| [ ] | P05 | Independent acquisition / recovery | IN_PROGRESS | — | Session 2026-09-11 (P05) below. Foundation delivered: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch_sync.{h,c}` (per-branch CFO accumulator, timing offset, frame-wrap/slot-continuity bookkeeping, lock/acq epoch staleness snapshot, 14/14 gtest incl. G1 test 4 in pure form), `tests/passive_rx/check_branch_hw_isolation.sh` (hardware-isolation guard + self-test, 2 ctest entries), `docs/passive_branch_wiring_plan.md` (file:line wiring map). NOT done: nothing wired into the `nr-ue.c` RT read loop (deliberately out of scope, see P05's controller ruling -- that file remains dirty with another session's uncommitted edits) -- so G1 test 5 and the full end-to-end exit criterion are not met. **UPDATED 2026-09-11 (P06a): the lifecycle half of that wiring IS now in `nr-ue.c`** (see the P06a session), and `nr_rx_branch_sync_is_stale`'s job-side analogue (`nr_rx_branch_dispatch_is_stale`) is consumed by the DL queue. Still NOT done: per-branch `nr_rx_branch_sync_t` instances (CFO/timing per branch) and G1 test 5 |
| [ ] | P06 | Branch-local PDCCH discovery / grants | IN_PROGRESS | — | Session 2026-09-11 (P06a) below. DELIVERED: per-branch DL job FAN-OUT only — `nr_rx_branch_set_dispatch()`/`nr_rx_branch_dispatch_is_stale()` (pure, 4 new gtest cases), `nr_pdsch_passive_queue_enqueue_fanout()` wired into both DL job sites in `nr_pdcch_blind_monitor_rt.c`, per-job `lock_epoch`/`acq_epoch` snapshot in existing padding (sizeof 384 unchanged), consumer-side stale-epoch drop, per-branch `PDSCHQ-BRANCH` counters, `RX_BRANCHES` launcher knob. NOT DONE — this is P06's open CORE: branch-OWNED discovery state (learned RNTIs, candidate persistence, energy/noise gates, discovered layouts) is still one shared instance, so a branch can still acquire another branch's grants; UL not fanned out (P08). **BLOCKED on live evidence: X410 unreachable (uhd_find_devices: "No UHD Devices Found", 2026-09-11 13:48 UTC), the two live captures were not attempted, so none of the above has ANY live verification.** |
| [x] | P07 | Independent DL decoding | PASS (replay form; live independent mode gated on P06/P10) | 2026-09-11 | Session 2026-09-11 (P07) below. `docs/passive_dl_branch_view_audit.md` (antenna-axis audit + mechanism decision), single-branch view in `nr_pdsch_passive_decode.{h,c}` (thread-local shadow UE, `nb_antennas_rx=1`, `rxdata={rxdata[phys]}`; MRC/retry/subset/BRANCHFO/planned-branch nvar all off by their own gates, DMRSFO EMA/apply + SFO_CORRECT explicitly gated), `ISAC_DL_BRANCH_VIEW`, `unsupported_multilayer_in_branch_view` counter, `ISAC_PDSCH_VERDICT_TRACE`, per-job `branch_id`/`physical_channel` (sizeof unchanged), `tests/passive_rx/replay_branch_view.sh` = ctest `dl_branch_view_replay` PASS on the P02 fixture: legacy `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24` unchanged (full log byte-identical bar the version banner and the new summary line); views 0/1/2/3 crc_ok 34/31/34/34 of 38 (view1 = 3 TBs coverage loss, recorded not tuned), data_submits == own crc_ok on every view, unsupported_multilayer 0/38 (fixture is all Nl=1). **LIVE CAVEAT: 34/34 is NOT expected live coverage.** This fixture is all Nl=1, but `nr_pdsch_passive_decode.c`'s own measured note records the live srsRAN cell scheduling num_layers=4 on ~97 % of grants (3 on ~2 %, 1-2 on a handful), so a one-antenna branch view will reject ~97 % of live grants as `unsupported_multilayer_in_branch_view` (plan sec 2.3-conformant, explicit, counted apart from CRC failure) -- expected live per-branch data-aided coverage is a few percent of grants until a rank>1 single-antenna scheme exists. UPDATED 2026-09-11 (P06a): the producer no longer tags 0/0 — it fans each grant out to every active branch with real ids/phys/epochs, so live independent mode is now REACHABLE; it has not been RUN (X410 unreachable), so the live multilayer fraction is still unmeasured. NOT done: live independent mode ( CFR ABI has no branch slot until P10), all-four-in-parallel replay, per-branch DMRSFO/SFO tracker (P09) |
| [ ] | P08 | Independent UL decoding / context pool | NOT_STARTED | — | — |
| [ ] | P09 | Namespace / TLS / concurrency audit | IN_PROGRESS | — | Session 2026-09-11 (P09) below. DELIVERED: `docs/passive_branch_namespace_audit.md` (10 identifier-assignment sites classified over branch x direction x decode-vs-reconstruction x UE-session, plus a full `__thread` classification of the 7 named files); ONE concrete collision FIXED — the passive DL decode's `harq_unique_pid` was `2000 + harq_process_nbr`, identical for every branch P06a fans one occasion out to, and is now `2000 + branch_id*32 + hpn%32` (new header `openair1/PHY/NR_UE_TRANSPORT/nr_passive_harq_tag.h`, `static_assert` bound 2127 < 3000, 5 new gtest cases, `nr_rx_branch_test` 24/24). Stride 32, not 16: the DCI HARQ-process-number field is 5 bits when the cell sets `harq-ProcessNumberSizeDCI-1-1` (`nr_pdcch_blind_monitor.h:263`, the 7th field of `pdcch_blind_monitor_dci_bits`, read at that width by `nr_pdcch_blind_monitor.c:2475`), so 16 would alias hpn 0 with hpn 16 on the SAME branch and break the branch-0 legacy pin above hpn 15 (found in fix round 1). `sizeof(nr_pdsch_passive_job_t)` unchanged at 384 (the fix reads the existing `t_view_branch`, no new field). Legacy replay `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24` and `replay_branch_view.sh` 34/34/31/34/34 of 38 both unchanged. RECORDED NOT FIXED (audit secs 4.1/4.2): the passive UL decode tags EVERY TB `0` (`nr_pusch_passive_decode.c:1072` → `openair1/PHY/NR_TRANSPORT/nr_ulsch_decoding.c:129`) across up to 6 concurrent contexts, and its own `PASSIVE_UL_HARQ_TAG_BASE 4000` is dead code — → P08; the passive DL re-encode (3000 range) is not branch-strided — → P10 with the CFR ABI. **NOT VALIDATED END-TO-END: `replay_branch_view.sh` runs ONE VIEW PER PROCESS, so no harness here can put two branches' jobs with the same `harq_process_nbr` in one process concurrently; and the linked soft-decoder backend keeps no per-id state, so the corruption is unobservable offline even if it were reachable. The fix rests on code inspection (`nrLDPC_coding_aal.c:654`) plus an isolated arithmetic unit test.** NOT DONE: the sanitizer/race half of G2 test 7, and G2 test 6 (branch-local noise/timing under distinct per-channel impairments) — both need either a sanitizer build or live RF |
| [ ] | P10 | CFR ABI and producer migration | IN_PROGRESS (ABI slice only) | 2026-09-11 | Session 2026-09-11 (P10a) below. DELIVERED, controller-scoped to the ABI slice: `nr_isac_submit_cfr_multi_branch()` (new entry point with a `uint8_t branch_id` tail argument) in `nr_isac.{h,cc}`, with the pre-existing `nr_isac_submit_cfr_multi()` reduced to a ONE-LINE wrapper passing `NR_ISAC_BRANCH_NONE` (0xFF) -- that wrapper, not an argument audit, is the structural reason the other 8 producer call sites are unchanged; the tag threaded `Snapshot.branch_id` -> `PendingRow.branch_mask` -> `CfrWindow.branch_mask` -> `PipelineReport.branch_mask` (accumulated exactly like `source_mask`, since one PendingRow merges every co-timed submission); a report field OMITTED WHOLESALE when no producer tagged a branch (`branch_mask`, plus `branch_id` only when the CPI is unambiguously ONE branch -- a fused CPI is never made to name one of its branches, and an untagged CPI is never made to claim branch 0, which is a REAL branch under P03's map); `nr_pdsch_passive_view_branch()` exported from `nr_pdsch_passive_decode.{h,c}` (the P06a/P07 thread-local view identity, `NR_ISAC_BRANCH_NONE` when no view is armed); and ONE producer migrated -- `nr_pdsch_data_aided.c`, all four of its submissions, closing the `TODO(P10)` P07 left at that ABI. `test_nr_isac_python_parity` gains `test_branch_identity_report()` (untagged CPI carries NO branch field; single-branch CPI reports mask+id; two-branch CPI reports the mask and does NOT name one; and the tagged report line with only the two branch fields removed is CHARACTER-IDENTICAL to the untagged one bar the wall clock -- i.e. carrying identity changes no numeric result). Both replay baselines re-proven byte-identical: legacy `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24`, `replay_branch_view.sh` 34/34/31/34/34 of 38 with data_submits == own crc_ok. Session 2026-09-11 (P10b) below CONTINUES this row: the three remaining DL CFR producers of the AoA audit's row 3 -- `csi_rx.c`'s CSI-RS tap, `nr_pdcch_blind_monitor_rt.c`'s blind DM-RS tap and `phy_procedures_nr_ue.c`'s SSB/PBCH tap -- now derive their submissions from a new `nr_isac_submit_plan()` (`nr_isac.{h,cc}`, logic in `pipeline_types.h`'s `build_submit_plan()`): ONE untagged multi-antenna submission at <= 1 active branch (structurally the previous call -- the producer's own antenna count is passed through verbatim and `first_ant` is 0, so the packing expression is unchanged), and one SINGLE-ANTENNA submission per active branch otherwise, sliced by pointer offset out of the same antenna-major buffer and tagged with that branch's id so P13a's router delivers it to that branch's own engine. `nr_pdsch_data_aided.c` needed NO further change (P10a + P07's single-branch view already give it both the identity and the right physical channel). `nr_isac_aoa_antennas()` was deliberately NOT removed -- see the P10b session entry. Session 2026-09-11 (P10c) below CONTINUES this row and closes the producer list: the two UL CFR producers (`nr_pusch_data_aided.c`, `nr_pusch_passive_decode.c`) now use the SAME `nr_isac_submit_plan()` as the DL taps -- **they are NOT a NONE-only special case.** The brief's physical-constraint argument (coherent multi-antenna combining, therefore not per-branch attributable) was checked against source and holds for the DECODE but NOT for what these producers SUBMIT: `H = Y_a/X` is formed PER ANTENNA against one common reconstructed X (`nr_pusch_data_aided.c:222`) and the DM-RS path submits `ul_ch_estimates[0*num_sp + a]` per antenna, deliberately uncombined (`:873` says why -- combining would destroy the AoA inter-element phase), into an antenna-major buffer whose plane `a` is UE physical receive channel `a` (the UL FEP writes `rxdataF[a] <- ue->common_vars.rxdata[a]`). So a real per-branch identity was already present at both call sites and is now used -- strictly better than the explicit-NONE tagging the brief scoped, and it makes the brief's item-5 question (attribute an untagged UL row to the lowest active branch, or drop it?) MOOT: it is attributed CORRECTLY. `available_antennas` is the passive gNB context's own allocated plane count (`min(nb_antennas_rx, PASSIVE_UL_MAX_ANT)`, fixed at `passive_gnb_prepare()`), so a branch mapped past it is skipped and counted rather than slicing an unallocated plane. Measured, not argued: with `rx_branches="0,1"` the pre-change binary logs P13a's untagged fan-in warning (`2 branches active but a CFR producer still submits untagged rows; they are ATTRIBUTED to branch 0, not measured there`) and the post-change binary does not, with all 24 UL-CONFIG per-grant decode lines byte-identical in both the 1-branch and 2-branch configurations. This also closes P10b's review Minor finding 3 (UL submissions silently truncated to plane 0 by `requested_antennas_ = 1`). **P08 is UNAFFECTED and still fully open** -- splitting the UL DECODE per branch is separate, larger work and nothing here attempts it. STILL NOT DONE after P10c:  acquisition time is still not carried alongside identity; the passive DL re-encode's 3000-range `harq_unique_pid` is still not branch-strided. Pre-P10b wording: the other 8 producers (CSI-RS, SSB, blind PDSCH DM-RS, PUSCH pilot/data, attached-UE) still submit untagged; nothing routes, groups, windows or fuses by branch (still ONE engine -- P13's engine-per-branch is separate); acquisition time is not carried alongside identity; and the passive DL re-encode's 3000-range `harq_unique_pid` is still not branch-strided (`docs/passive_branch_namespace_audit.md` sec 4.2). G3 NOT passed |
| [ ] | P11 | Support / allocation provenance | IN_PROGRESS (audit + contract only) | 2026-09-11 | Session 2026-09-11 (P11+P12) below. Deliverable: `docs/cfr_support_and_reference_contract.md` Part P11 — all six CFR producers audited line-by-line on (a) `k_abs` coordinate, (b) `l_sym`/`slot_frac` symbol time, (c) `noise_var` provenance, (d) allocation/branch membership, (e) symbol-level-Doppler wording risk. RESULT: (a) correct in VALUE at 6/6 but UNENFORCED — an out-of-grid `k_abs` is dropped one RE at a time at `sensing_engine.cc:543` with no counter — and SSB normalises modulo `ofdm_symbol_size` not `nof_prb*12` (`nr_isac_ssb_axis.c:11`, inert today); support is also OVERSTATED by SSB (240 claimed from ~60 measured PBCH DM-RS) and PUSCH DM-RS (comb-1 claimed from comb-2), and CSI-RS at `csi_type!=0` reports an RB-aggregate. (b) `l_sym` is DEAD — assigned at `sensing_engine.cc:339`, never read — and FIVE of 6 producers pass `slot_frac = 0.0f` unconditionally (corrected 2026-09-11 fix round 1; an earlier version of this row said four, which contradicted the deliverable's own per-producer table), so their rows are timestamped at the slot boundary (assumed, up to ~464 us at 30 kHz); the sixth, `nr_pdsch_data_aided.c`, computes it only when sub-slot sampling is active and otherwise passes `0.0f` too (`:390-391`) — and `subslot_symbols` DEFAULTS TO 0 (`nr_isac.cc:236`), so at the default configuration it is six of six; even when it does compute it, the model is a uniform 1/14-slot one that ignores the long cyclic prefix. Also flagged in fix round 1: `phy_procedures_nr_ue.c:1417` hardcodes `k_ssb = 0` into `nr_isac_ssb_k_abs()`, so SSB's `k_abs` is shifted on any nonzero-kSSB cell (the source comment at `:1408-1416` documents this; correct for this deployment only). (c) NOT per-branch, and P10a's `branch_id` does not change it: 3 producers pass an ANTENNA-AVERAGED scalar, 2 pass literal `1.0f`, SSB passes `0.0f` — while the per-antenna value already exists and is discarded (`nr_dl_chest_nvar_ant[]`, `nr_dl_channel_estimation.c:31`, already consumed per branch by `nr_pdsch_passive_decode.c:1366-1378`); separately the four producer-side noise quantities are in four unrelated scales yet drive a `1/sigma^2` merge weight (`sensing_engine.cc:541`). (d) source-class and branch membership ARE tracked; per-TRANSMISSION (RNTI/allocation/HARQ) membership is NOT and the ABI cannot express it (`nr_isac.h:102-113`). (e) no symbol-level Doppler claim exists; one PRF wording over-claim noted at `nr_pdsch_data_aided.c:375`. NOTHING FIXED — every finding needs an ABI/DSP change (P13) or a rebuild+test cycle; see the session entry's "Nothing fixed, and why". G3 NOT passed |
| [ ] | P12 | Physical time / reference contract | IN_PROGRESS (audit + contract only) | 2026-09-11 | Session 2026-09-11 (P11+P12) below. Deliverable: `docs/cfr_support_and_reference_contract.md` Part P12. RESULT: `detections[].bistatic_range_m` has THREE meanings under one field name. `sensing_engine.cc:807` computes `range_bin * range_res_m` and subtracts nothing, but `apply_sync_correction(..., delay_reference_bin = 0.0, ...)` (`sensing_engine.cc:784`) shifts the whole window by `-los_bins` when `sto_applied || sfo_applied` (`sync_correction.cc:378-379`), i.e. it puts the admitted direct path on bin 0 IN THE CFR DOMAIN. So (C) sync admitted = bistatic EXCESS path; (B) sync ran, admission failed = raw receiver-window-origin delay with `reject_reason` set; (A) `sync_enable=false` or `rows<3` = raw delay with `reject_reason` the EMPTY STRING and `los_bins` a DEFAULT not a measurement. This CORRECTS the inherited note that "range_m subtracts no LOS reference" — the subtraction exists, conditionally, one layer upstream of the formula. G3 test 5: the CPI-level validity record IS exported and is near-complete (`report_writer.cc:171-177`), but there is NO invalid-reference status — `detect_clean()` runs unconditionally (`sensing_engine.cc:796`) and `range_m` is written for every component (`:801-810`) with no reference check, so an unreferenced detection is byte-indistinguishable from a referenced one apart from a boolean four fields away. "A reflected dominant path is not automatically LOS": the code assumes it IS — `sync_correction.cc:236-238` takes the arg-max of the mean CIR profile, no geometry check, no earliest-arrival preference (the same mechanism as the already-CLOSED 2026-08-11 range-bias root cause, one layer up). "Consistent physical definitions, not identical offsets": NOT YET and structurally blocked — one engine, one `dl_clock_tracker_`, one `report.sync`, rows from different branches pooled into one `PendingRow`, so today it is ONE offset fitted from pooled multi-branch data, which is worse than the error the clause anticipates; needs P13's engine-per-branch. The "~40 m range-axis bias" was re-read as CLOSED, not re-derived, and is recorded as closed. NOTHING FIXED: an omit-on-invalid guard would delete all output from the legitimate `sync_enable=false` mode, and the additive `range_reference` status field is a schema change owned by P16 whose downstream compatibility cannot be verified without a radio. G3 NOT passed |
| [ ] | P13 | Four independent sensing engines | IN_PROGRESS (engine array + routing only) | — | Session 2026-09-11 (P13a) below. DELIVERED, controller-scoped to the ENGINE ARRAY ITSELF: `nr_isac.cc`'s `std::unique_ptr<SensingEngine> engine` singleton replaced by `std::array<std::unique_ptr<SensingEngine>, NR_RX_BRANCH_MAX> engines` INDEXED BY BRANCH ID (not packed to the active count — `nr_rx_branch_set_t::b[]` is itself branch-id-indexed, so with this shape the index IS the identity and no second mapping table exists to fall out of step); one engine constructed per ACTIVE branch in `nr_isac_init()`, all started/stopped together in `nr_isac_start()`/`nr_isac_stop()`; `nr_isac_submit_cfr_multi_branch()` routes to `engines[branch_id]`. Per-branch output identity derived by `branch_pipeline_config()` (`pipeline_types.h`): `rx_id`, `out_path`, `report_path` and the ZeroMQ `report_endpoint` are all made distinct when >1 branch is active, and returned UNCHANGED when exactly one is — keyed on `n_active`, not on `branch_id == 0`, so a single-branch deployment naming any branch id still writes exactly the configured paths. **The safety-critical rule**: a CFR tagged with a branch that has no engine is DROPPED, counted (`dropped_unrouted`, census logged at stop) and logged once per offending id — never folded into another branch's coherent window; `NR_ISAC_BRANCH_NONE` resolves to the LOWEST ACTIVE branch (the legacy engine), with a loud one-shot warning that untagged rows are ATTRIBUTED, not measured, whenever >1 branch is active. `SensingEngine` was AUDITED for singleton assumptions and needed NO internal change: accumulator, CPI planner, clutter map, DL/UL clock trackers, motion/hierarchical trackers, sequence counter, ReportWriter (with its own ZeroMQ context) and both worker threads are all non-static members; the only file-scope mutable state anywhere under `NR_UE_ISAC/` is `detector.cc`'s two `thread_local` shape-keyed CUDA workspace caches (per-thread, hence per-engine, and fully re-initialised per CPI by `reset_cpi()`) and `sync_correction.cc`'s `static std::atomic<bool>` CUDA-failure/log-once latches (shared DEGRADATION, no data path). `test_nr_isac_python_parity` 18 cases (was 15): `test_branch_engine_routing()` (G4 test 3 in pure form — inactive/out-of-range/garbage branch ids all resolve to "drop", never to 0), `test_branch_output_identity()` (the legacy regression pin), `test_branch_engines_are_independent()` (two live engines started together, fed different CFR at different cadences with different branch tags — the first engine's closed CPI is character-identical to the same engine run alone). Both new gates proven non-vacuous by deliberate falsification. Both replay baselines UNCHANGED (34/0/24 and 34/34/31/34/34). **RESOURCE SCALING, flagged before anyone runs four branches live**: N engines means N snapshot pools, N sets of `detector.cc` thread_local CUDA/FFT workspaces and N concurrent CPI working sets — at 273 PRB the per-CPI working set is already what `CRASH_FIXES_IMPLEMENTED.md` §8 root-caused as the `std::bad_alloc`, and the binding limit there was `RLIMIT_MEMLOCK` under `mlockall(MCL_CURRENT|MCL_FUTURE)`, not free memory. Nothing in this task budgets for 4x; measure the locked-memory headroom before a four-branch 273 PRB capture. Fix round 1 (2026-09-11): two real safety defects found in review and fixed — (i) `physical_channel >= 0` and `n_active` were two DIFFERENT active-branch predicates and P03's parser allowed them to disagree (a `rx_branch_phys_map` entry for a branch not named in `rx_branches`), so `rx_branches="0"` + `rx_branch_phys_map="0:0,1:1"` built TWO engines while deriving ONE unsuffixed output identity for both — exactly the silent collision P13a exists to prevent; fixed at the ROOT in `nr_rx_branch.c`'s validation loop (the equivalence is now a parser invariant every `physical_channel >= 0` consumer already assumed) AND independently in P13a by giving both sites one `branch_is_active()` predicate; (ii) `nr_pdsch_passive_branch_view_resolve()` fabricated `branch_id = 0` — a REAL branch — when `ISAC_DL_BRANCH_VIEW` named a physical channel no active branch maps, which P13a had just turned from a report label into a ROUTING decision; now yields `NR_ISAC_BRANCH_NONE` with a one-shot warning, with the HARQ lane explicitly mapped back to 0 so the id namespace is byte-unchanged. Proven live: the same command printed `branch=0` before and `branch=255` + the new warning after, `data_submits=34` either way. Fix round 2 (2026-09-11): a THIRD consumer of `t_view_branch` that both earlier sweeps missed -- `nr_pdsch_passive_decode.c:1118`'s per-branch multilayer-rejection counter indexed by `t_view_branch & (NR_RX_BRANCH_MAX-1)`, so round 1's honest 0xFF sentinel masked to index 3 and credited REAL branch 3's `br3=` counter with an unmapped view's rejections. Diagnostic-only (nothing routes on that array) but the same fabricated-identity pattern relocated from br0 to br3. Fixed with a new pure `nr_rx_branch_counter_index()` in `nr_rx_branch.h` (-1 = names no branch) guarding the per-branch increment; the unconditional aggregate counter is left alone. A complete re-grep confirms this is the LAST consumer: 5 sites total -- the declaration, the one writer, and three readers (HARQ lane, guarded in round 1; the identity accessor, which correctly propagates the sentinel; this counter). 32 gtest cases (was 31). Fix round 3 (2026-09-11): traced every writer of the OTHER variable carrying the same idiom, `nr_pdsch_passive_queue.c`'s `job.branch_id`, and found the SAME round-1 regression at 2 of its 4 masked sites. `nr_pdsch_passive_branch_view_resolve()` at `:190` OVERWRITES `job.branch_id` in place, so the post-resolve census counters `g_br_decoded`/`g_br_crc_ok` could mask 0xFF onto real branch 3; both now go through `nr_rx_branch_counter_index()`. The other two (`g_br_stale_epoch`, `g_br_queued`) read the PRE-resolve value -- written only by the zero-initialised producer job or by `nr_rx_branch_set_dispatch()`, which emits active branches only -- so their masks are correct bounded uses and are deliberately LEFT, with the invariant documented at all three places. Tree-wide grep for both masking idioms now returns only those two bounded uses plus `nr_passive_harq_tag.h:138`'s `%`, which round 1 already guarded at its single call site. All per-branch census arrays are diagnostic; no routing, data or report field was ever affected. NOT DONE: this does NOT remove AoA (P14 Stage B/C, which this task unblocks) and does NOT tag the remaining five CFR producers (P10 continuation) — so with >1 branch active those five still feed the lowest active engine as an attribution. **UPDATE (P10b, 2026-09-11): three of those five are now tagged** (CSI-RS, blind DM-RS, SSB), leaving the two UL producers; and the engine channel count is now derived from the branch set (AoA-removal audit Stage B item 1). G4 NOT passed |
| [ ] | P14 | AoA removal / migration | IN_PROGRESS (audit + Stage A item 1 only) | 2026-09-11 | `docs/aoa_removal_audit.md`: full touchpoint audit by section 2.4 row + A/B/C staged order. Stage A item 1 DONE (AOA_ENABLE/AOA_UL_ENABLE env override + globals removed, obsolete keys rejected loudly). Stage B blocked on P13, Stage C blocked on Stage 5 (P17-P19) — see the session entry. |
| [ ] | P15 | UL scene-support preservation | NOT_STARTED | — | — |
| [ ] | P16 | Reports / consumers | NOT_STARTED | — | — |
| [ ] | P17 | DL geometry / Jacobians / rank | NOT_STARTED | — | — |
| [ ] | P18 | DL association / global estimator | NOT_STARTED | — | — |
| [ ] | P19 | DL admission / OTA comparison | NOT_STARTED | — | — |
| [ ] | P20 | Launcher / manifests | NOT_STARTED | — | — |
| [ ] | P21 | Sustained throughput / isolation | NOT_STARTED | — | — |
| [ ] | P22 | Required pipeline release evidence | NOT_STARTED | — | — |
| [ ] | U00 | Optional feature-off compatibility | NOT_STARTED | — | — |
| [ ] | U01 | Four-channel UL acquisition | NOT_STARTED | — | — |
| [ ] | U02 | Independent UL detections / metadata | NOT_STARTED | — | — |
| [ ] | U03 | DTD/DFS / pre-solver OTA logs | NOT_STARTED | — | — |
| [ ] | U04 | Same-transmission association | NOT_STARTED | — | — |
| [ ] | U05 | Model / actual-geometry observability | NOT_STARTED | — | — |
| [ ] | U06 | WNLS / sliding-window solver | NOT_STARTED | — | — |
| [ ] | U07 | RX-count / measurement-mode comparison | NOT_STARTED | — | — |
| [ ] | U08 | Motion / multipath robustness | NOT_STARTED | — | — |
| [ ] | U09 | Optional deliverables / limitations | NOT_STARTED | — | — |

## Gate summary

| Gate | Meaning | Status | Passed date | Artifact / reviewer |
|---|---|---|---|---|
| G0 | Reproducible baseline and acceptance profile | IN_PROGRESS | — | P02's own sub-condition now MET (2026-09-11, Fix round 2): a reproducible supported DL+UL replay fixture exists (`sensing_manual_fixed.UtvBT7`, `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened`), registered and checker-verified. G0 as a WHOLE still cannot PASS, because P01's own manifest remains IN_PROGRESS on survey/limits (every `deployment_dependent_limits` entry UNSET, `geometry.surveyed=false` -- unchanged by P02, not this task's scope). State plainly: G0 = IN_PROGRESS, blocked only on P01's outstanding survey/acceptance-limit work, not on P02 any longer |
| G1 | Acquisition routing, time and branch isolation | IN_PROGRESS | — | Tests 1-4 exist and PASS in pure/standalone form across P03 (`nr_rx_branch_test`), P04 (`nr_rx_span_pool_test`) and P05 (`nr_rx_branch_sync_test`'s `RxBranchSyncG1Test4`); P05 additionally adds a hardware-isolation guard (`branch_hw_isolation`/`branch_hw_isolation_selftest` ctest entries) covering the "no unauthorized hardware operation" half of the exit criterion at the SOURCE level. G1 as a WHOLE cannot PASS: test 5 (standalone-replay comparison) and end-to-end live-worker isolation require the `nr-ue.c`/`nr-ue-ru.c` read-loop wiring, which no session has done yet (controller-scoped, blocked on that file's owning session committing first). **UPDATED 2026-09-11 (P06a): the LIFECYCLE part of that wiring is now live in `nr-ue.c`** — branch lock / loss-of-lock / RF-discontinuity epoch transitions are driven from the real read loop's own sync, RXDISCONT, RFSTALL and CFOTRK sites, the `nb_antennas_rx` cross-check is wired and fatal, and branch epochs appear on the periodic RFCENSUS line. Still outstanding for G1: test 5 (standalone-replay comparison) and per-branch acquisition/digital-correction state (one `PHY_VARS_NR_UE` / `nr_rx_branch_sync_t` per branch); and NOTHING of the new wiring has live evidence (X410 unreachable 2026-09-11) |
| G2 | Independent supported DL/UL decoding | IN_PROGRESS | — | P07 (DL side) delivers G2 tests 1 and 3 in replay form (`dl_branch_view_replay`: per-view CRC verdicts and reconstructed TBs match the 4-antenna reference wherever the view decodes, a branch's data-aided submissions == its own CRC-OK count). G2 as a whole cannot pass: P06 (PDCCH isolation), P08 (UL), P09 (namespace audit) pending; test 5's failure-class cases are only partly exercised (CRC fail 4-7/38 and multilayer rejection path built but 0/38 on this fixture); no parallel four-view run. Unsupported traffic fraction, live: the cell measured num_layers=4 on ~97 % of grants (decoder's own note), so live independent mode is expected to report ~97 % `unsupported_multilayer_in_branch_view` -- the replay's 0/38 must not be read as the live figure. **UPDATED 2026-09-11 (P06a): still NOT MEASURED.** The path that would measure it is now built (per-branch fan-out + per-branch `unsupported_multilayer` attribution on the `PDSCHQ-BRANCH` line), but the live capture was not attempted — X410 unreachable (`uhd_find_devices`: No UHD Devices Found, 13:48 UTC). Do not read ~97 % as measured; it remains the decoder's inference from the cell's own scheduling. **UPDATED 2026-09-11 (P09) — exit item "no resource namespace collision": the `harq_unique_pid` class is now FIXED for the DL fan-out path** (per-branch stride, `nr_passive_harq_tag.h`, bound asserted at compile time and in test). Two classes remain OPEN and are recorded in `docs/passive_branch_namespace_audit.md`: the passive UL decode tags every transport block `0` across up to 6 concurrent contexts (sec 4.1, → P08) and the passive DL re-encode's 3000 range is not branch-strided (sec 4.2, → P10). The TLS half of the audit found NO thread-local whose branch-dependent content survives into another job (sec 5); `t_view_branch`/`t_view_ue` are the correctly-scoped pattern. So this exit item is PARTIALLY met, and deliberately not claimed as met: the fix is not backed by an observed failure-then-pass, only by code inspection + unit test, because the harness runs one branch view per process and the linked LDPC backend keeps no per-id state. |
| G3 | CFR identity, support and physical references | IN_PROGRESS | — | Contract document exists as of 2026-09-11 (`docs/cfr_support_and_reference_contract.md`, P11+P12 audit); the five G3 tests are INVENTORIED, not implemented. **Coverage: 0 of 5 covered, 2 PARTIAL, 3 not covered.** Test 1 (worker-delay invariance) PARTIAL — the invariant holds STRUCTURALLY (every physical time derives from the producer's `slot_idx + slot_frac`, never wall clock; the deferred PDSCH path publishes the producer's monotonic slot via `nr_isac_abs_slot_override`) and `test_causal_cpi_pipeline()` (`python_parity_test.cc:436`) covers close/drain/restart, but no test varies delay or compares two runs; residual violation: `start_utc_ns` is stamped at `submit()`, on the CONSUMER thread for deferred paths, so it is a PROCESSING time where P16 requires an acquisition-derived one. Test 2 (wrap/reorder/duplicate/RNTI/epoch) PARTIAL — wrap and reorder ARE implemented (`unwrap_submission_slot()`, `sensing_engine.cc:399-412`) and duplicates merge by design on `row_key`, but no test exercises any of them, and RNTI reassignment / epoch reset are invisible at this layer (the ABI carries neither). Test 3 (allocation/pilot sweep vs frequency axis, support masks, symbol timestamps) NOT COVERED — nothing sweeps producer-side support; the mask cannot express interpolated-vs-measured and silently loses out-of-grid REs; symbol timestamps do not exist to verify. Test 4 (known timing/CFO/SFO perturbation) NOT COVERED — `cuda_sync_test.cc:76-101` is a CPU-vs-CUDA PARITY test, not an accuracy test, and the `selftest`/`selftest_los` injection mechanism from older project history DOES NOT EXIST in this tree (zero `selftest` hits under `openair1/`); offline-testable today via `python_parity_test.cc`'s existing synthetic-`CfrWindow` pattern, and judged the highest-value missing test. Test 5 (deliberate admission failure) NOT COVERED and the property does NOT hold — there is no invalid-reference status to assert, so it cannot pass until that field exists. G3 exit also requires reconstruction scope to be explicit, which it is not: findings P11-A3 (interpolated REs reported as measured support), P11-C1 (no per-branch noise), P11-D1 (no allocation membership) and P12-4 (one pooled reference across branches) are all open and all need ABI/engine changes at P13. **G3 NOT PASSED** |
| G4 | Four detectors, AoA removed, UL support retained | IN_PROGRESS | — | P13a (2026-09-11) delivered the ENGINE ARRAY: one SensingEngine per active receive branch, branch-id-indexed, with no-misroute routing and per-branch output identity. **Coverage: test 3 PARTIAL (pure form), tests 2/4 PARTIAL (structural), test 6 not addressed.** Test 2 (four-engine replay == four standalone runs) NOT COVERED end to end -- the replay harness runs ONE process per branch view and never starts the sensing engine at all, so no replay can compare four concurrent engines; the in-process analogue IS covered by test_branch_engines_are_independent() (a second live engine fed different CFR leaves the first engine’s closed CPI character-identical). Test 3 (permuting branch ids / delaying one branch cannot alter another’s maps, histories or thresholds) PARTIAL: the routing half is pinned by test_branch_engine_routing() and the state half follows structurally from every accumulator/planner/clutter-map/tracker/counter being a non-static member (audited, cited in the P13a session entry), but nothing yet permutes ids or delays a branch on real data. Test 4 (invalid/missing UL does not suppress valid DL) holds PER ENGINE already (test_invalid_ul_does_not_suppress_dl()) and now holds ACROSS branches by construction -- separate objects, separate threads, no shared state -- but is unmeasured across branches. Test 6 (parser rejects removed AoA-only keys) belongs to P14 and is untouched. G4 exit also requires eight separately identified DL/UL views and no runtime AoA dependency: neither is reached, because the other five CFR producers are still untagged (P10 continuation) and AoA removal is P14 Stage B/C, which this task unblocks but does not perform. **G4 NOT PASSED** |
| G5 | Observable AoA-free DL global fusion | NOT_STARTED | — | — |
| G6 | Sustained operation and required OTA release | NOT_STARTED | — | — |
| U00 | Optional UL disabled-mode compatibility | NOT_STARTED | — | — |
| UG1 | Four-channel UL acquisition | NOT_STARTED | — | — |
| UG2 | Independent UL target detections | NOT_STARTED | — | — |
| UG3 | OTA direct-path-referenced DTD/DFS | NOT_STARTED | — | — |
| UG4 | Same-UE/same-transmission association | NOT_STARTED | — | — |
| UG5 | Actual receiver geometry, rank and conditioning | NOT_STARTED | — | — |
| UG6 | Observable-mode localization and comparisons | NOT_STARTED | — | — |
| UG7 | Motion, multipath and dropout robustness | NOT_STARTED | — | — |

## Session — 2026-09-10: planning and inspection

**Scope:** read the current sens6 source and configuration, write the implementation specification and initialize the progress ledger. No receiver implementation was requested/executed in this documentation session.

**Files written:**

- `adaptive_RX_pipeline.md`: detailed stages, tests/gates, present/missing blocks, retractions/removals, verified-versus-proposed commands, debugging runbook and optional AoA-free UL phase.
- `adaptive_RX_pipeline_progress.md`: dated task/gate tables and evidence templates.

**Commands/actions performed:** read-only SSH `git branch`, `git rev-parse`, `git status`, `find`, `sed`, `grep` and source/config file listings. Read the publisher's abstract/introduction for DOI `10.1016/j.sigpro.2025.110265`. Created only the two Markdown documents and copied them to the canonical worktree. Documentation checks and copy hashes are recorded below when complete.

**Measured/observed facts:**

1. Branch is `merge/adaptive-sensing` at the recorded commit, with the two pre-existing file changes listed above.
2. Native sensing uses one process-wide engine. AoA-off requests one sensing channel, not four engines.
3. Passive decoding contains shared discovery/queue state and cross-branch diversity behavior.
4. The current global birth path requires AoA; no AoA-free four-RX global fusion is wired into it.
5. Current CFR ABI omits branch/emitter identity and hardware acquisition timestamp. Submission UTC is worker-time stamped.
6. The default adaptive config disables sensing, so the sensing-enabled build fails its launcher guard. The alternate AoA config's linear array does not pass the rank-two parser.
7. Optional UL direct/target path records and a four-RX DTD/DFS solver are absent from the inspected pipeline.

**Reasoned design conclusions, not OTA measurements:**

- Branch isolation and measurement contracts must precede fusion.
- Four unrestricted instantaneous 3-D DTD/DFS receiver pairs supply at most eight scalar measurements for twelve target-plus-moving-UE state unknowns; a full solution is underdetermined without additional information/time structure.
- A short constant-velocity window may improve observability, but rank/ambiguity must be established for the actual geometry and motion.

**Retractions:** see plan section 2.3. No previous hardware CRC numbers have been reused as new validation. The no-UCI reconstruction inference, pilot availability guarantee, AoA-off behavior and readiness of existing run configs were narrowed to what source actually establishes.

**Outcome:** D01/D02 complete. All P/U implementation tasks remain NOT_STARTED. No live validity/performance claims.

**Next action when implementation is authorized:** P01/P02—freeze the baseline manifest and acceptance profile and identify valid channel-specific DL/UL replay evidence; then implement branch ownership/routing. Repair the minimal test harness early if baseline acquisition needs it, without claiming four-branch support exists.

## Session — 2026-09-10: P01 baseline manifest and acceptance profile (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-10 (sens6 host clock reads UTC 2026-09-10T23:xx -- see
  sens4/sens6 clock-skew note; the plan/ledger's own "2026-09-10" heading is consistent with this).
Task IDs / gate: P01 / G0 (Stage 0).
Intended falsifiable claim: a generated manifest's every recorded hash/identity matches the live
  tree/build; a manifest with one hash altered is detected as a mismatch; the shipped acceptance/
  geometry files are correctly rejected as incomplete (UNSET/unsurveyed); a manifest whose selected
  test conf has pdcch_blind_monitor_full_auto=1 is rejected (2026-09-11 TESTING MODE RULE amendment).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing @
  50c8312fa46f17850aeee579abfb0a745f8465ff (unchanged from the plan's baseline). `git status --short`
  at generation time carried MORE dirty files than the plan's recorded two -- see "Current status"
  table above and the manifest's own `notes.tree_dirty_state_vs_plan_baseline` field. None were
  reset/clean/stashed/touched; the manifest generator only reads and records.
Files modified / added / removed: ADDED (this session): tests/passive_rx/baseline_manifest.sh,
  tests/passive_rx/check_manifest.py, tests/passive_rx/acceptance_four_rx.json,
  tests/passive_rx/geometry_four_rx.json, tests/passive_rx/baselines/manifest_20260910_50c8312-
  dirty.json, tests/passive_rx/baselines/manifest_20260910_50c8312-dirty_tracked.patch. MODIFIED:
  none. REMOVED: none. The two pre-existing dirty items (and every other dirty file found at session
  start) were left exactly as found.
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem sha256 b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe
    (62803480 bytes, mtime 2026-09-10T23:09:00Z);
  liboai_usrpdevif.so sha256 ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583
    (3263992 bytes, mtime 2026-09-10T23:02:17Z); liboai_device.so -> liboai_usrpdevif.so;
  tracked-diff patch sha256 f078f2db56c2206386d891e3b9d3a21bca72576c91b718561297869d7ebc1368
    (787 lines, `git diff --binary` over ALL currently-dirty tracked files, not only the two the
    plan named);
  adaptive_manual_diag.conf sha256 86559555d95505b1d45c710c71bf5cbce97fde55e2e8cb2d423a0619332dba18;
  acceptance_four_rx.json sha256 57896964efde38903a8c4537dd09cc6d92e7c2f565a101bbf1778cbc42e6ad8b;
  geometry_four_rx.json sha256 f27963008c0d63279c25f3fdc3af846d43563b6fffa4af05181940b2532bcdb1;
  manifest itself sha256 f5ffa616687535cd5fe8e16f97bf8579b13dfa2618a0babf40d27ad67da170a2.
  Full per-file list (2004 scoped files, all 5 case confs, CMake cache, compiler/UHD versions):
  tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json.
Exact commands:
  bash tests/passive_rx/baseline_manifest.sh
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json
  python3 tests/passive_rx/check_manifest.py --acceptance tests/passive_rx/acceptance_four_rx.json --geometry tests/passive_rx/geometry_four_rx.json
  python3 tests/passive_rx/check_manifest.py --selftest
  (No build, no radio, no launcher invocation. run_adaptive_receive_test.sh was read, never run.)
Artifact paths (include raw logs and VOID attempts): tests/passive_rx/baselines/manifest_20260910_
  50c8312-dirty.json (+ sibling _tracked.patch); full command transcripts in the P01 task report
  (/home/sens/NICOLA/.superpowers/sdd/adaptive_RX_pipeline/task-P01-report.md, local to the
  controlling session, not part of this repo).
Baseline and comparison definition: "live tree/build" = the sens6 checkout at the moment each
  command ran (git working tree + cmake_targets/ran_build/build), recomputed fresh by
  check_manifest.py -- never compared against a second copy of the manifest itself.
Predeclared assertions / thresholds: none deployment-dependent (Stage 0 has no scoring thresholds);
  the plan's EXACT invariants (section 3.3/4/5.4) and the 2026-09-11 full_auto=0 rule are the only
  frozen checks at this stage, both encoded in acceptance_four_rx.json.
Observed result, with denominators:
  - check_manifest.py against the fresh manifest: 0 mismatches ("MANIFEST OK"), 1 informational
    note (the manifest's own output file postdates its own scoped-file snapshot -- unavoidable
    self-reference, not scored as a mismatch).
  - check_manifest.py --acceptance/--geometry against the shipped v1 files: 15 reasons reported
    (14 UNSET deployment-dependent limits + geometry not surveyed) -- correctly INCOMPLETE.
  - check_manifest.py --selftest: 4/4 cases behaved as predeclared -- (a) fresh manifest PASS,
    (b) one binary hash altered -> FAIL, (c) shipped acceptance/geometry -> FAIL, (d) selected_test_
    conf swapped to adaptive_no_hints.conf (full_auto=1) -> FAIL. SELFTEST OVERALL: PASS.
  - Launcher-guard replication finding (measured, not assumed): against the CURRENT cache
    (ENABLE_ISAC_SENSING:BOOL=ON), every enable=0 (decoding-only-shaped) conf's own
    sensing/build guard would report BLOCKED -- the guard requires ENABLE_ISAC_SENSING:BOOL=OFF
    for an enable=0 conf. Only the enable=1 (sensing-enabled) confs currently pass that specific
    guard against this build. Recorded per-conf in the manifest; not exercised by running the
    launcher.
  - aoa_track_dl.conf's rx_array ("0,0;0.0434,0;0.0868,0;0.1302,0") independently verified to fail
    the rank-two AoA parser (openair1/PHY/NR_UE_ISAC/nr_isac.cc:102-125, condition at :118): the 3
    baseline vectors are exactly collinear (Gaussian-elimination rank 1), a sufficient condition for
    that parser's eigenvalue test to reject it regardless of numerical tolerance. Plan section 5.3's
    claim CONFIRMED, not merely repeated.
Status (PASS / FAIL / VOID / BLOCKED): IN_PROGRESS. Not PASS: the geometry survey and every
  deployment-dependent acceptance limit are BLOCKED (no survey performed; no application/deployment
  requirements supplied to derive limits from -- inventing them was explicitly disallowed).
Validity reasons and affected intervals: N/A -- no capture was taken this session (read-only
  identity/config work only).
Hypotheses supported / contradicted: supported -- aoa_track_dl.conf's rx_array fails the rank-two
  parser (plan section 5.3, now independently verified rather than inherited). Supported -- against
  the CURRENT build cache, every currently-existing enable=0 conf is launcher-BLOCKED for sensing/
  build mismatch, not just conceptually "decoding-only shaped" (a new, more precise finding than the
  plan recorded). Neither contradicts anything in the plan.
Retraction, if any: none. (Two STALE mentions elsewhere in this file -- "existing untracked file:
  aoa_track_dl.conf" and "existing modified file: run_adaptive_receive_test.sh" in the original
  2026-09-10 baseline-fact table -- are not retracted, they describe the plan's OWN inspection
  baseline; the "Current status" table above adds, rather than replaces, the row with what was
  actually measured at P01 session time.)
Remaining limitation: no conf in the repository currently matches "acquisition-only" (sensing
  disabled AND both DL and UL blind decode disabled) -- recorded as `"conf":null,"reason":"no
  config exists"` per the brief's instruction, not fabricated. Antenna-to-physical-channel mapping
  and receiver geometry remain UNSURVEYED; the acceptance profile is frozen for exact invariants
  only. P02 (reproducible DL/UL replay fixtures) has not started.
Next highest-value action: P02 -- select or capture a bounded receiver-only OTA fixture (manual
  mode, full_auto=0) sufficient to replay a known-valid supported single-antenna DL and UL case, per
  the plan's Stage 0 tests. A physical antenna survey is also a prerequisite for any future G5 work
  but is not on P02's critical path.
Reviewer / accomplishment date if gate passed: gate NOT passed; no reviewer assigned.
```

**Additional measured notes not captured by the template above:**

- **The tree carries more dirty state than the plan's own 2026-09-10 baseline row records.**
  `git status --short` at P01 session time additionally showed 5 more modified tracked files
  (`executables/nr-ue-ru.c`, `executables/nr-ue.c`, `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `radio/USRP/usrp_lib.cpp`,
  `tests/passive_rx/monitor/monitor.html`, `tests/passive_rx/monitor/monitor.py`) and 3 more
  untracked files (`tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/run_manual_x410.sh`) beyond the plan's recorded
  `M tests/passive_rx/run_adaptive_receive_test.sh` / `?? tests/passive_rx/aoa_track_dl.conf` pair.
  This is recorded as a measured fact (full list in the manifest's `git.status_short`), not resolved
  or explained further under P01 -- P01's job is to record identity, not to audit unrelated dirty
  work. None of it was touched, reset, or committed by this session.
- **`adaptive_manual_dlul.conf`** (untracked, part of the extra dirty state above) is a fifth
  `sensing = {...}` config discovered while classifying P01's three required cases. It is
  sensing-enabled (enable=1), full_auto=0 (admissible), and has BOTH DL and UL blind decode
  enabled with a pinned dedicated CORESET/search-space/BWP -- recorded as an additional
  `sensing-enabled` candidate in the manifest alongside `aoa_track_dl.conf` (the brief's named
  primary), not chosen as primary since the brief specifically names `aoa_track_dl.conf`.
- **Both sensing-enabled candidate confs set `pdcch_blind_monitor_pdsch` decode field to `1`**
  ("decode+count CRC pass rate only", per `nr_pdcch_blind_monitor.c`'s own config-key comment),
  not `2` ("also submit the reconstructed CFR"), while their own `sources=` lines list `pdsch_data`
  as a sensing source. Recorded as measured in the manifest; NOT investigated further here --
  P01 is identity/config recording, not decode-path validation (that is P02+ scope).

### Fix round 1 (2026-09-10, same day) — reviewer findings addressed

Coordinator review of the P01 session above returned "Approved with one Important finding + two
minors." All three addressed, commit `e4c8cadd5f22b40605f82f6adedc838ee8e056f5` (short
`e4c8cadd5f`), on top of the P01 commit `6fcb7a6c319d7ae7ad4b6cb03c075de0e2f7a77a` (short
`6fcb7a6c31`).

1. **IMPORTANT**: `antenna_mapping_and_geometry` and `timestamp_units` citations were hand-typed
   strings, unverified by `check_manifest.py`, and two had already drifted from the live source
   (`nr_isac.cc` `parse_array` recorded as "102-125", actually 102-128; `usrp_lib.cpp` RFCHAN
   recorded as "2485-2494", actually 2486-2496 for that specific `LOG_I` call). Fixed:
   `baseline_manifest.sh` gained `citation()`/`citation_json()` -- every citation is now a grepped
   `{file, line_start, line_end, token}` object, derived at generation time, never hand-typed
   (including a "loud, not silent" `CITATION_ERROR` to stderr if an end-token search comes up
   empty, after that exact failure mode silently produced a wrong 102-102 range on the first
   attempt at the `parse_array` citation -- caught before committing, not after).
   `check_manifest.py` gained `check_citations()` (recursive walk for `{file,line_start,line_end,
   token}` objects, re-verifies `token` still occurs in `[line_start,line_end]` of the LIVE file),
   wired into `check_identity()`. `--selftest` gained case (e): a copy with one citation's line
   range shifted by 500 must FAIL.
2. **MINOR**: `check_identity()`'s `os.readlink(liboai_device.so)` had no missing-file guard --
   wrapped in `try/except OSError`, now a listed mismatch rather than an unhandled crash.
3. **MINOR**: regenerated the manifest at the current HEAD (this branch's own prior P01 commit,
   `6fcb7a6c31`, which is the CORRECT thing for a fresh generation to record -- a manifest
   necessarily predates the commit that adds it, and re-running it after a commit to "catch up" is
   the pre-existing, already-documented self-reference limit, not a new one) and replaced the old
   `baselines/manifest_20260910_50c8312-dirty.json` (+ patch) pair via `git rm` rather than
   accumulating both, per the reviewer's instruction that `baselines/` holds one manifest per
   generation.

**Commands + output** (selftest run BEFORE the fix-round-1 commit, at the regenerated manifest's
own HEAD `6fcb7a6c31` -- this is the valid evidence; a run of the SAME command AFTER committing
would correctly show case (a) failing on `git.commit` drift, since the manifest was generated
before the commit that adds it, exactly as documented for the original P01 session above):

```text
$ python3 tests/passive_rx/check_manifest.py --selftest
(a) fresh manifest (manifest_20260910_6fcb7a6-dirty.json) expected PASS: PASS
(b) manifest with altered nr_uesoftmodem.sha256 expected FAIL: PASS
(c) shipped acceptance/geometry files expected FAIL (unset/unsurveyed): PASS
    <14 UNSET lines + "geometry: surveyed=False (must be true)">
(d) manifest with selected_test_conf full_auto=1 (adaptive_no_hints.conf) expected FAIL: PASS
(e) manifest with a shifted citation line range expected FAIL: PASS

SELFTEST OVERALL: PASS
```

**Files changed this round** (commit `e4c8cadd5f`): `tests/passive_rx/baseline_manifest.sh`
(modified), `tests/passive_rx/check_manifest.py` (modified),
`tests/passive_rx/baselines/manifest_20260910_50c8312-dirty.json` + `_tracked.patch` (removed),
`tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json` + `_tracked.patch` (added; git
recorded these as renames, same effect). No other file touched; the pre-existing dirty state
documented in the P01 session above is unchanged and was not re-inspected this round.

**`git status --short` after this round's commit:**

```text
 M executables/nr-ue-ru.c
 M executables/nr-ue.c
 M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c
 M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
 M radio/USRP/usrp_lib.cpp
 M tests/passive_rx/monitor/monitor.html
 M tests/passive_rx/monitor/monitor.py
 M tests/passive_rx/run_adaptive_receive_test.sh
?? adaptive_RX_pipeline.md
?? adaptive_RX_pipeline_progress.md
?? tests/passive_rx/MANUAL_DL_UL.md
?? tests/passive_rx/adaptive_manual_dlul.conf
?? tests/passive_rx/aoa_track_dl.conf
?? tests/passive_rx/run_manual_x410.sh
```

Identical dirty/untracked set to before this round (only the already-documented pre-existing
extras) -- confirms nothing beyond the intended 4 paths was touched.

## Session — 2026-09-11: P02 fixture audit and bounded capture (BLOCKED)

```text
Date/time (Europe/Zurich): 2026-09-11 (sens6 host clock reads UTC; capture window
  2026-09-11T00:14:34Z - 2026-09-11T00:17:39Z; see sens4/sens6 clock-skew note -- both timestamps
  are sens6's own clock, no cross-host comparison made).
Task IDs / gate: P02 / G0 (Stage 0).
Intended falsifiable claim: at least one existing or newly-captured replay.bin, produced by the
  CURRENT binary under manual mode (full_auto=0), replays with >=1 successful DL control and >=1 UL
  record and no radio opened; a fixtures.json entry with one hash altered is rejected by the
  checker.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing @
  e4c8cadd5f22b40605f82f6adedc838ee8e056f5 (unchanged this session -- P02 did not commit before
  writing this entry; see "Files modified" below for what is staged to commit).
  `git status --short` at session end: unchanged pre-existing dirty set (5 modified tracked files
  + `tests/passive_rx/run_adaptive_receive_test.sh` modified, `adaptive_RX_pipeline.md`,
  `adaptive_RX_pipeline_progress.md`, `tests/passive_rx/MANUAL_DL_UL.md`,
  `tests/passive_rx/adaptive_manual_dlul.conf`, `tests/passive_rx/aoa_track_dl.conf`,
  `tests/passive_rx/run_manual_x410.sh` untracked) PLUS this session's
  ` M tests/passive_rx/check_manifest.py` and new untracked
  `tests/passive_rx/baselines/fixture_audit_20260911.md`, `tests/passive_rx/baselines/fixtures.json`,
  `tests/passive_rx/replay_header.py`. Nothing reset/stashed/clobbered.
Files modified / added / removed: ADDED: tests/passive_rx/replay_header.py,
  tests/passive_rx/baselines/fixture_audit_20260911.md, tests/passive_rx/baselines/fixtures.json.
  MODIFIED (to be committed): tests/passive_rx/check_manifest.py (new check_fixtures() function,
  --fixtures CLI mode, selftest case (f)). MODIFIED but NOT committed (another session's untracked
  file; diff recorded in the fixture audit doc instead, per the brief): tests/passive_rx/
  run_manual_x410.sh (REPLAY=1 opt-in, byte-for-byte identical behaviour when REPLAY is unset --
  verified by rendering the effective env -i argument list both ways). REMOVED: none.
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem sha256 b064c9957bc1b68d717543eb2bac28c4b2616115bc675f5fed38685f008742fe (same as
    the P01 manifest's recorded value -- binary unrebuilt since P01, confirmed by
    check_manifest.py NOT listing it as a mismatch even though git.commit/tracked_diff_patch_sha256
    now DO mismatch, see below);
  liboai_usrpdevif.so sha256 ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583
    (also unchanged/matching);
  attempt-2 receiver.conf sha256 a39e40f00e4ad0f456cb2b5c947f6e1e6bdbdadef069306231ef3075349ade92;
  attempt-2 run.log sha256 b9618723f3440b3832599c6b205a87d053937fdf2bac5b3c6ff6fdc48f830a6b.
Exact commands:
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260910_6fcb7a6-dirty.json
    (run BEFORE any P02 edit -- found 2 pre-existing mismatches, see "P01 manifest staleness" below)
  pgrep -a -x nr-uesoftmodem; pgrep -a -x nr-softmodem
  timeout 10s uhd_find_devices --args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174
  find openair1 openair2 executables radio -path '*/tests/*' -prune -o \
    ( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' ) \
    -newer cmake_targets/ran_build/build/nr-uesoftmodem -print -quit
  sudo -n python3 tests/passive_rx/replay_header.py <replay.bin>   (x22, all pre-existing fixtures)
  sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8090 bash tests/passive_rx/run_manual_x410.sh   (attempt 1)
  sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8091 bash tests/passive_rx/run_manual_x410.sh   (attempt 2)
  python3 tests/passive_rx/check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json
  python3 tests/passive_rx/check_manifest.py --selftest
  (No cmake/build command run at any point. No radio operation outside the two launcher invocations
  above, both through the manual launcher only.)
Artifact paths (include raw logs and VOID attempts):
  /home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI (attempt 1, VOID_RF_OR_ASSERT);
  /home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb (attempt 2, RF-valid, no replay.bin);
  full transcript: /home/sens/NICOLA/.superpowers/sdd/adaptive_RX_pipeline/task-P02-report.md
  (local to the controlling session, not part of this repo);
  tests/passive_rx/baselines/fixture_audit_20260911.md (full audit table + root-cause citations);
  tests/passive_rx/baselines/fixtures.json (registry, status=BLOCKED, admissible_fixtures=[]).
Baseline and comparison definition: "admissible" per the brief = same binary sha256 AND
  full_auto=0 AND >=1 successful DL record AND >=1 UL record, recomputed live by
  tests/passive_rx/replay_header.py + check_manifest.py --fixtures, never assumed from a filename
  or an old handover claim.
Predeclared assertions / thresholds: none deployment-dependent; the plan's exact invariants
  (full_auto=0, same-binary identity, no ground-truth-timing source) are the only frozen checks.
Observed result, with denominators:
  - 22/22 pre-existing replay.bin fixtures: INADMISSIBLE. All 22 fail on BOTH independent grounds
    (producing binary != current build's b064c995...; full_auto=1). Every one passed the reader's
    own internal self-consistency checks (magic match, file_bytes==header_bytes+iq_bytes) -- these
    are genuine, correctly-parseable captures, just not usable as a P02 baseline. Full table:
    tests/passive_rx/baselines/fixture_audit_20260911.md section 2.
  - Recorder bound check (brief requirement): iq_bytes=314,572,800 <= 512 MiB (536,870,912) for
    16 frames x 1,228,800 samples/frame (= 122.88 Msps x 10 ms, confirms 273 PRB geometry) x 4
    antennas x 4 B; matches every existing fixture's exact 316,268,528-byte size
    (header_bytes=1,695,728 + iq_bytes).
  - New capture attempt 1 (sensing_manual_fixed.XmtvnI): VOID_RF_OR_ASSERT. RFSTALL
    (ERROR_CODE_OVERFLOW) at the very first USRP_RX_START; 0 "SIB1 common facts"; nic_missed
    unchanged. Cold-start RF variance, not investigated further per the memory rule (kept as
    evidence, not retried indefinitely).
  - New capture attempt 2 (sensing_manual_fixed.JPdoGb): validity.txt=
    RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, stop_reason=DURATION_COMPLETE, process_exit=0,
    cleanup_status=CLEAN, nic_missed before==after (27520822), SIB1 count=1, RXDISCONT/RFSTALL=0/0,
    receiver.conf pdcch_blind_monitor_full_auto=0 (confirmed), binary sha256 matches current build.
    DL: PDSCHQ crc_ok=38583/42014 (91.8%). UL: pusch_passive crc_ok=10562/12694 (83.2%). By every
    radio-level criterion this is a good, valid manual-mode capture. BUT: grep -aic replay run.log
    = 0 (not even a recorder failure-path log line fired), no replay.bin file exists in $OUT.
  - Root cause (proven from source, cited file:line, independently re-verified against the live
    tree with `sed -n '<range>p' <file> | grep -F -- '<token>'` for all 4 citations, all FOUND):
    nr_passive_replay_capture.c:206-209's ONLY arm-trigger requires
    job->sweep_ticket.generation != 0; sweep_ticket is populated ONLY inside
    nr_pdcch_blind_monitor_rt.c:2100's `if (g_pdsch_sweep_on && !is_dci10)` block (outside it,
    :2098 leaves it all-zero); g_pdsch_sweep_on is set in pdsch_sweep_maybe_enable() to
    `ready = cfg->dl_full_auto && (...)` (:164-166,184) -- REQUIRES full_auto=1. Under the mandatory
    TESTING MODE RULE (full_auto=0), this chain is provably always false, for every job, in every
    run, regardless of RF quality or duration. Not inferred from one run: the single call site of
    nr_pdsch_config_sweep_select() (the only place sweep_ticket.generation is ever assigned) is
    inside that same full_auto-gated block.
  - Attempt 3 NOT spent: the failure is deterministic/structural (proven above), not RF-variance;
    a third identical-code-path run cannot produce a different outcome, and running it anyway would
    be indistinguishable from retrying/tuning to force a pass, which the plan forbids.
  - check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json: "FIXTURES REJECTED (1):
    admissible_fixtures is empty" -- exit 1, correctly rejects (there is nothing to accept).
  - check_manifest.py --selftest: new cases (f1) synthetic single-fixture registry PASS,
    (f2) altered replay_bin_sha256 in a copy -> FAIL (correctly detected), (f3) real shipped
    fixtures.json -> FAIL (correctly detected, empty registry). Pre-existing (b)/(c)/(d)/(e) still
    PASS unchanged. (a) "fresh manifest passes" now FAILS -- see "P01 manifest staleness" below;
    this is NOT introduced by P02's logic, it is P01's manifest being older than the live tree.
    SELFTEST OVERALL: FAIL (driven entirely by pre-existing (a), not by any new case).
  - P01 manifest staleness (measured BEFORE any P02 edit, so not caused by this session):
    check_manifest.py against manifest_20260910_6fcb7a6-dirty.json reported 2 mismatches
    (git.commit: manifest=6fcb7a6c... live=e4c8cadd5f...; git.tracked_diff_patch_sha256 mismatch)
    -- the "P01 fix round 1" commit landed after the manifest was generated. build.nr_uesoftmodem.
    sha256 was NOT among the mismatches (binary unrebuilt). This session's own edits to
    tests/passive_rx/check_manifest.py and tests/passive_rx/run_manual_x410.sh add two more
    expected scoped_file diffs on top of that pre-existing staleness. Regenerating the P01 manifest
    is P01's scope, not done here.
Status (PASS / FAIL / VOID / BLOCKED): BLOCKED. Deliverable 1 (fixture audit) PASS. Deliverable 2
  (bounded capture) executed correctly (2 attempts, one RF-valid) but did not yield a usable
  fixture -- BLOCKED by a structural code gap outside P02's scope, not a capture-quality failure.
  Deliverable 3 (replay verification) NOT PERFORMED -- no fixture exists to verify (correctly not
  attempted; nothing to run ISAC_PASSIVE_REPLAY_INPUT against). Deliverable 4 (fixtures.json +
  checker extension + selftest) PASS on its own terms -- the checker code is built and correctly,
  demonstrably rejects both a synthetic altered-hash fixture and the real empty registry. G0: was
  IN_PROGRESS, now correctly downgraded to BLOCKED (see gate table) -- "at least one reproducible
  supported DL and UL case" is not met and cannot be met by more capture attempts alone.
Validity reasons and affected intervals: attempt 1 VOID_RF_OR_ASSERT (RFSTALL at stream start,
  whole run affected, 0 usable data). Attempt 2 RF-valid for its own SIB1/DL/UL/NIC/exit criteria
  (whole 180 s window usable for THOSE purposes) but yields no replay evidence at all (0/180s
  usable for the replay-fixture purpose specifically) -- two different validity questions about the
  same run, both recorded rather than collapsed into one label.
Hypotheses supported / contradicted: contradicted -- the working assumption (carried in the plan's
  own P02 description and MANUAL_DL_UL.md) that a REPLAY=1 opt-in on the manual launcher would be
  sufficient to produce a manual-mode replay fixture. It is necessary (the launcher now correctly
  passes the env vars, verified byte-for-byte) but not sufficient: the recorder's arm condition is
  wired exclusively to the full-auto sweep mechanism the TESTING MODE RULE forbids running.
Retraction, if any: none of P02's own prior claims (P02 had not started). Not a retraction of P01.
Remaining limitation: no admissible replay fixture exists; G0 cannot pass until either (a)
  nr_passive_replay_capture.c's arm condition is changed to not require sweep_ticket.generation
  when cfg->dl_full_auto=0 (a real design decision about what "settled" means for a job with no
  sweep, belonging to that file's owner, out of P02's scope), or (b) an alternative reproducible-
  fixture mechanism is agreed for manual mode. The 22 pre-existing fixtures remain available as
  DM-RS/PDSCH decode reference material (all internally well-formed, per the audit) but cannot
  serve as a P02 baseline under this plan's binary/full_auto invariants.
Next highest-value action: decide (with whoever owns nr_passive_replay_capture.c /
  nr_pdcch_blind_monitor_rt.c) whether to relax the recorder's arm condition for full_auto=0, or to
  define a different manual-mode-compatible arm signal (e.g. "N consecutive successful DL+UL jobs"
  instead of a sweep-settled ticket) -- then re-run P02's capture step once that lands. Until then,
  P02/G0 stay BLOCKED and no later stage should be scored against "a validated replay baseline
  exists".
Reviewer / accomplishment date if gate passed: gate NOT passed; no reviewer assigned.
```

### Fix round 1 + Fix round 2 (2026-09-11, same day) — controller-ruled: recorder arm condition is a plan/code defect, fixed in scope

The P02 session above reported the replay recorder BLOCKED by a structural defect and recommended
against self-directed patching. The controller reviewed, ruled it in scope as a small, targeted
fix, and directed two rounds of work (a session restart occurred between rounds; the tree state
-- uncommitted diagnostics, the 00:55 UTC binary, the staged manifest rename, and captures
3NB5Ri/JLDoZc/LH2I9w/Ma654f -- survived it and was continued rather than redone).

**Defect 1 (DL gate, fixed fix round 1).** `nr_passive_replay_capture.c`'s `nr_passive_replay_dl()`
(~line 201-209) required `job->sweep_ticket.generation != 0` to arm. `sweep_ticket` is populated
only inside `nr_pdcch_blind_monitor_rt.c:2100`'s `if (g_pdsch_sweep_on && !is_dci10)` block, and
`g_pdsch_sweep_on` requires `cfg->dl_full_auto` true (`:164-166,184`) -- structurally unreachable
under the plan's mandatory manual mode. **Ruling**: treat `generation==0` (manual mode, no sweep in
progress) as an already-settled ticket. Fixed both gates in `nr_passive_replay_dl()`, 2-line comment
citing the manual-mode rule, `LOG_I` "REPLAY geometry ..." kept in `nr_passive_replay_init()`.

Rebuilt (binary `b81b21e37dae9ad4666319274a6e32134055329eec150af3c75af4a89578c5d7`), regenerated the
manifest, captured twice (`3NB5Ri` RF-valid but still 0 replay activity; `JLDoZc` VOID_RF_OR_ASSERT
from an unrelated late-run RFSTALL). **Defect 1's fix alone was necessary but NOT sufficient** --
`3NB5Ri` proved this empirically (RF-valid, high DL/UL volume, still zero `replay`-related log
lines).

**Diagnosis, not more blind retries.** Rather than spend the 3rd allowed capture attempt on an
identical code path, added temporary `LOG_A` instrumentation (init geometry, armed-state, per-call
`dl#`/state/status/success/gen/settled/ul_seen, first-UL-seen one-shot, arm-trigger one-shot),
rebuilt twice more (`5745d305...`, DURATION=30; `74d2b818...`, DURATION=60 with the first-UL
diagnostic added), and ran two SHORT diagnostic captures (`LH2I9w`, `Ma654f`) -- explicitly NOT
counted against the 2-attempt cap, since their purpose was instrumentation, not fixture production.
`LH2I9w` showed `dl#1..20` with `success=1` repeatedly but `ul_seen=0` always. `Ma654f` (60s) never
printed "first UL seen" at all, despite `PDSCHQ decoded=7644 crc_ok=7050` and
`pusch_passive try=6114 crc_ok=5010` in the SAME run -- proving `nr_passive_replay_ul()` was never
called even once, while ordinary DL/UL decode volume was high.

**Defect 2 (UL gate, found by this diagnosis, fixed fix round 2, controller-verified).**
`nr_pdcch_blind_monitor_rt.c:1738-1742` called `nr_passive_replay_ul()` only inside
`if(cand_task[ti].ok && cand_task[ti].ul_auto)`. Manual mode pins `dci01 = "1:43"`, so `ul_auto` is
always false and the hook never fires -- `ul_seen` never becomes true, so the (now-fixed) DL arm
condition's `atomic_load(&ul_seen)` clause can never pass either. **Confirmed before fixing** (per
the ruling's own instruction to cite, not guess) that `boot_rnti`, `dci01_length` and
`ul_out.raw_payload` are all populated in the manual/pinned path independent of `ul_auto`:
`dci01_length` at `nr_pdcch_blind_monitor_rt.c:1071-1074` (unconditional whenever `scan_01`),
`boot_rnti` at `:1452-1460` (gated only on a confirmed-RNTI-set hit, not on `dl_full_auto`), and
`ul_out.raw_payload` at `:555-556` inside `nr_pdcch_blind_cand_worker_body()`'s
`nr_pdcch_blind_decode_01_mode()` call (runs whenever `t->ul_scan`, `ul_auto` is passed as a
parameter INTO the decode, not a gate on whether it runs). **Ruling**: move the
`nr_passive_replay_ul(...)` call so it fires once whenever `cand_task[ti].ok`, ahead of the existing
`if(cand_task[ti].ok && cand_task[ti].ul_auto)` block (which keeps gating DISCOVERY --
`nr_pdcch_ul_discovery_grant()` -- unchanged), and delete the copy inside that block. Applied
exactly as specified; 6-line comment added citing the three file:line facts above.

Trimmed the diagnostics to the two the ruling said earn their keep: the `REPLAY geometry ...` line
in `nr_passive_replay_init()` (kept, `LOG_I`) and ONE one-shot `LOG_I` "REPLAY ARMED at
absolute_slot=%ld" at the successful `RP_ARMED`->`RP_REQUESTED` transition inside
`nr_passive_replay_dl()`. Deleted: the per-call `dl#`/`diag_count` block and the first-UL one-shot.
Confirmed clean (`grep -c REPLAYDIAG` = 0 in both files).

**Pre-build check honored a live conflict.** Before rebuilding, `pgrep -x nr-uesoftmodem` showed
another session's autonomous multi-probe scan actively running (`adaptive-rx-UL-DL` tree,
`captures/autonomous_lan_8min_20260911_072035_3354627/`). Per instruction, NOT killed -- polled
`pgrep -x nr-uesoftmodem` every 60s (background poll, then a coordinator-directed foreground
`ssh ... for i in $(seq 60); do pgrep ... || exit 0; sleep 60; done; exit 1` capped by the tool at
600s and continued in the background) until it exited on its own (~free within the first poll
interval after the second instruction landed). Radio confirmed `claimed: False` before proceeding.

**Rebuild**: `cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem oai_usrpdevif
--parallel 4`. Final binary sha256
`c3810019905f3fb661138d12fd5b9b6156d47e36344ae871f08047ba65dc7889` (only
`nr_pdcch_blind_monitor_rt.c.o`/`nr_passive_replay_capture.c.o` recompiled, relinked; one
pre-existing unrelated `-Wformat-zero-length` warning at `nr_pdcch_blind_monitor_rt.c:1291`, not
from this change).

**Manifest regenerated a final time** (one-pair rule: `git rm` the round-1-commit pair, regenerate
fresh so the new manifest's own scoped-file snapshot doesn't reference now-deleted siblings --
same self-reference trap P01's "Fix round 1" already documented) after ALL round-2 source/checker/
fixtures.json edits landed, so `check_manifest.py --selftest` case (a) genuinely reflects the final
state, not an intermediate one:
```text
$ python3 tests/passive_rx/check_manifest.py --selftest
(a) fresh manifest (manifest_20260911_b90276d-dirty.json) expected PASS: PASS
(b) manifest with altered nr_uesoftmodem.sha256 expected FAIL: PASS
(c) shipped acceptance/geometry files expected FAIL (unset/unsurveyed): PASS
(d) manifest with selected_test_conf full_auto=1 (adaptive_no_hints.conf) expected FAIL: PASS
(e) manifest with a shifted citation line range expected FAIL: PASS
(f1) synthetic single-fixture registry expected PASS: PASS
(f2) synthetic registry with altered replay.bin hash expected FAIL: PASS
(f3) shipped fixtures.json expected PASS (>=1 admissible fixture registered): PASS

SELFTEST OVERALL: PASS
```
Note case (f3)'s assertion direction flipped from round 1 (was "expected FAIL, empty registry") to
"expected PASS, >=1 admissible fixture registered" -- the shipped `fixtures.json` is no longer
empty by construction, so the OLD assertion would now be testing a stale assumption, not the
checker's correctness. The REQUIRED negative case per the brief (a copy with one hash altered is
rejected) is case (f2), on a synthetic registry, unaffected by this.

**Capture (final, fix round 2)**:
```text
$ sudo -n env REPLAY=1 DURATION=180 MONITOR_PORT=8096 bash tests/passive_rx/run_manual_x410.sh
OUTPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7
verdict=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE cleanup=CLEAN exit=0
```
validity=RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, stop_reason=DURATION_COMPLETE, process_exit=0,
cleanup=CLEAN, SIB1=1, nic_missed before==after (27607564), RXDISCONT/RFSTALL=0/0, full_auto=0
confirmed, binary matches. `REPLAY ARMED at absolute_slot=8852`.
`REPLAY READY: .../replay.bin slots=320 UL=24 DL-controls=38 IQ=314572800 bytes`.
`replay_header.py`: version=2, n_ul=24, n_dl=38 (34 success / 4 failure), file_bytes match=True.
1 attempt sufficed (of 2 allowed).

**Replay verification (no radio, deliverable 3)**:
```text
$ cd cmake_targets/ran_build/build && sudo -n env \
    ISAC_PASSIVE_REPLAY_INPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7/replay.bin \
    LD_LIBRARY_PATH=$(pwd):/usr/local/lib ./nr-uesoftmodem \
    -O /home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7/receiver.conf \
    -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 --ue-rxgain 40 --ue-nb-ant-rx 4 \
    --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 \
    --freq-sync-I 0.001 --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
    --ntn-initial-time-drift -4.25 -A 90
...
REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened

real	0m4.463s
$ echo $?
0
```
Confirmed `nr-uesoftmodem.c:428-434` (replay branch returns before any radio/UHD call) -- and the
tool's own "no radio opened" text in the PASS line is a second, independent confirmation.
No-ground-truth-timing check: `nr_passive_replay_read()` (`nr_passive_replay_capture.c:225-303`)
reads only the file's own header/IQ/job metadata (source-index continuity at `:266-269`) -- no gNB
log, GT file, or wall-clock alignment opened anywhere in it or its callee
`nr_pdsch_passive_decode()`.

**Fixture registration**: `tests/passive_rx/baselines/fixtures.json` `status` BLOCKED -> PASS,
`admissible_fixtures` `[]` -> 1 entry (`sensing_manual_fixed.UtvBT7`, all 3 file hashes +
producing-binary hash + full_auto=0 + header stats + replay verdict). `replay.bin` was `root:600`
by the C source's own `open(...,0600)` (out of this fix's scope to change); `chmod 644` applied to
THIS registered fixture only (sha256 re-verified unchanged after chmod) so the checker is runnable
without sudo, matching every other registered-artifact convention in this repo. `check_manifest.py
--fixtures tests/passive_rx/baselines/fixtures.json` -> `FIXTURES OK`, exit 0.

**Files changed this round** (beyond the P02 session's original 4): `openair1/PHY/NR_UE_TRANSPORT/
nr_passive_replay_capture.c` (modified, DL gate + one `LOG_I`), `openair1/PHY/NR_UE_TRANSPORT/
nr_pdcch_blind_monitor_rt.c` (modified, UL hook moved), `tests/passive_rx/check_manifest.py`
(modified, f3 assertion direction), `tests/passive_rx/baselines/fixtures.json` (modified, resolved),
`tests/passive_rx/baselines/manifest_20260911_b90276d-dirty.json` + `_tracked.patch` (regenerated,
same one-pair rule). `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` (no `_rt`) is another
session's dirty file -- confirmed NOT staged, NOT committed.

**Status**: P02 PASS, G0 IN_PROGRESS (P01's own outstanding survey/limits work is the only remaining
blocker, unchanged by this fix). See updated checklist/gate rows above.

## Documentation verification

Record structure/link/checklist checks and local/remote file-copy equality here. These are documentation checks only, not receiver or solver gate evidence.

- PASS on 2026-09-10: both documents contain the same 34 unique task IDs; only D01/D02 are checked and dated complete.
- PASS: balanced fenced blocks, valid relative document links, all G0–G6 and UG1–UG7 present, no diagram blocks.
- PASS: all 10 shell-example blocks pass `bash -n`; none of the documented build/test/receiver commands was executed.
- PASS on 2026-09-10: both documents published to the canonical sens6 worktree and local/remote SHA-256 equality verified. The two pre-existing user files retained their content hashes. Only the two requested Markdown files were added; receiver source/config/build files were not modified.
- Plan SHA-256 at publication: `5d3ece65dbc9c7b6c22837a1c0f43131120db1db4cbbde7542d478a940f27d10`. This ledger is updated after the initial copy check; its final copy is reverified during synchronization.

## Acceptance-profile record — fill before evaluated captures

As of 2026-09-10 (P01) this table has a versioned home: `tests/passive_rx/acceptance_four_rx.json`
(`profile_version:1`, `frozen_date:null` -- frozen for the plan's exact invariants only, per the
brief). Every row below is `status:"UNSET"`, `value:null` in that file by construction; no number
has been invented. `tests/passive_rx/check_manifest.py --acceptance ... --geometry ...` exits
non-zero while any row is UNSET or the geometry is unsurveyed (verified 2026-09-10, see the P01
session below).

| Required field | Value / evidence |
|---|---|
| Profile path / hash / freeze date | UNSET. Path/hash of the record itself: `tests/passive_rx/acceptance_four_rx.json` (sha256 `57896964efde38903a8c4537dd09cc6d92e7c2f565a101bbf1778cbc42e6ad8b` at 2026-09-10); `frozen_date:null` |
| Experiment purpose and active RX set | UNSET (`acceptance_four_rx.json:deployment_dependent_limits.experiment_purpose_and_active_rx_set`) |
| Surveyed geometry / coordinate frame / uncertainty | UNSET; formalized separately as `tests/passive_rx/geometry_four_rx.json` (`surveyed:false`, sha256 `f27963008c0d63279c25f3fdc3af846d43563b6fffa4af05181940b2532bcdb1` at 2026-09-10) |
| Supported traffic, layer/waveform scope and denominators | UNSET (`...deployment_dependent_limits.supported_traffic_layer_waveform_scope_denominators`) |
| Capture duration / minimum eligible event count | UNSET (`...capture_duration_minimum_eligible_event_count`) |
| Acquisition and sample continuity requirements | SET, as an EXACT INVARIANT (not deployment-dependent): "Exact identity; no unexplained baseline sample loss" -- now `acceptance_four_rx.json:exact_invariants.zero_unexplained_baseline_sample_loss` (`value:true`), alongside the plan's other exact invariants (`exact_source_config_binary_identity`, `baseline_launcher_exit_code:124`, `no_rf_tx_calls_in_passive_mode`, `single_hardware_owner`, `ul_fusion_enable_default:false`, and the 2026-09-11 amendment's `full_auto_must_be_zero:true`) |
| Queue / latency / memory limits and rationale | UNSET (`...queue_latency_memory_limits_and_rationale`) |
| Detector range **and Doppler** scoring thresholds | UNSET (`...detector_range_and_doppler_scoring_thresholds`) |
| DL position / velocity / availability criteria | UNSET (`...dl_position_velocity_availability_criteria`) |
| UL reference/association quality criteria | UNSET (`...ul_reference_association_quality_criteria`) |
| Rank tolerance, state scaling and conditioning policy | UNSET (`...rank_tolerance_state_scaling_conditioning_policy`) |
| UL position / velocity / availability criteria | UNSET (`...ul_position_velocity_availability_criteria`) |
| Catastrophic-error definition / robustness targets | UNSET (`...catastrophic_error_definition_robustness_targets`) |
| Covariance coverage / calibration procedure | UNSET (`...covariance_coverage_calibration_procedure`) |
| Calibration versus held-out evaluation allocation | UNSET (`...calibration_vs_held_out_evaluation_allocation`) |

Unset limits are not automatic passes. Fill them from application needs, geometry and independent calibration evidence; do not choose them after seeing evaluated metrics.

## Session — 2026-09-11: P03 branch abstraction (foundation) + globals audit (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~10:30-11:15
Task IDs / gate: P03 (Stage 1 foundation); G1 not exercised (see below)
Intended falsifiable claim: a pure, unit-testable branch identity/epoch/lifecycle module exists
  and matches G1 tests 2 and 4 in isolated ("pure") form; a process-wide globals audit of the
  files P03's brief names exists with file:line for every symbol found; neither claim depends on
  anything running live or on the nr-ue.c RT read loop being touched (out of this task's scope
  per the controller's ruling in task-P03-brief.md).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD 8847b6a92200ffe8b044bda13bf83be56e0ee7ab (unchanged by this session). Pre-existing dirty
  state unchanged and untouched: `M executables/nr-ue-ru.c`, `M executables/nr-ue.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`, and untracked
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`,
  `adaptive_RX_pipeline.md`, `adaptive_RX_pipeline_progress.md` (this file) -- confirmed via
  `git status --short` before and after this session's edits; none of these paths were staged.
Files modified / added / removed:
  Added: `docs/passive_branch_globals_audit.md`, `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.h`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch.c`,
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_branch_test.cc`.
  Modified: `CMakeLists.txt` (added `nr_rx_branch.c` to `PHY_NR_UE_SRC`; registered the
  `nr_rx_branch_test` gtest target, mirroring `test_nr_pdsch_config_sweep`'s pattern -- own .c
  file linked directly, `UTIL`+`GTest::gtest`, no PHY_NR_UE dependency), `nr_isac.h`/`.cc`
  (`[sensing] rx_branches`/`rx_branch_phys_map` parsing, `nr_isac_rx_branches()` accessor,
  `nr_isac_set_nb_antennas_rx()` setter), `nr_isac_stub.c` (matching no-op stubs, required so the
  `ENABLE_ISAC_SENSING=OFF` build still links).
Executable / driver / config / geometry / acceptance hashes: not applicable -- no receiver run,
  no capture; only the gtest binary `nr_rx_branch_test` was built and run.
Exact commands:
  ssh sens6 'pgrep -a -x nr-uesoftmodem' (empty both before configure and before build --
  polled once, no wait needed)
  cd /home/sens/NICOLA/adaptive-rx-sensing/cmake_targets/ran_build/build && cmake .
  cmake --build . --target nr_rx_branch_test --parallel 4
  ctest --test-dir cmake_targets/ran_build/build -R nr_rx_branch_test --output-on-failure
Artifact paths (include raw logs and VOID attempts): none beyond the ctest console output pasted
  in task-P03-report.md; one FAILING intermediate run (SegFault, `RxBranchParse.
  RejectsDuplicateBranch`, root-caused to LOG_E firing before `logInit()` -- fixed by adding
  `logInit();` to the test's `main()`, matching `nr_hyp_sweep_test.cc`'s existing pattern) is
  recorded in task-P03-report.md, not re-run against a stale binary.
Baseline and comparison definition: not applicable (new module, no prior behavior to compare
  against; explicitly NOT wired into any existing behavior in this task).
Predeclared assertions / thresholds: the brief's 5 parse-reject cases (duplicate branch,
  duplicate physical, out-of-range, more-branches-than-antennas, missing active-branch mapping)
  plus G1 test 2 and test 4 in pure form (see task-P03-brief.md's exact wording, reproduced in
  the test file's own comments above each case).
Observed result, with denominators: 14/14 gtest cases pass (`nr_rx_branch_test`, 4 suites:
  RxBranchParse x8, RxBranchCheckAntennas x1, RxBranchPermute x1, RxBranchLifecycle x4). No other
  target was built or run; `nr-uesoftmodem` was not rebuilt or executed.
Status (PASS / FAIL / VOID / BLOCKED): PASS for the two falsifiable claims stated above (module
  builds and its own tests pass; audit exists with file:line citations). P03 as a WHOLE task
  remains IN_PROGRESS -- see "Remaining limitation" below; this is a controller-scoped foundation
  slice of P03, not full P03 completion, and G1 itself is NOT exercised by this session.
Validity reasons and affected intervals: not applicable (no live data collected).
Hypotheses supported / contradicted: supports the audit's own finding that
  `PHY_VARS_NR_UE.rf_map` is the concrete field coupling a reused per-branch struct back to
  hardware ownership (`openair1/PHY/defs_nr_UE.h:285`, `nr-ue-ru.c`'s `openair0_dev[]`/
  `openair0_cfg_g[]` indexed by `rf_map.card`); supports that the three passive queues'
  single global `PHY_VARS_NR_UE *g_ue` (`nr_pdcch_passive_queue.c:73`,
  `nr_pdsch_passive_queue.c:88`, `nr_pusch_passive_queue.c:70`) is the clearest "one branch
  assumed" defect blocking P04/P07/P08. Contradicts nothing; no prior claim was tested.
Retraction, if any: none.
Remaining limitation: nothing in this session is wired into the nr-ue.c RT read loop (by
  controller ruling, deferred to P04/P05); `nr_isac_set_nb_antennas_rx()` has no live caller yet
  (nr-uesoftmodem.c is outside this task's committed-file scope -- see the audit doc's
  "nb_antennas_rx reachability" section for the exact citation of where the live value already
  exists at `nr_isac_init()`'s call time and why it doesn't reach the function today); G1's
  actual 5 tests (stall/drop, frame-wrap+discontinuity+relock against a REAL sample stream,
  acquisition-vs-standalone-replay comparison) all need P04's buffer delivery and P05's real
  digital correction to be answerable end-to-end, not just P03's pure identity/epoch struct.
Next highest-value action: P04 (immutable channel-buffer delivery) -- needs the `nr_rx_branch_t`
  built here as the addressing/epoch substrate for per-branch sample ownership.
Reviewer / accomplishment date if gate passed: not gated; G1 stays NOT_STARTED (unchanged by this
  session, per the controller's ruling that P03 alone cannot make G1 testable end-to-end).
```

## Session — 2026-09-11: P04 immutable buffer delivery (Stage 1, foundation) (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~11:20-12:05
Task IDs / gate: P04 (Stage 1 immutable buffer delivery); G1 tests 1 and 3 exercised in pure form
  (see below); G1 as a whole stays NOT_STARTED (unchanged by this session, per the controller's
  ruling that P04 alone cannot make G1 testable end-to-end without the nr-ue.c read-loop wiring).
Intended falsifiable claim: a pure, unit-testable refcounted per-branch span pool exists that (a)
  routes byte-exact channel content to the correct branch under both an identity and a permuted
  P03 physical-channel map (G1 test 1, pure form), and (b) enforces the declared per-branch drop
  policy -- a stalled branch drops its own oldest entries and is counted, sibling branches and the
  producer are unaffected, no consumer ever observes an overwritten buffer (G1 test 3, pure form)
  -- plus refcount correctness (a span held by N branches returns to the free list only after all
  N release it; double release is rejected, not a double free) and epoch carry-through (a span's
  acq_epoch is fixed at acquire time and unaffected by later acquires/publishes under a different
  epoch). None of this depends on anything running live or on the nr-ue.c RT read loop being
  touched (out of this task's scope per the controller's ruling in task-P04-brief.md).
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD 835481c8596c59eca799c5928d90e0203e7238f6 (unchanged by this session before commit). Pre-
  existing dirty state unchanged and untouched: `M executables/nr-ue-ru.c`,
  `M executables/nr-ue.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`, and untracked
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`,
  `adaptive_RX_pipeline.md` -- confirmed via `git status --short` before and after this session's
  edits; none of these paths were staged.
Files modified / added / removed:
  Added: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.h`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_rx_span_pool.c`,
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_span_pool_test.cc`.
  Modified: `CMakeLists.txt` (added `nr_rx_span_pool.c` to `PHY_NR_UE_SRC` next to
  `nr_rx_branch.c`; registered the `nr_rx_span_pool_test` gtest target mirroring
  `nr_rx_branch_test`'s pattern -- own .c + `nr_rx_branch.c` linked directly, `UTIL` +
  `GTest::gtest`, no PHY_NR_UE library dependency), `adaptive_RX_pipeline_progress.md` (this
  session entry + P04 row + status paragraph).
Executable / driver / config / geometry / acceptance hashes: not applicable -- no receiver run,
  no capture; only the gtest binary `nr_rx_span_pool_test` was built and run.
Exact commands:
  ssh sens6 'pgrep -x nr-uesoftmodem' (empty, polled before configure and before build)
  cd /home/sens/NICOLA/adaptive-rx-sensing/cmake_targets/ran_build/build && cmake .
  cmake --build . --target nr_rx_span_pool_test --parallel 4
  ctest --test-dir cmake_targets/ran_build/build -R nr_rx_span_pool_test --output-on-failure
Artifact paths (include raw logs and VOID attempts): none beyond the ctest/gtest console output
  pasted in task-P04-report.md; no failing intermediate run -- the module built and passed on the
  first build+run attempt.
Baseline and comparison definition: not applicable (new module, no prior behavior to compare
  against; explicitly NOT wired into any existing behavior in this task).
Predeclared assertions / thresholds: the brief's 4 test groups verbatim -- (1) routing: 4
  channels, distinct deterministic per-channel sequence, byte-exact `memcmp` under an identity
  map ("0:0,1:1,2:2,3:3") and a permuted map ("0:3,1:2,2:1,3:0") via P03's
  `nr_rx_branch_set_parse`, checked against the SAME published span (routing is an accessor-time
  decision in this design, not a publish-time one); (3) stall: hold_budget=3, 4 active branches,
  branch 2 never takes across 10 publishes -> branch 2's `dropped_spans==7`, its surviving ring
  holds spans {7,8,9} oldest-first, branches 0/1/3 (take+release every publish) see 0 drops and
  all 10 spans in order with correct `absolute_slot`/`first_sample_ts`, `producer_stalls==0`, and
  every buffer is filled with its own span index on acquire and checked byte-exact on take
  (overwrite detector); refcount: a span held by 2 branches stays off the free list until both
  release, a second release once refcount is 0 returns -1 (not a double free); epoch carry: a
  span's `acq_epoch` set at acquire is unchanged by a later acquire/publish under a different
  epoch, verified by taking both spans off one branch's ring and comparing each against a
  simulated branch epoch. Plus one additional init-validation suite (mirroring
  `nr_rx_branch_test.cc`'s rejection coverage): NULL pool, invalid n_ch/samples_per_buf/n_buf/
  hold_budget, active_branches with no bit set or an out-of-range bit, and the undersized-pool
  case `n_buf < 1 + n_active_branches*hold_budget` -- all rejected with -1 and an unchanged pool
  on the exact-fit boundary case.
Observed result, with denominators: 5/5 gtest cases pass (`nr_rx_span_pool_test`, 5 suites:
  SpanPoolRouting, SpanPoolStall, SpanPoolRefcount, SpanPoolEpoch, SpanPoolInit). Hand-verified
  the stall test's drop arithmetic against the implementation before trusting the assertion
  (publish i for i>=3 drops exactly one oldest entry per publish under hold_budget=3, giving 7
  drops over i=3..9 and a final ring of {7,8,9} -- matches). No other target was built or run;
  `nr-uesoftmodem` was not rebuilt or executed.
Status (PASS / FAIL / VOID / BLOCKED): PASS for the two falsifiable claims stated above (module
  builds and its own tests pass, including G1 tests 1 and 3 in pure form). P04 as a WHOLE task
  remains IN_PROGRESS -- see "Remaining limitation" below; this is a controller-scoped foundation
  slice of P04, not full P04 completion, and G1 itself is NOT exercised end-to-end by this
  session (no real producer thread, no nr-ue.c wiring, no live IQ).
Validity reasons and affected intervals: not applicable (no live data collected).
Hypotheses supported / contradicted: supports the brief's claimed design property that channel
  routing can be made an accessor-time decision (via `nr_rx_span_for_branch`/
  `nr_rx_span_channel` reading `physical_channel` off P03's `nr_rx_branch_t`) fully decoupled
  from publish-time behavior -- the SAME published span was routed correctly under two different
  branch-to-physical-channel maps without a second publish. Supports that a per-branch bounded
  ring with oldest-drop-on-full is sufficient to guarantee "a slow branch cannot hold others
  indefinitely" (plan sec 4) without any blocking, coordination or backpressure between branches
  -- verified structurally (drop_oldest only ever touches the stalled branch's own ring/refcount
  share) and empirically (branches 0/1/3's counters stayed exactly 0 while branch 2 accumulated
  drops). Contradicts nothing; no prior claim was tested.
Retraction, if any: none.
Remaining limitation: nothing in this session is wired into the nr-ue.c RT read loop (by
  controller ruling, deferred to P05+); there is no real producer thread and no live IQ --
  `nr_rx_span_pool_acquire`/`publish` were driven directly from the test's main thread, not from
  a hardware read loop, so the "~2 kHz single-producer" assumption behind the module's one-mutex
  design (documented as a `ponytail:` comment in the header) is architectural, not load-tested.
  `sample_rate_hz` was added to the pool as a plain pool-lifetime constant (plan sec 3.2 lists it
  under "Run / hardware") since it cost one field and no extra logic; `run_id`/`rx_id` were
  deliberately NOT duplicated into the span/pool -- they already live on P03's
  `nr_rx_branch_set_t`/`nr_rx_branch_t`, which every real caller of this pool already holds, so
  duplicating them here would be redundant state that could drift. `nr_rx_span_pool_release()`
  validates the (pool, span, branch_id) triple's *range* but does not verify that `branch_id`
  actually holds the specific ref-unit being released -- refcount is a single shared integer, not
  per-branch-tagged, so a caller could in principle route a release through the wrong branch_id
  without the module detecting it (it would still be a legitimate decrement of a real outstanding
  reference, just not from the branch that logically owned it). This matches the brief's stated
  scope (refcount as a plain count) and is flagged here as a caller-discipline requirement for
  P05's wiring, not a defect fixed in this session.
Next highest-value action: P05 (independent digital correction/recovery per branch), which is
  also the task that finally wires both `nr_rx_branch_t` (P03) and `nr_rx_span_pool_t` (P04) into
  the real nr-ue.c read loop -- current controller guidance is that this wiring waits on that
  file's owning session committing its edits first.
Reviewer / accomplishment date if gate passed: not gated; G1 stays NOT_STARTED (unchanged by this
  session, per the controller's ruling that P04 alone cannot make G1 testable end-to-end).
```

## Session — 2026-09-11: P05 independent digital correction/recovery (Stage 1, foundation) (IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~13:10-14:40
Task IDs / gate: P05 (Stage 1 independent digital correction/recovery); G1 test 4 exercised in
  pure form (below); G1 as a whole stays IN_PROGRESS (unchanged in kind by this session -- tests
  1/3 were already pure-form-PASS from P04, test 2 from P03/P04; this session adds test 4 pure-
  form and a source-level hardware-isolation guard, per the controller's ruling that P05 alone
  still cannot make G1 testable end-to-end without the nr-ue.c read-loop wiring).
Intended falsifiable claim: (a) a pure, unit-testable per-branch digital correction/recovery
  state exists (`nr_rx_branch_sync_t`) whose reset() clears exactly the correction fields the
  brief names (CFO accumulator, timing, synchronized flag) while leaving identity/epoch/slot-
  continuity state untouched; whose on_slot() advances slot-continuity bookkeeping, counts a
  frame wrap on a small backward frame-index step, and REJECTS (no state change) a backward step
  of more than one frame -- the "unsigned slot delta breaks concurrent CPI" defect class; whose
  is_stale() correctly detects staleness after EITHER an epoch half moves (lock_epoch via
  nr_rx_branch_lose_lock(), acq_epoch via nr_rx_branch_set_rf_discontinuity()) and fails safe on
  NULL; and whose apply_cfo() is digital-only by construction (its signature cannot reach any
  device/retune API, not merely by convention) -- G1 test 4 ("inject frame wrap, sample-counter
  discontinuity and one-branch re-lock; assert correct epoch transitions and no old/new mixing")
  in pure form, extending P04's own span-pool-level version of the same test to P03's raw
  lock_epoch/acq_epoch pair with no span pool involved. (b) branch code structurally cannot reach
  the radio: a checked-in grep-based guard over the three branch modules
  (`nr_rx_branch.c`/`nr_rx_branch_sync.c`/`nr_rx_span_pool.c`) for forbidden hardware-access
  symbols, itself proven (not assumed) to both catch a planted violation and NOT false-positive on
  a legitimate symbol name containing "rf_" as a substring (`nr_rx_branch_set_rf_discontinuity`).
  (c) a file:line map of the real nr-ue.c/nr-ue-ru.c per-slot loop exists, separating
  AcquisitionOwner statements from per-branch ones and naming which new nr_rx_branch_sync_t field
  replaces each per-branch one -- read-only, no code changed in either file.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD 419f18bf42c2aa732d790ee62f3dcb6beeb64a9e (unchanged by this session before commit). Pre-
  existing dirty state unchanged and untouched, confirmed via `git status --short` before and
  after this session's edits (none of these paths were staged): `M executables/nr-ue-ru.c`,
  `M executables/nr-ue.c`, `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`, and untracked `adaptive_RX_pipeline.md`,
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`.
Files modified / added / removed:
  Added: `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch_sync.h`,
  `openair1/PHY/NR_UE_TRANSPORT/nr_rx_branch_sync.c`,
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_branch_sync_test.cc`,
  `tests/passive_rx/check_branch_hw_isolation.sh`, `docs/passive_branch_wiring_plan.md`.
  Modified: `CMakeLists.txt` (registered the `nr_rx_branch_sync_test` gtest target mirroring
  `nr_rx_span_pool_test`'s pattern -- own .c + `nr_rx_branch.c` linked directly, `UTIL` +
  `GTest::gtest`, no PHY_NR_UE library dependency; registered `branch_hw_isolation` and
  `branch_hw_isolation_selftest` as plain shell-script ctest entries), this ledger (session entry
  + P05 checklist row + G1 gate row + status paragraph).
Executable / driver / config / geometry / acceptance hashes: not applicable -- no receiver run,
  no capture; only the gtest binary `nr_rx_branch_sync_test` and the shell script
  `check_branch_hw_isolation.sh` were built/run.
Exact commands:
  ssh sens6 'pgrep -x nr-uesoftmodem' (empty, polled before configure and before build)
  cd /home/sens/NICOLA/adaptive-rx-sensing/cmake_targets/ran_build/build && cmake .
  make nr_rx_branch_sync_test -j8
  ctest -R "nr_rx_branch|nr_rx_span|branch_hw_isolation" --output-on-failure
Artifact paths (include raw logs and VOID attempts): none beyond the ctest/gtest console output
  pasted in task-P05-report.md; no failing intermediate run on the FINAL committed sources -- one
  intermediate link failure (missing `main()` in the new gtest binary, `GTest::gtest` alone does
  not provide one, unlike a framework that links `GTest::gtest_main`) was caught and fixed before
  the first passing build; not an RF/logic failure, not counted as a VOID attempt.
Baseline and comparison definition: not applicable (new module, no prior behavior to compare
  against; explicitly NOT wired into any existing behavior in this task).
Predeclared assertions / thresholds: the brief's 4 module functions verbatim, each with its own
  gtest suite -- reset() clears cfo_hz/cfo_accum_hz/timing_offset_samples/shift_for_next_frame/
  synchronized to zero/false while last_absolute_slot/frame_wraps/snap_lock_epoch/snap_acq_epoch
  are UNCHANGED (2 cases, incl. NULL no-op); on_slot() forward-advance/no-wrap, small-backward-
  frame-index-decrease-counts-as-wrap-and-advances, small-backward-same-frame-index-not-a-wrap,
  large-backward (>1 frame) REJECTED with state unchanged, EXACTLY-one-frame-backward accepted
  (boundary case), NULL/invalid-slots_per_frame rejected (6 cases); is_stale() false when epochs
  match, true after nr_rx_branch_lose_lock() (lock_epoch), true after
  nr_rx_branch_set_rf_discontinuity() (acq_epoch), fails safe (1) on either pointer NULL (4
  cases); apply_cfo() accumulates and records the latest correction, NULL no-op (1 case); plus the
  dedicated G1-test-4 case building two/three job snapshots under one epoch pair, bumping the
  branch's epoch via each of the two P03 mechanisms independently, and asserting exactly which job
  is stale and which is fresh (1 case) -- 14 cases total. The hardware-isolation guard's own
  self-test (`--selftest`) plants a forbidden symbol (`trx_read_func`) in a temp file and asserts
  the checker DETECTS it, AND separately plants the real, legitimate
  `nr_rx_branch_set_rf_discontinuity` symbol name (chosen because it contains "rf_" as a
  substring, the exact false-positive trap a naive grep would fall into) in a second temp file and
  asserts the checker does NOT flag it -- both halves are asserted, not just the detection half.
Observed result, with denominators: 14/14 gtest cases pass (`nr_rx_branch_sync_test`, 5 suites:
  RxBranchSyncReset, RxBranchSyncCfo, RxBranchSyncOnSlot, RxBranchSyncStale, RxBranchSyncG1Test4).
  `ctest -R "nr_rx_branch|nr_rx_span|branch_hw_isolation"` -- 5/5 tests pass (`nr_rx_branch_test`,
  `nr_rx_span_pool_test`, `nr_rx_branch_sync_test`, `branch_hw_isolation`,
  `branch_hw_isolation_selftest`), confirming no regression to the P03/P04 modules this task links
  against. The real (non-selftest) `branch_hw_isolation` run against the three actual branch
  module source files found ZERO forbidden-symbol hits, confirming the false-positive risk
  (`nr_rx_branch_set_rf_discontinuity`'s "rf_" substring) identified and handled by the
  word-boundary regex does not currently manifest in the real files either. No other target was
  built or run; `nr-uesoftmodem` was not rebuilt or executed; `pgrep -x nr-uesoftmodem` was empty
  immediately before both the cmake reconfigure and the build.
Status (PASS / FAIL / VOID / BLOCKED): PASS for the three falsifiable claims stated above (module
  builds and its own tests pass including G1 test 4 in pure form; the hardware-isolation guard
  detects a planted violation and does not false-positive, proven not assumed; the wiring plan
  document exists with verified file:line citations against the live tree). P05 as a WHOLE task
  remains IN_PROGRESS -- see "Remaining limitation" below; this is a controller-scoped foundation
  slice of P05, not full P05 completion, and G1 test 5 / the full end-to-end exit criterion are
  NOT exercised by this session (no live worker, no nr-ue.c wiring, no live IQ).
Validity reasons and affected intervals: not applicable (no live data collected).
Hypotheses supported / contradicted: supports that the "no old/new mixing" primitive (P05's own
  wording) generalizes cleanly from P04's span-level epoch carry (a single acq_epoch on a span) to
  P03's raw per-branch epoch PAIR (lock_epoch AND acq_epoch, independently load-bearing -- tested
  separately in RxBranchSyncG1Test4 via the two distinct P03 mechanisms that bump each). Supports
  that "digital-only, no hardware reachable" can be made a STRUCTURAL property of a function's
  signature (apply_cfo() takes nothing but its own struct and a double) rather than a runtime-
  checked convention, matching P05's brief text ("must be documented and asserted") more literally
  than a runtime assert could -- there is nothing to assert against because there is no path to
  violate. Contradicts nothing; no prior claim was tested. One thing WORTH FLAGGING AS A FINDING,
  not a contradiction: a naive (non-word-boundary) version of the hardware-isolation grep WOULD
  have false-positived on the real, already-committed `nr_rx_branch_set_rf_discontinuity` symbol
  (P03, `nr_rx_branch.c:188`) -- verified empirically (plain `grep -inE` without `\b` DOES match
  that line) before choosing the word-boundary form, not assumed safe.
Retraction, if any: none.
Remaining limitation: nothing in this session is wired into the nr-ue.c RT read loop (by
  controller ruling, still deferred -- that file remains dirty with another session's uncommitted
  edits). `docs/passive_branch_wiring_plan.md` records two design choices explicitly left OPEN for
  the wiring step rather than resolved here (not glossed over): (1) whether `UE->max_pos_iir` (the
  IIR filter feeding `max_pos_acc`) becomes a second per-branch field alongside
  `timing_offset_samples` or is folded into it -- the brief's struct does not name a separate IIR
  field; (2) whether the 3-state `stream_status` enum (`UNSYNC`/`SYNCING`/`SYNCED`) collapses onto
  the struct's 2-state `synchronized` flag or needs an additional field this task does not
  provide. The CFO-compensation-fields row is flagged with an honest caveat rather than a false
  file:line claim: `UE_thread()`'s own per-slot statements (the only ones read for this plan, per
  the brief's `nr-ue.c`/`nr-ue-ru.c` scope) do not themselves read/write
  `common_vars.freq_offset`/`freq_off_acc` -- the digital CFO estimation/compensation loop lives
  downstream in `phy_procedures_nr_ue.c`, outside this task's read. The hardware-isolation guard
  is a grep over TEXT, not a linker/compiler enforcement -- it catches a forbidden SYMBOL NAME
  appearing in the branch modules' own source, not e.g. a forbidden symbol reached transitively
  through a function pointer or macro that expands to one without the literal name appearing; this
  matches the brief's own specification (a checked-in script, not a build-system-level ban) and is
  stated here as a known limit of that specification, not discovered as a surprise.
Next highest-value action: the nr-ue.c/nr-ue-ru.c read-loop wiring itself (blocked on that file's
  owning session committing its edits first, per the controller's repeated ruling across P03-P05)
  -- once unblocked, `docs/passive_branch_wiring_plan.md` is the map for exactly what moves where,
  and the two open design choices in "Remaining limitation" above are the first decisions that
  wiring step needs to make before G1 test 5 becomes runnable.
Reviewer / accomplishment date if gate passed: not gated; G1 stays IN_PROGRESS (unchanged in kind
  by this session, per the controller's ruling that P05 alone cannot make G1 testable end-to-end).
```

## Session — 2026-09-11: P07 independent (single-branch) DL decoding (Stage 2, replay form) (PASS)

```text
Date/time (Europe/Zurich): 2026-09-11, ~13:40-15:30
Task IDs / gate: P07 (Stage 2 DL workers); G2 tests 1 and 3 exercised in REPLAY form; G2 as a
  whole stays IN_PROGRESS (P06/P08/P09 pending, per the controller's scope ruling for this task).
Intended falsifiable claim: (a) the passive DL decode chain (queue consumer -> nr_pdsch_passive_
  decode -> FEP -> chest -> nr_rx_pdsch -> LDPC -> data-aided reconstruction) can be made to see
  EXACTLY ONE receive antenna (a chosen physical channel) end to end, with every cross-branch
  mechanism (MRC / strongest-branch selection, selection-diversity retry, subset scan, BRANCHFO,
  DMRSFO EMA/apply, planned-branch nvar substitution, SFO_CORRECT) demonstrably off and the noise
  estimate that channel's own; (b) the default path is BIT-IDENTICAL to before (proven by replaying
  the P02 fixture before and after with the same command and diffing every verdict line);
  (c) per branch, the data-aided reconstruction consumes ONLY that branch's own CRC-accepted TBs
  (G2 test 3 in replay form: data_submits == crc_ok per view, a view with fewer CRC-OK has
  correspondingly fewer submits, never the reference's 34); (d) Nl > 1 grants are rejected
  explicitly in view mode with their own counter rather than decoded on one antenna.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing,
  HEAD d1b563f511 when this session started; ANOTHER session committed be65aa8764 ("nr-ue: manual
  DL/UL profile edits", executables/nr-ue.c only, the same edits that were already in the working
  tree and hence already in every binary this session built) mid-session, so this session's commit
  sits on be65aa8764. Pre-existing dirty state untouched and never staged, confirmed by
  `git status --short` before and after: `M executables/nr-ue-ru.c`,
  `M executables/nr-ue.c` (until be65aa8764), `M openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c`,
  `M openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`, `M radio/USRP/usrp_lib.cpp`,
  `M tests/passive_rx/monitor/monitor.html`, `M tests/passive_rx/monitor/monitor.py`,
  `M tests/passive_rx/run_adaptive_receive_test.sh`; untracked `adaptive_RX_pipeline.md`,
  `tests/passive_rx/MANUAL_DL_UL.md`, `tests/passive_rx/adaptive_manual_dlul.conf`,
  `tests/passive_rx/aoa_track_dl.conf`, `tests/passive_rx/run_manual_x410.sh`. (The plan's own
  checklist row for P07 lives in the untracked `adaptive_RX_pipeline.md` and was therefore NOT
  edited -- to be synchronized by its owning session.)
Files modified / added / removed:
  Added: `docs/passive_dl_branch_view_audit.md`, `tests/passive_rx/replay_branch_view.sh`,
  `tests/passive_rx/baselines/manifest_20260911_be65aa8-dirty.json` + `_tracked.patch`
  (old pair `manifest_20260911_b90276d-dirty.*` removed, one-pair rule; generated with this
  session's files STAGED, so the tracked patch carries only the other sessions' unstaged edits;
  `binary_stale_vs_tracked_source` = not_stale).
  Modified: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{h,c}` (view resolve/arm helpers,
  `reason` on the result, multilayer counter, DMRSFO/SFO_CORRECT gates, verdict trace, BRANCHVIEW
  stats line), `nr_pdsch_passive_queue.{h,c}` (job `branch_id`/`physical_channel` in the existing
  rnti padding -- sizeof 384 unchanged, verified against the fixture's job_bytes; consumer resolves
  the view and hands the SAME view UE to decode and submit), `nr_passive_replay_capture.c` (view
  per replay, data-aided path exercised on every CRC-OK TB via `nr_isac_data_aided_force`, per-
  status counts, `REPLAY-VIEW` summary line), `nr_pdsch_data_aided.{h,c}` (`g_data_submits`
  counter + getter, replay-only force flag, TODO(P10) at the ABI), `nr_pdcch_blind_monitor_rt.c`
  (2 x 2 lines: deterministic 0/0 identity on the job -- the fields were uninitialised stack
  padding before), `CMakeLists.txt` (ctest `dl_branch_view_replay`, SKIP_RETURN_CODE 77),
  `tests/passive_rx/check_manifest.py` (`verified_with_binary_sha256` accepted alongside
  `producing_binary_sha256`, new selftest case f4), `tests/passive_rx/baselines/fixtures.json`
  (`verified_with_binary_sha256` + `branch_view_replay` record; `producing_binary_sha256` kept),
  this ledger.
Executable / driver / config / geometry / acceptance hashes: nr-uesoftmodem before
  c3810019905f3fb661138d12fd5b9b6156d47e36344ae871f08047ba65dc7889 (= the fixture's producing
  binary), after 6bfe2c785d99718b9cddc5f18ee2fb710aee70962faefad0009b6a415a140573 (built at HEAD
  d1b563f511 + the then-uncommitted nr-ue.c edits that became be65aa8764 + this session's edits);
  liboai_usrpdevif.so unchanged ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583;
  receiver.conf 5022d875a26f1ec235ee209b1ff04ef3031290c149399702b7ef63d0c3a381ef (the fixture's).
  Note the rebuilt binary also contains the other sessions' uncommitted edits, exactly as the
  previous binary did. No geometry/acceptance change.
Exact commands:
  ssh sens6 'pgrep -x nr-uesoftmodem'   (empty before each of the three cmake builds)
  cd cmake_targets/ran_build/build && cmake --build . --target nr-uesoftmodem oai_usrpdevif --parallel 4
  # legacy replay, before and after, identical args (fixtures.json replay_verification.command,
  # run as plain user -- replay.bin is 0644):
  env ISAC_PASSIVE_REPLAY_INPUT=/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7/replay.bin \
      LD_LIBRARY_PATH=$(pwd):/usr/local/lib ./nr-uesoftmodem -O <fixture>/receiver.conf -r 273 \
      --numerology 1 --band 78 -C 3450000000 --ssb 150 --ue-rxgain 40 --ue-nb-ant-rx 4 \
      --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 \
      --freq-sync-I 0.001 --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
      --ntn-initial-time-drift -4.25 -A 90
  OUT=/tmp/p07/bv3 bash tests/passive_rx/replay_branch_view.sh      (= ctest -R dl_branch_view_replay)
  ctest -R "dl_branch_view_replay|nr_rx_branch|branch_hw_isolation" --output-on-failure  (5/5 pass)
  FIXTURE=/nonexistent ctest -R dl_branch_view_replay   (reported "Skipped", not passed)
  python3 tests/passive_rx/check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json
  bash tests/passive_rx/baseline_manifest.sh && python3 tests/passive_rx/check_manifest.py --selftest
Artifact paths (include raw logs and VOID attempts): /tmp/p07/legacy_before.log (old binary),
  /tmp/p07/legacy_final.log (new binary, default path), /tmp/p07/bv3/{legacy,view0..3}.log +
  summary.txt (final binary, trace on). One intermediate run (/tmp/p07/bv1) reported view1 as
  FAIL(i) because the script's first draft required exit 0 from the replay binary, whose exit 2
  ("REPLAY VOID") is by construction what a view with FEWER accepted TBs than the reference
  returns -- the script now requires "no radio opened" + identical-controls == the view's own
  crc_ok and accepts exit 2 for views only; legacy still requires the exact PASS line and exit 0.
  Not an RF/logic failure; recorded, not counted as VOID.
Baseline and comparison definition: the P02 fixture's own 4-antenna replay (34 CRC-OK / 4 CRC-fail
  of 38 recorded DL grants, 24 raw UL) is the reference; each single-branch view is compared per
  recorded grant against the SAME recorded TB hash (the binary's identical-controls check), so a
  view's crc_ok is by construction also its count of payloads identical to the reference.
Predeclared assertions / thresholds: the brief's (i)-(iii) only -- (i) every view runs to
  "no radio opened" with every accepted TB identical to the reference; (ii) legacy line unchanged
  `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened`; (iii) per view
  data_submits == crc_ok. Single-branch CRC BELOW 34 was predeclared acceptable (coverage loss to
  record, not tune).
Observed result, with denominators (38 recorded DL grants, all Nl=1; 24 raw UL untouched):
  legacy 4-ant: crc_ok 34, crc_fail 4, unsupported 0, unsupported_multilayer 0, data_submits 34,
    exit 0, verdict line identical to pre-change; full stdout/stderr diff vs the pre-change binary
    = the [HW] version banner line + the one new REPLAY-VIEW summary line, nothing else.
  view0 (phys 0): crc_ok 34/38, crc_fail 4, submits 34, identical-to-reference 34, exit 0.
  view1 (phys 1): crc_ok 31/38, crc_fail 7, submits 31, identical-to-reference 31, exit 2
    (REPLAY VOID: identical DL controls=31 failed=3) -> 3 TBs of coverage loss vs legacy.
  view2 (phys 2): crc_ok 34/38, crc_fail 4, submits 34, identical 34, exit 0.
  view3 (phys 3): crc_ok 34/38, crc_fail 4, submits 34, identical 34, exit 0.
  unsupported_multilayer_in_branch_view = 0/38 on every view: a DENOMINATOR statement (the fixture
  carries no Nl>1 grant), not evidence that the counter fires.
  Evidence the views really are single-antenna and really are different channels: the BRANCHFO
  line (gated nb_antennas_rx > 1) appears once in legacy and never in any view; CHESTDIAG nvar for
  the first three grants reads 13/51/13 (view0), 348/616/388 (view2), 246/728/244 (view3).
  ctest: dl_branch_view_replay PASS (22.4 s); nr_rx_branch_test, nr_rx_branch_sync_test,
  branch_hw_isolation, branch_hw_isolation_selftest still PASS; missing-fixture path reports
  "Skipped" (exit 77), not PASS. check_manifest.py --fixtures: FIXTURES OK with the new binary via
  verified_with_binary_sha256; --selftest: PASS incl. new case f4.
Status (PASS / FAIL / VOID / BLOCKED): PASS for claims (a)-(d) on the brief's criteria (i)-(iii).
  Per-branch coverage recorded: view1 decodes 31/34 of the reference's accepted TBs (91 %),
  views 0/2/3 34/34 -- on THIS fixture branches 2 and 3 are NOT worse than branch 0, contrary to
  the memory note ("branches 1-3 much worse"); one 38-grant fixture is not a rig characterisation.
Validity reasons and affected intervals: no live data collected; replay only (no radio opened,
  verified by the binary's own verdict text and by nr-uesoftmodem.c:428-434 returning before
  device init).
Hypotheses supported / contradicted: supports that the view can be delivered without touching any
  antenna loop in FEP/chest/demod/data-aided code (mechanism (a') in the audit) -- every
  cross-branch mechanism switched itself off through its existing nb_antennas_rx / nbRx==4 gate,
  and only the DMRSFO EMA/apply + SFO_CORRECT needed explicit gates. Contradicts the expectation
  (brief, memory) that single-branch CRC would be well below the 4-antenna count on branches 2/3:
  measured equal on this fixture; branch 1 is the only weaker one (-3 TBs). Finding worth
  flagging: the recorded jobs' new identity bytes were uninitialised stack padding in the
  producer, so ANY consumer trusting them from a pre-P07 fixture would read garbage -- the replay
  overwrites them unconditionally and the producer now zero-initialises them.
Retraction, if any: none.
Remaining limitation: (1) live independent mode is unexercised -- with `rx_branches` naming >1
  branch the consumer resolves every job to the branch whose physical channel matches the job's
  tag, and the producer tags 0/0 until P06, so live independent mode today == a branch-0 (or
  lowest-active-branch) view for every grant; (2) `nr_isac_submit_cfr_multi()` has no branch
  identity slot (recorded as a P10 requirement in the audit sec 4 and as TODO(P10) in the code),
  so a multi-branch live run would fold every branch's CFR into one engine; (3) the multilayer
  rejection path and the failure classes of G2 test 5 beyond plain CRC failure (segment failure,
  all-zero TB, UCI-rescued, unsupported waveform/RV) are built or pre-existing but NOT exercised
  by this fixture; (4) no "all four in parallel" replay (the four views ran sequentially in one
  script, each in its own process); (5) DMRSFO/SFO per-branch tracking is simply OFF in view mode
  (no shared-EMA leakage, but also no correction) pending P09; (6) the shadow-UE copy refreshes an
  enumerated list of mutable scalars per job -- a future read of some other runtime-mutable
  `PHY_VARS_NR_UE` scalar inside the chain would see a snapshot from the thread's first job (the
  frame-parms staleness check covers cell reconfiguration only).
Next highest-value action: P06 (branch-local PDCCH discovery tagging each job with its branch) so
  that independent mode decodes each branch's OWN grant stream live, then P10 so the reconstruction
  can be routed per branch; screen a fixture with Nl>1 grants to exercise the multilayer counter.
Reviewer / accomplishment date if gate passed: P07 accomplished 2026-09-11 on the brief's own
  PASS criterion (replay form); G2 not passed.
```

## Session — 2026-09-11: P06a live branch lifecycle hooks + per-branch DL job fan-out (IN_PROGRESS — BLOCKED on live evidence)

```text
Date/time (Europe/Zurich): 2026-09-11 (host clock UTC 13:48-15:0x)
Task IDs / gate: P06a (P03 lifecycle wiring + P06/P07 bridge); feeds G1 and G2
Intended falsifiable claim: the read loop can own branch lifecycle and the DL producer can fan one
  discovered grant out to N branch-tagged jobs, WITHOUT changing the default (one-branch) path --
  falsifiable by the legacy replay's own line and by sizeof(nr_pdsch_passive_job_t).
Branch / full commit: merge/adaptive-sensing, on 0667e4dd09 (tree clean apart from the untracked
  tests/passive_rx/aoa_track_dl.conf, which is another session's and was not touched)
Files modified: openair1/PHY/NR_UE_TRANSPORT/{nr_rx_branch.h,nr_rx_branch.c,
  nr_pdsch_passive_queue.h,nr_pdsch_passive_queue.c,nr_pdcch_blind_monitor_rt.c,
  nr_pdsch_passive_decode.h,nr_pdsch_passive_decode.c,tests/nr_rx_branch_test.cc},
  openair1/PHY/NR_UE_ISAC/{nr_isac.h,nr_isac.cc,nr_isac_stub.c},
  executables/{nr-ue.c,nr-uesoftmodem.c}, tests/passive_rx/run_manual_x410.sh,
  tests/passive_rx/baselines/fixtures.json, this ledger, manifest pair regenerated
Executable hashes: nr-uesoftmodem b0ba754472c437c192d023d844a10cde205a43cccbe7279233f87d78825f1038,
  liboai_usrpdevif.so ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583 (unchanged)
Exact commands: cmake --build . --target nr-uesoftmodem oai_usrpdevif --parallel 4 (rc=0);
  ctest -R "nr_rx_branch|nr_rx_span_pool|branch_hw_isolation" (5/5 Passed, 19/19 gtest cases in
  nr_rx_branch_test, 4 of them new); the fixtures.json legacy replay command (exit 0);
  OUT=/tmp/p06a/bv bash tests/passive_rx/replay_branch_view.sh;
  gdb -batch -ex "ptype /o nr_pdsch_passive_job_t" ./nr-uesoftmodem
Artifact paths: /tmp/p06a/legacy_after.log, /tmp/p06a/bv/, /tmp/p06a/conf4_ant4.log,
  /tmp/p06a/conf4_ant2.log
Predeclared assertions: (a) legacy replay line unchanged; (b) sizeof(job) stays 384;
  (c) 5/5 existing branch ctests still pass; (d) rx_branches > antennas is fatal at startup
Observed result, with denominators:
  (a) "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened", exit 0;
      replay_branch_view.sh legacy/view0/view1/view2/view3 crc_ok 34/34/31/34/34 of 38 --
      byte-for-byte the P07 numbers.
  (b) sizeof = 384 unchanged: acq_epoch occupies the former 4-byte hole at offset 324, lock_epoch
      the former 7-byte hole at 348 (now a 3-byte hole). The P02 fixture's job_bytes check passes,
      which is also what makes (a) runnable at all.
  (c) 5/5 ctest Passed; nr_rx_branch_test 19/19 (was 15).
  (d) --ue-nb-ant-rx 2 with rx_branches="0,1,2,3" aborts at startup with
      "SENSING: rx_branches names 4 branches but only 2 receive antennas are configured"; the same
      conf at --ue-nb-ant-rx 4 replays to REPLAY PASS.
Status: IN_PROGRESS.
  **BLOCKED: X410 unreachable (uhd_find_devices --args type=x4xx,addr=192.168.20.2:
  "No UHD Devices Found", checked 2026-09-11 13:48:09 UTC), live captures not attempted.**
  Controller-ruled mid-task: do not poll or wait for the radio; complete everything except the two
  live captures. Consequence stated plainly: the fan-out, the per-branch counters, the per-branch
  multilayer attribution and the RX_BRANCHES launcher knob have ZERO live verification. The live
  unsupported-multilayer fraction (G2's open number) was NOT measured and is NOT estimated here.
Validity reasons: every result above is offline (replay / unit test / startup check). No radio was
  opened by anything in this session (the replay path exits before radio init).
Hypotheses supported: a branch-tagged job can carry its epoch snapshot inside existing padding, so
  Stage 2's staleness rule costs no ABI change.
Retraction: none.
Remaining limitation: the per-branch decode still runs against ONE shared acquisition/sync
  (plan sec 3.1's explicitly-allowed initial form); branch-owned PDCCH discovery state (learned
  RNTIs, candidate persistence, energy/noise gates, discovered layouts) is untouched and remains
  P06's open core; UL is untouched (P08); the CFR ABI still has no branch slot (P10).
Next highest-value action: the two live captures the moment the X410 is back --
  RX_BRANCHES=0,1,2,3 DURATION=180 then the same DURATION with RX_BRANCHES unset -- reading
  per-branch crc_ok with denominators, dropped[full] under 4x decode load, the branch epochs at
  end, and the live multilayer fraction.
Reviewer / accomplishment date if gate passed: n/a (no gate passed)
```

### Design, file:line (as committed)

| Item | Where | Note |
|---|---|---|
| Branch set instantiated / owned by the read loop | `executables/nr-ue.c` `ue_branches_lock/lose_lock/discontinuity/epoch_str` helpers immediately above `UE_thread()` | They act on the ONE process-wide set parsed by `nr_isac_init()`, reached through the new `nr_isac_rx_branches_mutable()`. No second instance: the read loop is the AcquisitionOwner and the only writer. All four are no-ops when sensing is off / the branch config failed to parse |
| Shared sync applied to every branch | `ue_branches_lock()` / `ue_branches_lose_lock()` | Plan sec 3.1 ("preserve the present shared per-branch plan initially") — cited in the code comment. Independent per-branch acquisition is the wiring plan's "Yes" rows and is NOT this task |
| `lose_lock` call sites | `nr-ue.c`: UE_thread entry (`UE->is_synchronized = 0`), RXDISCONT invalidation, RFSTALL invalidation, CFOTRK retune invalidation, and (fix round 1) the `handle_sync_req_from_mac(UE) == 0` branch | **Corrected in fix round 1**: the original claim "every site where the shared `is_synchronized` is cleared inside `UE_thread`" covered only the direct ASSIGNMENTS. `handle_sync_req_from_mac()` retunes the radio and clears the flag inside itself, so it needed a hook at its call site. **Reachability corrected in fix round 2 -- NOT attached-UE-only**: the pending-sync flag is set both by `handle_reconfiguration_with_sync()` (RRC_CONNECTED-gated, never reached under `--passive-rx`) AND by `nr_ue_decode_mib()`'s `cellBarred` branch (`openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c:149-158`, dispatched from `nr_rrc_mac_config_req_mib()` at `config_ue.c:1125` with no `IS_PASSIVE_RX_MODE` gate on the path), and a passive receiver does decode MIB -- so a `cellBarred` cell reaches this hook under `--passive-rx` as well. The hook is correct on both paths |
| `lock` call sites | `nr-ue.c`: the `sync_ref` forced-sync branch, and the successful-acquisition block just before the "UE synchronized!" `LOG_A` | `absolute_slot` is the loop counter, passed through as the lock slot |
| `set_rf_discontinuity` call sites | `nr-ue.c`: the `nr_rx_continuity_check` failure (BEFORE the `ISAC_DISC_NO_RESYNC` switch — the gap happened whether or not this build reacquires), the RFSTALL `nrue_ru_reinit()` success, and the CFOTRK `nrue_ru_reinit()` | Common-mode by construction (P03 bumps `acq_epoch` on every active branch) |
| `nb_antennas_rx` cross-check | `executables/nr-uesoftmodem.c`, `nr_isac_set_nb_antennas_rx(get_nrUE_params()->nb_antennas_rx)` immediately before `nr_isac_init()` — the single site P03's audit names | The check inside `nr_isac_init()` was changed from "LOG_E + sensing disabled" to `AssertFatal`: a silently-off sensing pipeline on a misconfigured branch list reads as an empty capture instead of a misconfiguration |
| Branch epochs in the periodic stats line | `nr-ue.c` RFCENSUS line, new `branches=[b0:L<lock>/A<acq>/s<state> ...]` field | |
| Fan-out helper (pure, unit-tested) | `nr_rx_branch.{h,c}`: `nr_rx_branch_dispatch_t`, `nr_rx_branch_set_dispatch()`, `nr_rx_branch_dispatch_is_stale()` | Kept in the pure P03 module so the gtest needs no PHY dependencies; `set_dispatch` REJECTS rather than truncates when the caller's array is too small (a silently dropped branch makes per-branch coverage wrong in a way no counter can reveal) |
| Fan-out producer | `nr_pdsch_passive_queue.c`: `nr_pdsch_passive_queue_enqueue_fanout()`, called from both DL job sites in `nr_pdcch_blind_monitor_rt.c` (the fast enqueue-from-DCI path and the deferred path) | With no branch set or one active branch it enqueues exactly ONE job — the legacy path. UL untouched (P08) |
| Epoch snapshot on the job | `nr_pdsch_passive_queue.h`: `uint32_t acq_epoch` (offset 324) and `uint32_t lock_epoch` (offset 348), both in former padding, `sizeof` unchanged at 384 | |
| Staleness drop | `nr_pdsch_passive_queue.c` consumer, before the branch-view resolve | Only in independent mode (`n_active > 1`); counted per branch as `dropped_stale_epoch`, never decoded anyway |
| Per-branch counters | `nr_pdsch_passive_queue_stats_t.per_branch[]` + the new `SENSING: PDSCHQ-BRANCH` lines | Printed only when `n_active > 1`. The line's own text states the asymmetry: GRANTS ARE SHARED (one discovery fanned out, so the `q` columns are expected equal), PAYLOADS AND CRC ARE PER BRANCH |
| Per-branch multilayer attribution | `nr_pdsch_passive_decode.c`: `t_view_branch` thread-local + `g_view_unsupported_multilayer_br[]`, formatted by `nr_pdsch_passive_view_unsupported_multilayer_str()` | The existing aggregate counter and its BRANCHVIEW/replay readers are unchanged |
| Launcher knob | `tests/passive_rx/run_manual_x410.sh`: `RX_BRANCHES` (validated `^[0-3](,[0-3])*$`), identity `rx_branch_phys_map` derived from it, appended by `awk` to the RENDERED `$OUT/receiver.conf` only; effective values recorded in `$OUT/arm.txt` | Unset = the rendered conf is byte-identical to the tracked one. NOT exercised live (radio unreachable); the conf-rendering itself was verified offline |


### Fix round 1 (2026-09-11, same day) — reviewer findings addressed

1. **IMPORTANT — missing lifecycle hook at the MAC resync site.** `handle_sync_req_from_mac()`
   clears `UE->is_synchronized` INSIDE ITSELF, after retuning the radio, so the original wiring
   (which covered the direct assignments in `UE_thread`) missed it and the report's "every site
   where `is_synchronized` is cleared inside `UE_thread`" was overstated — corrected above and in
   the report. Failure it admitted: an attached-UE MAC resync retunes the radio, branch epochs do
   not move, and DL jobs fanned out before the retune stay epoch-FRESH and are decoded against
   post-retune samples — the exact old/new mixing P05's epochs exist to prevent. **The "inert
   under `--passive-rx`" claim made here was itself wrong and is retracted in fix round 2 below:
   the `cellBarred` branch of MIB decode reaches this hook under `--passive-rx` too.**
   Fixed at the call site: `if (handle_sync_req_from_mac(UE) == 0) { ue_branches_discontinuity();
   ue_branches_lose_lock(); continue; }`.
2. MINOR — the consumer's stale-epoch `continue` now sits ABOVE the
   `saved_fo`/`nr_slot_fep_fo_override_hz` save, so the save/restore pairing holds structurally
   rather than by the accident of the drop being before the assignment.
3. MINOR — the `PDSCHQ` log-line comment now states its own exactness limit at `n_active > 1`:
   `dropped_full`/`dropped_stale` are aggregate (the ring evicts before anything reads the job's
   branch), and a stale-dropped job counts in its branch's `q` with only `stale` to explain it.
4. MINOR — restored the trailing `|` on the P06 checklist row (broken markdown rendering).
5. MINOR — `fixtures.json` churn: the P06a commit reserialised all 674 lines because the writer
   used `indent=1`. Rewritten with the file's own original convention,
   `json.dumps(obj, indent=2) + "\n"` with key order preserved (`object_pairs_hook=OrderedDict`),
   so the diff against `0667e4dd09` is now the two intended entries only. Any future writer must
   use those exact parameters.

Re-verified offline after the fixes (no radio; X410 still unreachable):
`cmake --build . --target nr-uesoftmodem oai_usrpdevif --parallel 4` rc=0 (binary
`08d3912439a8216ef37f4691563370e81a69147b87c4c69b18d2ed7c48481ecb`), legacy replay `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no
radio opened` (exit 0), `replay_branch_view.sh` 34/34/31/34/34 of 38, `ctest` 5/5,
`nr_rx_branch_test` 19/19, `check_manifest.py --selftest` PASS / `--fixtures` OK.
Commit: the "P06a fix round 1" commit on `merge/adaptive-sensing`, the child of `32b3239f5b` (the SHA itself is deliberately not written here -- recording it in this file required an amend, which changed it; `git log --oneline 32b3239f5b..` resolves it).

### Fix round 2 (2026-09-11, same day) — wording only, no logic change

Fix round 1's own justification overstated in the opposite direction. It called the MAC-resync hook
"attached-UE only … which is why no replay can observe it". **Retracted.** Traced in this session
rather than taken on trust: `nr_ue_synch_request()` (`openair1/SCHED_NR_UE/fapi_nr_ue_l1.c:403`)
sets `PHY_VARS_NR_UE.synch_request.received_synch_request`, and it is reached from TWO places —
`handle_reconfiguration_with_sync()` (genuinely attached-only, RRC_CONNECTED-gated, never reached
under `--passive-rx`) and `nr_ue_decode_mib()`'s `cellBarred` branch
(`openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c:149-158`). The second is dispatched from
`nr_rrc_mac_config_req_mib()` (`openair2/LAYER2/NR_MAC_UE/config_ue.c:1125`) with **no
`IS_PASSIVE_RX_MODE` gate anywhere on that path** (the only such gate in that file is at
`config_ue.c:2163`, on an unrelated path), and a passive receiver decodes MIB. So a gNB signalling
`cellBarred` fires this hook under `--passive-rx` as well: unlikely on this rig, not impossible,
and in principle replay-observable.

Corrected in the code comment, the design-table row above and the fix-round-1 text above. **No
logic change**: the hook is correct on both reachability paths — which is precisely why finding
this changed nothing but the words. Citation note (corrected again in fix round 3): `L2_interface_ue.c:126-130` DOES exist --
`openair2/RRC/NR_UE/L2_interface_ue.c`, the RRC->MAC dispatch switch whose
`case NR_MAC_RRC_CONFIG_MIB:` routes the decoded MIB into `nr_rrc_mac_config_req_mib()` -- and it
is a real step in the same chain. My fix-round-2 claim that it "does not exist in this tree" was
wrong: I searched only `openair2/LAYER2/NR_MAC_UE/`, where it is not, and concluded from one
directory. It simply is not the LOAD-BEARING citation for the `cellBarred` trigger itself; that is
`config_ue.c:1125` / `nr_ue_procedures.c:149-158`, which are correct.

Because a `.c` comment changed, the binary was rebuilt and the replay re-run rather than assuming a
comment cannot matter: `pgrep -x nr-uesoftmodem` empty → build rc=0 → binary ``0d011b9c3a709ea6b3d90fb9c104d715aa8576f53521de105de6536d1dff1f41`` →
legacy replay `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0),
`ctest` 5/5, `nr_rx_branch_test` 19/19, `check_manifest --selftest` PASS / `--fixtures` OK. No
radio: the X410 is still unreachable.

### Fix round 3 (2026-09-11, same day) — prose only

Fix round 2's own citation dispute was wrong. `L2_interface_ue.c:126-130` DOES exist, at
`openair2/RRC/NR_UE/L2_interface_ue.c` (`case NR_MAC_RRC_CONFIG_MIB:` →
`nr_rrc_mac_config_req_mib(instance_id, 0, …mib, …access_barred)`), and it is a real step in the
same chain — the RRC→MAC dispatch that routes the decoded MIB into the function whose
`nr_ue_decode_mib()` call reaches the `cellBarred` branch. I had searched only
`openair2/LAYER2/NR_MAC_UE/` (where the file is not) and concluded "does not exist in this tree"
from a single directory — the same one-place-looked, whole-claim-made error as fix round 1's
overstated coverage. It is simply not the LOAD-BEARING citation for the `cellBarred` trigger, which
is `config_ue.c:1125` / `nr_ue_procedures.c:149-158`, both correct and unchanged.

No code, no ledger-row, no comment change: the reachability finding, the `nr-ue.c` comment and the
design-table row are all confirmed correct as they stand. Prose only, in this note and in the
report. No rebuild.

## Session — 2026-09-11: P09 decoder/re-encoder namespace + TLS audit, per-branch harq_unique_pid (Stage 2, IN_PROGRESS)

Date/time (Europe/Zurich): 2026-09-11, ~14:00-17:00 CEST (host clock is UTC; +2h).
Task IDs / gate: P09 (shared-resource / namespace audit); feeds G2 exit item "no resource namespace collision".

Intended falsifiable claim: the `harq_unique_pid` handed to the shared `nrLDPC_coding_interface` by the
passive DL decode is unique across (branch x direction x decode-vs-reconstruction x UE/session) for every
pair of transport blocks that can be in flight at the same moment; and no thread-local in the decode /
re-encode files carries branch-dependent content from one job into another on the same thread.
Falsifier for the first half: any two (branch, harq_process_nbr) pairs mapping to the same id, or any id
escaping the 2000-2999 range. Falsifier for the second half: a `__thread` object read by job B whose
content was written by job A on a different branch.

Branch / full commit / dirty patch / untracked-file manifest:
  branch `merge/adaptive-sensing`, parent `2566d256d2ca53f8bf79d05cec37978aee5ecaf7`;
  manifest `tests/passive_rx/baselines/manifest_20260911_2566d25-dirty.json` +
  `..._tracked.patch` (its sha256 is recorded inside the manifest itself, not repeated here -- writing it
  into this file would change the very diff it hashes; `check_manifest.py <manifest>` reports MANIFEST OK
  against the final tree, which is the check that matters);
  pre-existing untracked file not touched by this task: `tests/passive_rx/aoa_track_dl.conf`.

Files modified / added / removed:
  ADDED  `openair1/PHY/NR_UE_TRANSPORT/nr_passive_harq_tag.h` (namespace map, per-branch stride,
         `static_assert` bound, `nr_pdsch_passive_harq_tag()`)
  ADDED  `docs/passive_branch_namespace_audit.md` (the audit deliverable)
  MOD    `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (include; `NR_PDSCH_PASSIVE_HARQ_TAG_BASE`
         moved to the new header; `t_view_branch` declaration moved up so `passive_ldpc_decode()` can read
         it; the tag expression at the former `:703`, now `:712`)
  MOD    `openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_branch_test.cc` (5 `PassiveHarqTag` cases)
  MOD    `tests/passive_rx/baselines/fixtures.json` (appended binary sha, P09 note, `branch_view_replay.p09_rerun`)
  ADDED  `tests/passive_rx/baselines/manifest_20260911_2566d25-dirty.json` + `_tracked.patch`
  MOD    this ledger
  NO change to `nr_pdsch_passive_job_t` — `sizeof` stays 384, the fixture's `job_bytes` check untouched.
  NO change to any UL or attached-UE tag site (see "Hypotheses" below for what was found there).

Executable / driver / config / geometry / acceptance hashes:
  `nr-uesoftmodem` sha256 `cd89de4928f8761a83ea494e57f28c9fae2deda32ca16204f3fa72e3f8f01800`
  `liboai_usrpdevif.so` sha256 `ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583` (UNCHANGED,
    and equal to the fixture's `producing_liboai_usrpdevif_sha256` — not rebuilt, nothing in it was touched)
  fixture `/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7`, receiver.conf `5022d875...`, full_auto=0.

Exact commands:
  make nr_rx_branch_test && ./nr_rx_branch_test
  make nr-uesoftmodem -j8                                   (no warnings, no errors)
  sudo -n env ISAC_PASSIVE_REPLAY_INPUT=<fixture>/replay.bin LD_LIBRARY_PATH=$(pwd):/usr/local/lib \
    ./nr-uesoftmodem -O <fixture>/receiver.conf -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
    --ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation --cont-fo-comp 1 \
    --freq-sync-P 0.05 --freq-sync-I 0.001 --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
    --ntn-initial-time-drift -4.25 -A 90
  sudo -n bash tests/passive_rx/replay_branch_view.sh
  PURPOSE="P09: ..." bash tests/passive_rx/baseline_manifest.sh
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260911_2566d25-dirty.json
  python3 tests/passive_rx/check_manifest.py --fixtures tests/passive_rx/baselines/fixtures.json

Artifact paths: `/tmp/p09_build.log`, `/tmp/p09_legacy.log`, `/tmp/p09_bv.log`,
  `/tmp/replay_branch_view.Vhzhhr/` (per-view logs + `PDSCH-VERDICT` trace).

Baseline and comparison definition: the P07/P06a recorded baselines, byte-for-byte —
  legacy `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0), and
  `replay_branch_view.sh` 34/34/31/34/34 crc_ok of 38 records with data_submits == own crc_ok per view.
  Any change to either was a declared STOP condition, to be investigated rather than tuned away.

Predeclared assertions / thresholds:
  (a) branch_id=0 must reproduce the literal pre-P09 tag `2000 + harq_process_nbr`;
  (b) the (branch x harq process) product must be injective;
  (c) no input, including malformed, may produce an id outside [2000, 3000);
  (d) both replays identical to baseline;
  (e) `sizeof(nr_pdsch_passive_job_t)` == 384.

Observed result, with denominators:
  `nr_rx_branch_test` 24/24 PASS (19 before + 5 new). (a) 16/16 process numbers pin exactly; (b) 64/64
  (branch, process) pairs distinct; (c) 65536/65536 (uint8 x uint8) inputs inside the namespace;
  (d) legacy replay exit 0 with the identical verdict line; branch-view script exit 0, per-view crc_ok
  34/34/31/34/34 of 38, data_submits equal to crc_ok on every view, unsupported_multilayer 0 on every view;
  (e) unchanged (no struct field added — the fix reads the existing `t_view_branch`).
  Stride bound, computed not chosen: `2000 + (4-1)*32 + (32-1) = 2127 < 3000`, headroom 873 ids; the
  stride is 32 because the DCI HARQ-process-number field is 4 bits by default but 5 with
  `harq-ProcessNumberSizeDCI-1-1`, which this receiver's own parser reads at the configured width
  (`nr_pdcch_blind_monitor.h:263` / `nr_pdcch_blind_monitor.c:2475`). Corrected in fix round 1: the
  first cut used 16 and would have aliased hpn 0 with hpn 16 on ONE branch at the 5-bit width.
  Manifest identity check OK; `--fixtures` OK after appending the new binary sha.

Status: IN_PROGRESS (P09 row), and NOT a gate pass.

Validity reasons and affected intervals: offline only. No radio was touched (X410 unreachable,
  `uhd_find_devices`: "No UHD Devices Found", confirmed by the controller for this session). The replay
  path opens no device (`nr-uesoftmodem.c:428-434`), which each run's own "no radio opened" verdict
  restates.

Hypotheses supported / contradicted:
  SUPPORTED — the P06a review's finding: `TAG_BASE + harq_process_nbr` is identical for branch 0's and
    branch 1's job on one occasion, by construction, because the fan-out copies the same `dlsch_pdu`.
  NEW, and recorded not fixed — the passive UL decode tags EVERY transport block `0`:
    `nr_pusch_passive_decode.c:1072` passes `ulsch_id = 0` into `nr_ulsch_decoding()`, which sets
    `harq_unique_pid = ULSCH_id` (`openair1/PHY/NR_TRANSPORT/nr_ulsch_decoding.c:129`), across up to `NR_PUSCH_PASSIVE_MAX_CTX` = 6
    concurrent contexts, and colliding with the attached `nr_dlsch_decoding.c:76` id for harq_pid 0.
    The file's own `PASSIVE_UL_HARQ_TAG_BASE 4000` exists to prevent exactly this and is NEVER REFERENCED.
    Not fixed here: `ulsch_id` also indexes `gnb->ulsch[ulsch_id]` inside the callee, so it is not a
    one-line substitution, and there is no offline test that could prove the change. → P08.
  NEW, recorded not fixed — the passive DL re-encode (3000 range, `blind_harq_tag()`) is not branch-strided
    either; it is an ENCODE-side id and no backend in this tree indexes state by one, and fixing it belongs
    with the P10 CFR-ABI change the queue already carries a `TODO(P10)` for.
  CONTRADICTED — the worry that the branch-view replay would move: it did not, on any view.
  TLS audit found NO case of branch-dependent content surviving into another job. `t_view_branch` /
    `t_view_ue` / `t_view_phys` are the correctly-scoped pattern (re-armed unconditionally on every path of
    `nr_pdsch_passive_branch_view()`); `g_harq` holds branch-dependent state but is protected by thread
    confinement plus a full per-job rewrite; everything else is written-before-read scratch.

Retraction, if any: none.

Remaining limitation — stated because the ledger must not imply more than was tested:
  1. **The fix is NOT validated end-to-end.** `replay_branch_view.sh` runs ONE VIEW PER PROCESS
     (`ISAC_DL_BRANCH_VIEW=<phys>`, a fresh `nr-uesoftmodem` each), so two branches' jobs with the same
     `harq_process_nbr` never coexist in one process, and the harness cannot produce the collision — hence
     cannot demonstrate its removal. Views 1-3 DID exercise non-zero branch tags (2032+/2064+/2096+) and
     produced payloads identical to the reference, which shows the new values are harmless, not that the
     collision is gone.
  2. **It could not be observed even if reachable**: the backend linked here (`nrLDPC_coding_segment`) keeps
     no per-`harq_unique_pid` state. The corruption mechanism (`nrLDPC_coding_aal.c:654/742`, HARQ-combined
     buffers addressed by the id) needs an AAL/bbdev accelerator, which this tree has never run. The fix
     therefore rests on code inspection plus the isolated arithmetic unit test, and on nothing else.
  3. G2 test 6 (branch-local noise estimation and timing under distinct per-channel impairments) and the
     race/sanitizer half of G2 test 7 are NOT done — both need a sanitizer build or live RF.
  4. The two recorded-not-fixed namespace classes above remain open (P08, P10).

Next highest-value action: P08's UL work should start by fixing finding 4.1 (the constant-`0` UL decode id),
since it is a live collision today and not merely a hazard created by future fan-out.

Reviewer / accomplishment date if gate passed: n/a (IN_PROGRESS, G2 exit item only partially met).

### Fix round 1 (2026-09-11, same day) — reviewer finding: the stride assumed a 4-bit HARQ field

IMPORTANT, and correct: `NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE` was 16, derived from "the DCI
HARQ-process-number field is 4 bits". That is only the DEFAULT. This codebase's own parser is told
the width: `nr_pdcch_blind_monitor.h:263` declares `harq_pid_bits` as "default 4; 5 with
harq-ProcessNumberSizeDCI-1-1", it is the 7th operator-settable field of
`pdcch_blind_monitor_dci_bits` (`adaptive_manual_dlul.conf:16`, currently `4` on this deployment),
and `nr_pdcch_blind_monitor.c:2475` reads `harq_pid` at exactly that width. At 5 bits
`harq_process_nbr` runs 0..31, so a stride of 16 aliased hpn 0 with hpn 16 **on the same branch** --
the collision P09 exists to remove, one axis over -- and additionally broke the branch-0 legacy-tag
identity above hpn 15, because the pre-P09 formula had no modulo and was correct to 2031. The
original `BranchZeroReproducesTheLegacyTag` could not see that break: it looped `h < 16`.

Fixed: `STRIDE 32`, which covers both configurable widths. New bound, recomputed:
`2000 + (4-1)*32 + (32-1) = 2127 < 3000`, headroom 873 ids; `static_assert` updated; the runtime
restatement in `StrideBoundHoldsForTheConfiguredBranchCount` now pins 2127. Both affected tests
widened to the full 0..31 range (`BranchZeroReproducesTheLegacyTag`, `AllBranchProcessPairsAreDistinct`
-- 128 pairs, was 64). Verified the widened pin is not vacuous: at stride 16 it fails at h=16
(`tag(0,16)` would be 2000, expected 2016). The "4-bit" derivation corrected in the header comment,
audit sec 3 and the commit message, each carrying the `nr_pdcch_blind_monitor.h:263` citation.
The 3000 re-encode range's own `harq_pid % 16` (`blind_harq_tag()`) carries the SAME 5-bit exposure;
it is out of scope here (P10) and the audit now says so explicitly rather than leaving it implied.

Also fixed: audit table site 5 was missing its directory -- it is
`openair1/PHY/NR_TRANSPORT/nr_ulsch_decoding.c:129`, not `NR_UE_TRANSPORT/`.

Confirmed on the reviewer's other note: the commit does legitimately carry `fixtures.json` plus the
manifest pair beyond the review package's stat. That is the established append-only convention
(`verified_with_binary_sha256` appended, `producing_binary_sha256` never overwritten) plus the
one-pair manifest rule, and it still holds this round -- the fixtures diff is 25 insertions / 3
deletions, not a reserialisation.

Re-run after the rebuild (`nr-uesoftmodem` sha256 `9b3b3b43a2b60ffc469c9351fe091b453e971e39b3fcf6da2b7ed395275e8d6e`):
`nr_rx_branch_test` 24/24; legacy replay `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24;
no radio opened` exit 0; `replay_branch_view.sh` 34/34/31/34/34 of 38, exit 0. Both STOP conditions
re-checked and neither fired. Views 1-3 now run on tags 2032+/2064+/2096+ (were 2016+/2032+/2048+).
Manifest for this round: `tests/passive_rx/baselines/manifest_20260911_535231d-dirty.json`
(the 2566d25 pair from the first commit is kept, not replaced). Artifacts: `/tmp/p09r1_build.log`,
`/tmp/p09r1_legacy.log`, `/tmp/p09r1_bv.log`, `/tmp/replay_branch_view.WvWZni/`.

## Session — 2026-09-11: P08a per-context UL harq_unique_pid namespacing (Stage 2, P09 follow-up, IN_PROGRESS)

```text
Date/time (Europe/Zurich): 2026-09-11 17:32 CEST (host clock is UTC; 15:32 UTC)
Task IDs / gate: P08a (offline slice of Stage 2 P08), gate G2 "no resource namespace collision".
  Finishes P09's audit finding 4.1. NOT a claim of P08 completion: P08's branch-independent UL
  decode VIEW is untouched, and the UL tag still has no branch axis because there is no UL fan-out.
Intended falsifiable claim: every concurrent passive UL decode context emits a DISTINCT
  harq_unique_pid, disjoint from every other submitter type's range, and no other behaviour moves.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing, parent
  ec24e5d74922f6532cc6e7ff644b37c74103406e; manifest pair
  tests/passive_rx/baselines/manifest_20260911_ec24e5d-dirty.json + _tracked.patch. Untracked and
  deliberately NOT committed: tests/passive_rx/aoa_track_dl.conf (another task's file).
Files modified / added / removed:
  openair1/PHY/NR_UE_TRANSPORT/nr_passive_harq_tag.h  (+58: the 5000 UL-decode range)
  openair1/PHY/defs_gNB.h                             (+9: PHY_VARS_gNB::harq_unique_pid_base)
  openair1/PHY/NR_TRANSPORT/nr_ulsch_decoding.c       (:133, the one behavioural line)
  openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c (base wired per ctx; dead define removed)
  openair1/PHY/NR_UE_TRANSPORT/tests/nr_rx_branch_test.cc (+4 cases)
  docs/passive_branch_namespace_audit.md              (sites 5+10, sec 4.1, sec 6)
  tests/passive_rx/baselines/fixtures.json            (append-only; + ul_config_replay record)
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem after  6607a1da4085604b8a4aee86946f9c3cb4bbb29611949dffa6eb9cd363a743de
  nr-uesoftmodem before 252df29ea0916d9c9ad2f5f983c0dfad7723c55adf6f70f94a74e7c89223ecc0
    (the pre-change binary, rebuilt from `git stash` purely to measure the baseline)
  liboai_usrpdevif.so ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583 (unchanged)
  fixture sensing_manual_fixed.UtvBT7 (replay.bin ea24da3e..., receiver.conf 5022d875...)
Exact commands:
  make -j8 nr_rx_branch_test nr-uesoftmodem      (pgrep -x nr-uesoftmodem empty first)
  ./nr_rx_branch_test
  OUT=/tmp/p08a_bv2 bash tests/passive_rx/replay_branch_view.sh
  env ISAC_PASSIVE_REPLAY_INPUT=$F/replay.bin ISAC_PASSIVE_REPLAY_UL_CONFIG=1 \
      LD_LIBRARY_PATH=$PWD:/usr/local/lib ./nr-uesoftmodem -O $F/receiver.conf -r 273 ... -A 90
  python3 tests/passive_rx/check_manifest.py tests/passive_rx/baselines/manifest_20260911_ec24e5d-dirty.json
Artifact paths (include raw logs and VOID attempts): /tmp/p08a_bv (pre-final build), /tmp/p08a_bv2
  (final), /tmp/p08a_ulcfg_before.log, /tmp/p08a_ulcfg.log, /tmp/before.txt, /tmp/after.txt.
  No VOID attempts. One build failure (string literals mangled by a remote heredoc), fixed, not an
  experiment.
Baseline and comparison definition: P07/P06a/P09's recorded replay baselines (legacy 34/0/24,
  branch view 34/34/31/34/34 of 38) and, NEW this session, a before/after pair on the UL-CONFIG
  replay measured on this session's own pre-change binary rather than inherited.
Predeclared assertions / thresholds: tags pairwise distinct over ctx 0..5; highest id 5005 < 6000;
  no tag inside 0..31 (the attached DL decode range); legacy + branch-view replay numbers unchanged.
Observed result, with denominators:
  nr_rx_branch_test 28/28 (was 24; 4 new PassiveUlHarqTag cases).
  Legacy replay: REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened, exit 0.
  replay_branch_view.sh: 34/34/31/34/34 of 38 records, script exit 0 — identical to P07/P06a/P09.
  UL-CONFIG replay (the path that actually runs the changed line): 24/24 UL grants replayed,
  crc=9/24 rejected=0 repeat_mismatches=0 BOTH before and after, all 24 per-grant lines
  byte-identical under diff.
Status (PASS / FAIL / VOID / BLOCKED): IN_PROGRESS (pending review). No gate is claimed passed.
Validity reasons and affected intervals: offline only, no radio (X410 unreachable). The replay
  evidence is valid for what it covers and is explicitly NOT evidence of collision removal.
Hypotheses supported / contradicted:
  SUPPORTED — `ulsch_id` cannot be tag-offset: nr_ulsch_decoding.c:101-102 uses it to index BOTH
    phy_vars_gNB->ulsch[] and ->pusch_vars[], and a passive context allocates exactly one of each,
    so the brief's "always 0" guess is right AND is a hard constraint, not an accident. The base
    therefore lives on the INSTANCE (PHY_VARS_gNB::harq_unique_pid_base) and the callee keeps
    adding the index.
  CONTRADICTED — "UL has no replay-through-full-decode path" (P08a's brief, echoed from P09/P02).
    nr_passive_replay_ul_config.h:37/42 calls nr_pusch_passive_decode(ue, 0, ...) under
    ISAC_PASSIVE_REPLAY_UL_CONFIG=1, so the changed line IS exercised offline on 24 recorded grants.
    Corrected in the audit doc and fixtures.json rather than left standing.
  CONTRADICTED — reviving the file's dead PASSIVE_UL_HARQ_TAG_BASE 4000 would have fixed this.
    4000 is already live as the UL RE-ENCODE base (nr_pusch_data_aided.h:23), so it would have
    swapped a decode-vs-decode alias for a decode-vs-re-encode one. Deleted; decode owns 5000+.
Retraction, if any: none of this session's own claims. Two inherited claims corrected (above).
Remaining limitation: replay drives ONE decode context sequentially and the linked backend
  (nrLDPC_coding_segment) keeps no per-id state, so no offline instrument in this tree can observe
  the collision before or its absence after — same standing as P09's DL fix. The DL RE-ENCODE
  range (3000, blind_harq_tag's `% 16`) is still unstrided and still carries the 5-bit exposure:
  P10. The UL tag gains no branch term until UL fan-out exists: P08.
Next highest-value action: P08 proper — the branch-independent UL decode VIEW; then P10's
  re-encode-side namespacing, which is where the remaining recorded collisions are.
Reviewer / accomplishment date if gate passed: pending.
```

### Design, file:line (as committed)

| Piece | Where | Note |
|---|---|---|
| `NR_PUSCH_PASSIVE_HARQ_TAG_BASE 5000` + `nr_pusch_passive_harq_tag_base(ctx)` | `nr_passive_harq_tag.h` | returns the BASE only; the callee still adds `ULSCH_id`, so upstream's "unique among one instance's ULSCHs" property is preserved and the context axis is added on top |
| `uint32_t harq_unique_pid_base` | `openair1/PHY/defs_gNB.h:374` | 0 for a real gNB — every `PHY_VARS_gNB` in the tree is `calloc`'d (`executables/nr-gnb.c:384` + the `SIMULATION/NR_PHY` sims), so upstream is bit-identical |
| `TB_parameters->harq_unique_pid = phy_vars_gNB->harq_unique_pid_base + ULSCH_id` | `openair1/PHY/NR_TRANSPORT/nr_ulsch_decoding.c:133` | the only behavioural line; the shared file had to be touched because `TBs[]` is its local |
| `gnb->harq_unique_pid_base = nr_pusch_passive_harq_tag_base(ctx)` | `nr_pusch_passive_decode.c`, `passive_gnb_prepare()` | one base per decode context |
| `NR_PUSCH_PASSIVE_ULSCH_PER_CTX 1` | `nr_passive_harq_tag.h`, used for `gnb->max_nb_pusch` AND the stride, `static_assert`'d beside the `ulsch`/`pusch_vars` allocation | the stride is DERIVED from the allocation, so the two cannot drift apart by hand |
| dead `PASSIVE_UL_HARQ_TAG_BASE 4000` | removed from `nr_pusch_passive_decode.c` | never referenced, and collided with the live UL re-encode base |

Bound, as arithmetic (not a chosen constant): `5000 + (6-1)*1 + (1-1) = 5005 < 6000`, headroom 994.
Unlike the DL stride there is no DCI-field width in play here — the second term is an ARRAY INDEX
whose range is the per-context `ulsch[]` size, which is why it is tied to the allocation instead.

### Fix round 1 (2026-09-11, same day) — reviewer findings addressed

Approved with 1 Important (documentation-only by the reviewer's own ruling) + 4 Minor. No
behavioural code change this round; one gtest and one C shim added.

IMPORTANT — **the accelerator PRUNES the id, so the base map's disjointness is a SOFTWARE property,
not a device guarantee.** Verified from source rather than taken on the review's word:
`nrLDPC_coding_aal.c:654-656` (and `:742-743`) compute
`segment_offset = harq_unique_pid * NR_LDPC_MAX_NUM_CB + i` with `NR_LDPC_MAX_NUM_CB = 144`
(`nrLDPCdecoder_defs.h:78`) and then `% active_dev.num_harq_codeblock`, whose default is **512**
(`nrLDPC_coding_aal.c:1083`, operator-settable). Two ids alias for the same segment index iff
`144*(p-q) = 0 (mod 512)`; `gcd(144,512) = 16`, so iff `p-q = 0 (mod 32)` — aliasing period
`num_harq_codeblock / gcd(NR_LDPC_MAX_NUM_CB, num_harq_codeblock)` = 32 at the defaults. The
review's concrete example holds: `5000 - 1000 = 4000 = 125*32`, so passive UL decode ctx 0 and the
attached DL re-encode at `harq_process_nbr` 0 prune onto one slot (moot inside a `--passive-rx`
process; the 1000 range is attached-only). Two further consequences found while doing the
arithmetic and recorded with it: **P09's DL branch stride is itself 32**, so branch `b` and branch
`b+1` at the same `harq_process_nbr` alias at the device — the collision that stride removes in
software; and `512/144 = 3.55`, so a default-configured device holds barely three ids' worth of
segments and **no base map can make more than that many concurrent ids safe** — the requirement is
operator-side (`num_harq_codeblock >= 144 * ids in flight`), with co-prime-safe spacing an
improvement on the margin, not a fix. All of it is inherited from P09's map, out of scope to change
here, and now documented in `nr_passive_harq_tag.h`'s header comment and a new subsection of the
audit's sec 2 so the map is never read as a hardware guarantee. Unreachable in this tree either
way: the linked backend is `nrLDPC_coding_segment`.

MINOR 2 — "every `PHY_VARS_gNB` is `calloc`'d" was imprecise. Corrected to ZERO-INITIALISED
everywhere it was claimed (`defs_gNB.h`'s field comment, audit sec 4.1, the report), enumerating all
four spellings: `calloc_or_fail` (`nr-gnb.c:384`, `ulsim.c:727`, `ulsim_mu_mimo.c:538`), `calloc`
(`dlschsim.c:350`, `ulschsim.c:371`, `pucchsim.c:402`), `malloc16_clear` (`pbchsim.c:386`) and
`malloc` + `memset` (`dlsim.c:754-755`, `prachsim.c:385-386`).

MINOR 3 — new gtest `PassiveUlHarqTag.AZeroInitialisedGnbReproducesTheUpstreamTag` pins the
zero-default on the REAL struct (and that `base + ULSCH_id == ULSCH_id` for `ULSCH_id` 0..7, i.e.
the literal pre-P08a expression). It needed a small C shim,
`openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_harq_tag_gnb_pin.{c,h}` (+2 lines of CMake):
`PHY/defs_gNB.h` pulls `common/utils/threadPool/thread-pool.h`, whose `_Atomic(uint64_t) dead_mask`
is C11 syntax that does not compile as C++ — found by trying the direct include first. The shim
uses only the type definition and `calloc`, so it adds no link dependency.

MINOR 4 — no action, as ruled.

MINOR 5 — citations fixed: audit sec 4.1 `nr_ulsch_decoding.c:101-102` -> `:102-103`; report body
`nr_pusch_passive_decode.c:1072` -> `:1084` (post-edit line; `:1072` was the pre-edit location and
is what the commit message correctly cites as "was").

Re-run (`nr-uesoftmodem` sha256 `6cc3f0689be2807d6e036349ab82e026f8f567f07d2801ccf9416ab24ee019e2`
— the binary DID change despite the edit being comment-only, so the replays were re-run rather than
assumed): `nr_rx_branch_test` **29/29** (was 28); legacy replay `REPLAY PASS: identical DL
controls=34 failed=0 raw UL=24; no radio opened` exit 0; `replay_branch_view.sh` 34/34/31/34/34 of
38, exit 0; UL-CONFIG replay `crc=9/24 rejected=0 repeat_mismatches=0` with all 24 per-grant lines
still byte-identical to the PRE-change baseline binary `252df29ea0`. Manifest for this round:
`tests/passive_rx/baselines/manifest_20260911_6aedd38-dirty.json` (the `ec24e5d` pair from the
first commit is kept, not replaced). Artifacts: `/tmp/p08a_r1_bv/`, `/tmp/p08a_r1_ulcfg.log`.

## Session — 2026-09-12: P11fix SSB CFR producer k_abs axis (Stage 3, P11 follow-up, defect closure)

```text
Date/time (Europe/Zurich): 2026-09-12 08:10-08:30 CEST (host clock UTC)
Task IDs / gate: P11fix (P11 audit findings P11-A1 and the k_ssb literal). No gate claimed.
Intended falsifiable claim: the SSB CFR producer's k_abs is the CRB/Point-A carrier-grid coordinate
  nr_isac.h:85-86 declares -- in [0, nof_prb*12) at the wrap, and equal to ssb_start_subcarrier + i
  for element i -- and both properties fail on the previous implementation.
Branch / full commit / dirty patch / untracked-file manifest:
  merge/adaptive-sensing, parent 8530b707e0505745ac8feefe1f0cf171ca407541.
  Before: 5 tracked modified + 1 untracked (tests/passive_rx/aoa_track_dl.conf, pre-existing,
  NOT touched and NOT committed here).
Files modified / added / removed:
  MOD openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h    signature: (start, carrier_bandwidth_sc, out);
                                                     k_ssb and ofdm_symbol_size both gone
  MOD openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.c    k_abs[i] = (start + i) % carrier_bandwidth_sc
  MOD openair1/SCHED_NR_UE/phy_procedures_nr_ue.c   the one call site: fp->N_RB_DL*12; comment
                                                     corrected (two of its claims were false)
  MOD openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc  new test_ssb_k_abs() (5 assertions)
  MOD docs/cfr_support_and_reference_contract.md    P11-A1 -> FIXED; new finding P11-A4; five
                                                     phy_procedures_nr_ue.c line citations
                                                     re-derived by grep (three were already stale
                                                     before this session, from P10b's branch loop)
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem sha256 f914159ca4bff8cdce1d2493f58bebb5544074f63998df760e8b762db382cf82
  fixture sensing_manual_fixed.UtvBT7 (registered, unchanged)
Exact commands:
  pgrep -x nr-uesoftmodem                                    EMPTY before every build
  make -j8 nr-uesoftmodem test_nr_isac_python_parity          rc=0
  ./test_nr_isac_python_parity                                "native sensing golden parity checks passed"
  (falsification) reinstate old expression, rebuild, run      rc=1 "SSB k_abs must start at
                                                              ssb_start_subcarrier"; restored+rebuilt
  (cd /tmp/p10a_offbuild && make nr-uesoftmodem -j8)          rc=0 (ENABLE_ISAC_SENSING=OFF links)
  <registered fixture replay command, fixtures.json>          rc=0
  OUT=/tmp/p11fix_bv bash tests/passive_rx/replay_branch_view.sh   dl_branch_view_replay: PASS
  bash tests/passive_rx/baseline_manifest.sh ; check_manifest.py
Artifact paths: /tmp/p11fix_legacy.log, /tmp/p11fix_bv/summary.txt
Baseline and comparison definition: the P07/P06a/P10c replay baselines (legacy 34/0/24;
  views 34/34/31/34/34 of 38) and the pre-change parity binary.
Predeclared assertions / thresholds: both replays byte-identical to baseline; parity PASS; OFF
  build links; the new test must FAIL against the old expression (else it proves nothing).
Observed result, with denominators:
  legacy replay "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened" exit 0
  replay_branch_view legacy 34 / view0 34 / view1 31 / view2 34 / view3 34 of 38, data_submits ==
    crc_ok on every view, script exit 0 -- identical to baseline
  test_nr_isac_python_parity PASS (26 cases, 1 new); falsified as predeclared
  ENABLE_ISAC_SENSING=OFF nr-uesoftmodem rc=0
Status (PASS / FAIL / VOID / BLOCKED): PASS (offline only; X410 unreachable, no live RF attempted)
Validity reasons and affected intervals: every run is the offline replay path, which prints its own
  "no radio opened" verdict. No radio was opened at any point.
Hypotheses supported / contradicted:
  CONTRADICTED (the brief's lead, refuted algebraically): recovering k_ssb as
    `ssb_start_subcarrier % 12` would have changed NO output. The only use of k_ssb was
    base_sc = ((start - k_ssb)/12)*12, and for start >= 0, start - (start % 12) == 12*floor(start/12),
    so floor((start - start%12)/12) == floor(start/12) -- exactly what the hardcoded 0 already gave.
    The recovery is an identity with the placeholder it would have replaced, not an improvement.
  CONTRADICTED (secondary, 3GPP field width): even taken at face value, `% 12` is not a valid
    recovery. nr_phy_common.c:482-483 shifts FR1's k_SSB by `>> scs_common`; at scs_common = 0
    (15 kHz) the post-shift sc_offset keeps the full TS 38.211 7.4.3.1 FR1 range 0..23 (in-tree
    witness: the `< 24` guard at nr_ue_dci_configuration.c:502), so `% 12` reports 11 for a true 23.
    FR2 passes k_SSB unshifted but its range is 0..11. So only FR1 at 15 kHz common SCS exceeds 12 --
    a real configuration, not a hypothetical.
  CONTRADICTED (the source comment being replaced, both halves):
    (a) "no field ... carries it either (verified: grepped the whole PHY tree, zero hits)" -- the
        NR_DL_FRAME_PARMS half holds (re-grepped, still zero), but a PHY-reachable path DOES exist:
        ue->nrUE_config.ssb_table.ssb_subcarrier_offset (fapi_nr_ue_interface.h:666, written by
        config_ue.c:210). The value was never unreachable; it is simply not wanted.
    (b) "CORRECT for this deployment (Phase 1 already confirmed this cell's actual kSSB is 0)" --
        the registered fixture runs --ssb 150 == 12*12 + 6, so 150 % 12 == 6. The defect was ACTIVE
        on this deployment's own captures, mislabelling every SSB row by 6 subcarriers.
  SUPPORTED (new, traced not assumed): the CRB flooring itself is the defect. Element i of the PBCH
    channel estimate is the estimate AT subcarrier ssb_start_subcarrier + i. Two independent walkers
    show it: nr_pbch_channel_estimation() (nr_dl_channel_estimation.c:644-773) starts re_offset at
    first_carrier_offset + ssb_start_subcarrier and dl_ch at index 0 and advances both by 12 per RB
    in lockstep (skipping 144 together for the SSS in symbol 1); nr_pbch_extract() (nr_pbch.c:45-62)
    walks rx_offset = first_carrier_offset + ssb_start_subcarrier against dl_ch_estimates index 0.
    The correct coordinate therefore carries no k_SSB term and no flooring, so the parameter was
    REMOVED rather than plumbed -- a shorter diff than either of the brief's two anticipated outcomes.
Retraction, if any: retracts the two source-comment claims quoted above (both were inherited, both
  were false when re-measured), and the docs' "inert today" disposition for the k_ssb literal.
Remaining limitation:
  * OFFLINE ONLY. The replays exercise the SSB tap's CODE path (the fixture's sources list contains
    "ssb") but their PASS criteria are CRC/data_submits counts and do not read k_abs at all, so they
    are a non-regression check, not evidence for the fix. The evidence for the fix is the unit test
    plus its falsification, nothing more.
  * No CPI ever closes in replay (nr_isac_start() is not called), so no SSB row has been observed
    landing at its corrected subcarrier in a real range profile.
  * The 6-subcarrier correction changes the SSB row's frequency labelling on every past ssb-sourced
    capture. Any sensing result taken from an "ssb" source before this commit used the wrong axis;
    none is quoted anywhere in this ledger, but do not inherit one.
  * P11-A2 is untouched: an out-of-grid k_abs is still dropped silently per RE at
    sensing_engine.cc:543. This fix removes one producer of such values; it adds no counter.
Next highest-value action: unchanged -- P08 (branch-independent UL decode view) and AoA-removal
  Stage B items 3-5.
Reviewer / accomplishment date if gate passed: n/a (no gate claimed).
```

## Session template — copy for each future work session

```text
Date/time (Europe/Zurich):
Task IDs / gate:
Intended falsifiable claim:
Branch / full commit / dirty patch / untracked-file manifest:
Files modified / added / removed:
Executable / driver / config / geometry / acceptance hashes:
Exact commands:
Artifact paths (include raw logs and VOID attempts):
Baseline and comparison definition:
Predeclared assertions / thresholds:
Observed result, with denominators:
Status (PASS / FAIL / VOID / BLOCKED):
Validity reasons and affected intervals:
Hypotheses supported / contradicted:
Retraction, if any:
Remaining limitation:
Next highest-value action:
Reviewer / accomplishment date if gate passed:
```

## Experiment table template

| Run ID | Date | Task/gate | Source/config identity | Validity | Active RX / mode | Main measured result | Evidence path |
|---|---|---|---|---|---|---|---|
| sensing_manual_fixed.XmtvnI | 2026-09-11 | P02 / G0 | binary `b064c995...` (current), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | VOID_RF_OR_ASSERT | 4 RX, manual/passive-rx | RFSTALL (UHD ERROR_CODE_OVERFLOW) at first USRP_RX_START; 0 SIB1; no replay attempted | `/home/sens/NICOLA/captures/sensing_manual_fixed.XmtvnI` |
| sensing_manual_fixed.JPdoGb | 2026-09-11 | P02 / G0 | binary `b064c995...` (current), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE (radio-level valid) / BLOCKED (replay recorder never armed) | 4 RX, manual/passive-rx | SIB1=1, DL crc_ok 38583/42014 (91.8%), UL crc_ok 10562/12694 (83.2%), clean exit, 0 RXDISCONT/RFSTALL -- but 0 `replay`-related log lines and no `replay.bin` produced (structural, source-cited in P02 session below, not RF variance) | `/home/sens/NICOLA/captures/sensing_manual_fixed.JPdoGb` |
| sensing_manual_fixed.3NB5Ri | 2026-09-11 | P02 / G0 fix round 1 | binary `b81b21e3...` (DL sweep_ticket gate fixed), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE / still BLOCKED (UL gate not yet found) | 4 RX, manual/passive-rx | SIB1=1, DL crc_ok 91.9%, UL try=33704 crc_ok 80.9%, clean exit -- still 0 replay activity; DL gate fix alone insufficient (UL `ul_seen` never true) | `/home/sens/NICOLA/captures/sensing_manual_fixed.3NB5Ri` |
| sensing_manual_fixed.JLDoZc | 2026-09-11 | P02 / G0 fix round 1 | binary `b81b21e3...`, same conf, REPLAY=1 | VOID_RF_OR_ASSERT | 4 RX, manual/passive-rx | RFSTALL (UHD ERROR_CODE_OVERFLOW) at t=157.27s, ordinary late-run RF variance, unrelated to the recorder; used to live-verify via `/proc/<pid>/environ` that `ISAC_PASSIVE_REPLAY_CAPTURE`/`FAILURES` do reach the process | `/home/sens/NICOLA/captures/sensing_manual_fixed.JLDoZc` |
| sensing_manual_fixed.LH2I9w | 2026-09-11 | P02 / G0 diagnostic (not counted against the 2-attempt cap; temporary instrumented build, DURATION=30) | binary `5745d305...` | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE | 4 RX, manual/passive-rx | First live evidence of the second defect: `dl#1..20` show `success=1` repeatedly but `ul_seen=0` always | `/home/sens/NICOLA/captures/sensing_manual_fixed.LH2I9w` |
| sensing_manual_fixed.Ma654f | 2026-09-11 | P02 / G0 diagnostic (not counted against the 2-attempt cap; temporary instrumented build, DURATION=60) | binary `74d2b818...` | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE | 4 RX, manual/passive-rx | "first UL seen" NEVER printed despite PDSCHQ decoded=7644/crc_ok=7050 and pusch_passive try=6114/crc_ok=5010 -- proved `nr_passive_replay_ul()` was never called, root-causing the `ul_auto`-gated call site at `nr_pdcch_blind_monitor_rt.c:1738-1742` | `/home/sens/NICOLA/captures/sensing_manual_fixed.Ma654f` |
| replay_branch_view (P07, /tmp/p07/bv3) | 2026-09-11 | P07 / G2 tests 1+3 (replay form) | binary `6bfe2c78...` (d1b563f511 + P07), fixture `sensing_manual_fixed.UtvBT7` (`c3810019...`-produced), receiver.conf `5022d875...`, full_auto=0 | replay PASS (no radio) | 4 RX legacy + single-branch views 0/1/2/3 | legacy 34/38 CRC-OK (line unchanged); view0 34, view1 31, view2 34, view3 34 of 38; data_submits == crc_ok on every view; unsupported_multilayer 0/38 (all Nl=1) | `/tmp/p07/bv3/summary.txt`, per-job trace in `/tmp/p07/bv3/*.log`; recorded in `fixtures.json` `branch_view_replay` |
| sensing_manual_fixed.UtvBT7 | 2026-09-11 | P02 / G0 fix round 2 (final) | binary `c3810019...` (DL + UL gates both fixed, diagnostics trimmed to the two permanent lines), `adaptive_manual_dlul.conf`, full_auto=0, REPLAY=1 | RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE, replay PASS | 4 RX, manual/passive-rx | SIB1=1, clean exit, 0 RXDISCONT/RFSTALL. `REPLAY ARMED at absolute_slot=8852`; `REPLAY READY ... slots=320 UL=24 DL-controls=38 IQ=314572800 bytes`. Replay verification (no radio): `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened`, exit 0, wall 4.463s. **P02 PASS.** | `/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7` |
| p06a_offline (/tmp/p06a) | 2026-09-11 | P06a / G1+G2 (offline only) | binary `b0ba7544...` (0667e4dd09 + P06a), fixture `sensing_manual_fixed.UtvBT7` | replay PASS (no radio); LIVE NOT ATTEMPTED -- X410 unreachable (`uhd_find_devices`: No UHD Devices Found, 13:48 UTC) | 4 RX legacy + 4-branch config parse | legacy replay unchanged (34 identical DL controls / 0 failed / 24 raw UL); replay_branch_view 34/34/31/34/34 of 38; nr_rx_branch_test 19/19 (4 new dispatch cases); sizeof(job) 384 unchanged; rx_branches=0,1,2,3 with --ue-nb-ant-rx 2 aborts at startup as designed, with 4 it replays PASS. NO per-branch live crc_ok, NO live multilayer fraction, NO queue-drop measurement under 4x load. | `/tmp/p06a/legacy_after.log`, `/tmp/p06a/bv/`, `/tmp/p06a/conf4_ant{4,2}.log` |
| p09_offline (/tmp/p09*) | 2026-09-11 | P09 / G2 (offline only) | binary `cd89de49...` (2566d256d2 + P09), fixture `sensing_manual_fixed.UtvBT7`, manifest `manifest_20260911_2566d25-dirty.json` | replay PASS (no radio); NO LIVE — X410 unreachable | 4 RX legacy + single-branch views 0/1/2/3 | `nr_rx_branch_test` 24/24 (5 new `PassiveHarqTag` cases: cross-branch injectivity over the whole 4x32 product, branch-0 legacy pin `2000+hpn` for hpn 0..31, out-of-range guard stays < 3000, stride bound 2127); legacy replay `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` exit 0; replay_branch_view 34/34/31/34/34 of 38, script exit 0 — both identical to the P07/P06a baselines. Views 1-3 ran on NON-ZERO branch tags (2032+/2064+/2096+) with payloads identical to the reference. NOT measured: any concurrent two-branch same-`harq_process_nbr` case (one view per process), no sanitizer build, no live RF. | `/tmp/p09_legacy.log`, `/tmp/p09_bv.log`, `/tmp/replay_branch_view.Vhzhhr/`, `/tmp/p09_build.log` |

| p08a_offline (/tmp/p08a*) | 2026-09-11 | P08a / G2 (offline only) | binary `6607a1da...` (ec24e5d749 + P08a), fixture `sensing_manual_fixed.UtvBT7`, manifest `manifest_20260911_ec24e5d-dirty.json` | replay PASS (no radio); NO LIVE — X410 unreachable | 4 RX legacy + views 0/1/2/3 + UL-CONFIG UL decode | `nr_rx_branch_test` 28/28 (4 new `PassiveUlHarqTag` cases: all 6 decode contexts pairwise distinct, none inside the attached DL 0..31, range disjoint from 0/31/1000/2000/2127/3000/3255/4000/4005, bound 5005). Legacy replay 34/0/24 exit 0 and `replay_branch_view` 34/34/31/34/34 of 38 — both unchanged. NEW: the UL decode IS offline-reachable (`ISAC_PASSIVE_REPLAY_UL_CONFIG=1`), 24 grants, `crc=9/24 rejected=0 repeat_mismatches=0` and all 24 per-grant lines byte-identical BEFORE (binary `252df29e...`, rebuilt from stash) and after. NOT measured: any two contexts concurrent (replay is sequential, one ctx), no AAL/bbdev backend, no sanitizer build, no live RF. | `/tmp/p08a_bv2/`, `/tmp/p08a_ulcfg{,_before}.log` |
| p10c_offline (/tmp/p10c*) | 2026-09-11 | P10c / Stage 3 (offline only) | binary `be296771...` (1c6885950c + P10c), fixture `sensing_manual_fixed.UtvBT7`, multi-branch confs `/tmp/p10b_multi/receiver{,.aoa1}.conf` (non-registered copies) | replay PASS (no radio); NO LIVE -- X410 unreachable | 4 RX legacy + views 0/1/2/3 + UL-CONFIG UL decode at 1 and 2 branches | `test_nr_isac_python_parity` PASS incl. new `test_ul_submit_plan()` (deliberately falsified: fails with `branches beyond the passive UL context allocated planes must be skipped, not sliced`). UL-CONFIG `crc=9/24 rejected=0 repeat_mismatches=0` and all 24 per-grant lines byte-identical before/after on the registered fixture conf, on a 1-branch engine-building conf and on a 2-branch conf. The 2-branch pair is the behavioural evidence: untagged-fan-in warnings 1 before / 0 after. `replay_branch_view` 34/34/31/34/34 of 38 unchanged. NOT measured: a closed CPI (replay never calls `nr_isac_start()`), two branches' CFR actually differing, any live RF. | `/tmp/p10c_ul_{before,after}.log`, `/tmp/p10c_ul_{1br,multi}_{before,after}.log`, `/tmp/p10c_bv_{before,after}/` |

For optional UL comparisons, list every 2-RX pair, 3-RX triple and 4-RX set for DTD, DFS and joint modes. Include unobservable modes, failures and missing-data denominators. Do not place placeholder accuracy values in this ledger.

## Rules for corrections and completion

- Keep old session entries; append corrections with the contradicted claim and new evidence.
- A valid low-performance run is FAIL, not VOID. A broken measurement/time/source contract is VOID; do not interpret its accuracy metrics.
- Do not check an implementation task merely because it builds or a helper test passes.
- Distinguish existing source blocks from newly validated four-branch behavior.
- Keep optional UL completion separate from required DL pipeline completion.
- No full completion claim until all required gates have dated, reviewable evidence.

## nr-ue.c audit and commit (2026-09-11)

Audited the uncommitted `executables/nr-ue.c` diff (7 hunks, +29/-3, from the 2026-09-10 manual DL/UL
port; `nr-ue-ru.c` read as context only, not committed). Committed as `be65aa8764c3`, that one file only.

| Hunk | What it does | Verdict |
|---|---|---|
| readFrame short read | short/failed read -> `oai_exit=1`, free trash buf, return (no acquisition on partial IQ) | OK; CONCERN: all modes, not passive-only (no-op on rfsim, which always returns nsamps); no early-out on entry, so warm-up loop may re-read a dead radio up to 49x before exit |
| syncInFrame | `return` when `oai_exit` after read | OK |
| UE_thread acquisition | `break` after readFrame if `oai_exit` | OK |
| first-symbol read after sync | `break` on `oai_exit || ret<0` | OK |
| slot read | `break` on `oai_exit || tmp<0` before RXDISCONT test/dispatch | OK |
| next-frame symbol read | same guard | OK |
| RFSTALL exit | `oai_exit=1; nrue_ru_stop(); nrue_ru_end();` before `exit(3)` | OK (`trx_usrp_end` idempotent); CONCERN: attached mode only -- a UL actor writing between `nrue_ru_end` and `exit(3)` would deref `priv==NULL`; also `nrue_ru_end` may block for UHD RPC timeouts on a dead claim (bounded, try/catch) |

Compile-level: `nrue_ru_stop/end/read` signatures match `nr-ue-ru.h`, header included, `oai_exit` via
`softmodem-common.h`; no build run (another agent may be building / capture may be live). The live
manual-profile capture (`sensing_manual_fixed.UtvBT7`) covers compilation and the happy path of the read
guards in `--passive-rx`; it does NOT exercise the error branches, attached mode, or rfsim.

## Dirty-tree audit and commits (2026-09-11)

Remaining uncommitted tracked files from the 2026-09-10 manual DL/UL port, audited file by file
(no build run), one commit per file on `merge/adaptive-sensing`. Full per-hunk analysis:
`.superpowers/sdd/adaptive_RX_pipeline/dirty-tree-audit-report.md` (local machine).

| file | hunks | verdict | SHA / reason not committed |
|---|---|---|---|
| `executables/nr-ue-ru.c` | 4 | OK (2 CONCERN) | `6e5b9f332a` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` | 2 | OK | `0153a129e1` (amended, message only) |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` | 2 | OK | `4ea9131417` |
| `radio/USRP/usrp_lib.cpp` | 16 | OK (3 CONCERN) | `88b19dac4a` |
| `tests/passive_rx/monitor/monitor.py` | 2 | OK (py_compile passes) | `3d4ad602ec` |
| `tests/passive_rx/monitor/monitor.html` | 8 | OK | `2a7db652e1` |
| `tests/passive_rx/run_adaptive_receive_test.sh` | 3 | OK (2 CONCERN, bash -n passes) | `f9fd0fe636` |

Top concerns (behaviour outside `--passive-rx`, none exercised by `sensing_manual_fixed.UtvBT7`):
`trx_usrp_read` now treats ANY UHD metadata error (overflow/timeout) as fatal -> `oai_exit`, so an
attached-UE B210 dies on a single recoverable 'O'; `trx_usrp_end` takes the non-recursive
`rx_mutex`, so an `exit_function()` entered on the producer thread while it holds the lock
(iqrecorder limit, SIGINT delivered to the UE thread) deadlocks instead of exiting; a full
`nrue_ru_reinit` now re-issues `set_gpio_src` on X410 (caught, returns -1, no core dump).


## Session — 2026-09-11: P10a CFR ABI branch-identity slot (Stage 3, P10 IN_PROGRESS)

Date/time (Europe/Zurich): 2026-09-11, ~17:30-19:00 CEST (host clock is UTC; +2h).
Task IDs / gate: P10 (CFR ABI and producer migration), controller-scoped to the ABI slice only —
the full producer migration and one-engine-per-branch remain Stage 3 / P13. Feeds G3; G3 NOT passed.

Intended falsifiable claim: branch identity can be added to the CFR submission ABI and carried to the
report without changing ANY existing numeric result or any unmigrated producer's behaviour.
Falsifiers, all three checked: (a) either replay baseline moving off 34/0/24 or 34/34/31/34/34;
(b) a branch field appearing in a report built only from untagged submissions; (c) the tagged report
line differing from the untagged one anywhere other than the two branch fields (and the wall clock).

Branch / full commit / dirty patch / untracked-file manifest:
  branch `merge/adaptive-sensing`, parent `b370bab566426bb9ffa55c289cf713b2f58f59e4`;
  manifest `tests/passive_rx/baselines/manifest_20260911_b370bab-dirty.json` + `..._tracked.patch`;
  pre-existing untracked file not touched by this task: `tests/passive_rx/aoa_track_dl.conf`.

Files modified / added / removed (no file added, no file removed):
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac.h`   (`NR_ISAC_BRANCH_NONE`, `nr_isac_submit_cfr_multi_branch()`)
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac.cc`  (new body; legacy entry point becomes a one-line wrapper)
  MOD  `openair1/PHY/NR_UE_ISAC/sensing_engine.h`  (`SensingEngine::submit()` gains a defaulted `branch_id`)
  MOD  `openair1/PHY/NR_UE_ISAC/sensing_engine.cc` (`Snapshot::branch_id`, `PendingRow::branch_mask`,
       accumulation in `consume()`, fold in `build_window()`, fold into the report in `process_window()`).
       The per-row mask is SPLIT BY DIRECTION (`dl_branch_mask`/`ul_branch_mask`) exactly as the source
       masks already are, because one PendingRow merges co-timed DL and UL submissions and a single
       mask would let a UL producer's branch tag surface in the DL window. That case is unreachable
       today (no UL producer is migrated) and the split is INSPECTION-ONLY, not covered by a test: at
       report level both windows fold into one `branch_mask`, so no report-level assertion can
       distinguish the two
  MOD  `openair1/PHY/NR_UE_ISAC/pipeline_types.h`  (`CfrWindow::branch_mask`)
  MOD  `openair1/PHY/NR_UE_ISAC/report_writer.h`   (`PipelineReport::branch_mask`)
  MOD  `openair1/PHY/NR_UE_ISAC/report_writer.cc`  (the omitted-when-unset JSON fields)
  MOD  `openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc` (`test_branch_identity_report()`)
  MOD  `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{h,c}` (`nr_pdsch_passive_view_branch()`)
  MOD  `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c` (TODO(P10) closed; all 4 submissions tagged)
  MOD  `tests/passive_rx/baselines/fixtures.json` (binary sha + P10a note)
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` (fix round 1: the missing OFF-build stub, plus a
       pre-existing missing `<stddef.h>` -- see fix round 1 below)
  MOD  `tests/passive_rx/baseline_manifest.sh` (ONE citation end-token, `SensingEngine::submit()`'s last
       parameter, which this task changed -- the generator hard-fails CITATION_ERROR otherwise)
  ADD  `tests/passive_rx/baselines/manifest_20260911_b370bab-dirty.json` + `_tracked.patch`
  MOD  this ledger
  The other 8 `nr_isac_submit_cfr_multi()` call sites (`csi_rx.c`, `nr_pdcch_blind_monitor_rt.c`,
  `nr_pusch_data_aided.c`, `nr_pusch_passive_decode.c`, `phy_procedures_nr_ue.c`) are UNTOUCHED.

Executable / driver / config / geometry hashes:
  `nr-uesoftmodem` sha256 `24a81dc52c9adafe9a7f21d383cd18ae58efdf5a18a8b6e90e21f63945284018`
  `liboai_usrpdevif.so` sha256 `ad0a71c56dbc509149337460af7d0e97c64d4f390f85bec48b8882aa64f64583` (UNCHANGED,
    not rebuilt, nothing in it was touched)
  fixture `/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7`, receiver.conf `5022d875...`, full_auto=0.

Exact commands:
  make nr-uesoftmodem test_nr_isac_python_parity -j8          (exit 0; the 3 warnings are pre-existing,
    in files this task did not touch: nr_pusch_passive_decode.c:315, phy_procedures_nr_ue.c:2030,
    nr_pdcch_blind_monitor_rt.c:1291)
  ./test_nr_isac_python_parity
  sudo -n env ISAC_PASSIVE_REPLAY_INPUT=<fixture>/replay.bin LD_LIBRARY_PATH=$(pwd):/usr/local/lib \
    ./nr-uesoftmodem -O <fixture>/receiver.conf -r 273 ... -A 90     (the registered replay command)
  sudo -n bash tests/passive_rx/replay_branch_view.sh
  PURPOSE="P10a: ..." bash tests/passive_rx/baseline_manifest.sh
  python3 tests/passive_rx/check_manifest.py <manifest> ; python3 tests/passive_rx/check_manifest.py --fixtures ...

Artifact paths: `/tmp/p10a_build.log`, `/tmp/p10a_legacy.log`, `/tmp/replay_branch_view.7jQoH2/`,
  `/tmp/p10a_rep/` (the report-writing replay experiment below, with its own edited receiver.conf).

Baseline and comparison definition: the P07/P06a/P09 recorded baselines, byte-for-byte — legacy
  `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0), and
  `replay_branch_view.sh` 34/34/31/34/34 crc_ok of 38 with data_submits == own crc_ok per view. Any
  change to either was a declared STOP condition, to be investigated rather than tuned away.

Observed result, with denominators:
  `test_nr_isac_python_parity` PASS, 15 cases (14 pre-existing + `test_branch_identity_report`).
  The new case was PROVEN NON-VACUOUS by deliberate falsification: changing its expected id 3 -> 4 made
  it fail with "a single-branch CPI must name its branch" (exit 1), and it passed again on restore.
  Legacy replay exit 0, verdict line identical, `REPLAY-VIEW phys=-1 branch=0 records=38 crc_ok=34
  crc_fail=4 unsupported=0 error=0 unsupported_multilayer=0 data_submits=34`.
  `replay_branch_view.sh` exit 0: legacy/view0/view1/view2/view3 crc_ok 34/34/31/34/34 of 38,
  data_submits equal to crc_ok on every view, unsupported_multilayer 0 on every view — the P07/P09
  baseline reproduced exactly.

Hypotheses supported / contradicted:
  CONTRADICTED, and it is the main measured finding of this session — the task brief's premise that
  "`adaptive_manual_dlul.conf` sets report_path/out_path and sensing.enable=1, so the replay genuinely
  runs the sensing engine and writes reports.jsonl". IT CANNOT, at any fixture duration:
  `executables/nr-uesoftmodem.c:437` returns from `nr_passive_replay_read()` inside the replay branch,
  while `nr_isac_start()` is at `:474` — so the engine's accumulation/processing threads never run and
  `SensingEngine::submit()` drops every row on `!running_.load()`. This is the same fact P07 already
  relied on when it added `nr_isac_data_aided_force` ("`nr_isac_submit_cfr_multi()` still drops the row
  when the engine is absent"), stated there from the other direction.
  Measured rather than inferred, in two steps: (1) the registered replay writes NO file at the fixture's
  own `report_path` (the fixture directory's file list is identical before and after the run — checked,
  because writing into a registered fixture would have been a defect of its own); (2) a supplementary
  run with a COPY of the receiver.conf pointing at a writable `/tmp/p10a_rep/reports.jsonl` AND carrying
  the surveyed `rx_pos_*`/`tx_pos_*` the original lacks (without them the engine refuses to construct at
  all: "SENSING: invalid native configuration: hierarchical ENU tracking requires surveyed noncoincident
  Tx/Rx") still produced 0 report lines, on the legacy path and under `ISAC_DL_BRANCH_VIEW=2`, both with
  the replay verdict unchanged. So the report-side half of this slice is pinned by the offline
  `test_branch_identity_report()` — the brief's own stated fallback — and NOT by any replay artifact.
  Honest consequence for review: the branch tag's journey through the real binary is proven only as far
  as the submission call (the replay's `data_submits=34` with an untagged `phys=-1` view); the
  accumulator-to-JSON half is proven in the offline harness, which drives the identical
  `SensingEngine`/`ReportWriter` code, not a re-implementation.
  SUPPORTED — the wrapper strategy: the 8 unmigrated producers were not audited argument by argument,
  they simply cannot change, because `nr_isac_submit_cfr_multi()` is one delegation with the "no
  identity" value and the body is shared. Both replays are the end-to-end restatement of that.

Deliberate deviation from the brief, and why: the brief specified the legacy wrapper pass `branch_id=0`.
  It passes `NR_ISAC_BRANCH_NONE` (0xFF) instead. Branch 0 is a REAL physical receive branch under P03's
  `rx_branches`/`rx_branch_phys_map` (and is the branch `nr_pdsch_passive_branch_view()` arms most often),
  so "0 == unset" would both make every legacy row claim branch 0 and make a genuine branch-0 row
  indistinguishable from an untagged one — which would have broken the brief's OWN requirement that the
  field be OMITTED, not zero, when unset. The structural guarantee the brief actually asked for (one-line
  legacy wrapper, shared body, no signature change at 8 call sites) is unaffected.

Fix round 1 (2026-09-11, controller review: one Critical + 3 Minor):
  Manifest for this round: `tests/passive_rx/baselines/manifest_20260911_d13a97a-dirty.json`
  + `..._tracked.patch` (parent `d13a97a1dd05f21e8787f17cc46d70d8d66af424`, the P10a commit).
  CRITICAL — `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` (compiled INSTEAD of nr_isac.cc when
  `ENABLE_ISAC_SENSING=OFF`, which is the CMake DEFAULT and the documented passive-receiver/X410
  production config) had no `nr_isac_submit_cfr_multi_branch()`, while `nr_pdsch_data_aided.c` calls it
  unconditionally and `PHY_NR_UE` links regardless of the option. The session's own build directory has
  sensing ON, so this never showed. Fixed with a no-op stub mirroring the existing
  `nr_isac_submit_cfr_multi` one. PROVEN BY LINK, not by inspection, in a SEPARATE build directory
  (`/tmp/p10a_offbuild`, `cmake -DENABLE_ISAC_SENSING=OFF -DENABLE_TESTS=OFF`): without the stub,
  `make nr-uesoftmodem` fails with three `undefined reference to 'nr_isac_submit_cfr_multi_branch'`
  naming `nr_pdsch_data_aided.c:419,428,434`; with it, exit 0.
  SECOND, PRE-EXISTING DEFECT FOUND BY ACTUALLY RUNNING THAT BUILD (this is why the reviewer asked for a
  link, not an eyeball): `nr_isac_stub.c` DID NOT COMPILE AT ALL under ENABLE_ISAC_SENSING=OFF --
  `NULL undeclared` at the two branch-set accessors added by P03, because `nr_isac.h` includes
  `<stdint.h>` but nothing provides `<stddef.h>`. So the OFF configuration has been broken since P03 and
  nobody noticed, the ON build directory never compiling this file. Fixed with the missing include;
  the OFF build now reaches a clean link. Not a P10a regression, fixed here because the link proof the
  review asked for is unobtainable without it.
  MINOR 2 — `static_assert(NR_RX_BRANCH_MAX <= 32, ...)` next to the branch-bit computation in
  `sensing_engine.cc`, so raising the branch count past the mask width fails loudly instead of silently
  leaving ids >= 32 untagged. MINOR 3/4 are report wording only (no code).
  Re-verified on the fix-round binary `24a81dc5`: parity test 15/15, legacy replay
  `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24` exit 0 with the fixture directory unchanged,
  `replay_branch_view.sh` 34/34/31/34/34 with data_submits == own crc_ok. The ON build directory is left
  in its ON state, matching the manifest; the OFF directory is a scratch tree under /tmp and is not part
  of the repository.

Status: IN_PROGRESS (P10 row, ABI slice only), and NOT a gate pass. G3 not passed.

Validity reasons and affected intervals: offline only. No radio was touched — the X410 is unreachable
  for this session by the controller's own statement, and every run here is the replay path, which each
  run's "no radio opened" verdict restates.

## Session — 2026-09-11: P14-audit AoA removal audit, staged plan, first safe removal (Stage 4, P14 IN_PROGRESS)

Date/time (Europe/Zurich): 2026-09-11.
Task IDs / gate: P14 (Stage 4, G4). AUDIT + STAGED PLAN + Stage A item 1 only. NOT a gate pass;
  G3 is not passed and G4 is not approached by this session.

Intended falsifiable claim: the `AOA_ENABLE` / `AOA_UL_ENABLE` environment override named in the
  plan's section 2.4 removal table can be deleted TODAY without touching the AoA DSP, the global
  tracker, the report schema or any CFR producer, and without changing either recorded replay
  baseline — and every other row of that table cannot, for a reason stated per row.

Branch / full commit / dirty patch / untracked-file manifest: `merge/adaptive-sensing`, parent
  `3241cca0ba956c2eecb45c6f2c6a6bdd51de34e1`. Manifest regenerated for this round (see hashes).
  Pre-existing untracked file `tests/passive_rx/aoa_track_dl.conf` predates this session, is not
  referenced by anything this session touched, and was deliberately left alone.

Files modified / added / removed:
  ADD  `docs/aoa_removal_audit.md`  — the deliverable: full touchpoint audit by section 2.4 row
       (file:line, what it does, whether removal breaks a passing test / the global tracker, and
       the stage it must wait for), plus the A/B/C staged removal order.
  ADD  `openair1/PHY/NR_UE_ISAC/nr_isac_env.c` — `nr_isac_obsolete_env_keys()`, `getenv()` only.
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac.cc` — removed `environment_bool()`, the two globals and the
       env override; `PipelineConfig` is now the single source of truth for the AoA switch; one
       `LOG_E` naming both obsolete keys when either is still set.
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac.h` — dropped `extern int AOA_ENABLE/AOA_UL_ENABLE`,
       declared `nr_isac_obsolete_env_keys()`.
  MOD  `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` — dropped the two globals (P10a touched this file;
       its `nr_isac_submit_cfr_multi_branch()` stub and `<stddef.h>` fix are untouched).
  MOD  `CMakeLists.txt` — `nr_isac_env.c` added to BOTH `NR_UE_ISAC_SRC` and the
       `ENABLE_ISAC_SENSING=OFF` stub library, so one definition serves both configurations.
  MOD  `openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc` — `test_obsolete_aoa_env_rejected()`.
  MOD  `tests/passive_rx/baselines/fixtures.json` — binary sha + note.
  MOD  this ledger.
  NOT TOUCHED, by controller ruling and by design: `aoa.cc/.h`, `hierarchical_tracker.{cc,h}`,
  `enu_tracker.{cc,h}`, `cross_leg_fusion.{cc,h}`, `report_writer.cc`, `pipeline_types.h`,
  `sensing_engine.{cc,h}`, `sync_correction.{cc,h}`, `csi_rx.c`, `nr_pdcch_blind_monitor_rt.c`,
  `nr_pdsch_data_aided.c`, `phy_procedures_nr_ue.c`, and P07's `t_view_branch` mechanism in
  `nr_pdsch_passive_decode.c` (distinct from AoA; explicitly checked, no coupling).

Executable / driver / config / geometry / acceptance hashes:
  `nr-uesoftmodem` sha256 `fffb56a2dfa61e978a89c1d5bc4851f54e00f33e90014ce6b3a9a618fb1e7a35`
  `liboai_usrpdevif.so` UNCHANGED (not rebuilt; nothing in it was touched).
  fixture `/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7`.

Exact commands:
  `cmake .` then `make nr-uesoftmodem test_nr_isac_python_parity -j8`  (exit 0; the only 3 warnings
    are the pre-existing ones in files this session did not touch: `nr_pusch_passive_decode.c:315`,
    `phy_procedures_nr_ue.c:2030`, `nr_pdcch_blind_monitor_rt.c:1291`)
  `./test_nr_isac_python_parity`
  `sudo -n env ISAC_PASSIVE_REPLAY_INPUT=<fixture>/replay.bin LD_LIBRARY_PATH=$(pwd):/usr/local/lib
    ./nr-uesoftmodem -O <fixture>/receiver.conf -r 273 ... -A 90`   (the registered replay command)
  `sudo -n bash tests/passive_rx/replay_branch_view.sh`
  `pgrep -x nr-uesoftmodem` (empty) before building.

Artifact paths: `/tmp/p14_cmake.log`, `/tmp/p14_build.log`, `/tmp/p14_legacy.log`,
  `/tmp/p14_bview.log`.

Baseline and comparison definition: the P07/P06a/P09/P10a recorded baselines, byte-for-byte —
  legacy `REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened` (exit 0), and
  `replay_branch_view.sh` 34/34/31/34/34 crc_ok of 38 with `data_submits == own crc_ok` per view.
  Any change to either was a declared STOP condition.

Predeclared assertions / thresholds: parity test passes with the new case; both replay baselines
  reproduce EXACTLY; the new case is proven non-vacuous by deliberate falsification.

Observed result, with denominators:
  `test_nr_isac_python_parity` PASS, 16 cases (15 pre-existing + `test_obsolete_aoa_env_rejected`).
  NON-VACUOUS BY FALSIFICATION: neutering the counter's `++present` in `nr_isac_env.c` made it fail
  with `AOA_ENABLE must be reported obsolete, not honoured` (exit 1); restoring it passed again.
  Legacy replay exit 0, verdict line identical, `REPLAY-VIEW phys=-1 branch=0 records=38 crc_ok=34
  crc_fail=4 unsupported=0 error=0 unsupported_multilayer=0 data_submits=34`.
  `replay_branch_view.sh` exit 0: legacy/view0/view1/view2/view3 crc_ok 34/34/31/34/34 of 38,
  `data_submits` equal to own crc_ok on every view — the recorded baseline reproduced exactly.

Status (PASS / FAIL / VOID / BLOCKED): IN_PROGRESS (P14 row, audit + Stage A item 1 only).
  NOT a gate pass. **Stages B and C are NOT done and are NOT safe to attempt yet**:
  * Stage B (5 items: engine channel count, the four `nr_isac_aoa_antennas()` producers + the
    accessor, per-branch decode provenance, the `rx_array_calibration` refactor, the P16 report
    schema version) needs P13 (one engine per branch) to exist first. Every one of those items
    replaces "how many antennas does the AoA switch allow" with "which branches are active", and
    there is no per-branch engine to route to yet. Removing the accessor now drops all four
    producers to one antenna.
  * Stage C (4 items, ending in the deletion of `aoa.cc/.h`, the `aoa_*`/`rx_array*` keys and the
    `AoaEstimate` model) needs Stage 5 (P17-P19, AoA-free fusion) to EXIST first.
    `hierarchical_tracker.cc:145` births a global track only from a detection carrying a valid AoA.
    Cutting the angle before the replacement model exists takes global fusion output to zero, with
    no offline harness able to show any replacement is equivalent — the exact regression the plan's
    own G4/G5 gating forbids ("Fusion cannot begin before this gate").

Validity reasons and affected intervals: offline only. No radio was touched — the X410 is
  unreachable for this session by the controller's own statement, and every run here is the replay
  path, whose own verdict line restates "no radio opened".

Hypotheses supported / contradicted:
  SUPPORTED — the section 2.4 table's `AOA_ENABLE`/`AOA_UL_ENABLE` names are NOT stale, but they
  are not configuration keys either: they were ENVIRONMENT variables silently overriding the real
  `[sensing] aoa_enable` / `aoa_ul_enable` keys (`nr_isac.cc:283`, via `environment_bool()` at
  `:165`). Both spellings coexisted. Nothing in the repository set them, and the two exported
  globals they wrote had no reader outside `nr_isac.cc` — measured across all
  `.sh/.py/.conf/.md/.c/.cc/.h`, not assumed. The two `AOA_ENABLE` strings in the parity test are
  assertion MESSAGES for `aoa_observed_mask()`, not symbol uses.
  CONTRADICTED — the brief's expectation that the config-key row would be the obvious Stage A item
  because "this repo's confs still use that literal spelling". They do not: eight
  `tests/passive_rx/ue.passive*.conf` set the lowercase `aoa_enable`, which is LIVE and must not be
  rejected until Stage C. Only the environment half was obsolete.
  CONTRADICTED — the brief's file list implies a CUDA AoA path to audit ("CPU/CUDA synchronization
  paths"). There is no `aoa` reference in any `.cu` file; the only sync-side symbol is
  `aoa_observed_mask()` in `sync_correction.cc:725`, a pure row-mask helper.
  CONTRADICTED — the working assumption that a removal sweep would find dead or duplicated AoA
  code to delete cheaply. Every public symbol in `aoa.h` has a production caller AND a parity-test
  case. There is no "unused" AoA code in this tree.
  Filename correction for the docs: the file is `aoa.cc`, not `isac_aoa.cc`; the stale name
  survives in `SIMULATION/TOOLS/sensing_channel.c:632` and in several project documents.

Retraction, if any: none.

Remaining limitation: the removal executed here is a configuration-surface back door, not any part
  of the AoA measurement path — by design. The audit is a static one: it resolves every symbol to
  its callers and every config key to its consumers, but it did not run an AoA-enabled capture
  (no radio, and the replay path cannot start the sensing engine — see P10a's measured note at
  `nr-uesoftmodem.c:437` vs `:474`). A definition of `nr_isac_obsolete_env_keys()` inside
  `nr_isac.cc` was tried first and does not link the offline test (it pulls `nr_isac.o` in, which
  needs `config_get`/`uniqCfg`/`exit_function`/the branch-set parser); the separate translation
  unit is that constraint, not a preference.

Next highest-value action: P13 (one engine per branch). It is the unlock for all five Stage B
  items, and four of them are literally the same edit repeated at four producers.

Reviewer / accomplishment date if gate passed: n/a — not a gate pass.

## Session — 2026-09-11: P11+P12 CFR support/reconstruction and range-reference contract audit (Stage 3, G3 inventory)

Date/time (Europe/Zurich): 2026-09-11.
Task IDs / gate: P11 + P12 (Stage 3, G3). AUDIT + CONTRACT DOCUMENT + G3 TEST INVENTORY only,
  controller-scoped (mirrors the P03/P09/P14-audit pattern). NOT a gate pass; G3 remains NOT PASSED.
  No code changed. No radio (X410 unreachable).

Intended falsifiable claim: for each of the six CFR producers and for the reported range axis, every
  clause of the plan's P11 and P12 paragraphs is either already true in this tree — provable by
  citing the line that makes it true — or is not, in which case the exact line and the reason are
  named. Specifically: (i) `k_abs` is the CRB/Point-A coordinate at every producer; (ii) the row's
  physical measurement time is a MEASURED symbol time, not an assumed slot boundary; (iii) `noise_var`
  is per-branch now that P10a's `branch_id` exists; (iv) allocation membership is tracked per CPI;
  (v) `bistatic_range_m` has ONE defined physical reference; (vi) admission failure yields an invalid
  reference status rather than a confident number. Claims (ii), (iii), (iv) and (vi) are REFUTED;
  (i) is upheld in value but is unenforced and one producer normalises against the wrong modulus;
  (v) is refuted in the specific sense that the field has THREE possible meanings, selected by a
  boolean elsewhere in the report.

Branch / full commit / dirty patch / untracked-file manifest: `merge/adaptive-sensing`, parent
  `57ec7dccaaa752d54a6d89f22a0f29665ecc85b3`. No binary produced, no build run, so no manifest
  regeneration and no `fixtures.json` append (the one-pair rule is not engaged — nothing was
  compiled). Pre-existing untracked `tests/passive_rx/aoa_track_dl.conf` predates this session, is
  not referenced by anything here, and was deliberately left alone (same disposition as P14-audit).

Files modified / added / removed:
  ADD  `docs/cfr_support_and_reference_contract.md` — the deliverable. Part P11 (support and
       reconstruction) covers all six producers on (a) `k_abs` coordinate, (b) `l_sym`/`slot_frac`
       symbol-time semantics, (c) `noise_var` provenance, (d) allocation/branch membership, and
       (e) a search for symbol-level Doppler claims. Part P12 (range/frequency reference) states the
       range-reference contract, the three regimes it has today, the admission-failure behaviour,
       the dominant-tap-is-LOS assumption, and why per-branch references are blocked on P13. Ends
       with a 13-row findings summary, the G3 five-test inventory, and an explicit "what was not
       fixed and why".
  MOD  `adaptive_RX_pipeline_progress.md` — this entry, the P11/P12 rows, and the G3 gate row.

Executable / driver / config / geometry / acceptance hashes: n/a — nothing built, nothing run.

Exact commands: read-only inspection on `sens6` (`grep -n` / `sed -n` over
  `openair1/PHY/NR_UE_ISAC/`, `openair1/PHY/NR_UE_TRANSPORT/`, `openair1/PHY/NR_UE_ESTIMATION/`,
  `openair1/SCHED_NR_UE/`). Every line number in the deliverable was re-checked against the file
  after drafting; six citations were wrong on first pass and were corrected (`sensing_engine.cc`
  531→543 and 525→541, `csi_rx.c` 988→989, `nr_dl_channel_estimation.c` 643→644,
  `report_writer.cc` 181-185→178-182, `phy_procedures_nr_ue.c` 1470→1469).

Artifact paths: `docs/cfr_support_and_reference_contract.md` (in-tree, committed).

Baseline and comparison definition: n/a — no measurement. This is a source audit; its evidence is
  the cited lines, and its falsification method is re-reading them.

Predeclared assertions / thresholds: n/a.

Observed result, with denominators:
  P11 (a) `k_abs` — CORRECT IN VALUE at 6/6 producers. NOT enforced anywhere: an out-of-grid `k_abs`
    is dropped one RE at a time at `sensing_engine.cc:543` with no counter and no log, so a wrong
    producer coordinate shrinks the row's support invisibly. One producer, SSB
    (`nr_isac_ssb_axis.c:11`), normalises modulo `ofdm_symbol_size` (4096 at 273 PRB) rather than
    the declared `nof_prb*12` (3276); inert for every in-carrier SSB placement, latent otherwise.
    Separately, measured support is overstated by 2 producers: SSB claims 240 REs from a PBCH DM-RS
    interpolation that measures ~60, and PUSCH DM-RS claims comb-1 from a comb-2 interpolation;
    CSI-RS at `csi_type != 0` reports an RB-aggregate at `k = rb*12` (`csi_rx.c:313-314`).
  P11 (b) symbol time — `l_sym` is DEAD: assigned at `sensing_engine.cc:339` and never read
    anywhere in that file (`consume()` from `:482` reads `s.subcarrier[i]` only; `PendingRow` has no
    symbol field). The only time coordinate is `slot_idx + slot_frac`, and FIVE of 6 producers pass
    `slot_frac = 0.0f` unconditionally (CORRECTED in fix round 1 — this read "4" at first commit and
    contradicted the deliverable's own table), i.e. their rows are timestamped at the slot boundary — an assumption of up to
    13/14 slot (~464 us at 30 kHz). The one producer that computes it (`nr_pdsch_data_aided.c:415`)
    uses a uniform 1/14-slot model that ignores the long cyclic prefix. SSB additionally passes a
    PBCH-RELATIVE symbol index (0..2), which is the wrong domain and is harmless only because
    nothing reads it.
  P11 (c) branch-specific noise — NO, and P10a does not change it. All 6 producers pass a single
    scalar: 3 pass an ANTENNA-AVERAGED value (`csi_rx.c:426` divides by `nb_antennas_rx*ports`;
    `nr_dl_channel_estimation.c:1486` returns `nvar_acc/nvar_ant_count`), 2 pass the literal `1.0f`,
    and SSB passes `0.0f`. The per-antenna value ALREADY EXISTS and is discarded one call earlier:
    `nr_dl_chest_nvar_ant[]` (`nr_dl_channel_estimation.c:31`, written at `:1310`/`:1476`) is already
    consumed per branch by `nr_pdsch_passive_decode.c:1366-1378`. Second, separate finding: the
    engine uses `weight = 1/noise_var` (`sensing_engine.cc:541`) as an inverse-variance merge weight,
    but the four producer-side quantities are in four unrelated scales, so any CPI mixing sources
    combines them by a ratio set by units rather than by SNR.
  P11 (d) allocation membership — SOURCE-class and BRANCH membership are tracked (`PendingRow`'s
    `source_mask`/`source_occurrences[]`/`dl_branch_mask`/`ul_branch_mask`,
    `sensing_engine.cc:57-80`; window-level `branch_mask`, `pipeline_types.h:40-45`; report at
    `report_writer.cc:128-134`). Per-TRANSMISSION membership is NOT: no RNTI, no `(start_rb,num_rb)`,
    no HARQ/TB key anywhere, and `nr_isac_submit_cfr_multi_branch()` (`nr_isac.h:102-113`) has no
    parameter that could carry one. `align_allocation_families()` counts families INFERRED from the
    observed support pattern; that is an inference, not recorded membership.
  P11 (e) symbol-level Doppler — NO such claim found (searched every `doppler` hit under
    `NR_UE_TRANSPORT/`, `SCHED_NR_UE/`, `executables/` — all are NTN ephemeris pre-compensation —
    and every `per-symbol`/`symbol-level`/`single symbol` hit under `NR_UE_ISAC/` and `docs/`). One
    wording over-claim reported as such, not as a violation: `nr_pdsch_data_aided.c:375` says
    sub-slot grouping multiplies "the effective PRF", which is a uniform-sampling statement about a
    strongly non-uniform sequence. The code is correct (the axis is built from `row_time_slots`,
    not an assumed PRF); only the comment over-claims.
  P12 range reference — `bistatic_range_m` has THREE meanings today under one field name.
    `sensing_engine.cc:807` computes `range_bin * range_res_m` and subtracts nothing; but
    `apply_sync_correction(..., delay_reference_bin = 0.0, ...)` (`sensing_engine.cc:784`) shifts the
    whole window by `-los_bins` when `sto_applied || sfo_applied` (`sync_correction.cc:378-379`),
    i.e. it moves the admitted direct path onto bin 0 IN THE CFR DOMAIN. So: (C) sync admitted =
    bistatic EXCESS path; (B) sync ran and admission failed = raw receiver-window-origin delay,
    `reject_reason` set; (A) `sync_enable=false` or `rows<3` (`sensing_engine.cc:775`) = raw delay,
    `reject_reason` the EMPTY STRING and `los_bins` a default rather than a measurement. This
    reconciles, and corrects, the note carried in project history that "range_m subtracts no LOS
    reference" — the subtraction exists, it is just conditional and one layer upstream of the formula.
  P12 admission failure (G3 test 5) — the CPI-level validity record IS published and is more complete
    than the brief assumed (`report_writer.cc:171-177`: `rows`, `admitted_rows`, `los_bins`,
    `sto/sfo/cfo` values + `applied` flags, `reject_reason`; UL mirror at `:234-243`). What does not
    exist is an invalid-reference STATUS: `detect_clean()` runs unconditionally
    (`sensing_engine.cc:796`) and `range_m` is written for every component (`:801-810`) with no
    reference check, so a regime-A/B detection is byte-indistinguishable from a regime-C one apart
    from a boolean four fields away.
  P12 "a reflected dominant path is not automatically LOS" — the code assumes it IS:
    `sync_correction.cc:236-238` takes the arg-max of the mean CIR power profile as the anchor, with
    no geometry check and no earliest-arrival preference. Directly analogous to the already-closed
    2026-08-11 range-bias root cause (a target as strong as the direct path captured the UE's own
    timing loop), one layer up.
  P12 per-branch references — NOT YET, and structurally blocked: one `SensingEngine`, one
    `dl_clock_tracker_` (`sensing_engine.cc:777`), one `report.sync` per CPI, and rows from different
    branches merged into the same `PendingRow`. So the current state is not "identical offsets across
    branches" (the error the plan clause anticipates) but one offset fitted from POOLED multi-branch
    data. Unfixable at the reference layer; needs P13's engine-per-branch.
  G3 test inventory — 0 of 5 covered, 2 of 5 PARTIAL, 3 of 5 not covered:
    1 (worker-delay invariance) PARTIAL — the invariant holds structurally (every physical time
      derives from producer `slot_idx + slot_frac`, never wall clock; the deferred path publishes the
      producer's monotonic slot via `nr_isac_abs_slot_override`), and `test_causal_cpi_pipeline()`
      (`python_parity_test.cc:436`) covers close/drain/restart ordering, but no test varies delay or
      compares two runs. Residual violation: `start_utc_ns` is stamped at `submit()`, on the CONSUMER
      thread for deferred paths, so it is a processing time reported where P16 requires an
      acquisition-derived one.
    2 (wrap/reorder/duplicate/RNTI/epoch) PARTIAL — wrap and reorder ARE implemented
      (`unwrap_submission_slot()`, `sensing_engine.cc:399-412`, signed delta folded to +/-cycle/2)
      and duplicates merge by design on `row_key`; but no test exercises any of them, and RNTI
      reassignment / epoch reset are invisible at this layer (finding P11-D1).
    3 (support/frequency-axis/symbol-timestamp sweep) NOT COVERED — nothing sweeps producer-side
      support; the support mask cannot express interpolated-vs-measured (P11-A3) and silently loses
      out-of-grid REs (P11-A2); symbol timestamps do not exist to verify (P11-B1).
    4 (known timing/CFO/SFO perturbation) NOT COVERED — `cuda_sync_test.cc:76-101` is a CPU-vs-CUDA
      PARITY test, not an accuracy test; the two benchmarks are timing harnesses. No estimator-
      accuracy test exists. The `selftest`/`selftest_los` injection mechanism from older project
      history does NOT exist in this tree (grepped `openair1/` for `selftest`: zero hits) — that
      pipeline was rewritten. Offline-testable TODAY via the existing synthetic-`CfrWindow` pattern
      in `python_parity_test.cc`; judged the single highest-value missing test.
    5 (deliberate admission failure) NOT COVERED, and the property does not hold — there is no
      invalid-reference status to assert, so the test cannot pass until the field of P12-2a exists.

Status (PASS / FAIL / VOID / BLOCKED): IN_PROGRESS for P11 and P12 (contract documented and
  G3 inventoried; the contract clauses themselves are largely NOT met by the code). NOT a gate pass.
  G3 = NOT PASSED, 0/5 covered.

Validity reasons and affected intervals: n/a — source audit, no capture, no interval.

Hypotheses supported / contradicted:
  CONTRADICTED — "P11's CRB/Point-A convention may already be satisfied, so there may be nothing to
    do": satisfied in value, but unenforced, undetectably violated at the accumulator, and wrong in
    modulus at one producer.
  CONTRADICTED — "P10a's `branch_id` may have made noise per-branch": it did not; `branch_id` is an
    identity tag and never touches `noise_var`.
  CONTRADICTED — "`range_m` subtracts no LOS reference" (carried in project history): it does,
    conditionally, in the CFR domain at `sensing_engine.cc:784` + `sync_correction.cc:378-379`.
  SUPPORTED — "P12 may already be partially satisfied by `los_bins`/`sto`/`sync`": the CPI-level
    correction values and validity ARE exported and are near-complete; only the per-detection
    reference STATUS is missing.
  SUPPORTED — "branch-consistent definitions may legitimately be blocked on Stage B/P13": confirmed,
    and for a stronger reason than expected (one pooled reference, not four identical ones).

Retraction, if any: none of this session's own. One inherited claim is corrected rather than
  retracted (the "range_m subtracts no LOS reference" note, above). The "~40 m range-axis bias" was
  re-read as CLOSED and is explicitly recorded as closed in the deliverable, per the standing
  instruction; it was NOT re-derived and NOT cited as a live defect.

Remaining limitation: this is an audit. Not one of the 13 findings is fixed, and three of them
  (P11-C1 per-branch noise, P11-D1 allocation membership, P12-4 per-branch references) require ABI
  or engine changes that belong to P13. The deliverable's contract statements are normative text,
  not enforced invariants — nothing in the build fails if a future producer violates them.

Nothing fixed, and why (deliverable 3, deliberately declined): three candidates were considered.
  (1) OMIT `bistatic_range_m` on invalid reference — the brief's own suggested example — was
  rejected because regime A (`sync_enable=false`) is a legitimate configuration used by every
  `tests/sensing_sim` scene conf, so omitting there would delete all detection output from those
  runs; it is also a schema behaviour change that P16 owns. (2) ADD a `range_reference` status field
  now — additive and cheap, but a schema change without P16's version bump, and its downstream
  compatibility cannot be verified from this task (no radio; the replay path cannot start the
  sensing engine), so claiming it safe would be exactly the unmeasured-inherited-claim failure this
  project has been burned by. (3) COUNT the silently dropped out-of-grid REs (P11-A2) — genuinely
  worth doing, but it touches `sensing_engine.{h,cc}` and the report, needs a rebuild plus manifest
  regeneration under the one-pair rule, and needs its own test; it belongs with G3 test 3, where the
  assertion that consumes the counter gets written at the same time. Audit + inventory alone is the
  correct outcome and is recorded as such.

Next highest-value action: G3 test 4 (inject a known delay/CFO/SFO into a synthetic `CfrWindow` and
  assert recovery). It needs no new mechanism — `python_parity_test.cc` already builds synthetic
  windows and drives the public API — it is the only test that can tell whether the sync estimator
  is CORRECT rather than merely self-consistent, and P12's entire range contract rests on it.
  Second: the `range_reference` status field (P12-2a), scheduled with P16's schema version bump,
  which unblocks G3 test 5.

Reviewer / accomplishment date if gate passed: n/a — not a gate pass.


## Session — 2026-09-11: P13a independent per-branch SensingEngine array (Stage 4, G4)

```text
Date/time (Europe/Zurich): 2026-09-11, evening.
Task IDs / gate: P13 (Stage 4, G4). CONTROLLER-SCOPED to the engine array and its routing:
  NOT the whole of P13/G4 (AoA removal is P14; tagging the other five CFR producers is P10's
  continuation; both were explicitly excluded by the task brief and both are gated on this).
Intended falsifiable claim: replacing the single process-wide SensingEngine with one instance per
  active receive branch, routed by the branch_id P10a added to the CFR ABI, (a) leaves a
  single-branch/legacy receiver bit-identical -- same rx_id, same out_path, same report_path, same
  replay verdicts -- and (b) cannot misroute a CFR row into a branch that did not measure it.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing, parent
  1b6a175ca60a0ff15a504a550bde708353494651; before: only `?? tests/passive_rx/aoa_track_dl.conf`
  (another session's, untouched); manifest
  tests/passive_rx/baselines/manifest_20260911_1b6a175-dirty.json (+ _tracked.patch).
Files modified / added / removed:
  MOD  openair1/PHY/NR_UE_ISAC/nr_isac.cc         singleton -> engine array; construction loop;
                                                  start/stop over the array; routing + drop counters
  MOD  openair1/PHY/NR_UE_ISAC/pipeline_types.h   branch_suffix_path/_endpoint,
                                                  branch_pipeline_config, branch_engine_index
  MOD  openair1/PHY/NR_UE_ISAC/nr_isac.h          branch_id is now load-bearing (comment)
  MOD  openair1/PHY/NR_UE_ISAC/sensing_engine.h   submit()'s branch_id comment: routing is one
                                                  layer up, an engine never sees a foreign row
  MOD  openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc   3 new cases
  MOD  tests/passive_rx/baselines/fixtures.json   binary sha + P13a note
  MOD  adaptive_RX_pipeline_progress.md           this entry, the P13 row, the G4 gate row
  ADD  tests/passive_rx/baselines/manifest_20260911_1b6a175-dirty.json (+ _tracked.patch)
Executable / driver / config / geometry / acceptance hashes: nr-uesoftmodem sha256
  a1eba0837e1874f6b5fe46a248b1da9d4554f315c2412cd2a790aa04dd33d241 (every number below is from
  THIS binary; liboai_usrpdevif.so not rebuilt). No radio touched -- the X410 is unreachable and
  every run below is the offline replay path, which prints its own "no radio opened" verdict.
Exact commands:
  pgrep -x nr-uesoftmodem                                   (empty before every build)
  make nr-uesoftmodem test_nr_isac_python_parity -j8        rc=0, no new warnings
  ./test_nr_isac_python_parity                              "native sensing golden parity checks passed"
  (cd /tmp/p10a_offbuild && make nr-uesoftmodem -j8)        rc=0  (ENABLE_ISAC_SENSING=OFF still links)
  <registered fixture replay command, fixtures.json>        rc=0
  sudo -n bash tests/passive_rx/replay_branch_view.sh       dl_branch_view_replay: PASS
  bash tests/passive_rx/baseline_manifest.sh ; check_manifest.py [--fixtures]
Artifact paths (include raw logs and VOID attempts): manifest + fixtures.json above; supplementary
  multi-branch runs in /tmp/p13a_multi (rx_branches="0,1") and /tmp/p13a_multi2 (rx_branches="1,2"),
  both non-registered /tmp copies of the fixture receiver.conf -- the registered fixture and its
  conf were never modified (directory listing captured before/after and identical).
Baseline and comparison definition: the two registered replay baselines (legacy 34/0/24 and the
  five-view 34/34/31/34/34), plus the parity binary, all at parent 1b6a175ca6.
Predeclared assertions / thresholds: both replay baselines byte-identical; parity suite green;
  a single-branch config's derived PipelineConfig equal field-for-field to the base.
Observed result, with denominators:
  - Parity suite 18/18 (was 15/15).
  - Legacy replay: "REPLAY-VIEW phys=-1 branch=0 records=38 crc_ok=34 crc_fail=4 ...
    data_submits=34" and "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio
    opened", rc=0. UNCHANGED.
  - replay_branch_view.sh: legacy/view0/view1/view2/view3 crc_ok 34/34/31/34/34 of 38,
    data_submits == own crc_ok on every view, "dl_branch_view_replay: PASS". UNCHANGED.
  - Supplementary rx_branches="0,1": startup log "... array_calibration=0 engines=2" and BOTH
    derived report paths created (/tmp/p13a_multi/reports_b0.jsonl, reports_b1.jsonl);
    rx_branches="1,2" created reports_b1/_b2. The construction loop and the per-branch path
    derivation therefore run in the REAL binary, not only in the unit test.
  - Falsification (both new gates, rebuilt and re-run): making branch_engine_index() fall back to
    engine 0 for an inactive branch -> "a CFR tagged with an INACTIVE branch must be dropped, never
    misrouted to another engine", exit 1; making branch_pipeline_config() suffix at n_active==1 ->
    "a single-branch receiver must keep exactly the configured output identity, unsuffixed",
    exit 1. Both pass again on restore.
Status (PASS / FAIL / VOID / BLOCKED): delivered; P13 IN_PROGRESS, G4 NOT passed.
Validity reasons and affected intervals: offline only. No radio.
Hypotheses supported / contradicted: SUPPORTED -- "instantiating N of the same class is
  sufficient". SensingEngine holds no static, no global and no file-scope mutex: the accumulator
  (rows_), CPI planner, adaptive clutter map, both IndependentClockTrackers, both MotionTrackers,
  the HierarchicalEnuTracker, cpi_sequence_, the snapshot pool/queues, both worker threads and the
  ReportWriter (which opens its own std::ofstream and its own zmq_ctx_new()) are all non-static
  members. The BLOCKED stop condition did not trigger.
Retraction, if any: none.
Fix round 1 (2026-09-11, controller review: 2 Important + 4 Minor, all addressed): see the P13
  row above and task-P13a-report.md's "Fix round 1" section. Files additionally touched:
  nr_rx_branch.{c,h} (reject a phys_map entry for an unnamed branch), nr_rx_branch_test.cc (+2
  cases, 31 total), nr_pdsch_passive_decode.c (stop fabricating branch 0 on an unmapped view; HARQ
  lane pinned to its pre-P13a value). Both new gates falsified deliberately; both replay baselines
  re-proven unchanged on binary 83922f65. Fix round 2 (2026-09-11, 1 Minor from that round's own diff): nr_rx_branch.h gains nr_rx_branch_counter_index(), nr_pdsch_passive_decode.c:1118 guarded, nr_rx_branch_test.cc +1 case (32 total), falsified; both replays re-proven unchanged on binary cb81d725.
Remaining limitation: see the report's honesty section -- neither replay can exercise
  cross-engine isolation end to end (one process per branch view, and the engines are never
  started in replay mode at all), so isolation is proven in-process by the parity test and the
  construction/derivation half is proven live by the supplementary runs. The inactive-branch DROP
  path is currently UNREACHABLE live by construction, because the only branch-tagging producer
  derives its tag from the same branch set (nr_pdsch_passive_branch_view_resolve() always lands on
  an active branch); it is a guard for the producers P10's continuation will add.
Next highest-value action: P10's continuation -- tag the remaining five CFR producers with their
  measuring branch, which is what turns the untagged-attribution warning off and makes G4 tests 2
  and 4 measurable on real data. Then P14 Stage B item 1 (engine channel count from the branch set
  instead of aoa_antennas), which this task unblocked.
Reviewer / accomplishment date if gate passed: n/a -- not a gate pass.
```


## Session — 2026-09-11: P10b branch-aware routing for the remaining CFR producers (Stage 3/4, AoA-removal Stage B item 2)

```text
Date/time (Europe/Zurich): 2026-09-11, night.
Task IDs / gate: P10 continuation (Stage 3) + docs/aoa_removal_audit.md Stage B items 1 and 2.
  NOT a gate pass: G3 and G4 both remain open.
Intended falsifiable claim: the remaining DL CFR producers can be made branch-aware -- one
  single-antenna submission per active receive branch, reading that branch's own physical channel
  and routed to that branch's own SensingEngine -- while the <= 1-active-branch path (today's
  default, and the live co-located-array AoA path) stays byte-identical.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing, parent
  875ae85048f80ed428f318b5d504a2a28395324a; before: only `?? tests/passive_rx/aoa_track_dl.conf`
  (another session's, untouched, still untracked after); manifest
  tests/passive_rx/baselines/manifest_20260911_875ae85-dirty.json (+ _tracked.patch).
Files modified / added / removed:
  MOD  openair1/PHY/NR_UE_ISAC/nr_isac.h          nr_isac_submit_plan_t + nr_isac_submit_plan()
  MOD  openair1/PHY/NR_UE_ISAC/pipeline_types.h   build_submit_plan() (the testable logic)
  MOD  openair1/PHY/NR_UE_ISAC/nr_isac.cc         C wrapper + skip census; engine channel count
                                                  from the branch set; the AoA/multi-branch
                                                  explanation before the construction loop
  MOD  openair1/PHY/NR_UE_ISAC/nr_isac_stub.c     the matching ENABLE_ISAC_SENSING=OFF stub
  MOD  openair1/PHY/NR_UE_TRANSPORT/csi_rx.c                    CSI-RS tap -> plan
  MOD  openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c blind DM-RS tap -> plan
  MOD  openair1/SCHED_NR_UE/phy_procedures_nr_ue.c              SSB/PBCH tap -> plan
  MOD  openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc      test_branch_submit_plan()
  MOD  docs/aoa_removal_audit.md                  Stage B items 1+2 marked done; row 3 and row 4
                                                  rows updated; the "nr_isac_aoa_antennas() is
                                                  dead and goes with them" claim CORRECTED
  MOD  tests/passive_rx/baselines/fixtures.json   binary sha + P10b note
  MOD  adaptive_RX_pipeline_progress.md           this entry, the P10 row, the P13 row
  ADD  tests/passive_rx/baselines/manifest_20260911_875ae85-dirty.json (+ _tracked.patch)
  NOT touched: nr_pdsch_data_aided.c -- P10a + P07 already closed it (see below).
Executable / driver / config / geometry / acceptance hashes: nr-uesoftmodem sha256
  42fe305d68cb9b07e960296d9188f5781a424176f5660cbe7fa8faa2d36a378f (every number below is from
  THIS binary; liboai_usrpdevif.so not rebuilt). No radio touched -- the X410 is unreachable; every
  run below is the offline replay path, which prints its own "no radio opened" verdict.
Exact commands:
  pgrep -x nr-uesoftmodem                                   (empty before every build)
  cmake .                                                   rc=0
  make nr-uesoftmodem test_nr_isac_python_parity -j8        rc=0, 3 pre-existing warnings, no new
  ./test_nr_isac_python_parity                              "native sensing golden parity checks passed"
  gcc -fsyntax-only ... nr_isac_stub.c                      rc=0 (the OFF-build stub compiles)
  OUT=/tmp/p10b_replay2 bash tests/passive_rx/replay_branch_view.sh   dl_branch_view_replay: PASS
  <multi-branch supplementary runs, /tmp/p10b_multi>        rc=0
  bash tests/passive_rx/baseline_manifest.sh ; check_manifest.py [--fixtures]
Artifact paths (include raw logs and VOID attempts): /tmp/p10b_replay2 (five-view replay logs),
  /tmp/p10b_multi (supplementary multi-branch runs: receiver.conf = rx_branches "0,1";
  receiver.aoa.conf = the same plus aoa_enable; receiver.aoa1.conf = aoa_enable at ONE branch --
  all three are /tmp COPIES of the registered fixture's receiver.conf, the registered fixture and
  its conf were never modified), /tmp/p10b_build*.log.
Baseline and comparison definition: the two registered replay baselines (legacy 34/0/24 and the
  five-view 34/34/31/34/34) and the parity binary, all at parent 875ae85048.
Predeclared assertions / thresholds: both replay baselines byte-identical; parity suite green; the
  <= 1-branch plan equal field-for-field to the call the producers made before.
Observed result, with denominators:
  - Parity suite 19/19 (was 18/18): + test_branch_submit_plan().
  - replay_branch_view.sh: legacy/view0/view1/view2/view3 crc_ok 34/34/31/34/34 of 38,
    data_submits == own crc_ok on every view, "dl_branch_view_replay: PASS". UNCHANGED. The legacy
    row IS the legacy baseline: "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24".
  - Supplementary rx_branches="0,1": "... AoA=0 ... engines=2", BOTH derived report paths created
    (/tmp/p10b_multi/reports_b0.jsonl, reports_b1.jsonl), replay verdict unchanged. So the plan
    runs in the REAL binary on a two-branch set, not only in the unit test.
  - Supplementary rx_branches="0,1" + aoa_enable: the new pre-loop LOG_E fires, and the engine's
    OWN pre-existing guard then refuses ("AoA requires four channels, a rank-two array, and
    surveyed Tx/Rx"), so the combination is now HARD-REFUSED with an explanation, not silently
    bearingless. rx_branches="0" + aoa_enable still builds "AoA=1 ... engines=1" -- the co-located
    AoA path is intact.
  - Falsification (the new gate, rebuilt and re-run twice): forcing the <= 1-branch plan to name
    branch 0 instead of NR_ISAC_BRANCH_NONE -> "with no branch set the plan must be the legacy
    untagged submission, antennas unchanged", exit 1; forcing every branch entry to read antenna
    plane 0 -> "the second branch must read ITS physical channel, and be tagged by branch id not
    ordinal", exit 1. Both pass again on restore.
Status (PASS / FAIL / VOID / BLOCKED): delivered. P10 still IN_PROGRESS (two UL producers remain),
  P13/G4 still not passed, AoA-removal Stage B items 1+2 DONE.
Validity reasons and affected intervals: offline only. No radio. The X410 is unreachable.
Hypotheses supported / contradicted:
  SUPPORTED -- "AoA and multi-branch are mutually exclusive deployment modes". Investigated rather
  than assumed: the config parser (nr_isac.cc) parses aoa_enable/rx_array and rx_branches
  INDEPENDENTLY and rejects neither combination, so the brief's "pick the safer interpretation"
  fallback applied on the config evidence alone. The MECHANISM then settled it against coexistence:
  sensing_engine.cc's build_window() takes w.antennas = min(requested_antennas_, the MINIMUM
  available_antennas over the window's rows), and attach_aoa() only runs at dl_window.antennas == 4,
  so one 1-antenna branch row in the same engine as the 4-antenna AoA submission collapses the whole
  window to one antenna and silently disables AoA. Running both paths -- the literal "safer"
  reading -- is therefore the WORST option: it is the only one that loses AoA without saying so.
  Multi-branch mode submits per branch and does NOT also submit the AoA blob.
  CONTRADICTED -- docs/aoa_removal_audit.md's Stage B item 2 claim that "nr_isac_aoa_antennas() is
  dead and goes with them, in the same commit". It is not dead: it is what selects the four-element
  co-located extraction, the live AoA path that is still the ONLY angular input to the global ENU
  tracker. Removing it here would have dropped every AoA deployment to one antenna. Corrected in
  the audit doc: the accessor dies with Stage C item 4, not Stage B.
Retraction, if any: none.
Remaining limitation (what replay can and cannot prove for the NEW paths):
  - The replay fixture exercises the DL decode chain, so it proves the legacy path is unchanged and
    it proves engine CONSTRUCTION and per-branch report-path derivation on a two-branch set. It
    does NOT close a CPI (0 lines in either reports_b*.jsonl) and therefore cannot show a
    per-branch CFR row arriving in a per-branch DETECTOR -- the same limitation P13a recorded.
  - The three migrated producers' multi-branch submissions are therefore proven at the PLAN level
    (unit-tested, deliberately falsified) and at the ROUTING level (they go through exactly the
    nr_isac_submit_cfr_multi_branch() entry point P13a hardened -- no new routing code exists),
    not end to end on real multi-branch data. There is no fixture with more than one branch's IQ.
  - The "physical channel the producer cannot reach" skip is unit-tested only; it is currently
    unreachable live, because the antenna-count check in nr_isac_init() plus P03's 1:1 map make a
    phys >= nb_antennas_rx configuration hard to construct on this fixture.
  - The ENABLE_ISAC_SENSING=OFF stub was syntax-checked, not link-checked in a full OFF tree.
Next highest-value action: the two UL producers (nr_pusch_data_aided.c, nr_pusch_passive_decode.c)
  -- the last untagged submitters -- which turns off the untagged-attribution warning entirely and
  makes G4 tests 2 and 4 measurable. Then AoA-removal Stage B item 4 (lift rx_array_calibration out
  from behind aoa_enable into per-branch chain metadata), which is the next item that P13a/P10b
  actually unblocked. Items 3 and 5 of Stage B remain separate future work, as does Stage A item 2's
  comment rewrite.
Reviewer / accomplishment date if gate passed: n/a -- not a gate pass.
```

## Session — 2026-09-11: P10c UL CFR producer branch awareness (Stage 3, closes the producer list)

```text
Date/time (Europe/Zurich): 2026-09-11, night (host clock UTC).
Task IDs / gate: P10 continuation (Stage 3), UL side. NOT a gate pass: G3 and G4 remain open.
Intended falsifiable claim: the two UL CFR producers carry a REAL per-branch identity (their
  submissions are per-antenna, not the coherently combined decode), so they can be made branch-aware
  exactly like the DL taps, while the <= 1-active-branch path stays byte-identical.
Branch / full commit / dirty patch / untracked-file manifest: merge/adaptive-sensing, parent
  1c6885950c46c69724bc09eccef3c6936564f1d2; before and after: only
  `?? tests/passive_rx/aoa_track_dl.conf` (another session's, untouched); manifest
  tests/passive_rx/baselines/manifest_20260911_1c68859-dirty.json (+ _tracked.patch).
Files modified / added / removed:
  MOD  openair1/PHY/NR_UE_TRANSPORT/nr_pusch_data_aided.c       data-aided UL tap -> plan
  MOD  openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c   UL DM-RS tap -> plan
  MOD  openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc      test_ul_submit_plan()
  MOD  docs/aoa_removal_audit.md                                row 3 gains both UL producers;
                                                                new Stage B item 2b
  MOD  tests/passive_rx/baselines/fixtures.json                 binary sha + P10c note
  MOD  adaptive_RX_pipeline_progress.md                         this entry, the P10 row, exp table
  ADD  tests/passive_rx/baselines/manifest_20260911_1c68859-dirty.json (+ _tracked.patch)
Executable / driver / config / geometry / acceptance hashes:
  nr-uesoftmodem before (parent, rebuilt from stash) c668fdd1f9b965d087c933c1889dca25bc89f77dfcaae8ecbbe1249da272c99d
  nr-uesoftmodem after  be296771eb3ab81dda7610629b7782059aa89108ad1a845991a7c0417200793f
  (the after hash reproduced EXACTLY across two independent builds of the same tree)
  fixture /home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7 (registered), its receiver.conf
  unmodified; multi-branch confs are non-registered /tmp copies (P10b's).
Exact commands:
  pgrep -x nr-uesoftmodem                                     (empty before every build)
  cmake . && make nr-uesoftmodem test_nr_isac_python_parity -j8
  ./test_nr_isac_python_parity
  bash /tmp/p10c_ulcfg.sh  <log>                              (registered fixture conf)
  bash /tmp/p10c_ulcfg2.sh /tmp/p10b_multi/receiver.aoa1.conf <log>   (1 branch, engines built)
  bash /tmp/p10c_ulcfg2.sh /tmp/p10b_multi/receiver.conf      <log>   (2 branches)
  OUT=/tmp/p10c_bv_{before,after} bash tests/passive_rx/replay_branch_view.sh
Artifact paths: /tmp/p10c_ul_{before,after}.log, /tmp/p10c_ul_{1br,multi}_{before,after}.log,
  /tmp/p10c_bv_{before,after}/, /tmp/p10c_make{,2}.log, /tmp/p10c_make_before.log.
Baseline and comparison definition: the pre-change binary was REBUILT from `git stash` and every
  probe run on it first, so nothing is inherited. Comparison = the 24 `UL-CONFIG source=...` per-grant
  lines (exact diff), the `UL-CONFIG REPEATABLE` census line, the presence/absence of P13a's untagged
  fan-in warning, and the registered `replay_branch_view.sh` 5-view table.
Predeclared assertions / thresholds: (1) UL-CONFIG must be bit-identical before/after in every conf
  (BLOCKED otherwise, per the brief); (2) the DL 34/34/31/34/34 baseline must not move; (3) at 2
  active branches the untagged fan-in warning must DISAPPEAR (this is the only positive evidence
  that the change does anything).
Observed result, with denominators:
  - UL-CONFIG, registered fixture conf: crc=9/24 rejected=0 repeat_mismatches=0 before and after,
    24/24 per-grant lines byte-identical (diff empty). Same on the 1-branch conf and the 2-branch
    conf: 24/24 identical in each.
  - untagged fan-in warnings, 2-branch conf: 1 before, 0 after. 1-branch conf: 0 / 0 (one engine
    never warns, and the single-branch AoA submission is still 4 antennas, untagged, unchanged).
  - replay_branch_view: 34/34/31/34/34 of 38, script exit 0, before and after.
  - test_nr_isac_python_parity PASS; new case falsified deliberately (fails with "branches beyond the
    passive UL context allocated planes must be skipped, not sliced") and passes on restore.
Status (PASS / FAIL / VOID / BLOCKED): PASS (offline). No radio touched; X410 unreachable.
Validity reasons and affected intervals: replay-only. The registered fixture conf builds NO sensing
  engine ("hierarchical ENU tracking requires surveyed noncoincident Tx/Rx"), so on that conf the UL
  CFR block is reached but submits nothing -- which is why the 1-branch and 2-branch confs (which DO
  build engines, and on which ULCFRIDX fires) were run as well.
Hypotheses supported / contradicted:
  CONTRADICTED (in part): the brief's premise that the UL submission is a coherently combined
  measurement with no per-branch attribution. The COMBINING is in the decode; the SUBMISSION is
  per-antenna and both files already refused to combine before submitting.
  SUPPORTED: the brief's underlying caution -- splitting the UL DECODE per branch is a different
  task (P08) and was not attempted.
Retraction, if any: none.
Remaining limitation: no closed CPI in replay (nr_isac_start() is never reached), so a per-branch UL
  row has never been seen arriving in a per-branch DETECTOR; no fixture carries two branches' real
  IQ, so "the two branches' CFR differ" is still unmeasured; nothing on air.
Next highest-value action: P08 (branch-independent UL decode view) remains the open UL item; and
  AoA-removal Stage B items 3-5.
Reviewer / accomplishment date if gate passed: n/a (no gate claimed).
```

### The item-5 decision (drop vs attribute), and why it dissolved

The brief asked whether an untagged UL submission under multi-branch mode should keep landing on the
lowest active branch (with P13a's warning) or be DROPPED like the AoA/multi-branch combination.
Both options are bad for the same reason -- they assume the measurement cannot be attributed. It can:
the submitted rows are per-antenna, and plane `a` is physical receive channel `a`. So the decision
taken is the third one the brief explicitly left open ("if you find a real per-branch identity I
missed, use it"): tag each plane with its own branch. Neither arbitrary attribution nor data loss.

The residual case is a branch whose `physical_channel` is beyond what the passive UL context
allocated (`nant < 4`). That one IS dropped -- by the shared `nr_isac_submit_plan()`, counted in
`plan_skipped_branches` and logged once -- which is the pre-existing house rule (P10b), not a new
UL-specific policy. Slicing a plane the context never allocated would be an out-of-bounds read.

## Session — 2026-09-12: P13b three hardening fixes from prior review rounds (defect closure)

Not a plan task. Three independent, small, offline-only findings that this series' **own** fix/review
rounds surfaced and explicitly deferred, batched into one session:

| # | Origin | Finding |
|---|--------|---------|
| 1 | `task-P13a-report.md` (fix rounds 2-3) | Four separate instances of a raw `branch_id & (NR_RX_BRANCH_MAX-1)` mask aliasing the `NR_ISAC_BRANCH_NONE` sentinel onto real branch 3, each found only by manual review one round apart. That report flagged "a checked-in grep guard (style of `check_branch_hw_isolation.sh`) would enforce the helper — flagged, not done." |
| 2 | P10b (`nr_isac.cc`'s own comment, and `docs/aoa_removal_audit.md` Stage B item 2) | `aoa_enable` + several `rx_branches` was refused only at RUNTIME, by `SensingEngine`'s constructor, after the configuration had been half-applied. The comment in the code said making it a parse-time rejection "is an operator call ... deliberately left open". |
| 3 | P10b submission-plan wrapper | `if(written>0&&set)` gated BOTH the skip counter and the `LOG_E`, so `written==0` — every active branch naming an unreachable physical channel, i.e. total data loss for that producer — incremented nothing and logged nothing. |

```text
Date/time (Europe/Zurich): 2026-09-12, ~04:00-06:30 CEST (host clock UTC).
Task IDs / gate: none (hardening / defect closure on top of P10b, P13a, P10c). No gate claimed.
Intended falsifiable claim:
  (1) a raw branch-id mask re-introduced anywhere in openair1/PHY/NR_UE_TRANSPORT or
      openair1/PHY/NR_UE_ISAC fails a registered ctest, while the sanctioned helpers, the comments
      that quote the banned idiom, and the two bounded uses do NOT;
  (2) aoa_enable with >1 active branch is refused at CONFIG PARSE time with both field names in the
      message and sensing left OFF, while aoa_enable with ONE branch is byte-for-byte unchanged;
  (3) submit_plan_skipped(set, 0) with 2 active branches == 2 (pre-fix: 0).
Branch / commit: merge/adaptive-sensing, parent 99f94438708af52398177aae74d48fed03edcc0f.
  Untracked before AND after (another session's, untouched): tests/passive_rx/aoa_track_dl.conf.
Files modified / added:
  A tests/passive_rx/check_branch_id_masking.sh        (item 1, the guard + its --selftest)
  M CMakeLists.txt                                     (item 1, add_test x2)
  M openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c  (item 1, `branch-mask-ok` markers x2)
  M openair1/PHY/NR_UE_ISAC/pipeline_types.h           (items 2+3, two free predicates)
  M openair1/PHY/NR_UE_ISAC/nr_isac.cc                 (items 2+3, the two call sites)
  M openair1/PHY/NR_UE_ISAC/tests/python_parity_test.cc (items 2+3, two new cases)
  M adaptive_RX_pipeline_progress.md, tests/passive_rx/baselines/fixtures.json (+ new manifest)
Executable hash: nr-uesoftmodem sha256 ce11542f7f50b066812ef3c2f70f653efa5f03b2ba9383739468695d9648f6e9
Exact commands (all on sens6, tree /home/sens/NICOLA/adaptive-rx-sensing):
  bash tests/passive_rx/check_branch_id_masking.sh --selftest       -> PASS, 3/3 planted, 0 FP
  bash tests/passive_rx/check_branch_id_masking.sh                  -> OK, 116 source files clean
  cmake . && make -j8 nr-uesoftmodem; make -j8 <test targets>
  ctest -R "branch_id_masking|branch_hw_isolation|nr_rx_branch|nr_rx_span|python_parity" -> 8/8
  ./test_nr_isac_python_parity                                      -> passed (20 cases, was 18)
  bash tests/passive_rx/replay_branch_view.sh                       -> PASS
  bash /tmp/p13b_item2.sh   (three doctored copies of the P02 fixture conf through the replay binary)
  (cd /tmp/p10a_offbuild && make nr-uesoftmodem -j8)                -> rc=0
Artifact paths: /tmp/p13b_build2.log, /tmp/p13b_off.log, /tmp/p13b_falsify_build.log,
  /tmp/replay_branch_view.xPnFG6/, /tmp/p13b_item2.b39Q/ (conflict|legacy|branches .log + .conf).
Baseline and comparison definition: the parent commit's own replay numbers and parity suite.
Predeclared assertions / thresholds: legacy replay 34/0/24; branch views 34/34/31/34/34;
  test_nr_isac_python_parity passes; ENABLE_ISAC_SENSING=OFF links.
Observed result, with denominators:
  * legacy "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24"; views 0-3 = 34/31/34/34 of 38
    recs, data_submits == crc_ok in every view. UNCHANGED.
  * ctest 8/8; parity 20/20 cases.
  * FALSIFICATION for item 3 (run, not argued): with submit_plan_skipped() reverted to the pre-fix
    `written > 0 && ...` expression the suite FAILS with "python parity test failed: with no
    reachable antenna at all BOTH active branches must be counted as skipped" (rc=1); restored, rc=0.
  * Item 2 END TO END in the real binary, three doctored copies of the P02 fixture conf:
    - aoa_enable=1 + rx_branches="0,1": "SENSING: aoa_enable is set together with 2 active
      rx_branches ... sensing disabled" -- both field names present, refused at parse time, replay
      still PASS 34/0/24;
    - aoa_enable=1 alone (the LEGACY pin): falls through to the pre-existing next check verbatim
      ("aoa_enable requires a valid surveyed rank-two four-element array; sensing disabled") --
      the new check is invisible to the single-branch AoA deployment;
    - rx_branches="0,1" without aoa_enable: not refused, replay PASS.
  * Item 1: the guard found the two LIVE raw masks in nr_pdsch_passive_queue.c (:181, :353). Both
    are documented-bounded uses from P13a fix round 3 (they read the ENQUEUE-time branch_id, before
    the only writer that can set the sentinel), so each got an inline `branch-mask-ok` marker with
    its reason instead of a code change. Opt-out-with-a-reason was chosen over widening the pattern
    so every exception stays a reviewable line.
Status: PASS for all three items (offline evidence only; no radio).
Validity reasons and affected intervals: no capture was taken; nothing on air. The replay evidence
  is the P02 fixture replayed through the real binary, which opens no radio.
Hypotheses supported / contradicted:
  SUPPORTED: the P13a report's claim that a grep guard is feasible for this defect class -- it is,
    but ONLY with a naming heuristic (a purely syntactic pattern cannot separate `i_ssb & 3` from
    `branch_id & 3`), and the heuristic's two failure directions are written into the script header.
  CONTRADICTED (small, mine): my first cut of the guard flagged three COMMENT lines that quote the
    banned idiom to explain the P13a fix, and its own self-test fixture false-positived because a
    trailing comment on an unrelated mask contained the word "branch". Both are in the header now as
    stated limits rather than silently patched away.
Retraction, if any: none.
Remaining limitation:
  * Item 1 is textual: it cannot see a mask split across lines, hidden behind a macro, written as
    `& 0x3` or `% NR_RX_BRANCH_MAX`, or applied to a differently-named variable assigned from a
    branch id. It guards against recurrence of the exact idiom that recurred four times.
  * Item 2's parse-time check makes P10b's pre-loop warning unreachable from the config path; that
    block was replaced by a comment pointing at the new site. SensingEngine's constructor guard is
    KEPT (defence in depth, per this project's pattern) and is now the only backstop for any future
    path that sets aoa_enable without going through nr_isac_config().
  * Item 3 changes only the CENSUS. A producer whose branches are all unreachable still submits
    nothing -- that is correct (there is nothing to submit); it is now counted and logged once.
  * Pre-existing, NOT caused here and NOT fixed here: `make -j8` (all targets) fails on `rftest`
    (missing `forms.h`) and on `test_nr_ue_ra_procedures` (libMAC_UE_NR referencing
    `nr_pdcch_blind_*` symbols it does not link). Neither touches any file changed in this session.
Next highest-value action: unchanged -- P08 (branch-independent UL decode view) and AoA-removal
  Stage B items 3-5.
Reviewer / accomplishment date if gate passed: n/a (no gate claimed).
```

### Fix round 1 (2026-09-12, controller-ruled) — the guard's own %-form blind spot

Review: Approved, 0 Critical / 0 Important, 4 Minor; only MINOR 1 actioned (2-4 judged acceptable
as-is by the reviewer). The script's header LISTED `branch_id % NR_RX_BRANCH_MAX` as a known miss
while a LIVE instance of exactly that form sat in a scanned file (`nr_passive_harq_tag.h`) -- a
declared limitation that is currently reachable is a hole, not a limitation, and it weakened the
"OK -- no raw branch-id masks" line. Fixed by widening `MASK_PATTERN` with
`|%[[:space:]]*NR_RX_BRANCH_MAX` (deliberately keyed to that macro, not a bare `% n`: the same header
line carries a legitimate `harq_process_nbr % NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE`), removing `%`
from the KNOWN LIMIT list, and giving the live instance a `branch-mask-ok` exemption naming the
call-site guard that makes it safe -- re-traced, not inherited: `nr_pdsch_passive_harq_tag()` has one
non-test caller and P13a maps `NR_ISAC_BRANCH_NONE` to lane 0 on the line above it, so the reduction
is a defensive backstop the sentinel never reaches. Self-test extended on both sides (a planted
`%`-form that must be detected; an exempted `%` line and a `% 32` different-modulus line that must
not be). The exemption was proven load-bearing by deleting it and watching the scan fail.
Verification: `--selftest` 4/4 planted, 0 false positives; real scan OK over 116 files;
`make -j8 nr_rx_branch_test` rc=0 and ctest 3/3 (the header is C source and the edit splits an
expression across lines, so it was compiled rather than argued). No behaviour change: comments plus
a test-only script.
