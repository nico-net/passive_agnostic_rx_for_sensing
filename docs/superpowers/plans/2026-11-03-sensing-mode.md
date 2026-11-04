# Sensing Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** add `[sensing] mode = "sensing"`. It produces continuous, quality-scored, wideband (>= 128 valid PRB) CFR
records from one or more PCIs. A slow scheduler switches (PCI, message type, RNTI) units on or off. Every message of a
unit that is on is processed fully.

**Architecture:**
- **Protected baseline.** Sensing mode runs on top of traffic mode: DCI records, solver probes and check decodes.
  SSB and confirmed CSI-RS are always on.
- **Quality.** Per-PRB CFR quality (SNR, local-fit residual, inter-symbol consistency) is computed once, centrally, in
  the CFR push path. `nr_sense_emit_rs` attaches it to the record through the CFR frame id. Data-aided records add a
  reconstruction confidence.
- **Cost.** Each unit's CPU cost is measured per job, in 50 ms buckets.
- **Scheduler.** A pure decision function runs every 4 s in one process-wide thread, and works for N >= 1 cells. Its
  output is a per-cell atomic enable table, checked at the existing gate sites before any work starts.

**Tech Stack:** C11, C++17 gtest, CMake/Ninja, Python 3 (offline scorer).

**Spec:** `docs/superpowers/specs/2026-11-03-two-modes-design.md` (§4, §5, §6, §7, §8, §9, §10 steps 3-7).

**Depends on:** the traffic-mode plan (`docs/superpowers/plans/2026-11-03-traffic-mode.md`), which must be complete and
past its operator checkpoint. From it this plan uses:
- `nr_isac_mode()` and `nr_isac_mode_resolve()` (Task 1);
- `nr_sense_emit_dci` (Task 3);
- `nr_traffic_cfg_ref` (Task 5);
- `nr_traffic_decode_wanted` and `nr_traffic_decode_result` (Task 6).

## Global Constraints

- Everything in the traffic plan's Global Constraints applies unchanged. In short: no new env, CLI or -D flag; legacy
  unchanged when no `mode` key; sens6 files frozen and new configs are `.cfg`; builds under `flock -s heavy.lock` and
  never while `pgrep -x nr-uesoftmodem` finds a process; all module `.so` files present; SIGINT only; explicit
  `git add`; no push; portable code that also compiles on sens6 (x86, sm_89).
- Sensing stays off the GPU. The quality, cost and scheduler code is CPU-only. The GPU LDPC inside a data-aided
  decode is decode work, not sensing.
- New `.cfg` key: `[sensing] sense_min_prb`, an integer, default `128`.
- A record is wideband when `valid_cfr_prbs >= sense_min_prb`.
- Spec constants:
  - bucket `SENSE_BUCKET_NS = 50000000`;
  - decision period `SENSE_DECIDE_NS = 4000000000`;
  - streak `SENSE_CONFIRM = 2`;
  - cost window `SENSE_WIN_DECISIONS = 4`;
  - headroom `SENSE_HEADROOM = 0.40`;
  - hysteresis margin `SENSE_HYST = 0.20`;
  - `SENSE_MAX_ENABLES = 2`;
  - age cap `SENSE_AGE_CAP = 2.0` (multiplier), reached at `SENSE_AGE_FULL_NS = 10000000000`.
  All are compile-time constants in `nr_sense_sched.h`.
- Never select per grant inside an enabled unit. A drop inside an enabled unit is a **violation**, not normal
  operation.
- Disabled units are gated **before** work starts, and are counted as `disabled_skips`, never as failures.
- Single PCI must work. The scheduler runs at N = 1, and PCI-level fairness has nothing to choose between.
- PDCCH monitoring stays on for every PCI. Scan suspension is not part of this plan (spec §7).
- Data-aided PDSCH from an enabled unit decodes every grant of that RNTI to the end. Never trade CRC for latency.
- Metrics are per PCI and per link. The aggregate goes in its own line. Never pool across cells, hosts or bandwidths.
- No real-time claim without measurement. Every reported number carries a PROJECT_MEMORY §0.1 label.

## Review Focus

1. **A carrier narrower than `sense_min_prb`.** Expect: one log line at start; no record is wideband; the scheduler
   enables nothing wideband; SSB/CSI-RS keep running. (Test: Task S6, `NarrowCarrierEnablesNothing`.)
