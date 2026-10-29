# MPCI-AUDIT — static-audit cleanup (2026-10-29)

Branch `mpci/c-audit` off `mpci/int-1029b` 99ac57a902 (mpci-main + A10 + A11 + B6). Plan R14 item 3 / PROJECT_MEMORY §32.3 item 3. Labels per PROJECT_MEMORY §0.1. Host: DGX GB10 aarch64,
shared with another user: builds and unit tests only, no replay, no benchmark, no radio. NO FLAGS: no new env var, CLI option or `-D` switch, no test seam. N=1 behaviour and log output unchanged.

## 1. Audit coverage widened (`tools/mpci/static_audit.py`, `mpci_audit_files.txt`)
- Source set [OFFLINE VERIFIED, DGX build]: `ninja -t inputs` of `nr-uesoftmodem` and of the libraries it loads (RECEIVER_TARGETS: oai_usrpdevif, rfsimulator, params_*, libcoding, libldpc*, libldpc_cuda,
  the GPU modules, libdfts, libnr_gpu_platform, shm_iq*, iqplayer), repository files only: **555 sources**. The old GLOBS covered 155 of them; the other **400** are now listed in
  `tools/mpci/mpci_audit_files.txt` (all of them, also those without state today, so a static added later is seen).
- New modes: `--coverage BUILD_DIR` (exit 1 when a receiver source or a header with state is not scanned) and `--add-coverage BUILD_DIR` (append). `--check` stays offline (cloud ctest).
- **Headers**: a `static` in a header is one copy per including unit and was invisible. `receiver_headers()` finds them; 14 headers with state were added (e.g. `nr_pdcch_cell_len_state.h`, whose A4 rows were
  missing from the table, `nr_pdsch_retry_pred.h`, `nr_dl_gpu_memory.h`, `nr_gpu_green.h`). Not scanned by design: asn1c-generated codecs in the build directory (read-only descriptor tables).
- Table: **2865 -> 4023 rows** (+1158 in 172 files; 0 rows dropped). New rows by class: PROCESS 681, CONST 312, TLS 104, PER_CELL 61. The 54 G1 rows: PROCESS (section 2). `--strict` rc 0, **0 UNCLASSIFIED**.
  `static_audit_check` in ctest is now `--strict` (was `--check`); `test_static_audit.py` 21 tests (3 new: audit file list, header detection, repo strict).
- Classification of the new rows, from what the code does [CODE-READ]:
  - PER_CELL, converted with `NR_CELL_STATIC` / `NR_CELL_REF` (56 rows, 11 files): `nr_pdsch_config_sweep.c` 31 (per-RNTI contexts, cell-wide observations/prior, field book, BC9 certification epoch, census; the file
    was in no glob and not in the A5 report), `nr_pdsch_dmrs_scan.c` 8 (identity pool, votes, base-rate table), `nr_td_order.c` 4 (SIB1 TDRA store, census), `nr_pbch.c` 4 (PBCH ok/fail counters read by the cell's census, diag
    reference), `nr_dci_history.c` 2 (the BC9 history), `nr_pdsch_xoverhead.c` 1, `nr_td_cb0_wire.c` 1 (`g_ctx`), `slot_fep_nr.c` 1 (branch FO table; ISAC_RX_BRANCH_FO stays unset), `nr_pdsch_retry_pred.h` 1 (per-RNTI rescue memory in a
    header), `nr_ue_procedures.c` 1 (`prnti_harq`), `nr_csirs_disc_fp.c` 2 (new allocation record of the history). Already converted by earlier tasks but never in the table: `nr_sfo_state.c`, `nr_pdcch_cell_len_state.h` (5 rows).
  - PROCESS (681): configuration/log/tracer infrastructure, MAC/RRC/NAS/PDCP/RLC/SDAP/F1AP/NGAP tables indexed by UE instance (Mod_id == cell index) or unused by the passive receiver, LTE and simulation libraries, radio
    devices, GPU modules that are one-per-process (allocation fences, green contexts, census counters), CB0 backend selection / host CPU-share token bucket / grant-work buffer pool, one-shot log budgets and read-once policy caches.
  - CONST (312) and TLS (104): read-only tables and existing thread-locals (auto rules).
  - Not converted on purpose, open for B6: `g_sched` of `nr_td_cb0_wire.c` is a host CPU-share token bucket shared by the cells; a cell that bursts can spend the tokens of the other (one bucket per host is the design of that
    budget, its split per cell belongs to the admission/pool work).

