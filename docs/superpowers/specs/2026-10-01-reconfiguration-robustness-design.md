# Reconfiguration robustness — design

Date: 2026-10-01. Status: **frozen** after operator review round 1 (2026-10-01): 140-bit DCI bound; HARD_REVERIFY vs
HARD_RESET (identity change); continuity loss logged as such; SI modification = pre-announcement; semantic SIB1 hash;
one central epoch owner; **every rule must work on both SA and NSA cells over the air** (§3.6, §4.6). Related: `2026-10-01-technique-d-convergence-levers-design.md` (shares `config_epoch` and the
COLD / VERIFY / TRACKING compute modes of its §9). Background: `PROJECT_MEMORY.md` §11.6–§11.11, §23.5–§23.8, §24 K8, K10, K11.

## 1. Problem

The receiver cannot read RRC Reconfiguration: on SA it arrives after security activation (ciphered SRB); on NSA the NR
configuration travels inside LTE RRC. `nr_passive_rrc_harvest.c` only covers an unciphered RRCSetup. Every reconfiguration
must therefore be detected from its **effects on the signal** and the affected learned state re-acquired.

Verified state of the code (2026-10-01, `adaptive-rx-UL-DL` @ `03a4cf3ae1`):

| Learned state | Reaction to change today | Gap |
|---|---|---|
| Technique D PDSCH config (per RNTI × TDA) | REOPEN after a failure streak under a 1e-6 budget (`reopen_context`, `nr_pdsch_config_sweep.c` ~724/~1186) | none local; no cell-wide trigger |
| DCI 1_1 field layout pin | dropped on block/giveup or config-key change, reseeded (`nr_dci11_pin_select`, `nr_dci11_pin.h:60-75`) | none local |
| DCI length per (CORESET geometry, RNTI) | **never cleared once `found`** (`nr_pdcch_dci_length_sweep.{c,h}`; only LRU eviction / reset) | **G1: no re-lock** |
| DCI length range | hard cap `dci_length > 63` rejected (`nr_pdcch_blind_monitor.c` 3729, 3921, 3957, 4727, 4782; `NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN 64`) | **G2: commercial 1_1 > 63 bits never found** |
| One length per (geometry, RNTI) | single `found` | **G3: two search-space sets with different 1_1 sizes conflict** |
| Dedicated CORESET bank | add/covers only (`nr_pdcch_coreset_bank.h:61-83`), no remove/demote | **G4 (K8): a moved/removed CORESET is never noticed** |
| BWP | passive tracker `nr_passive_bwp` (offline verified) | partial |
| Cell-wide changes (MIB, SIB1, P-RNTI SI modification, PCI/carrier) | none; SIB1 facts logged once; SIB1 cache keyed by PCI only (K11) | **G5 (K10): no change detector, no unified epoch, stale queued jobs credited** |

## 2. Goal and success criteria

| Metric | Target |
|---|---|
| Recovery after a **soft** change (dedicated reconfig of one or more UEs, BWP switch) — first change evidence → TRACKING again for the affected RNTIs | ≤ 10 s at ≥ 100 grants/s per UE |
| Recovery after a **hard** change (MIB / SIB1 content / PCI / carrier / announced SI modification) | ≤ 30 s (= the cold-start target of the levers spec), typically much faster via re-verification |
| **0 stale winners**: no CONVERGED / locked / banked state used against new-epoch evidence without re-verification; no queued job of an old epoch credited | always |
| DCI lengths up to the 38.212 polar DCI limit are discoverable | §4.1 |
| No regression when nothing changes (no spurious epochs on a stable cell over a 60-min soak) | false-trigger rate ≤ 1 per hour soft, 0 hard |

## 3. Principles

1. Detect change from the signal only; gNB logs are validation ground truth only.
2. Scope the reaction as narrowly as the evidence allows (one RNTI < one CORESET < whole cell).
3. **Re-verify before discarding**: learned state becomes a *hint* (tried first) instead of *truth* until confirmed again;
   contradicted hints are discarded. This is the operator's decision for hard changes too.
4. Every consumer of learned state checks `config_epoch`; every queued job carries it; old-epoch outcomes are dropped.
5. All new behaviour is behind flags whose defaults keep today's behaviour until validated, except the 140-bit cap lift
   (a pure capability extension, validated by tests).
