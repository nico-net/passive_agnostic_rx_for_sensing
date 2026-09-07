# Passive-RX UL Adaptive Discovery — Design

**Branch:** total-passive-rx-UL-DL-graphics · **Host:** sens6
**Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue
**Supersedes-context:** `PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md`'s "FULL UL ADAPTIVE
CATALOGUE (2026-09-07)" section — this doc picks that catalogue's recommended path (A + C) and
turns it into a concrete design, then extends it: the catalogue's (C) only covers UL field
*widths*; it has no analogue of Technique D (UL field *value interpretation* — TDA table
contents, DM-RS config, MCS table), which is added here as Component 3 once the gap was noticed.

## Problem

UL adaptive discovery is 0% built. Every UL config value today
(`pdcch_blind_monitor_ul_dci_bits`, `pdcch_blind_monitor_ul_tda`, `pdcch_blind_monitor_ul_dmrs`,
etc.) was hand-solved once, offline, by replaying captured payloads against this one cell's own
gNB scheduler log — a method with no equivalent on a real deployment with no such log. DL adaptive
discovery (CORESET footprint, DCI length, RNTI bootstrap, PDSCH-interpretation sweep) is now
working end to end and unattended (see "Current DL state" below); this design closes the
equivalent gap for the uplink, to the extent it is closable.

## Current DL state (context, not in scope here)

As of commit `d29596d136` (2026-09-07), the DL chain runs end to end unattended: Technique A
(CORESET footprint via DM-RS correlation, hit-driven termination) converges to the exact known
CORESET; Technique C (DCI-length sweep, CRC-oracle, accumulated across occasions) locks the
correct length; Technique B (RNTI bootstrap) now uses a 16-entry multi-UE table (free-slot-first,
weakest-evidence eviction) instead of a single incumbent slot — that table fix is what closed the
"AUTO decodes 127k genuine payloads but accepts=0" symptom recorded near the end of the handover
doc (a noise RNTI could displace the real, already-confirmed C-RNTI from a single incumbent
slot; with 16 slots and eviction-by-weakest-evidence it can't anymore); Technique D
(`nr_pdsch_config_sweep.{h,c}`) resolves PDSCH payload *interpretation* (TDA S/L, DM-RS
additional position, MCS table) via a TB-CRC-scored, per-grant-interleaved round-robin sweep.
**This means UL work is not blocked on any DL-side defect** — the shared extractor is healthy,
and Technique B's bootstrap RNTI is available to UL scoring immediately (it's the UE's C-RNTI,
not DL-specific).

## Scope

Build:
- **(A)** Port Technique C's DCI-length sweep to format 0_1. [catalogue item A]
- **(C)** TB-CRC-scored field-*boundary* sweep — which fields are, how many bits wide.
  [catalogue item C]
- **(New) Component 3** — TB-CRC-scored field-*value-interpretation* sweep — the UL analogue of
  Technique D, discovering TDA table contents (S/L/mapping/k2 per index), DM-RS config
  (type/additional-position/max-length), MCS table, and transform precoding. Not in the
  catalogue; identified while reviewing this design (Component 2's field-width sweep leaves these
  semantic values as hand-set, exactly as Technique D exists because DL's own field-width
  extraction being correct doesn't mean the RRC-configured *meaning* of those fields is known).

Catalogue items (B) (PUSCH cross-observation, 3 new DSP primitives) and (D) (SRS-based sync) stay
out of scope — (B) is a fallback only needed if (C)/Component 3 can't resolve every field,
neither is needed to reach this design's stated goal.

**Explicitly deferred, not because it's wrong but because it touches shipped code**: migrating
DL's Technique D (`nr_pdsch_config_sweep.c`) onto the shared engine this design introduces (see
"Shared hypothesis-search engine" below). The engine generalizes cleanly to DL's hypothesis shape
— see that section — but `nr_pdsch_config_sweep.c` is already tested (8+3 tests) and live-wired;
refactoring working, committed code is a separate task, not bundled into this one.

