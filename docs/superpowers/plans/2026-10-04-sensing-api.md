# Sensing API (no algorithms) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Non-Claude agents (Codex) follow the same per-task discipline manually: failing test first, implement, green, commit, per-task report, stop for review.

**Goal:** When sensing is enabled the passive receiver runs **no sensing algorithm**; it only exports, through one versioned API, every observation it already produces (grant records, reference-signal records, per-antenna CFR) with timing, configuration-validity and RF metadata, for downstream sensing work done elsewhere.

**Architecture:** The `NR_UE_ISAC` engine (range-Doppler, ECA clutter, CLEAN, sparse Doppler, matrix completion, AoA, trackers, detection reports, isac_sync STO/CFO/SFO, CFAR, ZMQ detection bus) is deleted. The public entry points the receiver already calls (`nr_isac_submit_cfr*`, `nr_isac_enabled`, `nr_isac_source_enabled`) are kept as a thin adapter onto a new export module `nr_sense_api` that writes (a) JSON Lines records (schema v2, superset of `nr_passive_obs` v1) and (b) a binary CFR frame stream, both through non-blocking drop-on-full rings with a writer thread, optionally mirrored on a ZMQ PUB socket. The receiver never reads anything back (true today, CMakeLists.txt:1118-1120).

**Tech Stack:** C11 (OAI style), C++17 gtest, CMake/Ninja, Python 3 stdlib (reader tool), optional libzmq (already linked when found).

**Spec:** this document (§ Design below) + PROJECT_MEMORY §21 (observation design target) + `nr_passive_obs.h` schema v1 comment (current contract).

## Design

**D1. What is removed.** All sources in `NR_UE_ISAC_SRC` (CMakeLists.txt:1101-1117) except `nr_isac_ssb_axis.c` (PBCH needs it; always compiled today): `isac_fft`, `isac_sync`, `target_tracker`, `multi_target_tracker`, `matrix_complete`, `sparse_doppler`, `clean_deconv`, `det_quality`, `isac_aoa`, `eca_clutter`, `range_doppler`, `detection_report`, `sensing_engine`; `nr_isac.cc` is rewritten as the adapter (D3). Their tests/tools (`test_isac_sync`, `isac_sync_replay`, `test_eca_clutter`, `test_target_tracker`, `test_multi_target_tracker`, `test_matrix_complete`, `test_det_quality`, `test_isac_aoa`, `test_sparse_doppler`, `test_clean_deconv`, `test_occ_clean`, the engine parts of `test_nr_isac_ssb_source`) are removed. Engine outputs (`*_detections.csv`, `*_rvm_N.f32`, `reports.jsonl`, ZMQ detection bus on 5556) disappear. Offline consumers of those outputs (§F of the map: `tests/sensing_sim/`, `monitor/`, detection/track scripts) are deleted, except files under the frozen sens6 pathspec, which are never touched.

**D2. What stays in the receiver (not sensing algorithms).** CFR production by the receiver's own channel estimators and the data-aided CFR reconstruction (`nr_pdsch_data_aided.c`, `nr_pusch_data_aided.c`); receiver-side trackers (`ISAC_CFO_TRACK_HZ`, `ISAC_SFO_CORRECT`, `ISAC_DMRS_FO_APPLY`; `ISAC_RX_BRANCH_FO` stays unset); `nr_isac_ssb_axis.c`; the `[sensing]` config section (also read by `nr_pdcch_blind_monitor.c:2989` and `nr_csirs_monitor.c:103`); `nr_passive_obs`, `nr_passive_metrics`, `nr_passive_ue_ctx`, `nr_passive_cfg_epoch`.

**D3. API surface (receiver side, unchanged signatures).** `nr_isac_init/start/stop/enabled/source_enabled/submit_cfr/submit_cfr_at/submit_cfr_multi/subslot_config` keep their prototypes in `nr_isac.h`; `nr_isac_aoa_antennas` is removed (AoA algorithm) — callers get the antenna count from frame parameters. `submit_*` copy the CFR into a CFR frame and enqueue it (never block; drop and count on full). `[sensing]` keys kept: `enable`, `sources`, plus new `cfr_path`, `records_path`, `endpoint` (ZMQ, optional), `cfr_ring_frames`, `cfr_max_re`. Every other legacy key (cfar_*, clean_*, mc_*, track_*, aoa_*, rx_array, sync_*, rx_pos_*, tx_pos_*, illuminator_id, out_path, report_path, report_endpoint, cpi_slots, ...) is **accepted and ignored** with one LOG_W listing the ignored keys (frozen sens6 `.conf` files must still parse). `rx_id` and `rx_array` (antenna layout, metadata only) are copied into the session record.

