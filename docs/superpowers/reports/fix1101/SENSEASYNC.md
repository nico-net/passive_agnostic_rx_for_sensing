# fix1101/senseasync: sensing work off the decode-critical threads

Base cand1101 (ac5c4ec2af). Branch fix1101/senseasync. Host: DGX aarch64.

## 1. Where the sensing time went  [OFFLINE VERIFIED, DGX replay 2026-11-01, pair3 logs, ISAC_METRICS + EVMGRANT]

Logs: night/pair3/{c1,c2}_pci{415,923} (sensing cfg, cand1101 build). The `DATA_AIDED_ASYNC` / "off the decode thread" lines are absent from all 8 logs
(0 matches): the async path never engaged. Eligibility was `cw->Nl == 1`, and 87-93 % of the decoded grants are Nl > 1 (EVMGRANT Nl field), so the
re-encode + windowed LS of nearly every CRC-OK grant ran inline on the PDSCH consumer thread.

| source (sense_prod_us / count, 100 s) | c1 415 | c2 415 | c1 923 | c2 923 | thread |
|---|---|---|---|---|---|
| pdsch_data | 3.29 s / 19170 = 172 us | 3.22 s / 18873 = 171 us | 1.66 s / 9486 = 175 us | 3.46 s / 20694 = 167 us | PDSCH consumer (inline) |
| pdsch_dmrs (PDCCH-side DM-RS tap) | 158 ms / 1096 = 144 us | 145 ms / 964 | 37 ms / 232 | 345 ms / 2430 = 142 us | scan thread, ~11 calls/s |
| pusch_dmrs | 116 ms / 2247 = 51 us | 102 ms / 1983 | 432 ms / 18130 = 24 us | 1.19 s / 48.8k = 24 us | UL consumer |
| ssb, csirs | 22 ms each | same | 2-6 ms | 4-12 ms | negligible |
| prach, srs | 12-14 s (3.8 ms/call), 0.5-0.7 s | | | | other agents (fix1101/prach, srsq) |
| sense_rec (grant meta records) | 1.7 s / 221k = 7.6 us | | | | consumer, small |

So on the consumer threads sensing was 1.7-3.5 s per 100 s (1.7-3.5 % of one thread), mean 170 us per call, 190-210 calls/s.
The per-call tail (large Nl 4 / 217 PRB grants) is not in the logs (only sums). EVMGRANT e2e (us): qwait p50 131-259, p99 3.3-6.3 ms;
e2e[6] p50 1.5-2.7 ms, p99 8.5-15.3 ms. The same cells in the older "i" runs (Nl > 1 export skipped, pdsch_data prod 0.2-0.47 s) show
qwait p99 2.9-5.2 ms and e2e[6] p99 8.6-14.7 ms: the sensing delta in the replay is NOT separable at p99 from these fields alone
[HYPOTHESIS: it adds queue wait to the grant behind a long Nl 4 tap; needs a per-grant timer]. PDCCH-side DM-RS tap: 0.16 % duty, 144 us per call, not the p99 driver.

## 2. What moved

`openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c/.h`
- `nr_isac_pdsch_data_aided_submit_async` accepts Nl 1..4 (NR_MIMO_LS_MAX_NL), contiguous and non-contiguous allocations (the latter snapshots the carrier band
  instead of running inline), TB up to 4 x 64 kB. Pool 8 jobs (queue hard cap NR_BGQ_MAX), all prefaulted before RX start.
- The inline fallback is gone. A grant the worker cannot take (no free job, queue full/busy, geometry, pool not built) is SHED and counted; the tap
  never runs on the decode thread. Only no-output early returns (G refusal + its counter, source off, Nl outside 1..4) still execute on the caller (a few compares).
- Worker scratch (re-encode/LS buffers, ~12 MB TLS: seg storage, coded bits, scrambled, mod_syms, isac_h, ygath, win_k, isac_k/l, segments) is now
  file-scope and built + prefaulted ON the daBg worker by a warm job in `_async_init` (before the RX fence): no alloc/first-touch after start.
