# TIMBIMODAL (2026-10-11): why TIM PCI 923 OTA runs are bimodal, and a CRC gate on cell-wide solver state

Host: DGX GB10 (aarch64), worktree `rr-csd-tbm`, branch `csd/tim-bimodal` (from `csd/otatest` 953e219b71). Fix commit
501cf6d1ac. Every build, unit test and replay went through `rr-orchestration/heavy.sh`, one at a time. No radio, no captures.
OTA logs read: `/home/nicola/NICOLA/ota_runs/ab_T{s1_1_093120,s2_1_094205,r1_1_093643,r2_1_094729}` (2026-10-11) and
`ab_Tq*` (2026-10-10). Results are kept per run, per cell and per capture; nothing is merged.

## 1. What differs between good and bad runs

Good = Tr1 (26 CRC-OK RNTIs), Ts1 (28). Bad = Tr2 (0), Ts2 (9). Same build, same envs (t26r / t26s).

Ruled out **[MEASURED, OTA DGX 2026-10-11, Tr1 vs Tr2 logs]**:
- Sync, SIB1, CSS0, Point A: identical (PCI 923, the same CSS0 autoconf line, SIB1 decoded, offsetToPointA = 38, the same
  CORESET bank add `offset=0 span=216`). TRACKLOCK/TSYNC_OBS drift and ANTPOW/RFPOW agree within normal spread.
- PHY quality: the SI-RNTI PDSCH decodes in the bad run too (Tr2 `EVMGRANT rnti=0xffff crc=1 snr=6.9`; Tr1 runs give UE
  p50 7.0 dB). The 0.9-2.5 dB "post-eq SNR" of Tr2 comes only from the full decodes of one wrongly solved RNTI (below).
- DM-RS identities: the per-UE ids ARE in the bad run's hypotheses. RNTIs present in both runs, 11 min apart:
  0x5c34 -> 877, 0x49ec -> 323, 0x5bac/0x4ff2 -> 1159, 0x4f0c/0x501e -> 368. Tr1 SOLVED each with (that id, data 923);
  in Tr2 the DMRS_SCAN top-2 of 0x5c34 is (877, 2735) from 9 s on (206 voted slots), of 0x49ec (323, ...), and they never
  solve (0x5c34: 674 grants, 0 full decodes). So "wrong ids locked early" is not the cause.
- CSI-RS discovery: Tr2 has one wrong confirmed map (row 16, P/O 80/48) and one CONFIG_EPOCH at ~92 % of the run; CSI-RS slots
  are 2 of 40-80 slots and the solver does not search on them, so this cannot zero every RNTI. GPU partition: same in both runs.
- Queue: both runs drop 20-45 % stale jobs in the first ~25 s (Tr1 1887/2646, Tr2 5034/5749, Ts1 2323/3620), then run with no
  stale drops. Not discriminating.

What does discriminate **[MEASURED, OTA logs]**:

| run | first SOLVED (PDSCHQ report idx / total) | first solution | RNTIs solved | CRC-OK full decodes |
|---|---|---|---|---|
| Ts1 | 1 / 14 | add_pos 1 table 1, id 1289/923 | 33 | 3623 |
| Tr1 | 2 / 13 | add_pos 1 table 1, id 1660/923 | 26 | 4814 |
| Tq1_231425 | 2 / 13 | add_pos 1 table 1 | 11 | 1593 |
| Tq2_231945 | 3 / 13 | add_pos 1 table 1 | 16 | 2630 |
| Ts2 | 4 / 14 | add_pos 1 table 1 | 10 | 740 |
| Tq1_230208 | 4 / 13 | add_pos 1 table 1 | 7 | 520 |
| **Tr2** | 4 / 14 | **add_pos 2, table 2, S 0, scr 2735 (= DM-RS and data 2735), 0x47f0** | **1** | **0** |

Solves cluster right after the first one (Tr1: 1 solve in report 2, 6 by report 3, 13 by report 4): the first solution is
the cell prior / REUSE source that the other RNTIs try first, and they then only need their own identity. Outcome follows
when the first correct solve happens; Tr2's first (and only) solve was wrong.

## 2. Root cause of Tr2 (0 CRC-OK)

- **[MEASURED, OTA Tr2]** 0x47f0 was SOLVED (line 28809, ~ 1/4 into the run) "by 2 independent CRC passes" on add_pos 2,
  MCS table 2 (64QAM-LowSE, the lowest code rates), S 0, scr 2735. All TIM solutions in every other run are add_pos 1,
  table 1, S 1, data id 923. Its full decodes then gave 0 CRC-OK in 256 (`full0/256`, post-eq SNR 1.7-2.2 dB) and the solver
  reopened it (line 36557). The run had 6 probe passes in ~57k probes and 1 CRC-OK PDSCH in total (the SI-RNTI).
