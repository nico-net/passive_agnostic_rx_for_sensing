# Unified-memory GPU headroom from available memory (2026-10-25)

Branch fix/unified-gpu-free (base fix/dgx-isa-twin 5ae2b9937b). Host: DGX GB10 aarch64, shared.

## Bug
[MEASURED, DGX GB10] cudaMemGetInfo free = MemFree (8.7 GB) while MemAvailable = 113 GB (page cache). caps.free_memory fed nr_gpu_membudget (warm set split_cap, csirs ring, nID window, elastic), so test_nr_gpu_pass failed (capped=4918) with a large page cache.

## Change
- nr_gpu_platform.h: `nr_gpu_headroom_calc(model, free_raw, total, host_avail)` (pure): SHARED -> min(host_avail, total); DISCRETE/NONE or unknown host (SIZE_MAX) -> free_raw. New field `caps.free_raw`.
- nr_gpu_platform.c: after the tier is final (so a probe-failed or forced DISCRETE keeps cudaMemGetInfo free), `caps.free_memory = nr_gpu_headroom_calc(..., nr_host_membudget_live_avail())`. host_avail is the host module's min(MemAvailable, cgroup headroom), swap never counted; no new /proc parsing, no flags, no constants. nr_gpu_platform_free_at_probe() returns the headroom. GPU_PLATFORM line prints `free_raw=` and `free=` once.
- Tests (nr_gpu_platform_test.cc): fixture meminfo/cgroup trees (unified: available not MemFree; cap by total; cgroup 16 GiB limit - 4 GiB used wins over MemAvailable; unknown host; discrete/cpu keep cuda free even with large host avail) + mocked SHARED/DISCRETE probes.

## Double accounting (how it works now)
One pool, the host's available memory A. Host budget B_host = 0.80*A - 1.5 GB - GPU commitments (nr_host_membudget_set_unified callback = nr_gpu_membudget_committed_total, unchanged). GPU budget B_gpu = 0.85*headroom - GPU reserve, headroom = min(A, total). The GPU modules commit against B_gpu; the host deducts those commitments from its own budget, so the sum host + GPU stays within 0.80*A - reserve and the same bytes are not granted twice. Residual (pre-existing, unchanged): the GPU budget is fixed at probe and does not deduct host commits made before GPU commits; host headroom is live so later GPU commits shrink it. [CODE-READ]

## sens6 / x86 discrete unchanged
[CODE-READ + unit] `nr_gpu_headroom_calc` returns free_raw for any model != SHARED; the only new code path in probe is guarded by `caps.model==NR_GPU_SHARED`; test GpuHeadroom.DiscreteAndCpuTiersKeepCudaFreeWhateverTheHost and Platform.DiscreteProbeKeepsCudaFree. Frozen sens6 gate: clean.

## Results [OFFLINE VERIFIED, DGX GB10 aarch64 unit]
- test_nr_gpu_platform 34/34, test_nr_gpu_membudget_mem 15/15, test_nr_host_membudget 16/16.
- `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps ctest -R gpu -j1`: 44/44 passed (2 skipped by design: *_cpu), incl. test_nr_gpu_pass (67 tests). 
- MemFree/MemAvailable (kB): before 5629004 / 110870876; after 11387124 / 108828160 (page cache not dropped, was already small MemFree ~5.6-11 GB, < 16 GB threshold throughout). test_nr_gpu_pass rerun at MemFree 9099788 kB passed. Log: `total=130660904960 free_raw=7701323776 free=106761125888 model=shared`.
