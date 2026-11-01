# pred1101/harq -- DL HARQ retransmission anticipation

Branch pred1101/harq (from mpci-int-1031). Status: built and tested.

## What already existed (kept)
The per-cell soft store (`g_harqc`, 64 entries, LRU, host buffer or device blocks, `harq_retx_decide` + `nr_hq_retx_rule`) already retains the soft sum of every CRC-failed
first transmission on both the CPU path (`passive_ldpc_decode`) and the GPU pass, and combines same-NDI/same-TBS grants of any rv. Combining physics test (rv0+rv2) exists:
`Cb0.EveryRvDecodesAloneAndCombined`.

## Changes
- `nr_hq_retry.h`: `nr_hq_window_slots/nr_hq_expired` (150 ms air-time age window), `nr_hq_ndi_evicts`, `nr_hq_alone_decodable(bg, rv, r1024)` (TS 38.212 5.4.2.1 circular-buffer
  wrap over the systematic columns, smallest kb => sound for any TBS). `nr_snr_gate.h`: `nr_snr_gate_r1024`.
- `nr_pdsch_passive_decode.c`: counters (per cell, NR_CELL_STATIC) nohist / nohist_ok / refused / evict_ndi / evict_lru / evict_age; `harqc_acquire` sweeps idle sums older than the
  window (releases device blocks) and counts LRU takeover of a live sum; entry `soft_slot`; `harq_retx_decide` counts NDI-toggle eviction and no-history retransmissions;
  CRC credit at the verdict; `out->hq_mode`; HARQC line extended.
- `nr_pdcch_blind_monitor_rt.c`: a solved RNTI's rv != 0 grant with NO stored sum was refused; now decoded alone when `nr_hq_alone_decodable` on the MCS code rate (failure stores its
  sum for the next retransmission); refused ones counted.
- Observability: EVMGRANT `hq=` (0 first, 1 combined, 2 retx alone), ISAC_METRICS `retx_combined, retx_crc_ok, retx_no_history, retx_no_history_ok, retx_refused, evicted`.
- No new flags/env vars.

## Tests (added)
- `test_nr_td_cb0_batch`: `Cb0.AloneDecodablePredicateIsSoundAgainstTheDecoder` (predicate true => real OAI encode + receiver decoder passes alone, 12 shapes x LBRM x rv).
- `test_nr_hq_retry`: `HqAnticipation.*` (NDI toggle evicts, window, store lifecycle, gate monotone).

## NOT verified / hypotheses
- No replay/OTA. [HYPOTHESIS] extra CRC from alone-decoded rv2/3 retransmissions of unsolved-history pids; cost = one extra decode per accepted such grant (cap pdsch_max_per_slot applies).
- Counters double count a grant when the GPU pass defers to the old path (shadow / need_cpu).
- On-air: HARQC line `retx_no_history=ok/n refused evicted`, EVMGRANT hq=2, ISAC_METRICS keys above.
- GpuPassCells.HarqSlabsAreSeparate: not touched, not in this path.
- Same-file risk: nr_pdsch_passive_decode.c, nr_pdcch_blind_monitor_rt.c (gate at the solver block), nr_passive_metrics*.

## Results [OFFLINE VERIFIED, DGX aarch64 unit]
Build (heavy.sh, -j6): nr-uesoftmodem pdsch_gpu ldpc_cuda + test targets, no errors. ctest passed: test_nr_hq_retry, test_nr_td_cb0_batch (+_discrete, _cpu; includes the new real-LDPC
soundness test), test_nr_snr_gate, test_nr_harq_init_tx, test_nr_pdsch_retry_pred/_gate, test_static_audit_scanner, static_audit_check (`static_audit.py --check` and `--strict` rc 0;
tools/mpci/mpci_statics.tsv updated: 6 PER_CELL counters, 1 TLS). Full ctest and GpuPassCells not run.
