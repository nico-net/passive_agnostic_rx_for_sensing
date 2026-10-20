# Latency knobs A/B on t26bE, DGX replay, 2026-10-17

Sonnet agent; saved by the orchestrator. Setup: csd/int16 108faf65b1, build_t26, p99_replay.sh CAPSET=b and tim, TMO=90, 4 RX, interleaved, same knob values for both cells. Baseline ota/t26bE.env. Label [MEASURED, DGX replay <cap>, t26bE + diff, n=2-4]. Env files/scripts: session scratchpad knobs/ (mk.py, run.sh, score.py, index.txt).
Result: no existing-knob set meets acceptance (crc_q not lower, recovered/drops not higher, tot p99 and scan p99 lower on both caps). Keep t26bE unchanged.

Arms: P probes halved (RATE 0.25, FILL 12, BURST 8, BUDGET 12, BACKOFF 100, DL_TRAFFIC_PROBE 32), split Pa (rate/fill/burst) / Pb (budget/backoff/traffic probe); R all five retry arms off; D (DED_BUDGET_US 150, NID_CAPTURE_EVERY 4, DISCOVERY_BUDGET_PCT 15, JOINT_ANON_RATE 0.5), split Da (DED_BUDGET_US 150) / Db (other three); PD = P + D.

| arm | n | b tot p99 | b crc_q | b rec | b rnti, min n_real | tim tot p99 | tim crc_q | tim rnti, min n_real |
|---|---|---|---|---|---|---|---|---|
| A0 t26bE | 4 | 4220 (3981-4567) | 96.7 | 22 | 30-31, 4327 | 2830 (2605-2938) | 74.1 | 13-14, 4057 |
| P | 4 | 4094 (3525-4309) | 96.2 | 22 | 19-35, 1633 | 2770 | 74.4 | 12-13, 2878 |
| Pa | 3 | 5502 | 96.8 | 23 | 19-30, 1671 | 2925 | 73.8 | 12-14, 4520 |
| Pb | 3 | 4893 | 96.2 | 22 | 23-31, 4622 | 3018 | 76.1 | 12-14, 5104 |
| D | 2 | 4548 | 96.0 | 31 | 22-29, 2614 | 2879 | 77.5 | 12, 3708 |
| Da | 3 | 4580 | 96.2 | 32 | 33-34, 4542 | 3018 | 75.5 | 1-13, 32 |
| Db | 3 | 3864 (3804-3879) | 95.6 | 22 | 22-27, 3016 | 2840 | 77.9 | 11-13, 3136 |
| PD | 2 | 4063 | 96.3 | 24 | 26-28, 3432 | 2251 (one failed run) | 88 | 1, 112 |
| R | 2 | 5262 | 93.9 (91.0-96.8) | 13 | 26-28, 3105 | 2686 | 73.5 | 12-13, 5067 |
Scan p99 (SCANTIME_FINE): b A0 1916-2642, P 1287-2119, Db 1497-1877, R 3002-3311; tim A0 1152-1603. Probe stage_s b A0 1.7-1.9 s, P 1.0-1.2 s. Drops 0.0 % except tim P4 3.56 %.

Reading:
- Probe reduction cuts probe GPU time (1.7 -> ~1.0 s) but tot p99 moves inside the A0 spread (~300 us) and costs layout discovery (fewer RNTIs, delayed first grants).
- Retries are not redundant after TDA_FIELD on V: off lowers CRC and lengthens the tail.
- Db lowers V p99 and scan p99 without CRC loss but does not help TIM and delays discovery.
- tot p50 flat (b ~1.5 ms, tim ~1.0 ms): GPU contention not set by these knobs. Reduced-probe/budget arms show intermittent discovery failures (~1 in 4-5 runs), A0 0 of 4.
Next lever (code): GPU priority for real-grant chest/LDPC/demod over probes and retries.
Not covered: retry arms not bisected (TDRA_RETRY also gates the retry predictor), RECOVERY_ASYNC/RX_RECOVER untested, no DPDK/USRP load, single 12 s captures.