2. **A unit whose RNTI disappears (UE idle).** Expect: it is not eligible, its age stops accumulating, it is disabled by
   hysteresis, and it never takes capacity. (Test: Task S6, `IdleUnitGetsNoBoostAndAges0`.)
3. **A queue-full drop inside an enabled unit.** Expect: it counts as a violation, and the next decision removes the
   lowest-value unit at once without waiting for the streak. (Test: Task S6, `ViolationRemovesLowestImmediately`.)
4. **The first decisions, when no history exists.** Expect: at most one new unit per decision, using the source's
   per-cell p99 as its estimate. (Test: Task S6, `ColdStartOneUnitPerDecision`.)
5. **A CFR with fewer than 12 REs in a PRB, or `noise_var <= 0`.** Expect: that PRB is invalid, and the record still
   formats with a valid mask. (Test: Task S2, `DegeneratePrbInvalid`.)

---

### Task S1: Sensing mode resolution, `sense_min_prb`, discovery skip recovery

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac_mode.c/.h` (traffic plan Task 1), `nr_isac.cc` (`sense_min_prb` key)
- Modify: `openair1/PHY/TOOLS/nr_admit.{c,h}` (attempt counter, backlog), the JANON site
  (`nr_pdcch_blind_monitor_rt.c` janon_should_run / janon_worker) and the DMRS_ID site (`nr_pdsch_passive_queue.c` cdmrs_run)
- Test: `openair1/PHY/NR_UE_ISAC/tests/nr_isac_mode_test.cc`; `nr_admit_test.cc` (create it if no nr_admit test exists)

**Interfaces:**
- Produces:
  - `nr_isac_mode_cfg_t` gains `int min_prb`;
  - `int nr_isac_sense_min_prb(void)` (default 128);
  - per-class counters: attempts, skipped, recovered, expired (in `nr_admit_stats` and ISAC_METRICS, all modes).
- Sensing resolution (spec §4.1):
  - `sources = ssb | csi_rs | pdsch_dmrs | pdsch_data | pusch_dmrs | srs`, plus `prach` only when explicitly set.
  - `pdsch_decode = 2` (data-aided export).
  - `pusch_decode = 1`.
  - An explicit key can narrow this, never widen it.
- **Decision (operator 2026-11-04, replaces "never shed outside legacy")**: every mode keeps today's cumulative
  shed tiers, unchanged. In addition:
  - Count attempts per class, so the skip *rate* is visible (today only skips are counted).
  - JANON and DMRS_ID items that are skipped are not freed. They go into a bounded per-cell backlog (small fixed
    depth; when it is full, drop the oldest and count it as expired). The backlog is replayed when
    `nr_pool_site_shed_tiers()` is 0 (no overload). An item past its age limit is dropped and counted as expired;
    JANON uses its existing JANON_FRESH_NS (1 s), DMRS_ID uses the same 1 s unless the code shows a tighter validity.
    Replay never runs while shedding, so it adds no CPU under overload.
  - NID: no change (acquisition is never shed; only rechecks are, and those are superseded by newer samples).
  - CSI-RS: VERIFY whether the `deferred` path in `nr_csirs_disc_feed.c` keeps the drained data on a skip. If it
    drops it, report this and do not change it in S1.
  - Single-PCI behaviour is unchanged (admission is off below 2 active cells).

- [ ] **Step 1: Write the failing tests**:
  - `SensingDefaults`: `resolve("sensing", 0, -1, -1)` gives the sources mask above, pdsch 2, pusch 1, min_prb 128.
  - `SensingNarrowedByExplicitSources`: an explicit `ssb|pdsch_dmrs` stays exactly that.
  - `MinPrbKey`: 64 is kept, and 0 or a negative value falls back to 128 with an error.
  - `AdmitTiersUnchanged`: the tier table and cumulative shedding are the same as before, in every mode.
  - `AdmitCountsAttempts`: attempts include admitted and skipped calls.
  - `BacklogReplaysWhenIdle`: a skipped item is replayed once shedding is 0 and counted as recovered.
  - `BacklogExpiresOld`: an item older than its limit is dropped and counted as expired; a full backlog drops the oldest.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement it.** The sensing branch of the resolver, the key parse, the counters, and the backlog with replay.
- [ ] **Step 4: Run the tests, then the full build and ctest.** Expected: only the known ARM failures.
- [ ] **Step 5: Commit** with the message `feat(sense): mode=sensing resolution, sense_min_prb key, discovery skip backlog and counters`.

### Task S2: CFR quality (pure)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_quality.h`, `nr_sense_quality.c` (in the `nr_sense_api` library)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_sense_quality_test.cc`

**Interfaces:**
- Produces:
  ```c
  #define NR_SENSE_MAX_PRB 275
  typedef struct {
    uint16_t allocated_prbs, estimated_usable_prbs, valid_cfr_prbs;
    uint8_t valid_mask[(NR_SENSE_MAX_PRB + 7) / 8];  /* bit p = PRB (k_abs/12 - first_prb) valid */
    int16_t first_prb;
    float snr_db_p50, snr_db_p10;    /* over valid PRBs; NAN when none */
    float resid_db;                  /* median per-PRB local-fit residual / signal, dB; NAN when none */
    float sym_consist;               /* 0..1 correlation of per-symbol estimates on shared k; NAN with < 2 symbols */
    float recon_conf;                /* data-aided only, set by the producer; NAN otherwise */
  } nr_sense_quality_t;
  /* iq antenna-major complex float, as nr_sense_push_cfr. Pure, no allocation, O(n_ant * n_re). */
  void nr_sense_quality_compute(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                                const uint8_t *l_sym, float noise_var, nr_sense_quality_t *q);
  ```
- Per PRB, the algorithm decides:
  - **SNR**: mean |H|² over antennas and REs, divided by `noise_var`.
  - **Residual**: fit H on the PRB's REs of one symbol with a first-order (linear in k) model, per antenna. The
    residual is the mean |H - fit|² divided by the mean |H|².
  - **Precoding boundaries (operator 2026-11-04)**: DM-RS CFR is the precoded channel H·W, and W may change at every
    PRG boundary (PRB bundling: 2, 4 PRB or wideband, learned per RNTI by the PRG sweep in nr_pdsch_passive_decode.c).
    No S2 computation may fit, smooth or correlate across a PRG boundary. The per-PRB fit above already holds this,
    because PRG boundaries are PRB boundaries. Any future multi-PRB step must use the learned PRG size, and must
    assume the smallest PRG (2 PRB) when the size is unknown, never wideband.
  - **Valid** when all of these hold:
    - SNR >= `NR_SENSE_PRB_SNR_MIN_DB` (3.0);
    - residual <= `NR_SENSE_PRB_RESID_MAX` (0.25);
    - REs in the PRB >= 3;
    - `noise_var > 0`.
  - These constants are provisional. They may be set only from general principles or from the sens6 lab with known
    references (spec §9.1), never from the DEIB/OTA cells (operator 2026-11-03).
- `sym_consist`: the normalised correlation between the estimates of the two most populated symbols over the k they
  share.
- `allocated_prbs` = the distinct PRBs touched by `k_abs`. `estimated_usable_prbs` = `allocated_prbs` here; producers
  may lower it. `shortcut: usable = allocated until producers report rate-matched PRBs; lower it per producer when
  the rate-matching set is known`.

- [ ] **Step 1: Write the failing tests.** Use synthetic CFRs from a known channel: a flat channel with a 2-tap
  delay, plus AWGN at a set SNR.
  - `CleanChannelAllValid`: 20 dB SNR, 128 PRB, 2 symbols, 4 antennas. `valid_cfr_prbs` = 128, `snr_db_p50` is
    within 1 dB of 20, `sym_consist` > 0.9.
  - `NoiseOnlyNoneValid`: noise only, so `valid_cfr_prbs` = 0 and `snr_db_p50` is NAN.
  - `HalfBandFaded`: PRBs 0-63 at 20 dB and 64-127 at -10 dB, so valid = 64 and the mask bits equal the truth.
  - `ResidualFlagsCorruptPrb`: in a clean CFR, randomise the phases in 10 PRBs, so those 10 are invalid through the
    residual alone, with their SNR still high.
  - `DegeneratePrbInvalid`: a PRB with 2 REs, and a separate call with `noise_var = 0`. Both invalid, and no NaN is
    written into the mask.
  - `SingleSymbolConsistNan`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement `nr_sense_quality_compute`.**
- [ ] **Step 4: Run the tests to verify they pass.** Add a micro-benchmark line printed by the test: ns per call at
  128 PRB x 4 antennas x 2 symbols. Copy it to the report (spec §4.2 VERIFY: cost).
- [ ] **Step 5: Commit** with the message `feat(sense): pure per-PRB CFR quality (SNR, local-fit residual, symbol consistency, valid mask)`.

### Task S3: Quality and link context in CFR records

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.c`:
  - `push_cfr`: compute the quality in SENSING mode only, and store it in a per-cell ring indexed by
    `frame_id % cfr_ring_frames`;
  - export `bool nr_sense_quality_for_frame(uint64_t frame_id, nr_sense_quality_t *q)`.
