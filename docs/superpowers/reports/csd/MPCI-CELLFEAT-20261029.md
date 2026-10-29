# MPCI-CELLFEAT (2026-10-29): passive PRACH, passive SRS, sense-API session and dashboard view per cell

Branch `mpci/c-cellfeat` (worktree `/home/nicola/NICOLA/wt/rr-mpci-cellfeat`), base `mpci-main` @ `8ed7059411`. Closes the A8 open item "PRACH / SRS passive, ISAC sensing, sense-API session, dashboard: cell 0 only" (MPCI-A8-20261026.md line 121).
Host: DGX GB10 aarch64, shared. Build `cmake_targets/ran_build/build`: `-DOAI_USRP=ON -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DNB_ANTENNAS_RX=4 -DENABLE_TESTS=ON`, RelWithDebInfo, Ninja, `nice -n 19`, `-j8`, `flock -s /tmp/td_measure.lock`, `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps`.
Labels: `[OFFLINE VERIFIED, DGX GB10 aarch64 unit]`, `[REPLAY, DGX GB10 aarch64, file-published IQ, no radio]`, `[CODE-READ]`, `[HYPOTHESIS]`. NO FLAGS: no env var, option or `-D` switch was added; every name is derived.

## STOP result

| STOP item | result |
|---|---|
| tests pass (two-cell interleave for PRACH, SRS, sense records/session; module tests unchanged) | MET `[OFFLINE VERIFIED, DGX GB10 aarch64 unit]`: `test_mpci_cellfeat_sense` (4), `test_mpci_cellfeat_srs` (3), `test_nr_prach_passive` (13 old + 3 new), `test_nr_isac_adapter` (13 old + 1 new), `test_mpci_cellfeat_monitor` (4); `test_nr_sense_api` 14 pass + 1 skip (the skip is `GoldenReaderFixture`, it needs `NR_SENSE_GOLDEN_DIR`, as before), `test_nr_sense_records` 15, `test_nr_srs_blind`, monitor suites `test_multicell` 17, `test_mpci_dash` 30, `test_monitor` 32, `test_health` 16, all unchanged and green |
| `test_static_audit` / `--strict` clean for our rows | MET: `static_audit_check` and `test_static_audit_scanner` pass; `--strict` lists no `CELLFEAT` row (the 114 UNCLASSIFIED left are G1=63 and G4=51, not ours) |
| ctest no new failures vs the known list | MET: 353 tests, 12 not green, all in the known list or an unbuilt helper: `dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam256`, `test_nr_ssb_rate_match_prod`, `test_ldpc_cuda_pool_cpu` (SEGFAULT), `nr_td_sim_test`, and `test_nr_pusch_uci_*` x6 "Not Run" (the helper binary `nr_pusch_ucisim` is not in the `tests` target; built by hand it passes 6/6). `test_nr_pusch_ra0_qam64` passed this time (intermittent). Load-flaky `test_nr_spin_handoff`, `time_management_tests`, `test_iq_chan_soak5`, `test_nr_gpu_pass*` passed in this run |
| `nr-uesoftmodem` links | MET `[OFFLINE VERIFIED, DGX GB10 aarch64 build]`: `ninja -k 0 -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests`, 0 FAILED, no warning in an edited file |
| N=1 log line set unchanged (solo replay diff) | MET `[REPLAY, DGX GB10 aarch64]`, see "N=1 equivalence" below |
| one 2-cell replay with per-cell PRACH / SRS / sense / dashboard activity tagged `[c0]` / `[c1]` | MET for sense and dashboard, PRACH and SRS are quiet on these captures (evidence below) |

## What changed

