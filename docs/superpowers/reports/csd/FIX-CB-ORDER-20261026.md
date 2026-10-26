# FIX-CB-ORDER 2026-10-26 (branch fix/cb-order, from adaptive-rx-UL-DL 57df6abb85)

## What
The TB abort rule (NR_CB_FAIL_LIMIT) and the per-code-block metric (decodeTried / cb_tried / cb_ok) are now the same function of the
block verdicts on the CPU pool and on the GPU paths: blocks are judged in block order, the NR_CB_FAIL_LIMIT-th failed block is the abort
point, every later block is untried and failed.

1. LDPCdecoder (nrLDPC_decoder.c): a block cut short mid-decode by the TB abort returns NR_LDPC_ITER_CUT (0xFFFF, >= numMaxIter, so every
   `it < numMaxIter` caller is unchanged) and does NOT call note_cb_failure. Public signature unchanged. [CODE-READ]
2. coding_defs.h: decode_abort_t gets a failure bitmap by block index and per-block views (init_abort_block). A view of block r reports
   "abort" only when >= NR_CB_FAIL_LIMIT blocks BEFORE r have failed, so a block is only ever cut short if it lies after the abort point in
   block order, whatever the pool's completion order. The early abort still saves CPU. Plain (non-view) objects keep the old semantics
   (CUDA plugin CPU fallback, GPU host, gNB/UE callers all unchanged). nr_cb_order_result() is the one block-order definition.
3. nrLDPC_coding_segment_decoder.c: decodeTried = ran to completion; after the pool joins, nrLDPC_finalize_TB_order() derives tried/ok in
   block order (a block a worker finished after the abort point becomes untried+failed, bits zeroed) and leaves the TB abort object holding
   exactly the in-order failure count. TB CRC semantics untouched (TB passes only if every block + TB CRC pass).
4. nr_pdsch_gpu_ldpc_host.c (check mode): the `gab` shortcut is gone; GPU vs CPU compare ok, bits and decodeTried block by block for the
   whole TB (also when the abort flag was set on entry or the limit was hit).
   The GPU rule itself (gpu_tb, CUDA plugin :599, k_pick) was already block order and is unchanged.

## STOP result
- test_nr_cb_abort (extended) passes, failures=0 [OFFLINE VERIFIED, DGX GB10 aarch64 unit]. New checks: cut block not tried and no failure
  counted; view of a block with failures only after it still decodes; entry-aborted TB; pure rule incl. rate-matching-error block; real
  LDPCdecoder + nrLDPC_finalize_TB_order (dlsym from libldpc.so) with injected completion order forward / reverse / shuffled on 5 failure
  patterns == sequential in-order result (the GPU rule model) in tried_at/ok_at, nfail, abort flag, TB verdict, zeroed bits.
  Mutation check: breaking the limit in nr_cb_order_result makes 94 checks fail.
- ctest -R 'cb_abort|ldpc|gpu_pass|gpu_ldpc|pusch|pdsch' -j1: 73 tests, only test_ldpc_cuda_pool_cpu (SEGFAULT, known) fails; the 6
  test_nr_pusch_uci_* are Not Run until nr_pusch_ucisim is built (not in `tests`); built it, all 6 pass. gpu_pass/gpu_ldpc/pusch_gpu pass
  (two *_cpu skipped by the tests themselves). [OFFLINE VERIFIED]
- nr-uesoftmodem, nr-softmodem and `tests` build and link (ninja -k 0, 0 FAILED). [OFFLINE VERIFIED]

## Open items
- No device-level GPU-vs-CPU test of the real nr_pdsch_gpu_ldpc_host check mode exists (needs live GPU TB entry); the equivalence is
  tested against the sequential rule model that gpu_tb/k_pick implement. The check-mode comparison itself is [CODE-READ] only.
- rfsim gate: ulsch decoding (nr-softmodem, CPU pool) cb_tried/cb_ok (harq triedSegments/processedSegments) on failing TBs can change
  (now deterministic, no inflated tried / failure count); TB CRC, postconv CRC and ttc must not move. Hot path adds one popcount over <=5
  words under the existing per-iteration mutex.