**D4. Records (JSON Lines, schema 2).** One line per record, `"schema":2`, `"type"` ∈ {`session`, `grant`, `rs`}; unknown values are JSON `null`; consumers ignore unknown keys; a meaning change needs schema 3. Every record carries `epoch`, `identity_gen`, `dedicated_epoch` (from `nr_cfg_epoch_snapshot`, null when `ISAC_RECONF` is off), `abs_slot`, `frame`, `slot`, `t_mono_ns`, `rx_ts` (RF sample timestamp of the slot start when available, else null), `pci`.
- `session` (at start and whenever carrier/fs/n_rx/gain changes): `carrier_dl_hz`, `carrier_ul_hz`, `scs_khz`, `fs_hz`, `n_rb_dl`, `n_rx`, `rx_gain_db` (null if unknown), `rx_id`, `rx_array` (string from config or null), `build` (git describe), `schema`.
- `grant`: every v1 field unchanged (names and meaning) + `dl_coverage` (`queue`|`inline`, fixes K29) + `verif` (TRUSTED/HINT/SUSPECT of the RNTI's DCI length from UeContext when enabled, else null).
  UL grants additionally (null when unknown): `k2`, `dci_slot`, `transform_precoding`, `freq_hopping`, `dmrs_type`, `dmrs_add_pos`, `dmrs_n_scid`, `ta_est_samples` (UL timing offset of this UE estimated from its DM-RS relative to the DL frame, sign convention documented), `cfo_est_hz` (per-UE UL CFO estimate when the receiver computes one), `ul_timing_ref` (`dl_frame` | `ta_tracked`).
- `rs`: `source` (ssb, csirs, pdsch_dmrs, pdsch_data, pusch_dmrs, pusch_data — the existing submit sites; PDCCH DM-RS is out of scope; SRS, PRACH: see Task 6; PUCCH out of scope for now), `dir` (DL|UL), `rnti` (null for SSB/CSI-RS), `start_rb`, `nb_rb`, `n_re`, `l_syms` (list), `ports`, `n_ant`, `noise_var`, `slot_frac`, `cfr_frame_id` (link into the CFR stream, null if the CFR frame was dropped), `ssb_index` / `k_ssb` (SSB; the hard-coded k_ssb=0 at phy_procedures_nr_ue.c ~1600 is fixed to the real value), `csirs_resource` (row/fd/l0/density/period when known). UL `rs` records also carry the UL fields of the matching grant (`ta_est_samples`, `transform_precoding`, `dmrs_*`) so a CFR frame can be interpreted without joining.

**D5. CFR frame stream (binary, little-endian, version 1).** File `cfr_path` (and ZMQ topic `cfr` when `endpoint` set). Frame = fixed header + arrays:
`magic "NRCF" | u16 version=1 | u16 header_len | u64 frame_id | u8 source | u8 n_ant | u16 reserved | u32 n_re | u64 abs_slot | f32 slot_frac | f32 noise_var | u32 epoch | u32 identity_gen | u16 k_abs[n_re] | u8 l_sym[n_re] | pad to 4 | f32 iq[n_ant][n_re][2]`. `frame_id` is monotonic per run; the matching `rs` record carries it. Frames larger than `cfr_max_re` are truncated and flagged (`n_re` reduced, record `truncated:true`).

**D6. Non-functional.** Producers (PHY RX, blind monitor, PDSCH queue, PUSCH monitor threads) never block, never malloc, never do I/O: preallocated frame ring, drop-on-full with per-source counters exported in `ISAC_METRICS` (`sense_cfr_drop`, `sense_rec_drop`, `sense_io_errors`). Sensing disabled (`[sensing].enable=0`, default) ⇒ zero cost beyond one atomic load per call site and identical receiver behaviour.

## Global Constraints

- Repository rules (CLAUDE.md): sens6-frozen gate before every commit (`git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`); new configs use `.cfg`; `git add <explicit paths>`; no stash; never build while `nr-uesoftmodem` runs; evidence labels naming the host; `ISAC_RX_BRANCH_FO` stays unset.
- Lock policy (`/tmp/td_measure.lock`): shared for builds/tests, exclusive per rfsim/benchmark run only.
- Regression gate: `tests/passive_rx/dgx/rfsim_regress.sh` 4-RX postconv gate (CLAUDE.md "Regression:" line) must PASS with sensing disabled (baseline) and with sensing enabled and all sources on.
- Merged-main defaults stay ON and unchanged: ISAC_TD_GRANTWORK, ISAC_TD_CB0_ELIM, ISAC_TD_FIELDBOOK=2, ISAC_TD_CB0_BACKEND=auto, ISAC_TD_TB_CPU_WHILE_ACQ; levers C, P, E off. `ISAC_RECONF` default off.
- Both directions: every record/frame type exists for DL and UL where the signal exists; UL timing is relative to the DL frame (the receiver does not know other UEs' TA) and says so (`ul_timing_ref`).
- `nr_passive_obs` schema v1 consumers (`campaign.py`, `check_obs.py`) keep working: v1 file output is unchanged unless `records_path` is set.
- Build both modes: `ENABLE_ISAC_SENSING=ON` (API + optional ZMQ) and `OFF` (stub: every API call is a no-op); `ENABLE_TESTS=ON` must link in both modes (today it probably does not in OFF mode).

## Review Focus

1. **Slow or absent consumer / disk full** — writer falls behind or `write()` fails: producers keep running at full rate, drops and I/O errors are counted, never a stall or crash (owner: Task 1, `SlowWriterNeverBlocksProducer`, `IoErrorCounted`).
2. **Legacy `.conf` with algorithm keys** (frozen sens6 confs, `adaptive_*sensing.conf`) — parses, warns once, exports with the kept keys (owner: Task 2, `LegacySensingKeysAcceptedAndIgnored` using a real frozen conf read-only).
3. **Epoch bump between capture and write** — record/frame carry the epoch at capture time, not at write time (owner: Task 3, `EpochStampedAtCapture`).
4. **1 RX vs 4 RX and very large CFR (273 PRB, many DM-RS symbols)** — n_ant and n_re correct; oversize frames truncated and flagged, not overflowed (owner: Task 1, `OversizeFrameTruncatedAndFlagged`; Task 4, `FourAntennaFrameLayout`).
5. **UL never exercised on the DGX bed (K28)** — phy-test rfsim has no PUSCH; UL records/frames must be validated on the sens6 SA bed with an attached UE (owner: Task 5 Step 5b).
6. **Sensing disabled** — byte-identical receiver logs/metrics keys except new zero counters, gate PASS (owner: Task 5 gate arm `api_off`; Task 2 `DisabledApiIsNoop`).

---

### Task 1: `nr_sense_api` export core (records ring, CFR frame ring, writers, ZMQ mirror)

**Files:** Create `openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.{h,c}`; Test `openair1/PHY/NR_UE_TRANSPORT/tests/nr_sense_api_test.cc`; Modify `CMakeLists.txt` (library + `test_nr_sense_api`).

**Interfaces — Produces:**
```c
#define NR_SENSE_SCHEMA 2
#define NR_SENSE_CFR_VERSION 1
typedef enum { NR_SENSE_SRC_SSB, NR_SENSE_SRC_CSIRS, NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PDSCH_DATA,
               NR_SENSE_SRC_PUSCH_DMRS, NR_SENSE_SRC_PUSCH_DATA, NR_SENSE_SRC_COUNT } nr_sense_src_t;
typedef struct { const char *records_path, *cfr_path, *endpoint; uint32_t cfr_ring_frames, cfr_max_re, rec_ring_slots; } nr_sense_cfg_t;
int  nr_sense_open(const nr_sense_cfg_t *cfg);          /* 0 ok; starts writer thread(s) */
void nr_sense_close(void);                              /* drains, joins, safe to call twice */
bool nr_sense_enabled(void);                            /* lock-free */
/* Producer side: never blocks, never mallocs. Returns frame_id or 0 if dropped. */
uint64_t nr_sense_push_cfr(nr_sense_src_t src, int n_ant, const float *iq, int ant_stride, int n_re,
                           const uint16_t *k_abs, const uint8_t *l_sym, uint64_t abs_slot, float slot_frac,
                           float noise_var, uint32_t epoch, uint32_t identity_gen, bool *truncated);
bool nr_sense_push_record(const char *json_line);       /* pre-formatted JSON object, no newline; false if dropped */
typedef struct { uint64_t cfr_pushed, cfr_dropped[NR_SENSE_SRC_COUNT], rec_pushed, rec_dropped, io_errors, truncated; } nr_sense_stats_t;
void nr_sense_stats(nr_sense_stats_t *out);
```
Ring design copies `nr_passive_obs.c` (fixed slots, PRIO_INHERIT mutex, one writer thread, drop-on-full, `after_close` counter); CFR frames are preallocated `cfr_ring_frames × (header + cfr_max_re × (2 + 1 + 8 × n_ant_max))` bytes with `n_ant_max = 8`.

- [ ] **Step 1: Failing tests** (`nr_sense_api_test.cc`): `RoundTripCfrFrame` (push 2-antenna, 12-RE frame; read the file back; every header field and IQ value equal; frame_id 1), `RecordsAreJsonLines` (push 3 records; file has 3 lines, each parses as an object containing `"schema":2`), `SlowWriterNeverBlocksProducer` (writer paused through a test hook `nr_sense_test_pause_writer(true)`; 10 000 pushes into a 16-frame ring return within 50 ms total; `cfr_dropped` = 10 000 − 16), `IoErrorCounted` (records_path in a read-only dir via test hook on the fd: `io_errors > 0`, no crash), `OversizeFrameTruncatedAndFlagged` (`n_re = cfr_max_re + 5`: stored `n_re == cfr_max_re`, `*truncated == true`, `stats.truncated == 1`), `CloseTwiceAndPushAfterCloseSafe`, `ZmqMirrorOptional` (GTEST_SKIP when built without ZMQ; else a SUB socket receives topic `cfr` with identical bytes).
- [ ] **Step 2:** build `test_nr_sense_api` under `flock -s`, run, expect failures (symbols missing).
- [ ] **Step 3:** implement `nr_sense_api.c` per the interface (writer writes CFR frames with one `write()` per frame, records as `line + '\n'`; ZMQ PUB mirror when `endpoint` non-null and ZMQ is available, `#ifdef` guarded).
- [ ] **Step 4:** all 7 tests green; shuffle seeds 1/3/5; TSAN run of the test binary if the TSAN build option exists (as for `test_nr_passive_obs`).
- [ ] **Step 5: Commit** `feat(sense): nr_sense_api export core — records + CFR frame stream, non-blocking`.

### Task 2: Remove the sensing engine; `nr_isac.cc` becomes the API adapter; config compatibility

**Files:** Modify `CMakeLists.txt:1101-1139` (library sources both modes; remove engine tests/tools at :2359-2492 except non-ISAC `test_dlsch_fixed_point`), rewrite `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (adapter) and `nr_isac.h` (drop `nr_isac_aoa_antennas`; doc comment = D3), `nr_isac_stub.c` (no-op API); delete the algorithm sources/headers/tests listed in D1; update the 8 includers of `nr_isac.h` only where they use removed symbols (`nr_isac_aoa_antennas` callers → `frame_parms->nb_antennas_rx`). Test `openair1/PHY/NR_UE_ISAC/tests/nr_isac_adapter_test.cc`.

**Interfaces — Consumes:** Task 1 API. **Produces:** unchanged receiver-facing prototypes of `nr_isac_init/start/stop/enabled/source_enabled/submit_cfr/submit_cfr_at/submit_cfr_multi/subslot_config` (D3).

- [ ] **Step 1: Failing tests** (`nr_isac_adapter_test.cc`): `SubmitMultiProducesOneFramePerCall` (enable via a test config: `[sensing] enable=1 sources="all" cfr_path=<tmp>`; `nr_isac_submit_cfr_multi` with 4 antennas → one frame, n_ant 4, source mapped from the old enum: CSI_RS→CSIRS, PDSCH_DATA→PDSCH_DATA, PDSCH_DMRS_BLIND→PDSCH_DMRS, PUSCH_DMRS→PUSCH_DMRS, PUSCH_DATA→PUSCH_DATA, SSB→SSB; legacy PDSCH_DMRS (no caller) → PDSCH_DMRS), `SourceFilterRespected` (`sources="ssb,csirs"` → PDSCH submissions produce nothing and `nr_isac_source_enabled(PDSCH_DATA)==false`), `LegacySensingKeysAcceptedAndIgnored` (parse a COPY in /tmp of `tests/passive_rx/adaptive_dl_ul_sensing.conf` — never modify the original; init succeeds; one LOG_W naming ignored keys; captured via a log hook), `DisabledApiIsNoop` (enable=0: every call returns immediately, no files created, `nr_isac_enabled()==false`).
- [ ] **Step 2:** red.
- [ ] **Step 3:** implement adapter: parse kept keys with the existing config helpers (same section name `sensing`), map enums, call `nr_sense_open/close/push_cfr`; epoch/identity from `nr_cfg_epoch_snapshot` when `nr_cfg_reconf_enabled()` else 0 with a `reconf` flag in the session record. Delete engine files and CMake entries; fix OFF-mode test linking.
- [ ] **Step 4:** green in BOTH build modes: configure a second build dir `-DENABLE_ISAC_SENSING=OFF` and build `tests`; full ctest in the ON dir; `grep -rn "sensing_engine\|range_doppler\|eca_clutter\|isac_sync\b" --include=*.c --include=*.cc --include=*.h --include=CMakeLists.txt .` returns nothing outside docs and the frozen snapshot.
- [ ] **Step 5: Commit** `refactor(sense)!: remove sensing algorithms; nr_isac is a thin export adapter (no algorithm runs)`.

### Task 3: Records schema 2 — session, grant (v1 superset + epoch/verif/inline DL), rs

**Files:** Modify `nr_passive_obs.{c,h}` (format v2 when `records_path` configured; v1 path untouched), `nr_pdsch_passive_queue.c:1494-1523`, `nr_pusch_passive_decode.c:1576-1600`, `nr_pdcch_blind_monitor_rt.c` (inline DL decode site ~7248: K29), `nr_isac.cc` (emit `rs` record per submitted frame, `session` record at open and on RF change), `executables/nr-uesoftmodem.c` (session metadata: carrier, fs, scs, n_rb, n_rx, gain from `nr_rf_card_config_gain` result / `nrue_ru_adjust_rx_gain` updates, `rx_id`, `rx_array`, git describe from the existing build-version macro). Test `tests/nr_sense_records_test.cc`.

**Interfaces — Consumes:** Task 1 `nr_sense_push_record`; Task 2 adapter; `nr_cfg_epoch_snapshot()`, `nr_ue_ctx_get()` (UeContext, may be disabled). **Produces:** `void nr_sense_emit_session(const NR_DL_FRAME_PARMS *fp, double rx_gain_db);` `void nr_sense_emit_grant(const nr_passive_obs_t *o, int dl_inline);` `void nr_sense_emit_rs(nr_sense_src_t src, const nr_sense_rs_meta_t *m, uint64_t cfr_frame_id);` with `typedef struct { uint16_t rnti; int start_rb, nb_rb, n_re, n_ant, ports, ssb_index, k_ssb; uint32_t l_sym_mask; float noise_var, slot_frac; uint64_t abs_slot; int frame, slot; const char *csirs_resource; bool truncated; } nr_sense_rs_meta_t;` (−1 = unknown → JSON null).

- [ ] **Step 1: Failing tests:** `UlGrantCarriesUlFields` (PUSCH passive-decode fixture: k2, dci_slot, transform_precoding, dmrs_* and ta_est_samples present; unknown ⇒ null), `UlRsRecordSelfContained` (UL rs record repeats the grant's UL fields), `GrantV2IsV1Superset` (format a v1 struct both ways: every v1 key/value identical in v2, plus `type`, `schema`=2, `epoch`, `identity_gen`, `dedicated_epoch`, `dl_coverage`, `verif`), `EpochStampedAtCapture` (stamp, bump epoch via test hook, write: record has the old epoch), `NullsForUnknown` (reconf off ⇒ epoch fields null; UeContext off ⇒ verif null; rx_ts unknown ⇒ null), `RsLinksCfrFrame` (rs record `cfr_frame_id` equals the pushed frame id; dropped frame ⇒ null), `SessionOnGainChange` (two gain updates ⇒ two session records), `InlineDlRecorded` (inline decode path fixture produces a grant with `dl_coverage:"inline"`).
- [ ] **Step 2:** red. **Step 3:** implement; fix SSB `k_ssb` (pass the real k_SSB from frame params at phy_procedures_nr_ue.c ~1600 instead of 0) with test `SsbKssbNotHardcoded` in the same file. **Step 4:** green; v1 tests (`test_nr_passive_obs`) unchanged and green; shuffle seeds 1/3/5.
- [ ] **Step 5: Commit** `feat(sense): schema-2 records — session, grant (epoch, verif, inline DL), rs linked to CFR frames`.

### Task 4: Receiver gating decoupled from the engine; per-source coverage

**Files:** Modify `nr_pdcch_blind_monitor_rt.c` (`want_dmrs` ~3532, `want_data` ~3533, `need_chest` ~6902, early-out ~6976), `phy_procedures_nr_ue.c` (SSB/PDSCH taps), `csi_rx.c:949-1058`, `nr_pusch_passive_decode.c:1002-1116`, `nr_pdsch_data_aided.c`, `nr_pusch_data_aided.c`. Test `tests/nr_sense_gating_test.cc` + extend `test_nr_pdcch_blind_monitor` fixtures.

**Interfaces — Consumes:** `nr_isac_source_enabled` (Task 2). **Produces:** no new symbols; behaviour: CFR work happens iff the corresponding source is enabled, independent of any (removed) engine state.

- [ ] **Step 1: Failing tests:** `ApiOffNoExtraChannelEstimation` (sensing disabled: blind monitor fixture performs the same number of `nr_pdsch_channel_estimation` calls as before — count via existing test hook or a new static counter under `#ifdef NR_SENSE_TEST`), `PdschDmrsSourceDrivesChest` (only `pdsch_dmrs` enabled ⇒ chest runs and one frame per accepted grant), `FourAntennaFrameLayout` (4-RX fixture ⇒ frames have n_ant 4, antenna-major IQ matches the estimator buffer), `EverySourceReachable` (each of the 6 sources produces ≥ 1 frame in its existing fixture: SSB PBCH fixture, CSI-RS search fixture, PDSCH queue fixture, PUSCH passive decode fixture, data-aided DL/UL fixtures).
- [ ] **Step 2–4:** red → implement → green; full ctest; shuffle seeds 1/3/5.
- [ ] **Step 5: Commit** `refactor(sense): CFR production gated only by API source enables`.

### Task 5: Retire engine consumers; reader tool; docs; DGX validation

**Files:** Delete `tests/sensing_sim/`, `tests/passive_rx/monitor/` detection bus parts (`monitor.py`, `demo_scene.py`, `test_monitor.py`), and the detection/track scripts listed in the map §F (`design_scene.py`, `check_detection.py`, `gt_score.py`, `plot_rvm.py`, `rnti_gate.py`, `e0_axis.py`, `chance_level.py`, `merge_receivers_walltime_n.py`, `plot_est_vs_gt.py`, `plot_fused_tracks.py`, `score_passive_tracks.py`, `tests/ota_sync_bench/{grid_dump,process_capture}.py`) — **only if not under the frozen pathspec**; `auto_acquire.py` keeps its non-detection functions (remove only the `reports.jsonl` reader). Update run scripts (`run_passive_rx.sh`, `run_adaptive_receive_test.sh`, `_run_*.sh`) to stop referencing detection outputs. Create `tools/sense_api/read_sense.py` (reads records + CFR frames, joins on `cfr_frame_id`, prints a summary, `--npz OUT` export) and `tools/sense_api/test_read_sense.py` (fixture written by `test_nr_sense_api`'s golden-file test). Create `tests/passive_rx/sense_api.example.cfg` (`[sensing] enable=1 sources="all" records_path=... cfr_path=...`). Update PROJECT_MEMORY.md (§21 → implemented schema 2 + CFR v1; §10.2 keys; §11 log lines; §24 K28/K29 status; remove engine sections' operational instructions, keep history labelled HISTORICAL) and CLAUDE.md only if a rule mentions removed tools.

- [ ] **Step 1: Failing test:** `test_read_sense.py::test_join_records_and_frames` on the golden fixture (record count, frame count, every `rs.cfr_frame_id` resolves, IQ shape `(n_ant, n_re)`).
- [ ] **Step 2–4:** implement reader; delete consumers; `grep -rn "reports.jsonl\|5556\|_detections.csv" tests tools executables openair1` returns only HISTORICAL docs; full ctest + python tests.
- [ ] **Step 5 (orchestrator, exclusive lock per run):** 4-RX gate ×2 per arm on DGX: `api_off` (default), `api_all` (`sense_api.example.cfg`: all 6 sources, records + CFR files on local disk). Pass criteria: both arms gate PASS; `api_all` drop counters `sense_cfr_drop`/`sense_rec_drop` reported; CPU µs/slot of the receiver (thrprof) `api_all` ≤ +10 % vs `api_off`; `read_sense.py` parses the `api_all` output with every `rs` linked. Record results `[MEASURED, DGX aarch64 ...]` in PROJECT_MEMORY.
- [ ] **Step 5b (operator, sens6 SA bed, after R13 bring-up):** UL arm: `api_all` with an attached UE generating UL traffic (iperf UL) — pass: `grant` UL records with `crc`, `rs` pusch_dmrs/pusch_data frames linked, `ta_est_samples` stable within ±2 samples for a static UE, K28 closed; DL arm repeated on the SA cell (SIB1 present). Steps and validation rules appended to `tests/passive_rx/sa_bed/RUNBOOK.md` and PROJECT_MEMORY.
- [ ] **Step 6: Commit** `feat(sense)!: sensing API v2 complete — engine consumers retired, reader tool, docs`.

### Task 6: New UL observations — PRACH (6a), SRS (6b)

Investigation (2026-10-04, code read of rr/integration): the passive receiver detects/decodes/estimates **none** of SRS,
PUCCH, PRACH today; UL processing exists only for PUSCH (`nr-ue.c:690-698` → `nr_pusch_passive_monitor_process`), with
reusable UL FFT `nr_pusch_passive_fep_symbol` (`nr_pusch_passive_decode.c:583-625`) and the fake-gNB adapter
`passive_gnb_prepare` (`:375-480`) linked as `PHY_NR_PASSIVE_UL` (CMakeLists.txt:1051-1082). UL timing: only N_TA_offset;
per-UE N_TA unknown, ±400-sample residual; PUSCH uses a per-grant delay search — SRS needs the same. TDD only
(FDD needs a second carrier: out of scope). These are receiver decoding/estimation steps (not sensing algorithms) and
export through Task 1-3 (`rs` sources `prach`, `srs`; `prach` records carry preamble, energy, delay — no CFR).

**6a PRACH (small-medium, first):** config is fully broadcast (SIB1 rach-ConfigCommon already in `nrUE_config.prach_config`,
`config_ue.c:243-275`); add `NR_TRANSPORT/nr_prach.c` (+ `nr_prach_common.c` if not linked) to `PHY_NR_PASSIVE_UL`; on RACH
occasions (38.211 PRACH table lookup, which `nr_pdcch_sib1_prior.c:130-133` skips today) build a `prach_item_t`
(`PHY/defs_gNB.h:31-48`) like `nr_schedule_rx_prach` (`nr_prach.c:259`) and call `rx_nr_prach_ru` (`:381`) + `rx_nr_prach`
(`:408`) on `ue->common_vars.rxdata`. Validation ground truth: overheard RAR RAPID + TA (`nr_passive_mac_ta.h`). Tests:
`PrachOccasionTable` (config index → occasions), `PrachDetectsSynthPreamble` (`compute_nr_prach_seq` generated preamble at a
known delay → detected preamble id and delay ±1 sample), `PrachMatchesRarRapid` (fixture). Needs SIB1 (absent ⇒ inert, logged).
**6b SRS (large; blind, SA and NSA alike):** the dedicated `srs-Config` is normally carried in a ciphered RRCReconfiguration
(and on NSA in LTE RRC), so — consistent with the receiver's agnostic design (PROJECT_MEMORY §1/§11.5: dedicated IEs are
inferred, never read; `nr_passive_rrc_harvest.c` "rarely useful") — SRS parameters are found **blindly**; an unciphered
RRCSetup carrying `srs-Config` (harvest extension) is only an optional HINT tried first, never required.
Blind pipeline (pattern of `nr_csirs_blind_search.c`): (1) comb-energy detector on UL symbols (last 6 of UL slots):
comb ∈ {2,4,8} × comb offset × symbol count {1,2,4} → PRB span; (2) recurrence over slots → T_SRS/offset; (3) sequence
search: base-sequence group u ∈ 0..29 (n_ID^SRS mod 30 with group/sequence hopping off; with group hopping the per-slot
group follows from n_ID mod 30 once found) × cyclic shift (8 or 12) → a few hundred correlations (GPU batch optional);
(4) LS estimate + interpolation with the existing gNB chain (`generate_srs_nr` → `nr_get_srs_signal` (`srs_rx.c:54`, add
to `PHY_NR_PASSIVE_UL`) → `nr_srs_ls_channel_estimation` (`nr_ul_channel_estimation.c:778`) →
`nr_srs_channel_interpolation` (`:937`)) driven by a synthesised `nfapi_nr_srs_pdu_t`; per-UE delay search as for PUSCH.
Attribution (SRS carries no RNTI): aperiodic SRS after a decoded DCI 0_1/1_1 SRS request of a known RNTI (field already
decoded, `nr_pdcch_blind_monitor.c:5029,5061`); delay match with that UE's PUSCH `ta_est_samples`; CFR similarity; else an
anonymous track id (comb, offset, CS, group, period) — records carry `rnti` or `srs_track_id` + `attribution` method.
Tests: `SrsCombDetectorSynth` (generated SRS, comb 2/4, offsets → detected), `SrsPeriodFromRecurrence`,
`SrsGroupCsSearchSynth` (u, CS recovered), `SrsLsEstimateSynth` (known channel → CFR ±0.5 dB),
`SrsAperiodicAttribution` (trigger at RNTI → SRS attributed), `SrsHarvestHintOptional` (hint used when present, search
still works without). Validation: sens6 SA bed (OAI gNB normally configures periodic SRS; gNB config/logs = ground
truth, validation only). Sub-commits: detector+search, estimation+export, attribution.
Validation: offline synthetic tests on DGX; live on the sens6 SA bed (PRACH/RAR/Msg4 on air; also the OCUDU ZMQ bed,
`tests/passive_rx/ocudu/ocudu_zmq_broker.py:37`, lets the receiver sync before the UE attaches). Commit per sub-task:
`feat(sense): passive PRACH detection + export`, `feat(sense): blind SRS detection, CFR export and attribution`.

## Self-review record (2026-10-04)

- Coverage vs Design: D1 → Task 2/5; D2 → Task 2 (keep list) + Task 4; D3 → Task 2; D4 → Task 3; D5 → Task 1; D6 → Task 1 (non-blocking) + Task 5 (CPU budget). K28 (UL never exercised) stays dependent on a bed with PUSCH (SA bed, R13); K29 fixed in Task 3.
- UL: grants + PUSCH DM-RS/data CFR covered (Task 3/4), UL-specific fields added, UL validation only possible on the sens6 SA bed (Step 5b). SRS/PUCCH/PRACH: Task 6 (6a PRACH first; 6b SRS blind (SA and NSA), harvest only as optional hint). PUCCH deliberately out of scope for now (operator 2026-10-04).
- Out of scope (deliberately): PDCCH DM-RS CFR export (no estimator output is submitted today; add as source 7 later), any detection/tracking/clutter/Doppler processing, wall-clock air-time beyond `rx_ts`.
