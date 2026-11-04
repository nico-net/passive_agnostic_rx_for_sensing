# CRC-free DCI Parameter Extraction (E1-E6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Traffic mode decodes every DCI field exactly. Layouts are proven from DCI bits, spec constraints and DM-RS
physics, not from PDSCH/PUSCH CRC trials. The useless probe and UL decodes that starve the multi-PCI scan go away.

**Architecture:**
- The existing cell-wide DCI 1_1 resolver keeps its job: it prunes layout sums by length, RIV, antenna-port rows and
  TDA.
- A new pure prover splits each surviving layout into individual fields and tests them against per-RNTI payload
  banks (DAI counter, K1 vs TDD, MCS-table exclusion).
- A new pure DM-RS scorer tests full layout hypotheses on the grid.
- The solver gains a CRC-free SOLVED path with evidence levels, and families of solved layouts per cell.
- Decode triage stops probing RNTIs whose layout cannot yet be proven cheaply.

**Tech Stack:** C11 (OAI UE PHY), C++17 GoogleTest, CMake/Ninja, Python 3 (scorer).

**Spec:**
- `docs/superpowers/specs/2026-11-04-param-extraction-addendum.md` (approved 2026-11-04);
- parent `docs/superpowers/specs/2026-11-03-two-modes-design.md`.

**Inventory of existing code** (read before each task; file:line as of 28fb07a820; all under
`openair1/PHY/NR_UE_TRANSPORT/`):

- `nr_pdcch_dci11_layout_sweep.{c,h}` (SW): the DL 1_1 resolver.
  - `nr_dci11_layout_t` (SW.h:103) holds group sums `bwp_ind, pre_mcs, pre_ant, ant_ports, post_ant, tail`.
    `pre_ant` is TB2 + HARQ pid + DAI + TPC + 2nd TPC + PRI + K1 + the Rel-16/17 extras.
  - `nr_dci11_offsets_t` (SW.h:137).
  - Switch tables `kBwpInd..kPucchCell` (SW.c:86-115).
  - Pruning: `nr_dci11_resolver_observe` (SW.c:722), `layout_eval` (SW.c:440), `nr_dci11_resolver_observe_rnti`
    (SW.c:792, HARQ-pid step). Wilson helper (SW.c:487, static).
- `nr_traffic_dci_fields.c:22` `nr_dci11_harq_fields`: splits `pre_ant` only when the split is unique.
- `nr_pdsch_solver.{c,h}` (SOL):
  - `significant()` (SOL:547) and `chance_p`/`poisson_tail` are static. `hq_score` (SOL:470).
  - `seed_tuples` (SOL:930, the cell prior); `plan_slot` (SOL:1316); `search_backoff` (SOL:903).
  - PARK through `note_dmrs` (SOL:671); `nr_pdsch_solver_layout_state` (SOL:2578).
- `nr_pdcch_blind_monitor_rt.c` (RT):
  - `dmrs_fit_classes` (RT:187) and `dmrs_sym_rx` (RT:150): a one-symbol PRB occupancy fit.
  - Probe gate and budget (RT:1457-1469, 9606, 9729-9753). `traffic_decode_wanted` call (RT:~9748): probes always
    run.
  - `pdsch_dmrs` CFR tap (RT:~10186-10260): its own FEP and `nr_pdsch_channel_estimation`.
- `nr_pdsch_dmrs_scan.{c,h}`: `nr_dmrs_scan_prb_score_gold` (scan.c:240), which scores one ID on a CRB list.
- `nr_pdcch_ul_layout_lock.{c,h}`: `nr_ul_layout_fit_mcs` (lock.c:94), a bank-plus-significance pattern;
  `nr_ul_layout_infer_k2` (lock.c:376); LLR/SPRT decide helpers.
- `nr_passive_acq_state.{c,h}:155-172`: per-cell TDD from SIB1 (`nr_passive_acq_tdd_known`, ...).
  `nr_tdd_pattern.h`: `nr_tdd_slot_direction`.
- `nr_pdsch_solver_mcs_qr` (SOL:280): (Qm, R) per table and MCS.

## Global Constraints

- Everything in the traffic plan's Global Constraints applies (`docs/superpowers/plans/2026-11-03-traffic-mode.md`):
  - no new env, CLI or -D flag;
  - legacy (no `mode` key) unchanged in effect;
  - no per-cell or per-operator tuning, no fixed cores, sensing off the GPU;
  - new configs use `.cfg`; sens6 frozen paths untouched;
  - stop receivers with SIGINT only; `ISAC_RX_BRANCH_FO` unset;
  - `git add` explicit paths, never `-A` or stash; no push;
  - builds under `flock -s rr-orchestration/locks/heavy.lock`; 24 `.so`.
