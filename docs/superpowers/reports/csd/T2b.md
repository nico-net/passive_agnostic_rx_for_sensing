# T2b — exact footprint history beyond the IQ ring

Host: DGX aarch64, sandbox without a CUDA device. Branch: `csd/t2b`.

## Implementation and integration

[CODE-READ] `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_fp.c:93` retains Q12 contrast sums, squared sums, and counts indexed by **absolute slot modulo 640**, plus all-slot totals. Every legal period divides 640, so summing the appropriate residues gives exactly T2's on/off sufficient statistics for every period/offset. The stored coordinates include every symbol, subcarrier within RB, RB, and the original whole-slot pooled comb sample. RB parity is preserved through the RB coordinate. There is no forgetting, time-window truncation, changed normalization, or lowered confidence threshold.

[CODE-READ] `nr_csirs_disc_fp.c:272` implements the fixed-signature `nr_csirs_disc_detect()`. It folds only writes after its saved `ring->written`, splits at physical wrap, and uses `meta.abs_slot` for the lattice and `meta.valid` for inclusion. Work is linear in new slots, independent of elapsed lock duration. Normalization batches are capped at 100 slots. Calls without new writes do not add evidence. `nr_csirs_disc_detect_reset()` at line 263 frees all accumulated statistics. Geometry, masks (including in-place RE-mask edits), and a rewound write counter also invalidate history; changing only `min_strength` reuses it.

The first call after reset bootstraps the currently available ring snapshot. **On a cell/epoch change U5 must also clear the old-epoch raw ring**, or the available snapshot still contains old-cell IQ. With the normal first call after 100 writes, every valid pushed slot since lock is included. If more than `capacity` entries are written between detector calls, the overwritten IQ is irretrievable: the call returns `-1` before adding evidence, and the caller must reset to restart from the available snapshot. No claim is made to recover dropped or never-observed samples. U1's read lock and the existing single worker must serialize detect/reset; the detector does not inspect U1 private state.

[CODE-READ] `nr_csirs_disc_fp.c:107` reduces only the 168 pooled comb statistics while screening a lattice. Detailed RB moments are reduced only for symbols passing the unchanged T2 comb gate. This avoids scanning all RB statistics for every period on every batch. The original stateless CPU and GPU footprint APIs still use the same shared decision code and preserve their semantics.

[CODE-READ] `nr_csirs_disc_gpu.cu:88` adds GPU normalization of just the new contiguous batch. The same integer kernels serve the stateful path, with explicit copies supporting ordinary and managed U1 memory. CPU accumulation and clustering operate on the compact returned contrast batch; a GPU failure falls back to exact CPU normalization. The CPU fallback retains every slot and antenna (no subsampling). The additive `gpu_ops.normalize` tail field in `nr_csirs_disc.h:159` requires rebuilding core and module together; existing fields and signatures are unchanged.

`nr_csirs_disc.h:116` documents the integration default: **U5 passes `cfg.min_strength = 1.03f` unless `ISAC_CSIRS_DISC_MIN_STRENGTH` overrides it**. The existing NULL/zero default of 2.0 remains unchanged for compatibility. U5 files were not modified. These routines allocate/work only when invoked by the opt-in discovery pipeline; no new receiver hook or default-on action was added.

## Memory and work budget

[ANALYTICAL] Let `D = 168*(N_RB+1)`. Persistent sums/squares plus totals require `16*641*D` bytes, plus a small mask/count/metadata allocation: **358.22 MiB at 217 RB**, **453.52 MiB at 275 RB**. This does not grow with slots, antennas, or lock duration. CPU normalization scratch is at most 13.97 MiB at 217 RB; decision scratch is about 1.2 MiB. CUDA adds at most 55.63 MiB of copied four-RX IQ plus 13.97 MiB of contrast scratch for a 100-slot batch. Together with the existing 356 MiB, 640-slot four-RX IQ ring, the stateful path stays below 0.8 GiB at 217 RB, comfortably below 1% of the GB10 host's 121 GB.

