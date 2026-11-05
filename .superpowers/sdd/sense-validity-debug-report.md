# Sense validity debug report (branch sense/s1fix, commit e66832e533 on top of 42be5be883)
Evidence labels: [SIM VERIFIED] DGX aarch64, rfsim 106 PRB 4 RX, s7 cfg, 150 s. Nothing calibrated on OTA.

## Defect 1: valid_cfr_prbs = 0 for every source
Root cause (per producer, what noise_var reaches nr_sense_quality_compute):
- SSB: phy_procedures_nr_ue.c passes 0.0f (no estimate) -> `!(noise_var>0)` invalidated every PRB. Also on air, not only in sim.
- CSI-RS: integer noise_power of csi_rx.c (25 in rfsim, same scale as the LS H). The PRBs carry ONE RE each (row 2, density 1/RB): the `tot[p] < 3` rule makes valid=0 by construction (a sparse source; spec says CSI-RS is never wideband). Not changed.
- PDSCH DM-RS: estimator nvar (same call as H); 0 in a noiseless sim -> invalid.
- PDSCH data: dec.nvar is in rxdataF units (clamped to integer 1 by the estimator) but H = Y/X is in |Y|/|X| units (constellation scale ~16): noise 1 vs |H|^2 ~ 1 gave SNR <= 0 dB -> invalid. Scale mismatch, not just zero.
- PUSCH data uses nr_sense_h_noise_var (own second-difference estimate, OK). SRS passes c->noise_var (own).
Fix: nr_sense_quality.c: noise_var <= 0/NaN -> per-PRB local-fit residual (E[sse] = (cnt-2) n_ant sigma^2); noise floored at NR_SENSE_RECON_IMPL_LOSS (1e-3, the existing 30 dB implementation-loss bound) x PRB mean power, so noiseless gives ~30 dB, valid. pdsch_data (nr_pdsch_data_aided.c da_emit) now passes nr_sense_h_noise_var of H itself (sensing mode only; legacy/traffic unchanged).
Tests: test_nr_sense_quality 30/30 (3 new: noiseless valid at 30 dB for nv in {0,NaN,1e-30,1e-12}; missing estimate ~ given estimate within 1.5 dB; noise-only stays invalid without estimate; DegeneratePrbInvalid updated: nv=0 is now "unknown", not invalid).

## Defect 2: no pdsch_dmrs records, unit never admitted
Root cause (chicken-and-egg): the DM-RS tap and its unit gate (nr_sense_unit_gate, the only thing that counts a message for a disabled unit) sit in the blind monitor AFTER the solver branch, which enqueues the job and `continue`s (nr_pdcch_blind_monitor_rt.c ~9957). In the solver bed (pdsch_decode[try=0], cfr_submits=0 in the old log) no grant reaches the gate: no row, no wb_rate, scheduler never sees the unit.
Fix: consumer_dmrs_tap in nr_pdsch_passive_queue.c, called on TB CRC OK of a solver job (new job.dmrs_tap, set in the solver enqueue only; sensing mode only): gate (counts the message / skips when disabled), FEP of first DM-RS symbol, nr_pdsch_channel_estimation, comb-2 export as the monitor tap, unit cost accounted. Monitor tap unchanged (non-solver grants).
Test: Sched.DmrsUnitGainsHistoryFromTheGateAndIsEnabledNextToData (test_nr_sense_sched_rt).

## Build/test
ninja -k 0 clean, 24 .so; ctest -j4: 391 tests, 18 failures = baseline (uci x7, ra0_qam64/256, ul_disc, nr_td_sim, ssb_rate_match_prod, iq_chan, gpu_pass_cells, gpu_pass_geo_discrete, dft, ldpc_cuda_pool_cpu, modulation). static_audit --check rc 0; frozen check exit 0.

## rfsim 150 s (s7 cfg, --ue-nb-ant-rx 4) [SIM VERIFIED]
| source | records | valid_cfr_prbs | snr_db_p50 median |
| ssb | 9537 | 20 (6358), 8 (3179); all >0 | 29.996 |
| csirs | 795 | 0 (all; 1 RE per PRB) | null |
| pdsch_dmrs | 15104 | 106 (all) | 29.996 |
| pdsch_data | 4499 | 106 (all), recon_conf 1 | 29.996 |
Scheduler: 4 enable decisions; decision 9: admitted pdsch_data + pdsch_dmrs; pdsch_dmrs stays enabled, pdsch_data alternates (4 disable decisions; pool/ring ranking, not investigated). violations 0, ring_drops 0, disabled_skips 11791. pdsch_dmrs records: 15104 (cfr_exported 13831 in the previous run). SNR 30 dB = the ceiling, noiseless sim.
s7_cfr.bin deleted.

## Concerns
- 30 dB SNR is the floor-derived ceiling in a noiseless sim; no real-noise rfsim validation here (unit tests only).
- SSB/DM-RS on air: the record-residual fallback sees an interpolated/smoothed H (SSB) so it under-estimates noise -> SNR optimistic; SSB is narrow (<128 PRB) so never wideband.
- Lock-order: the lead's order (td_measure then heavy) is the reverse of another agent's (heavy then td_measure); I deadlocked once and killed my own waiters; used heavy->td afterwards. Another run's rm -rf /tmp/passive_rx deleted my first records file; copied the records inside the lock.
- Monitor tap code is duplicated in consumer_dmrs_tap (not refactored, to leave the legacy path untouched).
- Narrow: consumer DM-RS tap runs only on CRC-OK full decodes (verified identity), not on CRC-fail grants.
