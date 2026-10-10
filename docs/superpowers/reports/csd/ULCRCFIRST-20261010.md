# UL layout lock: SIB1-first and CRC-first (2026-10-10, branch csd/ul-crc-first)

## Why
OTA (Vodafone PCI 415, ota_runs/ab_V1_1_202044/run.log): PUSCHQ queued=19532 decoded=0 crc_ok=0; every job was a DM-RS probe at
~0-1 dB SNR, the CRC-free lock never converged in 150 s. The SIB1 initialUplinkBWP was never used.

## What
1. `nr_pdcch_sib1_prior_t` gains `ul_bwp_start/ul_bwp_size/ul_bwp_valid` (config_ue.c, NRRIV2BW/NRRIV2PRBOFFSET with MAX_BWP_SIZE).
   The UL msg3/pusch-ConfigCommon TDRA list is NOT stored (not needed; k2 still comes from the DCI statistics).
2. `ISAC_UL_LOCK_SIB1_FIRST=1` (needs `ISAC_UL_LAYOUT_LOCK=1`): the SIB1 UL BWP is a BWP candidate N, is added to the hypothesis
   list before the generic ones (index 0 when the list is first built) and is ranked first for CRC trials. Still a hypothesis.
3. `ISAC_UL_LOCK_CRC_FIRST=1` (K = `ISAC_UL_LOCK_CRC_K`, default 3, max 4): once a DCI length is fitted, one DCI per slot with
   MCS <= 10 also yields up to K real PUSCH jobs (tagged `NR_UL_LAYOUT_TRIAL`, hypothesis encoded in hyp_generation), ranked
   SIB1 BWP, then DM-RS votes, tails best-two first. They use the normal queue/decoder (no DL ticket needed, no SRS/discovery
   feedback). A TB CRC pass calls `nr_ul_layout_crc_result` and locks that exact layout (BWP, prefix, tail, k2) at once:
   `ULLAYOUT_CRC_LOCK`. Votes/bit statistics are untouched and remain the fallback. Locked grants count CRC passes; after 8
   consecutive CRC failures trials resume (excluding the failing layout) and a pass under another hypothesis re-locks
   (`ULLAYOUT_CRC_LOCK RELOCK`, other lengths re-derived under the new BWP). The grant book dedupe now also compares
   hyp_generation for trials only (same DCI, different hypothesis = distinct job).
   Stats: STATS line shows trials / crc_ok / crc_locks per length.

## Flags
Both default off; with them off no new code path runs (trials return 0, results ignored, book dedupe unchanged, new cell
fields zero). Byte-identical behaviour is argued from the code, not diffed on logs.

## Tests (evidence: offline unit, DGX aarch64, build_t26)
test_nr_pdcch_blind_monitor: 249 passed, 0 failed (6 ReconfigReplay skips pre-existing). New: UlLayoutLock.Sib1PriorCarriesTheUplinkBwp,
UlLayoutLock.CrcFirstLocksAtOnceSib1RankedFirstAndRelocks (flags-off returns 0 trials; SIB1 BWP is trial 0; CRC pass under
hypothesis 1 overrides ranking and locks with zero votes; no trials while locked; 8 failures then trials; re-lock to the SIB1 BWP).

## Risks
- CRC pass is only as reliable as the decoder under a wrong-but-near layout; false pass chance is the TB CRC24 (~6e-8) per job.
- Trials cost up to K PUSCH decodes per slot pre-lock; bounded by 1 DCI/slot, MCS filter and the existing drop-oldest queue; not
  measured under load. "Strongest probe SNR" preference not implemented (MCS filter only).
- The queue worker is untested end to end: only the layout-lock logic and the book/RT wiring compile; no replay/OTA run.
- Trial grants bypass the DL-ticket requirement (as probes do); UL-state update is skipped for them.
Evidence labels (PROJECT_MEMORY 0.1): OTA effect NOT MEASURED; replay NOT MEASURED; unit tests offline only.
