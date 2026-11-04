# Addendum to the two-modes design: CRC-free parameter extraction (2026-11-04)

Status: APPROVED by the operator 2026-11-04 (decisions in §9). Extends `2026-11-03-two-modes-design.md`. No code yet.

## 1. Why

Daytime OTA 2026-11-04 [MEASURED OTA], traffic mode, single PCI 923, one 240 s run (tmT2S923):

- DL decodes: 2.8k checks with 96 % CRC OK, but 70k probes with 3 % CRC OK.
- 61 % of the DL probes go to RNTIs that are never solved. The median RNTI appears in only 1 DCI.
- UL: 156k decodes, about 13 CRC OK. The UL layout is already locked without CRC, so these decodes only try to
  learn the MCS table.

In the pair runs, the decode load starves the scan, and scan jobs are dropped (traffic 6-28 % per cell). A dropped
scan job loses DCIs, which is exactly what traffic mode must not do. Today a CRC pass is the only proof of a DCI 1_1
layout; that proof is the expensive part.

Goal: decode **every** DCI field exactly, without PDSCH/PUSCH CRC wherever possible, and keep one LDPC decode
only as an optional confirmation. Never drop a DCI to save decode work. The same machinery must serve sensing,
because the DM-RS channel estimate it computes *is* the DM-RS CFR.

Non-goals: guessing. A field is decoded only when its layout is proven; otherwise it is null and the raw bits
stay in the record for an exact offline re-parse.

## 2. Principle

DCI bits are exact: the PDCCH CRC, scrambled with the RNTI, already protects them. The only unknown is the
**layout**: the position and width of each field. Field order is fixed by TS 38.212, and the total length is
known. A layout is the set of RRC-dependent widths. It is proven by evidence, cheapest first.

One resolver per (cell, RNTI, direction, DCI format), with layout states
UNKNOWN -> CANDIDATE -> SOLVED(bits|phys|crc). The record's `layout_state` gains the evidence level. A layout
becomes SOLVED only when exactly one hypothesis survives a significance test (the Wilson-style test the solver
already uses). Thresholds come from general principles or the sens6 lab, never from DEIB/OTA cells.

## 3. Evidence levels

### Level 0: bit statistics and spec constraints (no IQ)
Per RNTI, keep a bank of the raw payloads.
- Width arithmetic: TS 38.212 fixes some widths (TPC 2, PRI 3, NDI 1, RV 2, HARQ 4/5, MCS 5). Fields with a
  footprint pin the segment edges (Level 1). Widths inside a segment are then forced or limited to a few
  combinations.
- HARQ pid and NDI/RV: consecutive grants advance the pid by small steps; RV is mostly 0 on a new transmission.
  This is the existing UL stage 1, extended to DL 1_1.
- FDRA: the RIV must be valid for N_BWP. TDRA: the set of indices in use.
- MCS table: an MCS index used on a new transmission (NDI toggled) that is reserved in one table excludes that
  table (for example, 28 on a new TB means the 64QAM table).
- Counter DAI: across a UE's consecutive DL DCIs in one HARQ-ACK window, it must step 1, 2, 3, 0. That fixes the DAI
  width (0/2/4) and position.
- K1 (PDSCH-to-HARQ timing): find one table L[index] such that n + L[idx] is a UL slot of the SIB1 TDD pattern
  for every observed DCI. This is a constraint-satisfaction step; it recovers the RRC K1 list and rejects wrong
  splits. FDD cells skip this test and use the other levels.
- Pre-MCS group (VRB-to-PRB, PRB bundling, rate-matching, ZP CSI-RS) and the Rel-16/17 extras: either forced by
  width arithmetic or tested by Level 1 (VRB-to-PRB and PRB bundling change the DM-RS footprint) or by Level 0
  statistics.

### Level 1: physics without LDPC
Each surviving hypothesis predicts the DM-RS: its PRBs (RIV + VRB-to-PRB), symbols (TDRA + additional
positions), ports and CDM groups (antenna-ports field), and scrambling ID. One DM-RS correlation per grant scores
the hypotheses. The front end needs FFTs of 1-2 symbols over the allocated PRBs, against 14 symbols plus
demodulation plus LDPC for a decode. Constellation classification on a few data REs after DM-RS equalisation gives
Qm, and through it the MCS table. UL already does this (`nr_pdcch_ul_layout_lock.h`); DL reuses that design.

### Level 2: optional CRC confirmation
- One decode of the single surviving hypothesis, only when the measured SNR supports its MCS.
- Exponential backoff per RNTI after failures; any CRC pass resets it.
- SI/P/RA decodes, the content of SIBs and RARs, are unchanged.
- In sensing mode, data-aided CFR of enabled units decodes as before.

## 4. Layout families
UEs in one cell usually share an RRC template. A layout SOLVED for one RNTI becomes a cell-level family hypothesis.
A new RNTI is tested against the families first, with Level 0 and Level 1 only. An RNTI seen once can then be
parsed with no probe decode at all. Families are per cell and per DCI format, and are reset on identity change
(existing epoch listener).

## 5. Decode triage (replaces probes)
- A PDSCH/PUSCH decode is queued only when:
  - a hypothesis survived Levels 0-1, and
  - the SNR supports its MCS, and
  - the RNTI has been seen in 2 or more DCIs or matches a family.
- The backoff of §3, Level 2, applies.
- Traffic-mode UL: no decode by default. The MCS table comes from Level 0/1; `qm`/`tbs` stay null until it is known.
- The DCI record is written at accept, whatever the decode decision (unchanged).

## 6. Sharing with sensing
- The Level 1 DM-RS estimate is the per-grant `pdsch_dmrs` / `pusch_dmrs` CFR. It is computed once and used for:
  - layout scoring;
  - CFR export with S2 quality;
  - equalisation when the grant is decoded.
- A wideband CFR needs the DM-RS configuration that Level 1 learns, not LDPC. This removes duplicate front-end and
  channel-estimation work in sensing mode.
- PRG boundaries (S2 rule) apply to any multi-PRB step.

## 7. Tasks (proposed)
- **E1** DL 1_1 Level 0: payload bank, width arithmetic, DAI/K1/MCS-table constraint tests, significance test,
  layout_state evidence level. Pure and unit-testable. Fixes the Task 4 "1_1 HARQ fields mostly null" finding.
- **E2** Shared DM-RS checker (DL; UL reuses the existing one): hypothesis scoring plus constellation-based Qm,
  exporting its estimate as CFR.
- **E3** Cell layout families.
- **E4** Decode triage and backoff (§5); traffic-mode PUSCH default off.
- **E5** Sensing reuse of the E2 estimate; drop the duplicate channel estimation.

## 8. Validation
- Unit tests with synthetic DCI streams of known RRC configs (every field width combination), including adversarial
  cases: two hypotheses that differ only in footprint-free fields.
- sens6 OCUDU: gNB logs are ground truth for the layouts. They are used only for validation, never as receiver
  input. Target: every field equal to the gNB value for SOLVED layouts.
- DEIB OTA, daytime, alternating A/B with 3 or more runs per arm, per cell:
  - DCI records unchanged or higher;
  - decodes per DCI and CPU lower;
  - scan drop_pct lower;
  - field consistency: constraint violations 0 on SOLVED layouts.
- Never claim absolute DCI recall on DEIB.

## 9. Operator decisions (2026-11-04)
- FDD is out of scope: the K1 TDD test may be relied on.
- Traffic-mode PUSCH decode is off by default, because the UL payload is not needed for data collection.
- Order: E1-E5 run before the sensing plan resumes. The goal is a perfect traffic mode. The sensing tasks already
  done (S1, S2) wait.
