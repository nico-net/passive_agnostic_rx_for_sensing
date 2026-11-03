# Traffic Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** add `[sensing] mode = "traffic"` to the receiver. In this mode one record is written per accepted DCI on every
PCI, with no CFR export and PDSCH/PUSCH decode limited to solver probes plus rare check decodes. Validate it against
legacy on replay.

**Architecture:**
- A pure mode resolver turns the `.cfg` keys into one per-process mode plus narrowed defaults.
- At each DCI accept point, a fixed-size struct goes into the existing per-cell sense ring, the way CFR metadata already
  does (`record_binary` + `rs_jobs`). The writer thread formats it to JSON.
- A per-cell traffic-decode table decides, before enqueue, whether a non-probe decode runs.
- Config changes are written as `type="config"` records, and every DCI record points to one through `cfg_ref`.

**Tech Stack:** C11 (receiver), C++17 (nr_isac.cc, gtest unit tests), CMake/Ninja, Python 3 (offline reinterpretation
tool and its test).

**Spec:** `docs/superpowers/specs/2026-11-03-two-modes-design.md` (sections §2, §3, §8, §10 step 1). This plan covers
traffic mode only. Sensing mode gets its own plan after the operator checkpoint at the end of this one.

## Global Constraints

- No new env, CLI or -D flag. The only new keys are `[sensing] mode` and (sensing plan) `sense_min_prb`.
- With no `mode` key, behaviour stays exactly as today. The existing regression gate and ctest must not change.
- sens6 is FROZEN: never edit `tests/passive_rx/captures/*`, `tests/passive_rx/*.conf`, or
  `tests/passive_rx/sens6_host_snapshot_2026-09-30/*`. New configs use `.cfg`.
  - Check: `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`
- gNB logs and configs are validation ground truth only, never receiver input.
- Builds run under `flock -s /home/nicola/NICOLA/wt/rr-orchestration/locks/heavy.lock`, never while
  `pgrep -x nr-uesoftmodem` finds a process.
  - Build dir `cmake_targets/ran_build/build`:
    `ninja -k 0 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4`.
  - Known ARM ctest failures: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256, test_nr_pusch_ra0_qam64.
  - The build tree must hold all 17 module `.so` files: `ls cmake_targets/ran_build/build/*.so | wc -l` must equal
    the mpci-main build's count (24 files at e739bd51b0).
- Stop a receiver with SIGINT only. ISAC_RX_BRANCH_FO stays unset.
- `git add <explicit paths>`, never `-A`, never stash. Do not push. Branch `twomodes/design`, worktree
  `/home/nicola/NICOLA/wt/rr-twomodes`.
- Portable code: no fixed cores, no GB10-specific tuning. It must also compile on sens6 (x86, sm_89).
- Never trade PDSCH CRC for latency. A decode that starts runs to the end.
- Per-cell state uses the existing `NR_CELL_STATIC` / `NR_CELL_REF` pattern, never plain statics.
- Constants named in the spec: `T_CHECK_NS = 2000000000`, `CHECK_EVERY_GRANTS = 32`, `K_REOPEN = 4`.
- Evidence labels per PROJECT_MEMORY §0.1 on every reported number.

## Review Focus

1. **Ring full under a DCI burst.** Expect: the accept path never blocks; `dci_rec_lost` counts the drop and
   `dci_rec_generated` still equals `dci_accepted`. (Test: Task 2, `DciRingFullCountsLost`.)
2. **SI/RA/P/TC grants in traffic mode.** Expect: still decoded exactly as in legacy, because SIB1 feeds the BWP and
   configuration. (Test: Task 6, `NonCRntiClassesAlwaysDecoded`.)
3. **A solved RNTI with a low-SNR or rv>0 check failure.** Expect: it does not count toward the K_REOPEN reopen, so
   there is no churn on weak UEs. (Test: Task 6, `LowSnrFailDoesNotCountTowardReopen`.)
4. **A `.cfg` that sets `mode="traffic"` together with `sources="pdsch_data"` or `pdcch_blind_monitor_pdsch` decode=2.**
   Expect: the CFR settings are narrowed away with one log line, never honoured. (Test: Task 1, `TrafficRefusesCfrSources`.)
5. **A DCI 1_1 field inside the `pre_ant` group (HARQ pid, DAI, TPC, PRI, K1) on a solved layout whose split is not
   unique.** Expect: null in the record, never a guessed value. (Test: Task 4, `PreAntAmbiguousGivesNull`.)

---