- Modify: `nr_sense_records.h` (`nr_sense_rs_meta_t` gains `nr_sense_quality_t q; bool q_valid; int dir; int ssb_beam;
  uint32_t cfg_ref_ver;`) and `nr_sense_records.c` (the `nr_sense_emit_rs` lookup, plus the JSON in
  `nr_sense_rs_to_json`).
- Modify: `nr_pdsch_data_aided.c` (set `q.recon_conf`: the share of data REs whose re-encoded symbol equals the
  hard decision of the equalised symbol, computed where the re-encoded symbols are already compared, ~:507).
- Test: `tests/nr_sense_records_test.cc`, `tests/nr_sense_api_test.cc`

**Interfaces:**
- Precoding context (operator 2026-11-04): every DM-RS CFR record carries `prg_size` (2, 4, 0 = wideband, null =
  unknown) from the PRG sweep, so post-processing never treats a CFR as one channel across PRGs.
- Consumes: S2.
- JSON keys added to RS records:
  - `allocated_prbs`, `estimated_usable_prbs`, `valid_cfr_prbs`, `valid_mask` (hex), `first_prb`;
  - `snr_db_p50`, `snr_db_p10`, `resid_db`, `sym_consist`, `recon_conf`, `wideband` (bool:
    `valid_cfr_prbs >= nr_isac_sense_min_prb()`);
  - `dir` ("dl" or "ul"), `tx` (the PCI for DL; `"rnti:<hex>"` for UL), `ssb_beam`, `cfg_ref`.
  - The existing `layer_idx1`, `n_layers` and `port_mask` stay.
