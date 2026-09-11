# Passive-RX process-wide globals audit (P03)

Plan: `adaptive_RX_pipeline.md` (`adaptive-rx-sensing`, `merge/adaptive-sensing`, inspected at
`8847b6a92200ffe8b044bda13bf83be56e0ee7ab`). Task: P03 (Stage 1, `adaptive_RX_pipeline.md` sec 4).

Method: `grep -n "^static [^(]*;\|^[a-zA-Z_].* g_\|__thread\|_Atomic\|atomic_" <file>` per file
listed in the P03 brief, plus manual reads of every hit's surrounding context (and, where the
column-0 grep missed function-local `static`/atomic declarations relevant to sync/timing state,
a follow-up `grep -n "static "` pass) to confirm what each symbol actually holds and who reads/
writes it. Every entry below was found this way -- none are from memory or a prior handover doc.
Line numbers are as of the commit above; re-grep before acting on this against a later commit.

## Classification key

- **(H) hardware-owner** -- belongs to `AcquisitionOwner`: UHD device state, the common sample
  counter/RF continuity, retune/restart, or anything that moves the shared hardware frequency.
- **(B) must become per-branch** -- synchronization/CFO/timing state, PDCCH/PUSCH discovery
  (learned RNTIs, candidate persistence, energy/noise gates, discovered layouts), DL/UL passive
  queues and decode contexts, the sensing engine/pipeline singleton.
- **(S) shareable immutable** -- tables/LUTs, or config resolved once and read-only afterward.
- **(W) worker/TLS scratch** -- needs snapshot/clear at job boundaries once a worker can process
  more than one branch's jobs; not itself branch-identity-carrying state.

Every classification below is my own read of the code against `adaptive_RX_pipeline.md` sec 3.1's
definitions, not a claim the original authors intended this taxonomy.

---

## `executables/nr-ue.c` (2137 lines)

**File-scope, always compiled:**