### Task 1: Mode key and resolver

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/nr_isac_mode.h`, `openair1/PHY/NR_UE_ISAC/nr_isac_mode.c`
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.cc:248-300` (parse `mode`, apply the resolver), `nr_isac.h` (export `nr_isac_mode()`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2965-2975, 3452` (take the resolved pdsch/pusch defaults)
- Test: `openair1/PHY/NR_UE_ISAC/tests/nr_isac_mode_test.cc`, registered in `CMakeLists.txt` next to `test_nr_sense_records` (~:2963)

**Interfaces:**
- Produces:
  ```c
  typedef enum { NR_ISAC_MODE_LEGACY = 0, NR_ISAC_MODE_TRAFFIC = 1, NR_ISAC_MODE_SENSING = 2 } nr_isac_mode_t;
  typedef struct { nr_isac_mode_t mode; unsigned sources; int pdsch_decode; int pusch_decode; bool narrowed; } nr_isac_mode_cfg_t;
  /* mode_str NULL/"" -> LEGACY (all inputs passed through unchanged). explicit_* < 0 = key not set. Returns false on an unknown mode string. */
  bool nr_isac_mode_resolve(const char *mode_str, unsigned explicit_sources, int explicit_pdsch_decode,
                            int explicit_pusch_decode, nr_isac_mode_cfg_t *out);
  nr_isac_mode_t nr_isac_mode(void);   /* process-wide, set once by nr_isac_init(); LEGACY before */
  ```
- Rules:
  - Traffic sets `sources = 0` (no CFR source, ever) and `pdsch_decode = 1`, the decode-only level
    (`want_decode`, nr_pdcch_blind_monitor_rt.c:5460).
  - Traffic sets `pusch_decode = 1`. An explicit value may only lower it: 0 stays 0, and 2 becomes 1 with
    `narrowed = true`.
  - Sensing is parsed and reserved. Until the sensing plan lands it resolves exactly like LEGACY, with a LOG_W.
  - In traffic mode the sense API is opened even when `enable = 0`, because DCI records need the sink.

- [ ] **Step 1: Write the failing tests** in `nr_isac_mode_test.cc`:
  - `LegacyPassesThrough`: `resolve(NULL, 0x5, 2, 1, &o)` gives mode LEGACY, sources 0x5, pdsch 2, pusch 1,
    narrowed false. Same for `""`.
  - `TrafficDefaults`: `resolve("traffic", 0, -1, -1, &o)` gives mode TRAFFIC, sources 0, pdsch 1, pusch 1,
    narrowed false.
  - `TrafficRefusesCfrSources`: `resolve("traffic", 1u<<NR_ISAC_SRC_PDSCH_DATA, 2, 2, &o)` gives sources 0, pdsch 1,
    pusch 1, narrowed true.
  - `TrafficKeepsExplicitZero`: `resolve("traffic", 0, 0, 0, &o)` gives pdsch 0, pusch 0, narrowed false.
  - `UnknownModeFails`: `resolve("radar", 0, -1, -1, &o)` returns false.
- [ ] **Step 2: Run them to verify they fail.**
  Run: `cd cmake_targets/ran_build/build && ninja test_nr_isac_mode && ./test_nr_isac_mode`.
  Expected: compile error (no `nr_isac_mode.h`).
- [ ] **Step 3: Implement `nr_isac_mode_resolve` and `nr_isac_mode`** as pure C in `nr_isac_mode.c`.
  - In `nr_isac_init` add `string("mode", &mode_str)` to `params`. Resolve it after `config_get`.
    - A bad string: LOG_E and stay LEGACY.
    - `narrowed`: one `LOG_W(PHY, "SENSING: mode=traffic narrowed sources/decode keys (no CFR in traffic mode)\n")`.
    - In TRAFFIC skip the `if (!enable) return;` exit, and replace `configured_sources` with the resolved value.
      Keep the `csi_rs` fallback for LEGACY only.
  - In nr_pdcch_blind_monitor.c, after it parses `pdcch_blind_monitor_pdsch` / `_ul_pusch`, apply the resolver's
    pdsch/pusch levels when `nr_isac_mode() == NR_ISAC_MODE_TRAFFIC`, and keep the rest of the colon fields.
  - **VERIFY** in that file that `nr_isac_init()` runs before the monitor reads its keys. If it doesn't, call the
    resolver from the monitor with the same inputs; it is pure.
- [ ] **Step 4: Run the tests to verify they pass.** Run the same command. Expected: 5 PASS.
- [ ] **Step 5: Build and run ctest**, using the build command from Global Constraints.
  Expected: only the known ARM failures. Legacy is unchanged because no existing cfg has `mode`.
- [ ] **Step 6: Commit**
  ```bash
  git add openair1/PHY/NR_UE_ISAC/nr_isac_mode.h openair1/PHY/NR_UE_ISAC/nr_isac_mode.c openair1/PHY/NR_UE_ISAC/nr_isac.cc openair1/PHY/NR_UE_ISAC/nr_isac.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c openair1/PHY/NR_UE_ISAC/tests/nr_isac_mode_test.cc CMakeLists.txt
  git commit -m "feat(twomodes): [sensing] mode key and pure resolver (traffic narrows CFR away; legacy unchanged)"
  ```

### Task 2: Non-blocking binary DCI record path and the four counters

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.c`:
  - `rs_job_t` (:91-96);
  - the writer loop (:209-220);
  - `nr_sense_push_rs_binary` (:462) as the pattern;
  - stats (:60-69 in `nr_sense_api.h`).
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.h`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_sense_api_test.cc` (existing; it uses the fault hooks
  `nr_sense_test_pause_writer`, `nr_sense_test_fail_writes`)

**Interfaces:**
- Consumes: `nr_sense_dci_rec_t` and `nr_sense_dci_to_json` from Task 3. Declare the formatter here as a weak symbol,
  exactly like `nr_sense_rs_to_json` (nr_sense_api.c:9).
- Produces:
  ```c
  /* 1 queued, 0 dropped (ring full / no sink: counted in dci_lost), -1 API not open. Never blocks beyond the per-cell mu. */
  int nr_sense_push_dci(const nr_sense_dci_rec_t *r);
  /* nr_sense_stats_t gains: uint64_t dci_generated, dci_persisted, dci_lost; */
  ```
- Mechanism:
  - `record_binary[at] = 2` marks a DCI job. Store it in a parallel `dci_jobs` array sized `rec_ring_slots`,
    allocated in `nr_sense_open` with the same overflow checks as `rs_jobs` (:267).
  - `dci_generated` is incremented on every call that reaches the ring (queued or dropped).
  - `dci_persisted` is incremented after a successful `writev` of a type-2 line.
  - `dci_lost` counts: ring full, no_sink, format failure, and a write error on a type-2 line.

- [ ] **Step 1: Write the failing tests**:
  - `DciQueuedAndPersisted`: open with records_path in a tmpdir. Push 10 records, close. The file has 10 lines,
    and the stats read `generated 10`, `persisted 10`, `lost 0`.
  - `DciRingFullCountsLost`: `rec_ring_slots = 4` and `nr_sense_test_pause_writer(true)`. Push 10, so 6 return 0.
    Then `generated 10`, `lost 6`; after unpausing and closing, `persisted 4`.
  - `DciWriteErrorCountsLost`: `nr_sense_test_fail_writes(true)`, push 3, close. Then `lost 3` and `persisted 0`.
- [ ] **Step 2: Run them to verify they fail.** `ninja test_nr_sense_api && ./test_nr_sense_api --gtest_filter='*Dci*'`.
  Expected: compile error (no `nr_sense_push_dci`).
- [ ] **Step 3: Implement it.** Only a `memcpy` of the struct goes under `mu`. JSON formatting stays on the writer
  thread.
- [ ] **Step 4: Run the tests to verify they pass.** All `*Dci*` tests PASS, and the existing tests in that binary
  still pass.
- [ ] **Step 5: Commit** `nr_sense_api.c nr_sense_api.h tests/nr_sense_api_test.cc` with the message
  `feat(twomodes): binary DCI record path + generated/persisted/lost counters`.

### Task 3: DCI record struct, JSON and emit function

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_traffic_record.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_traffic_record.c`.
  Add them to the `nr_sense_api` library sources in CMakeLists.txt, where `nr_sense_records.c` is listed.
- Modify: `nr_sense_records.c` (reuse `stamp()`, `nr_sense_capture_grant_stamp`, `nr_sense_grant_to_json`)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_traffic_record_test.cc` (new, registered like `test_nr_sense_records`)

**Interfaces:**
- Produces:
  ```c
  typedef enum { NR_LAYOUT_FIXED = 0, NR_LAYOUT_SOLVED, NR_LAYOUT_CANDIDATE, NR_LAYOUT_UNSOLVED } nr_layout_state_t;
  typedef enum { NR_DCI_DECODE_NONE = 0, NR_DCI_DECODE_QUEUED = 1 } nr_dci_decode_t;
  typedef struct {
    nr_passive_obs_t o;            /* existing grant keys, filled as the post-decode callers do (crc = -1 here) */
    nr_sense_grant_meta_t m;       /* existing meta keys */
    uint8_t dci_format;            /* nr_blind_dci_format_t / UL 0_0, 0_1 */
    int8_t ss_type, al; int16_t cce, coreset_id; uint16_t dci_len;
    uint8_t dci_bits[18];          /* raw payload, MSB-first, dci_len bits (<= 140) */
    bool rnti_confirmed;           /* nr_pdcch_blind_monitor_rnti_confirmed() at accept */
    nr_layout_state_t layout_state; uint32_t layout_checks_ok, layout_checks_fail;
    uint32_t cfg_ref_ver;          /* with m.identity_gen + m.dedicated_epoch = cfg_ref */
    int16_t bwp_start, bwp_size, start_rb_bwp;  /* -1 unknown */
    int8_t vrb_prb, tda_index, k0, k1, dai, tpc, pri, srs_req, csi_req, ulsch_ind, cbgti, retx; /* -1 unknown */
    int8_t si_ind, short_msg_ind; int16_t short_msg; int8_t tb_scaling;   /* P/SI/RA only, -1 otherwise */
    int64_t pdsch_abs_slot, harq_fb_abs_slot;  /* -1 unknown */
    nr_dci_decode_t decode;
  } nr_sense_dci_rec_t;
  int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n);   /* writer-thread formatter, pure */
  void nr_sense_emit_dci(nr_sense_dci_rec_t *r);   /* stamps epoch/identity/t_mono like nr_sense_emit_grant_meta, then nr_sense_push_dci */
  /* outcome of a probe/check decode: {"type":"decode","abs_slot","rnti","dir","probe","crc","snr_db","learned":...} */
  void nr_sense_emit_decode(uint64_t abs_slot, uint16_t rnti, int dir, bool probe, int crc, float snr_db, const char *learned);
  ```
- JSON:
  - It is the exact output of `nr_sense_grant_to_json(&r->o, &r->m, ...)` with `"type":"grant"` replaced by
    `"type":"dci"`, plus the new keys, in the field order of the struct.
  - `dci_bits` is lowercase hex of `ceil(dci_len/8)` bytes.
  - `cfg_ref` prints as the string `"<identity_gen>.<dedicated_epoch>.<cfg_ref_ver>"`.
  - Every int field holding -1 prints `null`.
  - **Null rule**: when `layout_state` is CANDIDATE or UNSOLVED, every layout-dependent key prints null: start_rb,
    nb_rb, start_sym, nb_sym, mcs, mcs_table, qm, nl, tbs, harq_pid, ndi, rv, vrb_prb, tda_index, k0, k1, dai, tpc, pri,
    retx, pdsch_abs_slot, harq_fb_abs_slot.

- [ ] **Step 1: Write the failing tests**:
  - `DciJsonHasEveryGrantKey`: take the key list of `nr_sense_grant_to_json` for the same o/m, and check that
    every key appears in the DCI JSON and that `"type":"dci"` is present.
  - `DciJsonNewKeys`: a solved 1_1 fixture checks `dci_format`, `al`, `cce`, `dci_len`, `dci_bits` hex,
    `rnti_confirmed`, `layout_state:"solved"`, `cfg_ref:"3.7.2"`, `bwp_start`, `start_rb_bwp`, `k1`, `pri`, `tpc`.
  - `UnsolvedNullsLayoutFields`: the same fixture with UNSOLVED prints `"mcs":null,"nb_rb":null,"k1":null` and keeps
    `dci_bits`, `rnti`, `al`.
  - `DecodeRecordShape`: `nr_sense_emit_decode` writes one line with `"type":"decode"` and the given abs_slot, rnti
    and crc.
  - `DciJsonFitsRecordBytes`: an all-fields fixture formats to less than `RECORD_BYTES - 1`.
- [ ] **Step 2: Run them to verify they fail.** `ninja test_nr_traffic_record && ./test_nr_traffic_record`.
  Expected: compile error.
- [ ] **Step 3: Implement it.** Reuse `nr_sense_grant_to_json`, then trim its closing `}` and append the new keys.
- [ ] **Step 4: Run the tests to verify they pass.** Expected: 5 PASS.
- [ ] **Step 5: Commit** the files above plus CMakeLists.txt, with the message
  `feat(twomodes): DCI record struct, JSON (grant keys + DCI keys, null rule), decode record`.

### Task 4: DCI field extraction (pure)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_traffic_dci_fields.h`, `nr_traffic_dci_fields.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_traffic_dci_fields_test.cc`

**Interfaces:**
- Consumes: `nr_dci_bits_t` and `read_field` semantics (MSB-first) as in nr_pdcch_blind_monitor.c:4146;
  `nr_dci11_layout_t` (nr_pdcch_dci11_layout_sweep.h:87).
- Produces:
  ```c
  typedef struct { int8_t dai, tpc, pri, k1; } nr_dci_harq_fields_t;  /* -1 unknown */
  /* DCI 1_0 C-RNTI/TC-RNTI (TS 38.212 7.3.1.2.1): identifier, FDRA(fdra_bits), TDRA 4, VRB 1, MCS 5, NDI 1, RV 2, HARQ 4, DAI 2, TPC 2, PRI 3, K1 3. */
  bool nr_dci10_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, uint16_t fdra_bits, nr_dci_harq_fields_t *out);
  /* DCI 1_1: the fields inside the pre_ant group are only knowable when exactly one split of l->pre_ant into
     tb2{0,8} + harq{4,5} + dai{0,2,4,6} + tpc 2 (+2nd tpc{0,2}) + pri 3 + k1{0,1,2,3} + rel16/17 extras{0} exists
     under the layout's other switches. Returns false (out all -1) when not unique. */
  bool nr_dci11_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, const nr_dci11_layout_t *l,
                            const nr_dci11_offsets_t *off, nr_dci_harq_fields_t *out);
  ```
- Conflict found `[CODE]`: a solved DCI 1_1 layout knows only group sums (nr_pdcch_dci11_layout_sweep.h:80-103,
  "only their sum is visible"). HARQ pid, DAI, TPC, PRI and K1 of 1_1 are therefore often not parseable. They print
  null, and `dci_bits` with `cfg_ref` keeps them recoverable offline once the split is known. Report this at the
  checkpoint.
- UL 0_1/0_0: `nr_pdcch_blind_ul_result_t` already carries tpc, dai, srs_request, csi_request, ulsch_indicator
  (nr_pdcch_blind_monitor.h:784-789). Copy them into the record; nothing new to extract.

- [ ] **Step 1: Write the failing tests**:
  - `Dci10HarqFields`: build a 1_0 payload (fdra_bits 15 for 217 PRB) with DAI=2, TPC=1, PRI=5, K1=3, and check
    that the extracted values are equal.
  - `Dci10TooShortFails`: `dci_len` below the sum of the field widths returns false.
  - `Dci11UniqueSplitParses`: `pre_ant = 4+2+2+3+3 = 14`, i.e. tb2 0, harq 4, dai 2, tpc 2, pri 3, k1 3, with the
    values set in the payload. It returns true and the values are equal. **VERIFY** first that 14 is unique under
    the enumerated switch tables (`kTb2`, `kHarq`, `kDai`, `kTpc2`, `kP2H` in nr_pdcch_dci11_layout_sweep.c:90-125);
    if it isn't, pick a pre_ant the tables make unique, and put it in the test.
  - `PreAntAmbiguousGivesNull`: a `pre_ant` with two or more splits returns false, with every out field -1.
- [ ] **Step 2: Run them to verify they fail.** Expected: compile error.
- [ ] **Step 3: Implement it.** Enumerate the splits from the sweep's own switch tables. Do not copy them: expose
  them through a small accessor in nr_pdcch_dci11_layout_sweep.c if they are static.
- [ ] **Step 4: Run the tests to verify they pass.** Expected: 4 PASS.
- [ ] **Step 5: Commit** with the message `feat(twomodes): pure DCI 1_0/1_1 HARQ-field extraction (null when the 1_1 split is ambiguous)`.

### Task 5: Configuration references and config records

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_traffic_cfg.h`, `nr_traffic_cfg.c`
- Create: `tools/traffic/dci_reinterpret.py`, `tools/traffic/test_dci_reinterpret.py`, `tools/traffic/fixtures/cfgref_1_0.jsonl`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_traffic_cfg_test.cc`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
    uint32_t identity_gen, dedicated_epoch;
    int16_t coreset_id, coreset_start_rb, coreset_n_rb; int8_t coreset_dur;
    int16_t bwp_start, bwp_size;
    int16_t layout_rep;              /* nr_pdsch_solver_rep(); -1 none */
    nr_dci11_layout_t layout;        /* zero when layout_rep < 0 */
    int8_t s_by_tda[16], l_by_tda[16], dmrs_add_pos, mcs_table, rm_set;
  } nr_traffic_cfg_snap_t;
  /* Per cell, per RNTI (rnti 0 = cell-wide). Returns the version for this snapshot; on change bumps it and
     emits one {"type":"config","rnti","cfg_ref", every snapshot field} record. Rare by construction. */
  uint32_t nr_traffic_cfg_ref(uint16_t rnti, const nr_traffic_cfg_snap_t *s);
  void nr_traffic_cfg_reset(void);   /* on HARD_RESET epochs, from the monitor's epoch listener */
  ```
- Storage: a fixed table of 256 RNTIs per cell (`NR_CELL_STATIC`), LRU. It holds a 64-bit FNV-1a of the snapshot
  bytes and a version.
- Periodicity: when `nr_periodic_track.c` confirms an SPS/CG period, that period is added to the RNTI's snapshot as
  `int16_t period_slots` (-1 when none).

- [ ] **Step 1: Write the failing tests**:
  - `SameSnapshotSameVersion`: two calls with an identical snapshot return the same version, and only one config
    record is written.
  - `ChangeBumpsVersion`: changing `bwp_size` returns version+1 and writes a second config record.
  - `ResetRestartsVersions`.
  - `test_dci_reinterpret.py::test_1_0_fields_from_config`: the fixture holds 1 config record (bwp_size 217) and 1
    DCI record (1_0, `dci_bits`, cfg_ref pointing to it). The tool re-parses FDRA/TDRA/MCS/K1 from the bits with that
    bwp_size, and gets the record's own values.
- [ ] **Step 2: Run them to verify they fail.** `./test_nr_traffic_cfg` and `python3 -m pytest tools/traffic -q`.
  Expected: fail.
- [ ] **Step 3: Implement it.** `dci_reinterpret.py` handles 1_0 and 0_0 only, which are fixed layouts. 1_1 needs
  the layout from the config record: `shortcut: 1_1 offline re-parse not implemented, add when the pre_ant split
  is solvable offline`.
- [ ] **Step 4: Run the tests to verify they pass.**
- [ ] **Step 5: Commit** the files above, with the message `feat(twomodes): versioned cfg_ref + config records; offline 1_0/0_0 reinterpretation tool`.

### Task 6: Traffic decode policy

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_traffic_decode.h`, `nr_traffic_decode.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c/.h` (add `nr_pdsch_solver_reopen`,
  `nr_pdsch_solver_layout_state`, `nr_pdsch_solver_take_hrv_alert`)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_traffic_decode_test.cc`; extend `tests/nr_pdsch_solver_test.cc`

**Interfaces:**
- Produces:
  ```c
  /* dir 0 = DL, 1 = UL. Non-probe decodes only; solver probes (job.layout_probe) never ask. */
  bool nr_traffic_decode_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls,
                                bool ul_table_learned, uint64_t t_ns, bool hrv_alert);
  /* returns true when the RNTI must be reopened (K_REOPEN consecutive counted failures) */
  bool nr_traffic_decode_result(int dir, uint16_t rnti, int rv, float snr_db, bool crc_ok);
  /* solver additions */
  void nr_pdsch_solver_reopen(uint16_t rnti);          /* same effect as the 256-grant reopen (nr_pdsch_solver.c:1910-1916) */
  nr_layout_state_t nr_pdsch_solver_layout_state(uint16_t rnti, uint32_t *checks_ok, uint32_t *checks_fail);
  bool nr_pdsch_solver_take_hrv_alert(uint16_t rnti);  /* set where the HARQ/RV consistency check counts a violation (~:2015) */
  ```
- Rules (the spec decides these):
  - `rnti_class != C`: always true. SI/RA/P/TC are decoded exactly as in legacy.
  - DL, `ls == FIXED` (1_0 C-RNTI): false, since the layout and table are known.
  - DL, `ls == SOLVED`: true when `hrv_alert`, or when this RNTI has had at least `CHECK_EVERY_GRANTS` grants since
    its last check, or at least `T_CHECK_NS` since its last check.
  - DL, CANDIDATE/UNSOLVED: false. The solver's probes do the work.
  - UL: true while `!ul_table_learned`, then the same cadence as DL SOLVED.
- **Decision (design, within spec §3.4)**: a failure counts toward `K_REOPEN` only when `rv == 0` and
  `snr_db >= 10.0f` (`TRAFFIC_FAIL_MIN_SNR_DB`, the same high-SNR idea as fix1102/ul-uci). Any pass resets the count.
- Table: 128 RNTIs per cell and direction, `NR_CELL_STATIC`, LRU.

- [ ] **Step 1: Write the failing tests**:
  - `NonCRntiClassesAlwaysDecoded`: SI, RA, P and TC each return true.
  - `FixedLayoutNotDecoded`.
  - `SolvedChecksEvery32Grants`: 31 calls return false and the 32nd returns true; after a result, the count restarts.
  - `SolvedChecksAfter2s`: one false, then `t + 2 s` gives true.
  - `HrvAlertForcesCheck`.
  - `ReopenAfter4CountedFails`: 4 fails with rv 0 at 15 dB; the 4th returns true.
  - `LowSnrFailDoesNotCountTowardReopen`: 10 fails at 5 dB never return true.
  - `PassResetsFailCount`: 3 fails, 1 pass, then 3 fails: false.
  - `UlDecodesUntilTableLearned`.
  - Solver test `ReopenClearsSolution`: solve a fixture RNTI, call `nr_pdsch_solver_reopen`, and
    `layout_state == UNSOLVED`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement it.** Under `g_mu`, `nr_pdsch_solver_reopen` performs the same state change as the
  existing reopen at :1910-1916. `layout_state` maps `solved`, `has_cand` and `used` onto the enum.
- [ ] **Step 4: Run the tests to verify they pass.**
- [ ] **Step 5: Commit** with the message `feat(twomodes): traffic decode policy (probes + rare checks, K=4 high-SNR reopen) and solver hooks`.

### Task 7: Wire the accept points, decode gate and counters

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`:
  - DL accept right after `NR_BLIND_CTR_INC(g_accepts)` (:8634);
  - UL 0_1 after `NR_BLIND_CTR_INC(g_ul_accepts)` (:8319);
  - UL 0_0 after `NR_BLIND_CTR_INC(g_ul00_accepts)` (:8451);
  - the decode gate at the grant-drop block (~:9850-9860) and the enqueue at ~:9708/:9910.
