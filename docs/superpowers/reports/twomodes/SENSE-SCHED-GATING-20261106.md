# SENSE_SCHED gating: PDSCH units kept off in 923 S1 / 415 S2 (diagnosis and proposed fix)

Scope: diagnosis only, no production code changed, nothing run on the air. Evidence labels: [OTA LOW LOAD, DGX GB10, 4 RX] = raw logs of
`/home/nicola/NICOLA/ota_runs/sp_ota_20261105_225342` (per-run `run.log`, SENSE_SCHED / SENSE_UNITS lines); [CODE-READ] = source at HEAD b75ae5f8f6;
[OFFLINE] = existing unit tests. Cells are never merged: 923 and 415 are quoted separately.
Source files: `openair1/PHY/NR_UE_TRANSPORT/nr_sense_sched.c` (decision, pure), `nr_sense_sched_rt.c` (thread, sampler, log), `nr_sense_unit.c` (unit counters), `nr_srs_passive.c`, `nr_traffic_decode.c`.

## Root cause (short)

The budget is not what keeps PDSCH off. In 923 S1 the pool had room (budget = (300 - base) x 0.6 ms, base 138-210 ms in 46 of 55 decisions) and
`binding=queue_ul` is only the label of the *most utilised* resource when nothing was rejected. PDSCH stays off because of the **ranking and the one-cold-unit-per-decision admission**:

1. Unit value is `wb_rate_hz x valid_ratio x prbs x age_boost`, and for SRS and PUSCH-DMRS `wb_rate_hz` counts *gated grants / UL slots*, not records. An SRS unit that has never produced a record
   is priced at the maximum: 600/s x 0.2 (unknown-ratio floor) x 128 PRB x 2.0 (age boost) = 30720, constant for the whole run, 4-30x above any PDSCH or PUSCH unit.
2. A unit with no cost history ("cold") is admitted at most **one per decision** and needs **2 agreeing decisions** (`SENSE_CONFIRM`) to be enabled. The cold slot went to a different zero-yield PUSCH-DMRS RNTI at nearly every decision (churn), so the 2-in-a-row streak almost never completes, and a PDSCH unit never gets the slot.
3. Nothing in the score is updated by what the unit actually yields while its ratio is "unknown" (`n_cfr == 0`), so SRS (0-4 records) is never demoted.

## 1. Which term admitted srs and pusch_dmrs, and what "queue_ul" means

- `nr_sense_sched_decide` admits in rank order (nr_sense_sched.c:306-327). Per candidate `fits()` (line 139) tests every resource; `uses()` (line 85) gives QUEUE_UL to SRS only.
  QUEUE_UL cap = `NR_UNIT_LATE_NS` = 20 ms (nr_sense_unit.h:19), base 0 "unmeasured" (nr_sense_sched_rt.c:133), budget = 20 x (1 - 0.40) = 12 ms.
- The SRS unit's measured queue wait is 1.1-1.7 ms (`wait99` in SENSE_UNITS, 923 S1 decisions 10-53), i.e. 10-14 % of its budget. It is never over budget, so it never rejects anything.
- `binding` is set in two places. If a candidate is rejected (line 323-326) it is the resource with the worst overrun. **If nothing was rejected** (line 328-335) it is the resource with the highest
  `use/budget`. 46 of 55 decisions in 923 S1 print `binding=queue_ul admitted=2` [OTA LOW LOAD]; with `admitted=2` = SRS + one more and no rejection, queue_ul (13 %) simply beats pool (SRS cpu99 2.7 ms of ~70 ms = 4 %).
  So `queue_ul` is a report of "tightest", not a limit that was hit. The same `binding=queue_ul` appears in 415 S2 (34 of 47 decisions, admitted=2) and in 923 S2 at decisions 4-8 and 12 before PDSCH units warmed up.
  The report's "binding=queue_ul" reading in finding 1 should be corrected accordingly.
