# Task S1 fix round 1 report

Status: DONE_WITH_CONCERNS. Host: DGX aarch64, build dir cmake_targets/ran_build/build of this worktree (24 .so).

## Fixes
- I1: nr_admit.c counters are g_cls[16][NR_ADMIT_N], row = nr_cell_idx_fn() (weak; new out-of-line wrapper in common/utils/nr_cell.c; absent -> row 0, so test_nr_admit links without the cell module). nr_admit_class_counts and nr_admit_stats read the calling thread's cell; ISAC_METRICS/ADMIT are produced on the cell's thread. Shutdown stats (mpci_stop_cell_queues) run bound to the cell. TSV row now PER_CELL (line numbers updated).
- I2: nr_admit_try counts attempts only when nr_admit_on() (>= 2 cells), same rule for JANON and DMRS_ID. At one cell nothing is counted, nothing printed; test_nr_core_pool_budget AdmissionRunsEverythingWithOneCell restored to its original expectation (empty string).
- I3: mutation checks on test_nr_admit (each reverted afterwards): shed_tiers ignored in pop -> BacklogReplaysWhenIdle + PurgeWhileShedding FAIL; purge dropped -> BacklogExpiresOld + PurgeWhileShedding FAIL; expired counter dropped -> BacklogExpiresOld + DrainFreesAndCounts + PurgeWhileShedding FAIL. Unmutated: 9/9. New tests: PurgeWhileShedding, DrainFreesAndCounts, NoCountsWithOneCell, CountersPerCell (incl. legacy-mode line), and isac_mode SensingExplicitSourcesAllOutsideSetFallBack.
- M1 fixed: explicit sources entirely outside the sensing set fall back to the default set with narrowed=true (never 0).
- M2 fixed: backlog deadline uses nr_admit_now_ns() when enq_ns == 0.
- M3 fixed: class segments of the ADMIT line and the admit_* JSON fields are emitted only when nr_isac_mode() != legacy (weak hook in nr_admit.c; guard in nr_passive_metrics_collect). Legacy output is byte-identical.
- M4 fixed: nr_admit_skipq_drain (frees, counts expired) called at shutdown for JANON (final_stats) and DMRS_ID (new nr_pdsch_passive_queue_drain_skipq, called from mpci_stop_cell_queues before final_stats).
- M5: replay in cdmrs_run now calls nr_admit_done (busy accounting). NOT changed: janon CLOCK_REALTIME 50 ms timed wait (the cv uses the default clock; a wall-clock step only stretches/shortens one poll of a statistic worker, bounded by the 1 s item age).

## Tests
Full ninja -k 0 rc 0; ctest -j4: only baseline failures (qam64/256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, gpu_pass, dft_test, ldpc_cuda_pool_cpu, modulation) plus test_nr_pusch_uci_* 1-7 "Not Run": those executables have no target in this configuration (same in the rr-twomodes reference build), environmental. static_audit_scanner passed. admit/isac_mode/passive_metrics/core_pool: all pass. Frozen check exit 0.

## Concerns
- Worker integrations (janon_worker, cdmrs_run) are compiled, not run; replay is covered at nr_admit level only.
- nr_cell_idx_fn added to nr_cell.c (out-of-line TLS read); nr_admit.c now also has a weak nr_isac_mode reference (int return vs enum, same ABI).
