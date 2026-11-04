# Task S3 report (quality + link context in CFR records)

Status: DONE with concerns (below). Branch sense/s1fix. Host: DGX aarch64 (Grace), build dir cmake_targets/ran_build/build.

## What changed
- nr_sense_api.c: per-cell qring (NR_CELL_STATIC, rows added by hand in tools/mpci/mpci_statics.tsv; static_audit --check: no NEW/UNCLASSIFIED). push_cfr computes nr_sense_quality_compute only when nr_isac_mode()==SENSING, after the frame is accepted, outside mu, on the pushing thread; stored by frame_id % cfr_ring_frames with an id tag. nr_sense_quality_for_frame(id,&q) = tag check, SENSING only.
- nr_sense_records.{h,c}: meta gains q, q_valid, wideband, recon_valid, prg_valid, prg_size, cfg_ref_ver. emit_rs looks the quality up and sets wideband at emit time (writer formats later, so min_prb is frozen at emit). JSON keys appended only when q_valid.
- nr_pdsch_passive_decode.{c,h}: read-only nr_pdsch_prg_latched(rnti): 0/2/4 once the sweep LATCHED, else -1 (null).
- nr_pdcch_blind_monitor_rt.c (DM-RS CFR) and nr_pdsch_data_aided.c set prg_size (sensing mode only). nr_isac.cc copies recon/prg/cfg_ref from detail.
- nr_pdsch_data_aided.c + new nr_pdsch_da_recon.h: recon_conf (rank 1 only; Nl>1 stays null).
- CMake: nr_sense_api links nr_isac_mode (PUBLIC).

## Deviations from the brief (deliberate)
1. `dir` already existed ("DL"/"UL"). To avoid a duplicate key, records with q_valid print the lower-case "dl"/"ul" in place; legacy keeps "DL"/"UL". Meta fields `dir` and `ssb_beam` were not added: dir is derived from src, ssb_beam from ssb_index (SSB source only, else null).
2. recon_conf: the brief says "where re-encoded symbols are already compared, ~:507"; no such comparison exists in nr_pdsch_data_aided.c (:507 was the meta). Implemented as leave-one-out per-PRB MRC equaliser, hard decision vs re-encoded X (see nr_pdsch_da_recon.h, `shortcut:` comment: ignores phase slope across the PRB).
3. cfg_ref is null unless a producer sets cfg_ref_ver (no RS producer does yet): format "identity_gen.dedicated_epoch.ver" as in traffic records.
4. valid_mask hex omits trailing zero bytes; bit p = PRB first_prb+p.

## Tests (all green after final build)
- test_nr_sense_records 24/24, test_nr_sense_api 22/22 (incl. QRing.FrameIdRingMiss, DroppedPushLeavesNoQuality, LegacyPushComputesNothing, wide u32 path), test_nr_sense_quality 14/14 (incl. SenseReconConf x3).
- LegacyRecordUnchanged: golden captured from the pre-S3 binary (committed as literal kGoldenLegacyRs); direct formatter and the file record via the writer both equal it.
- Full build: ninja -k 0 (nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests) clean, 24 .so. ctest -j4 failures: only baseline ones + test_nr_gpu_pass_geo, which passes alone (flaky sibling of the listed gpu_pass tests); test_static_audit_scanner passes. Frozen check exit 0.
- RED/mutations (TDD caveat: the implementation was written before the first test run, so RED is shown by mutation):
  M3 lookup always false -> QualityKeysInSensingMode, FrameIdRingMiss, DroppedPushLeavesNoQuality, QualityMatchesCompute... FAIL.
  M1 drop the id tag check -> FrameIdRingMiss, DroppedPushLeavesNoQuality FAIL.
  M2 compute in every mode (guard removed in push) -> LegacyPushComputesNothing FAIL (this test was added because M2 first survived: the lookup guard alone hid the wasted compute).

