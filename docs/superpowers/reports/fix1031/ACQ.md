# fix1031/acq: A1 + A2 + A3 (acquisition)

Branch `fix1031/acq` off `mpci-int-1031`. No new flag, env var or option.

## What changed
- A1 `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` (`nr_initial_sync_impl`, ~l.705-725, ~l.935-953) and `nr_initial_sync_budget.{h,c}`:
  a pinned passive start (`--ssb`/`--initial-fo` given, full scan) counts failed full scans per cell (`NR_CELL_STATIC s_pinned_fail`).
  After `NR_ACQ_PINNED_ESCALATE_AFTER` (3) failures every scan runs `nr_acq_cfo_ladder()` around `--initial-fo` (centre, +-7.04 kHz alias,
  then +-k*SCS and their alias neighbours, bounded to +-`NR_ACQ_CFO_MAX_HZ` of the seed, <= 48 rungs). The SSB/PCI pin is kept, PBCH CRC accepts.
  A lock resets the count. Auto-acquire (unpinned) ladder unchanged. Logs: `ISAC_ACQ_ESCALATE`, `ISAC_ACQ_CFO_TRIAL coarse_hz=`, `ISAC_ACQ_LOCK_OFFSET seed_hz= locked_coarse_hz= delta_hz= trial=`.
- A2 `nr_pdcch_blind_monitor_rt.c` (CSS0 interleave, ~l.5478-5500): the call counter `s_occ % 8` is gone. CORESET#0 runs on EVERY slot of the MIB-derived SS#0 window
  (`nr_acq_slot_in_window(frame*slots_per_frame+slot, period, offset, dur)` of the css0 cfg). A window covering the whole period falls back to `slot % 8`.
- A3 `nr_initial_sync.c` (`nr_search_ssb_common`, new `nr_search_ssb_select`, `nr_scan_ssb`) and `defs_nr_UE.h` (`n_alt`, `alt_pss/alt_sss`, `comp_state`):
  a blind pass keeps every accepted (PSS,SSS) candidate, ordered by SSS metric; `nr_scan_ssb` walks them until a PBCH CRC passes and logs
  `SENSING: SSB_INVENTORY gscn= frame= cand(pci:sss:pss)=[...] chosen=`. A pinned pass keeps its old best-of-pinned selection (`n_alt == 1`).
  A blind candidate whose SSB overruns the capture is now skipped, not a hard `return false`.

## Tests `[OFFLINE VERIFIED, DGX aarch64 unit]`
- `test_nr_sync_pin` (extended): `inventory()` two co-channel cells 4 dB apart (3 PCI pairs): blind chooses the stronger, reports both PCIs in SSS-metric order, `select(1)` yields the weaker at its timing, pinned unchanged;
  `cfo_escalation()` seeds of +40 kHz and -47 kHz are not locked at rung 0 and lock at a later rung (end-to-end CFO error <= 2 kHz); +9 kHz locks at rung 0.
- `test_nr_initial_sync_budget` (extended): ladder order/bound/coverage (every error within +-45 kHz has a rung within 8.1 kHz, SCS 15 and 30 kHz);
  CSS0 slot rule covers every SS0 slot for 7/8/10 occasion slots per 10 and 5 offsets; the old counter rule provably never hits SS0 for the 8-call pattern.
Results: see commit message / build status in the final message.

## Not verified
- No receiver, replay or OTA run. The 9 kHz case in the brief locks at rung 0 in the clean synthetic (PSS+SSS estimators with in-place PSS compensation reach ~+-10 kHz); on air the failure is PBCH-CRC at low SNR/multipath, which a unit fixture without a real PBCH cannot reproduce. The unit test therefore proves the ladder mechanics (>30 kHz), not the field 9 kHz case.
- Blind PSS search reports one peak per N_ID_2 and drops those below the ratio gate; two cells > ~10 dB apart still give `n_alt == 1`.
- "SIB1 cache off for tests": `ISAC_SIB1_CACHE` already exists (default ON). No code change; the controller should export `ISAC_SIB1_CACHE=0` in test/replay runs (an existing variable, not a new one).

## Expected on-air effect `[HYPOTHESIS]`
Pinned starts with a seed beyond PBCH capture range (Vodafone 370 scans, 923 m2B 534 scans) lock within ~3 scans plus one ladder pass; 403's alias seed (2178) resolves via the +-7.04 kHz rungs;
SIB1 on WindTre 508 appears if the missed SS0 phase was the cause `[INFERRED]`; 3630 MHz locks the stronger of PCI 27/403 reproducibly with both listed.
Fields: `ISAC_ACQ_ESCALATE`, `ISAC_ACQ_LOCK_OFFSET delta_hz`, `SSB_INVENTORY`, `CSS0 interleave: ... SS0 slots`, SI-RNTI accept count.

## Risks for other fixes
`nr_pdcch_blind_monitor_rt.c` CSS0 interleave block (sense/ptrs/irc/phase agents touching the same function should rebase trivially); `defs_nr_UE.h` `nr_ssb_search_params_t` gains fields (appended at the end).
A failed pinned cell that is absent now spends up to 15 hypotheses per scan after the third failure.