6. **SA and NSA (operator rule).** The receiver must work over the air on both. No component may *require* SIB1, CORESET#0,
   SI-RNTI, P-RNTI paging or RA on the NR carrier: those are optional evidence when present (SA) and simply absent on
   NSA (no SIB1, k_SSB ≥ 24 possible, paging and RRC on LTE, contention-free RA with RAR-anchored C-RNTIs). §4.6 lists,
   per component, what it uses on each.
7. **One epoch authority.** A single `CellConfigEpoch` owner (§4.4) for the whole receiver; Technique D, the
   CellFieldBook (levers spec §4.6), DCI length contexts, layout pins, the CORESET bank and the queues are consumers.
   No module keeps a private epoch counter. Every learned record carries `{value, epoch_learned, verification_state}`
   and becomes `HINT` automatically when `epoch_learned` < current epoch.

## 4. Components

### 4.1 DCI length capacity (G2)
- Raise the maximum DCI payload from 63 to **A = 140 bits** (`NR_DCI_MAX_PAYLOAD 140`; with the 24-bit CRC, K = 164
  into the polar chain). The plan cites the governing TS 38.212 clauses (DCI CRC attachment / polar coding for DCI and the
  DCI size alignment rules) next to the constant.
- The payload type changes from `uint64_t` to a fixed multi-word bit vector (3 × 64 bits) with extraction helpers;
  the OAI polar decoder already writes its output into a `uint64_t[]` (`polar_decoder_int16(..., uint64_t *out, ...)`;
  callers pass `dci_estimation[2]`, i.e. 128 bits today) — the buffers grow to 3 words and the decoder's behaviour
  for A > 128 is verified by test.
- Touch points: `NR_PDCCH_DCI_LENGTH_SWEEP_MAX_LEN` and every `dci_length > 63` guard; payload storage that assumes
  ≤ 64 bits (`uint64_t` payloads, e.g. `bwp_probe_payload`, layout enumeration, re-encode/mismatch gate, joint GF(2)
  solver dimensions); DCI 1_1/0_1 layout sweeps (field-width budgets).
- Search order over the wider range: lengths ordered by distance to lengths already seen on this cell (any RNTI,
  any geometry), then outward; the per-length 6-sigma significance gate is unchanged. Cold sweep time must not grow
  by more than 25 % on cells whose true length is ≤ 63.