- Why SRS scores 30720: SRS has no job queue; `nr_sense_unit_gate_deferred(NR_UNIT_SRS, 0, 0)` (nr_srs_passive.c:455 -> nr_sense_unit.c:424-433) books **one skip of `sense_min_prb` PRBs per UL slot** while the unit is off,
  and `nr_sense_unit_job(NR_UNIT_SRS, ..., sense_min_prb, ...)` (nr_srs_passive.c:358) books the same message per processed capture when on. So `wb_rate_hz` = UL-slot rate (586-600/s in every SENSE_UNITS line, ON or off)
  with `est_usable_prbs` pinned to `sense_min_prb` (128) even when T_cell has stepped down to 32. Then ratio: `nr_sense_unit_ratio_known` needs `n_cfr > 0` (line 109); SRS has 0-4 records, so the ratio is the floor
  `SENSE_ELIG_VALID_MIN` = 0.2 (line 228); boost: no valid wideband record ever, so the age boost sits at the cap 2.0 (nr_sense_sched.c:231). 600 x 0.2 x 128 x 2 = 30720 = the admitted value printed in every one of the 55 decisions.
- PUSCH-DMRS units get the same treatment: one message per UL grant seen (146378 PUSCH tries in 923 S1 over ~214 s). Their value (550-9300) is above every PDSCH unit that was ever a candidate (pdsch_data 2631 once, pdsch_dmrs 450-3000 at decisions 10, 12, 32, 42, 46).
  Their CFR is empty (valid_cfr_prbs p90 = 0), so each PUSCH RNTI is enabled, measured at ratio ~0, shed (back-off 4..64 decisions), and the next RNTI takes its place as a fresh cold unit with the same inflated score.

## 2. Why it persists for the whole run

The decision is re-evaluated every `SENSE_DECIDE_NS` = 4 s (55 decisions per run; nr_sense_sched_rt.c:299-303), so this is not a latched decision. What persists is the *input*:

- SRS: stays admitted forever (value 30720 every decision) and, once ON, is an incumbent (rank x `INCUMBENT` = 1.25, line 237). 923 S1 `enabled_now` = 1-2, with SRS ON in nearly all SENSE_UNITS lines.
- Cold slot (line 307, 311, 322): only the highest-ranked cold candidate is admitted in a decision. Candidates are per (RNTI, source) rows (139 rows in 923 S1) and ranks change between decisions, so the admitted second slot rotates:
  decisions 3..10 of 923 S1 admit pdsch_data/18356, pusch_dmrs/19216, 19216, 17942, 17942, 20398, 18210, pdsch_dmrs/22590. A change to enabled needs `streak >= SENSE_CONFIRM` (= 2; line 372),
  and `streak` is reset whenever the unit is not admitted (line 347-349 region, `want == enabled`). A rotation of the cold slot therefore starves the confirmation. Only 9 enables happened in the whole run (SCHED `enables 9`), all PUSCH-DMRS or SRS.
- Shed units are held off 4, 8, ... 64 decisions (`SENSE_REPROBE_*`, line 357-361): the PUSCH units that were enabled and found empty stay out, but the number of PUSCH RNTIs is large, so there is always a new one.
- Once enabled and warm (non-cold), a unit no longer uses the slot: 923 S2 reached `admitted=3..5`, `enabled_now=3-4` after PDSCH 18356 got two consecutive cold slots at decisions 4-6 (pdsch_dmrs, then pdsch_data, enabled at decision 6), and from then on incumbency (x1.25) held it.
  S2 vs S1 in the same cell is therefore a bootstrap race of the cold slot (S2: UL load lower, a PDSCH unit won it twice in a row), not a different budget (S1 base mean ~170 ms, S2 ~190 ms; both had room).
- 415 S2 is the same pattern (`binding=queue_ul admitted=2` for 34 of 47 decisions; units_enabled 1).
- Not measured: the number of *eligible* PDSCH candidates that were skipped by `cold_taken`. SENSE_UNITS prints only the first 8 rows of 139 and the decision log does not count deferred cold units. The mechanism is certain from the code and from the rotation of the admitted second slot; the count is [HYPOTHESIS: several eligible PDSCH cold candidates per decision].

## 3. Do scores use the actual yield?

Partly, and not where it matters:

