# pred1101/prestage -- where the DL per-grant time goes, and what pre-staging can and cannot buy (2026-11-01)

Branch `pred1101/prestage` (from mpci-int-1031 HEAD a6e8f12e52). Evidence labels per PROJECT_MEMORY section 0.1.
Nothing here was run on air; the receiver, replays and OTA were not started.

## 1. Measurement: per-grant time, PCI 415, 4 RX, no pooling across hosts/cells

Source: EVMGRANT lines of `/home/nicola/NICOLA/ota_runs/a1I415_1_102531` (int1031, sensing OFF, n = 19494 full decodes) and
`a1S415_1_103130` (int1031, sensing ON, n = 15738), both DGX + X410, tune 3680010000, `[MEASURED DGX+X410 OTA 2026-11-01, 4 RX, single run each]`.
Fields: `e2e=arr2enq,qwait,pre,dec,...,tot`, `gp=total,before_try,try,prep,launch,gstart,gspan`, `gq=C,D,L span`. Scripts: session scratchpad `an/an.py`, `an/bt.py`, `an/tail2.py` (not in git).

| us | sensing off p50 / p90 / p99 | sensing on p50 / p90 / p99 |
|---|---|---|
| e2e tot | 1430 / 4126 / 10862 | 1780 / 4407 / 10236 |
| arr2enq (slot arrival -> grant enqueued, PDCCH side) | 517 / 1154 / 2196 | **764** / 1301 / 1931 |
| qwait (enqueue -> dequeue) | 126 / 758 / 3204 | 125 / 1197 / **4963** |
| pre (dequeue -> decode start; producer wait for k0>0, earlier members of a slot group) | 48 / 106 / 5441 | 47 / 100 / 4759 |
| dec (decode call) | 680 / 1556 / 4375 | 667 / 1446 / 3861 |
| gp total (the GPU pass call inside dec) | 553 / 1233 / 3304 | 550 / 1184 / 3171 |
| gp before-try (host job build) | 24 / 109 / 512 | 23 / 85 / 359 |
| gp prep / launch (host) | 24 / 66 | 24 / 66 |
| GPU: gspan = C 130 + D 218 + L 85 (p50 sum differs from the p50 of the sum) | 443 / 894 / 2544 | 440 / 833 / 2464 |

Findings:

1. **The pass is 39 % of the p50 e2e; 80 % of the pass is GPU** (gspan 443 of 553 us). C/D/L spans are the same for 24 PRB and for 217 PRB grants
   (D 218 us in both): the GPU part is a latency chain, not compute. Host part of the pass: before-try 24 + prep 24 + launch 66 (the launch overlaps the GPU start;
   gstart is 9-11 us) = at most ~115 us p50 (8 % of e2e).
2. **Everything that IQ-independent pre-staging could touch is <= ~50 us p50** (before-try + prep): steady-state RNTIs run the gold/DM-RS caches warm
   (before-try p50 23 us steady vs 27 first-seen vs 41 when the TBS grew, i.e. a gold-cache regrowth, 11 % of the before-try time). The p90/p99 of before-try
   (109/512 us) are not size dependent (rb>128: p99 66 us, rb<=64: p99 625 us), so they are scheduling, not data to stage. No host-side prestage can reach p50 1 ms.
3. **Speculative C (FEP + chest) before the DCI is not safe/useful here**: on DIRECT the FEP writes the consumer thread's own `rxdataF` (the pass key includes
   its pointer, `c_sig`), which the data-aided tap, the sensing taps and the retry arms read afterwards; a pre-run on another thread would need a copy of
   ~0.9 MB of rxdataF per grant to the right thread, costing as much as the C span it saves (130 us). Not implemented. The held-instance `skip_c` machinery already
   skips C on retry bodies of one decode call; there is nothing across calls to reuse because every slot has new IQ.
4. **Sensing-on latency (+350 us e2e p50) sits BEFORE the enqueue and in the consumer's between-grant time, not in the pass**: dec and gp are unchanged;
   arr2enq +247 us p50; qwait p99 +1.8 ms.
   * PDCCH thread: with `sources` containing `pdsch_dmrs`, `need_chest` is true and the accepted candidate took the slow path of
     `nr_pdcch_blind_monitor_rt.c` (full-slot FEP of the DM-RS symbol + `nr_pdsch_channel_estimation`, ISAC_METRICS `sense_prod_us_pdsch_dmrs`
     759309 us / 5570 = 136 us mean) BEFORE `nr_pdsch_passive_queue_enqueue`. [MEASURED ISAC_METRICS, same run]
   * Consumer thread: after every CRC-OK single-layer grant the consumer ran the whole data-aided re-encode (`sense_prod_us_pdsch_data` 511896 us / 3020 = 169 us mean)
     before taking the next job. [MEASURED ISAC_METRICS]
5. Tail (>= p99 grants, sensing off): mean 16.7 ms = arr2enq 1.3 + qwait 2.9 + pre 8.3 + dec 4.2 ms; "pre" is mostly the 100 us-step wait for a k0>0 hypothesis
   slot and the serialized members of a slot group, i.e. data/queue wait, not prestageable compute.

## 2. What changed