- New behaviour is active only in `mode = "traffic"` (and later sensing). Legacy calls none of it.
- **Never guess.** A field is decoded only from a proven layout. Otherwise it is null, and the raw `dci_bits` stay
  in the record.
- Significance: a layout is SOLVED only when exactly one split survives and its chance probability is below
  alpha = 1e-3. This is the solver's `SIGNIF_ALPHA`; reuse it, do not invent a new constant.
- Thresholds come from general principles or the sens6 lab, never from DEIB/OTA data (operator 2026-11-03).
- FDD is out of scope; the K1 test may rely on the SIB1 TDD pattern (operator 2026-11-04).
- Traffic-mode PUSCH decode is off by default (operator 2026-11-04). An explicit `pdcch_blind_monitor_ul_pusch` still
  narrows (cad5941532).
- Every new per-RNTI or per-family structure is per cell (`NR_CELL_STATIC`). Reset on identity change through the
  existing epoch listener (RT:~2022).
- Evidence labels on every claim; results per cell, never merged.

## Review Focus

1. **RRC reconfiguration of a SOLVED(bits/phys) layout mid-run.** Constraint violations rise, so the layout must
   reopen and stop decoding fields wrongly. Test in E1 (`ReopensOnViolations`) and E3.
2. **Two splits that differ only in footprint-free fields, on a stream that cannot separate them.** The layout must
   stay CANDIDATE with those fields null. Test in E1 (`IndistinguishableStaysCandidate`).
3. **TDD pattern not yet known (SIB1 not decoded).** The K1 test abstains instead of refuting. Test in E1
   (`UnknownTddAbstains`).
4. **The same RNTI value on two cells.** Banks and families never mix. Tests in E1 (`PerCellBanks`) and E3
   (`FamiliesPerCell`).
5. **A one-DCI RNTI with no family match.** No probe; the record is written with raw bits and null fields. Test in E4
   (`SingleDciNoProbe`).

---

### Task E1: DL 1_1 field prover (Level 0, pure) and wiring of proven fields

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_dci11_prove.{h,c}`
- Modify:
  - `nr_traffic_dci_fields.c` (`nr_dci11_harq_fields` uses a proven split when one exists);
  - `nr_passive_acq_state.{c,h}` (add the UL-capable getter);
  - RT (feed the bank at DL 1_1 accept, traffic mode only);
  - `nr_traffic_record.{h,c}` (record field `layout_evidence`);
  - `tools/mpci/mpci_statics.tsv` (rows for new per-cell statics).
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_prove_test.cc` (register like `test_nr_traffic_record`).

**Interfaces:**
- Produces:
  - `typedef struct { uint8_t tb2, harq, dai, tpc, tpc2, pri, k1, ext[6]; } nr_dci11_split_t;`. Widths of the
    `pre_ant` sub-fields in TS 38.212 7.3.1.2.2 order; `ext` = the Rel-16/17 extras in spec order.
  - `int nr_dci11_splits(const nr_dci11_layout_t *l, nr_dci11_split_t *out, int max);`. Enumerates every spec-legal
    split whose sum equals `l->pre_ant`, using the SW switch tables.
  - `typedef struct { nr_dci_bits_t payload; uint16_t len; int64_t abs_slot; } nr_dci11_obs_t;`
  - `typedef bool (*nr_ul_slot_fn)(int64_t abs_slot, int *known);`. Returns UL-capable (UL or MIXED). `*known = 0`
    means abstain.
  - `int nr_dci11_prove(const nr_dci11_layout_t *l, const nr_dci11_offsets_t *o, const nr_dci11_obs_t *bank, int n,
    nr_ul_slot_fn ul, nr_dci11_split_t *alive, double *chance_p, int max);`. Returns the number of surviving splits;
    `chance_p[i]` is the probability that split i survives by chance.
  - `bool nr_passive_acq_tdd_slot_ul_capable(int64_t abs_slot, int *known);` (wrapper over `nr_tdd_slot_direction`).
  - Record `layout_evidence`: `"bits"`, `"phys"`, `"crc"` or null.
