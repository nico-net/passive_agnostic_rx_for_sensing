# On-air A/B: ISAC_RA_TDRA_SWEEP (fix 9db7f4b113), 2026-10-16 evening

Runs 19:56-20:21 CEST (Friday EVENING load; do not pool with the afternoon runs). Interleaved, n=2 per arm per cell, 150 s, binary csd/int16 build_t26 at bf8179c4b2.
Arms: X = t26aXE (all-on), R = t26aXRE (= X + ISAC_RA_TDRA_SWEEP=1). Runner rr-orchestration/ota/run_r_20261016.sh. All 8 runs synced, SIGINT exit 130.
Label: [MEASURED, DGX OTA 2026-10-16 evening, cell, arm, n=2]. Tools: goal_metrics.py, health_grid.py.

## Target class (RAR / TC-RNTI with the default TDRA row)
| cell | arm | RATDRA ok=1 | ok=0 | nsym-9 class (tbl0 mcs0 rb25-51) crc_q |
|---|---|---|---|---|
| V | X | - | - | 0/10, 0/19 (0 %) |
| V | R | 130, 348 | 1, 2 | 5/5, 5/5 (100 %) |
| T | R | 0 | 0 | not triggered (T nsym 9 is another cause: dead slots 3/13) |
V: RA-RNTI 0x007f re-decodes S=2 L=12 -> S=1 L=13 in 432 of 435 tries; the same S=1 L=13 also rescues a few TC/C-RNTI grants (cls=1). The fix works on air and is general (it is triggered only by a failed common-SS DCI 1_0).

## Per-run scorecard (DL)
| run | n_real | tot p99 us | crc_q % | drops % | recovered |
|---|---|---|---|---|---|
| VX1 | 24367 | 13698 | 92.9 | 0.3 | 5225 |
| VX2 | 24379 | 17286 | 91.7 | 3.5 | 4221 |
| VR1 | 38286 | 14353 | 92.6 | 0.7 | 8392 |
| VR2 | 26452 | 13479 | 89.4 | 0.5 | 4648 |
| TX1 | 22966 | 5585 | 96.8 | 0.1 | 0 |
| TX2 | 28517 | 5170 | 95.0 | 0.1 | 1 |
| TR1 | 33604 | 6777 | 95.8 | 0.3 | 4 |
| TR2 | 15036 | 5807 | 91.9 | 0.1 | 4 |
UL: crc ~0 in all runs (ULCAUSE unchanged).

## Reading
- The target class goes 0 % -> 100 % on air (V); no CRC regression on either cell (T R inside the X spread; V crc_q 89-93 both arms). Latency and drops differences are within the n=2 noise.
- Recommend: turn ISAC_RA_TDRA_SWEEP=1 on in the next default env. Follow-up (planned): learn the S once per cell instead of retrying each time.
- Evening numbers are better than the afternoon (V crc_q 89-93 vs 84-91 rescued; T p99 5.2-6.8 ms vs ~8 ms); load-dependent, not a regression signal.
- Still failing G1 on latency everywhere (p99 5-17 ms); V rescues still large (4-8k per run, rescued p99 ~27-30 ms).