## Measured cost (DGX aarch64, synthetic CFR, 4 ant, 20 reps mean, single run, host shared with other agents; test QRing.CostPerRecord prints S3_COST)
- pdsch_dmrs 128 PRB x 2 sym comb-2 (n_re 1536): push 12.1 us legacy -> 75.3 us sensing: +63 us/record.
- pdsch_data full grid 217 PRB x 13 sym (n_re 33852): 168 -> 690 us: +522 us/record.
- pdsch_data full grid 128 PRB x 13 sym (n_re 19968): 95 -> 417 us: +322 us/record.
- recon_conf (not in the above) runs on the data-aided bg worker, not the RX thread; not separately timed.
- Quality is charged to NR_SENSE_COST_PACK so the existing sense CPU budget sees it.
## Thread placement
Quality runs on the pushing thread: the DM-RS CFR is pushed from the PDCCH blind monitor path (RX-side), the data-aided CFR from the data-aided bg worker. Deferral to the writer was not done: emit_rs needs the numbers right after the push. Added RX-side time is the DM-RS figure above, only in sensing mode.

## Concerns
- prg_size is null for every RNTI that never left the incumbent wideband arm (the gated sweep only latches after exploring), i.e. null in the normal case; non-null only after a latch. Per the brief ("null when not latched").
- Stack use of nr_sense_quality_compute (~35 KB) and nr_da_recon_conf (~18 KB) on caller threads; fine on current threads, not measured on small-stack ones.
- Unit-tested on synthetic data only; no OTA/replay run (receivers not run per instructions).
- tools/mpci/mpci_statics.tsv has pre-existing stale line numbers elsewhere; only one row was added (--write would rewrite ~1000 lines).

# Fix round 1 (review task-S3-review.md; coordinator rulings binding)

Build/test: ninja -k 0 full target set clean, 24 .so; ctest -j4: only baseline failures (gpu_pass_geo_discrete, pusch_ra0_qam256, ssb_rate_match_prod, gpu_pass_cells, dft_test, ldpc_cuda_pool_cpu, modulation, pusch_ul_disc + Not Run uci/DISABLED); static_audit_check and test_static_audit_scanner pass; frozen check exit 0. Sense tests: records 28, api 24, quality 17, all pass.

