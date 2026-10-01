# Reconfiguration Robustness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Non-Claude agents (e.g. Codex) must follow the same per-task discipline manually: failing test first, implement, green, commit, write a per-task report, and stop for review before the next task.

**Goal:** Detect and recover from RRC reconfigurations the passive receiver cannot read — on **SA and NSA** cells over the air — by lifting the DCI size cap to 140 bits, re-learning per-RNTI DCI lengths, giving the CORESET bank a life cycle, and introducing one cell-level `CellConfigEpoch` authority whose consumers turn learned state into hints after a change.

**Architecture:** Phase 1 (R1–R6) is local to the blind-PDCCH modules and independently testable; R15 (optional) adds a verified, batched PDCCH GPU path if profiling justifies it. Phase 2 (R7–R11) adds a single pure epoch module (`nr_passive_cfg_epoch`) and connects its consumers one by one. Phase 3 (R12–R14) validates with an offline mid-stream reconfiguration replay, the live SA bed (+ NSA-like arm) and a soak.

**Tech Stack:** C11 (OAI style), C++17 gtest, CMake/Ninja, Python 3 stdlib, OAI rfsim/SA beds.

**Spec:** `docs/superpowers/specs/2026-10-01-reconfiguration-robustness-design.md` (frozen 2026-10-01; read fully). Background: `PROJECT_MEMORY.md` §11.6–§11.11, §23.5–§23.8, §24 K10, K11, K37; levers spec §4.6 (CellFieldBook) and §9 (compute modes).

## Global Constraints

- **SA and NSA (operator rule):** no component may require SIB1, CORESET#0, SI-RNTI, P-RNTI paging or NR-side RA; those are optional evidence (spec §3.6, §4.6). Every live validation has an NSA-like arm.
- **DCI payload bound:** `NR_DCI_MAX_PAYLOAD 140` (A bits; K = A + 24 = 164 into polar). Search range for 1_1/0_1 lengths: 30..140 unless an earlier lower bound already exists in the code (keep the existing minimum).
- **One epoch authority:** only `nr_passive_cfg_epoch` increments an epoch; every consumer reads it; learned records carry `{value, epoch_learned, verification_state}`; stale ⇒ `HINT`.
- **Classes:** HARD_RESET (identity change: PCI, carrier/Point A/SSB ARFCN), HARD_REVERIFY (MIB change, SIB1 semantic change, SI-modification at the next modification boundary, continuity loss logged `CONTINUITY_LOSS`), SOFT (BWP change, ≥ 2 converged RNTIs reopened/SUSPECT within 2 s).
- Re-verify before discarding (HARD_REVERIFY and SOFT); HARD_RESET never carries dedicated state across identities.
- All new behaviour behind flags defaulting to today's behaviour (`ISAC_RECONF=0`), **except** the 140-bit capacity (pure extension).
- Targets: soft recovery ≤ 10 s, hard ≤ 30 s at ≥ 100 grants/s; 0 stale winners; ≤ 1 false SOFT trigger per hour and 0 hard on a stable cell.
- Repository rules (`CLAUDE.md`): sens6-frozen gate before every commit (`git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`); new configs use `.cfg`; explicit `git add`; no stash; never build while `pgrep -x nr-uesoftmodem`; evidence labels naming the host; regression gate `tests/passive_rx/dgx/rfsim_regress.sh` (DGX thresholds 98 % / 1 %; cloud x86 93 % / 2.5 %).
- Branch: `rr/reconfig-robustness` from `adaptive-rx-UL-DL`. Never push to `adaptive-rx-UL-DL`.
- **Acceleration (levers spec §9, operator 2026-10-01):** reject useless work, share common work, test several hypotheses per grant, batch expensive PHY work on the GPU, avoid cold relearning. Control logic (epoch module, triggers, state machines, CORESET life cycle, length contexts, KL/selection) stays on the CPU. GPU use here: (a) **GPU polar batch** (`nr_polar_gpu_mod`, `NR_GPU_POLAR=1`, bit-exact on GB10) for every DCI-length sweep and re-lock sweep, CPU path as fallback; (b) **idsweep GPU** for the scrambling-ID stage of continuous discovery where applicable; (c) **PDSCH re-learning** after an epoch change uses the levers VERIFY mode + GrantWork + top-K CB0 probes + batched GPU LDPC once `td/convergence-levers` provides them. GPU LDPC/FEP results are not trusted before the levers G1 (K34) / G3 (K35) fixes; the PDCCH GPU front end stays unused (V8).

## Coordination with the levers plan (branch `td/convergence-levers`)

- Phase 1 touches `nr_pdcch_*` files only — no overlap with levers Tasks 1–7, 4b, 4c, F1, F2 except `CMakeLists.txt` test blocks (trivial merge).
- Phase 2 R11 changes `nr_td_fieldbook` (levers Task 3) to mirror the central epoch: do R11 **after** `td/convergence-levers` is merged, or rebase onto it.
- R10 (Technique D consumers) touches `nr_pdsch_config_sweep.c` — coordinate with levers Task 4 / R2 (merge order: levers first).