- `valid_ratio` is an EWMA over the last records (nr_sense_unit.c:254-285) and eligibility drops below 0.2 (line 118). This catches a unit that exports records with no valid PRB (PUSCH-DMRS after its first records).
- A unit that produces **no record at all** has `n_cfr == 0`, so its ratio is "unknown" and passes eligibility (line 118, tested as intended by `Sched` test at nr_sense_sched_test.cc:280 "unknown ratio is not a bad ratio").
  There is no rule "enabled for a full window and still zero records". SRS in 923 S1 was ON for most of 214 s with 0 records exported (`srs_pregate_skip` ~ jobs) and kept value 30720.
- The age boost (up to x2) is anchored to `last_valid_wb_ns`, i.e. it *rewards* a unit that never yielded.
- The rate term counts offered grants/UL slots, which is the right proxy for a disabled PDSCH unit (its TBs are CRC-decodable on the DL, `pdschq_crc_ok` 5008) but is a bad proxy for SRS and PUSCH on this site (no UL TB decoded, SRS 0-4 records).
- So a unit yielding zero valid CFR can outrank one yielding wideband CFR, as the logs show.

## 4. Proposed general fix (not implemented)

No constants, flags, or cell tuning; every term uses an existing constant (`SENSE_WIN_DECISIONS`, `SENSE_ELIG_VALID_MIN`, `SENSE_CONFIRM`, `SENSE_REPROBE_*`) or a measured quantity.

**F1 - measured zero yield makes a unit ineligible (decide step, nr_sense_sched.c:222 region).**
Per (cell, slot, src) keep `en_dec` (decisions enabled in a row) and `cfr_at_en` (`n_cfr` when it was enabled; both new fields in `nr_sense_sched_state_t`). A unit enabled for >= `SENSE_WIN_DECISIONS`
decisions whose `n_cfr` did not grow, or whose window `wb_valid` count is 0, is "ratio known = 0": not eligible, so the existing shed path (`want=false`, `SENSE_CONFIRM` decisions) and `SENSE_REPROBE_BASE..MAX` back-off apply. Expected effect: SRS in 923 S1 leaves the admitted set after ~16 s and is re-probed after 4, 8, ..., 64 decisions
(on for ~16 s of every few minutes instead of the whole run).

**F2 - source-level yield prior instead of the constant 0.2 for an unknown ratio (line 228).**
For a unit with no usable ratio use the `valid_ratio` mean of the *known* units of the same source in the same cell (measured when they were enabled; decayed by the same `SENSE_COST_MEM_DECAY` memory), never below `SENSE_ELIG_VALID_MIN`; a source with no known unit keeps an optimistic ratio 1.0
so that every source is probed once. Expected effect: after a few zero-yield PUSCH-DMRS RNTIs the next PUSCH RNTI is priced at the floor 0.2 (not 1.0) and a never-probed or productive PDSCH unit outranks it; new RNTIs no longer re-inherit an inflated score.
Also: for SRS, use `est_usable_prbs` = the cell floor, not `nr_isac_sense_min_prb()`, so the value follows T_cell (nr_srs_passive.c:358/455 or in the view).

**F3 - a pending unit keeps the cold slot (rank, line 237; admission, line 311).**
Rank a candidate with `streak > 0` (admitted last decision, not yet confirmed) with the same `INCUMBENT` factor as an enabled unit, so the cold slot cannot rotate away before `SENSE_CONFIRM` completes. And count `cold_taken` only for cold units whose
demand estimate is 0 (unknown); a cold unit with a non-zero source-level estimate (lines 267-273) is already bounded by `fits()` and need not be limited to one per decision.
Expected effect: PDSCH units reach warm state in 2 decisions (8 s) when they are the best candidate, as happened by luck in 923 S2.

**F4 - report (no new flag).** Print `binding=none(top=<res>)` when nothing was rejected and add `elig=<n> cold_deferred=<n>` to the SENSE_SCHED line, so the next campaign can see eligible candidates that were skipped. This is a log string change only.