- Modify: `nr_pdsch_passive_queue.c:2269-2305`, `nr_pdcch_blind_monitor_rt.c:10130-10156`, and
  `nr_pusch_passive_decode.c:3329-3365`. In traffic mode these call `nr_sense_emit_decode` (probes included, with
  `probe` set) instead of `nr_sense_emit_grant_meta`, then `nr_traffic_decode_result`. When that returns true they
  call `nr_pdsch_solver_reopen`.
- Modify: `nr_passive_metrics_json.c` / `nr_passive_metrics.c`. Add the per-cell ISAC_METRICS keys
  `dci_accepted_{10,11,00,01}`, `dci_state_{fixed,solved,cand,unsolved}`, `dci_rec_generated`,
  `dci_rec_persisted`, `dci_rec_lost`, `check_decodes`, `check_fail`, `probe_decodes`, `reopens`, and the
  accept-path timing `dci_emit_us_p50`/`p99`.
- Test: extend `tests/nr_passive_metrics_test.cc` (the keys are present and null-safe).

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Steps at each accept point, only when `nr_isac_mode() == NR_ISAC_MODE_TRAFFIC`:
  1. fill `nr_sense_dci_rec_t` from `out` (DL) or `u`/`ul00` (UL) and from `cand_task[ti]` (al, cce, dci_length);
  2. set `rnti_confirmed = nr_pdcch_blind_monitor_rnti_confirmed(abs_slot, rnti)`;
  3. take the layout state from the solver (1_0 and 0_0 are FIXED; 0_1 is SOLVED when the UL layout lock holds,
     otherwise UNSOLVED);
  4. take the HARQ fields from Task 4;
  5. `cfg_ref_ver = nr_traffic_cfg_ref(...)`;
  6. compute `retx` from a per-RNTI, per-pid last-NDI table (null after an epoch change);
  7. `nr_sense_emit_dci`;
  8. increment `dci_accepted_<fmt>` in the same block.
