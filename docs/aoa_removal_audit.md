# AoA removal audit and staged removal plan (P14, Stage 4)

Date: 2026-09-11 (Europe/Zurich). Branch `merge/adaptive-sensing`, audited at
`3241cca0ba956c2eecb45c6f2c6a6bdd51de34e1`. Offline only; no radio was touched.

Scope ruling this document implements: P14 in `adaptive_RX_pipeline.md` says "execute the removal
table in section 2.4". AoA is not dead code. It is a large, live, tested subsystem, and it is the
ONLY thing that currently gives the global ENU tracker an angular measurement, i.e. the only working
global-fusion path. Stage 5 (P17-P19, AoA-free bistatic fusion) does not exist yet. Deleting the
tracker's AoA dependency now would take global fusion to zero with no offline way to show the
replacement works, which is exactly what G4/G5 gating exists to prevent ("Fusion cannot begin
before this gate").

So P14 is executed as: complete audit, ordered plan, and the first removal that is provably safe
today.

## 0. Survey method

`grep -rIn -i "aoa|azimuth|rx_array|bearing"` over `openair1/`, `tests/`, `ci-scripts/`,
`CMakeLists.txt`, followed by reading every hit and resolving every symbol to its callers. 408
case-insensitive `aoa` hits in C/C++ under `openair1/`. Findings that correct the task brief:

* The file is `openair1/PHY/NR_UE_ISAC/aoa.cc` / `aoa.h`, NOT `isac_aoa.cc`. Several project
  documents (and a comment in `SIMULATION/TOOLS/sensing_channel.c:632`) still say `isac_aoa.cc`.
* The section 2.4 table names `AOA_ENABLE`/`AOA_UL_ENABLE`. Those are NOT stale, but they are not
  config keys either: they were ENVIRONMENT variables read by `environment_bool()` in
  `nr_isac.cc:165`, used at `nr_isac.cc:283` to override the real `[sensing]` keys `aoa_enable` /
  `aoa_ul_enable`. Both spellings existed at once. This is the item executed below.
* There is NO AoA code in any `.cu` file. The "CPU/CUDA synchronization paths" P14 asks to audit
  carry no AoA dependency: the only sync-side AoA symbol is `aoa_observed_mask()`
  (`sync_correction.cc:725`, declared `sync_correction.h:53`), a pure row-mask helper.
* No dead AoA code exists. Every public symbol in `aoa.h` has a production caller. Nothing can be
  removed on "it is unused" grounds.
* `tests/passive_rx/monitor/monitor.html` ALREADY renders a missing bearing as "no AoA"
  (`:442`, `:484`), so omitting the angular report fields later does not break the monitor.

## 1. Touchpoint audit, by section 2.4 row

Legend for the last column: **A** = safe now, **B** = needs P13 (one engine per branch) first,
**C** = needs Stage 5 (P17-P19) to exist first. Where in doubt the later stage is assigned.

### Row 1 - `aoa.cc/.h`, AoA attachment and steering
| file:line | what it does | breaks if removed now? | stage |
|---|---|---|---|
| `PHY/NR_UE_ISAC/aoa.cc` (whole, 45 aoa hits) | `admit_aoa_for_tracking` (:238), `combine_aoa_estimates` (:272), `isolate_target_response` (:382), `grid_free_upa_aoa` (:574), `attach_aoa` (:729) | YES - `test_aoa()` and `test_aoa_component_mixture_and_cross_leg_fusion()` in `tests/python_parity_test.cc` cover all five; `attach_aoa` is called from the engine | C |
| `PHY/NR_UE_ISAC/aoa.h` (:18,:23,:27,:44,:51,:57) | the public surface of the above | same | C |
| `CMakeLists.txt:1089` (`aoa.cc` in `NR_UE_ISAC_SRC`) | builds it | same | C |
| `PHY/NR_UE_ISAC/sensing_engine.cc:6,:852,:891` | `#include "aoa.h"`; `attach_aoa()` on the DL window and on the UL window | YES - removing the calls zeroes every `Detection::aoa`, which the tracker (row 6) reads | C |
| `PHY/NR_UE_ISAC/sync_correction.{h:53,cc:725}` `aoa_observed_mask()` | picks which slow-time rows AoA may use (DL only vs DL+UL) | YES - parity test `:183-188` | C (mechanically trivial, but it is AoA's input gate; remove with row 1) |
| `SIMULATION/TOOLS/sensing_channel.{c,h}` `sensing_channel_set_rx_array()` (`c:579`, `h:112`), `rx_array`/`rx_array_boresight_deg` sim keys (`c:726-728`) | the SIMULATOR side: steers each path's tap per rx element. Not receiver code | NO, but it is the only way to generate an AoA-bearing fixture | C, and arguably keep: it is test apparatus, not production |

### Row 2 - AoA configuration and `AOA_ENABLE`/`AOA_UL_ENABLE`
| file:line | what it does | breaks if removed now? | stage |
|---|---|---|---|
| `nr_isac.cc:283` (was) `environment_bool("AOA_ENABLE"/"AOA_UL_ENABLE", ...)` | undocumented env override of the conf keys | NO - nothing in the repository sets either variable (checked all `.sh/.py/.conf/.md/.c/.cc/.h`); the two globals were read only inside `nr_isac.cc` | **A - DONE, see section 3** |
| `nr_isac.h:43-44`, `nr_isac.cc:24-25`, `nr_isac_stub.c:7-8` `extern int AOA_ENABLE/AOA_UL_ENABLE` | process-wide duplicate of `PipelineConfig::aoa_enable` | NO - no external reader | **A - DONE, see section 3** |
| `nr_isac.cc:220-221` conf keys `aoa_enable`, `aoa_ul_enable` | the REAL switch | YES - eight `tests/passive_rx/ue.passive*.conf` set `aoa_enable = 1` | C |
| `nr_isac.cc:222-225` `rx_array`, `rx_array_boresight_deg`, `rx_array_broadside_enu`, `rx_array_calibration` | array manifold + per-chain calibration | partly - see row 10; the calibration tuple must survive | C (geometry) / refactor (calibration) |
| `nr_isac.cc:228-235` `aoa_max_manifold_residual`, `aoa_max_phase_fit_rms_rad`, `aoa_max_azimuth_stddev_deg`, `aoa_max_elevation_stddev_deg` | `AoaQualityPolicy` admission limits | YES - `sensing_engine.cc:178-187` validates them and throws | C |
| `sensing_engine.cc:176-187,:205-207` | rejects UL AoA without master AoA, invalid quality limits, AoA without four channels / rank-2 array / surveyed Tx-Rx | YES - these guards keep a misconfigured AoA build from starting | C |

### Row 3 - `nr_isac_aoa_antennas()`-controlled extraction
This is the row the plan wants replaced by branch-aware routing. All four producers ask the AoA
switch "how many antennas may I extract", which is exactly the coupling P10/P13 remove.
| file:line | what it does | breaks if removed now? | stage |
|---|---|---|---|
| `nr_isac.cc:341`, `nr_isac.h:113`, `nr_isac_stub.c:36` | the accessor itself (returns 4 when AoA is on, else 0) | YES for all four producers below | ~~B~~ **C** - corrected by P10b: this is the live co-located-array AoA path's antenna count, not a branch-routing artefact. It dies with row 1, in Stage C item 4 |
| `PHY/NR_UE_TRANSPORT/csi_rx.c:965` (+ comments :28,:942,:1155) | CSI-RS sensing tap antenna count | YES - falls to one antenna | **B DONE (P10b)** - branch-aware via `nr_isac_submit_plan()`; the AoA count is still what a single-branch receiver submits |
| `PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2229` (+ comments :95,:2220-2227) | blind-PDCCH DM-RS tap antenna count | YES | **B DONE (P10b)** - same plan; `g_cfr_submits` deliberately still counts CANDIDATES, not branches |
| `PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c:210` (+ comments :206,:216) | data-aided PDSCH tap antenna count | YES | **B DONE (P10a + P07)** - the single-branch view already gives it `nb_antennas_rx = 1` on the branch's own plane, and P10a tags it from `nr_pdsch_passive_view_branch()`. No plan needed |
| `SCHED_NR_UE/phy_procedures_nr_ue.c:1425-1426` | attached-UE PDSCH tap antenna count, clamped to `fp->nb_antennas_rx` | YES | **B DONE (P10b)** - the SSB/PBCH tap, same plan |
| `PHY/NR_UE_TRANSPORT/nr_pusch_data_aided.c:178` | passive UL data-aided tap antenna count (the passive gNB context's own allocated plane count, not `nb_antennas_rx`) | YES | **B DONE (P10c)** - same `nr_isac_submit_plan()`. H = Y_a/X is formed PER ANTENNA against one common reconstructed X, so each plane is a real per-branch measurement |
| `PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:892` | passive UL DM-RS tap antenna count | YES | **B DONE (P10c)** - same plan; the per-antenna DM-RS estimates were already kept SEPARATE (the comment at `:873` says why), which is exactly what makes them sliceable per branch |
| comment-only references: `nr_pdsch_passive_decode.c:1281,:2091`, `nr_pusch_passive_decode.c:91`, `nr_pusch_data_aided.h:45`, `nr_pdsch_passive_queue.c:99` | explain why the per-antenna buffers are heap, not thread-local | NO - prose only. The heap decision must SURVIVE (it fixed a real AVX alignment fault); only the wording needs updating | B (wording) |

`PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:467` (`prs_meas[rxAnt]->dl_aoa = rsc_id`) is
upstream OAI PRS reporting, unrelated to this subsystem. Out of scope. Do not touch.

### Row 4 - Rank-two compact-array geometry, broadside, angular calibration
| file:line | what it does | stage |
|---|---|---|
| `nr_isac.cc` `parse_array()` (used at :291) with `p_array`/`p_rotation`/`p_broadside` | parses and validates the four-element rank-two array and its broadside | C |
| `nr_isac.cc:290-299` | the whole "AoA needs a valid array or sensing is disabled" block, and `aoa_antennas = 4` | C |
| ~~`nr_isac.cc:317`~~ (stale citation; P13a moved this into the per-branch construction loop) `SensingEngine(..., 275, branch_antennas)` | the engine's channel count | **B DONE (P10b)**: derived from `branch_active_count()` when >1 branch, still `aoa_antennas` at one branch |
| `pipeline_types.h` array geometry struct and its `:231` comment | representation | C |

### Row 5 - DL/UL bearing confirmation, bearing-only EKF updates
| file:line | what it does | stage |
|---|---|---|
| `cross_leg_fusion.{h:25,:35, cc:16,:27,:46,:63-99}` | pairs DL detections with UL AoA measurements; `auxiliary_ul_aoa`, covariance floor, angle-pair cost | C |
| `sensing_engine.cc:862,:891,:911,:919-921` | builds the UL AoA config, attaches AoA to the UL window, feeds `fuse_cross_leg` and the hierarchical tracker's auxiliary AoA list | C |
| `enu_tracker.h:80-81` `angle_innovation()` / `update_angles()`, `:94-96` counters, `:120-124` state | the bearing-only EKF update | C - **explicitly out of scope for this task by controller ruling** |
| `enu_tracker.cc` (38 aoa hits) | implementation of the above | C - do not touch |

### Row 6 - AoA-dependent hierarchical/ENU tracker
| file:line | what it does | stage |
|---|---|---|
| `hierarchical_tracker.h:23-24,:41,:66` | `aoa_temporal_sigma`, `minimum_aoa_temporal_stddev_deg`, the `auxiliary_aoa_measurements` argument, `valid_aoa_cache_` | C |
| `hierarchical_tracker.cc:26-27,:37,:57,:69-70,:105-106,:122-123,:145-148,:179-193,:210-211` | AoA-gated global BIRTH (`:145` - a global track is born only from a detection with a valid AoA), the AoA cache, auxiliary AoA updates | C. This is the single dependency that makes the subsystem unremovable today: with no AoA and no Stage-5 model, `:145` births nothing and global fusion output is zero |

### Row 7 - Passive cross-channel MRC, best-branch selection, rescue retries
Not an AoA dependency. No `aoa` hit anywhere in the combining path. Owned by P06/P07/P13 (the
branch-view mechanism `nr_pdsch_passive_view_branch()` is DISTINCT from AoA and must not be
confused with it). Listed here only to record that the audit checked and found no coupling.
Stage B, owned by P13, not by this row.

### Row 8 - Shared decode success used for all antennas' data CFR
`nr_pdsch_data_aided.c` reconstructs X once from the single accepted decode and measures
H = Y/X on every antenna (`:208-216`, `:290-305`). The coupling is real but it is a
BRANCH-independence issue, not an AoA one; P13 owns it. Stage B.

**RE-AUDITED 2026-09-12 (G1P3audit) against P06a/P07/P10a as landed: for the PASSIVE QUEUE path in
multi-branch mode this row is CLOSED, and it was closed by the branch VIEW, not by the CFR ABI.**
Traced end to end rather than re-asserted:
* `nr_pdsch_passive_queue.c:201-203` resolves a branch and arms the single-antenna view for the
  WHOLE chain; `nr_pdsch_passive_decode.c:1034-1036` sets `nb_antennas_rx = 1` and points
  `rxdata[0]` at that branch's own physical plane.
* The FEP loop is bounded by that same `fp->nb_antennas_rx` (`nr_pdsch_passive_decode.c:1263-1284`),
  so only plane 0 of the consumer's `rxdataF` scratch is written, and it holds the BRANCH's Y.
* The same `vue` and the same `rxdataF` are handed to the submit
  (`nr_pdsch_passive_queue.c:251-252`), where `isac_nof_ant` clamps to `fp->nb_antennas_rx == 1`
  (`nr_pdsch_data_aided.c:210-213`) -- so the ":206-216 loop over every antenna" this row names
  executes exactly once, on the branch's own plane.
* X is re-encoded from `dec.tb`, the TB THIS branch's own view CRC-accepted
  (`nr_pdsch_passive_queue.c:234-252`), and `nvar` is that view's own estimate.
* Identity is carried by `nr_pdsch_passive_view_branch()` (`nr_pdsch_passive_decode.c:921-927`,
  gated on `view_active()` so a legacy row is never labelled branch 0) into
  `nr_isac_submit_cfr_multi_branch()` at all four submissions
  (`nr_pdsch_data_aided.c:356`, `:391`, `:419`, `:428`, `:434`).
* The replay harness already asserts the consequence: `tests/passive_rx/replay_branch_view.sh`
  criterion (iii), `data_submits == that view's own crc_ok` (34/31/34/34 of 38).

**Residual, precisely scoped -- this row is closed, three adjacent things are not:**
1. **The GRANT is still one branch's discovery.** `nr_pdsch_passive_queue_enqueue_fanout()`
   (`nr_pdsch_passive_queue.c:364-381`) copies ONE job from the single shared blind PDCCH monitor to
   every active branch, so `job.dlsch_pdu` / `job.freq_alloc` / `job.rnti` -- which parameterise the
   reconstruction of X -- are another branch's decode. The TB, the Y and the tag are per-branch; the
   allocation metadata is not. That is P06's own open core (branch-OWNED discovery state), not this
   row, and it is the only thing still standing between "per-branch decode provenance" and
   "per-branch provenance end to end".
2. **Single-branch / AoA mode keeps the shared-X-across-antennas form, by design.** With
   `n_active <= 1` no view is armed, `isac_nof_ant` resolves to `nr_isac_aoa_antennas()` and the
   `:290-305` loop measures `Y_a/X` on all four elements against one X. That is the co-located-array
   deployment model and is CORRECT there (it is what carries the inter-element phase); it is not a
   branch-independence defect, and Stage C item 4 owns its eventual removal.
3. **No live evidence.** Everything above is source-traced plus the P02 replay fixture, which runs
   ONE VIEW PER PROCESS. No capture has ever run two branches concurrently (X410 unreachable since
   2026-09-11), so the *concurrency* half -- that two branches' views in one process do not disturb
   each other -- rests on the thread-local construction of `t_view_ue`/`t_view_phys`/`t_view_branch`
   (`nr_pdsch_passive_decode.c:910-913`) and on P09's `harq_unique_pid` striding, by inspection.

### Row 9 - AoA report fields, monitor views, configs, tests, benchmarks
| file:line | what it does | breaks if removed now? | stage |
|---|---|---|---|
| `report_writer.cc:143-153` `aoa_policy`, `aoa_quality_policy` blocks | config echo | no in-tree consumer found | B (schema version, P16) |
| `report_writer.cc:200-215` per-detection `azimuth_deg`/`elevation_deg` and the full `aoa` object | the measurement | `monitor.html` handles null already; `tests/sensing_sim/score_azimuth.py` and friends read it | B (P16 versioning) |
| `report_writer.cc:259-260` UL detection `aoa.valid/reason`; `:283-284` `temporal_aoa_rejections`, `auxiliary_aoa_updates` | diagnostics | no in-tree consumer | B |
| `pipeline_types.h:131` `AoaEstimate`, `:144` `component_aoa_count`, `:162` `Detection::aoa`, `:203-204` tracker counters, `:241-242` `AoaQualityPolicy`, `:283-288` config fields | the data model everything above shares | YES, everywhere | C |
| `tests/python_parity_test.cc` (61 hits: `:129-143`, `:183-188`, `:216-261`, `test_aoa`, `test_aoa_component_mixture_and_cross_leg_fusion`) | the AoA contract tests | they must be deleted IN THE SAME COMMIT as the code they cover, never before | C |
| `tests/sensing_sim/*` (`score_azimuth.py`, `make_scenes.py`, `_run_aoa_*.sh`, `report_aoa_factorial.py`, `phase_c_budget.py`, `analyze_separability.py`, `eval_harmonic_pos.py`, `score_ghost_census.py`, the `_scene_*_rx*.conf` set) and `tests/passive_rx/*` (`ue.passive*.conf` `aoa_enable`/`rx_array`, `run_passive_rx.sh`, `_run_pdsch_data_ab.sh`, `_run_bw_fleet.sh`, `_run_100mhz_vs_activeue.sh`, `monitor/`) | the AoA experiment apparatus | no test in the CMake `tests` target depends on them | B for the confs (they must stop setting `aoa_enable` before the key is rejected); the analysis scripts are historical evidence - archive, do not delete |
| `tests/nr-cu-nrppa/nr-cu-nrppa.c` | NRPPa "dl_aoa" positioning, upstream OAI | out of scope | - |

### Row 10 - Array calibration structure (refactor, not delete)
`nr_isac.cc:225-226,:295-297` `rx_array_calibration` = four `observed,gain,phase_rad,delay_ns`
tuples, parsed by `parse_array_calibration()`, stored in `PipelineConfig::array_calibration`,
echoed in the startup log (`:321`). `pipeline_types.h:231` documents that values are relative to
element zero. Per section 2.4 this must SURVIVE the removal as physical-channel mapping plus
cable/filter delay and gain, decoupled from the array manifold. It is currently gated behind
`aoa_enable` (`nr_isac.cc:290`), which is the part that must change: calibration is a property of
the receive chain and must apply with AoA off. **Stage B** (it belongs with P13's per-branch
engines, which is where per-chain metadata acquires an owner), and it is a REFACTOR - a blanket
delete here would lose real hardware calibration.

## 2. Staged removal order

### Stage A - safe now (2 items, 1 executed)
1. **DONE (this commit): remove the `AOA_ENABLE`/`AOA_UL_ENABLE` environment override and the two
   exported globals; reject the obsolete keys loudly.** See section 3.
2. **NOT done, deliberately: update the five comment-only `aoa` references** in
   `nr_pdsch_passive_decode.c:1281,:2091`, `nr_pusch_passive_decode.c:91`,
   `nr_pusch_data_aided.h:45`, `nr_pdsch_passive_queue.c:99`, plus the `isac_aoa.cc` misnomer in
   `sensing_channel.c:632`. These are prose and cost nothing, but the heap-not-thread-local
   rationale they record is still load-bearing, and its wording should be rewritten once P13 names
   the real reason (per-branch buffers) rather than being rewritten twice. Sequenced into Stage B
   alongside row 3.

Nothing else qualified. The audit specifically looked for, and did NOT find: dead AoA code, a
duplicate implementation, an AoA-only CMake target, an AoA-only CUDA path, or a report field with
no consumer that is not also part of the schema version P16 owns.

### Stage B - needs P13 (one engine per branch) first (5 items, in order)
1. **DONE (P10b).** `nr_isac.cc` engine channel count from the branch set instead of `aoa_antennas`.
   The audit's `:317` citation predated P13a and was stale: P13a had already moved the construction
   into a per-branch loop, so what remained was the ARGUMENT, not the site. P10b makes it
   `branch_active_count(branches) > 1 ? 1 : (aoa_antennas ? aoa_antennas : 1)` inside that loop -- a
   physically separated branch is one antenna by P03's 1:1 branch:physical map, and asking for four
   would reserve four planes per row in EVERY engine (4x the CFR working set, in a pipeline with a
   documented `std::bad_alloc` history) to hold three planes no producer can fill. With one active
   branch the expression is literally the previous one, so the AoA path is untouched.
2. **DONE (P10b) for the routing; `nr_isac_aoa_antennas()` deliberately NOT removed.** Row 3's four
   producers now derive their submissions from `nr_isac_submit_plan()`
   (`nr_isac.h`, `pipeline_types.h`'s `build_submit_plan()`): one untagged multi-antenna submission
   at <= 1 active branch -- byte-identical to the previous call -- and one SINGLE-ANTENNA submission
   per active branch otherwise, each reading that branch's own `physical_channel` (a pointer offset
   into the same packed antenna-major buffer, never a second pack) and tagged with its `branch_id`
   so P13a's router delivers it to that branch's own engine.
   `nr_pdsch_data_aided.c` needed NO change here: P10a already tags it via
   `nr_pdsch_passive_view_branch()`, and P07's single-branch view sets `nb_antennas_rx = 1` with
   `rxdata` pointing at the branch's own plane, so its `nr_isac_aoa_antennas()` clamp already
   resolves to that one physical channel. Its `:210` row is closed by the view mechanism, not by the
   plan.
   **This document's claim that "`nr_isac_aoa_antennas()` is dead and goes with them, in the same
   commit" is WRONG and is corrected here.** The accessor is what still selects the four-element
   co-located-array extraction, which is the live, validated AoA path and the ONLY thing feeding the
   global ENU tracker an angle. It is dead only once AoA's compact-array code itself is deleted --
   **Stage C item 4**, not Stage B. Removing it here would silently drop every AoA deployment to one
   antenna, which is exactly the regression this document's own Stage C rationale forbids.
   **Real gap found while doing this, NOT closed**: the configuration surface does not reject
   `aoa_enable` together with `rx_branches` naming more than one branch. They are mutually exclusive
   deployment models (co-located array on one branch vs physically separated single-antenna
   receivers) and they cannot be served by one engine -- `sensing_engine.cc`'s `build_window()` takes
   the MINIMUM available antenna count across rows, so a mixture collapses the window to one antenna
   and silently disables AoA. P10b therefore makes multi-branch mode SKIP the AoA submission (it does
   not run both paths) and `nr_isac_init()` now logs `LOG_E` naming the combination. Turning that
   into a hard rejection is an operator call -- it would refuse to start a receiver that starts
   today -- and is left open.
   Stage A item 2's comment rewrite still rides along here and is still NOT done.
2b. **DONE (P10c) - the two UL CFR producers.** `nr_pusch_data_aided.c` and
   `nr_pusch_passive_decode.c` were the last producers still calling the legacy untagged
   `nr_isac_submit_cfr_multi()`. They now use the same `nr_isac_submit_plan()`.
   **They are NOT a special case, and the "coherently combined, therefore not per-branch
   attributable" argument does not apply to what they submit** - it applies to the DECODE.
   The decode (`nr_rx_pusch_group_tp()` MRC -> LLRs -> LDPC) does combine up to
   `PASSIVE_UL_MAX_ANT` antennas, and that combination produces the shared REFERENCE (the
   re-encoded X, or the estimator state); what reaches the sensing engine is formed PER ANTENNA
   from it -- `H = Y_a/X` at `nr_pusch_data_aided.c:222`, `ul_ch_estimates[0*num_sp + a]` at
   `nr_pusch_passive_decode.c:969` -- into an antenna-major buffer whose plane `a` is UE physical
   receive channel `a` (the UL FEP writes `rxdataF[a] <- ue->common_vars.rxdata[a]`). Both files
   already refused to combine before submitting, for the AoA inter-element phase. So a UL branch
   submission is as honest as a DL one, and the multi-branch cases of "attribute it to the
   lowest branch" versus "drop it" both become moot: it is attributed CORRECTLY.
   `available_antennas` is the passive gNB context's own allocated plane count
   (`min(nb_antennas_rx, PASSIVE_UL_MAX_ANT)`, fixed at `passive_gnb_prepare()` time), so a branch
   mapped past it is skipped and counted rather than slicing a plane that was never allocated.
   This also closes P10b's review Minor finding 3 (under multi-branch mode the UL submission was
   silently truncated to plane 0 by `requested_antennas_ = 1`).
   **P08 is UNAFFECTED and still fully open**: splitting the UL DECODE per branch (a
   single-antenna UL decode view, the analogue of P07's `t_view_branch`) is a different, larger
   task with a real SNR-regression risk, and nothing here attempts it.
3. **DONE (P07 + P06a + P10a), re-audited and confirmed 2026-09-12 (G1P3audit).** Row 8 -
   per-branch decode provenance for the data-aided CFR. The earlier text here ("NOT done -- P10b
   touched the submission plan, not the shared-decode coupling") was measuring the wrong mechanism:
   the shared-decode coupling is broken by P07's single-antenna VIEW, not by the submission plan, and
   `nr_pdsch_data_aided.c` is therefore never reached with more than one antenna in multi-branch
   mode. Full file:line trace in Row 8 above. In multi-branch mode a data-aided CFR row carries that
   branch's own Y (its own physical plane), its own X (the TB its own view CRC-accepted), its own
   `nvar`, and its own id -- asserted offline by `replay_branch_view.sh`'s criterion (iii).
   NOT closed by this, and deliberately left in their own rows: the GRANT that parameterises X is
   still fanned out from one shared PDCCH monitor (**P06's open core**); single-branch/AoA mode
   retains the shared-X-across-antennas form by design (**Stage C item 4**); and nothing here has
   live multi-branch evidence (X410 unreachable).
4. Row 10 - lift `rx_array_calibration` out from behind `aoa_enable` into per-branch chain
   metadata. REFACTOR. Must precede any removal of the `rx_array*` keys, or the calibration is
   lost together with the geometry.
5. Row 9's report fields - P16 schema version, angular values OMITTED rather than zeroed.
   Requires the `tests/passive_rx/ue.passive*.conf` set to stop asserting `aoa_enable` first.
   `monitor.html` already tolerates a null bearing, so no consumer change is forced.

### Stage C - needs Stage 5 (P17-P19) to exist (4 items, in order)
1. P17/P18 land the AoA-free bistatic measurement model and global association, with their own
   offline tests, ALONGSIDE the existing AoA path. Nothing is deleted in this step.
2. Switch `hierarchical_tracker.cc:145` global birth and `:179-193` auxiliary updates onto the new
   model; keep the AoA path selectable until the new one is shown equivalent offline.
3. Delete row 5 and row 6's AoA members (`enu_tracker`'s angle update, `cross_leg_fusion`,
   `hierarchical_tracker`'s AoA cache and config) together with the parity-test cases that cover
   them, in one commit.
4. Delete `aoa.cc/.h`, `aoa_observed_mask()`, the `aoa_*` and `rx_array*` config keys (with a clear
   rejection message, mirroring section 3), and the `AoaEstimate`/`AoaQualityPolicy` types from
   `pipeline_types.h`. `CMakeLists.txt:1089` drops with the file.

Why nothing in Stage C can be pulled forward: `hierarchical_tracker.cc:145` births a global track
only from a detection carrying a valid AoA. Remove the angle before P17-P19 exist and the global
tracker emits nothing - a measured, live-validated capability replaced by zero, with no offline
harness able to show the replacement is equivalent. That is precisely the regression G4/G5 gating
exists to prevent.

## 3. What was executed (Stage A item 1)

`AOA_ENABLE` / `AOA_UL_ENABLE` were environment variables read by `environment_bool()`
(`nr_isac.cc:165`) at `nr_isac.cc:283`, silently overriding the `[sensing] aoa_enable` /
`aoa_ul_enable` configuration keys, and stored in two process-wide `extern int` globals exported
from `nr_isac.h`.

Removed:
* `nr_isac.cc` - the `environment_bool()` helper, both globals, and the override. `PipelineConfig`
  is now the single source of truth: `pipeline.aoa_enable` / `pipeline.aoa_ul_enable` are set
  straight from the parsed keys, the two AoA failure paths clear those fields instead of the
  globals, and `nr_isac_aoa_antennas()` plus the startup log read them.
* `nr_isac.h`, `nr_isac_stub.c` - the exported globals.

Added (the migration half of section 2.4 row 2, "reject obsolete keys clearly"):
* `openair1/PHY/NR_UE_ISAC/nr_isac_env.c` - `nr_isac_obsolete_env_keys()`, which counts the
  obsolete variables still present in the environment. Deliberately its own translation unit,
  `getenv()` only: it is compiled into BOTH the real pipeline and the `ENABLE_ISAC_SENSING=OFF`
  stub build (one definition, no stub duplicate), and the offline parity test links it without
  dragging in the configuration module. (Defining it inside `nr_isac.cc` was tried first and fails
  to link the test - referencing it pulls `nr_isac.o` in, which needs `config_get`, `uniqCfg`,
  `exit_function` and the branch-set parser.)
* `nr_isac.cc` - one `LOG_E` naming both keys when any is present, immediately after `config_get()`,
  so a stale launcher is told rather than silently ignored.
* `tests/python_parity_test.cc::test_obsolete_aoa_env_rejected()` - clean environment reports 0;
  `AOA_ENABLE` reports 1; adding `AOA_UL_ENABLE=0` reports 2 (present, not truthy - the point is
  that presence is rejected and the value is irrelevant); unsetting returns to 0.

Why this is safe, measured rather than assumed:
* No `.sh`, `.py`, `.conf`, `.md`, `.c`, `.cc` or `.h` file in the repository sets either variable.
* The globals had no reader outside `nr_isac.cc`. The two mentions in the parity test
  (`:184`, `:186`) are assertion MESSAGE strings for `aoa_observed_mask()`, not symbol uses.
* Behaviour with a conf that sets `aoa_enable = 1` is unchanged: the parsed key was already the
  fallback value the override defaulted to.
* Nothing in `aoa.cc`, `hierarchical_tracker.cc`, `enu_tracker.cc`, `cross_leg_fusion.cc`, the
  report schema or any CFR producer was touched.