## Review Focus

1. **NSA cell (no SIB1, no CORESET#0) with a dedicated reconfiguration** — recovery must use only NSA evidence (persistence, CFRA RAR anchor, other banked geometries); pinned in R3 (`SuspectWithoutCoreset0Evidence`) and R14 NSA-like arm.
2. **NSA SCG churn** (RNTIs appear/vanish in bursts) — must NOT fire `DEDICATED_CHANGE_SUSPECTED`; pinned in R8 (`VanishedRntisDoNotTriggerSoft`).
3. **Idle cell for minutes** — no CORESET demotion, no SUSPECT; pinned in R4 (`IdleCellNeverDemotes`) and R3 (`InactiveRntiNeverSuspect`).
4. **SI-modification announcement without an actual SIB1 change** — no epoch bump after the boundary; pinned in R8 (`SiModAnnouncedButUnchangedNoBump`).
5. **A queued decode job straddling an epoch change** — never credited; pinned in R9 (`OldEpochJobDropped`).

---

## PHASE 1 — local, independently testable

### Task R1: DCI payload bit-vector type (Sonnet)

**Files:** Create `openair1/PHY/NR_UE_TRANSPORT/nr_dci_bits.h` (header-only); Test `openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci_bits_test.cc`; Modify `CMakeLists.txt` (test block).

**Interfaces — Produces:**
```c
#define NR_DCI_MAX_PAYLOAD 140   /* TS 38.212: cite the DCI CRC-attachment / polar-coding-for-DCI clauses here */
#define NR_DCI_WORDS 3
typedef struct { uint64_t w[NR_DCI_WORDS]; } nr_dci_bits_t;   /* same packing as polar_decoder_int16's out[] */
static inline nr_dci_bits_t nr_dci_bits_from_u64(uint64_t v);
static inline uint64_t nr_dci_bits_field(const nr_dci_bits_t *b, int len, int msb_pos, int width); /* width <= 64 */
static inline bool nr_dci_bits_eq(const nr_dci_bits_t *a, const nr_dci_bits_t *b);
static inline uint32_t nr_dci_bits_hash(const nr_dci_bits_t *b, int len);
```
- [ ] **Step 1:** Read how existing extractors index the right-aligned `uint64_t` payload (e.g. `nr_pdcch_blind_extract_01` in `nr_pdcch_blind_monitor.c`, `raw_payload` "right-aligned to dci_length" in `nr_pdcch_blind_monitor.h:693`) and how `polar_decoder_int16(int16_t*, uint64_t *out, ...)` packs words for A > 64 (`openair1/PHY/CODING/nrPolar_tools/`). Write the packing rule as a comment at the top of `nr_dci_bits.h`.
- [ ] **Step 2: Failing test** — `nr_dci_bits_field` on a 47-bit payload built with `from_u64` equals the existing 64-bit shift/mask result for every (msb_pos, width) with width ≤ 16; for a 140-bit vector with known bits set at positions 0, 63, 64, 127, 128, 139, the field extraction returns them; `eq`/`hash` sanity.
- [ ] **Step 3–4:** implement, green. **Step 5:** commit `feat(rr): 140-bit DCI payload type (nr_dci_bits_t)`.

### Task R2: Lift the 63-bit cap end to end (Sonnet; Opus review — touches the live blind path)

**Files (touch points, verified 2026-10-01):** `nr_pdcch_dci_length_sweep.h:75` (`NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN 64` → `NR_DCI_MAX_PAYLOAD + 1`); `nr_pdcch_blind_monitor.c` guards `dci_length > 63` at ~3893, 4085, 4121, 4891, 4946, 5064; `nr_pdcch_blind_monitor_rt.c` clamps at ~1280, ~1290 (`x <= 63`), `LANE_BATCH_MAX_LEN 64` ~2009, `SWEEP_BATCH_MAX_LEN 64` ~2214; `uint64_t dci_estimation[2]` → `[NR_DCI_WORDS]` (`dci_nr.c` ~1160, `nr_pdcch_blind_monitor.c` ~3461, ~3515); every `uint64_t payload` / `raw_payload` field and function parameter in `nr_pdcch_blind_monitor.h` (143, 151, 223, 693, 875, 893, 933, 1034, 1078), `nr_pdcch_ul_discovery.h:38`, `nr_pdcch_dci11_layout_sweep.h` (135, 260), `nr_pdcch_joint_live.h:19`, `nr_pdcch_joint_solve.h` (55, 89), `nr_pdcch_discovery_replay.h:13`, `nr_pdsch_passive_queue.h:118` → `nr_dci_bits_t`. Grep again before starting: `grep -rn "uint64_t[^;]*payload\|> 63\|<= 63" openair1/PHY/NR_UE_TRANSPORT`.
- Joint GF(2) solver (`nr_pdcch_joint_solve.c`, `nr_pdcch_gf2_rnti.c`): if its matrices are sized for ≤ 64 payload bits, either extend to 140 or keep it opt-in with an explicit `dci_length <= 64` guard and a one-shot log — **decide by reading the code; record the decision in the commit.**
- [ ] **Step 1: Failing tests** (extend existing gtests): `test_nr_pdcch_blind_monitor` — a synthetic DCI 1_1 of 80 and 140 bits is polar-encoded with the real encoder, decoded through the blind candidate path and its RNTI recovered; `test_nr_pdcch_dci_length_sweep` — the sweep locks length 100 on synthetic evidence; existing ≤ 63 tests unchanged.
- [ ] **Step 2:** mechanical type migration (compile-driven), then raise guards to `NR_DCI_MAX_PAYLOAD`.
- [ ] **Step 3: Search order** — order candidate lengths by distance to lengths already seen on this cell (any RNTI/geometry), then outward; 6-sigma gate unchanged. Test: with a seen length 47, the first 10 lengths tried are 47, 46, 48, … .
- [ ] **Step 3b: GPU polar for the wider sweep.** Route the length-sweep candidate decodes (the existing `sweep_gpu_prefill` / lane batch call sites in `nr_pdcch_blind_monitor_rt.c` ~2141, ~2239) through `nr_polar_gpu_mod` for lengths up to 140 bits; extend `nr_polar_sc_cuda` / its params if they assume A ≤ 64 or ≤ 128 (check), keep per-item CPU fallback on `ok=0`. Test: `nr_polar_sc_cuda_test` extended with A = 80, 128, 129, 140 — bit-exact vs CPU (GPU build dir only; skipped without `ENABLE_LDPC_CUDA`).
- [ ] **Step 4:** full ctest; shuffle seeds 1/3/5; rfsim regression gate (cold-sweep time on the 106-PRB bed: `first_crnti_s` → `bank add` must not grow > 25 % vs before, 2 runs each), **measured on the DGX with and without `NR_GPU_POLAR=1`** (report both; the 25 % budget applies to the CPU path, the GPU path should be faster than before).
- [ ] **Step 5:** commit `feat(rr): DCI lengths up to 140 bits end to end (K37 part 1)`.

### Task R3: Per-RNTI length state machine SEARCHING/LOCKED/SUSPECT (Sonnet)

**Files:** `nr_pdcch_dci_length_sweep.{c,h}` (context struct ~155: add `uint8_t state; uint32_t miss_occasions; uint32_t epoch_learned;`), callers in `nr_pdcch_blind_monitor_rt.c` (DL ~4009-4152, UL ~4384); tests in `tests/nr_pdcch_dci_length_sweep_test.cc`.

**Interfaces — Produces:**
```c
typedef enum { NR_LEN_SEARCHING = 0, NR_LEN_LOCKED = 1, NR_LEN_SUSPECT = 2 } nr_len_state_t;
/* Called once per occasion in which a candidate set covering the locked length was searched for this RNTI. */
void nr_pdcch_dci_length_context_note_occasion(nr_pdcch_dci_length_context_t *c, bool accepted_at_locked, bool rnti_active_elsewhere, uint32_t n_suspect);
/* Returns the lengths to try for a SUSPECT context: old length first, then cell-seen lengths, then full range. */
int nr_pdcch_dci_length_context_relock_order(const nr_pdcch_dci_length_context_t *c, const int *cell_seen, int n_seen, int *out, int max);
```
- "RNTI active elsewhere" (SA and NSA): persistence-table sighting with this RNTI's mask at any length, RAR anchor (`record_trusted`), accepts on another banked geometry; SA additionally CORESET#0 C-RNTI accepts. Never requires CORESET#0.
- [ ] **Step 1: Failing tests:** `LockedToSuspectAfterNMisses` (N_suspect = 200 default, env `ISAC_RECONF_N_SUSPECT`), `InactiveRntiNeverSuspect` (misses without activity elsewhere keep LOCKED), `SuspectWithoutCoreset0Evidence` (activity only via persistence/RAR → SUSPECT), `RelockSameLengthReturnsLocked`, `RelockDifferentLengthReplaces` (logs `DCI length RELOCK rnti=… old=… new=…`), `RelockOrderOldFirst`.
- [ ] **Step 2–4:** implement; SUSPECT re-lock sweeps use the same GPU polar batch path as R2 Step 3b (CPU fallback) and may run on any of the N blind-PDCCH scan consumers (A7: thread-safe since `b6e5fb27ac` + `71dbfd582a`; length contexts are accessed under the existing `g_dl_length_lock`); on RELOCK reopen the Technique D contexts and the layout pin keyed on the old length (call existing reopen/invalidate entry points — read `nr_pdsch_config_sweep.h` and `nr_dci11_pin.h`); behind `ISAC_RECONF=1`.
- [ ] **Step 5:** commit `feat(rr): per-RNTI DCI length re-lock (SUSPECT) on SA and NSA evidence (K37 part 2)`.

### Task R4: Two concurrent lengths per (geometry, RNTI) (Sonnet)

- Extend the context to `found[2]` with per-length layout pin; a second significant length for the same RNTI on the same geometry is added (not a conflict). Tests: `SecondLengthAdded`, `ThirdLengthReplacesLeastRecent`, layout pin per length. Commit `feat(rr): up to two locked DCI lengths per (geometry, RNTI)`.

### Task R5: CORESET bank life cycle + remove API (Sonnet)

**Files:** `nr_pdcch_coreset_bank.{c,h}` (entry `nr_pdcch_discovered_coreset_t` in `nr_pdcch_blind_monitor.h:44-58`: add `{uint64_t last_accept_slot; uint32_t accepts_window; uint32_t verified_epoch; uint8_t state;}`), callers in `nr_pdcch_blind_monitor_rt.c`; test `tests/nr_pdcch_coreset_bank_test.cc` (create if absent, pattern of `test_nr_pdcch_coreset_map`).

**Interfaces — Produces:** `int nr_pdcch_coreset_bank_remove(int index);` `void nr_pdcch_coreset_bank_note_accept(int index, uint64_t slot);` `void nr_pdcch_coreset_bank_tick(uint64_t slot, bool traffic_elsewhere, uint32_t t_stale_slots, uint32_t t_remove_slots);` states VERIFIED / STALE / REMOVED.
- "Traffic elsewhere" (SA and NSA): accepts on other entries, PDCCH DM-RS occupancy outside every banked entry; SA additionally CORESET#0 C-RNTI accepts.
- [ ] **Step 1: Failing tests:** `IdleCellNeverDemotes`, `StaleWhenOtherTrafficVisible` (T_stale default 5 s), `StaleReverifiedReturnsVerified`, `StaleRemovedAfterTRemove` (default 30 s), `RemoveCompactsAndReopensKeyedContexts` (length + Technique D contexts on that geometry reopened), thread safety under the existing bank lock.
- [ ] **Step 2–5:** implement behind `ISAC_RECONF=1`; commit `feat(rr): CORESET bank life cycle VERIFIED/STALE/REMOVED (K8, K37 part 3)`.

### Task R6: Continuous low-duty discovery (Sonnet)

- After the first bank entry, keep Technique A running 1 occasion in 20 (`ISAC_RECONF_DISCOVERY_DUTY`); new extents verified and banked as today. The scrambling-ID stage of new-extent discovery uses the GPU idsweep path when the GPU build is present (stage-1/2 tool, 11× on GB10; CPU otherwise); the extra discovery work is spread over the N scan consumers. Test: unit test on the duty scheduler; rfsim gate with `ISAC_RECONF=1`: `scanq drop_full` not worse than baseline within spread, measured with 1 and 2 scan consumers. Commit.

---

## PHASE 2 — one epoch authority and its consumers

### Task R7: `nr_passive_cfg_epoch` pure module (Opus)

**Files:** Create `openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.{c,h}`; test `tests/nr_passive_cfg_epoch_test.cc`.

**Interfaces — Produces:**
```c
typedef enum { NR_EPOCH_SOFT = 0, NR_EPOCH_HARD_REVERIFY = 1, NR_EPOCH_HARD_RESET = 2 } nr_epoch_class_t;
typedef enum { NR_CAUSE_CELL_IDENTITY_CHANGE, NR_CAUSE_MIB_CHANGE, NR_CAUSE_SIB1_CHANGE, NR_CAUSE_SI_MODIFICATION_ANNOUNCED,
               NR_CAUSE_CONTINUITY_LOSS, NR_CAUSE_BWP_CHANGE, NR_CAUSE_DEDICATED_CHANGE_SUSPECTED } nr_epoch_cause_t;
typedef struct { uint32_t epoch; uint32_t identity_gen; nr_epoch_class_t last_class; nr_epoch_cause_t last_cause; } nr_cfg_epoch_snapshot_t;
uint32_t nr_cfg_epoch_current(void);                       /* lock-free read (atomic) */
uint32_t nr_cfg_epoch_identity_gen(void);                  /* bumps only on HARD_RESET */
void nr_cfg_epoch_note_identity(uint16_t pci, uint64_t ssb_arfcn, uint64_t point_a);
void nr_cfg_epoch_note_mib(uint32_t mib_hash_without_sfn);
void nr_cfg_epoch_note_sib1(uint32_t semantic_hash);       /* SA only; never called on NSA */
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t modification_period_slots);
void nr_cfg_epoch_note_continuity_loss(void);
void nr_cfg_epoch_note_bwp_change(void);
void nr_cfg_epoch_note_rnti_reopened(uint16_t rnti, bool was_converged, uint64_t abs_slot);
void nr_cfg_epoch_tick(uint64_t abs_slot);                 /* handles the SI-modification boundary */
typedef void (*nr_cfg_epoch_listener_t)(const nr_cfg_epoch_snapshot_t *);
void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn);
```
- [ ] **Step 1: Failing tests:** `PciChangeIsHardResetAndIdentityGen`, `MibChangeIsHardReverify`, `Sib1SemanticChangeBumps` / `Sib1SameSemanticNoBump`, `SiModAnnouncedBumpsOnlyAtBoundaryIfChanged`, `SiModAnnouncedButUnchangedNoBump`, `SiModBoundaryWithoutSib1ReacquiredBumps` (configurable), `ContinuityLossLoggedAsContinuity`, `TwoConvergedRntisWithin2sIsSoft`, `VanishedRntisDoNotTriggerSoft` (`was_converged=false`), `OneRntiReopenNoBump`, listeners called once per bump, thread-safe current().
- [ ] **Step 2–4:** implement (mutex for state, atomic epoch); log `SENSING: CONFIG_EPOCH n -> n+1 class=… cause=… scope=…`. **Step 5:** commit.

### Task R8: Trigger sources (Sonnet; Opus review)

- MIB hash (excluding SFN/half-frame/SSB index): `nr_ue_decode_mib` path (`openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c`).
- Identity: PCI from sync, SSB ARFCN/Point A from the acquisition state (`nr_passive_acq_state`).
- SIB1 **semantic** hash (SA only): canonicalise the decoded fields listed in spec §4.4 (from `config_ue.c` passive extraction / `nr_pdcch_blind_common_config_t`) into a fixed-order struct and hash it; periodic re-decode every 5 s — first check whether the passive MAC already re-decodes SIB1; if not, add a re-decode request hook (spec §7 open item; record the finding). SIB1 cache key → (PCI, semantic hash) (K11).
- P-RNTI short message: parse `systemInfoModification` from DCI 1_0 P-RNTI short messages in the blind monitor (SA only; the modification period from SIB1 BCCH-Config/PCCH-Config — read from the decoded SIB1).
- Continuity loss: existing stream-gap → LOST.
- BWP change: `nr_passive_bwp`.
- Dedicated reopen: Technique D `reopen_context` and R3 SUSPECT transitions call `note_rnti_reopened(rnti, was_converged=true, slot)`.
- Tests: unit tests per source with fixtures; `Sib1CanonicalHashIgnoresEncodingOnlyDifferences`. Commit.

### Task R9: Queued jobs carry the epoch (Sonnet)

- PDCCH scan queue, PDSCH and PUSCH decode queues (and GPU batches when present) stamp `config_epoch` at enqueue; a consumer dequeuing an older-epoch job processes nothing, counts `dropped_epoch` (exported in `ISAC_METRICS`), and feeds no Technique D / layout / length evidence. Test `OldEpochJobDropped` (queue unit test with a fake epoch source). Commit.

### Task R10: Consumers — length contexts, layout pins, CORESET bank, Technique D (Sonnet; Opus review)

- Listener: on any bump, length contexts LOCKED → SUSPECT (old length first); layout pin config key includes the epoch; CORESET entries VERIFIED → STALE (hint, re-verified); Technique D settled contexts → VERIFY mode (previous winner tried first; levers spec §9.1) — **coordinate with levers R1/R2; if they are not merged, implement as "reopen with previous winner as first hypothesis" only.**
- **Recovery-time dependency (explicit):** the §2 targets (soft ≤ 10 s, hard ≤ 30 s) for PDSCH re-learning assume the levers VERIFY mode + GrantWork + top-K CB0 probes + batched GPU LDPC (levers R1, R2, G1, G4). Without them, recovery runs at today's CPU Technique D speed (seconds at high decode rate, minutes at low p_true); R13 must report which compute path was active and score the targets against it.
- HARD_RESET: drop dedicated state of the old identity (keep dormant keyed by identity_gen; never applied to the new one).
- Tests: `SoftBumpMakesLockedSuspectKeepsOldFirst`, `HardResetNeverReusesOldIdentityState`, `HintConfirmedRestoresTrusted`. Commit.

### Task R11: CellFieldBook mirrors the central epoch (Sonnet) — after `td/convergence-levers` is merged

- `nr_td_fieldbook_bump_epoch` is called only by the R7 listener; the fieldbook's own `epoch` mirrors `nr_cfg_epoch_current()`. Test: bump via the epoch module → promoted fields lose their ordering bonus (levers test `EpochBumpDropsBonus` adapted). Commit.

---

### Task R15 (OPTIONAL): PDCCH GPU path (Sonnet implements; Opus reviews)

Optional: do it only if the levers F3 profile (or a dedicated blind-PDCCH profile with `ISAC_PDCCH_TIMING=1`) shows the
blind-PDCCH scan (FEP/LLR, demap, candidate decode, DM-RS correlation, re-encode checks) is a bottleneck after R2/R6
widen the search (140-bit lengths, continuous discovery, SUSPECT re-lock sweeps). Control logic stays on the CPU.

**Files:** `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.{cu,h}` (exists, built as `libpdcch_gpu.so`, **never loaded by the
receiver**, sign convention unverified — levers spec §9.4 V8), call sites in `nr_pdcch_blind_monitor_rt.c` (occasion body:
FEP/LLR ~3683-5076, candidate decode fan-out ~5309), `nr_polar_gpu_mod.c` (existing, bit-exact GPU polar).

- [ ] **Step 1 (verify before wiring):** live self-check mode (`ISAC_PDCCH_GPU_SELFCHECK=1`): for a sample of occasions run
  both the CPU path (`nr_pdcch_demapping_deinterleaving` + LLR) and the GPU module on the same IQ and compare LLR signs and
  magnitudes per REG; pass = sign agreement ≥ 99.99 % and identical accepted DCIs (RNTI, payload) on the 106-PRB rfsim bed.
  Until this passes the GPU path stays disabled.
- [ ] **Step 2:** batch per occasion across candidates × aggregation levels × (when R4 applies) both locked lengths, and across
  occasions of the same slot; CCE/candidate extraction, DM-RS correlation (for the coherence gate and Technique A occupancy)
  and candidate equalisation on the GPU; polar decode through the existing `nr_polar_gpu_mod` batch; CRC/RNTI recovery,
  re-encode/mismatch gate and all gates/persistence stay on the CPU (or move to the GPU only if bit-exact by test).
- [ ] **Step 3:** unified memory on GB10 (no staging copies), one persistent stream per scan consumer, never blocks the
  receive thread (enqueue + per-occasion completion, CPU fallback on timeout or error, one-shot LOG_W).
- [ ] **Step 4:** A/B on the DGX (106 and 273 PRB, 1 and 2 scan consumers, SA and NSA-like): accepts per occasion and
  accepted DCIs identical to the CPU path; `passivePdcchN` CPU and `scanq drop_full` / `max_lag` reported; commit only if
  identical decisions and lower CPU or latency.
- [ ] **Step 5:** commit `feat(rr, optional): PDCCH GPU path (verified vs CPU, batched, fallback)`; PROJECT_MEMORY V8 → resolved.

### Task R17: `UeContext` — per-UE context, change log and JSON (Opus designs the module, Sonnet implements; spec §4.7)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.{c,h}` (aggregator, life cycle, change log, inference, JSON
  serialisation; the header comment is the single source of the `uectx/1` schema)
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_ue_ctx_test.cc`
- Create: `tests/passive_rx/uectx/uectx_report.py` (+ `test_uectx_report.py`): per-UE timelines, change tables, CSV
- Modify (hooks only, one call each, behind `ISAC_UECTX_PATH` set): `nr_passive_obs` push path (tap every observation
  record), `nr_pdsch_config_sweep` reporter (`nr_pdsch_config_sweep_set_reporter`: converged / reopened / invalidated),
  DCI length state transitions (R3), CORESET bank transitions (R5), `nr_passive_bwp` changes, `nr_cfg_epoch_subscribe` (R7;
  stub epoch 0 until R7 lands), PDCCH blind monitor RNTI first-sighting / RAR anchor.

**Interfaces — Produces:**
```c
#define NR_UECTX_SCHEMA "uectx/1"
#define NR_UECTX_MAX_UE 1024           /* GONE contexts evicted LRU first */
typedef enum { NR_UE_FIRST_SEEN, NR_UE_ACTIVE, NR_UE_IDLE, NR_UE_GONE } nr_ue_state_t;
typedef enum { NR_UEV_TRUSTED, NR_UEV_HINT, NR_UEV_SUSPECT } nr_ue_verif_t;
typedef enum { NR_UEP_RNTI_CLASS, NR_UEP_ANCHOR, NR_UEP_CORESET, NR_UEP_DCI_LEN_DL, NR_UEP_DCI_LEN_UL, NR_UEP_DCI_LEN_STATE,
               NR_UEP_PDCCH_SCR_ID, NR_UEP_PDSCH_SCR_ID, NR_UEP_TD_WINNER, NR_UEP_TD_STATE, NR_UEP_MCS_TABLE, NR_UEP_DMRS_CFG,
               NR_UEP_MAX_LAYERS, NR_UEP_LBRM, NR_UEP_BWP, NR_UEP_PUSCH_LAYOUT, NR_UEP_COUNT } nr_ue_param_t;
