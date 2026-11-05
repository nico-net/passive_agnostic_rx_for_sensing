# Two receiver modes: traffic and sensing (design, 2026-11-03, revision 2)

Branch `twomodes/design` off `mpci-main` @ e739bd51b0 (worktree `wt/rr-twomodes`). Status: DESIGN, not approved, no code.

**How claims are marked** (labels per PROJECT_MEMORY §0.1):
- `[CODE]`: confirmed by reading the code at e739bd51b0 (file:line given).
- `[MEASURED ...]`: an existing measurement, with its host, cell and source. This design adds no new measurement.
- **Decision (operator)**: decided by the operator in the brainstorm.
- **Decision (design)**: chosen by the designer and not confirmed by the operator. These are listed in §11.
- **VERIFY**: an assumption that the implementation must check before relying on it.

## 1. Why

`[MEASURED OTA, DGX+X410, pair 415+923, §40.2]` Full decode of every DCI, PDSCH and PUSCH on every PCI does not fit
beyond about 2 busy cells on one DGX. Pairs lose 13-45k PDCCH occasions per cell per run and 6-32 % of sensing jobs.
The causes are too few fast cores, home-core contention, migrations and lock stalls. Scheduling tweaks only moved
the shortage. The receiver serves two consumers, and neither needs everything:

- **Traffic**: who is scheduled, where, how and when, on as many PCIs as possible. Used for real-time or offline
  analysis, and as input to a future generative RAN digital twin.
- **Sensing**: continuous, high-quality, wideband CFR observations from several PCIs, for a sensing algorithm on top
  of the receiver (out of scope) or for post-processing.

## 2. Mode selection

- **Decision (operator)**: one build, and the mode is fixed for the whole run. The key goes in the receiver `.cfg`:
  `[sensing] mode = "traffic" | "sensing"`. No env, CLI or -D flag is added.
- **Decision (design)**: with no key the receiver behaves exactly as today ("legacy"). The frozen sens6 configs, the
  regression gate and existing configs stay untouched.
- **Decision (design)**: each mode is its own code path keyed on the mode, plus defaults for the existing keys
  `sources`, `pdcch_blind_monitor_pdsch` and `pdcch_blind_monitor_ul_pusch` (nr_isac.cc:261,
  nr_pdcch_blind_monitor.c:2965/3452). An explicit existing key may narrow what a mode does, never widen it past the
  mode's invariants. Traffic mode refuses any CFR source and logs one line saying so.
- **Single PCI and multi-PCI** both run the same code. Every rule below holds for N = 1. PCI-level fairness has
  nothing to choose between when N = 1, and that is the only difference.
- Future, not in scope: changing mode during a run, or adapting the sensing set to live traffic.

## 3. Traffic mode (built and validated first)

### 3.1 What exists `[CODE]`
- Grant record: `nr_sense_emit_grant_meta` (nr_sense_records.c:266) calls `nr_sense_grant_to_json`
  (nr_passive_obs.c:168). Existing keys:
  - from the grant: abs_slot, t_mono_ns, frame, slot, pci, dir, rnti, rnti_class, start_rb, nb_rb, start_sym, nb_sym,
    mcs, mcs_table, qm, nl, dmrs_symb_pos, dmrs_scrambling_id, tbs, harq_pid, rv, ndi, crc, nvar, snr_db, fo_comp_hz,
    delay_samples, carrier_hz, scs_khz, fs_hz;
  - from the meta: epoch, identity_gen, dedicated_epoch, rx_ts, dl_coverage, verif, k2, dci_slot,
    transform_precoding, freq_hopping, dmrs_type, dmrs_add_pos, dmrs_n_scid, ta_*, cfo_est_hz, ul_timing_ref.
  - `start_rb` is CRB-indexed (BWP start + offset, nr_passive_obs.h:36).
- **The record is written only after a PDSCH or PUSCH decode.** It has 3 callers: nr_pdsch_passive_queue.c:2305,
  nr_pdcch_blind_monitor_rt.c:10156 and nr_pusch_passive_decode.c:3365. With decode off, a decoded DCI writes nothing.
  Solver probes write nothing either.
- Record path: `nr_sense_push_record` (nr_sense_api.c:446) copies the line into a per-cell ring under a per-cell mutex
  and signals the writer. It drops when the ring is full and does no I/O on the caller. The JSON is formatted on the
  caller's thread (about 2 KB of snprintf).
  - CFR metadata already has a deferred path: `nr_sense_push_rs_binary` (:462) copies a struct, and the writer thread
    formats it.
  - Counters: rec_pushed, rec_dropped, io_errors, zmq_drops, after_close (nr_sense_api.h:60).
