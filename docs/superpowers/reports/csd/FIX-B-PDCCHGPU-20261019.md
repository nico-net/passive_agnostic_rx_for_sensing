# FIX-B-PDCCHGPU-20261019 -- per-slot batched GPU PDCCH blind decode: NOT IMPLEMENTED, analysis only

Branch csd/int16-pdcchgpu (from freeze-20261018). No code changed. [READ FROM CODE] unless labelled.
Reason: OTA window running (nr-uesoftmodem alive, builds/benches forbidden), and the evidence below says the premise
(GPU polar per slot beats CPU) was already measured false twice on this code base.

## 1. Prior measurements that contradict the premise (not re-measured here)
- SCAN2-20261014.md s5 [MEASURED, DGX]: SC polar on GPU, batch of 1/8/32/128 items = 319/261/284/326 us total
  (kernel floor 230-285 us: serial SC tree, one warp per item) vs CPU 1.8 us per AL2 decode. GPU wins only at >= 500 items/call.
  Live: 50-100 decodes per pass (~0.2 ms CPU) vs 0.65-1.2 ms per GPU round trip, plus PDSCH contention.
- csd/int16-ppdcch 9354a94692 [MEASURED]: "stage A bit-exact (3.3M items) but mode 1 slower than CPU on tim; not merged".
- nr_pdcch_gpu_fep.h STATUS: GPU LLR sign parity vs CPU int16 chest FAILS (branch gate, fixed-point compensation, autoscale
  not ported). Bit-identical GPU chest/LLR is itself an unsolved sub-project.
So a one-submission GPU pass (FEP+chest+demap+polar+CRC) cannot reach <= 0.15 ms: kernel floor for polar alone is ~0.25 ms.

## 2. Where the 480 us goes (code map) and what could actually cut it
- arr->occ (~60 us): scan queue wait (nr_pdcch_passive_queue.c); p99 +240-390 us. Fix is scheduling, not GPU.
- pass (200-225 us): fep_llr is partly GPU already; decode ~130 us mean/hyp earlier measured; 49 % of scan-thread time is the
  anonymous DL length sweep (SCAN2 s6), which is NOT needed for known-RNTI USS grants (fast path exists).
- accept->enqueue (~110 us), nr_pdcch_blind_monitor_rt.c ~9720-10130: after CRC accept the thread runs, per grant:
  clock_gettime + a synchronous LOG_I rnti_seen line, nr_pdcch_coreset_map_accept_probe (DM-RS correlate on the CORESET),
  bc9_dci_accept, psd_note_ok, discovery observe, and for DCI 1_1 the PDSCH solver block (layout cache, reading hash,
  SOLVEDLAYOUT/DIAG formatting, getenv in static-init blocks). Suspects by size: coreset_map_accept_probe, the LOG_I
  string format (LOG_ASYNC exists), and the solver block. Needs a perf/BTIM split to confirm; not measured.
  Cheapest wins: defer log/probe/bc9/psd/discovery to after the enqueue (enqueue first, bookkeeping after),
  because none of them feed the PDSCH job. That alone removes most of the ~110 us from arr->enqueue with no GPU work.

## 3. Recommendation
1. Do accept->enqueue reordering (above) -- CPU only, low risk, target -80..-100 us. ~1 day incl. unit test + A/B on air-like replay by the owner.
2. Attack scan-queue wait / p99 (pass pinning, drop_full) separately.
3. Only if a GPU PDCCH lane is still wanted: first port the CPU int16 chest/LLR bit-exactly (sign parity gate), measure
   the polar kernel with a persistent kernel (no graph launch ~20 us, no per-call sync) against 1.8 us/decode CPU; expect a loss below ~500 items.
   Interference proof item (own highest-priority stream in RT green ctx, no sharing with csd/int16-fixad pools DL3/UL2/probe1) stands as the design.

## 4. NOT done
Everything in the task list: GPU kernels, graph/persistent kernel, GPU-vs-CPU tests, interference bench, microbench,
removal of ISAC_PDCCH_GPU_FEP / NR_GPU_POLAR / ISAC_PDCCH_GPU_SCAN switches, ctest. No builds or benches run (OTA gate).
