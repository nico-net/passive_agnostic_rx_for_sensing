# Fix B: CSI-RS rate-match set validated by CRC (ISAC_CSIRS_FULL_VALID=1), 2026-10-17

Sonnet agent; saved by the orchestrator. Commit c1f22163a6 on csd/int16-fixb. Flags default off: ISAC_CSIRS_FULL_AB_EVID=1|2|3 (diagnostic), ISAC_CSIRS_FULL_VALID=1 (needs ISAC_CSIRS_FULL=1).
Verdict: PARTLY MET. TIM even-frame 40/8 dead slots (abs%80 {8,48}) 0 % -> ~55 % (neighbours ~82 %); {18,58} and B' {16,17} stay 0 %. Vodafone unchanged. Replay only.

## Step 0 (decisive) [MEASURED, DGX replay tim, t26aXE + AB=1 EVID, n=1 per mode]
- Every baseline hypothesis (none/own/range/all) passes 0 grants in dead bins.
- all_ev: mode 1 25/110, mode 3 55/120 (phases 8/48/88/128 mod 160: 15-27 of 34-50 each), mode 2 (everywhere) 24/123 (regresses other slots).
- Mode 3: unconfirmed winners are overlapping aliases of one 40/8 pattern (rows 4/6/11/12, l0 8,9,12,13, rb 0:206-215); only the union passes; each alone fails.
- abs%40=18: no unconfirmed evidence in these replays. B' {16,17}: rate-matching the confirmed NZP 80/16,17 gives 0/24 per phase -> the id rule is not the (only) cause of B'.

## Design (steps 1-2)
Per phase (slot%160), until proven, each eligible grant is decoded twice on the same samples (rule set vs rule set + candidates = withheld confirmed NZP + unconfirmed evidence in that slot, DM-RS clashes removed). "alive" after 3 CRC-OK with the rule set; VALIDATED after 3 CRC-OK with candidates (and more than the rule); "gaveup" after 24 trials. Validated phases rate-match the candidates in nr_csirs_full_apply and no longer erase them (CSIFULLVAL lines). Trial decodes run with t_quiet (an un-quiet first version corrupted shared state: b CRC 56 -> 42 %). Id rule kept as seed; no per-cell constants.
Files: nr_csirs_blind_rt.c/.h, nr_pdsch_passive_decode.c, tests/nr_csirs_blind_rt_test_bridge.c, tests/nr_csirs_blind_search_test.cc.

## Unit test
test_nr_csirs_blind_search 139/139 incl. CsirsFullValid.CandidatesFillTheEmptyIdRuleSetPhaseGatedAndClashFree (rule set empty for dmrs 1677 / cell 923; candidate set 1 at slot 8, 0 at slot 9; DM-RS clash filtered; pop-923 grant keeps its rule set). The CRC state machine is covered by replays only.

## Replay (probe=0, any CRC OK per (rnti, abs), by abs%80)
tim [MEASURED, DGX replay tim, t26aXE + DECODE_DIAG, n=2 per arm]:
| bin | off r1 | off r2 | VALID r1 | VALID r2 |
|---|---|---|---|---|
| 8 | 0/111 | 0/131 | 43.2 % | 75/131 (57.3 %) |
| 48 | 0/98 | 0/117 | 43.0 % | 64/117 (54.7 %) |
| 18, 58 | 0 % | 0 % | 0 % | 0 % |
| B total | 0/413 | 0/489 | 100/460 (21.7 %) | 139/493 (28.2 %) |
| A (abs%10==3) | 0 | 0.4 % | 0 | 0 |
| B' {16,17} | 0 | 0 | 0 | 1/218 |
| odd {28,38,68,78} | 84.8 % | 82.2 % | 82.2 % | 82.5 % |
| rest | 75.9 % | 74.0 % | 75.5 % | 74.3 % |
b (V) [MEASURED, DGX replay b, n=2 per arm]: rest 55.8/54.8 off vs 54.3/58.7 VALID; A 56.4/58.5 vs 55.9/61.7; {16,17} 71/78 vs 71/80. No degradation.

## Not met / next
{18,58}: no 40/18 evidence available. B': confirmed NZP rate-matching does not help. Next: resolve the true resource shape (rows 16/17 l0=8 l1=12) and the 40/18 phase; for 16/17 test other mappings (density, CDM, REs inside the DM-RS set) with the same CRC tool. Cost: two decodes per grant in unproven phases until validated/gave up (bounded at 24 trials per phase).