- A session record (`type="session"`, nr_sense_records.c:233) carries the carrier, SCS, fs, N_RB, n_rx and the build.
  Every record carries `epoch`, `identity_gen` and `dedicated_epoch` (nr_passive_cfg_epoch.h).
- Formats recovered by the scan:
  - DL: 1_0 (SI, RA, P, TC, C) and 1_1 (C);
  - UL: 0_0, recovered off the 1_0 scan (`g_ul00_accepts`, nr_pdcch_blind_monitor_rt.c:2338), and 0_1.
- Lost-DCI counters per cell in ISAC_METRICS: `pdcch_occasions`, `pdcch_accepts`, `scanq_drop_full/stale/epoch`.
- PUCCH: no passive receiver exists. `nr_decode_pucch*` is gNB-only (NR_TRANSPORT/pucch_rx.c).
- Solver (nr_pdsch_solver.c/.h):
  - It learns each RNTI's DCI 1_1 layout, S/L, DM-RS position and MCS table only from PDSCH CRC passes.
  - Once solved, it decodes every grant in full. That full decode is also its safety check: it reopens after 256
    failures with no pass (:1910).
  - A HARQ/RV consistency check works from the DCI alone.
  - `nr_pdsch_solver_enabled()` is hardwired true.
- The DCI 1_1 layout sweep (nr_pdcch_dci11_layout_sweep.c) already knows where TPC, PRI, K1, DAI and the Rel-17
  options sit in each layout.

### 3.2 Scope
- **Decision (operator)**: every DCI on every PCI, DL and UL, covering all formats and RNTI classes the scan already
  recovers. Every parameter goes in the record (§3.3).
- **Decision (operator)**: DCI-only in v1. Blind PUCCH/UCI is a later, separate project.
- **Decision (operator)**: no CFR export at all.
- **Decision (operator)**: PDSCH decode only where the metadata needs it: the solver's probes until an RNTI is solved,
  then a rare check decode per solved RNTI.
- **Decision (design)**: PUSCH decode only for the per-RNTI MCS-table learning (fix1102/ul-uci), bounded the same way.
  When the table is unknown, `mcs_table` and `tbs` print null.

### 3.3 The DCI record
**Emission.** It is written at DCI accept: the point in nr_pdcch_blind_monitor_rt.c (~:9859) where `pdcch_accepts` is
incremented, and the UL equivalent. It reuses `nr_sense_emit_grant_meta` and the existing JSON, keeping every
existing key.
- **Decision (design)**: so as not to block the PDCCH critical path, the accept point copies a fixed-size struct into
  the per-cell ring (the `nr_sense_push_rs_binary` pattern). The writer thread formats the JSON.
  - The cost on the PDCCH thread is one mutex and one memcpy.
  - **VERIFY** under load on the pair: the per-cell `mu` is also taken by CFR producers in legacy and sensing modes.
    In traffic mode it has no other users. Measure lock wait in the traffic replay.

**Four counts per cell** (ISAC_METRICS), never merged:
1. `dci_accepted`: the existing `pdcch_accepts`, split by format and by `layout_state`;
2. `dci_rec_generated`: structs handed to the ring;
3. `dci_rec_persisted`: lines written by the writer, counted at write success;
4. `dci_rec_lost`: ring full, I/O error or ZMQ drop. These are output losses, kept separate from decoder misses.

Invariant: `dci_accepted == dci_rec_generated`, by construction. Persisted plus lost equals generated, up to the lines
in flight at shutdown, which are counted.

**Keys added to the record** (null when unknown):
- Identity and search: `dci_format` (1_0, 1_1, 0_0, 0_1), `ss_type` (css or uss), CORESET/bank id, `al`, `cce`,
  `dci_len`, `rnti`, `rnti_class` (existing).
- Raw payload: `dci_bits` in hex.
  - Raw bits are **not** self-sufficient offline: interpreting them needs the layout and the configuration valid
    when the DCI was received.
- Configuration reference: `cfg_ref`, a versioned reference to the configuration and layout in force. It combines
  `identity_gen`, `dedicated_epoch`, a per-RNTI `layout_ver`, and the bank entry id.
  - A new `type="config"` record is written **on every change** of what a `cfg_ref` points to: CORESET geometry, BWP
    (start, size), the RNTI's solved layout (field widths/offsets), the TDRA S/L by index, the DM-RS position, the MCS
    table, and the CSI-RS rate-matching set.
  - Offline, a DCI is interpreted with the latest config record whose `cfg_ref` matches. That record is on the same
    stream, so offline interpretation needs no receiver state.
  - Config records are rare (they follow learning events, not grants).
