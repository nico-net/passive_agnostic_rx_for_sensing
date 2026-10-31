# fix1031/sense — S3..S6 and the P3 gate (2026-10-31)

Branch `fix1031/sense` off `mpci-int-1031`. No new flags, env vars or -D switches. No receiver, replay or OTA was run.

## What changed
- **S3 receiver state per record.** `nr_sense_records.{h,c}`: per-cell slot ring (`NR_CELL_STATIC rx_st`, 1024 slots) holding sample counter, applied CFO, cumulative window shift and shift count. `executables/nr-ue.c` (after `nr_rx_read_slot`) notes each slot (`nr_sense_rx_note_slot`, rx_ts = read start minus first-symbol samples = slot start; CFO = digital `dl_Doppler_shift+freq_offset` when `cont_fo_comp`, plus `nrue_ru_radio_fo()`) and the frame-boundary window step (`nr_sense_rx_note_shift(-iq_shift_to_apply)`, positive = window later). RX gain is noted inside `nr_sense_emit_session`. `nr_sense_emit_rs` looks the record's slot up and fills `rx_cfo_applied_hz`, `rx_shift_cum_samples`, `rx_shift_count`, `rx_gain_db` (null when the slot was never noted). `cfo_est_hz` (per-UE residual estimate) stays null.
- **S4 slot clock.** SSB (`phy_procedures_nr_ue.c`) and CSI-RS (`csi_rx.c`) use `nr_sense_abs_slot_hfn(hfn_rx, frame_rx, slot)`; DL DM-RS (`nr_pdcch_blind_monitor_rt.c`, exported slot only, internal keys unchanged) uses `nr_sense_abs_slot_align(source_absolute_slot, ...)`. All sources now share the producer's un-wrapped clock (pdsch_data and UL already did). DL `rx_ts` comes from the ring (also for grant records).
- **S5 metrics.** `ISAC_METRICS`: `sense_cfr_exported_<src>`, `sense_cfr_drop_<src>` (new `cfr_pushed_src[]` in `nr_sense_stats_t`), `sense_unexported_pdsch_data` = pdschq_crc_ok minus exported pdsch_data frames, `sense_unexported_pusch_data` likewise (upper bounds: CRC-ok TBs that never produce data-aided CFR, e.g. multi-layer, SIB/RAR).
- **S6.** `nr_sense_noise_est.h`: PUSCH data-aided `noise_var` measured from the second difference of H over adjacent REs (was 1.0f; NaN if no triple). DL DM-RS, PDSCH data and PUSCH data records now stamp `ports`/`port_mask`/`dmrs_type`/`dmrs_n_scid`; the JSON no longer nulls these for DL.
- **read_sense.py**: `rx_state_report`, `coherence_check` (phase slope of H2/H1 per stream vs. the slope 2*pi*d/N predicted by the recorded window step; flags ok / window_shift / break / no_state), CLI `--coherence`, `--n-fft`.
- **P3 gate** `tests/passive_rx/dgx/score_rx.py`: `GATE_MODE=solver` (default via `auto`; old CONVERGED-only logs still go to postconv; postconv/legacy kept): n_solved, ttc to first `PDSCH_SOLVER rnti=.. SOLVED by`, CRC and decoded count after the last SOLVED (>= 99.8 % over >= 5000), `reopening` lines after the last SOLVED == 0, crc_floor, drop_full <= 2.5. `GATE_SOLVE_FIRST_MAX=60` s is PROVISIONAL (33.9 s was seen on rfsim); `rfsim_regress.sh` header documents it. `mpci_statics.tsv` gets the `rx_st` row.

## Tests `[OFFLINE VERIFIED, DGX aarch64 unit]`
- gtest `test_nr_sense_records` (hyper-frame helpers, ring/shift semantics, JSON fields, noise estimator) and `test_nr_passive_metrics` (new fields); see final ctest line in the hand-back.
- `python3 -m unittest test_score_rx` (26 incl. 8 solver tests on `fixtures/solver_synth_rx.log`, `[SYNTHETIC]`), `test_read_sense_rxstate` (6).

## Not verified
Any on-air behaviour; the 60 s solve bound; CFO/shift sign conventions against a real capture; GPU-pass path of SSB/CSI-RS (only the CPU producers were touched). `test_static_audit` also reports `g_ul_pin_seen_epoch` (nr_pusch_passive_decode.c:1725), present on the base, not mine.

## On-air expectation `[HYPOTHESIS]`
Every rs record carries receiver state; abs_slot is monotonic across 10.24 s; a coherence break is visible exactly at `rx_shift_count` steps. Watch `rx_*` record fields, `sense_unexported_*`, `sense_cfr_drop_*`.

## Risks for other fixes
`nr-ue.c` (near the slot read), `nr_pdcch_blind_monitor_rt.c` (DM-RS submit), `nr_pusch_data_aided.c`, `nr_pdsch_data_aided.c` meta initialisers, `mpci_statics.tsv`.