| area | file | change |
|---|---|---|
| sense-API session | `openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.{c,h}` | whole session state (mutex, cond, rings, fds, writer thread, stats, cost counters, ZMQ handles) is `NR_CELL_STATIC`; every call acts on the session of `nr_cell_idx()`; the writer is created bound to the cell that opened the session (arg = cell, `nr_cell_bind` first, same pattern as `nr_passive_obs`). New pure helpers `nr_sense_cell_file(cell, def, out, n)` (cell 0 unchanged bytes; cell k: `nr_cell_path` name, but an extension-less file name gets `.cell<k>` appended because `nr_cell_path` would turn it into a directory name that `open()` cannot create) and `nr_sense_cell_endpoint` (`tcp://h:p` -> port+k, `ipc://` like a file, `inproc://` + `.cell<k>`, else -1 and the cell exports to files only, logged) |
| adapter | `openair1/PHY/NR_UE_ISAC/nr_isac.{cc,h}`, `nr_isac_stub.c` | `[sensing]` parsed once (PROCESS); per-cell `CellSession` (active sources, RX antennas, derived export names, `nr_sense_cfg_t`). `nr_isac_start/stop/set_rx_antennas` and every submit act on the calling cell; `nr_isac_stop` of cell 0 also drops the parsed configuration (cell 0 stops last). New `nr_isac_source_configured(source)` (stub: 0) tells the prealloc whether PRACH is configured |
| passive PRACH | `nr_prach_passive.{c,h}` | pending list, RAR ring, noise floors, stitch tail, sequence cache, job queue, producer flag, counters, three warn-once flags: per cell. Big buffers (`prach_xu` 215 KB, `prach_freq`, `prev_tail` 64 KB, `jobs` ~70 MB at 4 RX) are `NR_CELL_STATIC_HEAP`, pre-allocated by `nr_prach_passive_cell_prealloc(cell)` (only when the PRACH source is configured). Worker created bound to its cell, one HOUSE float lease per job |
| passive SRS | `nr_srs_passive.{c,h}` | the `sr` state (~110 KB: job windows, event ring, searcher, hint, tracks) is one `NR_CELL_STATIC_HEAP` object, pre-allocated for every admitted cell by `nr_srs_passive_cell_prealloc(cell)` (also when SRS is off: PUSCH / PDCCH producers read `sr.active` on every call); the lock is a separate per-cell static. Worker bound to its cell, one HOUSE float lease per job |
| receiver main | `executables/nr-uesoftmodem.c` | N > 1: the cell's prealloc hook (bound, before the RX fence) allocates the PRACH / SRS heap objects and then opens the cell's session (`nr_isac_start`), PRACH worker and SRS searcher; the start hook emits the session record; the shutdown runs `nr_prach_passive_flush / nr_srs_passive_stop / nr_isac_stop` per cell through `mpci_for_each_cell` (highest first, cell 0 last). `nr_isac_set_rx_antennas` is called bound to the cell it describes. N = 1: unchanged place and order (after the radio start, session -> PRACH -> SRS); only the heap pre-allocation of cell 0 moves before the memory lock |
| dashboard | `tests/passive_rx/monitor/monitor.py` | `discover()` keeps cell 0 on its configured names (the per-cell copies `*.cell<k>.*` are excluded from the cell-0 glob, which picks the largest match) and lists `records_cells` / `cfr_cells`; a `CellReader` feeds each into the state of cell k (`RunState._child(k)`), so the existing Sensing export page of that cell shows its own records and CFR frames |
| audit | `tools/mpci/static_audit.py`, `mpci_statics.tsv` | files added to the scan (PRACH, SRS, sense API/records, adapter, stub, `nr_dashboard.c`), owner `CELLFEAT`; the scanner now finds `NR_CELL_STATIC*` objects glued in front of a function header (it lost every one of them after a `typedef struct {...};` before this fix; 7 already-converted G3 rows of `nr_gpu_pass_ul.c` surfaced and are marked PER_CELL "converted (G3)") |

## Sense records: how consumers tell the cells apart

`nr_sense_records` has no versioned optional-field mechanism: `"schema":2` is fixed and `rs` / `session` / `grant` records have a closed key list (checked in `nr_sense_rs_to_json`, `nr_sense_emit_session`, `nr_sense_grant_to_json`). Per the brief, no cell key was added. The cells are separated by **per-cell sessions and files**:

