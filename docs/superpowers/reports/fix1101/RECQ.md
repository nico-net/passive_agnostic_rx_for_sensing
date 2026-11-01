# fix1101/recq: measured quality on every sensing RS record

Problem [MEASURED, DGX OTA 2026-11-01, PCI 923, a2S923_1_112602]: 109,671 pusch_dmrs records, noise_var:1, energy:null, no quality;
97 % of those grants have DM-RS snr_db < 0 in UL_PUSCH_CRC. A consumer could not separate usable from noise.

## Schema (2, additive)
- new field `snr_db` on every `type:"rs"` record, placed right after `noise_var`. Real dB or `null`. `null` when the estimator has none,
  or returned its floor (-30 dB from nr_ulsch_dmrs_snr_db = signal below noise; DL "none" <= -90). Never a sentinel. Real negative SNR is printed.
- `noise_var`: now the receiver's estimate; `null` when not measured (was a constant 1 for pusch_dmrs, 0 for ssb/csirs-less cases).
  Units: |h|^2 of the exported CFR (receiver fixed point; per RE, complex E|n|^2); SNR = mean|h|^2/noise_var is scale free.
- `energy`: unchanged (PRACH only; schema defines it nowhere else). The binary CFR header noise_var carries the same value.

## Per source (file)
- pusch_dmrs (nr_pusch_passive_decode.c, nr_ulsch_demodulation.c, nr_ulsch_passive_branch.h): snr_db = pvp->ul_dmrs_snr_db (the value UL_PUSCH_CRC
  prints, GPU pass and CPU path); noise_var = new pvp->ul_dmrs_noise_var = nvar/(1-g), the sigma^2 the SNR already used (no new estimation).
- pusch_data (nr_pusch_data_aided.c): snr_db = grant DM-RS SNR passed in; noise_var = existing second-difference estimate.
- pdsch_data (nr_pdsch_data_aided.c; callers in queue, blind monitor, attached UE): snr_db = dec.dmrs_snr.comb_db (null if <= -90; attached-UE
  caller passes none); noise_var = dec.nvar (null if 0).
- pdsch_dmrs / pdsch_dmrs_blind (nr_pdcch_blind_monitor_rt.c): snr_db = 10log10(ant-0 mean|H|^2 / nvar), the gate's own ratio; noise_var = nvar.
- ssb, csirs, srs, prach: snr_db is present and null (no SNR on those paths); noise_var as before (SRS estimate, CSI-RS noise power, ssb/prach null).
Plumbing: nr_sense_rs_meta_t.snr_db/snr_valid, nr_sense_meta_set_snr(), JSON in nr_sense_records.c, copied in nr_isac.cc.
No gating, no flags, no new env.

## Tests [OFFLINE VERIFIED, DGX aarch64 unit]
- test_nr_sense_records RsCarriesMeasuredQuality (all 8 sources, floor/-99/NaN -> null, negative kept, unset -> null/null).
- test_nr_isac_adapter Adapter.RsRecordCarriesSnrAndNoiseVar: production export path (nr_isac_submit_cfr_multi_meta -> records file).
- test_nr_pusch_mmse2 UlDmrsSnr.KnownNoise extended with nr_ulsch_dmrs_noise_var.
- ctest sense|isac_adapter|mmse2|mpci|static_audit: 19/19 pass. Full `tests` target builds. No golden fixtures changed (none contain rs lines).
- Failing, not mine: test_nr_pusch_ul_disc UlAlias.TrackedSeedOnAliasIsCorrected (also fails in rr-int-1031 build), test_nr_ssb_rate_match_prod
  (known ARM), test_nr_pusch_uci_* (not built by `tests`).

## Not verified
No receiver/replay run. On-air: [HYPOTHESIS] pusch_dmrs snr_db is null for ~22 % (floored) and < 0 for most of the rest, matching UL_PUSCH_CRC;
check `snr_db` vs `dec_snr_db`/UL_PUSCH_CRC per rnti+abs_slot. The noise_var unit for pdsch_dmrs is the chest's nvar (same estimator units as h).
Risk: nr_isac_pdsch/pusch_data_aided_submit gained a trailing `dmrs_snr_db` argument (other branches calling them must pass it, NAN for none).