[ANALYTICAL] At 217 RB / four RX / 2000 slots/s, normalization visits 291.6 million antenna-RE samples/s. A sample is normalized once, then updates a single modulo-640 bin and totals (36,624 contrast coordinates per slot), rather than all 13 period lattices. The pooled decision pass visits at most `13*640*168` bin coordinates per batch before candidate-specific RB work. GPU normalization removes the expensive IQ power/reference work from the worker CPU; host accumulation is a sequential integer update. This is a bounded work design, **not a measured real-time guarantee**. Loaded-host throughput, GPU allocation/copy overhead, worker backlog and the <5 s lock-to-confirmation target remain deferred. Exact integer squares cannot overflow before roughly 4.35 years at 2000 slots/s; the implementation rejects the next update at that arithmetic bound. Normal epoch handling must still cover the shorter lifetime of the public 32-bit absolute-slot counter.

## Offline evidence

[OFFLINE VERIFIED, DGX aarch64, synthetic only] `tests/nr_csirs_disc_fp_test.cc:268` pushes through the production U1 API into a 640-slot ring and detects every 100 new slots. At 5 dB, four RX, full-band PDSCH plus DM-RS/SSB, row-1/density-3 TRS at symbols 4+8, RB 0–51 of 217, and pilot/data power 1.2, the exact combined footprint is first recovered at:

| Period | Slots | Observation time |
|---:|---:|---:|
| 5 | 600 | 300 ms |
| 10 | 800 | 400 ms |

These satisfy the requested <=2 s synthetic observation gate. They are first exact **100-slot batch** detections, not continuous-time minima or confirmation times. These incremental fixtures regenerate noise/channel realizations per batch (`seed = 0x839412 + 7919*fed`, first absolute slot 1003); they are not the same random realization as T2's single long synthetic window. Existing T2 windows remain in the suite.

[OFFLINE VERIFIED, DGX aarch64, synthetic only] `nr_csirs_disc_fp_test.cc:280` uses row 2, density one, RB 3–11 of 16, four RX, 5 dB, 1.6x pilots, full-band PDSCH and DM-RS, offset P−1. Every legal period recovers exactly. Longer periods necessarily need more observations under the retained conservative gate:

| Period | First exact batch, slots | Observation time, ms |
|---:|---:|---:|
| 4 | 200 | 100 |
| 5 | 200 | 100 |
| 8 | 300 | 150 |
| 10 | 300 | 150 |
| 16 | 500 | 250 |
| 20 | 500 | 250 |
| 32 | 600 | 300 |
| 40 | 900 | 450 |
| 64 | 1700 | 850 |
| 80 | 1800 | 900 |
| 160 | 4300 | 2150 |
| 320 | 9600 | 4800 |
| 640 | 21400 | 10700 |

The P=160/320/640 results explicitly require evidence beyond the 640-slot raw ring. This change removes the history ceiling, not the weak-signal sample requirement. U4 sequence confirmation and live scheduling/channel conditions can change total confirmation latency.

[OFFLINE VERIFIED, DGX aarch64] The initial focused stateful run passed weak TRS, all periods, all three 20,000-slot negative controls, and reset/cursor tests (4 passed, 1 GPU skip). Each negative control is checked after every 100 writes, for 200 decisions over 20,000 slots; these are synthetic 32-RB, four-RX, 5 dB cells. A new `dmrs_only` synthetic-generator flag supplies the pure DM-RS control, with noise elsewhere. Log: `/tmp/csd-t2b-stateful.log`.

