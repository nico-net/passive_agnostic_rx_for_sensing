# Task S5 report: per-unit cost accounting
Files: nr_sense_unit.{h,c}, nr_sense_sched.h (spec constants, new), tests/nr_sense_unit_test.cc (13 tests + DISABLED_Cost), QRing.UnitCfrFromWriter in nr_sense_api_test.cc, CMakeLists (nr_sense_api lib + test_nr_sense_unit), tools/mpci static audit (GLOBS + owner rule + one hand-added row `unit_tab`, PER_CELL). Sites: nr_pdsch_passive_queue.c, nr_pdsch_data_aided.c (da_run), nr_pdcch_blind_monitor_rt.c, nr_pusch_passive_decode.c, nr_srs_passive.c, nr_sense_api.c (writer).
Evidence label: DGX aarch64, synthetic unit tests and micro-benchmark; no replay, no OTA (a receiver may not be started in this task).

## Design
- Per cell (NR_CELL_STATIC `unit_tab`): 64 RNTI rows x 4 sources x 320 buckets of 50 ms, two 64-bit words per bucket: A = tag24|cpu_ns40, B = tag24|wait_max_us20|msgs10|wb_msgs10. ~1.3 MB BSS per cell, 21 MB at NR_CELL_MAX 16 (untouched pages are never faulted).
- Thread safety: every field is an atomic. Bucket words are updated by CAS loops (add when the tag matches, replace when older, drop when a late thread carries an older tag): many writers on one unit lose nothing (test ConcurrentWritersLoseNothing: 4 threads x 20000 jobs, sums exact). Row lookup scans 64 acquire-loaded keys. Row creation (new RNTI, rare) takes a per-cell atomic_flag try-lock; if busy that one sample is dropped, nobody waits. Victim = free row, else oldest `row_last`; key is cleared before the history is reset and published after. A writer that found a row just before its eviction may add one sample to the successor RNTI (bounded, memory-safe). Snapshot reads racily, each word atomically. Writers of cell c are threads bound to c (nr_cell_idx()); snapshot takes the cell explicitly.
- All four writer calls are no-ops outside SENSING (one `nr_isac_mode()` compare inside, plus `nr_sense_unit_cpu_ns()` returns 0 so sites skip their clock reads).
- Sites: PDSCH queue full decode (not layout probes) -> PDSCH_DATA/job.rnti, est=0 (cost only; wait = decode start - enqueue); daBg worker (re-encode + data-aided recon + export, i.e. the ~7 ms recon) -> PDSCH_DATA/j->rnti, est = grant PRBs (this is the one counted message of the grant: rate, wb_rate); DM-RS tap -> PDSCH_DMRS/out.rnti (extra chest + extraction as one message, ring hand-off as a cost-only second call); PUSCH DM-RS extraction + hand-off -> PUSCH_DMRS/g->rnti; SRS worker -> SRS/rnti 0 (reuses the existing thread-CPU pair, no new clock read); writer -> nr_sense_unit_cfr per formatted RS record with quality (valid_cfr_prbs after recon, snr_db_p50).
- Protected baseline (scan, discovery, solver, SSB/CSI-RS): nothing to add; S6 reads `nr_pool_cell_hold_ns` / `hold_ms` per class and `nr_sense_cost_add` counters that already exist.
- `late` = enqueue-to-end > NR_UNIT_LATE_NS = 20 ms (the DECSLOW threshold of the PDSCH queue). Decision of mine; the brief only says "existing late notion".

## Tests (observed red first: 12 of 13 failed against the stubs, then green)
Unit tests: BucketsAccumulate, P99OverWindow, NearestRankSingleOutlierIsNotP99, IdleBuckets..., LruEvictsIdle, ViolationAndSkipCounted, SnapshotPerCell, WaitRateAndWideband, WaitOutlierBecomesP99WhenFrequent, CfrQuality, OnlyInSensingMode (legacy and traffic leave the table untouched), ConcurrentWritersLoseNothing, ConcurrentCreateOfDistinctRntis; 10x repeat stable. QRing.UnitCfrFromWriter: legacy/traffic writer leaves the table empty, sensing produces the unit.
Full build (-k 0, 24 .so) + ctest -j4: 404 tests; 16 failures, all on the baseline list (uci_* x7 Not Run, ra0_qam64/256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, dft_test, ldpc_cuda_pool_cpu, modulation). After the last edit the sense/isac/data_aided/passive/static_audit subset (24 tests) passes. static_audit --check rc 0. Frozen check exit 0.
Mutation checks (restored after): (1) mode guard removed -> OnlyInSensingMode fails; (2) nearest rank ceil -> floor -> P99OverWindow fails; (3) LRU victim oldest -> newest -> LruEvictsIdle fails.

## Measured hot-path cost (DGX aarch64, 2e6 calls, single thread, hot cache)
- `nr_sense_unit_cpu_ns()` in sensing: 185 ns (CLOCK_THREAD_CPUTIME_ID is a syscall on aarch64); legacy/traffic: 1.8 ns.
- `nr_sense_unit_job`: 12 ns (row hit), 18 ns (64th row); legacy 2 ns. `nr_sense_unit_cfr`: 7.5 ns.
- Added per attributed job: PDSCH decode / daBg / PUSCH / SRS = 1 job call + 2 thread-CPU reads = ~390 ns (SRS: 0 extra CPU reads, ~35 ns); PDSCH_DMRS tap = 3 reads + 2 jobs = ~590 ns. Against 175 us..7 ms per job that is 0.01-0.3 %. Snapshot of 64 units: 126 us (scheduler thread only).
- NOT done: the with/without replay of raw_pci415_4s_1102_0951 (needs a running receiver, forbidden here); micro-benchmark only. Run it in S8.

## Deviations and concerns
- Brief test P99OverWindow ("319 buckets at 1 ms + 1 at 50 ms -> p99 50 ms") contradicts nearest rank on 320 buckets (p99 = 317th smallest = 1 ms). I kept nearest rank (as the brief demands) and assert it both ways: 316x1 ms + 4x50 ms -> p99 50 ms, p95 1 ms; 319+1 -> p99 1 ms. A single burst bucket does not move p99; if S6 wants burst protection it should also use a max.
- Idle buckets count as 0 in cpu percentiles; wait_p99 is the p99 over the per-bucket maximum waits of busy buckets (an approximation, not per-job).
- rate_hz is messages / 16 s fixed window (understates a unit younger than 16 s).
- A unit's message = est_usable_prbs > 0 call; est_usable = allocated PRBs (quality shortcut).
- Not attributed: writer-side quality compute (~1 ms/record, per frame, no RNTI at that point; it is in the REC cost per source) and drop-path violations / gate-site skips (S7 owns them; I added the APIs and `nr_sense_unit_set_enabled`, default enabled, which feeds `view.enabled`). SRS cost is per slot of the blind search, so it is unit (SRS, rnti 0); records with an attributed RNTI create SRS units that carry quality but no cost.
- mpci_statics.tsv: `static_audit.py --write` reflows ~1000 unrelated line numbers, so the single row was added by hand (line number 39 may drift).
- Layout probes are excluded from the PDSCH decode attribution.
