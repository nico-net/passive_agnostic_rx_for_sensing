# Passive Blind PUCCH Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In traffic mode, extract every UE's HARQ-ACK, SR and CSI from PUCCH. Look only where the decoded DL DCIs
predict a PUCCH, learn the resources from DL-derived labels, and never print a guessed field.

**Architecture:** Every stage is a pure, unit-testable module:
- Level-0 labels from the DCI stream;
- a format 0/1 cyclic-shift energy map, computed only at predicted slots and PRBs;
- per-RNTI and per-cell resource learning with a significance test;
- format 0/1 decode with derived DTX detection;
- format 2/3/4 located through the DM-RS first, then RNTI-hypothesis decoding confirmed by the Polar CRC.

A background worker per cell, under a CPU budget, runs them on the existing UL-slot capture. It emits "uci"
records.

**Tech Stack:** C11 (OAI UE PHY), C++17 GoogleTest, CMake/Ninja, Python 3 (scorer).

**Spec:** `docs/superpowers/specs/2026-11-05-pucch-extraction-design.md`. Parents:
`2026-11-03-two-modes-design.md` and `2026-11-04-param-extraction-addendum.md`.

## Global Constraints

- The project hard rules apply (CLAUDE.md, PROJECT_MEMORY §0.1):
  - sens6 frozen paths untouched; new configs `.cfg`; gNB logs are validation ground truth only;
  - evidence labels on every claim, naming the host;
  - never build while nr-uesoftmodem runs; SIGINT only; ISAC_RX_BRANCH_FO unset;
  - `git add` explicit paths, never `-A` or stash.
- No new env, CLI or -D flags. No per-cell or per-operator tuning. No fixed cores. No GPU is needed (CPU only).
- Portable: it builds and passes on the DGX (aarch64, USRP) and on sens6 (x86, `-DOAI_USRP=OFF`, RTX 4060 Ti).
- Traffic mode only. Legacy and sensing outputs stay byte-identical (tested).
- Never guess: a field is printed only from a learned or verified resource; otherwise it is null.
- Significance: alpha = NBS_SIGNIF_ALPHA (nr_blind_stats.h). Thresholds are derived; none is tuned on DEIB cells.
- Threads: never on the RX or scan threads. Never take g_phase2_mu or the UL-layout lock. Per-cell state through
  NR_CELL_STATIC with mpci_statics rows. Reset on identity change through the existing epoch listener.
- Per-cell CPU budget: 10 ms/s, oldest-due first, deferrals counted (the E1/E3 budget pattern).

## Review Focus

1. **A UE with K1 values or PRI that E1 has not proven** (fields null): the PUCCH must not be predicted from a
   guessed K1 → no work, counted (P4 `UnprovenK1NoPrediction`).
2. **Two UEs sharing a PRB with different cyclic shifts in one slot:** the resources are attributed separately and
   never swapped (P3 `TwoUesSamePrbDistinctShifts`).
3. **Frequency hopping (second hop on a mirrored PRB):** both hops are combined, and a hop hypothesis that is not
   learned prints nothing (P4 `HopPrbLearnedNotAssumed`).
4. **DTX at low SNR:** never reported as NACK; undecided instead (P5 `LowSnrDtxNotNack`).
5. **Multi-PCI:** the same RNTI value on two cells keeps its resources and labels separate (P4
   `PerCellResources`).

---

### Task P1: Level-0 HARQ-ACK labels from the DCI stream

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pucch_label.{h,c}`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pucch_label_test.cc`

**Interfaces:**
- Produces:
  - `typedef enum { NR_HQ_UNKNOWN=0, NR_HQ_ACK, NR_HQ_NACK } nr_hq_label_t;`
  - `void nr_pucch_label_note_dl(int cell, uint16_t rnti, int pid, int ndi, int rv, int64_t pdsch_abs_slot, int64_t harq_fb_abs_slot, int pri, int dai);`
  - `int nr_pucch_label_take(int cell, uint16_t rnti, int64_t fb_slot, nr_hq_label_t *out, int max);`. Returns the
    labels resolved for that feedback slot, in DAI order.