- When `q_valid` is false, legacy runs and records without a CFR print none of these keys. Legacy records are
  byte-identical to today's.

- [ ] **Step 1: Write the failing tests**:
  - `QualityKeysInSensingMode`: push a synthetic CFR with mode=SENSING and emit the RS record; the JSON has
    `valid_cfr_prbs`, `wideband`, `resid_db`, `dir`, `tx`.
  - `LegacyRecordUnchanged`: the same push in LEGACY gives a JSON string equal to a golden copy taken from the
    current output before the change (commit the golden).
  - `FrameIdRingMiss`: a frame id overwritten in the ring gives `q_valid` false, with no stale quality.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run the tests, the full build and ctest.**
- [ ] **Step 5: Commit** with the message `feat(sense): CFR quality + link context in RS records (sensing mode only)`.

### Task S4: Offline sensing scorer (spec §6)

**Files:**
- Create: `tools/sense/score_sensing.py`, `tools/sense/test_score_sensing.py`, `tools/sense/fixtures/` (small
  synthetic jsonl)

**Interfaces:**
- `score_sensing.py <records.jsonl>... [--min-prb 128] [--json out.json]` reads one records file per cell.
  - It prints, per PCI and per link (dir, tx, layer), the §6 table:
    - gap p50/p95/max between valid wideband CFRs;
    - availability over 100 ms and 1 s windows;
    - valid PRBs/s and the `valid_cfr_prbs` histogram;
    - a PRB x 1 s occupancy map (written to the JSON);
    - the SNR, residual and recon_conf distributions, and the valid CFR rate.
  - Then one aggregate line: union availability, and the rescue fraction (the share of 100 ms windows covered by
    exactly one PCI, and which PCI it was).
- Run-level stability fields come from ISAC_METRICS in `run.log` when given `--log`: violations, `disabled_skips`,
  units enabled over time.
- DL and UL, different `tx`, and different layers are never pooled.