- Tests per split:
  - **HARQ pid:** step 1..3 between an RNTI's consecutive grants (reuse the `fit_mcs` idea).
  - **Counter DAI:** for DCIs sharing one HARQ-ACK slot (n + L[K1 idx]) the value steps +1 mod 4 in slot order and
    restarts at 1 for a new feedback slot. Null rate 1/4 per step. A split with DAI width 0 is not tested here.
  - **K1:** there must be a table L[idx] in 0..15 such that every DCI lands on a UL-capable slot. The feasible set
    per idx is the intersection over its DCIs; an empty set refutes. Abstain when `known == 0`.
  - **MCS table:** on a new TB (NDI toggled for that pid under the split's reading), MCS 28 excludes table 2
    (qam256). The result goes into `nr_dci11_split_t`-level evidence that E2 completes. It never refutes a split.
- Bank: per cell and RNTI, the last `NR_DCI11_BANK` = 64 payloads (ring). Fed only in traffic mode.

- [ ] **Step 1: Write the failing tests.** Use a synthetic stream generator: a known RRC config (widths), scheduler
  traces with HARQ processes, a DAI per HARQ-ACK window, K1 from a list, and a TDD pattern DDDSU.
  - `EnumeratesSpecSplits`: for `pre_ant` = 9 and 38 (the Task 4 unique cases) exactly 1 split; for a typical
    ambiguous sum, all legal splits are listed and match a hand count.
  - `ProvesTrueSplit`: 200 DCIs of 8 UEs give exactly one alive split equal to the truth, with chance_p < 1e-3.
  - `DaiRefutesWrongWidth`: same sum, truth DAI 2 + K1 2 vs a DAI 4 + K1 0 hypothesis. The wrong one is refuted.
  - `K1RefutesOffUl`: a split whose K1 reading sends DCIs to DL-only slots is refuted, and the recovered L matches
    the truth list.
  - `UnknownTddAbstains`: `known = 0`, so K1 refutes nothing and the result equals the run without the K1 test.
  - `IndistinguishableStaysCandidate`: two splits that differ only in TPC vs tpc2 widths on a stream with constant
    values. Both alive, so no SOLVED.
  - `ReopensOnViolations`: a proven split, then 64 DCIs from a different config. Re-proving the bank refutes it.
  - `PerCellBanks`: RNTI 0x4601 on cell 0 and cell 1 with different configs gives two different proven splits.
  - `Mcs28ExcludesQam256`.
- [ ] **Step 2: Run them to verify they fail** (compile failure counts).
- [ ] **Step 3: Implement** `nr_dci11_prove.c` (pure, no allocation in `nr_dci11_prove`), the TDD getter and the
  wiring:
  - The bank is fed at the DL 1_1 accept point (traffic mode only).
  - The prover runs on the scan consumer when an RNTI's bank grows by 16, never on the RX thread.
  - `nr_dci11_harq_fields` reads through a proven split. The record's `layout_evidence` is `"bits"` when E1 alone
    proved it.
- [ ] **Step 4: Run the tests, then the full build and ctest** (known baseline failures only).
- [ ] **Step 5: Commit** with the message `feat(traffic): DCI 1_1 field prover (DAI, K1 vs TDD, HARQ, MCS table), proven fields in records`.

### Task E4: Decode triage (quick load cut) and traffic PUSCH off

**Files:**
- Modify: `nr_traffic_decode.{h,c}`, RT (probe gate at ~9729-9753), `openair1/PHY/NR_UE_ISAC/nr_isac_mode.c`
  (traffic `pusch_decode` default 0).
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_traffic_decode_test.cc`, `nr_isac_mode_test.cc`.

**Interfaces:**
- Consumes: E1 `nr_dci11_prove` results (number of alive splits per RNTI).
- Produces: `bool nr_traffic_probe_wanted(int cell, uint16_t rnti, int dir, int n_dci_seen, int n_alive, bool family_match, float snr_db, int mcs, int mcs_table);`
- Rules (traffic mode only):
  - Probe only when (`n_dci_seen` >= 2 or `family_match`), `n_alive` >= 1, and the SNR supports the MCS. The SNR
    check uses the existing gate margin (`nr_snr_gate_margin_db_cell`); it adds no new threshold.
  - Exponential backoff per RNTI after consecutive probe failures: probe 1 grant in 2^k, k <= 4. A CRC pass resets
    k to 0.
  - Traffic `pusch_decode` default 0. The UL MCS table comes from E1/E2 evidence; `qm` and `tbs` stay null until
    it is known.
  - DCI records are written at accept regardless (unchanged).

- [ ] **Step 1: Write the failing tests:**
  - `SingleDciNoProbe`: `n_dci_seen` = 1, no family, so no probe.
  - `SecondDciProbes`.
  - `BackoffDoublesAndResets`: failures give the pattern 1, 2, 4, 8, 16, 16; a pass resets it.
  - `LowSnrNoProbe`.
  - `TrafficPuschDefaultOff`: `resolve("traffic", 0, -1, -1)` gives pusch 0. An explicit pusch 1 is narrowed to 0
    with the narrowed flag (traffic never widens).
  - `LegacyUntouched`: in legacy mode the function is never consulted (test the call-site guard via the mode check
    helper).
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it.** At the RT probe gate: `if (nr_isac_mode() == NR_ISAC_MODE_TRAFFIC &&
  !nr_traffic_probe_wanted(...))`. Then skip the variants with `nr_pdsch_solver_feedback(token, false, false)`, as
  Task 7 did, and count `probe_skip`. Split the ISAC_METRICS counters into `check_dl, check_ul, probe_dl, probe_ul,
  probe_skip`.
- [ ] **Step 4: Run the tests, then the build and ctest.**
- [ ] **Step 5: Commit** with the message `feat(traffic): probe triage (seen>=2 or family, SNR, backoff), PUSCH decode off in traffic mode`.

### Task E2: Full-layout DM-RS scorer and constellation Qm (Level 1, pure)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_dl_layout_score.{h,c}`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_dl_layout_score_test.cc`

**Interfaces:**
- Produces:
  - `typedef struct { const uint16_t *crb; int n_crb; uint16_t dmrs_sym_mask; uint8_t dmrs_type, cdm_groups_mask, cdm_no_data; uint32_t nid; uint8_t nscid; } nr_dl_hyp_t;`.
    The PRBs come from the RIV and VRB-to-PRB mapping; `dmrs_sym_mask` from TDRA S/L, the type-A position and
    `add_pos` (`nr_pdsch_solver_dmrs_mask`).
  - `typedef struct { const c16_t *const *rxF; int n_ant; int n_sc; int symbols; float noise_var; } nr_dl_obs_t;`
  - `typedef struct { float coh; float occ_f1; float comb_ratio; int qm; float qm_llr; } nr_dl_score_t;`
  - `void nr_dl_layout_score(const nr_dl_hyp_t *h, const nr_dl_obs_t *o, nr_dl_score_t *s);`. Per-PRB coherence
    reuses `nr_dmrs_scan_prb_score_gold` on every DM-RS symbol in the mask; F1 of occupied PRBs against
    `h->crb`; comb-1/data energy ratio for CDM groups without data.
  - `int nr_qm_classify(const c16_t *eq, int n, float noise_var, float *llr);`. Returns 2, 4, 6 or 8, or 0 when
    undecided. It is a likelihood over the QAM constellations of TS 38.211 (no thresholds tuned on OTA data).

- [ ] **Step 1: Write the failing tests** on synthetic grids (4 RX, 273 PRB numerology):
  - `TrueHypothesisScoresHighest`: the truth against wrong RIV, wrong symbols, wrong add_pos and wrong CDM groups.
    Truth is best in 100/100 seeds at 10 dB.
  - `NoiseScoresLow`.
  - `QmClassifies`: QPSK/16/64/256 at 25 dB, correct in >= 99 %. At 5 dB, 256 vs 64 returns 0 (undecided), never a
    wrong certain answer.
  - `CombRatioSeparatesCdmNoData`.
  - Micro-benchmark: ns per hypothesis for 100 PRB, 2 DM-RS symbols.
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it.** Pure, no allocation; sensing stays off the GPU.
- [ ] **Step 4: Tests pass; build; ctest.**
- [ ] **Step 5: Commit** with the message `feat(traffic): full-layout DM-RS scorer and constellation Qm classifier`.

### Task E3: CRC-free SOLVED path and layout families

**Files:**
- Modify: SOL (`significant` path; new phys acceptance; families extending `seed_tuples`); RT (call E2 on the scan
  consumer for alive E1 splits, sharing the `dmrs_sym_rx` FEP).
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_solver_test.cc` (new cases).

**Interfaces:**
- Consumes: E1 alive splits; E2 `nr_dl_layout_score`, `nr_qm_classify`.
- Produces:
  - `int nr_pdsch_solver_layout_evidence(uint16_t rnti);`. Returns 0 none, 1 bits, 2 phys, 3 crc.
  - `bool nr_pdsch_solver_family_match(uint16_t rnti, nr_dci_bits_t payload, uint16_t len);`
- Phys acceptance:
  - Per RNTI, an SPRT over grants on the E2 score of each alive hypothesis. Reuse the UL lock LLR decide helpers
    and `NSE_BETA`/alpha 1e-3.
  - A single winner becomes SOLVED with evidence phys. The MCS table comes from the Qm majority via
    `nr_pdsch_solver_mcs_qr`.
- Families:
  - Per cell and DCI length: solved tuples (layout, split, add_pos, MCS table, scrambling) ranked by members.
  - A new RNTI whose E1 bank (even 1 DCI) is consistent with a family and whose first grant's E2 score passes is
    accepted as SOLVED(phys), with no probe.
  - Reset on identity change.
- Reopen: E1 violations or the E2 score failing over the last 16 grants reopen the layout. That is the existing
  `nr_pdsch_solver_reopen` path.

- [ ] **Step 1: Write the failing tests:**
  - `PhysSolvesWithoutCrc`: synthetic grants and E2 scores give SOLVED with evidence 2 and zero CRC feedback.
  - `FamilyAcceptsNewRntiOneDci`.
  - `FamilyRejectsInconsistentBits`.
  - `FamiliesPerCell`.
  - `ReopensOnScoreDrop`.
  - `CrcStillConfirms`: a CRC pass upgrades the evidence to 3.
- [ ] **Step 2: Verify they fail.**
- [ ] **Step 3: Implement it.** E2 runs on the scan consumer, never on the RX thread, and reuses the cached DM-RS
  symbol FEP.
- [ ] **Step 4: Tests; build; ctest; legacy rfsim 4-RX gate (both arms) under `flock -x /tmp/td_measure.lock`.**
- [ ] **Step 5: Commit** with the message `feat(traffic): CRC-free SOLVED via DM-RS physics, per-cell layout families`.

### Task E5: One DM-RS estimate for layout and sensing

**Files:**
- Modify: RT (`pdsch_dmrs` CFR tap at ~10186-10260, `dmrs_sym_rx` at ~150).
- Test: an existing sense-record test plus a new `nr_dmrs_share_test.cc` if a seam exists. Otherwise an rfsim
  record-equality check in Step 4.

**Interfaces:**
- The CFR tap reads the FEP'd DM-RS symbol from the same per-slot cache as E2/E3, instead of running its own
  `nr_slot_fep`.
- CFR values are unchanged within floating-point equality on the same input.

- [ ] **Step 1: Write the failing test:** the tap and the scorer request the same (slot, symbol), which gives exactly
  one FEP (count via a test counter).
- [ ] **Step 2: Verify it fails.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: rfsim 4-RX run:** `pdsch_dmrs` CFR records are byte-equal before and after, on the same seed and
  capture; `sense_prod_us_pdsch_dmrs` is lower.
- [ ] **Step 5: Commit** with the message `perf(sense): one DM-RS FEP shared by the layout scorer and the CFR tap`.

### Task E6: Validation (replay ground truth, OTA A/B)

**Files:**
- Create: `tools/traffic/score_fields.py` (+ `test_score_fields.py`); report
  `docs/superpowers/reports/twomodes/TRAFFIC-FIELDS-<date>.md`.
- Reuse: `tests/passive_rx/traffic/ota_traffic_ab.sh`, `tools/traffic/score_traffic_ab.py`.

**Steps:**
- [ ] **Step 1: Replay ground truth.** Use the sens6 OCUDU captures with gNB logs (validation only, never receiver
  input). Every field of every SOLVED layout must equal the gNB value. Report per field: decoded, correct and null
  rates, per capture. `[SIM VERIFIED]`/replay label.
- [ ] **Step 2: DEIB OTA daytime A/B**, traffic at the E3/E5 head against traffic at 4759260bbf, >= 3 runs per arm,
  cases single 415, single 923 and the pair. Per cell:
  - DCI records >= baseline;
  - decodes per DCI lower;
  - CPU lower;
  - scan `drop_pct` lower;
  - constraint violations on SOLVED layouts = 0;
  - share of 1_1 records with all fields non-null.
  Labels `[MEASURED OTA]`; never claim absolute recall.
- [ ] **Step 3: Commit** the report and the scorer.

## Self-review notes

- Spec §2-§8 map to tasks:
  - §3 L0 -> E1;
  - §3 L1 -> E2 and E3;
  - §3 L2 and §5 -> E4;
  - §4 -> E3;
  - §6 -> E5;
  - §8 -> E6.
- Order follows spec §7: E1 -> E4 -> E2 -> E3 -> E5 -> E6.
- E4 runs before E2/E3, with E1's `n_alive` and no family yet: `family_match` is false until E3 lands.
- Types: `nr_dci11_split_t` (E1) is used by E3; `nr_dl_hyp_t` and `nr_dl_score_t` (E2) by E3.