- **[HYPOTHESIS]** the two passes were chance CRC passes: tiny single-code-block TBs carry a 16-bit CRC (38.212 7.2.1),
  ~2^-16 per probe, and a busy RNTI (0x47f0 had 10626 decode jobs, 23 % of the run's) accumulates them on one candidate.
- **[CODE-READ, nr_pdsch_solver.c @953e219b71]** at SOLVED the solver sets, once and forever, (a) the cell prior
  `g_have_prior` (layout, add_pos, table, S map) and (b) the CORESET scrambling `g_key_scr`. The reopen path
  ("never decoded a full TB in 256 grants") clears only the RNTI's own `solved` flag, never (a) or (b). With
  `ISAC_PDSCH_SOLVER_REUSE=1` the reuse source is "the solved RNTI with the most full CRC passes", which accepts one with 0.
  So from line 28809 every unsolved RNTI's plan started with 0x47f0's wrong layout (REUSE while solved, then the stale prior
  after the reopen, to the end of the run), and RNTIs without own scan ids used scrambling 2735 on 3 grants in 4. Under the
  probe bucket (mean ~4 probes per grant in Tr2, often 1-2 under contention) the prior probe is emitted before the search.
- **[OFFLINE VERIFIED, DGX unit test]** `OnlyACrcProvenSolutionSeedsTheCell` reproduces it with the flag off: after a
  2-pass solve on add_pos 2 and its reopen, another RNTI's first probe (budget 1) is still the add_pos-2 prior.

Why the bad runs are late to their first correct solve (Ts2, Tq1_230208, and Tr2's first 25 %) is **not** established:
without `ISAC_PDSCH_SOLVER_PROBELOG` on OTA the per-probe hypotheses are not visible. It is a lottery on which RNTI's search
lands first, and it decides how fast the population is bootstrapped.

## 3. Fix (default off): `ISAC_PDSCH_SOLVER_PRIOR_CRC=1`

A solution seeds other RNTIs (cell prior, CORESET scrambling, REUSE source) only after it decoded a full TB with CRC OK
(`promote()` on the first `full_ok`). Two probe passes still solve the RNTI itself; only the cell-wide state waits for CRC
proof. General: no cell constant, no threshold beyond "one full TB CRC". A false solution never decodes a full TB (Tr2:
0/256), so it never becomes cell state; a real one decodes within a few grants, so the bootstrap is barely delayed.
`ISAC_PDSCH_SOLVER_REUSE` is now re-read after `nr_pdsch_solver_reset()` (test only; same runtime behaviour).

## 4. Validation

**[OFFLINE VERIFIED, DGX unit test via heavy.sh @501cf6d1ac]** `test_nr_pdsch_solver` 16/16 (new
`OnlyACrcProvenSolutionSeedsTheCell`: flag off reproduces the stale prior while solved and after the reopen; flag on
the search goes first; control: a solution with a full CRC pass does seed the cell). `test_nr_pdsch_dmrs_scan` 5/5.

**[MEASURED, replay DGX GB10, @501cf6d1ac, timul_replay.sh, ENVFILE=ota/t26s.env, 4 RX, speed 0.25; base = same binary,
flag off; fix = + ISAC_PDSCH_SOLVER_PRIOR_CRC=1]** solved / CRC-OK RNTIs / CRC-OK full decodes per run, 2 runs per arm:

| capture | base | fix |
|---|---|---|
| t12 raw_pci923_tim_12s_tile3 (TIM) | 5/4/171, 6/5/241 | 7/5/349, 5/5/201 |
| t4 raw_pci923_tim_4s_1010_1853 (TIM) | 1/1/1, 2/1/1 | 1/1/1, 1/2/2 |
| vb raw_pci415_12s_b (Vodafone) | 29/27/2058, 19/18/1398 | 29/29/1707, 30/30/2039 |
| v8 raw_pci415_12s_20261008 (Vodafone) | 4/4/625, 4/4/694 | 4/4/288, 4/4/661 |
| p421 raw_pci421_aligned_20261006 | 3/3/36, 0/0/0 | 4/3/34, 0/0/0 |

All 20 runs reached EOF, no crash. No replay had a false solve (0 reopens), so the bad mode did **not** reproduce on these
captures and the fix is neutral on them, as expected. The low v8 fix run (288) is upstream PDCCH variance, the same signature
SOLVERCOVER reported: 0xe01e 60 DCIs against 598-612 in the other three v8 runs; the flag acts only after a PDSCH solve.
vb base run 2 acquired late (23 planned RNTIs against 40). p421 run 2 solved nothing in either arm (late acquisition, seen
in SOLVERCOVER too).

## 5. OTA env delta

Next TIM / Vodafone OTA: `t26s.env` + `ISAC_PDSCH_SOLVER_PRIOR_CRC=1` (no other change). Optional diagnostics for the
still-open "late first solve": `ISAC_PDSCH_SOLVER_PROBELOG=1 ISAC_PDSCH_SOLVER_WHY=1` (log volume grows).

## 6. Risks and open points

- **No OTA validation, and no replay reproduction** of the bad mode; the effect on Tr2-like runs is
  **[CODE-READ + unit test]** only. It removes the lock-in, it does not make the first correct solve come earlier.
- The late-first-solve lottery (Ts2: 9 RNTIs) is not addressed. Candidates: PROBELOG on OTA; a stronger solve rule for
  CRC-16 TBs (a third pass, or full-TB confirmation before `solved`).
- A solution promoted by one genuine full CRC pass stays the prior even if that RNTI's later decodes fail; same as today.
- `g_key_scr` (first solution's scrambling for RNTIs without own scan ids) is per-UE on TIM by design and therefore wrong
  for other UEs there even when CRC-proven; unchanged here (scan-id RNTIs bypass it).