- [ ] **Step 1: Write the failing tests** with fixtures:
  - `test_gaps`: wideband records at t = 0, 0.1, 0.5 s give gaps 0.1 and 0.4, p50 0.25 (or the median the code
    defines; assert the defined value), max 0.4.
  - `test_not_wideband_excluded`: a 100-PRB record is not counted.
  - `test_rescue_fraction`: two PCIs covering complementary windows give a rescue fraction of 1.0 split 50/50.
  - `test_links_not_pooled`: DL and UL of the same PCI are reported separately.
  - `test_availability_windows`.
- [ ] **Step 2: Run them to verify they fail**, then implement, then check they pass:
  `python3 -m pytest tools/sense -q`.
- [ ] **Step 3: Commit** with the message `feat(sense): offline sensing scorer (continuity, availability, bandwidth, quality, multi-PCI rescue)`.

### Task S5: Per-unit cost accounting

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_unit.h`, `nr_sense_unit.c`
- Modify the attribution sites. Each takes thread CPU (`CLOCK_THREAD_CPUTIME_ID`) at start and end, and the
  enqueue-to-start wait:
  - PDSCH queue consumer, full decodes (nr_pdsch_passive_queue.c, at the job's start and end around the decode):
    src PDSCH_DATA, `job.rnti`;
  - data-aided worker (nr_pdsch_data_aided.c:830 path): PDSCH_DATA;
  - DM-RS CFR tap (nr_pdcch_blind_monitor_rt.c:9957): PDSCH_DMRS;
  - PUSCH DM-RS (nr_pusch_passive_decode.c:2545-2600): PUSCH_DMRS;
  - SRS (nr_srs_passive.c:365 path): SRS;
  - the protected baseline: scan, discovery and solver classes via the pool's per-class hold time
    (`nr_pool_cell_hold_ns`, nr_core_pool.h:127, plus the per-class `hold_ms` in `nr_pool_cell_cpu_line`), and
    SSB/CSI-RS via the existing `nr_sense_cost_add`.
- Test: `tests/nr_sense_unit_test.cc`

**Interfaces:**
- Produces:
  ```c
  typedef enum { NR_UNIT_PDSCH_DMRS = 0, NR_UNIT_PDSCH_DATA, NR_UNIT_PUSCH_DMRS, NR_UNIT_SRS, NR_UNIT_SRC_N } nr_unit_src_t;
  void nr_sense_unit_job(nr_unit_src_t s, uint16_t rnti, uint64_t cpu_ns, uint64_t wait_ns, bool late,
                         uint16_t est_usable_prbs, uint64_t t_ns);          /* calling thread's cell; lock-free per-cell slot */
  void nr_sense_unit_cfr(nr_unit_src_t s, uint16_t rnti, uint16_t valid_prbs, float snr_db, uint64_t t_ns); /* from emit_rs */
  void nr_sense_unit_violation(nr_unit_src_t s, uint16_t rnti);             /* a drop inside an enabled unit */
  void nr_sense_unit_skip(nr_unit_src_t s, uint16_t rnti);                  /* disabled_skips */
  typedef struct {
    uint16_t rnti; nr_unit_src_t src; bool enabled;
    float cpu_p95_ns, cpu_p99_ns;        /* per 50 ms bucket demand over the last SENSE_WIN_DECISIONS decisions */
    float wait_p99_ns; uint32_t late, violations, skips;
    float rate_hz, wb_rate_hz;           /* messages / those with est_usable >= min_prb */
    float valid_ratio, snr_db; uint64_t last_valid_wb_ns, last_msg_ns;
  } nr_sense_unit_view_t;
  int nr_sense_unit_snapshot(int cell, nr_sense_unit_view_t *out, int max);   /* scheduler thread; returns count */
  ```
- Table: 64 RNTIs x `NR_UNIT_SRC_N` per cell (`NR_CELL_STATIC`), LRU on `last_msg_ns`. Each unit has a ring of 320
  buckets of 50 ms. There is no allocation on the hot path. Writers use per-slot atomics; the snapshot reads them
  racily and is consistent per bucket.

- [ ] **Step 1: Write the failing tests**:
  - `BucketsAccumulate`: 3 jobs of 1 ms CPU in one bucket give 3 ms in that bucket.
  - `P99OverWindow`: 319 buckets at 1 ms and 1 bucket at 50 ms give a p99 of 50 ms and a p95 of 1 ms (use the
    nearest-rank definition, and assert it).
  - `LruEvictsIdle`: 65 RNTIs evict the oldest.
  - `ViolationAndSkipCounted`.
  - `SnapshotPerCell`: cell 0 and cell 1 never mix.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement it, and add the attribution calls.** They are active in SENSING mode only, one
  `nr_isac_mode()` branch per site.
- [ ] **Step 4: Run the tests, the full build and ctest.** Then a cost check: one replay of
  `raw_pci415_4s_1102_0951` in sensing mode (Task S8's cfg). Report the per-job attribution overhead from the
  thread-CPU delta of a build with and without the calls (spec §5.2 VERIFY).
- [ ] **Step 5: Commit** with the message `feat(sense): per-(PCI, source, RNTI) cost accounting in 50 ms buckets`.

### Task S6: Scheduler decision (pure)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_sched.h`, `nr_sense_sched.c` (decision only, no threads, no I/O)
- Test: `tests/nr_sense_sched_test.cc`

