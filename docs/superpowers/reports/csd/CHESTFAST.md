# CHEST-FAST — PDSCH channel-estimation investigation (2026-10-09)

## Verdict

**The requested <250 us full PDSCH job and no-SNR-loss gate was not met.** No fast estimator is enabled or retained. The opt-in `ISAC_CHEST_STAGE_TIMING=1` profiler was added to `nr_dl_channel_estimation.c`; the reference channel estimate, residual and per-antenna noise paths remain unchanged. The FFTW delay-peak experiment was removed after it failed the recorded-input and replay gates. [IMPLEMENTED, NOT VALIDATED AS A SPEEDUP]

The supplied T26c measurements put FEP at 300–500 us and LDPC at 250–300 us per job. Their sum alone exceeds 250 us before chest, equalisation or queue overhead, so channel-estimation changes alone cannot reach the requested full-job target. [MEASURED in T26c; ANALYTICAL lower bound]

## Profile and experiment

`perf` sampling was unavailable (`/proc/sys/kernel/perf_event_paranoid=4`). The opt-in profiler uses `CLOCK_THREAD_CPUTIME_ID` around pilot generation, the original delay transform, and the entire estimator call. With `ISAC_PDSCH_INLINE_POOL=1`, the four antenna tasks execute on the consumer thread, so these are CPU times for one `nr_pdsch_channel_estimation()` call. It reports every 16 calls per consumer. [CODE-READ]

| Recorded replay | Estimator CPU | Pilot | Delay IDFT | Other | EVMGRANT CRC-OK / total |
|---|---:|---:|---:|---:|---:|
| Reference (`replay_chestfast_profbase_130046`) | 162.3 us | 3.2 us | 139.7 us | 19.6 us | 96 / 129 |
| Rejected FFTW (`replay_chestfast_fast_131530`) | 186.9 us | 3.2 us | 162.6 us | 21.2 us | 101 / 133 |

These four timing columns are **medians of the logged cumulative per-consumer means**, not per-job p50 values. The reference delay path occupied about 86% of the measured estimator call. The fast path was slower in this sample. The available replay reached common grants (5 PRB, RNTI `0xffff`) and 48 PRB SI grants (`0x007f`), not the requested typical dedicated 16 PRB grant. [MEASURED, DGX sandbox replay `raw_pci415_12s_b`, 2026-10-09]

The candidate used a one-time FFTW float plan for the 4096-point delay peak, then the existing fixed-point FIR and noise calculation. It reproduced the reference's pre-IDFT integer scaling and truncation, but the fixed-point IDFT's stage rounding still chose a different delay peak often: one consumer reported 297 different used delays in 368 checked inputs. Check mode always published the reference delay. On 123 matching EVMGRANT keys between the reference and fast replays, **one CRC outcome differed**, 48 SNRs differed by more than 0.1 dB, and the worst paired SNR change was **−2.2 dB**. The separate totals above must not be compared as a CRC rate because the runs admitted different grants. This violates the no-SNR-loss requirement; the candidate was deleted. [MEASURED, DGX sandbox replay `replay_chestfast_check2_131127` and runs above]

The build included `libpdcch_nid_gpu.so`, but the sandbox reported `GPU_PLATFORM ... cc=0.0 ... model=unavailable`, followed by `PDCCH_NID_GPU init failed; capture disabled`. Dedicated CORESET discovery therefore produced no 16 PRB PDSCH jobs. The mandatory 16 PRB per-RE −50 dB equivalence test, full-job p50/p99, and dedicated-grant CRC/SNR comparison **remain unmeasured**. The existing `PDTIM` report only emits every 200 jobs; these runs had fewer decoded jobs, and no full-job quantile series was available. No OTA run was performed. [MEASURED, DGX sandbox replay; NOT VALIDATED]

## Build and replay

The receiver, `params_libconfig`, `raw_iq`, `pdcch_nid_gpu`, `csirs_disc_gpu` and `ldpc_cuda` built with `heavy.sh ninja -j6`. CMake used the task's requested options plus `-DENABLE_LDPC_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=121` and a writable `/tmp` CPM cache because the sandbox cannot write `/home/nicola/.cache/cpm`. The build used a copied `/tmp` ccache. The final build after removing FFTW passed. [OFFLINE VERIFIED, DGX sandbox]

Exact offline replay environment and command: [`chestfast_replay.sh`](../../../../tests/passive_rx/dgx/chestfast_replay.sh) with [`ue.passive.ota.chestfast.replay.cfg`](../../../../tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg). Run `bash tests/passive_rx/dgx/chestfast_replay.sh <name>`. It uses the T26d flags with `ISAC_CHEST_PDP_DIAG=0`, `ISAC_CHEST_DELAY_SHARE=0`, `ISAC_MLOCK=0`, `CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b`, `CAP_SAMPLES=1474560000`, `CAP_TICK=208029025`, speed 0.25, and no radio. The safe replay core map is scan 10–11, PDSCH 8,9,12,13,14. No process was pinned to cores 15–19. The T26d OTA map and its delay-sharing flag were not used. [OFFLINE VERIFIED replay setup]

## Next gate

On a host where GPU discovery works, run the script on the 12 s capture and first confirm dedicated 16 PRB jobs are present. Any new delay implementation must compare the full channel estimates and noise outputs against the reference on recorded inputs at every RE (relative error ≤ −50 dB), then match dedicated-grant CRC outcomes and EVMGRANT SNR before a timing claim. A full-job <250 us goal also requires FEP and LDPC work outside this estimator. [PLANNED]