typedef struct { int64_t value; uint32_t epoch_learned; nr_ue_verif_t verif; int64_t first_abs_slot, last_confirmed_abs_slot;
                 uint8_t source; } nr_ue_cfg_value_t;   /* value packed per param, documented in the header */
typedef struct { uint64_t grants_dl, grants_ul, crc_ok_dl, crc_ok_ul, bytes_dl, bytes_ul, retx_dl;
                 uint32_t mcs_hist[32], layers_hist[5]; float snr_ema_db, nvar_ema, fo_ema_hz, ta_ema_samples;
                 int16_t prb_start_min, prb_start_max; int16_t prb_size_min, prb_size_max; double prb_size_mean;
                 int64_t last_abs_slot; } nr_ue_stats_t;
typedef struct { uint32_t identity_gen; uint16_t rnti; uint16_t incarnation; nr_ue_state_t state;
                 nr_ue_cfg_value_t cfg[NR_UEP_COUNT]; nr_ue_stats_t st; uint32_t n_changes, n_reconfigs; } nr_ue_ctx_t;
bool nr_ue_ctx_open(const char *path, uint32_t ring_capacity, double snapshot_period_s);  /* starts aggregator+writer */
void nr_ue_ctx_close(void);                                        /* final snapshot of every UE, flush, join */
void nr_ue_ctx_on_obs(const nr_passive_obs_t *o);                  /* any thread, lock-free enqueue */
void nr_ue_ctx_on_param(uint16_t rnti, nr_ue_param_t p, int64_t value, nr_ue_verif_t v, int cause, int64_t abs_slot);
void nr_ue_ctx_on_anchor(uint16_t rnti, int anchor_kind, int64_t abs_slot);   /* TC-RNTI/RAR: may start an incarnation */
bool nr_ue_ctx_get(uint16_t rnti, nr_ue_ctx_t *out);              /* copy of the current incarnation (sensing API) */
void nr_ue_ctx_stats(uint64_t *events, uint64_t *written, uint64_t *dropped);
```
Single-writer design: hooks enqueue small events into a bounded MPSC ring; one aggregator thread owns all contexts,
applies life cycle / epoch / inference rules and serialises `ue_snapshot`, `ue_change`, `ue_reconfig` lines (keys in the
header order; unknown = null). The RT path never blocks; full ring ⇒ drop + count.

- [ ] **Step 1: Failing tests** (gtest, fake clock / fake epoch source):
  `FirstGrantCreatesFirstSeenThenActive`, `IdleAfterTidleGoneAfterTgone`, `RarAnchorOnGoneRntiStartsNewIncarnation`,
  `NsaChurnIsLifecycleNotReconfig` (RNTI vanishes and reappears without a changed TRUSTED value ⇒ 0 `ue_reconfig`),
  `TrustedValueChangeEmitsChangeAndReconfig` (DCI length RELOCK 47 → 53 ⇒ one `ue_change` + one `ue_reconfig`
  class `DCI_SIZE`), `ChangesWithinWindowGroupedAsMixed`, `StatisticsNeverEmitReconfig` (MCS/PRB vary per grant),
  `EpochBumpMakesHintThenReverified`, `EpochBumpContradictedEmitsDiscardedAndChange`, `HardResetClosesOldIdentity`,
  `DedicatedChangeSuspectedFromTwoConvergedUes` (feeds `nr_cfg_epoch_note_rnti_reopened` exactly once per UE),
  `JsonRoundTripSchemaV1` (every key present, unknown = null, one line per record), `FullRingDropsAndCounts`,
  `CloseWritesFinalSnapshotForEveryUe`.
- [ ] **Step 2: Run → FAIL. Step 3: Implement** the module; then the hooks (each one guarded so that with
  `ISAC_UECTX_PATH` unset nothing is enqueued and behaviour is bit-identical). **Step 4: Run → PASS**; full ctest; rfsim
  regression gate (`tests/passive_rx/dgx/rfsim_regress.sh 2`) with `ISAC_UECTX_PATH` unset ⇒ unchanged, and once with it
  set ⇒ gate still PASS, writer drops = 0, file parses with `uectx_report.py`.
- [ ] **Step 5: Offline tool:** `uectx_report.py FILE [--rnti X] [--csv OUT]` → per-UE timeline (life cycle, parameter
  values over time), change table, reconfiguration list, summary counts; unit test with a fixture file.
- [ ] **Step 6: Validation hooks:** R12 replay must produce exactly the injected per-UE reconfigurations as
  `ue_reconfig` records; R13 live bed scores per-UE recovery times from the `ue_change` timestamps.
- [ ] **Step 7: Commit** — `feat(rx): UeContext per-UE context, change log and uectx/1 JSON (spec 4.7)`.

Coordination: Technique D fields come from the levers branch (`td/convergence-levers`: field book votes/pruned
fields, fail-open, levers C/P state); before that branch merges, R17 records only what `adaptive-rx-UL-DL` exposes and
leaves the extra fields null.

## PHASE 3 — validation

### Task R12: Offline mid-stream reconfiguration replay (Sonnet)

- Fixture driver (gtest or C harness) over the existing synthetic PDCCH/PDSCH fixtures + hidden-waveform transmitter (`openair1/SIMULATION/NR_PHY/hidden_waveform.h`): inject at deterministic slots (a) DCI 1_1 length change for one RNTI, (b) CORESET move, (c) SIB1 semantic change, (d) PCI change; run each in **SA** (SIB1/CORESET#0 present) and **NSA-like** (absent) variants. Assert recovery (back to LOCKED/VERIFIED/converged) within the target slot budgets **and** that no result computed from old-epoch input reaches new-epoch state (instrumented counters). Commit evidence.

### Task R13: Live SA bed + NSA-like arm (Sonnet; operator for Docker/5G core)

- OAI SA bed (5G core on the DGX if Docker access is confirmed, else OCUDU/OAI-ZMQ fallback): BWP switch via the telnet harness (`tests/passive_rx/run_bwp_switch.sh`), UE detach/re-attach under a changed dedicated config (antenna-ports table change → different 1_1 size), gNB restart with a changed cell config. Arms: SA (SIB1 used) and NSA-like (SIB1/SI/P-RNTI paths disabled). Campaign runner (`tests/passive_rx/campaign/campaign.py`), ≥ 5 runs per arm; score recovery times, `CONFIG_EPOCH` lines (class/cause), `dropped_epoch`, stale winners vs gNB truth (validation only).

### Task R14: Soak + documentation (Sonnet; Opus review)

- 60-min stable-cell soak (SA and NSA-like): false SOFT ≤ 1/h, hard 0. PROJECT_MEMORY: §24 K10/K11/K37 status, §11 new log lines (`CONFIG_EPOCH`, `DCI length RELOCK`, CORESET state changes), §10.2 new env vars (`ISAC_RECONF`, `ISAC_RECONF_N_SUSPECT`, `ISAC_RECONF_DISCOVERY_DUTY`, thresholds), §16, §25. `/code-review` on the branch. Commit and push the branch (not `adaptive-rx-UL-DL`).

## FINAL — mandatory

### Task R16: Comprehensive PROJECT_MEMORY.md update (Opus; last task, after R14 and R15 if run)

The plan is not complete until `PROJECT_MEMORY.md` reflects everything this branch changed, so that a fresh agent with no
conversation history can operate and extend the receiver from the file alone. Every statement carries an evidence
label (§0.1) naming host and commit; DGX, cloud x86 and OTA results stay in separate tables.

- [ ] **Step 1 — header:** a dated update block summarising what the branch delivered, the merge commit, and what is still open.
- [ ] **Step 2 — architecture and modules:** §2.1 pipeline diagram (epoch authority, CORESET life cycle, length re-lock,
  140-bit DCI path, UeContext aggregator); §2.2 block reference updated for B6/B7 (blind PDCCH, DCI recovery); §3.3 new modules
  (`nr_dci_bits.h`, `nr_passive_cfg_epoch.{c,h}`, bank life cycle API, length state machine, optional PDCCH GPU path) with
  their tests; §3.4 new tools/scripts (replay driver, campaign arms).
- [ ] **Step 3 — operation:** §10.2 every new env var / config key with default and agnostic status (`ISAC_RECONF`,
  `ISAC_RECONF_N_SUSPECT`, `ISAC_RECONF_DISCOVERY_DUTY`, thresholds, `ISAC_PDCCH_GPU_SELFCHECK` if R15 ran); §10.4 the beds used
  (SA bed, NSA-like arm flags, 5G core / OCUDU fallback actually used).
- [ ] **Step 4 — logging:** §11 every new log line with meaning and normal/abnormal values, plus §21 the `uectx/1` JSON
  schema pointer and `ISAC_UECTX_PATH`/`ISAC_UECTX_PERIOD_S` (`CONFIG_EPOCH … class= cause=`,
  `DCI length RELOCK`, CORESET VERIFIED/STALE/REMOVED transitions, `dropped_epoch`, SIB1 semantic-hash change, SI-modification
  pre-announcement and boundary decision) and the new `ISAC_METRICS` fields.
- [ ] **Step 5 — validation:** §12 gates touched (G5A/G5B/G6/G7/G11) with the new PASS conditions; §13 offline results
  (new gtests, replay fixture SA + NSA-like); §14 rfsim/SA-bed results with numbers (recovery times per change class,
  stale-winner count, soak false-trigger rate, CPU vs GPU polar sweep times, R15 A/B if run); §15 any OTA observation.
- [ ] **Step 6 — status and issues:** §16 status table rows (DCI recovery, CORESET discovery, reacquisition G11,
  NSA/SIB1-less G5B); §19 state machine section updated with what is now implemented vs still design target; §23.5–§23.8
  block references; §24 K8, K10, K11, K37 (and V8 if R15 ran) marked resolved or updated with evidence, new K-entries for
  anything found; §25 next steps re-ordered; §0.6 document index (this spec/plan → done).
- [ ] **Step 7 — consistency check:** grep the file for statements this branch made stale (e.g. "63-bit", "never cleared",
  "bank not cleared on cell change", "no unified epoch") and correct or mark them `HISTORICAL`; verify every commit hash
  cited exists (`git cat-file -e <hash>`); sens6-frozen gate; commit `docs: PROJECT_MEMORY - reconfiguration robustness
  delivered (…)` and push the branch.

## Self-review record (2026-10-01)

Spec coverage: §4.1 → R1, R2; §4.2 → R3, R4; §4.3 → R5, R6; §4.4 → R7, R8, R9, R10, R11; §4.5 → R10; §4.7 UeContext → R17; §4.6 SA/NSA → Global Constraints + R3/R5/R8 evidence lists + R12/R13 NSA arms; §5 → R12–R14; §7 open items → R8 (SIB1 re-decode mechanics, modification period) and R12/R13 (thresholds); §8 phases → plan order. Documentation: R16 (mandatory final PROJECT_MEMORY.md update). Gaps: none known; R2's joint-solver decision and R8's SIB1 re-decode finding are explicit decision steps. Acceleration (levers §9) reused: GPU polar (R2 3b, R3), idsweep GPU + N scan consumers (R6), VERIFY/GrantWork/probes/GPU LDPC (R10, dependency on the levers branch); PDCCH GPU path as OPTIONAL R15 (profile-gated, verify-before-wire).