**Interfaces:**
- Consumes: `nr_sense_unit_view_t` (S5).
- Produces:
  ```c
  typedef struct { float cap_ns, base_p99_ns; } nr_sense_res_t;   /* one shared resource per window */
  typedef enum { NR_RES_POOL = 0, NR_RES_QUEUE_DL, NR_RES_QUEUE_UL, NR_RES_GPU, NR_RES_SENSE_RING, NR_RES_N } nr_res_id_t;
  typedef struct {
    int n_cells; int pci[8];
    const nr_sense_unit_view_t *units[8]; int n_units[8];
    nr_sense_res_t res[NR_RES_N];
    int min_prb; uint64_t now_ns;
  } nr_sense_sched_in_t;
  typedef struct {   /* kept between decisions by the caller */
    uint8_t streak[8][64][NR_UNIT_SRC_N]; bool enabled[8][64][NR_UNIT_SRC_N]; uint16_t rnti[8][64];
    uint64_t last_pci_valid_wb_ns[8];
  } nr_sense_sched_state_t;
  typedef struct { int enables, disables; nr_res_id_t binding; float value_last_admitted; } nr_sense_sched_out_t;
  void nr_sense_sched_decide(const nr_sense_sched_in_t *in, nr_sense_sched_state_t *st, nr_sense_sched_out_t *out);
  ```
- The rules, from spec §5.1-5.5. These are the decisions the tests pin:
  - **Eligibility**: the unit has `wb_rate_hz > 0` in the last window and `valid_ratio >= 0.2`
    (`SENSE_ELIG_VALID_MIN`). A PCI is eligible if any of its units is.
  - **Value**: `wb_rate_hz x valid_ratio x mean est_usable_prbs`, with `snr_db` as the tie-break.
    - The age boost is `1 + (SENSE_AGE_CAP - 1) x min(1, age / SENSE_AGE_FULL_NS)`, where age is
      `now - last_valid_wb_ns`. It applies only while the unit is eligible.
    - The PCI-level boost uses the same formula on `last_pci_valid_wb_ns`.
    - The final value is the unit value x the unit boost x the PCI boost.
  - **Demand**: per resource, the unit's `cpu_p99_ns` per bucket, scaled to the window. For NR_RES_QUEUE_*, the
    measure is `wait_p99_ns` against the queue's deadline budget instead.
  - **Admission**: `cap - base_p99` is the available amount per resource. Admit in value order while every resource
    stays <= `(1 - SENSE_HEADROOM) x available`. Record the binding resource.
  - A unit with no history uses the source's per-cell p99, and at most one new unit is admitted per decision.
  - **Changes**: an enable or disable needs `SENSE_CONFIRM` consecutive decisions that agree. A disable also needs
    value < `(1 - SENSE_HYST) x value_last_admitted`, or lost headroom. At most `SENSE_MAX_ENABLES` enables per
    decision; disables are unlimited.
  - **Violations**: any unit with `violations > 0` since the last decision causes the lowest-value enabled unit of
    its resource group to be disabled now (no streak).
  - **No fixed one-unit-per-PCI rule.**

