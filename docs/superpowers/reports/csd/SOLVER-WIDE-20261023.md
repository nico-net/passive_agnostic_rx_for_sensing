# SOLVER-WIDE 2026-10-23: wide-only RNTIs were never solved

Branch `csd/solver-wide` off adaptive-rx-UL-DL @ e9c874c308. Host: DGX aarch64 (GB10), build_t26, 4 RX, 106 PRB rfsim phy-test. Labels per PROJECT_MEMORY 0.1; rfsim CRC is an upper bound.

## Cause [READ]
Since 0fb22ecbd0 the solver is always on and takes every C-RNTI DCI 1_1 (Technique D is not reached). The search, the candidate confirmation and the refinement probe only readings with num_rb <= SOLVER_SEARCH_MAX_RB (48). An RNTI whose grants all read > 48 RB gets no probe: `why ... wide=N`, never solved.

## Fix [IMPLEMENTED]
One rule, no flag, no per-cell constant (`nr_pdsch_solver.c`): per RNTI count grants with / without a reading <= 48 RB. After 16 grants (SOLVER_WIDE_AFTER) with no narrow reading ever seen, the search, confirmation and refinement accept any width, under the unchanged probe budget (token bucket, fair share, backoff, 24 per grant). One narrow reading seen once restores the 48 RB bound for good (narrow behaviour unchanged, N=1 equivalent). The probe rate and budget are untouched, so extra cost is bounded by the existing bucket (0.5 probe/slot).

Alternative, fallback to Technique D for RNTIs the solver cannot plan: rejected. It needs a second, unmaintained decode path with its own per-(RNTI,TDA) state alongside the solver's, a routing handoff, and its own cost; the solver already has the cell prior, scrambling search, CSI-RS rate-match learning and HARQ consistency. The 48 RB bound is a CPU cost guard on the probe bucket, not a GPU limit (`nr_pdsch_gpu_fep.cu` has no 48 RB guard; the only 48 there is a comment).

## Unit tests [MEASURED, DGX unit test] `test_nr_pdsch_solver`: 29 pass
- WideOnlyRntiIsSolvedAfterTheWaitAndNarrowIsUnchanged: no probe in the first 15 wide grants; 106 PRB solved after 17 grants, 273 PRB after 16 (cell prior); narrow control 16 PRB and 48 PRB both 2 grants (equal).
- OneNarrowReadingKeepsTheOldBound: with a narrow reading present only that reading is ever probed.

## rfsim 4-RX, 106 PRB, 420 s, one arm each [MEASURED, DGX rfsim]
| | before (e9c874c308) | after |
|---|---|---|
| `why rnti=0x1234` | solved=0 grants=28000 plan=27999 wide=27999 | solved=1 grants=28000 probes=626 p_ok=1 |
| solved at (signal time from first grant) | never | 1354 ms (16 wide grants wait + 2 passes) |
| pdsch_decoded / CRC ok | 0 / 0 | 28528 / 24159 (84.69 %) |
| fulls ok (solver stats) | 0 | 23995 / 27711 |
| n_contexts, postconv, ttc tda0/tda2 | 0, n/a | 0, n/a |
| drop_full | 0.0061 % | 0.0008 % |
| gate verdict | FAIL | FAIL |

Gate not passed. The gate criteria (n_contexts >= 2, postconv CRC, ttc tda0/tda2) are scored from `Technique D CONVERGED` / SWEEP context lines, which the always-on solver path no longer emits (0 contexts); they cannot pass on any build since 0fb22ecbd0 regardless of this fix [INFERRED from score_rx output]. The CRC floor 93.4 also fails (84.69 %): not investigated further in this task; solved-path CRC on rfsim wide grants is lower than the retired Technique D number. Proposed: re-base the gate on solver metrics (`PDSCH_SOLVER per-rnti solve=`, full ok/tr) - owner decision, the gate script was not edited.

## Not done
- 273 PRB rfsim bed, extra probe CPU/GPU time measurement, Vodafone `raw_pci415_12s_b` replay (solved RNTI counts and CRC before/after): stopped because the orchestrator took the heavy lock for a live capture (release file blind_capture_done). Narrow-grant behaviour is covered by the unit equivalence test only.

## OTA check to run
After the fix, in the OTA log: `grep "wide-only for 16 grants"` lists RNTIs that fell to the wide rule; for those `PDSCH_SOLVER why rnti=... solved=1` should appear, and RNTIs that were solved before must have identical `solve=` times (narrow path untouched). Compare per-RNTI solved counts and `full ok/tr` against a run on e9c874c308 of the same cell.
