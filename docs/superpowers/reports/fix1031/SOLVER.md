# fix1031/solver: D10 (first decode), D11 (PARK v2), D6 (all-CB diagnostic)

Base mpci-int-1031 (9d1649f17d). Host: DGX aarch64. Evidence label for all test results: [OFFLINE VERIFIED, DGX aarch64 unit] unless stated.

## What changed
- D10, `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c`: the slot-phase census `g_phs` is kept at 160-slot resolution; the phase period `g_per`
  (per cell, `NR_CELL_STATIC`) is learnt: 20 until the CRC passes of full-length allocations put two different start symbols on one phase
  (`phase_conflict`), then 40, 80, 160 (`phase_census_note`, `period_grow`). Every use of the old constant (`SOLVER_PHASES`=20) now goes through
  `ph_of()` / `phase_S()`: the seed S, the per-RNTI SOLVERPHASE/SOLVERTDDEND tables (arrays now 160 long; `period_grow` spreads learnt entries over
  the finer phases). Reset restores 20. New accessor `nr_pdsch_solver_phase_period()` (nr_pdsch_solver.h). No flag.
- D11, `nr_signal_evidence.h`: the Beta(a,1) LLR is replaced by a Bernoulli exceedance SPRT per hypothesis class: draw x = (p_single < 0.05); H1 rate 0.3;
  H0 rate = `R->h0`, measured on the reference with the same estimator (0.08 on a Gaussian cell: the mirrored upper tail and 0.5 dB bins make
  P0(p<0.05) > 0.05), clamped [0.05, 0.15]. The H1-shape (`NSE_MIN_PASSES` real passes) precondition is dropped; the noise reference alone is needed.
  `nse_rnti_t.had_pass`: any real TB pass or a proven solution means never parked; a parked RNTI that passes is un-parked at once. Re-test 1 in 2^k
  unchanged (`nse_due`). `nse_h1_shape` stays only for the PARK log line in nr_pdsch_solver.c.
- D6, new `tests/pdsch_cb_all_offline.c` + CMake target `pdsch_cb_all_offline` (next to `pdsch_cb_offline`): decodes every CB of one
  ISAC_PDSCH_LLR_DUMP record and prints `cb -> bit range -> first..last OFDM symbol -> iters -> ok`, plus the first failed block. Symbol borders
  from S, L, PRB, Qm, Nl, optional DM-RS symbols/data REs (CLI args), scaled to G. No runtime change; NR_CB_FAIL_LIMIT untouched.

## Tests (test_nr_pdsch_solver, 66 pass, 0 fail)
- `SeedNewRnti.FirstSeededProbeCarriesThePhaseStartSymbol` (existing, now built): 40/40 new RNTIs pass in the first grant. Its population loop needed
  `g > 120` (was 60): with 60 no seed tuple existed yet (no seed probes, 0/40).
- new `SeedNewRnti.PhasePeriodOfFortySlotsIsLearntAndSeeded`: period 20 -> 40 learnt, 40/40 first-grant passes; reset returns 20.
- new `SeedNewRnti.FirstSeededProbeCarriesTheOtherFamilyOfAnUnseparatedRep` (cdm=1 vs cdm=2 family, population rep 10 of merged 10..19): 40/40.
- Park: `InaudibleRntiParksWithinFortyDraws` (one class, pure noise: 300/300 parked, mean 32.4 draws, p95 70, worst 150), `DecodableRntiNeverParks`
  (weakest-tenth signal, 600 draws x 300: 0 parked), `AnRntiWithAnyCrcPassIsNeverParkedAndIsUnparked`. Existing Park tests adjusted:
  14-class noise RNTI parks in mean 77 grants (bound relaxed 40 -> 100: the last of 14 independent class tests decides); the "no decision without
  H1 shape" part removed (no longer a precondition). Un-park after gaining signal: 100/100, mean 32 grants.

## NOT verified
- No replay, OTA or receiver run; PARK/period/first-decode effects on air are untested. `pdsch_cb_all_offline` builds but was NOT run on a real dump
  (no PDL1 dump at hand, libldpc.so not built here); the symbol map assumes a symbol-major LLR buffer. The rep-family test was not checked with
  the refinement seeding disabled (no negative control).
- `tools/mpci/static_audit.py --check` already fails on the base (stale line numbers in mpci_statics.tsv); `g_per` is NR_CELL_STATIC; the tsv was not
  regenerated (a --write rewrites ~1600 lines and would conflict with the other fix branches). The regenerate belongs to the integration step.

## Expected on-air effect [HYPOTHESIS]
- 923 (40-slot pattern): `g_per` goes to 40; first-seeded S and SOLVERPHASE overrides stop mixing slot 8/18; log line
  `PDSCH_SOLVER cell slot-phase period 20 -> 40 slots`. 415: time to first CRC per new RNTI back to base level (first-slot pass 23-30 %).
- PARK lines (`PARK` log with n_obs/n_park) appear for inaudible RNTIs within ~30 draws per class instead of never; parked RNTIs decode 1 grant in 2^k.

## Risks for other fixes
- nr_pdsch_solver.c: the per-RNTI struct grew (3x uint32 + 2x int8 arrays 20 -> 160, about 2 KB x 256 RNTIs); the TDRA starvation rule (rr-fix-tda,
  same file) and any change using `% SOLVER_PHASES` must use `ph_of()`. Any new reader of `g_phs` must fold by `per_now()`.
- A noise reference contaminated by undecoded real RNTIs raises `h0` (clamped 0.15) and slows parking, never speeds it.