- Confidence: `layout_state` is one of:
  - `fixed`: 1_0/0_0 (TS 38.212 §7.3.1). The widths depend only on the BWP size, which the receiver learns from its
    own SIB1 decode (nr_pdcch_blind_monitor.c:195).
  - `solved`, `candidate` or `unsolved`.
  - Fields that depend on the layout are null unless the state is `fixed` or `solved`.
  - Plus `layout_checks_ok` and `layout_checks_fail` for that RNTI so far, so a consumer can weight a `solved`
    record.
- Allocation (BWP-relative **and** CRB): `bwp_start`, `bwp_size`, `start_rb_bwp`, `nb_rb`, `start_rb` (CRB,
  existing). Also `vrb_prb`, the TDRA index with S/L (existing start_sym/nb_sym), K0 (DL), K2 (UL, existing).
- HARQ and timing:
  - `harq_pid`, `ndi`, `rv` (existing), `dai`, K1 (PDSCH-to-HARQ), TPC (PUCCH or PUSCH), PUCCH resource indicator;
  - abs slots of the PDSCH/PUSCH and of the HARQ feedback;
  - `retx`, from the NDI toggle and the per-RNTI pid history. It is null when the layout is not fixed/solved, or
    when the history was broken by an epoch or a gap.
- Rest of the payload when present: antenna ports, DM-RS sequence init, SRS request, CSI request, CBGTI, BWP indicator,
  carrier indicator, TB2 MCS/NDI/RV, UL-SCH indicator, transform precoding, frequency hopping. For P/SI/RA: the
  short-message indicator, the SI indicator, TB scaling.
- `mcs`, `mcs_table`, `qm`, `nl`, `tbs` (existing). TBS is only filled when the table is known.
- Periodicity: the record carries `abs_slot` and `t_mono_ns` (existing), from which the consumer derives
  periodicity. **Decision (design)**: the receiver's own periodic tracker results (nr_periodic_track.c, SPS/CG
  periods) go in config records when they confirm a period. No new tracker is built.
- `decode`: `none` or `queued`. The outcome of a probe or check decode (CRC, SNR, learned S/L/table) goes into a small
  `type="decode"` record keyed by (abs_slot, rnti). In traffic mode the legacy post-decode grant record is not written,
  so each grant has exactly one DCI record.

**Decision (design)**: DCI 2_x is out of v1. The scan has no blind size/RNTI search for it.

### 3.4 Decode policy in traffic mode
- **Unsolved RNTI**: the solver's probes run as today (fair budget, nr_pdsch_solver.c:570). This is discovery, and it
  keeps priority.
- **Solved RNTI**: a full check decode on the first grant after `T_check`, or on every 32nd grant, whichever comes
  first. One more is decoded immediately when the HARQ/RV consistency check flags a violation.
  - Reopen after K consecutive failed checks with no pass. **Decision (design)**: `T_check` = 2 s and K = 4,
    compile-time constants. Today's 256-grant rule assumes every grant is decoded.
  - A wrong solution can therefore produce `solved` records for about K checks before it is reopened. Its
    `layout_checks_fail` count is visible in every record. The replay measures how long this lasts.
- All other PDSCH/PUSCH decode is off, as are the CFR producers, SRS and PRACH. SSB and CSI-RS **acquisition** stays
  (no export), because the solver's probes need CSI-RS rate matching.
- The CPU freed goes back to the existing pool. No core is reserved per activity.

### 3.5 Traffic validation
1. **ctest** (known ARM failures in CLAUDE.md). New unit tests:
   - the record JSON (fixture, all keys, null rules);
   - the config record and the `cfg_ref` round trip (a fixture DCI is reinterpreted offline from its config record);
   - the check-decode limiter;
   - the reopen rule;
   - the four counters' invariant.
2. **Replay**, DGX, under the replay lock: `raw_pci415_4s_1102_0951` and `raw_pci923_4s_1102_0951`, legacy vs traffic,
   3 runs per arm, alternating. **Decision (design)** pass bounds:
   - `dci_accepted == dci_rec_generated` exactly, and `dci_rec_lost` = 0 on replay;
   - solved RNTIs: traffic-arm mean >= 90 % of the legacy-arm mean;
   - `scanq_drop_*` mean <= legacy mean + one legacy standard deviation, and process CPU lower;
   - `[OFFLINE VERIFIED]` field agreement: for solved RNTIs, the record's PRBs, MCS and TBS equal the legacy
     post-decode record for the same (abs_slot, rnti);
   - lock wait on `mu` and accept-path cost reported (p50/p99).