## Shared hypothesis-search engine

Components 2 and 3 (and, per the deferred item above, potentially DL's Technique D later) are the
same shape of problem: search a space of hypotheses about how to interpret already-correctly-
received bits, using the transport-block CRC as the only oracle that can tell a wrong
interpretation from a right one. Rather than duplicate that shape twice, both components sit on
one shared engine (new files `nr_hyp_sweep.{h,c}`, hypothesis-type-agnostic via an opaque
`void*` hypothesis + caller-supplied comparison/build/reject callbacks — deliberately NOT a C++
template or a heavier generic-programming construct; this project's C code already uses the
callback-struct pattern for exactly this, see `nr_pdcch_dci_length_scorer_fn`). Four stages,
replacing the flat "enumerate the Cartesian product, round-robin, decide by ratio" design from
the previous revision of this doc:

**Stage a — admissible hypothesis generation.** `nr_hyp_sweep_init()` takes a list of constraint
predicates (`bool (*)(const void *hyp, void *ctx)`) applied to a hypothesis-space generator,
instead of a hardcoded "Cartesian product then filter." First implementation for Component 2
registers exactly one constraint (`sum(widths) == dci_length`); for Component 3, exactly one
constraint per swept dimension being spec-legal (mirroring Technique D's existing `kSL` table
construction). This is deliberately phrased as **"generate the admissible set under currently
known constraints"**, not "enumerate and filter by sum" — future constraints (BWP behavior
actually observed, whether frequency hopping ever appears live, cross-occasion DCI consistency,
observed SRS/CSI-RS usage, grant-validity history) register as additional predicates in the same
list with no change to the engine itself.

**Stage b — equivalence-class collapsing (no decode cost).** Before any PUSCH/PDSCH decode is
attempted, hypotheses are grouped by applying each to a handful of already-observed real candidate
payloads and comparing the resulting extracted grant (`nr_pdcch_blind_ul_result_t` for Component
2/3) field-by-field. Hypotheses producing byte-identical grants on every sample payload collapse
into one class — this is exactly the "60 splits differ only in padding placement" finding from the
original manual UL solve, now made structural instead of a one-off observation. **Confirmed
applicable to Component 2/3** (the padding-invariance finding is measured, on this cell). **Not
assumed for DL's Technique D space** if that engine is ever migrated (see below) — different MCS
tables generally produce different modulation, so it may find zero true equivalence classes there;
that would need its own measurement, not an assumption either way, before quoting a win.