- cell 0: the configured `records_path`, `cfr_path`, `endpoint` byte for byte; cell k > 0: `records.cell<k>.jsonl`, `cfr.cell<k>.bin` (`nr_cell_path` rule: the extension stays last; no extension -> `.cell<k>` appended), endpoint port + k / `.cell<k>`.
- every `rs` and `session` record already carries `"pci"` (and `session` the cell's `carrier_dl_hz`, `n_rb_dl`, `fs_hz`), so a record is self-describing even when files are merged. In the replay below `records.jsonl` has `pci` 415 on 43140 of 43142 records and `records.cell1.jsonl` has 403 on 10373 of 10375 (the others are the first session record each cell emits at start, before its carrier is known, `pci` null/0, as in N=1).
- frame ids (`cfr_frame_id`) and `CFR` headers are per session, so they restart at 1 in each file.

## Audit table (rows of owner CELLFEAT; `static_audit.py --strict` clean)

| object(s) | file | class | reason |
|---|---|---|---|
| `mu mu_once cv active closed_seen pause_writer fail_writes write_chunk writer_limit written_items s_after_close opened stopping no_sink frames frame_lengths frame_cap/re/ant/stride/head/count records record_lengths record_cap/head/count rs_jobs record_binary cfr_fd record_fd worker next_id stats cost_ns cost_count zmq_context zmq_pub` | `nr_sense_api.c` | PER_CELL | one export session per cell. `after_close` was renamed `s_after_close` first (a `nr_sense_stats_t` member has the same name: shadowing trap) |
| `nr_sense_open::warned_no_zmq` | `nr_sense_api.c` | PROCESS | log once per process |
| `receiver_id receiver_array` | `nr_sense_records.c` | PROCESS | session identity from `[sensing]`, same strings for every cell |
| `nr_sense_rs_to_json::names` | `nr_sense_records.c` | CONST | name table |
| `pending pending_head rar_events rar_head prach_xu prach_freq prev_tail prev_tail_slot cached_root/count/short floors jobs job_mu job_cv job_thread worker_started worker_stop producer_busy job_read job_write prach_us prach_occasions prach_drops prach_skipped observed_gain_db`, `process_slot::warned_fdd warned warned_boundary` | `nr_prach_passive.c` | PER_CELL | one PRACH pipeline per cell (the SIB1 RACH config, RAR ring and gain are the cell's); `prach_xu prach_freq prev_tail jobs` are HEAP |
| `test_*` (10) | `nr_prach_passive.c` | PROCESS | hooks of `NR_PASSIVE_PRACH_TEST` builds, never in the receiver |
| `srs_lock sr_state` (macro `sr`) | `nr_srs_passive.c` | PER_CELL | one SRS searcher per cell; `sr_state` is HEAP |
| `configured_sources configured initialized reconf cfr_antennas subslot_* base_* rx_id rx_array` | `nr_isac.cc` | PROCESS | `[sensing]` parsed once by `nr_isac_init` before any cell thread, read-only afterwards, identical for every cell |
| `cell_session` (`CellSession`: `active_sources`, `receiver_antennas`, `cfg`, derived names) | `nr_isac.cc` | PER_CELL | sources run per cell, antennas per RU, export names per cell |
| `max_re legacy_keys source_map`, `get_nr_prach_duration::duration`, `process_srs::offsets` | | CONST | tables |
| `nr_dashboard.c` | | no static object | launcher of one detached python monitor (single instance per port, `--single`); its `dashboard.log` is that process's stdout. PROCESS by design: one dashboard reads every cell's lines and files. `ISAC_METRICS_PATH` is one file with a `"cell"` key since MPCI-DASH, also by design |

### Process-wide by design (and why)
- Dashboard launcher / process (above); `ISAC_METRICS` file (cell key, DASH).
- `[sensing]` configuration and the session identity strings: configuration, not state.
- The core pool / float lane (B5): shared by all cells, cells lease from it.
- `nr_sense_emit_*` formatters, `source_map`, tables: pure.

### Unbound-thread sites (the ones that would have read cell 0)
- `executables/nr-uesoftmodem.c` main loop: `nr_isac_set_rx_antennas(cell_id)` ran on the unbound main thread; now bound to `cell_id` around the call.
- Sense writer, PRACH worker, SRS worker: created through `nr_core_plan_create` which has no cell argument; each worker binds itself from its argument as its first action (same pattern as `nr_passive_obs`, `nr_pdcch_uss_tracker`).
- `nr_srs_passive_hint` (called from `openair2/LAYER2/NR_MAC_UE/nr_passive_rrc_harvest.c`): runs on the MAC / sync actors, bound per cell by A8 `nr_ue_bind_actor` `[CODE-READ]`.
- `nr_prach_passive_gain_changed`, `nr_sense_emit_session` in `nrue_ru_adjust_rx_gain_impl`: called from the cell's UE thread (`nr_ue.c` binds `UE->Mod_id`) `[CODE-READ]`.
- No `nr_sense_*` / `nr_isac_submit_*` caller in a `.cu` file or a GPU callback was found (grep over the tree).

### Heap objects pre-allocated before the RX fence
`prach_xu prach_freq prev_tail jobs` (only when the PRACH source is configured) and `sr_state` (always), per admitted cell, in `mpci_cell_sensing_open` (called from `mpci_cell_prealloc`, bound to the cell). The sense rings (`nr_sense_open`: frame ring, record ring, job ring, ~53 MiB at 4 RX / 106 PRB with the defaults) and the SRS buffers are `malloc`ed by `nr_isac_start` / `nr_srs_passive_start`, which for N > 1 also run in the prealloc hook, so nothing of the three modules is allocated after the fence. For N = 1 `nr_isac_start` and the two starts keep their place after the radio start (byte-identical order); only the heap pre-allocation of cell 0 is done before the memory lock (the static arrays of before were part of the image).

### CPU and GPU
- PRACH worker per job and SRS worker per job take one `NR_POOL_HOUSE` float lease for the bound cell (`nr_pool_site_float_acquire/release`, no-op when the float lane is not merged). The sense writer is **not** leased: it is I/O bound (JSON formatting of a few records plus `writev`), and a pool token held across a `write()` could stall decode on a slow disk `[HYPOTHESIS]`.
- None of the three modules uses the GPU; nothing to wire to the per-cell GPU state (G3).

## Tests added

| test | file | what it proves |
|---|---|---|
| `CellFeatSense.TwoCellsInterleavedOnOneThreadEqualSoloRuns` | `tests/nr_mpci_cellfeat_sense_test.cc` | cell A (CFR frames + records) and cell B open sessions with their own files, one thread re-binds between calls: both file pairs and both stats equal the solo runs byte for byte; closing B leaves A enabled |
| `TwoBoundThreadsEqualSoloRuns` | same | same with two threads bound to their cell |
| `ClosedCellCountsAfterCloseOtherCellKeepsRunning` | same | `after_close`, `enabled`, files per cell |
| `PerCellNamesKeepCellZeroBytes` | same | `nr_sense_cell_file` / `nr_sense_cell_endpoint`: cell 0 unchanged, dir form, no-extension form, ipc / inproc / tcp / overflow |
| `PassivePrachCells.InterleavedOnOneThreadEqualSoloRuns`, `TwoBoundThreadsEqualSoloRuns`, `SkippedOccasionIsCountedOnItsCellOnly` | `tests/nr_prach_passive_test.cc` + bridge | two cells with different preamble delay (32 / 8 samples): the record each cell's worker pushes (the push is attributed by `nr_cell_idx()` of the worker, so it also proves the worker is bound) equals the solo record; a skipped occasion counts on its cell only |
| `CellFeatSrs.*` (3) | `tests/nr_mpci_cellfeat_srs_test.cc` | two cells (512-point 24 PRB 4 RX and 1024-point 51 PRB 2 RX) start, stop and count events / drops independently, also from two bound threads |
| `Adapter.SessionsArePerCellWithOwnFiles` | `NR_UE_ISAC/tests/nr_isac_adapter_test.cc` | cell 0 on `cfr` / `records`, cell 1 on `cfr.cell1` / `records.cell1`, frame counts 3 vs 2, session record in each, stopping cell 1 leaves cell 0 and the parsed configuration, cell 0's stop drops it |
| `test_mpci_cellfeat_monitor` (4) | `tests/passive_rx/monitor/test_mpci_cellfeat.py` | cell 0 keeps its own name even when a cell file is larger; per-cell records feed the state of their cell (counts by source); a single-cell run is unchanged |

## 2-cell replay `[REPLAY, DGX GB10 aarch64, file-published IQ, no radio]`

Command: `B=<build> CFG=<sd_t26ul.cfg with sensing enabled: sources=ssb,csi_rs,pdsch_dmrs,pdsch_data,pusch_dmrs,prach,srs, records_path/cfr_path relative to the run dir> DUR=150 tests/passive_rx/mpci/run_two_cell_replay.sh cellfeat2` (cell 0 = PCI 415, 217 PRB, from `raw_pci415_12s_b`; cell 1 = PCI 403, 51 PRB, from `raw_iliad403_4s_1023_2315`; speed 0.25; SIGINT stop through `timeout --signal=INT`, rc 124 as designed). Run dir `replays/cellfeat2_120118` (git-ignored; logs gzipped). Per-cell evidence, ANSI stripped (the script's own `grep ^\[c0\]` evidence lines print 0 because of the ANSI prefix, a known property of the script):

- sync / MIB / SIB1 / EVMGRANT: c0 2 / 1 / 8 / 19328, c1 5 / 3 / 11 / 11671.
- **sense**: `records.jsonl` (cell 0) 43142 records = 2628 ssb + 6216 csirs + 134 pdsch_dmrs + 2 pdsch_data + 11101 pusch_dmrs + 23057 grant + 4 session, `pci` 415; `records.cell1.jsonl` 10375 = 2634 + 806 + 652 + 3 + 2811 + 3462 + 7 session, `pci` 403; `cfr.bin` 246 MB / `cfr.cell1.bin` 47 MB (CFR frames 20081 / 6906 = the `rs` records of each file). Session records: c0 `carrier_dl_hz` 3680010000, `n_rb_dl` 217; c1 3630000000, 51. `ISAC_METRICS` per cell: `sense_rec_drop` 0, `sense_io_errors` 0, `sense_cfr_no_sink` 0 in both cells.
- **PRACH**: quiet on both cells, with evidence: `[c0] SENSING: passive PRACH inactive until SIB1 RACH config is available` and the same line `[c1]` (the per-cell warn-once; before this change cell 1 would not have printed its own), `prach_us 0 prach_drops 0 prach_skipped 0` in both cells' `ISAC_METRICS`. Detection and RAR matching are covered by the unit tests, not by this replay.
- **SRS**: started on both cells (one `blind SRS sequence hopping v=1 is unsupported` warning each); both stop lines are per cell: `[c0] SENSING: SRS queue dropped=20231`, `[c1] SENSING: SRS queue dropped=4779`; `srs_us 0` in both cells (no job completed). The same `srs_us 0` with a larger drop count (40103 in 120 s) is what the N=1 baseline binary shows on the 415 capture (below), so this is **not** caused by the per-cell change; it is an open observation `[MEASURED, DGX replay, baseline binary]`: passive SRS never completes a job on this capture and drops every capture of its queue.
- **dashboard**: `monitor.Manager` offline over the run dir lists cells 415 (c0) and 403 (c1); the Sensing export state of each cell shows its own records (43142 vs 10375), by-source counts, CFR frame count (20081 / 6906) and its own session record (217 PRB 3680010000 Hz vs 51 PRB 3630000000 Hz). The receiver's own detached dashboard started (`dashboard.log` is its stdout, empty).
- `SHUTDOWN` step `sensing_stopped` printed once; both sessions closed (files complete).

## N=1 equivalence `[REPLAY, DGX GB10 aarch64]`

Solo replay of the 415 capture, same configuration (sensing enabled with PRACH and SRS), same publisher, 120 s, baseline = the `mpci-main` @ `8ed7059411` build of `/home/nicola/NICOLA/wt/rr-mpci-main` (binary mtime 07:17, commit 07:15 `[HYPOTHESIS]`: built from that commit) against this branch. Line templates (digits and JSON payloads normalised, ANSI and timestamps removed):

- startup lines before the first sync / grant: 281 vs 281, **identical** except the two inherent ones (the version line names the branch; the dashboard URL line is split identically by the ESC byte in both).
- the 26 sensing / shutdown / UE-context / CORE_PLAN / MPCI templates: identical sets; the 20 `SHUTDOWN` step lines: identical sequence (without `elapsed_ms`).
- `ISAC_METRICS` key set: identical (188 keys); `records.jsonl` key sets per type: identical, same record types (session, rs ssb / csirs / pdsch_dmrs / pdsch_data / pusch_dmrs, grant). Counts differ by run (replays are not deterministic: 3810 vs 3819 ssb, 12730 vs 13375 csirs, ...).
- The remaining whole-log differences (PDSCH_SOLVER, GPU_DISCOVERY_SLICE, PDCCH_NID_GPU, a SLOWPASS_BT backtrace line with a different thread name) are the nondeterministic decode lines of two replays at different load; they are not in the changed modules. `SLOWPASS_BT` (the existing slow-pass backtrace of the PDCCH consumer, blocked in `pthread_mutex_lock` inside `nr_pdsch_passive_queue_enqueue`) appears in both builds: baseline 0 and 2 lines, this branch 2 and 1 lines in two runs each `[MEASURED, DGX GB10, shared host, n=2 per build]`, so it is run-to-run load, not the change.

## Open items

1. **Host RAM / GPU books**: the PRACH heap (~70 MB per cell when PRACH is configured), the SRS state (~110 KB) and the sense rings (~53 MiB per cell at 4 RX / 106 PRB, scales with `cfr_max_re`, `cfr_antennas`, ring sizes) are not booked per cell in the B3 admission model (`MPCI_ADMIT host_used_mb`). They are small beside the ~13 GB per 217-PRB cell `[MEASURED, DGX, csd/mpci-int O5]`, but a 16-cell plan on a small host would need them.
2. **Passive SRS inert on these captures** (above): baseline and per-cell build both complete no job; not investigated (the drops are `trylock` / grid-mismatch / queue-full counted together in one counter).
3. **PRACH quiet** because `nr_pdcch_sib1_prior_get()->rach_valid` stays false on both cells in this flow; PRACH detection on a real RACH is not exercised by any replay.
4. The HOUSE leases of the PRACH / SRS workers are new float-lane users at N=1 (timing only, no log line): the quiet-host decode-time A/B of gate V2 should include a run with PRACH and SRS enabled. In the two solo A/B pairs here `SLOWPASS_BT` showed up in both builds.
5. The retire / re-activation of a running cell does not tear the sense session down (A8 open item, same as every other module: "run-time teardown").
6. Sense writer is not leased from the core pool (justified above); the lease around only its JSON formatting could be added if the writer ever shows up in CPU accounting.
7. ZMQ endpoint per cell is derived (port + k) and untested against a real subscriber (the unit test checks the name only; `test_nr_sense_api` has the ZMQ mirror test for one cell).

## Interfaces for the consumers

- Sense-API consumers (external sensing apps, dashboards): one records / CFR file pair per cell: cell 0 = the configured `records_path` / `cfr_path`, cell k = `nr_sense_cell_file(k, configured)`: `records.cell<k>.jsonl`, `cfr.cell<k>.bin`; endpoint: port + k (`tcp://`), `.cell<k>` (`ipc://`, `inproc://`). Records are schema 2 unchanged, `"pci"` in every record. The cell index of a file is the `.cell<k>` suffix (regex `\.cell(\d+)(?=\.|$)`, cell 0 has none).
- C API additions: `nr_sense_cell_file`, `nr_sense_cell_endpoint` (`nr_sense_api.h`); `nr_isac_source_configured` (`nr_isac.h`); `nr_prach_passive_cell_prealloc(cell)`, `nr_srs_passive_cell_prealloc(cell)`; all `nr_isac_*`, `nr_sense_*`, `nr_prach_passive_*`, `nr_srs_passive_*` calls act on the calling thread's cell.
- Dashboard: `monitor.py` `discover()` returns `records_cells` / `cfr_cells` (`{k: path}`); `Run` feeds them into `RunState._child(k)`; the per-cell Sensing export page (`snapshot_for(cell)`) shows them. No receiver-side dashboard change: the launcher and `ISAC_METRICS_PATH` stay process-wide.
- Lifecycle contract for the orchestrator: a new module with per-cell sessions is opened in `mpci_cell_prealloc` (bound, before the fence) and closed in the per-cell shutdown loop; cell 0 must stop last.

## Files changed
`openair1/PHY/NR_UE_TRANSPORT/nr_sense_api.{c,h}`, `nr_prach_passive.{c,h}`, `nr_srs_passive.{c,h}`; `openair1/PHY/NR_UE_ISAC/nr_isac.{cc,h}`, `nr_isac_stub.c`; `executables/nr-uesoftmodem.c`; tests `NR_UE_TRANSPORT/tests/nr_mpci_cellfeat_{sense,srs}_test.cc`, `nr_prach_passive_test{.cc,_bridge.c,_bridge.h}`, `NR_UE_ISAC/tests/nr_isac_adapter_test.cc`, `tests/passive_rx/monitor/{monitor.py,test_mpci_cellfeat.py}`; `cmake_targets/mpci_cloud.cmake` (block `CELLFEAT`); `tools/mpci/{static_audit.py,mpci_statics.tsv}`. sens6 frozen gate: `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` clean.