- Counters: `nr_sense_async_note(src, queued)` (nr_sense_api.c/.h), per-cell stats `async_queued[src]`, `async_dropped[src]`, exported in ISAC_METRICS as
  `sense_async_queued_<src>` / `sense_async_drop_<src>` for ALL 8 sources (nr_passive_metrics.{c,h}, nr_passive_metrics_json.c; sense_cost buffer 4096 -> 6144).
  The other two agents (srs, prach) can call the same function. `DATA_AIDED_ASYNC` log line now prints `dropped=N(pool= queue= geom=)`.
- Not moved: the PDCCH-side DM-RS CFR tap (needs the scan thread's FEP/chest buffers; 0.16 % duty, 144 us per call), grant-meta records (7.6 us each).

## 3. Tests  [OFFLINE VERIFIED, DGX aarch64 unit]
- New `test_nr_pdsch_data_aided_async` (tests/nr_pdsch_data_aided_async_check.c): production tap (re-encode stand-in, scramble, modulate, RE sweep, LS, sub-slot grouping)
  inline vs async, hash of every exported frame (slot, frac, k/l, noise, layer meta, every float of every antenna row), Nl 1..4 x {type1 2 CDM, type1 1 CDM with 256QAM/64QAM,
  type2 non-contiguous PRB list, 90 PRB QAM256} x {no sub-slot, sub-slot groups}: 32 cases, all identical; the consumer's rxdataF is overwritten right after the
  submit (snapshot independence); async frames never exported on the caller thread; a 64-grant Nl 4 burst: 8 queued, 56 shed, counters (local + per-source ISAC) match, 0 inline.
- The LDPC encoder is a deterministic stand-in (the real encoder is shared by both runs): bit-exactness is of the reconstruction/export chain around it.
- Build: nr-uesoftmodem pdsch_gpu ldpc_cuda tests, ninja -k 0, no errors. ctest -R "static_audit|data_aided|da_snap|mimo_ls|sense|passive_metrics|mpci_cellfeat": 10/10 pass
  (mpci_statics.tsv regenerated: the scratch pointers moved from function-scope to file-scope TLS, same class). ctest -R gpu_pass|mpci: all pass except
  `test_nr_gpu_pass_cells` (GpuPassCells.HarqSlabsAreSeparate, a GPU slab test; this change does not touch the GPU pass; not compared against the cand1101 baseline: no cand1101 build of it exists).

## 4. Not verified / risks
- Not run on air or in replay (forbidden here). Drop rate at the real burst structure is unknown: read `sense_async_drop_pdsch_data` / (queued+drop). If it is not rare, the pool is already at the queue cap (8); the next lever is a second worker, not inline.
- Records are now delayed by the worker (same CFR rows, later); slow-time index is the producer's absolute slot (override re-applied), unchanged.
- Test links with `--allow-multiple-definition` so the test's own `nr_isac_*` sink wins over the library's.
- Merge overlap: nr_passive_metrics{.c,.h,_json.c}, nr_sense_api.{c,h} (any agent adding fields next to `sense_async_*`/`sense_cfr_drop_*`), CMakeLists.txt near test_nr_ssb_rate_match_prod.

## 5. Expected effect [HYPOTHESIS]
Consumer-thread work from sensing goes from ~170 us per CRC-OK grant (tail ms for Nl 4 wide grants) to a snapshot memcpy of the grant's region (tens of us; `consumer_us_saved` in DATA_AIDED_ASYNC = worker run - snapshot). Records per 100 s may fall slightly if drops occur (CRC/latency have priority).
Read on air: `sense_async_queued_pdsch_data`, `sense_async_drop_pdsch_data`, `sense_cfr_exported_pdsch_data` (should stay ~37-46k/100 s), `DATA_AIDED_ASYNC ... dropped=`, EVMGRANT e2e qwait p99, pdschq_max_lag.