### 4.2 Per-RNTI DCI length adaptiveness (G1, G3)
- Length context state: `SEARCHING → LOCKED → SUSPECT → (LOCKED | SEARCHING)`.
- LOCKED → SUSPECT when the RNTI is still evidently active (persistence-table sightings at *any* length with the
  RNTI's CRC mask, RAR anchor, or blind accepts on another geometry) **and** produces zero accepts at the locked length
  for N_suspect occasions (default 200) in which a candidate set covering it was searched.
- SUSPECT: the sweep reopens for this RNTI only, trying the old length first, then lengths seen on the cell, then the
  full range; a fresh significant lock at the old length returns to LOCKED; a different length replaces it (logged
  `DCI length RELOCK rnti=… old=… new=…`); Technique D contexts and the layout pin keyed on the old length are reopened.
- Up to **two** concurrent LOCKED lengths per (geometry, RNTI) — a second significant length for the same RNTI on the
  same geometry is added, not treated as a conflict. Each has its own layout pin.

### 4.3 CORESET adaptiveness (G4)
- Bank entry health record: `{last_accept_slot, accepts_window, rntis_seen, verified_epoch, state}`.
- Life cycle: `VERIFIED → STALE → (VERIFIED | REMOVED)`:
  - VERIFIED → STALE: no fresh dedicated accepts on this entry for T_stale (default 5 s) **while dedicated traffic is
    visible elsewhere** (accepts on other entries or on CORESET#0 C-RNTI class, or PDCCH DM-RS occupancy outside every
    banked entry). An idle cell never demotes anything.
  - STALE: the entry keeps being searched (hint), and re-verification runs (fresh same-RNTI DCIs at later slots with
    different payloads, the existing verification rule). Success → VERIFIED; failure within T_remove (default 30 s) →
    REMOVED via a new `nr_pdcch_coreset_bank_remove(int index)`.
- Continuous discovery: after the first entry is banked, the occupancy map (Technique A) keeps running at a low duty
  cycle (default 1 occasion in 20) to find new/moved CORESETs; a new extent is verified and banked exactly like the
  first. Budgeted so `scanq drop_full` does not rise (§7).
- Removal or relock of an entry reopens the length and Technique D contexts keyed on that geometry.

### 4.4 Cell-wide change detector and unified `config_epoch` (G5)
Module `nr_passive_cfg_epoch.{c,h}` (CPU, pure state machine + hooks). Triggers:

| Trigger (signal only) | Class | Logged cause | SA | NSA |
|---|---|---|---|---|
| PCI change; carrier / Point A / SSB ARFCN change | **HARD_RESET** (new cell identity) | `CELL_IDENTITY_CHANGE` | ✓ | ✓ |
| MIB content change (any field except SFN / half-frame / SSB index) | HARD_REVERIFY | `MIB_CHANGE` | ✓ | ✓ |
| SIB1 **semantic** change: SIB1 re-decoded periodically (default every 5 s) and compared by a canonical hash of the decoded fields the receiver uses (PLMN, cell identity, TAC, carrier/Point A/SCS-specific carrier, BWPs, TDD pattern, common PDSCH/PUSCH TDRA, PDCCH-ConfigCommon/search spaces, RACH), never by raw encoded bytes | HARD_REVERIFY | `SIB1_CHANGE` | ✓ | — (no SIB1) |
| P-RNTI short message `systemInfoModification` | **pending** HARD_REVERIFY: the updated SI is broadcast in the following modification period (TS 38.331), so the detector arms a re-acquire-and-compare at the next modification-period boundary; the epoch is bumped only if the re-acquired SI differs (or unconditionally at the boundary if SIB1 cannot be re-decoded) | `SI_MODIFICATION_ANNOUNCED` then `SIB1_CHANGE` | ✓ | — (paging on LTE) |
| Stream gap → LOST (existing continuity detector) | HARD_REVERIFY — a reconfiguration *may have been missed*, not proven | `CONTINUITY_LOSS` | ✓ | ✓ |
| BWP change reported by `nr_passive_bwp` | SOFT | `BWP_CHANGE` | ✓ | ✓ (if dedicated BWPs are observable) |
| ≥ N_cell (default 2) distinct RNTIs whose Technique D contexts reopen or whose lengths go SUSPECT within W (default 2 s) | SOFT (cell-wide dedicated change) | `DEDICATED_CHANGE_SUSPECTED` | ✓ | ✓ — **primary dedicated-change signal on NSA** |

Actions:
- **HARD_RESET:** new `CellConfigEpoch` instance for the new identity; dedicated state of the old identity is **not** carried
  over as hints (it belongs to another cell); broadcast facts re-acquired from scratch; SIB1 cache keyed by the new
  (PCI, semantic hash). The old identity's state may be kept dormant (keyed by identity) for a fast return, never applied
  to the new one.
- **Soft:** `config_epoch++`. Cell-wide published lengths, CellFieldBook fields and CORESET entries lose *trusted*
  status but remain *hints* (tried first); confirmed again → trusted; contradicted → discarded.
- **HARD_REVERIFY:** `config_epoch++`; re-derive broadcast facts (CORESET#0/SS#0, SIB1 facts, carrier, TDD) from the new
  MIB/SIB1; **all dedicated state becomes hints to re-verify** (operator decision — no reset). The SIB1 cache key
  becomes (PCI, SIB1 semantic hash), fixing K11. On NSA (no SIB1) broadcast re-derivation is MIB-only (CORESET#0
  may be absent); carrier/TDD come from startup or signal estimation as today.
- Consumers that must honour the epoch: Technique D contexts (hint = ordering only, via CellFieldBook / ordering
  score), DCI length contexts (LOCKED → SUSPECT on epoch change with old length first), layout pins (config key
  includes epoch), CORESET bank (VERIFIED → STALE), CellFieldBook (`config_epoch`), passive BWP tracker.
- Queued jobs (PDCCH scan queue, PDSCH and PUSCH decode queues, GPU batches) carry the epoch at enqueue; a consumer
  dequeuing a job of an older epoch processes nothing and counts `dropped_epoch`. Sweep tickets already carry a
  generation; the epoch is added to the stale-ticket check.
- **Owner:** `nr_passive_cfg_epoch` is the only epoch authority (principle 7). The levers CellFieldBook's internal epoch
  becomes a mirror set by this module (`nr_td_fieldbook_bump_epoch` is called by the owner, never on its own).
- Every epoch change logs `SENSING: CONFIG_EPOCH n -> n+1 class=HARD_RESET|HARD_REVERIFY|SOFT cause=… scope=…` and is exported in
  the metrics/observation outputs (Track-A A2/A3).

### 4.5 Compute modes
The COLD / VERIFY / TRACKING modes of the levers spec §9 are driven by this module: a hard or soft epoch change puts
affected contexts in VERIFY (old configuration first, small neighbourhood, moderate probing) instead of COLD.

### 4.6 SA / NSA operation per component

| Component | Evidence used on SA | Evidence used on NSA | Must not assume |
|---|---|---|---|
| 4.1 DCI capacity | same | same | — |
| 4.2 length re-lock: "RNTI still active" | persistence sightings, RAR anchor, C-RNTI accepts on CORESET#0 or other geometries | persistence sightings, **CFRA RAR anchor** (NR-leg RA is contention-free), accepts on other banked geometries | CORESET#0 / SI-RNTI traffic |
| 4.3 CORESET life cycle: "traffic visible elsewhere" | other entries, CORESET#0 C-RNTI accepts, DM-RS occupancy outside banked entries | other banked entries, DM-RS occupancy outside banked entries | CORESET#0 exists |
| 4.4 triggers | all rows of the table | identity, MIB, continuity, BWP, dedicated-change rows only | SIB1, paging |
| 4.5 modes | same | same | — |

NSA-specific caution: SCG addition/release makes C-RNTIs appear and disappear in bursts; that is RNTI churn, **not** a cell
reconfiguration — the SOFT `DEDICATED_CHANGE_SUSPECTED` trigger counts only RNTIs that were **converged** and then
reopened/SUSPECT, not RNTIs that simply vanished.

## 5. Validation

1. **Unit tests (gtest):** length context state machine incl. two concurrent lengths and RELOCK; bank life cycle
   incl. "idle cell never demotes"; epoch module triggers and severities; queued-job epoch rejection; SIB1 cache key.
2. **Offline reconfiguration replay:** synthetic PDCCH/PDSCH streams (existing fixtures + hidden-waveform transmitter)
   where mid-stream the DCI length, the CORESET position, or the SIB1 content changes; pass = recovery within the
   targets and 0 old-epoch outcomes credited.
3. **Length > 63 bits:** fixture DCIs at 64, 80, 100, 128, 129 and 140 bits decoded and length-locked.
4b. **NSA-like arm** of every live test: SIB1 ignored (`ISAC_TD_IGNORE_SIB1=1` style flag), no SI/P-RNTI paths; the same
   recovery targets must hold using only the NSA evidence column of §4.6.
4. **Live SA bed:** OAI gNB reconfiguration via the existing BWP-switch harness (telnet); UE detach/re-attach under a
   changed dedicated config (e.g. different antenna-ports table → different 1_1 size); gNB restart with a changed cell
   config (hard change). Score with the Track-A campaign runner; ground truth from gNB logs (validation only).
5. **Soak:** 60 min stable cell, false-trigger rate within §2.
6. **OTA (Milan, SA and NSA cells):** any natural reconfiguration is logged by `CONFIG_EPOCH` lines (class + cause) and reviewed.

## 6. Out of scope
Decoding ciphered RRC; NSA LTE-side signalling; UL (PUSCH) dedicated reconfiguration beyond what the shared epoch
covers (UL length/interpretation contexts honour the epoch like DL ones).

## 7. Open items (planning parameters only)
- SIB1 re-decode mechanics in passive mode (does the OAI UE MAC re-decode SIB1 periodically, or is a passive re-decode
  hook needed) and the modification-period computation for the SI-modification pre-announcement.
- Default thresholds (N_suspect, T_stale, T_remove, N_cell, W) — tuned on the replay fixture and the live bed.

## 8. Implementation phases
- **Phase 1 (local, independently testable):** 140-bit DCI capacity → per-RNTI length SEARCHING/LOCKED/SUSPECT →
  dual-length support → CORESET life cycle + remove API → continuous low-duty discovery.
- **Phase 2:** the single `CellConfigEpoch` owner, then the consumers in order: queued decode jobs → DCI length
  contexts → layout pins → CORESET bank → Technique D contexts → CellFieldBook mirror.