Expected result: PDSCH-DMRS/data units admitted within ~10 decisions in the S1-like case, SRS and PUSCH-DMRS present but demoted to probe cadence when they yield nothing; where UL does yield (rfsim PUSCH, future UL cell) they keep full priority through F2 because their measured ratio is high.
Risk: F1 can disable SRS on a cell where SRS records arrive rarely (<1 per 16 s); the back-off keeps it probed. Check this against sens6 captures before merging (sens6 stays frozen, read only).

### Offline verification (no air, no receiver)

Pre-check done now: `pgrep -x nr-uesoftmodem` empty; `flock -s /home/nicola/NICOLA/heavy.lock` then `./test_nr_sense_sched` and `./test_nr_sense_unit` in
`cmake_targets/ran_build/build` -> 41 tests passed at the current binaries [OFFLINE, DGX aarch64].

To add with the fix (tests, no production flag):
- `nr_sense_sched_test.cc` (harness `h.step()`): (a) SRS unit with `wb_rate_hz` 600, `n_cfr` 0, enabled for `SENSE_WIN_DECISIONS` decisions: expect eligible == false after F1, shed after `SENSE_CONFIRM`, re-probe after 4 decisions; (b) two cold units, PUSCH `val` high and PDSCH lower, with the PUSCH unit rotating its RNTI every decision: expect the PDSCH unit enabled within 3 decisions (fails at HEAD); (c) cold slot with streak (F3): same candidate admitted twice in a row gets enabled; (d) a high-yield UL unit (ratio 0.9) still outranks PDSCH.
- Replay the 55-decision S1 sequence as a sampler-driven scenario (`nr_sense_sched_set_sampler`, as `nr_sense_sched_rt_test.cc` does): feed per-decision unit views built from the SENSE_UNITS/SENSE_SCHED values above (SRS wb 600, no CFR; PUSCH RNTIs rotating; PDSCH 18356/22590 wb 5-30).
- Run: `cd cmake_targets/ran_build/build && ninja test_nr_sense_sched test_nr_sense_sched_rt test_nr_sense_unit && flock -s /home/nicola/NICOLA/heavy.lock ctest -R 'nr_sense' --output-on-failure` (shared lock; check `pgrep -x nr-uesoftmodem` first; never build while the receiver runs).
- Rfsim/replay: the existing 4-RX regression `tests/passive_rx/dgx/rfsim_regress.sh` under `flock -x /tmp/td_measure.lock` (idle host, exclusive; run by the operator) with `mode=sensing` and the sens6 UL captures read-only, and compare units_enabled / exported PDSCH rec/s per source before and after.
  A replay cannot reproduce the exact OTA UL rotation; the gate is "PDSCH units admitted whenever a PDSCH CRC-ok stream exists, SRS/PUSCH demoted when they export zero valid PRB", not equal numbers.

## PRACH worker drops (26-47 % on 923): opinion

[CODE-READ, OTA LOW LOAD] The PRACH path is not a scheduler unit: `nr_unit_src_t` has pdsch_dmrs/pdsch_data/pusch_dmrs/srs only, so there is no gate, no value, no way for SENSE_SCHED to shed it, and its work is never booked as a unit's cost.
Each job takes one HOUSE-class pool lease (nr_prach_passive.c:863) and runs ~2-3.5 ms; `protected_total()` counts HOUSE in the pool baseline (nr_sense_sched_rt.c:70), so PRACH shrinks the pool headroom `(cap - base) x 0.6` of every unit, which is a real (if small, ~0.3 core at 1 job / 10 ms) cost to the other sensing units. The drops themselves (single worker, job ring full or producer busy,
nr_prach_passive.c:1040/1088) lose nothing that exists, since PRACH export is 0 on NSA cells, but a producer that fails 26-47 % of the time shows the worker is running behind the host (7.2-7.6 cores on 923). The general fix is to make PRACH a scheduler unit like SRS (a per-cell unit, value = measured valid PRACH records per second, same F1/F2 yield rule): with zero export over a window it is
shed with the exponential re-probe back-off, which frees its HOUSE lease and CPU for units that yield; on a cell with SA PRACH it keeps its priority. No per-cell constant, no new flag.