3. **sens6 lab cell** (§9.1): the lab gNB's own logs are the ground truth for **validation only**. DCI recall and
   field accuracy are measured against them there, and nowhere else.
4. **OTA, commercial NSA, daytime** (§9.2): single PCI first, then the pair 415+923, then 3-4 PCIs over 200 MHz.
   - Report per cell: accepted DCIs/s, lost PDCCH occasions, solved RNTIs, and the four record counters.
   - No DCI recall is claimed on commercial cells: there is no independent reference.

## 4. Sensing mode: signals and record quality (after traffic mode is validated)

### 4.0 What sensing mode keeps from traffic mode
**Decision (design)**: the scan, DCI records, discovery, the solver's probes and the check-decode policy of §3.4 run
in sensing mode too, as the **protected baseline**. An enabled data-aided PDSCH unit decodes every grant of its RNTI,
which also serves as that RNTI's check.

### 4.1 Signals
- **SSB** is always on, on every cell (operator).
- **CSI-RS/TRS** is on wherever blind discovery holds a confirmed resource (operator: conditional).
  - `[CODE]` Non-SOFT epochs reset discovery (nr_csirs_disc.c:200), and it is re-acquired automatically.
  - SSB and CSI-RS sit **outside** the unit scheduler. Their cost is part of the protected baseline.
- **Wideband sources** are scheduled per unit (§5): PDSCH DM-RS, data-aided PDSCH, PUSCH DM-RS, and SRS where present.
  PRACH stays as today (0 on NSA, operator statement).
- **Consequence of the 128-PRB rule** (§4.2): SSB (20 RB) and the TRS seen at DEIB (52-RB blocks,
  `[MEASURED replay]` CSI-FULL) are **never** wideband. Only full-band CSI-RS, which is not handled today (§8), could
  be. In practice, wideband records come from DM-RS and data-aided PDSCH of large grants, and from PUSCH/SRS when wide.
  SSB and CSI-RS keep continuity of the cell link (timing, CFO, coarse channel), not of wideband sensing.

### 4.2 Wideband record and validity
- **Decision (operator)**: a record is wideband when it has `valid_cfr_prbs >= 128`. This is a `.cfg` key in
  `[sensing]` (`sense_min_prb`, default 128).
  - The DEIB cells have 217 PRB `[MEASURED OTA]` and the sens6 lab cell has 273 (frozen sens6 launchers, `-r 273`).
    128 is reachable on both.
  - On a carrier with fewer than 128 PRB no record can be wideband. The receiver logs this once at start.