- Decode gate in traffic mode: for a non-probe job, `nr_traffic_decode_wanted(...) == false` means no enqueue
  (counted as `traffic_skip`, not `grantdrop`) and `decode = NONE` on the record. Probes and wanted decodes enqueue
  exactly as in legacy.
- **VERIFY** before coding:
  - (a) whether the solver's probe jobs pass through the `want_decode` grant-drop lines at ~:9855. If they do, the
    traffic gate must sit after the probe branch.
  - (b) that CSI-RS acquisition (rate matching for probes) does not depend on the `csi_rs` source bit being set,
    since traffic sets `sources = 0`. If it does, keep acquisition and gate only the CFR export.
  - (c) that DL dedup across passes (UL has `accept_dup`) means one accept per DCI. If DL accepts can repeat for one
    DCI, use `accept_dup` the same way before the emit, and count dups separately.

- [ ] **Step 1: Write the failing metrics test** `TrafficKeysPresent`: an emitted metrics line contains every new
  key.
- [ ] **Step 2: Run it to verify it fails.**
- [ ] **Step 3: Implement the wiring.** In legacy mode the added code must be unreachable: one `nr_isac_mode()`
  branch per site.
- [ ] **Step 4: Build and run ctest.** Expected: only the known ARM failures.
- [ ] **Step 5: Legacy gate.** With the build idle and under `flock -x /tmp/td_measure.lock`, run
  `tests/passive_rx/dgx/rfsim_regress.sh` with its defaults and check it passes the 4-RX gate thresholds in
  CLAUDE.md. Because the gate runs the legacy path, this proves legacy is unchanged.
