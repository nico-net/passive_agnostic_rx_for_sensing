# recovcfo report (branch twomodes/recovcfo, commit 460631e9d3)

## Root cause [MEASURED OTA DGX logs + CODE, 2026-11-05]
The RX_RECOVER re-sync CFO is `ssbInfo->freqOffset = initial_fo + pss_res.freq_offset + sss_res.freq_offset` (nr_initial_sync.c nr_scan_ssb).
PSS stage: half-symbol phase, range +-SCS (pss_nr.c:319 / :497). SSS stage: phase over the PSS-SSS spacing, `atan2/pi/4.3*SCS` (sss_nr.c:339-341),
measured AFTER the data was compensated by the PSS estimate (ssb_try_candidate, nr_initial_sync.c), so it only wraps if the PSS error exceeds +-SCS/4.3 = 6977 Hz.
A weak PSS peak (E1B re-sync: peak/avg 85/77 dB = 8 dB; A1B: 83/75; healthy re-syncs 10-15 dB, e.g. 86/71) gives such a PSS error, the SSS then
returns truth minus one period P = 2*SCS/4.3 = 13953 Hz: -13802 (E1B), -14282 (A1B epoch 2), and the A1B epoch-3 +14161 is the opposite alias that healed it.
PBCH CRC cannot reject it (rsrp 47 dB/RE, residual 0.46 SCS still decodes); the PBCH/DM-RS tracker has the same wrap, so nothing detects it afterwards.
nr-ue.c UE_synch then adds radio fo (248) -> "carrier off -13554".

## Reproduction [MEASURED unit, DGX aarch64]
test_nr_sync_pin / resync_alias(): production nr_search_ssb_common on a synthetic cell, true +200 Hz, PSS stage forced -11000 Hz off
-> pss -10792 sss -3578 est -14370 (OTA: -13802/-14282); +11000 -> +14827. Resolution returns -417 / +874 (the crude PSS-symbol-only injection leaves ~600 Hz).
Mutation (nr_cfo_alias_k returns 0) -> 7 FAILURES; restored -> all checks pass.

## Fix
- nr_initial_sync_budget.[ch]: nr_sss_cfo_period_hz(scs)=2*scs/4.3, nr_cfo_alias_k(est, prior, period). Bound derived from SCS only (half period).
- nr_initial_sync.c nr_scan_ssb: when a prior exists and the estimate is more than half a period from it, recompensate the SSB window by sss+k*P,
  redo FEP + PBCH; the alias is taken only if PBCH CRC passes, else the measurement stands. Log "SENSING: RESYNC_CFO_ALIAS ...".
- nr-ue.c UE_synch: prior set only for the RX_RECOVER path (fast_pss_position>=0, incl. its full-capture fallback): 0 (estimate is relative to the radio tuning)
  or dl_Doppler+freq_offset with cont_fo_comp. Initial acquisition, scan, MPCI live re-add: no prior -> unchanged.
- No env/CLI/-D flags, no per-cell constants. mpci_statics.tsv: 2 TLS rows added (audit --check rc 0).

## Not done
- Task 2 item 2 (post-hoc detection: PBCH ok but PDCCH/DM-RS quality collapse -> re-evaluate). No per-cell PDCCH quality baseline exists, the PBCH tracker
  has the same alias, and it needs OTA/fault-injection to validate. The prevention at re-sync covers the observed 2/84 mechanism only.
- rfsim cannot inject an RX_RECOVER (needs USRP ring gaps); no recovery-with-CFO run. The nr_scan_ssb re-evaluation branch (recompensate + PBCH) is not exercised by a unit test, only by the pure helper and the estimator test.
- Re-syncs after a plain sync loss (non-RX_RECOVER) are not given a prior.

## Verification [MEASURED, DGX aarch64, fresh build dir]
- Build: full CLAUDE.md list + coding oai_iqplayer params_yaml polar_gpu raw_iq rf_emulator vrtsim csirs_disc_gpu ldpc_cuda nr_csirs_disc_gpu_sweep pdcch_gpu pdcch_nid_gpu pdsch_gpu shm_iq_chan_gpu shm_iq td_cb0_gpu: 0 FAILED, 24 .so, liboai_device.so symlink.
- ctest -j4: 22 failed of 397. Known set (uci x7 Not Run, ra0_qam64/256, dft_test, test_nr_modulation, pusch_ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells,
  gpu_pass_geo_discrete, ldpc_cuda_pool_cpu) plus test_nr_spin_handoff, time_management_tests, test_nr_pdsch_gpu_demod_discrete, test_nr_gpu_pass_geo (all four pass on rerun, load-flaky)
  and test_nr_gpu_pass (also fails in the unmodified rr-e8 build). test_nr_sync_pin passes.
- static audit --check rc 0 (2 TLS rows added by hand, no renumber); frozen check OK.
- Legacy 4-RX rfsim gate, 1 run x 420 s, under flock -x heavy + td_measure: PASS [solver] n_solved=1 ttc 4.53 s, postsolve CRC 100.00 (51992 grants), crc_floor 96.14, drop_full 0.057 %. N=1 only (not the default 2).
  This run has no RX_RECOVER, so it shows no regression of the non-prior path, not the fix.
- No OTA run done.
