# Passive-RX UL Adaptive Discovery — Design

**Branch:** total-passive-rx-UL-DL-graphics · **Host:** sens6
**Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue
**Supersedes-context:** `PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md`'s "FULL UL ADAPTIVE
CATALOGUE (2026-09-07)" section — this doc picks that catalogue's recommended path (A + C) and
turns it into a concrete design. Read that section for the full option space (B/D/E) that this
doc deliberately does not build.

## Problem

UL adaptive discovery is 0% built. Every UL config value today
(`pdcch_blind_monitor_ul_dci_bits` etc.) was hand-solved once, offline, by replaying captured
payloads against this one cell's own gNB scheduler log — a method with no equivalent on a real
deployment with no such log. DL adaptive discovery (CORESET footprint, DCI length, RNTI
bootstrap) is now working end to end and unattended (see "Current DL state" below); this design
closes the equivalent gap for the uplink, to the extent it is closable.

## Current DL state (context, not in scope here)

As of commit `d29596d136` (2026-09-07), the DL chain runs end to end unattended: Technique A
(CORESET footprint via DM-RS correlation, hit-driven termination) converges to the exact known
CORESET; Technique C (DCI-length sweep, CRC-oracle, accumulated across occasions) locks the
correct length; Technique B (RNTI bootstrap) now uses a 16-entry multi-UE table (free-slot-first,
weakest-evidence eviction) instead of a single incumbent slot. That table fix is what closed the
"AUTO decodes 127k genuine payloads but accepts=0" symptom recorded near the end of the handover
doc — a noise RNTI could displace the real, already-confirmed C-RNTI from a single incumbent
slot; with 16 slots and eviction-by-weakest-evidence it can't anymore. **This means UL work is not
blocked on any DL-side defect** — the shared extractor is healthy, and Technique B's bootstrap
RNTI is available to UL scoring immediately (it's the UE's C-RNTI, not DL-specific).

## Scope

Build the catalogue's recommended path only:
- **(A)** Port Technique C's DCI-length sweep to format 0_1.
- **(C)** TB-CRC-scored field-boundary sweep, modeled on Technique D's decision rule
  (`nr_pdsch_config_sweep.c`), generalized to a combinatorial field-width hypothesis space.

Catalogue items (B) (PUSCH cross-observation, 3 new DSP primitives) and (D) (SRS-based sync) stay
out of scope — (B) is a fallback only needed if (C) can't resolve every field, and neither is
needed to reach this design's stated goal.

## Component 1 — UL DCI-length sweep (approach A)

`nr_pdcch_dci_length_sweep.{h,c}` is already format-agnostic (scorer-callback based); no changes
to that module.

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

## Component 2 — UL field-boundary sweep (approach C)