- Rules:
  - the next DCI on the same pid with NDI toggled means ACK;
  - the same NDI with a new RV means NACK;
  - nothing within 2 × the max K1 window means UNKNOWN;
  - a pid whose E1 field is not proven contributes nothing.

- [ ] **Step 1: Write the failing tests**:
  - `NdiToggleIsAck`
  - `SameNdiNewRvIsNack`
  - `MissingFollowupUnknown`
  - `UnprovenPidIgnored`
  - `DaiOrdersMultipleAcksInOneSlot`
  - `PerCellSeparation`
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it** (pure, bounded ring per cell and RNTI).
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit** with the message `feat(pucch): level-0 HARQ-ACK labels from the DL DCI stream`.

### Task P2: Format 0/1 cyclic-shift energy map (pure)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pucch_f01_map.{h,c}`
- Test: `tests/nr_pucch_f01_map_test.cc`

**Interfaces:**
- Produces:
  - `typedef struct { uint16_t prb; uint8_t sym0, nsym, occ; float e[12]; float noise; } nr_pucch_f01_cell_t;`
  - `int nr_pucch_f01_map(const c16_t *const *rxF, int n_ant, int n_sc, int slot, uint16_t n_id, const uint16_t *prbs, int n_prb, uint8_t sym0, uint8_t nsym, bool f1, nr_pucch_f01_cell_t *out, int max);`
- Method: base sequence per TS 38.211 5.2.2 with group/sequence hopping from n_id. Divide, then a 12-point IDFT;
  for F1, the OCC transform across symbols. Reuse the pucch_rx.c maths; no gNB structs.

- [ ] **Step 1: Write the failing tests** on synthetic grids (4 RX):
  - `SingleUeShiftRecovered` at every m_cs
  - `TwoUesSamePrbDistinctShifts`
  - `F1OccSeparated`
  - `NoiseFloorEstimated` within 10 %
  - micro-benchmark in ns per PRB
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it** (pure, no allocation).
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit** with the message `feat(pucch): format 0/1 cyclic-shift/OCC energy map`.

### Task P3: Resource learning per RNTI and families per cell

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pucch_learn.{h,c}`
- Test: `tests/nr_pucch_learn_test.cc`

**Interfaces:**
- Consumes: P1 labels, P2 cells.
- Produces:
  - `typedef struct { uint8_t format, sym0, nsym, occ, m0; uint16_t prb, prb_hop; } nr_pucch_res_t;`
  - `void nr_pucch_learn_observe(int cell, uint16_t rnti, int pri, const nr_hq_label_t *lab, int n, const nr_pucch_f01_cell_t *map, int n_map);`
  - `bool nr_pucch_learn_get(int cell, uint16_t rnti, int pri, nr_pucch_res_t *out);`. True only when LEARNED.
- Rule: per (RNTI, PRI), score each hypothesis by label-consistent peaks (ACK and NACK shift offsets per TS 38.213
  9.2.3); LEARNED when exactly one survives at alpha. Families are seeded from learned RNTIs of the same cell.
  The SIB1 common resources (Table 9.2.1-1) are exact hypotheses.

- [ ] **Step 1: Write the failing tests:**
  - `LearnsTrueResource`
  - `TwoHypothesesStayCandidate`
  - `HopPrbLearnedNotAssumed`
  - `PerCellResources`
  - `FamilySeedsNewRnti`
  - `CommonResourceFromSib1`
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit** with the message `feat(pucch): per-RNTI resource learning with significance, cell families`.

### Task P4: Prediction, worker, budget and format 0/1 decode in the receiver

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pucch_passive.{h,c}`
- Modify:
  - the UL-slot capture site (executables/nr-ue.c ~823, the blind-SRS capture) to hand traffic-mode cells' predicted
    slots to the worker;
  - `nr_traffic_record.{h,c}` (record type "uci");
  - `tools/mpci/mpci_statics.tsv`