- [ ] **Step 6: Commit** with the message `feat(twomodes): emit DCI records at accept, traffic decode gate, decode records, ISAC_METRICS traffic keys`.

### Task 8: Traffic configs and replay A/B

**Files:**
- Create: `tests/passive_rx/traffic/legacy_replay_415.cfg`. Copy it from
  `/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/a4c74c4f-0233-406b-99f4-58eb121db97b/scratchpad/legacy_replay_415_fusedJ.cfg`
  (the mpci-main replay config in use on 2026-11-03), without its `device`/`loader` lines (csd_replay.sh appends
  those), and with `enable = 1;` and `records_path`.
- Create: `tests/passive_rx/traffic/traffic_replay_415.cfg`, the same file plus `mode = "traffic";`.
- Create: `tests/passive_rx/traffic/ab_replay.sh`. It alternates the arms L T L T L T over the two captures, using
  `/home/nicola/NICOLA/wt/rr-orchestration/csd_replay.sh` with `BASECFG=<cfg>`,
  `CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_4s_1102_0951` (and `..._923_...`), `CAP_SAMPLES=491520000`, and
  `CAP_TICK` taken from that capture's `capture_info.json` `first_sample_tick` (194905280 for 415).
  - For 923, use centre 3759990000 and its SSB; csd_replay.sh hardcodes 415's `-C`/`--ssb`. Add a
    `CENTER`/`SSB_ARFCN` override to a copy in this directory, not to the orchestration script.