| Line(s) | Symbol | Class | Why |
|---|---|---|---|
| 100 | `extern _Atomic int nr_ue_rf_signal_absent;` (defined `openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:18`) | H | Freezes the shared timing integrator during a stream outage; the integrator it freezes (`max_pos_acc`) drives `shiftForNextFrame`, which per P05 must not move the common hardware frequency/sample origin without going through AcquisitionOwner. |
| 115-116 | `_Atomic long nr_ue_diag_producer_absolute_slot`, `nr_ue_diag_producer_wall_ns` | B | Diagnostic producer/consumer lag between the RT read loop (producer) and the async `dl_actors` decode workers (consumer, `phy_procedures_nr_ue.c`'s `nr_process_pbch_symbol()`). Once there are 4 independent read loops, each needs its own producer timestamp -- today it is a single instance because there is a single loop. |
| 278-279 | `extern _Atomic int nr_ue_cfo_resync_request;`, `extern int nr_ue_cfo_resync_hz;` (defined `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:65-66`) | H | CFO trim loop request/value that ultimately retunes the shared RF chain -- explicitly the kind of "no branch-local correction may move the common hardware frequency" state P05 calls out. |
| 842-852 | `_Atomic long nr_ue_pending_rebase_delta`, `_Atomic int nr_ue_pending_rebase_valid`, `_Atomic int nr_ue_rebase_epoch`, `_Atomic long nr_ue_diag_rf_timestamp`, `_Atomic long nr_ue_diag_samples_consumed` | H | The literal "coarse timing rebase" mechanism: `nr_ue_rebase_epoch` is this codebase's existing analogue of the plan's schema "RF continuity epoch" (sec 3.2), applied "through the same discard path acquisition uses to move the stream origin" per the file's own comment at line ~833-841. |
| 878-879 | `static double g_census_pow; static long g_census_pow_n;` | B | Absolute RF power off the just-read time-domain slot buffer; diagnostic input to the RFSTALL watchdog (below), which is per-loop today. |
| 888, 898-900 | `static double g_census_pow_ant[CENSUS_MAX_ANT]`, `g_census_pw2_ant[CENSUS_MAX_ANT]`, `static uint64_t g_census_clip_ant[CENSUS_MAX_ANT]`, `g_census_pw2_n` | B | RAW per-ANTENNA receive power/clip census (`CENSUS_MAX_ANT`=4). Already antenna-indexed, structurally close to per-branch, but implemented as flat arrays with cross-antenna normalization ("printed... normalised to the strongest branch") -- a branch reading another's array element is exactly the kind of cross-branch leakage G2's tests are designed to catch once this is wired per-branch. |
| 905-906 | `static double (*g_pwr_ref_fn)(int); static int g_pwr_ref_looked_up;` | H | `dlsym`-resolved handle to the ONE physical USRP driver's power-reference function; correctly a single process-wide cache, not per-branch. |
| 926-931 | `static double g_ulprobe_pow[2][CENSUS_MAX_ANT]; static long g_ulprobe_n[2]; static int g_ulprobe_on; static long g_census_pow_ant_n, g_census_ssb_slots, g_census_slots;` | B | Same per-antenna census pattern as above, split by DL/UL slot type (ULPROBE diagnostic). |

**Function-local `static`/locals inside `UE_thread()` (the single, persistent RX+decode loop,
starts line 1000) and its helper `shared_sfn_absolute_slot()` (line 44, `#ifdef
ENABLE_SIONNA_RK_PLUGINS` only):**

| Line(s) | Symbol | Class | Why |
|---|---|---|---|
| 44-48 | `shared_sfn_absolute_slot()`'s `static pthread_mutex_t clock_mutex`, `static uint64_t newest_absolute_slot`, `static bool initialized` | B | Mutex-guarded process-wide SFN-to-absolute-slot reference (conditionally compiled, Sionna RT co-sim plugin only). Directly the kind of "synchronization... timing state" P03 asks to make per-branch, if this build config is ever exercised on this project. |
| 430 | `static unsigned int deadline_warning_rate_limit` | W | TX-deadline-miss log rate limiter; trivial, but needs its own instance per RX/TX loop. |
| 857 | `static int left = 200;` (inside `nr_ue_timing_mutation_log()`) | W | Log-line budget for the rebase-mutation logger; shared budget across whatever calls it today. |
| 1007 | `nr_rx_continuity_t rx_continuity` (local, not `static`, but the single instance of `UE_thread`'s only invocation is today's stand-in for one branch) | H (detector) / B (reaction) | `nr_rx_continuity_check()`/`_commit()` (`executables/nr_rx_continuity.h`) detect a genuine UHD-timestamp gap -- an AcquisitionOwner-level event, matching exactly what `nr_rx_branch_set_rf_discontinuity()` (this task's new `nr_rx_branch.c`) is for. But its CONSEQUENCE in this code -- `UE->is_synchronized=0`, `stream_status=STREAM_STATUS_UNSYNC`, `shiftForNextFrame=0`, clearing the pending rebase (lines 1483-1503) -- is branch-owned sync state today entangled with the same detection. This is the concrete instance of P03's "separate hardware ownership... from fields that currently couple receiver state to ... retune". |
| 1046 | `int shiftForNextFrame` (local) | B | Per-loop incremental timing correction (`UE->max_pos_acc * time_sync_I`); becomes per-branch once branches sync independently (P05). |
| 1158, 1228 | `static int tsync_cap`, `static int tsync_reset` | S | `getenv()`-cached feature-toggle constants, resolved once and read-only afterward; uniform across the process, not branch-specific. |
| 1346 | `static int s_map_done` | W | One-shot "print the TDD slot map" diagnostic guard. |
| 1461-1462 | `static long s_rxts_prev_consumed`, `static long s_rxts_discont_total` | B | RXDISCONT (RF discontinuity) detection/accounting for `UE_thread`'s own read loop -- the exact "sample-counter discontinuity" G1 test 4 targets. |
| 1491 | `static int s_disc_invalidate` | S | `getenv("ISAC_DISC_NO_RESYNC")`-cached toggle, resolved once. |
| 1694-1697 | `static int s_wd_on; static double s_ref; static int s_bad; static long s_fires;` | B | RFSTALL watchdog state (running-max reference power, consecutive-bad-window count) -- per-loop today, needs to be per-branch once there are 4 independent loops each capable of stalling independently. |
| 1947-1948 | `static int s_reinits; static int s_reinit_cap` | H | Gates **`nrue_ru_reinit()`** (device close/reopen/restart, see `nr-ue-ru.c` below) -- a literal hardware-restart trigger fired from inside the RX loop; squarely AcquisitionOwner's domain, and the plan explicitly calls out separating state that "couples receiver state to ... device restart" (P03 sentence 2). |

---

## `executables/nr-ue-ru.c` (636 lines)

| Line(s) | Symbol | Class | Why |
|---|---|---|---|
| 86 | `static int nrue_cell_count;` | B | Count of configured cells (one `NR_DL_FRAME_PARMS` per cell); cell/frame-parms context is explicitly ReceiverBranch-owned per plan sec 3.1 ("owns synchronization, cell context..."). |
| 87 | `static nrUE_cell_params_t *nrue_cells;` | B | Per-cell config array (band, numerology, N_RB_DL, SSB start...) -- cell context, branch-owned. |
| 88 | `static NR_DL_FRAME_PARMS *nrue_cell_fp;` | B | Per-cell `NR_DL_FRAME_PARMS`, the frame-parms struct `PHY_VARS_NR_UE` embeds. Reusing `PHY_VARS_NR_UE` per branch (as P03 suggests) means this array is naturally what becomes 4 branch-owned frame-parms instances -- but today it is a flat array indexed by `cell_id`, not owned by any branch object. |
| 90 | `static int nrue_ru_count;` | H | Count of configured RUs (radio units) -- one per physical antenna/USRP channel. |
| 91 | `static nrUE_RU_params_t *nrue_rus;` | H | Per-RU config (maps to `physical_channel`); AcquisitionOwner's domain. |
| 93 | `openair0_config_t openair0_cfg_g[MAX_CARDS] = {};` (non-static, external linkage) | H | The literal UHD device open() config, indexed by card/`ru_id` == physical channel. Read/written by `nrue_ru_reinit()` (device close/reopen), `nrue_ru_read()` (the read loop the plan explicitly names), and RX/TX gain/frequency setters (lines ~469-633). |
| 94 | `static openair0_device_t openair0_dev[MAX_CARDS] = {};` | H | The literal open UHD device handles, same indexing. |

`nrue_ru_read()` (line 498) and `nrue_ru_reinit()` (line 387) -- the two functions the plan
names verbatim ("nrue_ru_read, retune or device restart") -- both operate purely on
`openair0_dev[]`/`openair0_cfg_g[]` indexed by `UE->rf_map.card`; `PHY_VARS_NR_UE.rf_map`
(`openair1/PHY/defs_nr_UE.h:285`) is the field that couples a reused `PHY_VARS_NR_UE`-per-branch
struct to hardware ownership and is the concrete field P03 asks to separate out.

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (2519 lines, RT tap, compiled into `PHY_NR_UE`)

| Line(s) | Symbol(s) | Class | Why |
|---|---|---|---|
| 67 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B | Same symbol as `nr-ue.c:115`; read here to compute this consumer's own lag. |
| 131-132 | `g_occasions_run`, `g_candidates_run` | B | Per-occasion/candidate counters, no branch attribution today. |
| 137, 148 | `g_length_swept`, `g_length_found` | B | DCI-length-sweep discovery state ("discovered layouts"). |
| 151-153 | `g_pdsch_sweep_on`, `g_pdsch_configuration`, `g_dl_length_state` | B | PDSCH config-sweep discovery state. |
| 193 | `g_constdiag_left` | W | TEMPORARY diagnostic rate limiter (comment: "TEMPORARY, see CONSTDIAG below"). |
| 199-205 | `g_ul_sched`, `g_ul_crc_hit`, `g_ul_disc_call`, `g_ul00_accepts`, `g_ul00_rejects`, `g_ul_accepts`, `g_ul_rejects` | B | UL discovery/accept counters. |
| 210 | `g_accepts` | B | Raw plausibility-accept counter (Step 1-4 of decode_and_extract). |
| 213-214 | `g_last_reject_reason`, `g_last_reject_rnti` | W | TEMPORARY diagnostic (comment: "root-cause pass"), last-value only. |
| 219-221 | `g_accepts_10`, `g_accepts_class[NR_BLIND_RNTI_CLASS_COUNT]`, `g_cfr_submits` | B | Per-class accept / final-submission counters. |
| 226 | `g_held_energy` | B | Energy/noise gate reject counter -- explicitly named in the brief ("energy/noise gates"). |
| 251-252 | `g_energy_floor`, `g_energy_nseen` | B | Adaptive energy-floor estimate feeding the same gate. |
| 268-271 | `g_held_persist`, `g_held_rnti_set`, `g_held_snr`, `g_held_mismatch` | B | Candidate-persistence / RNTI-set / SNR / mismatched-bits gate reject counters -- "candidate persistence" and "energy/noise gates" from the brief, verbatim. |
| 304-312 | `g_btim_ns[BTIM_N]`, `g_btim_n[]`, `g_btim_max[]`, `g_btim_hist[8]`, `g_btim_over_slot`, `g_btim_on` | W | Per-stage wall-time benchmark histogram; perf diagnostic, not correctness/branch-identity state. |
| 359-366 | `g_dec_try`, `g_dec_ok`, `g_dec_skip_rv`, `g_dec_over_cap` | B | Decode-attempt counters. |
| 389-390 | `_Atomic uint64_t g_al_accepts[4]`, `uint32_t g_al_rotate[4]` | B | Per-aggregation-level accept counts and rotating CCE scan start -- LEARNED discovery state that biases future scan order; matches "discovered layouts" directly. |
| 391-392 | `g_dec_unsup`, `g_data_submits` | B | Unsupported-scope / data-aided-submit counters. |
| 404-411 | `g_recent[NR_PDCCH_BLIND_PERSIST_MAX]` (struct array), `g_recent_head`, `g_recent_count` | B | RNTI-persistence ring buffer -- exactly "learned RNTIs, candidate persistence" from the brief. `rnti_persistence_check()` (line ~409) is the reader/writer. |
| 464-466 | `static nr_pdcch_dci_length_bank_t ul_lengths; static uint64_t ul_geometry; static pthread_mutex_t ul_length_lock;` | B | Mutex-protected UL DCI-length-bank discovery state -- "discovered layouts", mutable and lock-protected because it is genuinely written from more than one path today. |
| 2235-2237 | `static __thread float isac_h[...]; static __thread uint32_t isac_k[...], isac_l[...];` | W | CFR-submission scratch buffers, thread-local. **This is the concrete instance of the plan's own warning** ("Thread-local storage alone is not sufficient when a worker later processes another branch: snapshot/restore or clear branch-dependent state at job boundaries") -- nothing clears these between jobs from different branches today because there is only one branch. |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` (3320 lines, offline/discovery-helper lib)

| Line(s) | Symbol(s) | Class | Why |
|---|---|---|---|
| 86-88 | `static nr_pdcch_blind_monitor_cfg_t g_cfg; static int g_parsed; static int g_enabled;` | B | The `[sensing] pdcch_blind_monitor_*` config, parsed once, read by the RT tap via `nr_pdcch_blind_monitor_get_cfg()`. P06 names this file explicitly as a "configuration/discovery helper" to become branch-owned. |
| 259 | `static bool s_dedicated_found;` | B | Dedicated-CORESET discovery-complete latch. |
| 281-282 | `static uint16_t s_hit_count[NR_PDCCH_MAX_CANDIDATE_WINDOWS]; static int s_obs_calls;` | B | Per-6RB-window hit-count accumulator feeding CORESET-offset autodiscovery (`AUTODISCOVER_OBS_CALLS`-windowed convergence, see the file's own comment at ~line 290-303). |
| 308-313 | `static nr_pdcch_extent_cand_t s_ext_cand[NR_PDCCH_EXTENT_MAX_CAND]; static int s_ext_n, s_ext_idx; static bool s_ext_verified; static int s_ext_occ; static uint64_t s_ext_generation;` | B | PDCCH occasion-extent autodiscovery candidate catalog/state. |
| 319 | `static extent_evidence_t s_ext_evidence[NR_PDCCH_BLIND_MAX_UE];` | B | Per-UE-slot confirmation evidence for the extent search. |
| 1728, 1773, 1788, 2657, 2665, 2675, 2689 | `static const uint8_t/int32_t g_table_*[...]` (7 tables, incl. `g_ul_tda_j[6]`) | S | 3GPP-spec lookup tables (TDRA S/L tables, TDA-j table, etc.) -- `static const`, never written after initialization. Exactly the "tables, LUTs" the brief's (S) bucket names. |
| 3276-3278 | `static pthread_mutex_t common_facts_lock; static nr_pdcch_blind_common_config_t common_facts; static bool common_facts_valid;` | S (with a caveat) | SIB1-decoded "common facts" (PCI, DL/UL BWP, TDA tables), published via `nr_pdcch_blind_publish_common()` / read via `nr_pdcch_blind_get_common(pci,...)` (PCI-checked) / cleared via `nr_pdcch_blind_reset_common()`. All 4 branches observe the SAME cell, so a single SIB1 decode legitimately applies to all of them -- this is an example of the codebase ALREADY implementing the plan's "Worker pools and immutable code/tables may be shared" principle correctly, with a proper lock and a PCI-keyed validity check. Flagged (S) rather than (B), but worth re-confirming once branches can genuinely observe different PCIs (should not happen in this project's single-cell deployment, but the PCI check exists precisely to catch it if it ever does). |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.c` (250 lines)

| Line(s) | Symbol(s) | Class | Why |
|---|---|---|---|
| 48 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B | Same producer-lag symbol as above. |
| 54-58 | `static nr_pdcch_passive_job_t g_ring[NR_PDCCH_PASSIVE_QUEUE_MAX_DEPTH]; static int g_depth, g_head, g_tail, g_count;` | B | The passive PDCCH job ring buffer itself -- single instance today, needs to become per-branch (or branch-tagged) so P04's "bounded ownership... a slow branch must not read overwritten samples or hold all other branches indefinitely" is enforceable. |
| 60-61 | `static pthread_mutex_t g_lock; static pthread_cond_t g_cv;` | B | Guards the ring above. |
| 63-69 | `static _Atomic int g_running, g_stop; static _Atomic uint64_t g_queued, g_processed, g_dropped_full, g_dropped_stale, g_max_lag;` | B | Queue lifecycle + drop-policy counters -- directly what G1 test 3 ("stall one consumer... assert declared drops") exercises, currently with no branch attribution. |
| 71-73 | `static pthread_t g_threads[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS]; static int g_nthreads; static PHY_VARS_NR_UE *g_ue;` | B | Fixed consumer thread pool plus **a single global `PHY_VARS_NR_UE*`** -- the smoking gun for "one branch assumed": every consumer decodes against the SAME UE context regardless of which branch's samples it was handed. |
| 78 | `static consumer_arg_t g_args[NR_PDCCH_PASSIVE_QUEUE_MAX_CONSUMERS];` | B | Per-consumer-index argument slots (consumer index != branch identity today). |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (326 lines)

Structurally identical to the PDCCH queue above (ring + lock/cv + atomics + thread pool +
`g_ue`); all classified (B) for the same reasons, listed once rather than per-line:

| Line(s) | Symbol(s) | Class |
|---|---|---|
| 66 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B |
| 68-72 | `g_ring[]`, `g_depth`, `g_head`, `g_tail`, `g_count` | B |
| 74-75 | `g_lock`, `g_cv` | B |
| 77-82 | `g_queued`, `g_decoded`, `g_crc_ok`, `g_dropped_full`, `g_dropped_stale`, `g_max_lag` | B |
| 84-86 | `g_running`, `g_stop`, `g_nthreads` | B |
| 87-88 | `g_threads[]`, `g_ue` (single `PHY_VARS_NR_UE*`) | B |
| 95 | `static c16_t *g_rxdataF[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];` | W | Per-CONSUMER (not per-branch) frequency-domain scratch, **deliberately heap-allocated, not `__thread`** -- the file's own comment (line 91-94) cites `PASSIVE_RX_ONLY_HANDOVER.md`'s "shifted `__thread` layout producing an AVX alignment fault" as the reason. Already the right pattern per the plan's TLS warning; still needs branch-tagging once one consumer can service more than one branch back to back. |
| 100 | `g_args[]` | B |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (2117 lines)

This file is almost entirely per-branch-blind AGGREGATE diagnostic counters (LDPC/LLR/segment/
shape/subset/pipeline-timing histograms) -- listed grouped by purpose rather than one row per
counter (all confirmed by reading their usage, not just their declaration):

| Line(s) | Symbol(s) | Class | Why |
|---|---|---|---|
| 2 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B | Same producer-lag symbol. |
| 46 | `extern __thread uint32_t nr_dl_chest_nvar_ant[];` (defined elsewhere) | W | Per-antenna noise-variance estimate, already TLS. Antenna == future `physical_channel`, so this is directly branch-relevant, but the queue's thread pool (above) is generic, not antenna-pinned -- exactly the "worker later processes another branch" case the plan warns needs an explicit clear. |
| 76-81 | `kPdtimName[]` (S, `static const char*const`), `g_pdtim_ns/n/max[PDTIM_N]`, `g_pdtim_calls`, `g_pdtim_on` | S / W | Per-stage (fep/chest/alloc/demod/ldpc) wall-time histogram; perf diagnostic. |
| 149-152 | `static parmset_t g_parmset[PARMSET_MAX]; static int g_parmset_n; static uint64_t g_parmset_other; static pthread_mutex_t g_parmset_lock;` | B | Lock-protected census of distinct (mcs,tbl,Qm,bg,nl,cdm,...) grant "shapes" seen -- a diagnostic dedup table, but aggregated with no branch attribution. |
| 247-248 | `static _Atomic uint64_t g_branch_try[NR_DL_CHEST_MAX_ANT], g_branch_ok[NR_DL_CHEST_MAX_ANT];` | B | Per-antenna chest-retry try/ok counters -- note the pre-existing name "branch" here means "per-antenna chest retry branch", unrelated to this plan's `ReceiverBranch`, but conceptually the same axis (per-antenna = per future branch). |
| 262-395 | `g_llr_n/absum/zero/sat/clip8[2]`, `g_subset_try/ok[NR_PDSCH_SUBSET_N]`, `g_shape_n/tbs/rb/rv/G/K/F/C/Z[3]`, `g_rbhist[3][NR_PDSCH_RBHIST_BINS]`, `g_nsym[3][2]`, `g_llr_sgnsum/pos[2]`, `g_llr_posbit/nbit[2][2]`, `g_llr_tb[2]`, `g_ldpc_seg_fail/tb_fail/zero_tb/ok/iface_err`, `g_seg_ok_sum/tot_sum`, `g_segidx_tot/fail[NR_PDSCH_SEGIDX_MAX]`, `g_segidxc_tot/fail[NR_PDSCH_CBUCKETS][...]`, `g_pipe_n/sum[2][...]` | B | All cross-branch AGGREGATE decode/LDPC/LLR diagnostic counters, all `_Atomic`, none branch-attributed. Exactly the class of state G2 test 5 ("exercise failed TB CRC... verify exact admitted pilot/data paths and counters") needs disaggregated once there are 4 branches. |
| 299 | `static double g_sfo_ppm_ema;` | B | SFO EMA -- genuine per-branch synchronization state (each branch's own RF observations will differ), currently a single scalar. |
| 348, 353 | `static __thread uint32_t t_seg_K, t_seg_F, t_seg_C, t_seg_Z, t_seg_E, t_seg_R, t_seg_lbrm, t_seg_BG;` | W | Segmentation-parameter scratch cached per-thread for post-decode logging/shape counters; reused across jobs on the same worker thread. |
| 637 | `static __thread passive_harq_t g_harq; // zero-initialised per thread` | W (high priority) | Per-thread HARQ/decode scratch. Comment "zero-initialised per thread" means C's TLS zero-init runs ONCE at thread start, not between jobs -- if the same worker thread later decodes a DIFFERENT branch's transport block, nothing clears this between jobs. This is the concrete instance of P07's "consume only one branch's samples" / P09's "restore reused-worker state even on errors" requirement. |
| 1099, 1590-1591, 1619-1625 | `static __thread fourDimArray_t *toFree/toFree2..5; static __thread int16_t *llr; static __thread uint32_t llr_cap;` | W | Heap-backed (not raw TLS array) scratch buffers, resized on demand -- same "avoid the AVX alignment fault" pattern as `g_rxdataF` above; still needs job-boundary awareness once shared across branches. |
| 1257, 1317, 1369 | `static _Atomic uint64_t s_dfo_n, s_dfo_rej, s_fo_n;` | B | CFO/frequency-offset-related diagnostic counters, per-branch sync state once disaggregated. |
| 770, 1742, 1970 | `static __thread unsigned long s_sd_n, s_evm_n, s_subset_seen;` | W | Per-thread one-shot/rate-limited diagnostic counters. |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (1395 lines)

| Line(s) | Symbol(s) | Class | Why |
|---|---|---|---|
| 39 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B | Same producer-lag symbol. |
| 89 | `static PHY_VARS_gNB *g_gnb[NR_PUSCH_PASSIVE_MAX_CTX];` | B | **The "global context cap of six" cited in the brief**: `NR_PUSCH_PASSIVE_MAX_CTX` = **6**, defined at `nr_pusch_passive_decode.h:76`. Per the file's own comment (line ~83-88): "Every buffer the receive chain writes -- the rxdataF ring, pusch_vars, the ULSCH HARQ, the tpool -- hangs off `PHY_VARS_gNB`, so sharing one across threads would interleave two grants' intermediate state silently." This is a fixed-size decode-context pool with NO branch dimension; P08 states exactly this problem ("four branches times existing consumer counts cannot simply reuse indices"). |
| 90 | `static int g_gnb_nant;` | B | Antenna count used to configure each pooled gNB context. |
| 91-92, 96-97, 233, 239-240 | `g_try`, `g_crc_ok`, `g_rej_unsup`, `g_rej_setup`, `g_seg_fail`, `g_zero_tb`, `g_ta_refined`, `g_uci_trials`, `g_uci_rescued`, `g_cfr_submits`, `g_cfr_re`, `g_ant_pw[PASSIVE_UL_MAX_ANT]`, `g_ant_n` | B | Same pattern as the PDSCH file: cross-branch aggregate UL decode/CFR-submit counters, no attribution. |
| 122-127 | `static int g_ta_sweep_n; static int32_t g_ta_sweep_start, g_ta_sweep_step; static _Atomic uint32_t g_ta_sweep_seq; static _Atomic uint64_t g_ta_try[TA_SWEEP_MAX], g_ta_ok[TA_SWEEP_MAX];` | B | Timing-advance sweep search state -- sync/timing state, explicitly in scope per plan sec 3.2's schema and P05. |
| 167-175 | `kUtimName[]` (S), `g_utim_ns/n/max[UTIM_N]`, `g_utim_hist[8]`, `g_utim_over_slot`, `g_utim_on` | S / W | Per-stage wall-time histogram, perf diagnostic. |
| 678 | `static _Atomic int s_stage_once;` | W | One-shot diagnostic-stage flag. |
| 881-883, 898 | `static __thread float *ul_h; static __thread uint32_t *ul_k, *ul_l, ul_cap; static __thread int s_audit;` | W | Heap-backed (not fixed-array) TLS CFR-submission scratch, same pattern as the PDSCH file's `toFree*`/`llr`. |

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c` (247 lines)

Structurally identical to the two DL passive queues above; same classification, listed once:

| Line(s) | Symbol(s) | Class |
|---|---|---|
| 48 | `extern _Atomic long nr_ue_diag_producer_absolute_slot;` | B |
| 50-54 | `g_ring[]`, `g_depth`, `g_head`, `g_tail`, `g_count` | B |
| 56-57 | `g_lock`, `g_cv` | B |
| 59-64 | `g_queued`, `g_decoded`, `g_crc_ok`, `g_dropped_full`, `g_dropped_stale`, `g_max_lag` | B |
| 66-70 | `g_running`, `g_stop`, `g_nthreads`, `g_threads[]`, `g_ue` (single `PHY_VARS_NR_UE*`) | B |
| 75 | `g_args[]` | B |

---

## `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (337 lines, before this task's edits)

| Line(s) | Symbol | Class | Why |
|---|---|---|---|
| 24-25 | `int AOA_ENABLE = 0; int AOA_UL_ENABLE = 0;` (extern, non-static, true global linkage; declared in `nr_isac.h`) | S | "Process-wide policy values requested by the deployment" per the file's own comment -- set once at `nr_isac_init()`, read-only afterward. Fits (S)'s technical criteria, but note: P14 ("AoA removal/migration") will restructure this into a per-branch-aware design, so treat this classification as provisional pending P14, not as "no work needed." |
| 30 | `std::unique_ptr<SensingEngine> engine;` | B | **The sensing-engine singleton the brief names explicitly.** Plan sec 3.1: "Each branch owns a `SensingEngine` configured for one antenna" -- today there is exactly one, for the whole process. |
| 31 | `PipelineConfig pipeline;` | B | The single `[sensing]`-derived pipeline config the one `engine` above is constructed from; becomes per-branch alongside `engine`. |
| 32 | `std::atomic<bool> enabled, started;` | B | Lifecycle flags for the single `engine`; tied 1:1 to it. |
| 33 | `uint32_t aoa_antennas = 0;` | B | Antenna count assumed by the single engine's AoA path -- currently hardcoded to 4 when AoA is enabled (line ~223 in the pre-edit file), not derived from any live antenna count; this task's `nr_isac_set_nb_antennas_rx()`/`nr_rx_branch_set_check_antennas()` addresses the RELATED-but-separate `rx_branches` count check, not this field. |

This task ADDS three new process-wide symbols to this file (`branches`, `branches_valid`,
`expected_nb_antennas_rx`) -- all (B)/config-adjacent themselves, foundation-only, not yet
consulted by anything on the RT path. See "P03 additions" below.

---

## `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` (766 lines)

Notably sparse in file-scope mutable globals compared to every other file audited here -- most
acquisition-time state already lives inside `PHY_VARS_NR_UE`/`nr_ue_ssb_scan_t` (function
arguments), not file statics. This is a useful data point for "Reuse `PHY_VARS_NR_UE` where
appropriate" (P03 sentence 1): the initial-sync-owned fields are already struct-scoped, in
contrast to `nr-ue.c`'s `UE_thread()` locals, which sit outside any struct.

| Line(s) | Symbol | Class | Why |
|---|---|---|---|
| 359 | `static volatile int s_rms_done;` (inside the SSB-scan worker, gates the "PER-CHANNEL RMS AT ACQUISITION" diagnostic, comment at line 353-356) | W | One-shot-per-PROCESS diagnostic that already loops over all `nb_antennas_rx` channels in one shot (so it does not itself need to become 4 separate per-branch instances) -- but the "once per process" gate means it never re-fires on a later branch relock. Low severity, informational only. |

---

## P03 additions to `nr_isac.cc`/`nr_isac.h` (this task)

New process-wide state, all (B)/config, all foundation-only (nothing on the RT path reads them
yet):

- `nr_isac.cc`: `nr_rx_branch_set_t branches;`, `bool branches_valid;`, `int
  expected_nb_antennas_rx;` (anonymous namespace, internal linkage).
- `nr_isac.h`/`.cc`: `nr_isac_rx_branches()` (accessor, returns `NULL` unless sensing is enabled
  and the branch config parsed), `nr_isac_set_nb_antennas_rx()` (setter for the antenna-count
  cross-check).

## `nb_antennas_rx` reachability -- what I found, and the design choice this task made

`nr_isac_init()` (no arguments) is called from exactly two places: `executables/nr-uesoftmodem.c:248`
(the real path) and `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` (the `ENABLE_ISAC_SENSING=OFF`
no-op stub). At the real call site, the live antenna count IS already resolved by that point:
`get_options(uniqCfg)` (`nr-uesoftmodem.c:236`, which parses `CMDLINE_NRUEPARAMS_DESC` --
including `ue-nb-ant-rx` -> `nrUE_params.nb_antennas_rx`, `executables/nr-uesoftmodem.h:60`) runs
BEFORE `nr_isac_init()` at line 248, so `get_nrUE_params()->nb_antennas_rx` is live by then. But
`nr_isac_init()` does not take it as a parameter and does not include `nr-uesoftmodem.h` (a
library-layering direction PHY_NR_UE does not otherwise take on the executable's headers), so
there is currently NO live path from that value into `nr_isac.cc`.

Per the brief's own permitted alternative ("or take it as an init argument"), the honest options
were: (a) change `nr_isac_init()`'s signature and edit its two call sites
(`nr-uesoftmodem.c`, `nr_isac_stub.c`), or (b) add a separate setter callable before init and
leave it unwired in this foundation-only task. This task took (b):
`nr_isac_set_nb_antennas_rx(int)` was added, defaulting to 0 ("unknown"), under which
`nr_isac_init()` skips the antenna-count reject rather than failing on a value it cannot
evaluate. Nothing calls this setter yet -- wiring `nr-uesoftmodem.c:248` to call
`nr_isac_set_nb_antennas_rx(get_nrUE_params()->nb_antennas_rx)` immediately before
`nr_isac_init()` is the concrete next step, deferred because `nr-uesoftmodem.c` is outside this
task's committed-file scope (see the P03 brief's "Commit" section) and P03 is explicitly
"foundation only, deliberately off the real-time path." The antenna-count check itself
(`nr_rx_branch_set_check_antennas()`) is implemented and unit-tested in isolation in
`nr_rx_branch_test.cc`'s `RxBranchCheckAntennas.MoreBranchesThanAntennasRejected` case, so it does
not depend on this wiring to be verifiable.

## Summary counts (H / B / S / W, symbols not lines)

| File | H | B | S | W |
|---|---|---|---|---|
| `executables/nr-ue.c` | 8 | 15 | 3 | 3 |
| `executables/nr-ue-ru.c` | 6 | 3 | 0 | 0 |
| `nr_pdcch_blind_monitor_rt.c` | 0 | ~30 | 0 | 4 |
| `nr_pdcch_blind_monitor.c` | 0 | 13 | 8 | 0 |
| `nr_pdcch_passive_queue.c` | 0 | 12 | 0 | 0 |
| `nr_pdsch_passive_queue.c` | 0 | 11 | 0 | 1 |
| `nr_pdsch_passive_decode.c` | 0 | ~55 | 2 | ~15 |
| `nr_pusch_passive_queue.c` | 0 | 11 | 0 | 0 |
| `nr_pusch_passive_decode.c` | 0 | ~20 | 2 | 7 |
| `nr_isac.cc` (pre-P03) | 0 | 4 | 2 | 0 |
| `nr_initial_sync.c` | 0 | 0 | 0 | 1 |

Counts are approximate for the largest files (`nr_pdcch_blind_monitor_rt.c`,
`nr_pdsch_passive_decode.c`, `nr_pusch_passive_decode.c`) where dozens of `_Atomic` diagnostic
counters follow one repeated pattern (per-class/per-bucket LLR/LDPC/shape histograms) --
every symbol is still listed with its line number(s) in the tables above; the count rolls up
near-identical rows rather than omitting them.

## Cross-cutting observations

1. **The single global `PHY_VARS_NR_UE *g_ue` in all three passive queues
   (`nr_pdcch_passive_queue.c:73`, `nr_pdsch_passive_queue.c:88`,
   `nr_pusch_passive_queue.c:70`) is the clearest single "one branch assumed" defect** in the
   whole audit: every consumer thread in every queue decodes against the SAME UE context no
   matter which physical channel's samples it was handed. P04/P07/P08 cannot proceed without
   resolving this.
2. **The UL decode-context pool's fixed cap of 6 (`nr_pusch_passive_decode.c:89`,
   `NR_PUSCH_PASSIVE_MAX_CTX` = 6 at `nr_pusch_passive_decode.h:76`)** is the concrete numeric
   instance of the brief's "cite it" request -- confirmed by reading the header, not inferred.
3. **TLS is used correctly in some places and riskily in others.** `g_rxdataF[]`
   (`nr_pdsch_passive_queue.c:95`) and `toFree*`/`llr` (`nr_pdsch_passive_decode.c`,
   `nr_pusch_passive_decode.c`) are already heap-backed per-consumer-index rather than raw
   `__thread` arrays, specifically because a prior session hit an AVX alignment fault with the
   latter (cited in both files' own comments) -- this is the pattern P04's "bounded
   ownership/refcounts or explicit copies" should generalize. In contrast, `g_harq`
   (`nr_pdsch_passive_decode.c:637`, `static __thread passive_harq_t`) and the CFR-submission
   scratch buffers in `nr_pdcch_blind_monitor_rt.c:2235-2237` are still raw `__thread`/`static
   __thread` arrays with no job-boundary clear -- today safe only because there is one branch.
4. **Hardware ownership genuinely is already separated from cell/sync context in one place**:
   `nr-ue-ru.c` cleanly splits `nrue_rus`/`openair0_dev[]`/`openair0_cfg_g[]` (H, physical
   RU/device) from `nrue_cells`/`nrue_cell_fp` (B, cell/frame-parms context) via distinct arrays
   already. The coupling P03 warns about is not in this file's own globals -- it is in
   `PHY_VARS_NR_UE.rf_map` (`openair1/PHY/defs_nr_UE.h:285`), the field that ties a reused
   per-branch `PHY_VARS_NR_UE` struct back to `openair0_dev[UE->rf_map.card]`.
5. **`nr_pdcch_blind_monitor.c`'s `common_facts` (line 3276-3278) is the one piece of state in
   this whole audit that is correctly shared already** -- single SIB1 decode, PCI-keyed,
   mutex-protected, explicitly designed for the "all branches see the same cell" case. Worth
   citing as the existing pattern to follow for any OTHER genuinely-cell-wide (not branch-local)
   state P06-P09 identify.
6. **`AOA_ENABLE`/`AOA_UL_ENABLE` and `aoa_antennas` (`nr_isac.cc:24-25,33`) already hardcode an
   antenna count (4) rather than deriving it from any live source** -- the same class of defect
   this task's `rx_branches`-vs-antenna-count check is designed to catch, but for a DIFFERENT
   config surface (`aoa_enable`/`rx_array`, not `rx_branches`). Out of scope for P03 (P14 owns
   AoA migration) but worth flagging so P14 does not have to rediscover it.