- Test: `tests/nr_pucch_passive_test.cc`

**Interfaces:**
- Consumes: P1-P3, `nr_passive_acq_tdd_slot_ul_capable`, DCI-record join keys.
- Rules:
  - a slot or PRB is processed only if a decoded DCI points to it with a proven K1 (and PRI where needed);
  - F0/F1 HARQ-ACK is decoded with a learned resource; DTX is decided against the map's noise floor with a derived
    threshold;
  - SR energy is checked on the learned SR resource at its learned period (nr_periodic_track);
  - per-cell budget of 10 ms/s, oldest-due first, deferrals counted;
  - traffic mode only.

- [ ] **Step 1: Write the failing tests:**
  - `UnprovenK1NoPrediction`
  - `NoDlNoWork` (zero calls)
  - `LowSnrDtxNotNack`
  - `BudgetDefersOldestFirst`
  - `LegacyAndSensingUntouched`
  - `UciRecordJoinsDci`
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it** (worker thread SCHED_OTHER nice 19, named "pucchBg"; no scan/RX locks).
- [ ] **Step 4: Run the tests, the full build and ctest, and the rfsim 4-RX legacy gate** (DGX) or the x86
  equivalent (sens6).
- [ ] **Step 5: Commit** with the message `feat(pucch): predicted-slot worker, format 0/1 HARQ-ACK/SR decode, uci records`.

### Task P5: Format 2/3/4 (DM-RS first, RNTI hypotheses, Polar CRC)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pucch_f234.{h,c}`
- Test: `tests/nr_pucch_f234_test.cc`

**Interfaces:**
- Produces:
  - `int nr_pucch_f2_locate(const c16_t *const *rxF, int n_ant, int n_sc, int slot, uint16_t n_id, const uint16_t *prbs, int n_prb, uint8_t sym0, uint8_t nsym, float *score, int max);`
  - `int nr_pucch_f2_decode(..., const uint16_t *rntis, int n_rnti, nr_uci_t *out);`. CRC-verified payload only
    (Polar ≥12 bits). Reed-Muller payloads are printed only when they agree with the P1 labels.
- Formats 3/4: the same API shape, with DFT-s DM-RS.

- [ ] **Step 1: Write the failing tests:**
  - `F2DmrsLocatesWithoutRnti`
  - `F2PolarCrcConfirmsRntiAndLength`
  - `WrongRntiNeverConfirms`
  - `RmPayloadNeedsLabelAgreement`
  - `F3DftsLocate`
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it**, reusing the pucch_rx.c and polar decoder maths.
- [ ] **Step 4: Run the tests, the build and ctest.**
- [ ] **Step 5: Commit** with the message `feat(pucch): format 2/3/4 DM-RS-first locate and CRC-verified UCI decode`.

### Task P6: Validation (sens6 ground truth, DEIB OTA A/B)

**Files:**
- Create: `tools/traffic/score_uci.py` (+ test); report `docs/superpowers/reports/twomodes/PUCCH-<date>.md`.

- [ ] **Step 1: sens6 lab** (OCUDU ZMQ bed and LiteOn + COTS UEs). The gNB UCI log is ground truth, validation
  only. Per format:
  - detection;
  - false alarm;
  - ACK/NACK/DTX confusion;
  - resource correctness;
  - CSI correctness;
  - CPU per cell.
  Label `[MEASURED OTA sens6]` or `[SIM VERIFIED sens6]`.
- [ ] **Step 2: DEIB OTA on the DGX**, daytime, alternating A/B against traffic mode without PUCCH, ≥ 3 runs per
  arm, single PCI and pair:
  - agreement with the P1 labels;
  - the no-loss rule (RNTIs with ≥ 2 DCIs, DCI records, SOLVED, full-field records) and scan/decode drops not worse;
  - CPU.
- [ ] **Step 3: Commit** the report and the scorer.