- Create: `tools/traffic/score_traffic_ab.py` with a test `tools/traffic/test_score_traffic_ab.py`.

**Interfaces:**
- The scorer reads `run.log` ISAC_METRICS lines (the last line per cell) and the records file of each run. For each
  arm and cell it prints the mean and sd of: `dci_accepted_*`, `dci_rec_generated`, `dci_rec_lost`, solved RNTIs
  (distinct RNTIs with `layout_state == solved`), `scanq_drop_*`, process CPU (from the run's `/proc` sample or
  `CELL_CPU`), and `dci_emit_us_p99`.
- Field agreement: for each (abs_slot, rnti) that is solved in T and has a legacy post-decode grant record in L, it
  compares start_rb, nb_rb, mcs and tbs, and prints the agreement %. Runs are different, so it matches only identical
  (abs_slot, rnti) pairs and reports how many were compared.
- Pass bounds (spec §3.5, all `[OFFLINE VERIFIED, DGX replay]`, per capture, never pooled):
  - `dci_accepted == dci_rec_generated` exactly;
  - `dci_rec_lost == 0`;
  - solved RNTIs: T mean >= 90 % of L mean;
  - `scanq_drop_*` T mean <= L mean + L sd;
  - CPU T < L;
  - field agreement >= 99 % on the compared pairs.

