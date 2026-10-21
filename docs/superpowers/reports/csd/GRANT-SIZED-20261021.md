# Grant-sized GPU work, 2026-10-21 (branch csd/int16-grantsize)

Labels: [CODE-READ] read in the tree, [Bench, DGX] synthetic-grant unit bench on the DGX GB10, idle of receivers, `flock /tmp/td_measure.lock`, ABAB arms, not on air; [INFERRED] reasoning. Final timing is judged only OTA. Base: csd/int16-pooldisc. Commits: 696a23b691, 7f56f5b7df.

## 1. Inventory: what each stage processes vs what the grant needs [CODE-READ]

DL pass (nr_gpu_pass_graph.cu enqueue(); sub-graphs C / D(+IF) / L(+IF)).

| stage | extent before | needs | status |
|---|---|---|---|
| k_fep_exact (C) | grid (14 symbols x nbRx), every grant, symbol list baked at capture (job sym_mask 0x3fff) | PDSCH window + the grant's DM-RS symbols (+ demod symbols); all nbRx antennas | **was full slot; now per-grant mask** |
| k_ls / k_delay_exact / k_fir (chest) | rows = ndmrs x nl x nbRx x nseg; row nb_rb, k0 staged per row in device memory; delay = one 4096 IDFT per row | same | already sized (rows, RBs) |
| chest_post, k_gate_d | rows of the grant; n = 12 nrb | same | already sized |
| demod: k_gather, k_mask_pw, k_tail, EVM chain | grid (ext/128, 14) from the class; every kernel reads `nre[m]` / `sym_mask` from the device job block (h2d each grant) and returns for blocks past it | the grant's REs | already sized by data; the over-sized class grid only costs idle blocks (launch + early return, ~us) |
| k_unscr, k_llr_stats, k_norm_* | G (grant) | | sized |
| k_rdm | grid = ncb class, one block per code block, padding blocks write zeros | nd blocks | cheap (zero fill) |
| LDPC k_ldpc4_t | grid = ncb class x cluster size; **padding blocks (class - C) ran the full decoder** (zero input) | nd blocks | **was padded; padding now stops at entry** |
| CRC (cand = ncb x 8), k_pick | cand K = 0 for padding, pick loops nd | | sized |
| DISCRETE only (sm_89 RTX 4060 Ti): per-pass H2D of the whole IQ window (nbRx x (slot + N) samples), pil (213 KB), geo, scr (class max Qm); D2H of est and rxF (nbRx x 14 x N x 4 B each) and llr (class) | baked memcpy node sizes | the grant's symbols / RBs / G | **not changed** (§5) |

Shared work across grants of one slot [CODE-READ]: the GPU pass has no cross-grant sharing. Each decode thread owns its rxdataF and each grant's pass redoes its FEP into it (the CPU slot share / t_fep_cache and the chest cache are CPU-path only; the pass ignores a cache hit). So sizing adds no duplication: grants of one slot transform overlapping symbol sets into separate buffers, exactly as before, minus the symbols a grant does not read.