* `nr_pdcch_blind_monitor_rt.c` (enq_early, around line 9990): when the post-estimation SNR gate is off (`min_snr_lin <= 0`, derived from the config, no new switch) the
  decoded DCI is enqueued BEFORE the DM-RS CFR tap's FEP + estimate (the normal path enqueued unconditionally anyway: num_sc >= 12, so `snr_ok`). Same job fields as before
  (`layout_probe` stays 0 for the tap path, class counter and dump label stay "normal"); the tap's decode section is skipped (`enq_done`) so the grant is not enqueued twice.
  With `min_snr_lin > 0` or without a tap nothing changes. New counter `enq_before_tap` in the "blind PDCCH monitor summary" line.
  Limit: the tap of candidate k still runs before candidate k+1 of the same occasion is enqueued (one thread, sequential loop).
* `nr_pdsch_data_aided.c/.h`, `nr_pdsch_da_snap.h`, `nr_pdsch_passive_queue.c`: `nr_isac_pdsch_data_aided_submit_async`. The consumer copies the TB bytes, the grant structs and the
  part of rxdataF the tap reads (symbols of the PDSCH window, REs of the contiguous allocation, wrap at the FFT size; `nr_da_snap_copy`) into one of 4 per-cell pre-allocated,
  prefaulted jobs and hands it to a nice-19 `daBg` worker (existing `nr_bgq` + parked-thread machinery, thread built before the fence in `cell_prealloc`, armed at queue start).
  The worker calls the unchanged `nr_isac_pdsch_data_aided_submit` on the copy: identical CFR rows. Never drops: no free job / queue busy / non-contiguous allocation /
  TB > 64 kB / pool not built runs the tap inline exactly as before. Log: `SENSING: DATA_AIDED_ASYNC done= queued= inline= consumer_us_saved=` (first, every 2000th).
* `tools/mpci/mpci_statics.tsv`: `g_da`, `g_da_q`, `g_enq_before_tap` classified PER_CELL (NR_CELL_STATIC).
* `CMakeLists.txt`: `test_nr_pdsch_da_snap`.

## 3. Tests

`[OFFLINE VERIFIED, DGX aarch64 unit]`, all through heavy.sh, build dir `cmake_targets/ran_build/build`:

* build: `ninja nr-uesoftmodem pdsch_gpu ldpc_cuda` rc 0 (no new warnings; the one `-Wformat-zero-length` in nr_pdcch_blind_monitor_rt.c is old).
* new `test_nr_pdsch_da_snap` (2 tests, PASS): the snapshot holds exactly the elements the tap reads (its own enumeration `re = start_re + j`, wrap at the FFT size),
  bit for bit, and writes nothing else (poisoned destination), for wrap-around, full-row, one-RB-at-the-edge and 217 PRB cases; out-of-range regions are refused.
* ctest (8/8 PASS): test_nr_pdsch_da_snap, test_nr_pdsch_retry_gate, test_nr_pdcch_blind_monitor, test_mpci_multicell_monitor, test_mpci_percell_scorers,
  test_nr_cell_static_tls, test_static_audit_scanner, static_audit_check (static audit clean: UNCLASSIFIED=0 after classifying the 3 new PER_CELL statics).
* `test_nr_gpu_pass` (full GPU pass suite, under `flock -x /tmp/td_measure.lock`): PASS 69 s (the GPU path is untouched by this branch; run as regression).
* GPU span in the lab, `GpuPassStamps.SplitSpansAndLdpcVerdicts` (repeated back-to-back passes on the idle GB10, `[MEASURED DGX aarch64 unit, single run]`):
  clean pass gspan 356-903 us: nrb 96 Nl1 C 107 + D 106 + L 143; nrb 217 Nl1 C 131 + D 123 + L 233; nrb 217 Nl2 C 156 + D 521 + L 226.
  On air (section 1) C is 124-130 us p50. A warm back-to-back pass costs the same as on air, so there is **no cold-start penalty** a warm-up/pre-touch launch could remove.


## 4. Not done / not verified

* No pre-staging of grant geometry/graphs/LDPC parameters was implemented: section 1 shows the IQ-independent part is ~50 us p50 and the graphs/classes are already
  instantiated at start (default warm set); a hit/miss predictor would only count. The counters the task asked for do not exist for that reason; the new counters are
  `enq_before_tap`, `DATA_AIDED_ASYNC queued/inline/done/consumer_us_saved`.
* The async data-aided path and the enqueue reorder are verified by reading and by the snapshot unit test only; no end-to-end bit-exactness test of
  `submit` vs `submit_async` exists (needs a UE context). Controller: compare `sense_prod_count_pdsch_data` / `sense_cfr_exported_pdsch_data` and
  `sense_prod_count_pdsch_dmrs` on vs off (must match within run noise; drops would show `inline=`).
* `arr2enq` second and later candidates of one occasion still wait behind the previous tap.

## 5. Expected on-air effect `[HYPOTHESIS]` and where to read it

* sensing ON, PCI 415: arr2enq p50 764 -> ~520 us (the sensing-off value), e2e p50 1780 -> ~1.5 ms; qwait p99 4963 -> nearer the sensing-off 3204 us
  (consumer freed by ~170 us per Nl=1 CRC-OK grant). Sensing OFF: no change (no tap, no data-aided).
* Read: EVMGRANT `e2e=` arr2enq/qwait, ISAC_METRICS `sense_prod_count_pdsch_dmrs`/`pdsch_data`, `enq_before_tap`, `DATA_AIDED_ASYNC ... inline=` (should be ~0),
  `consumer_us_saved`.

## 6. Risks for other branches touching the same files

* `nr_pdcch_blind_monitor_rt.c` fast-path block (pred-hash / pred-periodic / harq agents anticipating grants may edit the same enqueue block).
* `nr_pdsch_data_aided.c` line numbers moved: `tools/mpci/mpci_statics.tsv` rows are keyed by line, re-run `static_audit.py --write` after merging.