- [ ] **Step 1: Write the failing tests** (pure, synthetic views):
  - `BandwidthFirst`: of two units with equal cost, the higher `wb_rate x prbs` is admitted.
  - `QualityTieBreak`.
  - `AgeBoostBoundedAt2x`: a 30 s age gives exactly x2.
  - `IdleUnitGetsNoBoostAndAges0`: `wb_rate_hz = 0` gives boost 1 and the unit is not admitted.
  - `IneligiblePciTakesNoCapacity`: a PCI with only 50-PRB traffic gets 0 units, and its capacity goes to the
    other PCI.
  - `NoOneUnitPerPciRule`: two productive PCIs where B is much better and capacity fits one unit give B's unit,
    unless A's age boost x value exceeds it.
  - `AdmissionStopsAtHeadroom`: with pool available 100, units of 25 each, headroom 0.4, 2 are admitted and the
    binding resource is POOL.
  - `PerResourceBinding`: the GPU binds before the pool for data units.
  - `HysteresisHoldsWithin20Pct`.
  - `StreakNeedsTwoDecisions`.
  - `MaxTwoEnables`.
  - `ViolationRemovesLowestImmediately`.
  - `ColdStartOneUnitPerDecision`.
  - `SinglePciWorks`: `n_cells = 1`.
  - `NarrowCarrierEnablesNothing`: every `est_usable < min_prb` gives 0 enabled.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement `nr_sense_sched_decide`.**
- [ ] **Step 4: Run the tests to verify they pass.**
- [ ] **Step 5: Commit** with the message `feat(sense): pure unit scheduler (bandwidth-first value, bounded age fairness, per-resource admission, hysteresis)`.

