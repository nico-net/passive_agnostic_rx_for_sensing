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