- Three bandwidth quantities, all in the CFR record:
  - `allocated_prbs`: PRBs assigned to the transmission (from the DCI). Used for prediction.
  - `estimated_usable_prbs`: allocated PRBs minus those known to be unusable (CSI-RS/ZP rate-matched-only symbols,
    PRBs outside the reading's verified FDRA). Used for prediction.
  - `valid_cfr_prbs`: PRBs that passed the validity test below. **Sensing performance is scored on this one only.**
- **Validity is not SNR alone.** Per record:
  - (a) post-estimation SNR >= floor. `[CODE]` This exists (`snr_db`, `snr_valid`, nr_sense_records.h).
  - (b) **pilot residual**: normalised fit residual of the pilots against the estimate (per PRB, then the record).
    For DM-RS with two or more DM-RS symbols or CDM ports, the consistency of the per-symbol estimates.
    **New**: `[CODE]` no residual is exported today.
  - (c) for data-aided PDSCH: the TB CRC. `[CODE]` This is satisfied by construction: data-aided reconstructs only
    from CRC-verified TBs (nr_pdsch_data_aided.h:23).
  - (d) **reconstruction confidence**: for data-aided, the share of REs whose re-encoded symbol matches the hard
    decision of the equalised symbol. For DM-RS, the DM-RS identity's verification status (`verif`, existing).
  - A PRB is valid when (a), (b) and (d) pass on it. The record's `valid_cfr_prbs` counts those PRBs, and the record
    carries the per-PRB valid mask.
  - **Decision (design)**: thresholds are compile-time constants, calibrated on the sens6 lab cell where references
    exist (§9.1). They are never tuned per cell.
  - **VERIFY**: the per-PRB residual costs O(REs) on the sensing CPU, which stays off the GPU. Measure its cost per
    record on replay before enabling it by default.
- **Physical link context.** Every CFR record carries:
  - direction (DL/UL);
  - transmitter: the PCI for DL; for UL, RNTI and the TA state (existing `ta_*`);
  - DM-RS ports, layer, rank and precoding indication where known (`layer_idx1`, `n_layers`, `port_mask`, existing);
  - SSB index (beam) where known;
  - `cfg_ref`.
  - Records with a different direction, transmitter or precoding are **different channels**. The metrics of §6
    never pool them.
  - This context lives in the record, not in the scheduling key.

## 5. Sensing mode: unit scheduler

### 5.1 Unit and invariants (operator)
- Unit = (PCI, message type, RNTI), for wideband sources only.
- A slow scheduler decides every 2-5 s which units are **enabled**. **Decision (design)**: 4 s, the period of the
  existing core split.
- **No grant-by-grant selection inside an enabled unit.** Every message of an enabled unit is processed fully: no
  intentional skip, no early termination.
  - Any drop inside an enabled unit (async drop, queue full, drop-oldest, stale) is an **enabled-unit violation**. It
    is counted separately from intentional disabling.
  - The existing drop paths stay as the safety net that bounds queue depth. After a violation, the scheduler removes
    the lowest-value unit at the next decision, without waiting for the streak.
- Disabled units are gated **before** work starts, at the existing per-source gate sites (§5.6) plus an RNTI check at
  enqueue (`job.rnti`). Disabled-unit messages are counted as `disabled_skips`, never as violations or failures.
- Zero violations is an engineering **target**. It is measured and never claimed as a real-time guarantee.

### 5.2 Cost model (measured demand, not products of percentiles)
- **Per-job measurement**: thread CPU (`CLOCK_THREAD_CPUTIME_ID`) at job start and end, plus enqueue-to-start and
  start-to-end wall time, attributed to the job's (PCI, source, RNTI).
  - `[CODE]` A per-job thread-CPU pattern already exists in the SRS/PUSCH queues (nr_srs_passive.c:94,
    nr_pusch_passive_queue.c:284/609).
- **Short windows**: each unit accumulates CPU demand in 50 ms buckets. **Decision (design)**: 50 ms, inside the
  10-100 ms range. Over the last 4 decisions (about 16 s, 320 buckets) the scheduler reads per unit:
  - p95/p99 of per-bucket CPU demand (this is the measured burst, not rate x cost);
  - p95/p99 queue latency;
  - deadline misses: jobs late against their slot deadline, using the existing `late` notion (RECOV late_ms);
  - GPU time per bucket for units using GPU LDPC (data-aided PDSCH), plus GPU pacer refusals.
- **Per-resource admission.** A unit is admitted only if **every resource it uses** keeps headroom at its p99 demand:
  - the shared core pool (CELL_CPU accounting, nr_core_pool.c:758);
  - the specific queue it uses (latency p99 below its deadline budget);
  - the GPU when used (pacer refusals stay 0 and GPU busy stays below its limit);
  - the per-cell sense ring and writer.
  - The binding resource is reported. This captures contention and bottlenecks that a global CPU figure hides.
- **Protected baseline first**: the measured p99 demand of scan + discovery + solver + SSB/CSI-RS (per cell, summed)
  is reserved before any unit is admitted.
- **Headroom** is conservative and calibrated by experiment. **Decision (design)**: start at 40 % of each resource;
  lower it only from replay/OTA data showing 0 violations and no rise in lost PDCCH occasions. The value is a
  compile-time constant, recorded in the design when calibrated.
- New code: a fixed-size per-cell unit table (e.g. 64 RNTIs x 4 sources), LRU, preallocated buckets, no allocation
  on the hot path.
- A unit with no history is admitted with its source's per-cell p99 as the estimate, at most one new unit per
  decision.
- **VERIFY**: the cost of the per-job attribution, about 2 clock reads per job. Measure it on replay.

### 5.3 Value
Ranked bandwidth first, then quality (operator):
1. predicted valid wideband PRBs/s = recent rate of the unit's messages with `estimated_usable_prbs >= 128`, times
   their recent valid ratio (`valid_cfr_prbs / estimated_usable_prbs`);
2. CFR quality (valid-record SNR and residual) as the second key;
3. bounded fairness (§5.4) as a capped multiplier.

### 5.4 Fairness (bounded)
- **Eligibility**: a unit or a PCI takes part in fairness only if it has **genuine wideband opportunities**:
  messages with `estimated_usable_prbs >= 128` seen in the last window, and a recent valid ratio above a floor.
  A PCI whose traffic cannot produce wideband CFRs is not eligible and gets no boost. It still has SSB and CSI-RS.
- **Age** = time since the unit's (or PCI's) last **valid wideband CFR**, not since its last grant.
  - The boost grows with age up to a cap. **Decision (design)**: at most x2, reached at 10 s.
  - It is applied only while eligible. An ineligible or idle unit's boost is 0 and its age does not accumulate.
  - Low-value RNTIs are limited the same way: the boost multiplies value, so a low-value unit stays low.