### Task S7: Scheduler wiring, gates, violations, metrics

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_sched_rt.c` (thread and resource sampling)
- Modify: the start/stop path in `executables/nr-uesoftmodem.c`, next to `mpci_control_thread` (:1035). The scheduler
  thread is started in SENSING mode for N >= 1 cells, and it must not depend on the core split (which needs 2 or
  more cells).
- Modify the gate sites (spec §5.6). Each one checks `nr_sense_unit_enabled(src, rnti)` before any work, and calls
  `nr_sense_unit_skip` when it returns false:
  - PDSCH data: nr_pdsch_data_aided.c:198/:830, nr_pdcch_blind_monitor_rt.c:5459;
  - PDSCH DM-RS: nr_pdcch_blind_monitor_rt.c:9957;
  - PUSCH DM-RS: nr_pusch_passive_decode.c:2147/:2545/:2600;
  - SRS: nr_srs_passive.c:365.
  - SSB and CSI-RS are never gated.
- Modify `nr_traffic_decode.c` (traffic plan Task 6): in SENSING mode, `nr_traffic_decode_wanted` returns true for
  every grant of an RNTI whose PDSCH_DATA unit is enabled.
- Modify the drop paths: each calls `nr_sense_unit_violation` when the dropped job's unit is enabled.
  - PDSCH queue full / stale / drop-oldest (nr_pdsch_passive_queue.c);
  - data-aided async drop (`nr_sense_async_note(..., 0)`);
  - PUSCH queue drops;
  - sense ring full for an enabled-unit record.
- Modify `nr_passive_metrics*.c`:
  - per-cell keys `sense_units_enabled`, `sense_violations`, `sense_disabled_skips`, `sense_wb_valid`,
    `sense_wb_valid_prbs`, `sense_gap_max_ms`, `sched_binding`;
  - one `SENSING: SENSE_SCHED` log line per decision (enables, disables, binding, per-unit value of the admitted set).
- Test: `tests/nr_sense_sched_rt_test.cc` (gate table and violation routing with a stub mode), plus the metrics test.

**Interfaces:**
- Produces:
  - `bool nr_sense_unit_enabled(nr_unit_src_t s, uint16_t rnti)`: per-cell atomic table, lock-free read;
  - `void nr_sense_sched_start(void)` / `void nr_sense_sched_stop(void)`.
- Resource sampling every decision:
  - POOL: `cap` = pool tokens x window, from `nr_pool_stats`; `base` = scan/disc/solver class hold over the window.
    **VERIFY** which `nr_pool_stats_t` fields hold the per-class hold time, and record them in the report.
  - QUEUE_DL/UL: queue wait p99 against the deadline budget, using the existing late threshold (RECOV `late_ms`).
  - GPU: pacer refusals and GPU busy, from the CELL_GPU line source (nr-uesoftmodem.c:1054). **VERIFY** the function
    name.
  - SENSE_RING: per-cell `rec_dropped` delta.
- A unit is enabled only after its enable is published. A message already in flight when its unit is disabled is
  finished: no early termination.

- [ ] **Step 1: Write the failing tests**:
  - `DisabledUnitSkippedBeforeWork`: the stubbed gate returns false, `skips` + 1, and the work function is not
    called.
  - `InFlightFinishesAfterDisable`.
  - `DropInEnabledUnitIsViolation`.
  - `DropInDisabledUnitIsNotViolation`.
  - `SsbCsirsNeverGated`.
  - `MetricsKeysPresent`.
- [ ] **Step 2: Run them to verify they fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Full build and ctest; the legacy regression gate** (`tests/passive_rx/dgx/rfsim_regress.sh` under
  `flock -x /tmp/td_measure.lock`, idle host). Expected: the 4-RX gate thresholds of CLAUDE.md pass, i.e. legacy is
  unchanged.
- [ ] **Step 5: Commit** with the message `feat(sense): scheduler thread, pre-work unit gates, violation routing, sensing metrics`.

### Task S8: Replay validation, headroom calibration, checkpoint

**Files:**
- Create: `tests/passive_rx/sensing/sensing_replay_415.cfg` (the traffic plan's `legacy_replay_415.cfg` plus
  `mode = "sensing";`, `records_path` and `cfr_path`), `sensing_replay_923.cfg`
- Create: `tests/passive_rx/sensing/run_replay.sh`, which reuses the traffic plan's `ab_replay.sh` mechanics
  (csd_replay.sh, CAP/CAP_TICK, the centre/SSB override)
- Create: `docs/superpowers/reports/twomodes/SENSING-REPLAY-<date>.md`

**Interfaces:**
- No new code. The scorer (S4) and ISAC_METRICS (S7) produce every number.

- [ ] **Step 1: Run the replays.** Run `run_replay.sh` with 3 runs per capture (415, 923) in sensing mode, then the
  same in traffic mode for the PDCCH-health context. Wait until `pgrep -x nr-uesoftmodem` is empty first; the heavy
  lock is taken exclusively.
- [ ] **Step 2: Score.** `python3 tools/sense/score_sensing.py <records> --log <run.log> --json ...` for every run.
- [ ] **Step 3: Write the report.** Labels are `[OFFLINE VERIFIED, DGX replay]`, per capture, never pooled. It
  contains:
  - the §6 table;
  - lost PDCCH occasions, sensing vs traffic on the same capture (engineering target: sensing no higher);
  - violations (target: 0) and `disabled_skips`;
  - the quality cost from S2 and the attribution overhead from S5;
  - the binding resources;
  - every VERIFY outcome.
  There are no "beats legacy" criteria (spec §9.3).
- [ ] **Step 4: Headroom stays at 0.40 (operator 2026-11-03).** No parameter is calibrated on the DEIB/OTA cells.
  Report the measured violations and PDCCH loss at 0.40 only. Any change to a constant comes from general reasoning
  or from the sens6 lab with known references, and needs a ledger ruling.
- [ ] **Step 5: Frozen check** (the traffic plan's command) exits 0.
- [ ] **Step 6: Commit** the cfgs, the script and the report.
- [ ] **Step 7: Operator checkpoint.** Stop. The next steps are spec §9.1 (sens6 lab, where the validity thresholds
  are calibrated against known references) and §9.2 (NSA: single PCI, then the pair, then 3-4 PCIs over 200 MHz,
  daytime, >= 3 runs per arm, rescue fraction). They need the operator and the hardware. Scan suspension (spec §7)
  is decided only from these measurements.

## Self-review notes

- Spec coverage:
  - §4.0-4.2: S1, S2, S3.
  - §5.1: S6, S7.
  - §5.2: S5, S7 (the resources).
  - §5.3-5.5: S6.
  - §5.6: S7.
  - §6: S3, S4, S7.
  - §7: excluded by design.
  - §8.3: S1.
  - §8.4: S2, S3.
  - §8.5: S5.
  - §8.8: S7 (no dependency on the split).
  - §9: S8 plus the checkpoint.
  - §10 steps 3-7.
- Types are consistent: `nr_unit_src_t` (S5) is used by S6 and S7; `nr_sense_quality_t` (S2) by S3; resolver fields
  extend traffic Task 1.
- Conflicts for the operator:
  - S1: resolved 2026-11-04 (keep shedding, add attempt counter and skip backlog).
  - S2: provisional validity thresholds.
  - S7: VERIFY on the pool and GPU stat field names.
