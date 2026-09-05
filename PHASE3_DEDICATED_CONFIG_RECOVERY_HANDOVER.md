# Phase 3 — recover the dedicated config by search — HANDOVER

**Status: PARTIAL. Techniques B and C's own logic are unit-tested and correct; Technique A does
NOT converge on live air, and Task 5's live validation did not reach Steps 2-4 as a result.**
**Branch:** total-passive-rx-UL-DL-graphics
**Host:** sens6 · **Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue

## What this phase adds

`pdcch_blind_monitor_autodiscover`: intended to recover the dedicated CORESET geometry
(Technique A, DM-RS correlation across the whole carrier) and DCI 1_1 payload length
(Technique C, histogram sweep) by search, keyed by a bootstrapped C-RNTI (Technique B, from
Phase 1's common search space) rather than requiring `pdcch_blind_monitor_coreset`/`_ss`/`_bwp`/
`dci_length_override` to be hand-derived from a gNB log. Default off; requires
`pdcch_blind_monitor_autoconf=1` (Phase 1) already on. Tasks 1-4 (bootstrap accessor, correlation
scanner, length sweep, orchestration wiring) are implemented, unit-tested (69/69 offline tests),
and code-reviewed clean (two fix rounds on the integration task caught and fixed a real stack
buffer overflow and a `g_cfg`-leakage bug — see `.superpowers/sdd/2026-09-04-phase3-dedicated-
config-recovery/progress.md` for the full review record).

## Measured (Task 5, live validation on sens6's own cell, known-good ground truth)

**Ground truth** (already manually configured and log-confirmed, from Phase 1's work): dedicated
CORESET `num_groups=45` (270 RB) starting at RB 0, `bwp=[0..273)`, 1 symbol duration, PCI=2 as the
DM-RS scrambling ID. This is the answer key every measurement below is checked against.

- **CORESET footprint discovery: FAILED TO CONVERGE.** Zero `"Phase 3 autodiscover -- CORESET
  footprint"` lines across several live captures (45-200+ seconds each, both with confirmed heavy
  DL/UL iperf traffic and during a quieter traffic period), single-antenna (`NANT=1`/`MRC=0`,
  per commit `5b519a1429`'s finding that 4-branch MRC gives 0% CRC on this deployment).
- **Root cause investigated, not (yet) found.** Added two temporary (now permanent, env-gated:
  `ISAC_DISCOVER_DIAG=1`) diagnostics to trace exactly where the pipeline stalls:
  1. `nr_pdcch_blind_monitor_process()` IS being entered (confirmed: `enabled=1`,
     `autodiscover=1`, `pdsch_decode=2` all correct) and `nr_pdcch_blind_monitor_autodiscover_step()`
     IS being called continuously (thousands of calls observed).
  2. `nr_pdcch_coreset_map_scan()` (Technique A) returns `n=0` candidates on **every single
     sampled call** — not "occasionally misses," never once found anything above the 0.836
     significance bar.
  3. **Decisive measurement**: instrumented the raw, per-window correlation value regardless of
     threshold. At the KNOWN-GOOD location (RB 0, symbol 0), live correlation measured
     **0.02-0.39** across ten samples spanning a full capture — scattered around and below the
     code's own documented PURE-NOISE floor (~0.209 for an 18-pilot window), not clustered near
     the threshold. The carrier-wide maximum (best of 45 windows) measured **0.35-0.51**, with the
     winning `rb_offset` jumping unpredictably between samples (18, 108, 42, 36, 216, 174, 36, 222,
     228, 12...) — the signature of extreme-value noise statistics (max-of-45 trials), not a real,
     stable transmission. **This is not a threshold-calibration problem** (the code's own comment
     claims 0.8-0.95 for a real hit; live values never got within 2x of the 0.836 bar) — something
     is preventing the reference DM-RS from correlating with whatever is actually on the air at
     that location, or the assumed (RB, symbol) location itself does not carry a live dedicated
     PDCCH transmission as often as needed for this scan to see one.
  4. **Eliminated as causes** (checked directly against this file's own already-proven-working
     code, not assumed): DM-RS reference-generation call parameter order (`nr_gold_pdcch`'s
     `(N_RB_DL, symbols_per_slot, nid, ns, l)` matches exactly, `ns`/`l` not swapped);
     `nr_pdcch_dmrs_ref`'s count-parameter semantics (already fixed correctly in Task 2, confirmed
     against the real function `nr_dmrs_rx.c:124-129`); the FEP call's `sample_offset` argument
     (`0`, identical to the proven-working candidate-scan FEP at this file's own line 982);
     correlation being scale-invariant (rules out any amplitude/normalization bug, since the metric
     divides out any constant gain).
  5. **Not yet checked, flagged for the next session**: whether `disc_symbol=0` (hardcoded, per a
     `ponytail:` comment assuming "this deployment's dedicated CORESETs are always 1 symbol
     starting at 0") is actually correct under autodiscover specifically — the proven-working scan
     path (`run_occasion()`) reads `symbol = cfg->ss_first_symbol` from CONFIG rather than
     hardcoding it, and while CSS0 autoconf's own `ss_first_symbol=0` happens to match the manual
     ground truth's assumed value, this was inferred, not independently re-confirmed against a
     live `ISAC_OTA_CFG=1`/`ISAC_PDCCH_CFGTRACE=1` dump during an autodiscover run specifically.
     Also not checked: whether a residual timing offset (STO) — the exact class of impairment
     this project's separate OTA-sync stack (CLAUDE.md section 6/7) exists to correct — imposes a
     per-subcarrier phase ramp across the 6-RB window that would decorrelate this SPECIFIC
     magnitude-of-complex-sum measurement while leaving unrelated, phase-insensitive measurements
     (RFCENSUS's `rf_pow`, CSI-RS SNR) unaffected; `nr_slot_fep_ant()`'s own STO/CFO handling
     relative to what the rest of the receive chain applies has not been traced end to end.
- **dci_length sweep: NEVER REACHED** (gated on Step 1's discovery succeeding first — never got
  past the `disc_n_cand > 0` check per the fix-round-1 design). Cannot be scored.
- **Genuine FULLCRC decodes at the bootstrapped RNTI: N/A**, blocked by the above.
- **Time-to-discovery: N/A** (never converged within any tested window, up to ~200s).

## What no amount of this phase's search recovers (per the roadmap's own caveat)

TDRA table contents, MCS table selection, DM-RS additionalPosition, rate-matching patterns —
Techniques A-C recover WHERE and HOW LONG, not how to INTERPRET a payload whose CRC has already
passed. A second, TB-CRC-oracle search stage is flagged, not attempted, by this plan — see the
roadmap's "What no amount of search recovers" section and `PHASE1_CSS0_AUTOCONF_HANDOVER.md`'s
section 12 for the exact prior bug shape (total DCI length correct, two field widths wrong, 0%
CRC while RNTI cross-checks still passed). This remains moot until Technique A converges.

## Still open

1. **The actual root cause of Technique A's live-air non-convergence.** Two concrete next
   experiments, in priority order:
   - Confirm `cfg->ss_first_symbol` and `cfg->ss_monitoring_slot_periodicity`/`_offset` under a
     live `autodiscover=1` run (they are inherited from CSS0 autoconf, never set by Technique A/B/C
     themselves) actually match the manual ground truth's assumed values, rather than trusting the
     inference in this doc.
   - Trace `nr_slot_fep_ant()`'s STO/CFO handling against what the proven-working candidate-scan
     path effectively benefits from (it runs downstream of the RT tap's own timing-tracking loop
     applied elsewhere in the receive chain) — if the discovery tap's single-antenna FEP is missing
     a correction the normal path gets "for free" from shared receiver state, that would explain
     noise-level correlation at a location known to carry real reference signal energy.
2. Task 5's Steps 2-4 (footprint/length/decode scoring) cannot run until (1) is resolved.
3. The known dual-frame-of-reference issue in `bwp_start` (flagged during Task 4's review,
   parked as latent/inert on this specific cell since its CORESET starts at RB 0) remains
   unexercised — it would only matter once a real footprint with a nonzero `rb_offset` is
   ever discovered, which has not happened.
4. `dci01_scan` is still leaked from CSS0's config into the (currently never-reached) dedicated
   search — parked in the review record, harmless for the DL-1_1 goal this feature targets.

## Diagnostics added (kept, env-gated, zero-cost when off — same convention as this project's
other `ISAC_*` debug flags)

- `ISAC_DISCOVER_DIAG=1` (wired into `tests/passive_rx/captures/run_arm.sh` as `DISCOVERDIAG=1`):
  prints `DISCOVERDIAG ENTRY`/`CFG` once, then `DISCOVERDIAG calls=N n=...` every 200 calls (or
  immediately whenever `n>0`), from `nr_pdcch_blind_monitor_rt.c`/`nr_pdcch_blind_monitor.c`.
  Also enables `COREMAPDIAG calls=N rb0_corr=... max_corr=... max_rb=... thresh=...` from
  `nr_pdcch_coreset_map.c`, printing the raw correlation at the known-good RB 0 and the
  carrier-wide maximum regardless of whether either clears the significance bar — the instrument
  that produced this handover's decisive measurement.