## 2. The 54 G1 rows: LDPC pool and CB0 contexts (`ldpc_cb0.cu` 29, `ldpc_decoder.cu` 25)
Decision: **process-wide pool keyed by job, not per cell**; no code change needed, verified [CODE-READ ldpc_cb0.cu, ldpc_decoder.cu, nrLDPC_coding_cuda_decoder.c]:
- CB0 (`ldpc_cb0.cu`): one device context (stream, work buffers, `MAX_SLOTS` slots). `ldpc_cb0_submit` takes a free slot under `g_state_mu`, stages the input, metadata, results and bits in that slot, enqueues under
  `g_submit_mu` on the one stream and returns a ticket naming the slot; `collect` releases it. A submission owns its slot from submit to collect, so jobs of several cells never alias; nothing selects by cell.
- TB pool (`ldpc_decoder.cu`, `nrLDPC_coding_cuda_decoder.c`): one worker decodes the code blocks of every queued TB in one launch sequence (dynamic batching, cells share launches by design); a TB owns a contiguous slot range
  of the pinned pool for the life of its request.
- Breaker, sticky error, capture window, fault-injection hooks: one CUDA context => device-wide fault => the breaker opens for all cells (same decision as the CSI-RS GPU breaker, G4). Counters: aggregate telemetry.
- Base-graph and (BG, Z) tables, graph cache keyed by launch shape: cell independent. Two scanner artifacts (`std::map` template argument, namespace alias) classified as such.
- All 54: PROCESS with a per-group note naming the key (slot/ticket/request). The cell-carrying parts are the callers' (`cb0_job_t.cell`, section 3).

## 3. Pool tasks and workers bound to their cell
Worker entry points checked: every per-cell queue/worker thread already bound itself (`nr_cell_bind` / `nr_cell_thread_create` in the worker entry points of `NR_UE_TRANSPORT`). The ones that ran other cells' work as cell 0 (or as their starter's cell):
- **`pushTpool` / `pushTpool_mask`** (`common/utils/threadPool`): the shared UE `Tpool` workers (chest, PDSCH demod, PDCCH scan tasks, initial sync, slot rx/tx, LDPC segment decode, CUDA LDPC prep) ran every task with TLS cell 0.
  `task_t` now carries `cell`, stamped from `nr_cell_idx()` at push; the worker binds to it before `func`. The stop sentinel keeps cell 0. Inline execution (no workers) is unchanged. `thread-pool` links LOG (nr_cell.c).
- **CB0 batch pool** (`nr_td_cb0_batch.c`): `cb0_job_t.cell` = the caller's cell; `cb0_pool_main` binds to it for the job.
- **GPU-check queue** (`nr_gpu_bgq.h`, used by `nr_pdsch_gpu_demod_host.c` `g_bgq` and by the per-cell `g_cdmrs_q`): section 4.
- Left as designed: CHAN class lease of the wideband channelizer (cell 0, process-wide, B6); GPU callbacks/CUDA event threads (carry `cell` in the job, never use TLS; documented in nr_cell.h).

## 4. GPU-check queue: each check carries its cell
`g_bgq` (gpuCheckBg: the bit-exact GPU demod/chest self-checks) is one worker for all cells and took the cell of its first caller (`q->cell`), so the core-pool lease and the TLS of every check were that cell's.
`nr_bgq_push` now records `nr_cell_idx()` of the pusher in the item; `nr_bgq_run_one` binds the worker to the item's cell (`nr_cell_bind`) and calls `lease_in(item cell)`. `q->cell` stays as the starter (informational).

## 5. Lazy per-cell allocations after `nr_cell_alloc_fence` / mlockall
Moved before the fence:
- **CSI-RS footprint history** (`nr_csirs_disc_fp.c`): was allocated at the first `detect()` of the cell (CPU/accumulate path), after the fence. `nr_csirs_disc_fp_prealloc(n_rb)` (called from `nr_csirs_disc_start`, i.e. from
  `nr_csirs_disc_cell_prealloc`, bound to the cell, before the fence) allocates the arrays (host, or unified when the GPU module accumulates); `nr_csirs_disc_detect_reset` keeps them zeroed (a reset on the worker never
  allocates); `nr_csirs_disc_fp_release` (stop / failure path) frees them. Nothing is allocated when the GPU module detects on the resident IQ (`ops->detect`, the production GPU path never touches the host history).
- **DCI 1_1 / 0_1 layout resolvers** (`nr_pdcch_blind_monitor_rt.c`): 2 x ~4 MB `calloc` per cell at the first arm, after the fence. Now allocated by `nr_pdcch_cell_prealloc(cell)`; zeroed (`n_hyp` 0) reads like the NULL of before at every use (checked at every use).
  Cost: 8 MB host per cell from the start instead of from the first arm.