[OFFLINE VERIFIED, DGX aarch64] Final CPU `ninja tests nr-uesoftmodem` and CUDA `ninja csirs_disc_gpu test_nr_csirs_disc_fp` completed. Logs: `/tmp/csd-t2b-build-final.log`, `/tmp/csd-t2b-gpu-build-final.log`. The CUDA targets compile for sm_121. Final GPU availability check explicitly skipped both GPU tests (`/tmp/csd-t2b-gpu-skip.log`); this is **[IMPLEMENTED, NOT VALIDATED on GPU]**, not a GPU runtime pass.

[OFFLINE VERIFIED, DGX aarch64] **Final footprint suite: 42 passed, 2 GPU-only tests skipped (44 total)**. All original T2 fixtures still pass, including exact stateful/stateless output and strength checks added to the recovery helper. All six stateful tests completed: five passed and GPU equality explicitly skipped. Final extracted footprint log: `/tmp/csd-t2b-final-fp.log`; authoritative CTest detail: `cmake_targets/ran_build/build/Testing/Temporary/LastTest.log`. The final run reproduced every detection horizon in the tables above.

[OFFLINE VERIFIED, DGX aarch64 sandbox] **One full CPU CTest run, excluding `^nr_td_sim_test$`: 148/157 passed**, exit 8 for exactly the nine documented baseline exceptions: `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `time_management_tests`, `dft_test`, `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. No other test failed. Full log: `/tmp/csd-t2b-ctest-all.log`. CPU/CUDA final builds have no compiler warnings. No production/test code was changed after this final regression.

The full-history parity test at `nr_csirs_disc_fp_test.cc:323` compares each 100-slot incremental decision against the original stateless detector on the corresponding full prefix, through 1,300 slots, including invalid/missing entries and multiple ring wraps. Reset on an already wrapped ring is checked against its current stateless snapshot, and changed normalization masks invalidate the old history. The existing T2 recovery helper additionally checks exact stateful output fields and float strength against the stateless result for all its recovery fixtures.

## Commands and deferred measurements

Build/test commands used the shared measurement lock and niceness 19, after checking the orchestration BUSY marker and absence of `nr-uesoftmodem`. No receiver, replay, OTA, or GPU timing run was started.

```bash
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DIR=/tmp/rr-csd-b-ccache \
  ninja -C cmake_targets/ran_build/build -j8 tests nr-uesoftmodem
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DIR=/tmp/rr-csd-b-ccache \
  ninja -C cmake_targets/ran_build/build_gpu -j8 csirs_disc_gpu test_nr_csirs_disc_fp
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 \
  ctest --test-dir cmake_targets/ran_build/build -j4 -E '^nr_td_sim_test$' --output-on-failure
```

**DEFERRED MEASUREMENTS — GPU runtime**, on the DGX with a CUDA device and the measurement gates available:

```bash
cd /home/nicola/NICOLA/wt/rr-csd-b
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_BACKEND=gpu \
  cmake_targets/ran_build/build_gpu/test_nr_csirs_disc_fp \
  --gtest_filter='CsirsDisc.GpuMatchesCpu:CsirsDisc.StatefulGpuMatchesCpu'
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_BACKEND=gpu \
  ctest --test-dir cmake_targets/ran_build/build_gpu -j4 -R '^test_nr_csirs_disc_fp$' --output-on-failure
```

The stateful GPU test checks every normalized integer against CPU and every footprint/strength after each 100-slot batch over 12,000 slots, including wrap, P=160, ordinary host rings and managed rings. Direct invocation of the normalization callback must succeed, so a missing CUDA path cannot silently count as GPU equality. Without a module/device the test explicitly uses GTEST_SKIP.

**DEFERRED MEASUREMENTS — integration/performance:** orchestrator-owned raw-capture replay, OTA all-confirmations <5 s, four-RX/217-RB sustained 2000 slots/s detector budget, worker overruns, RFSTALL and scan-queue drops. Run performance measurements under the exclusive measurement lock after the U5 merge; the functional commands above do not constitute throughput measurements. `nr_td_sim_test` is intentionally excluded from this task's one full CPU regression, as requested.