- [ ] **Step 1: Write the scorer test.** `test_score_traffic_ab.py::test_bounds` takes two synthetic arms of 3 runs
  each and checks the pass/fail output for each bound, including a failing `dci_rec_lost`.
- [ ] **Step 2: Run it to verify it fails**, then implement `score_traffic_ab.py` until it passes.
- [ ] **Step 3: Run the A/B.** Wait until `pgrep -x nr-uesoftmodem` is empty (another session replays on this
  host), then run `bash tests/passive_rx/traffic/ab_replay.sh`. It takes the heavy lock exclusively through
  csd_replay.sh. Expected: 12 run directories under `/home/nicola/NICOLA/ota_runs/replay_tm*`.
- [ ] **Step 4: Score.** `python3 tools/traffic/score_traffic_ab.py /home/nicola/NICOLA/ota_runs/replay_tm*`.
  Record the table verbatim in `docs/superpowers/reports/twomodes/TRAFFIC-REPLAY-20261103.md`, with labels, the
  build commit, the capture names and every failed bound. Do not tune anything to pass.
- [ ] **Step 5: Frozen check.** `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`
  exits 0.
- [ ] **Step 6: Commit** the cfgs, the scripts, the scorer and its test, and the report, with the message
  `test(twomodes): traffic-mode replay A/B configs, scorer, report`.
- [ ] **Step 7: Operator checkpoint.** Stop here. Report the table, the 1_1 pre_ant finding (Task 4), and every
  VERIFY outcome. Sens6 and NSA OTA (spec §10 step 2) wait for operator approval.

## Self-review notes

- Spec coverage:
  - §2 is covered by Task 1.
  - §3.3 is covered by Tasks 2, 3, 4, 5 and 7: four counters, the non-blocking path, dci_bits, cfg_ref with config
    records, layout_state, BWP-relative fields, retx, HARQ timing, periodicity.
  - §3.4 is covered by Tasks 6 and 7.
  - §3.5.1-2 are covered by Tasks 1-8.
  - §3.5.3-4 (sens6, OTA) are deferred to after the checkpoint, as the spec orders.
- Conflicts surfaced for the operator:
  - Task 4: 1_1 pre_ant fields are often null.
  - Task 7 VERIFY (a-c).
  - Task 8: the 923 replay needs a centre/SSB override.