New files `nr_pdcch_ul_field_sweep.{h,c}`. Shaped like `nr_pdsch_config_sweep.{h,c}`
(init/next/feed/winner + global singleton) but **not literally shared code** — the hypothesis
type here is a width-vector over the swept `nr_pdcch_blind_ul_opts_t` fields (see below), not a
small enumerated Cartesian product. Duplicating the ~30-line decision rule (round-robin,
min-trials, win-ratio, floor — copied from `nr_pdsch_config_sweep.c`'s `feed()`) is cheaper and
clearer than building one generic templated engine for two call sites.

**Hard prerequisite**: only runs once `bootstrap_rnti != 0` (Technique B) — scoring TB CRC on
candidates not matched to the real UE wastes every decode cycle. Gate explicitly on this, don't
just let it run inefficiently.

### Swept fields and their legal ranges

From `nr_pdcch_blind_ul_opts_t` (`nr_pdcch_blind_monitor.h`), enumerate every legal combination of
these fields whose total equals the live-verified `dci_length` (from Component 1):

| field | legal range (TS 38.212 7.3.1.1.2) |
|---|---|
| `carrier_indicator_bits` | {0, 3} — **added per the CA/SUL fix below, was previously assumed fixed 0** |
| `ul_sul_bits` | {0, 1} — **added, was previously assumed fixed 0** |
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

Raw Cartesian product is large; filter to combinations summing to `dci_length` before generating
hypotheses (mirrors the original manual solve's "3963 raw → 60 survivors" experience on this
cell). **New hard cap, `NR_PDCCH_UL_SWEEP_MAX_HYP` (recommend 256, larger than Technique D's 64
since this space is bigger and cell-dependent)**: if the filtered set exceeds it, refuse to start
the sweep and log loudly (`"UL field sweep: N candidate splits exceeds cap, refusing to guess"`) —
never silently truncate, since truncation would bias toward whichever were enumerated first.

### Wiring

For each 0_1 candidate, once a hypothesis is picked (round-robin, same pattern as Technique D):
build a full `opts` (fixed/known fields + hypothesis's swept fields) → `extract_01()` → if
plausible and RNTI matches the bootstrap set → build the UL grant → `nr_pusch_passive_decode()` at
the k2-offset slot → feed TB-CRC pass/fail back into the sweep.

**No PUSCH-decoder signature change needed.** `nr_pusch_passive_decode()` already takes an
interpreted grant (`nr_pdcch_blind_ul_result_t`), not raw field widths — the catalogue's wording
("generalize the PUSCH decoder to accept a swept field-width vector") is achieved entirely at the
PDCCH extraction call site, by varying `opts` per hypothesis before calling `extract_01()`.

### Expected tie behavior

Per this project's own prior finding, multiple hypotheses can be TB-CRC-identical (differ only in
where zero-valued padding sits). This is not a bug — see the offline test plan below, which turns
"is this padding-only" into a measured fact rather than an assumption.

## Data flow

```
Component 1: PDCCH 0_1 candidate LLR --dec@hyp len--> CRC pass/hash --accum--> dci_length locked
Component 2 (gated on: dci_length locked AND bootstrap_rnti != 0):
  PDCCH 0_1 candidate LLR + locked len --dec@hyp opts--> extract_01() --RNTI match?-->
    nr_pusch_passive_decode() @ k2 slot --> TB CRC --feed--> sweep decision
  winner locked --> g_cfg.ul overwritten --> from here on, identical to the manual-conf path
```

## Testing

- **Component 1**: 2-3 new gtest cases proving the DL and UL length-sweep states never
  cross-contaminate (reuse the existing synthetic-scorer-stub pattern from
  `nr_pdcch_dci_length_sweep_test.cc`).
- **Component 2 — offline known-answer tests before any live capture** (this is the part the
  catalogue explicitly calls for): synthesize TB-CRC outcomes assuming today's hand-solved
  `pdcch_blind_monitor_ul_dci_bits` values are ground truth; confirm the engine converges to them.
  Mirrors Technique D's own "3 offline ground-truth tests" pattern.
  - **New, falsifiable check** (answers the open "item E" question from the catalogue rather than
    assuming an answer): when the synthetic test produces a tie, inspect whether the tied
    hypotheses differ only in fields that don't change the extracted grant (inert padding) or in
    fields that do (a real, unresolved ambiguity). This measurement is what turns "residual
    RRC-only unknowns might be inert" from a hypothesis into a fact for this cell's config shape.
- **Live validation**: not blocked on any known defect (see "Current DL state" above). Still
  sequenced after the offline tests pass, per the catalogue's own instruction.

## Declared Scope / Non-Goals

| Axis | Likelihood on a realistic target network | Failure mode if hit | Verdict / action |
|---|---|---|---|
| Band / numerology / bandwidth not auto-discovered | N/A — different axis | N/A | **Not in scope.** `--ue-scan-carrier` already does blind carrier-frequency search within a given band; band/numerology/PRB count are still CLI flags (`--band`, `--numerology`, `-r`). MIB/SIB1 carry enough to derive these, but no code path feeds them back into runtime config today. Real gap, but a separate MIB-parsing task, not folded into this plan. |
| Compact DCI 0_2/1_2, or fallback-only 0_0/1_0 | Low-medium (Rel-16 URLLC/overhead feature; this project's OCUDU/srsRAN-style target cell and most mid-band SA deployments default to 0_1/1_1) | **Safe failure** — length sweep just never converges, falls back to manual conf | Low criticality. Documented gap, not built for. |
| CA (cross-carrier scheduling) / SUL | Low-medium for a single mid-band TDD cell | Was previously a **silent-wrong-answer risk** (fixed 0 excludes the correct split from the hypothesis space entirely) | **Fixed in this design** — `carrier_indicator_bits`/`ul_sul_bits` are now swept dimensions (see table above), not hardcoded assumptions. Cheap: 2 fields, negligible growth to the search space. |
| Search-space blow-up on a config with more free bits than this cell's | Medium, deployment-dependent | **Safe failure** — refuses to start rather than guessing (the `NR_PDCCH_UL_SWEEP_MAX_HYP` cap above) | Medium criticality, already handled by the loud-refusal design. Don't raise the cap to force convergence. |
| RRC-only residual widths (SRS-resource-count-dependent, CSI-trigger-state-dependent) beyond what's listed above | Unknown | Uncertain — depends on whether ties are inert (padding-only) or real | **Can't rate without measuring.** The offline test plan's new falsifiable tie-inspection check is what resolves this, not an assumption. |

## Files touched

- `nr_pdcch_blind_monitor_rt.c` — new UL length scorer, second sweep state instance, Component 2
  wiring into `run_occasion()`.
- `nr_pdcch_ul_field_sweep.{h,c}` — new, Component 2 engine.
- `nr_pdcch_ul_field_sweep_test.cc` — new, offline known-answer + tie-inspection tests.
- `nr_pdcch_dci_length_sweep_test.cc` — extended, DL/UL independence cases.
- `[sensing] pdcch_blind_monitor_full_auto` — UL wiring attaches to this existing flag (already
  reserved for this per the catalogue's recommended sequencing item 5), no new flag name.