**Stage c — plausibility gate, reject-only.** Between extraction and decode: structural checks
(RIV bounds via the existing `riv_to_prb_alloc()`-style checks, antenna-ports table row validity,
TDA-index range) plus temporal consistency (cross-occasion field consistency for the same
bootstrapped RNTI, mirroring Technique C's existing degenerate-fixed-point check). **Hard
invariant, stated explicitly so it isn't relaxed later**: this stage only eliminates
spec-impossible hypotheses. It never ranks or scores surviving ones by plausibility — doing so
would bias the search toward assumptions about the very network configuration being inferred.

**Stage d — TB-CRC oracle, scored over surviving classes.** Round-robin + min-trials + win-ratio +
floor (Technique D's existing decision rule, copied not shared as literal code since the
hypothesis type differs) is the *first* implementation, but the state it tracks is
trials/passes **per surviving equivalence class**, not per raw hypothesis index. The stopping rule
is "one class is statistically ahead of every other surviving class," and `winner()` returns a
representative hypothesis from that class. This directly targets the real goal — "every subsequent
grant is interpreted correctly" — rather than "the exact RRC-configured values," which the
padding-equivalence finding shows isn't always recoverable or even necessary. Architected so a
later revision can replace round-robin with eliminate-on-contradiction without touching the class
model (out of scope to build now — round-robin is deterministic and cheap to validate first).

## Component 1 — UL DCI-length sweep (approach A)

Unchanged from the previous revision. `nr_pdcch_dci_length_sweep.{h,c}` is already
format-agnostic (scorer-callback based); no changes to that module.

New code:
- `nr_pdcch_autodiscover_ul_length_scorer()` in `nr_pdcch_blind_monitor_rt.c`, mirroring the
  existing DL scorer but calling `nr_pdcch_blind_decode_and_extract_01()`. `dci_length` is a
  parameter independent of `opts` (opts only carves fields *after* CRC recovery) — this scorer
  passes a placeholder/default `nr_pdcch_blind_ul_opts_t`; the CRC oracle doesn't need field
  widths to be right yet.
- A second, independent `nr_pdcch_dci_length_sweep_state_t` instance (own give-up cap), fed from
  the UL candidate path in `run_occasion()`. Never shares state with the DL sweep.
- Reuses Technique B's already-shared bootstrap RNTI (UE-generic, not DL-specific) as a
  near-certain significance shortcut, same as DL.

## Component 2 — UL field-boundary sweep (approach C), on the shared engine

**Hard prerequisite, unchanged**: only runs once `bootstrap_rnti != 0` (Technique B) and once
Component 1 has locked `dci_length`.

### Swept fields and their legal ranges

From `nr_pdcch_blind_ul_opts_t` (`nr_pdcch_blind_monitor.h`), Stage a's generator enumerates every
legal combination of these fields consistent with the registered constraints (initially: sum
equals the live-verified `dci_length`):

| field | legal range (TS 38.212 7.3.1.1.2) |
|---|---|
| `carrier_indicator_bits` | {0, 3} |
| `ul_sul_bits` | {0, 1} |
| `bwp_indicator_bits` | {0, 1, 2} |
| `freq_hopping_bits` | {0, 1} |
| `harq_pid_bits` | {4, 5} |
| `dai1_bits` | {1, 2} |
| `dai2_bits` | {0, 2} |
| `sri_bits` | {0, 1, 2} |
| `precoding_info_bits` | {0, 1, 2, 3, 4} |
| `antenna_ports_bits` | {2, 3, 4, 5} |
| `srs_request_bits` | {2, 3} |
| `csi_request_bits` | {0, 1, ..., 6} |
| `cbg_bits` | {0, 1, 2} |
| `ptrs_dmrs_bits` | {0, 1, 2} |
| `beta_offset_bits` | {0, 2} |
| `dmrs_seq_init_bits` | {0, 1} |

`carrier_indicator_bits`/`ul_sul_bits` are swept (not assumed 0) specifically to close the
CA/SUL silent-wrong-answer risk identified in the non-goals review below.

**New hard cap on the RAW (pre-equivalence-collapse) generated set**,
`NR_HYP_SWEEP_MAX_RAW` (recommend 4096, since Stage b collapses duplicates before Stage d's
per-class bookkeeping, which is what actually needs to stay small — the manual solve's own
"3963 raw" precedent fits comfortably under this): if exceeded, refuse to start and log loudly
rather than silently truncating. A second cap on **surviving equivalence classes**
(`NR_HYP_SWEEP_MAX_CLASSES`, recommend 64, matching Technique D's existing cap) governs Stage d's
bookkeeping cost, since that's what round-robin actually pays per trial.

### Wiring

For each 0_1 candidate: Stage a/b/c run once, offline of any per-candidate decode, to produce the
class list. Per candidate: round-robin a surviving class → build a full `opts` (fixed/known
fields + the class representative's swept fields) → `extract_01()` → if plausible (Stage c) and
RNTI matches the bootstrap set → build the UL grant → `nr_pusch_passive_decode()` at the
k2-offset slot → feed TB-CRC pass/fail back into the sweep, attributed to the CLASS.

**No PUSCH-decoder signature change needed** — `nr_pusch_passive_decode()` already takes an
interpreted grant (`nr_pdcch_blind_ul_result_t`), not raw field widths; varying `opts` per
hypothesis at the extraction call site is sufficient.

## Component 3 (new) — UL field-*interpretation* sweep, the UL analogue of Technique D

Runs only after Component 2 has locked field widths (a wrong width makes every field value
meaningless, same "geometry before interpretation" ordering Technique D already documents for
DL). Same shared engine, different hypothesis type:

| field (from `nr_pdcch_blind_ul_opts_t`) | swept values |
|---|---|
| `tda_start[idx]` / `tda_length[idx]` / `tda_mapping[idx]` / `tda_k2[idx]`, per TDA index | TS 38.214 Table 6.1.2.1.1-2-shaped (S,L,mapping,k2) tuples, mirroring Technique D's `kSL`-style pre-filtered legal-combination table, one per index actually observed in live traffic (not the full spec table — same reasoning `nr_pdsch_config_sweep_init()` already documents: sweeping every table entry needs traffic that exercises every index, which a passive receiver can't arrange) |
| `dmrs_config_type` | {0, 1} |
| `dmrs_add_pos` | {0, 1, 2, 3} |
| `dmrs_max_length` | {1, 2} |
| `transform_precoding` | {0, 1} |
| `mcs_table` | {0, 1, 2} (+ transform-precoding variants per the opts comment) |

Stage b (equivalence collapsing) is genuinely uncertain here, unlike Component 2 — flagged, not
assumed, per the "Shared hypothesis-search engine" section above. Oracle and decision rule
(Stage d) are otherwise identical in shape to Component 2's.

## Data flow

```
Component 1: PDCCH 0_1 candidate LLR --dec@hyp len--> CRC pass/hash --accum--> dci_length locked
Component 2 (gated on: dci_length locked AND bootstrap_rnti != 0):
  [offline] generate admissible width-vectors (2a) --> collapse to classes (2b) -->
  per candidate: pick class --> extract_01() --> plausibility gate (2c) --> RNTI match? -->
    nr_pusch_passive_decode() @ k2 slot --> TB CRC --feed(class)--> sweep decision (2d)
  winner class locked --> g_cfg.ul field widths overwritten
Component 3 (gated on: Component 2 locked):
  same 2a-2d shape over (TDA table / DM-RS config / MCS table / transform precoding) hypotheses
  winner class locked --> g_cfg.ul interpretation fields overwritten
--> from here on, identical to the manual-conf path
```

## Testing

- **Component 1**: 2-3 new gtest cases proving the DL and UL length-sweep states never
  cross-contaminate (reuse the existing synthetic-scorer-stub pattern from
  `nr_pdcch_dci_length_sweep_test.cc`).
- **Shared engine** (`nr_hyp_sweep.{h,c}`): unit tests independent of both UL components —
  synthetic hypothesis types proving each stage in isolation: constraint-list generation produces
  the expected admissible set; equivalence collapsing correctly merges byte-identical-output
  hypotheses and correctly leaves distinct ones separate; the plausibility gate rejects only
  spec-impossible synthetic hypotheses and never reorders survivors; the class-scored decision
  rule converges to the correct class under synthetic pass-rate data and correctly refuses to
  decide when two classes are statistically tied.
- **Component 2/3 — offline known-answer tests before any live capture** (the catalogue's own
  instruction, still followed): synthesize TB-CRC outcomes assuming today's hand-solved
  `pdcch_blind_monitor_ul_dci_bits`/`_ul_tda`/`_ul_dmrs` values are ground truth; confirm each
  engine converges to the correct class. Mirrors Technique D's own "3 offline ground-truth tests."
  - **Falsifiable check, Component 2 only** (answers the open "item E" question rather than
    assuming an answer): record whether the synthetic ground-truth-equivalent class actually
    contains more than one width-vector (i.e., whether the real config has a padding-equivalent
    twin) — turns "residual RRC-only unknowns might be inert" from a hypothesis into a measured
    fact for this cell's config shape.
  - **Same check, Component 3, framed as open rather than assumed**: does equivalence-collapsing
    ever fire on the (TDA/DM-RS/MCS-table) hypothesis space? Record the answer; don't assume it
    either way.
- **Live validation**: not blocked on any known defect (see "Current DL state" above). Still
  sequenced after the offline tests pass, per the catalogue's own instruction.

## Declared Scope / Non-Goals

| Axis | Likelihood on a realistic target network | Failure mode if hit | Verdict / action |
|---|---|---|---|
| Band / numerology / bandwidth not auto-discovered | N/A — different axis | N/A | **Not in scope.** `--ue-scan-carrier` already does blind carrier-frequency search within a given band; band/numerology/PRB count are still CLI flags (`--band`, `--numerology`, `-r`). MIB/SIB1 carry enough to derive these, but no code path feeds them back into runtime config today. Real gap, but a separate MIB-parsing task, not folded into this plan. |
| Compact DCI 0_2/1_2, or fallback-only 0_0/1_0 | Low-medium (Rel-16 URLLC/overhead feature; this project's OCUDU/srsRAN-style target cell and most mid-band SA deployments default to 0_1/1_1) | **Safe failure** — length sweep just never converges, falls back to manual conf | Low criticality. Documented gap, not built for. |
| CA (cross-carrier scheduling) / SUL | Low-medium for a single mid-band TDD cell | Was previously a **silent-wrong-answer risk** (fixed 0 excludes the correct split from the hypothesis space entirely) | **Fixed in this design** — `carrier_indicator_bits`/`ul_sul_bits` are swept dimensions (Component 2 table above), not hardcoded assumptions. |
| Search-space blow-up on a config with more free bits than this cell's | Medium, deployment-dependent | **Safe failure** — refuses to start rather than guessing (`NR_HYP_SWEEP_MAX_RAW`/`MAX_CLASSES` caps) | Medium criticality, already handled by the loud-refusal design. Don't raise the caps to force convergence. |
| RRC-only residual widths (SRS-resource-count-dependent, CSI-trigger-state-dependent) beyond what's listed in Component 2's table | Unknown | Uncertain — depends on whether ties are inert (padding-only) or real | **Can't rate without measuring** — the offline test plan's falsifiable tie-inspection check resolves this, not an assumption. |
| DL Technique D not migrated onto the shared engine | N/A — deliberate scope cut | N/A — DL keeps its existing, tested, working implementation | Not critical now; flagged as future cleanup, see "Scope" above. |

## Files touched

- `nr_hyp_sweep.{h,c}` — new, the shared 4-stage engine (Stages a-d), hypothesis-type-agnostic.
- `nr_hyp_sweep_test.cc` — new, per-stage unit tests on a synthetic hypothesis type.
- `nr_pdcch_blind_monitor_rt.c` — new UL length scorer (Component 1), Component 2/3 wiring into
  `run_occasion()`.
- `nr_pdcch_ul_field_sweep.{h,c}` — new, Component 2: hypothesis generator + constraints for the
  field-width space, built on `nr_hyp_sweep`.
- `nr_pdcch_ul_interp_sweep.{h,c}` — new, Component 3: hypothesis generator + constraints for the
  TDA/DM-RS/MCS-table space, built on `nr_hyp_sweep`.
- `nr_pdcch_ul_field_sweep_test.cc` / `nr_pdcch_ul_interp_sweep_test.cc` — new, offline
  known-answer + tie-inspection tests per component.
- `nr_pdcch_dci_length_sweep_test.cc` — extended, DL/UL independence cases.
- `[sensing] pdcch_blind_monitor_full_auto` — UL wiring (Components 1-3) attaches to this existing
  flag (already reserved for this per the catalogue's recommended sequencing item 5), no new flag
  name.
