# DL CRC confirming replays, 2026-10-16 16:30-16:37 (X410 unplugged)

p99_replay.sh, env t26aXE, binary csd/int16 build_t26, SPEED=1, 90 s, 2 runs per arm. Captures: V = raw_pci415_12s_b, T = raw_pci923_tim_12s_tile3 (older captures, not today's air). Label [MEASURED, DGX replay <cap>, t26aXE, n=2]. Per-slot script: session scratchpad replay_slots.py (qualification = goal_metrics corrected rule, +2 dB). Never mixed with OTA numbers.

| replay | all DL CRC | qualified CRC | slots 1-3/11-13 | slots 0/6/10/16 | T s8/18 even | T s8/18 odd | T s3/13 nsym11 |
|---|---|---|---|---|---|---|---|
| V retry=0 #1 | 4911 / 45 % | 1307 / 84 % | 440 / 74 % | 234 / 90 % | - | - | 106 / 65 % |
| V retry=0 #2 | 3819 / 43 % | 1033 / 75 % | 364 / 65 % | 168 / 92 % | - | - | 93 / 65 % |
| V retry=1 + CSIFULLAB #1 | 3485 / 49 % | 1125 / 78 % | 421 / 68 % | 174 / 93 % | - | - | 108 / 65 % |
| V retry=1 + CSIFULLAB #2 | 4052 / 45 % | 1131 / 83 % | 385 / 74 % | 196 / 91 % | - | - | 109 / 57 % |
| T CSIRS_FULL=0 #1 | 3806 / 67 % | 365 / 78 % | - | - | 9 / 0 % | 13 / 69 % | 5 / 0 % |
| T CSIRS_FULL=0 #2 | 5627 / 65 % | 570 / 74 % | - | - | 19 / 0 % | 24 / 75 % | 9 / 0 % |
| T CSIRS_FULL=1 + AB #1 | 5076 / 65 % | 581 / 71 % | - | - | 17 / 0 % | 24 / 71 % | 6 / 0 % |
| T CSIRS_FULL=1 + AB #2 | 4728 / 66 % | 559 / 71 % | - | - | 14 / 0 % | 24 / 71 % | 5 / 0 % |

## Findings
- Both on-air patterns reproduce offline on older captures: V slot-dependent loss (65-74 % in slots 1-3/11-13 vs 90-93 % in slots 0/6/10/16) and T dead bins (even-frame slots 8/18 and slots 3/13 with nsym=11: 0 %; odd-frame 8/18: 69-75 %). Both are now offline test cases.
- T: ISAC_CSIRS_FULL on or off changes nothing in the dead bins, and CSIFULLAB logged no line for T -> the periodic signal is NOT in T's CSI-RS bank at all; the fix needs discovery of that periodic resource (or erasure of the dead REs), not the existing full rate-matching.
- V TDRA retry: DECODETDRA recovers 334 / 341 grants per replay (before=0 after=1) vs 0-1 with retry=0, but the logged DL CRC moves only ~+3 points (45/43 -> 49/45 %): EVMGRANT apparently logs the first attempt, so recovered grants are not credited in the scorecard (to verify in code).
- V CSIFULLAB on failed grants (223 tests with results, both replays; each field = <grant S, toggled S>, 1 = CRC pass):
  - 136 (61 %): no hypothesis decodes (none/own/all CSI-RS sets x both S);
  - ~67 (30 %): decode with the grant's own S when NOT rate-matching around all CSI-RS (none=10 / own=10, all=00 in 36 of them) -> the normal path over-rate-matches (CSI-RS REs assumed where there are none) for these grants [INFERRED];
  - ~13 (6 %): decode only with the toggled S (TDRA).
- So V's ceiling is a mix: wrong CSI-RS rate-matching set (~30 % of failures), TDRA (~6 % in AB, plus the 334 retry recoveries), and a majority (61 %) not explained by CSI-RS set or S: still open (candidates: other unmodelled REs, PDCCH/own-CORESET RE overlap, layer/port mapping, DM-RS config).