- **Two levels**: PCI age first, then RNTI age within the PCI.
  - There is **no fixed one-unit-per-PCI allocation**. A productive PCI with an old age outranks a fresh one, but an
    ineligible PCI never takes capacity.
  - Diversity across productive PCIs comes from the PCI-level age term, bounded by the cap. Bandwidth and quality
    still dominate.

### 5.5 Stability
- A decision every 4 s. A change needs 2 consecutive agreeing windows (the split's streak rule, nr_core_pool.c:1139).
  Exception: violations act at once (§5.1).
- Hysteresis: a unit is disabled only if its value is below the last admitted unit's by a margin
  (**Decision (design)**: 20 %), or a resource lost headroom.
- At most 2 enables per decision, and any number of disables needed to restore headroom.
- Template: the split's `split_main` loop (sample, streak, rebaseline) or the 1 Hz `mpci_control_thread`
  (nr-uesoftmodem.c:1035). The split needs at least 2 cells (`dem_t0`); the sensing scheduler must also run at N = 1.

### 5.6 Gate sites (reused) `[CODE]`
- `nr_isac_source_enabled()` works per (cell, source). Today it is set once at start (nr_isac.cc:334). New: a runtime
  setter (atomic bitmask) and an RNTI check.
- Sites:
  - PDSCH data: nr_pdsch_data_aided.c:198/:830 and nr_pdcch_blind_monitor_rt.c:5459;
  - PDSCH DM-RS: nr_pdcch_blind_monitor_rt.c:9957;
  - PUSCH DM-RS: nr_pusch_passive_decode.c:2147/:2545/:2600;
  - PUSCH data: nr_pusch_data_aided.c:92;
  - SRS: nr_srs_passive.c:365;
  - CSI-RS: csi_rx.c:1005/:1161;
  - SSB: phy_procedures_nr_ue.c:1099.

## 6. Sensing metrics (per PCI and per link; aggregate in its own line; never pooled across cells)
| Dimension | Metrics |
|---|---|
| Continuity | inter-wideband-valid-CFR gap p50/p95/max, per PCI and aggregate (union of PCIs) |
| Availability | fraction of 25 ms (operator 2026-11-05; was 100 ms) and 1 s windows with >= 1 valid wideband CFR, per PCI and aggregate |
| Bandwidth | valid PRBs/s, distribution of `valid_cfr_prbs`, time-frequency coverage (PRB x window occupancy map) |
| Quality | SNR, pilot residual, reconstruction confidence, valid CFR rate (valid / produced) |
| Multi-PCI | per-PCI share of valid wideband CFRs; aggregate availability; **rescue fraction** = share of 25 ms windows where no PCI but one had a valid wideband CFR, and the share where each PCI was that one |
| Real-time | lost PDCCH occasions, discovery health (bank state, solved RNTIs, CSI-RS confirmed), queue stalls, deadline misses, CPU/GPU demand per resource, processing latency p50/p99 |
| Stability | enabled-unit violations, `disabled_skips` (intentional), units enabled over time, overload recovery time (violation to 0 violations), run length without violations |

Intentional disabling and failures inside enabled units are always reported separately. PDCCH monitoring stays on
every PCI in this release, so every missed occasion is a real miss.

## 7. PDCCH scan suspension: deferred (not in the first sensing release)
- PDCCH monitoring stays on for every PCI in the first sensing release.
- First, measure whether the unit scheduler removes discovery contention and lost PDCCH occasions (§6, Real-time).
  Scan-only suspension is considered only if PDCCH monitoring then remains a significant bottleneck.
- It never applies at N = 1.
- Kept as a future design:
  - suspend only persistently low-value cells when another PCI with wider grants needs the capacity;
  - periodic probing (e.g. 1 s every 10 s), hysteresis, automatic recovery;
  - missed occasions counted as `suspended_occasions`, never as decoder failures.
- `[CODE]` Constraints found:
  - No scan-only pause exists. The only per-cell pause parks the whole stream (nr-ue-ru.c:1016), and a resume after
    >= 10 ms triggers a timing re-lock and HARD_REVERIFY (nr-ue.c:1895, nr_passive_cfg_epoch.c:420). CSI-RS
    confirmations are forgotten and the CORESET bank goes STALE.
  - Solver solutions, MCS tables and the UL layout lock survive.
  - A future suspension must stop only the scan producer. **VERIFY**: the bank tick does not age entries across the
    gap (nr_pdcch_coreset_bank.c:424 compares `slot - last_accept_slot`); the HARQ/NDI history is marked broken.

## 8. Conflicts with the existing implementation and real-time constraints
1. **Grant records exist only post-decode** `[CODE]`. Traffic mode needs the new accept-time emission (§3.3).
2. **JSON formatting on the caller's thread** `[CODE]`. On the PDCCH path it must move to the writer (§3.3).
3. **`nr_admit` sheds CSI-RS discovery at tier 1 in pairs** (nr_admit.c:22). That conflicts with "protect
   discovery". The plan must decide whether sensing mode keeps it. It is not changed silently.
4. **No pilot residual or per-PRB validity exists** `[CODE]`. This is new sensing-side work (§4.2).
5. **Cost data is means per (cell, source) only** `[CODE]`. The per-unit buckets are new (§5.2).
6. **Full-band CSI-RS** (abs%160 = 24/63/105/135, CSI-FULL report) is not handled. Without it, CSI-RS can never
   reach 128 PRB at DEIB.
7. **The one-in-three Phase-2 lock episode** `[MEASURED OTA, §40.2]`, cause unknown, will hit both modes. It shows up
   as lost PDCCH occasions in traffic validation, and is reported, not hidden.
8. **The adaptive split** runs only with at least 2 cells. The sensing scheduler must not depend on it.
9. **The sens6 host** (x86, RTX 4060 Ti, sm_89, weak FP64) runs the lab tests.
   - The code must build there: CLAUDE.md cloud-x86 recipe plus the GPU modules, and all 17 module .so files.
   - The frozen sens6 files must not be edited. New configs go under `tests/passive_rx/` as `.cfg`.
   - The GPU paths differ from the DGX (non-cluster LDPC on sm_89; memory gpu-decode-pipeline-plan). Results are
     never merged across hosts.

## 9. Validation environments

### 9.1 Controlled laboratory (sens6 host, LiteOn gNB, OCUDU, 3-4 COTS UEs, single PCI)
- The gNB's logs and configuration are **validation ground truth only**, never receiver input.
- Traffic mode: DCI recall and field accuracy against the gNB scheduler logs; `cfg_ref` correctness against the
  known configuration.
- Sensing mode, across continuous, bursty, mixed DL/UL and high-load traffic generated by the UEs:
  - CFR extraction and reliability for every available source;
  - wideband production, quality, continuity, and computational stability (§6).
  - Channel-estimation references: the known configuration and the known transmitted signals (DM-RS sequences,
    re-encoded data from CRC-verified TBs) give references for validity calibration (§4.2 thresholds).
- Deliverables: a sens6 launcher and `.cfg` per mode, and a run script. The frozen files stay untouched.

### 9.2 Commercial NSA networks (DGX + X410, DEIB, indoor and outdoor)
- Single PCI first, then simultaneous multi-PCI over 200 MHz (the pair, then 3-4 PCIs).
- Natural traffic. Controlled UE-generated traffic where feasible (an operator-owned UE, never gNB access). Longer
  stability runs (for example 30-60 min) after the 240 s campaigns.
- Daytime only, at least 3 runs per arm, alternating. No night-load validation.
- Report §6 per PCI and in aggregate. **The multi-PCI result to show**: independent cells offer complementary sensing
  opportunities, i.e. aggregate availability and continuity above each single PCI's, through a non-zero rescue
  fraction.
- No claim of absolute CFR ground truth or DCI recall on commercial cells.

### 9.3 What is not a pass criterion
Sensing mode is **not** validated by beating legacy, full processing or random selection: there are no criteria on
per-PCI PRBs/s or gaps against legacy. Legacy runs on the same captures serve only as context (cost, PDCCH health).

### 9.4 Engineering targets (reported, not guaranteed)
- 0 enabled-unit violations in sustained runs;
- lost PDCCH occasions in sensing mode no higher than in traffic mode on the same cells (protected baseline);
- continuity and availability per §6, reported with distributions. No real-time claim without the measurements.

## 10. Order of work and tests
1. **Traffic mode**:
   - the mode key and the conflict policy;
   - the accept-time binary record, the four counters, and the config record with `cfg_ref`;
   - the added DCI fields;
   - the check-decode policy;
   - unit tests;
   - replay A/B (§3.5.2).
   - **Operator checkpoint.**
2. **Traffic on sens6** (§9.1) and single-PCI then multi-PCI NSA OTA (§9.2). **Operator checkpoint.**
3. **Sensing record quality**: the three bandwidth quantities, the per-PRB validity (residual, reconstruction
   confidence), link context; unit tests on synthetic CFRs (known channel plus noise: the valid mask and residual
   match the truth); a replay cost check.
4. **Sensing metrics (§6)**, offline first (a scorer on records), then live in ISAC_METRICS.
5. **Per-unit cost accounting (§5.2)**: unit table, buckets, per-resource p95/p99; unit tests on the bucket and
   percentile code; replay cost check.
6. **Scheduler (§5.1, 5.3-5.6)**:
   - value, fairness, admission, stability, gates;
   - pure-function unit tests: admission arithmetic, age cap and eligibility, hysteresis, the violation response,
     N = 1;
   - replay, then sens6, then NSA single-PCI and multi-PCI.
   - Headroom calibration.
7. **Decide on scan suspension (§7)** from the measurements of step 6, as a separate design if needed.

Every step: ctest; build under `flock -s heavy.lock`, never while nr-uesoftmodem runs; all 17 module .so files
present; legacy (no `mode` key) unchanged, checked by the existing regression gate.

## 11. Design decisions to confirm (not yet confirmed by the operator)
- Missing key = legacy.
- Explicit keys narrow but never widen a mode.
- The binary accept-path record.
- `type="config"` records with `cfg_ref`.
- `T_check` 2 s / every 32nd grant, K = 4.
- DCI 2_x out of v1.
- Bucket size 50 ms; decision period 4 s.
- Headroom starts at 40 % per resource.
- Age cap x2 at 10 s; hysteresis 20 %; at most 2 enables per decision.
- Validity thresholds calibrated on sens6.
- Traffic replay pass bounds (90 % solved RNTIs, drops within mean + 1 sigma).

## 12. Non-goals and anti-goals
- Not in scope:
  - detection, tracking or SLAM, or any sensing algorithm;
  - blind PUCCH in v1;
  - DCI 2_x in v1;
  - scan suspension in the first sensing release.
- No new env/CLI/-D flags. No per-cell or per-operator tuning. No fixed cores per activity. No GB10-specific tuning.
  Sensing stays off the GPU.
- Anti-goals:
  - a mode that "works" by dropping messages inside enabled units, or by losing DCIs to sensing contention;
  - trading PDSCH CRC for latency on any decoded grant (memory pdsch-data-for-sensing);
  - treating `candidate` or `unsolved` fields as ground truth;
  - pooling DL/UL, different transmitters or different precoding as one channel.
- Legacy behaviour (no `mode` key) stays unchanged.

## 13. Left to the builder
- Exact JSON key names beyond those listed.
- The binary record struct layout.
- Where the unit table lives.
- Whether the scheduler runs in the control thread or its own thread.
- Fixture design.
- The scorer for §6.

## Addendum 2026-11-05 (operator): adaptive continuity (plan S9)
- **Goal:** keep having data to process, while still preferring big grants.
- **Continuity target:** a valid CFR in every 25 ms window per PCI (cfg key `sense_cont_target_ms`, default 25).
- **Per-cell adaptive PRB floor T_cell:**
  - It starts at `sense_min_prb` (128).
  - When a cell's p95 gap between valid CFRs of >= T_cell PRBs exceeds the target over a decision window, T_cell
    steps down: 128, 96, 64, 48, then the floor `sense_min_prb_floor` (default 32).
  - When the rate of grants >= the next higher step would still meet the target with margin, it steps back up.
  - It uses the S6 streak and hysteresis, so it does not flap.
- **"Wideband" keeps its meaning:** valid_cfr_prbs >= sense_min_prb (128). T_cell only selects which grants are
  decoded and exported for continuity.
- **Decode priority:**
  - Grants >= T_cell are decoded for RNTIs with a proven layout, largest first within the budget.
  - Grants of unproven RNTIs are never force-decoded wide. The solver learns them from its normal probes; wide probes
    almost never pass [MEASURED OTA 2026-11-05].
- **Cross-cell:** a PCI in a continuity gap gets precedence for the shared pool over a PCI that meets the target.