- C1 FIXED: `dir` is always "DL"/"UL" (the brief's lower case dropped; S4 must read "DL"/"UL"). Test DirIdenticalWithAndWithoutQuality (same sources, ring hit vs frame_id 0). Mutation (lower-case when q_valid) fails DirIdenticalWithAndWithoutQuality and QualityKeysInSensingMode.
- I1+I2 FIXED (quality off the pushing thread): push_cfr no longer computes anything. The sense writer computes the quality of each frame while packing it (reads the ring slot in place; frames are now 4-byte aligned; no malloc, no stack on RX threads), stores it id-tagged in the per-cell qring (size cfr_ring_frames+64 so a few later batches cannot evict a pending frame; the brief's `% cfr_ring_frames` widened for this reason). emit_rs only sets want_q/min_prb; the writer formats an RS job after its frame has been processed: if the head job's frame is not yet processed and frames are queued, the frame stream runs first; a batch of jobs is cut at the first unprocessed one. Producers never wait: ring/drop semantics unchanged (tests: ProducersNeverBlockWhenTheWriterStalls = 300 push+emit with the writer paused in < 1 s with cfr_dropped/rec_dropped counted; JobsQueuedBeforeTheirFrame... keeps order and quality). New stats q_computed, q_miss (a record formatted without quality because its frame was overwritten/unavailable/never queued; never a stale value). Nothing is dropped by this mechanism.
  Measured (DGX aarch64, QRing.CostPerRecord, 20 reps, shared host, soft): pushing thread, sensing vs legacy: dmrs 128PRBx2sym 16.7 vs 18.6 us (-1.9), data 217PRBx13sym 178.1 vs 167.7 (+10.4, jitter), data 128PRBx13sym 107.5 vs 109.2 (-1.8) => ~0 (was +63/+522/+322 us). Writer per frame: 59.7 / 323.4 / 191.6 us. Tests: PushingThreadDoesNotCompute, LegacyPushComputesNothing, FrameIdRingMiss (70 pushes, id overwritten at +66), DroppedPushLeavesNoQuality.
- I3 FIXED: nr_pdsch_da_recon.h now uses a leave-one-out window of point-reflected pairs about (l,k): +-2 subcarriers, +-1 symbol, clipped to the PRB (never across PRB/PRG), each pair weighted by the smaller |X|^2 (ML Y*conj(X)/|X|^2 form, exactly symmetric so linear delay/CFO phase cancels). Tests NoBiasFromDelaySlopeOrPhaseRamp: 300 ns at 30 kHz (3.2 deg/SC) and 0.1 rad/symbol ramp, QAM16/64/256, each within 0.02 of the flat result and every PRB passing (an unpaired |X|^2-weighted window measured 0.978/0.967/0.853 at 256QAM before pairing). Documented in the header and in the schema: recon_conf/recon mask measure per-RE symbol accuracy of the raw demod against CRC-verified X (model fit/SNR), NOT whether the reconstruction is correct.
  Cost: nr_da_recon 7.1 ms for a 217 PRB x 12 symbol grant (31k RE, 4 ant, aarch64, -O2, best of 5, scratch bench) on the data-aided background worker (not the RX thread). That is heavy for many big grants per second; cheaper variants (float, no binary search) are possible, not done.
- M5 FIXED: unit u is cross-checked against the unit implied by mean energy (15 % tolerance) and needs >=64 symbols (>=256 for 256QAM); else NaN and no verdict. (Not the modulator amplitude table: the tables overflow int16 for 64/256QAM so their scale is not safely reusable.)
- I4 DONE (per-PRB, joins validity): nr_da_recon outputs a per-PRB pass mask (>= 6 scored REs and agreement >= NR_SENSE_PRB_RECON_MIN = 0.5, provisional: a wrong channel model decides near-randomly, <= 0.25 agreement at 16QAM+, a model-consistent PRB at a CRC-passing SNR agrees on a clear majority; to calibrate on sens6, never per cell). The mask is by absolute PRB so any sub-slot slice is exact; nr_sense_quality_apply_recon ANDs it into valid_mask and recounts valid_cfr_prbs (so wideband follows it); snr/resid percentiles stay as computed over (a)(b). Without a data-aided verdict (DM-RS, rank>1, NaN) validity is unchanged. The scalar recon_conf is the whole grant's value copied on each sub-slot record (documented). Tests ReconMaskJoinsPrbValidity, SenseQualityRecon.ApplyRecon..., per-PRB wrong-reconstruction test. Mutation (mask not joined) fails ReconMaskJoinsPrbValidity.
- M1 FIXED: UL (pusch_dmrs) and SSB goldens added (formatter output with q_valid=false; that path is the pre-S3 code: git diff vs 5f6733d34d shows removed lines only in the dir ternary and the emit block).
- M2 FIXED: nr_pdsch_prg_latched scans the table without bumping the LRU stamp and creates nothing; the per-record g_ptrs_lock stays (short, sensing mode only; caching needs invalidation on epoch changes, not done).
- M3 documented (meta comment: null prg_size = not latched, not wideband). M4 kept per the brief (ssb_beam duplicates ssb_index, SSB source only). M6 out of S3 scope (allocated_prbs from kept REs, estimated_usable = allocated; track for S4). M7 pre-existing; one more hand row (qcap).

Concerns now: (1) recon CPU 7 ms per 217-PRB grant on the bg worker; (2) prg_size still null unless the sweep latched; (3) cfg_ref still null (no RS producer sets cfg_ref_ver); (4) the DM-RS source has no (d) verdict, so its valid_cfr_prbs is (a)(b) only by design; (5) tests are synthetic, no replay/OTA run.
