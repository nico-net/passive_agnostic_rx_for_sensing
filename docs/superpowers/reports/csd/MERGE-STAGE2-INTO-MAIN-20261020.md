# Merge csd/int16-stage2 (828822adf8) into adaptive-rx-UL-DL (d21811d53a) -- 2026-10-20

Merge base fcf6c6fc2e. Main brought 4 commits (571bd335d4 blind DM-RS identity solver + solver-driven CORESET discovery, e17548a389 sa-bed arms, 18b1257d53, d21811d53a). Evidence: [CODE-READ] plus per-file syntax checks; build/ctest results at the end of this file.

## Conflicts and resolutions
| file | resolution |
|---|---|
| nr_dmrs_nid_solve.c / .h, tests/nr_dmrs_nid_solve_test.cc (add/add) | stage2 version. It is a strict superset: `nr_dmrs_nid_solve()` of main is `nr_dmrs_nid_solve_ex(..., fast=0)` with identical body (diff checked), plus the fast mode and the live-offer helpers the GPU NID worker uses. Main's public API (solve, predict_bits, cinit) is unchanged. |
| CMakeLists.txt | stage2 lines: nr_dmrs_nid_solve.c is compiled into `nr_pdcch_nid_worker` (links `m`), not into `nr_pdcch_blind_monitor` (would duplicate the symbols). |
| nr_pdcch_blind_monitor.c | main's `map_candidates_impl(..., pass0_n_out)` and `nr_pdcch_map_candidates_pass0()` kept; the staging is baked to two passes (stage2 rule, ISAC_MAP_PASS0_ONLY gone): `*pass0_n_out = n_pass0`. Main's new env read ISAC_PDCCH_EARLY_COMMIT (default off) is baked OFF: `s_early_commit_enabled = 0`, tests still enable it through `nr_pdcch_blind_monitor_autodiscover_early_commit_enable()`. |
| nr_pdsch_passive_queue.c | both: main's search-ticket split (`nr_pdcch_bwp_crc_search_result` for jobs inside the TD search) with stage2's `!recov_hold` guard. |
| nr_pdcch_blind_monitor_rt.c (9 hunks) | Main's hunks were all inside the passive-BWP tracker block (`nr_pbwp_enabled()`, former ISAC_BWP_TRACK, default off), which stage2 deleted as flag-gated dead code. Stage2 side taken for: pbwp snapshot/probe selection, CORESET discovery observe + the solver submit (`nid_trig`), pass-2 monitoring, probe accept, BWP SWITCH bookkeeping, DCI 1_1 layout `want` filter, ISAC_DCI11_OBSERVE_MM, ISAC_SI_RV_ANY. One main change kept: the discovery gate `|| nr_pdcch_blind_monitor_autodiscover_reseed_pending()` (with stage2's constant duty 20). |
| tests/nr_dl_adaptive_test.cc | main's modified exhaustive-walk test dropped (the walk is opt-in via the removed ISAC_MAP_PASS0_ONLY); main's early-commit-off static kept together with stage2's UlqPriority tests. |

## Env rule check
Env names read in the merged tree minus env names read at stage2 tip: only `ISAC_GNB_CORESET_IL` (main's gNB-side hook in nr_radio_config.c, simulator/gNB, not a receiver read; stage2 leaves gNB names alone). ISAC_NID_SOLVE (main, runtime kill switch default on) is baked on in `nid_solve_enabled()`.

## Limit (read this)
Main's rt.c solver wiring (worker thread, vote table, `nid_proposal`, `nid_submit`) is present and compiled, but its only caller sat inside the passive-BWP tracker block that stage2 removes (default off in every OTA config). So in this merge the main-branch solver is NOT started in the receiver; the solver library (nr_dmrs_nid_solve, tests test_nr_dmrs_nid_solve) is intact and used by stage2's GPU NID worker. The sa-bed BWP-switch arms depend on the tracker and need it re-wired before they pass; nr_passive_bwp.c/h and its tests are merged unchanged from main. No OTA or sa-bed run was made on the merge.