Listed, not moved (no allocation that the fence forbids, or behaviour change at N=1):
- All 37 `NR_CELL_STATIC_HEAP` objects (3 of them in the CSI-RS confirm header, first touched by the prepare) have a `NR_CELL_HEAP_PREALLOC` in a per-module prealloc (checked one by one); a first access after the fence is fatal by construction.
- Worker threads started lazily by `pthread_once` per cell at the first use, after the fence: PDCCH `janon` (queue calloc 256 items + thread), `disc`, `nid`, PDSCH `dmrsScan` worker and `dmrsIdBg` queue, UL `nid_bg` / `bgdisc`, `ul_layout` eval thread,
  `uss_tracker`. They work for cell 0 at N=1 (also lazily); the A11 failure of the recovery worker was a thread + arrays, its step never proven (A11 report 5.). Starting them early would move their "worker started" log lines and
  their idle threads at N=1, so they are listed for the 2-cell OTA instead: grep "worker started" per `[cK]` and any `FAILED to start`.
- Per-RNTI sweep contexts (`malloc` ~180 KB per new context, recycled through `g_spare_state`) and per-thread scratch (`lane_batch_vec_ready`, TLS buffers): allocated at run time by design, on the cell's own thread.
- GPU: elastic CSI-RS ring growth (`nr_csirs_disc_ring_resize`) allocates unified memory after the fence by design and fails closed (logged, keeps the size).

## 6. Tests [OFFLINE VERIFIED, DGX GB10 aarch64, build `cmake_targets/ran_build/build` of the worktree]
- New `test_mpci_audit_interleave` 5/5: two cells alternating on one thread for `nr_td_order` (SIB1 store + census), `nr_pdsch_xoverhead`, `nr_dci_history`, `nr_pdsch_dmrs_scan` (same RNTI, different identities, 6 vs 2 voted slots)
  and `nr_pdsch_config_sweep` (same RNTI/config/row: k0 certification of cell A 0x1, of cell B 0x2, cell 0 and the other cell untouched by a reset).
- `test_nr_pdsch_retry_pred` 3/3 (+ two cells interleaved); `test_nr_pdsch_retry_gate` 4/4 (+ queue item runs as its cell, lease gets the item's cell); `test_thread-pool` (+ tasks pushed from cells 3/0/7 run as those cells);
  `test_nr_csirs_disc_cell` (+ T4: footprint history arrays per cell, kept by reset, freed by release; CPU backend); sweep 199, dmrs_scan 14, td_order 16, xoverhead 5, dci_history 15 (+1 pre-existing skip).
- Build: `nr-uesoftmodem`, `oai_usrpdevif`, `rfsimulator`, `params_libconfig`, `nr-softmodem`, `tests` link, 0 failed steps. ctest: see section 7.

## 7. ctest and gates
`ctest -j4` under `flock -s /tmp/td_measure.lock`, `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps` [OFFLINE VERIFIED, DGX GB10 aarch64]: 373 tests, 358 run. Failures = exactly the known set: `dft_test`, `test_nr_modulation`,
`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `nr_td_sim_test`, `test_nr_ssb_rate_match_prod`, `test_ldpc_cuda_pool_cpu` (SEGFAULT), `test_nr_pusch_uci_*` x6 (Not Run: helper not in the tests target), plus `test_nr_gpu_pass` and
`test_nr_gpu_pass_geo` (load-flaky at -j4; re-run serially under the same lock: 3/3 pass, `test_nr_gpu_pass` 60 s). New or touched tests pass: `static_audit_check` (now `--strict`), `test_static_audit_scanner`, `test_mpci_audit_interleave`,
`test_nr_pdsch_retry_pred`, `test_nr_pdsch_retry_gate`, `test_thread-pool`, `test_nr_csirs_disc_cell`, `test_mpci_a3/a4/a5/a6_interleave`, `test_mpci_cellfeat_*`.
- `static_audit.py --write` then `--check` rc 0, `--strict` rc 0, 0 UNCLASSIFIED; `--coverage cmake_targets/ran_build/build`: 570 files covered, 0 not scanned.
- sens6 frozen gate: `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` clean.
- N=1: every conversion maps to the slot of cell 0 (`NR_CELL_REF` = `name_cells[nr_cell_idx()]`, 0 when unbound), the Tpool/CB0/check-queue binding is `nr_cell_bind(0)` for cell-0 pushers, and no log string changed
  [CODE-READ]; no replay or rfsim was run (shared host), so the N=1 equivalence suite (A9) and the 4-RX gate stay for the validation phase.

## 8. What the 2-cell OTA / replay must check
- `grep -E "\[c[0-9]\].*(worker started|FAILED to start)"`: lazy workers of section 5 for every cell.
- CSI-RS: `CSIRS_DISC` lines of cell 1 unchanged; no `footprint history preallocation failed`.
- DCI 1_1 / 0_1: `DCI11_LAYOUT armed` / `DCI01_LAYOUT armed` per cell, no `cannot allocate`.
- `GPU_CHECK_ASYNC done=` lines now carry the tag of the cell whose check ran last.