UL PUSCH pass [CODE-READ]: FEP items are per symbol (src == null skipped, only the grant's window), chest rows per DM-RS x antenna, k_ul_eq / k_ul_llr read `nre[m]` from the device block. Already sized; its only padded work was the LDPC padding blocks (same kernel, same prepare_tb), which the change covers.

## 2. What was cut

1. **FEP symbol set**. `k_fep_exact` keeps its captured 14 x nbRx grid; each block reads word 21 of the per-slot staging block (`par[21]`, rewritten before every launch like origin / rotation) and returns at once when its symbol is not in the grant's set. Set = [start_symbol, start+number_symbols) | dmrs_full | demod sym_mask | the job's dmrs_sym, computed in nr_gpu_pass_dl.c and carried in `job.copy_mask` (the existing "symbols the caller needs" field; the DISCRETE copy-back uses it too) -> `nr_gpu_fep_job_t.run_mask`. This is the set the CPU body transforms without slot share. No re-instantiation, no flag.
2. **LDPC padding blocks**. `nr_gpu_ldpc_job_t.pad` -> DJob K = 0 -> the kernel (both families, every cluster size) writes nsnap 0 / syn0 1 and returns before any cluster barrier. Real blocks are independent (own llr / res / c2v / unsat / snap regions), and k_pick, the CRC kernels and the host read only the first nd blocks, so output is unchanged. Applies to DL and UL.
3. **Held-instance retry** (Task 4 re-entry at D). C is skipped only if the retry's symbol set is inside what the instance's last C transformed (`Inst::fep_run`); otherwise C re-runs. `copy_mask` left c_sig for this reason.
4. **CPU slot-share cache** (nr_pdsch_passive_decode.c): after a grant-sized pass the `t_fep_cache` is not marked valid (`out->fep_mask != 0x3fff`), so a later CPU-path grant of the slot never reuses a partly transformed rxdataF.

Post-decode consumers of the thread's rxdataF [CODE-READ, nr_pdsch_passive_queue.c ~1820-1925]: all read the first DM-RS symbol (`ctz(dlDmrsSymbPos)`), which is in the set; `nr_dmrs_ref_db` reads the last DM-RS symbol, in the set; ZP score reads the grant window.

## 3. Equality evidence [unit, DGX GB10, shared-memory model]

`GpuPassSized.GrantSizedEqualsFullSlot` (new): 9 grants (24 / 48 / 96 / 217 PRB; nl 1, 2, 3, 4; Qm 2..8; windows of 6..13 symbols; 3 cases with 1-3 padding blocks, ncb 2-8). For each, the same job runs with `nr_gpu_pass_test_sized(0)` (full-slot FEP, running padding blocks = previous behaviour) and sized, with the unneeded rxdataF symbols pre-filled with a poison pattern. Compared: need_cpu, tb_ok, nvar, per-block seg_ok, LLR statistics, **final LLRs bit for bit**, TB bytes, rxdataF of every needed symbol on all 4 antennas bit for bit, and that every unneeded symbol is still poison (proves it was skipped and, with equal LLRs, never read). Also equal to the stage path (CPU-equivalent reference). Result: pass, 9/9 decoded.
`GpuPassSized.HeldRetryFollowsTheSymbolSet` (new): first body enters at C, a retry with a subset set enters at D, a retry needing one more symbol re-runs C and that symbol equals the full-slot FEP.
Regression: full `test_nr_gpu_pass` (59 tests, includes ManyCodeBlocksEqualStagePath with C up to 131 padded to the class, the UL suites, the gate, held retry, DISCRETE-neutral fixtures) and `ctest -R gpu` (18) all pass. Mixed slot: each grant of a slot is an independent pass into its own buffer, so "several grants one slot" reduces to the sequence above; the poison check is what proves no cross-grant dependence. Not tested: DISCRETE model (no discrete GPU here; `NR_GPU_PASS_DISCRETE` forces the discrete code path on the GB10 but none of the new tests were run in it).

## 4. Bench [Bench, DGX], synthetic grants, GPU span = first graph node -> completion, us, p50 / p99

Solo (one pass in flight, ABAB x2, n = 300 per arm):

| grant | full-slot | grant-sized |
|---|---|---|
| 24 PRB nl1, 12 sym | 246 / 278, 245 / 260 | 245 / 340, 242 / 252 |
| 24 PRB nl1, 6 sym | 234 / 256, 233 / 269 | 233 / 250, 234 / 262 |
| 24 PRB nl3 qm6 | 414 / 490, 414 / 444 | 413 / 442, 414 / 444 |
| 96 PRB nl3 qm4 | 605 / 665, 607 / 633 | 606 / 646, 606 / 636 |
| 217 PRB nl4 qm8 | 1292 / 1554, 1294 / 1588 | 1289 / 1326, 1292 / 1335 |

Four decode threads at once (DL pool, n = 1200 per arm, pooled):

| grant | full-slot | grant-sized |
|---|---|---|
| 24 PRB nl1, 12 sym | 337 / 807, 336 / 783 | 328 / 603, 322 / 790 |
| 24 PRB nl1, 6 sym | 309 / 615, 319 / 837 | 298 / 481, 288 / 473 |
| 24 PRB nl3 qm6 | 636 / 1200, 633 / 1148 | 627 / 1172, 626 / 1128 |
| 217 PRB nl4 qm8 | 2962 / 4261, 2806 / 4331 | 2927 / 4251, 2669 / 4166 |

Reading: **solo p50 is unchanged** (within +-1 us). The 14 x 4 = 56 FEP blocks fit in one wave of the 32 DL SMs, so the kernel takes one block's latency whatever the symbol count [INFERRED from the solo result and the grid size]; cutting blocks cannot shorten it. Under 4-way sharing p50 falls 3-4 % (12 symbols), 6-10 % (6 symbols), ~1-4 % (large), and short-window p99 falls 20-45 % in 3 of 4 arms (the other arm shows noise-level change; arms differ by ~+-10 % run to run, so treat single arms as noisy). **These benchmark grants do not show the padding-block cost**: with zero input the padding blocks converge at the first syndrome check on the bench (as GPU-SPAN-ROOTCAUSE §2.3 already notes), so the LDPC part of the saving is only visible on air, where that report measured padding blocks reaching the iteration limit in 55 % (ncb 2-8) and 87 % (ncb 9-16) of failing grants [MEASURED, earlier OTA]. Why they do not converge on air is still unproven; the change removes the dependence on the reason.

Expected size on air [INFERRED]: small. FEP: typical PDSCH windows are 11-13 symbols, i.e. 1-3 of 14 symbols and ~1-3 % of GPU span under sharing; the 55 % "own work" is latency-bound kernels that do not shrink with grant size. LDPC: up to the measured padding-block share of kspan for ncb >= 2 grants (169 vs 89 us conv-only, 291 vs 298 us limit-bound at ncb 2-8; swait 292 us at ncb 9-16) -- the part that matters for the big and multi-layer grants.

## 5. Not done, with reasons (list, not changed)

- **IF-D / IF-L conditional-node launch gap (~74 us, p50 47 + 21 [MEASURED]).** Candidate: replace the IF nodes by a `skip` check at the head of each body kernel (all ~15 D and ~8 L kernels return when `rb->skip`), keeping the gate kernel. A skipped grant would then pay a short chain of early-return launches instead of the IF launch. It touches every body kernel in 4 files, changes the skip-path timing of every no-chance grant, and cannot be judged on the bench (the IF launch latency is a driver cost under real concurrency). Needs its own A/B.
- **C -> D -> L graph boundaries and 27 node-to-node gaps (~58-75 us).** Merging C+D+L into one graph removes the boundaries but loses the held-instance re-entry at D and the split-mode memory cap; design decision, not a cheap cut.
- **EVM branch** (stream 2, ~12 kernels, joined before the D2H): measured parallel to the tail but joined before the LLR copy; making the join lazy would change when evm_db is ready for the host. Not touched.
- **DISCRETE (sm_89, 4060 Ti) PCIe copies**: IQ window H2D of the whole slot, est / rxF D2H of 14 symbols x nbRx, pilots / scr / geo at class size. The memcpy node sizes are baked per class; sizing them per grant needs `cudaGraphExecMemcpyNodeSetParams` per launch (an exec update, not a re-instantiation) or per-symbol-range memcpy nodes. Cannot be measured on this host; for the discrete GPU this is probably a bigger item than the FEP block count.
- **Pruned FFT** (output bins only for the grant's RBs): the last radix-4 levels could skip butterflies whose outputs nobody reads, bit-exactly (same arithmetic for the needed outputs), but it cuts arithmetic, not the 5 shared-memory phases that set the block latency; estimated gain 10-15 us of ~69 [INFERRED], not tried.
- **Padding block count at the graph grid** (grid still = class): blocks exit at once now; a smaller grid needs per-class graphs.

## 6. Risks

- ABI: `nr_gpu_ldpc_job_t` gained a field and `nr_gpu_fep_job_t.run_mask` / `nr_gpu_pass_dl_out_t.fep_mask` were added. Any test or tool binary built before this change and run against the new libpdsch_gpu.so fails (7 ldpc ctests failed on stale binaries until rebuilt). Rebuild all targets.
- A new reader of the thread's rxdataF outside the grant's window / DM-RS symbols would see stale data after a GPU pass. Inventory above found none; the unit poison check guards the pass itself, not CPU code added later.
- After a grant-sized pass, a CPU retry body with slot share redoes its FEP (cache not valid). Only reached when the retry goes to the old path (ISAC_GPU_PASS_RETRY_OLD default off, RECOVERY_ASYNC worker, special bodies). Extra FEP cost only.
- Retry bodies that widen the symbol set re-run C (previously skipped): only retries that need an extra symbol, none of the current arms except a start-symbol hypothesis earlier than the first body's.
- Padding blocks now report ts / res as instantaneous converged blocks: the `li=` padding diagnostics (npad_last, dpad) will read 0 / ~0 after this change, so they cannot be compared with the 2026-10-19 runs.

## 7. OTA spec (orchestrator runs; DGX; frozen FROZEN_20261018.env passed explicitly)

Build (DGX, aarch64, build dir already configured), via heavy.sh with the MPS env:
`ninja nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4` (rebuild every target: ABI change). Known ARM ctest failures unchanged (dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256/qam64).

Arms: A = csd/int16-pooldisc, B = csd/int16-grantsize (this branch, rfsim regression `tests/passive_rx/dgx/rfsim_regress.sh` 4-RX gate first). No flags, one config. Interleaved A B A B, TIM 8 min per arm + Vodafone 15 min per arm (2 + 2 per cell), cells never pooled.

Pass criteria per cell, B vs A: DL p50 (e2e) ~1 ms or lower and not above A's by more than run noise; DL p99 < 5 ms; CRC_q not lower than A; n_real (grants decoded, including TBS above 16 CB and nl >= 2, report them split by size) not lower than A; queue drops <= 4 %. Expected (not required): gspan p50 within noise of A or slightly lower (-1..-5 %), the benefit mostly in the `li=` LDPC span of grants with ncb >= 2 (kspan p50 and p90 by ncb class). Compare `li=` npad fields only within B (they read 0 now). If p50 is unchanged and only the large-grant LDPC kspan drops, that is the expected outcome; this change does not address the 74 us IF launches or the p99 launch stalls.
