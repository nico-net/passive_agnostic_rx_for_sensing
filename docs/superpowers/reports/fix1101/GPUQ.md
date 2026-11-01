# fix1101/gpuq: DECODE token released during the GPU pass wait

## Root cause [OFFLINE VERIFIED, code reading]
The GPU pass completion wait (`wait_done`, nr_gpu_pass_graph.cu) is predicted sleep + spin + blocking `cudaEventSynchronize`; it never calls
`nr_gpu_poll_relax`, the only path to `nr_pool_site_gpu_wait`. Additionally `nr_pool_gpu_wait` gave only inside an explicit give scope that nobody opened.
Hence CORE_POOL gpu_gives=0 and the DECODE token held across ~430 us of GPU span.

## Change
- nr_core_pool.{c,h}: first-pass DECODE leases (DL and UL) give without a scope (`nr_pool_gpu_give`, `nr_pool_site_gpu_give`), only when a waiter could be granted the
  freed token (caps, scan reserve, per-cell rules unchanged). `nr_pool_gpu_wait_end` re-acquires the same class/cell/deadline with RESUME priority (picked before fresh
  waiters of its cell and class; caps still apply). New stats: gpu_gives (now non-zero), gpu_resume wait hist/max, printed in the CORE_POOL line as `gpu_resume[wait_us p50 p99 max]`.
- nr_gpu_pass_graph.cu `wait_done`: give before the predicted sleep / unseen-class spin / blocking fallback, `nr_gpu_poll_end()` before the tail spin. Other poll loops (fep, ldpc, demod,
  budget rt wait) call `nr_gpu_poll_end()` at every exit. GPUPASS line gets `inflight[1= 2= 3= 4+=]` (passes in flight at launch).
- tools/mpci/mpci_statics.tsv: new statics classified.
- No flags/env. Per-thread state only.

## Tests [OFFLINE VERIFIED, DGX aarch64 unit]
test_nr_core_pool (updated give-scope test + new resume-priority test), static_audit_check, test_nr_gpu_pass_cpu, gpu_stage_*, test_nr_gpu_pass_discrete: pass.
test_nr_gpu_pass_cells HarqSlabsAreSeparate fails identically on the base (rr-int-1031): pre-existing.
bench_nr_core_pool 0 5 1 5 (synthetic, no GPU wait): dl p99 wait 1086 -> 1046 us, no regression; gpu_gives=0 there by construction.

## NOT done / not verified
- Part B (multi-grant batched GPU launch) NOT implemented: bit-exact batched kernels/graph with N grant nodes is a large change; not safe for tonight. `inflight[]` histogram measures
  how often a merge would apply.
- On-air effect untested (no replay run by me). [HYPOTHESIS] qwait p99 falls since tokens are free during ~430 us GPU spans; cost: resume wait when caps hold.
- Watch: CORE_POOL gpu_gives, gpu_resume p99/max, EVMGRANT qwait/dec, GPUPASS inflight.

## Risks
A parked thread holds a pass instance without a token (as before it held both). Resume wait can add to dec if the pool is saturated; if gpu_resume p99 is large, restrict gives. Touches nr_gpu_pass_graph.cu wait_done and nr_core_pool.c (srsq/other fixes).
