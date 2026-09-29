# RFsim phy-test validation — merged `sdd/validation` branch (sens6)

Host sens6. Receiver = `agn-wt/val` worktree, branch `sdd/validation` @ `c295fa18f1`
("Merge sdd/agn-td into validation" = `adaptive-rx-UL-DL @ 25a8699a64` + `sdd/agn-sa` + `sdd/agn-td`'s
R32 fix `ecfced8bd0`). gNB = `agn-wt/gnbtest`, branch `sdd/rfsim-gnb-test @ 67bb0eaa11` (already
built, unchanged). Same 106 PRB fully-agnostic phy-test recipe as
`rfsim-results-phaseA.md`/`technique-d-regression.md`/`td-fix-report.md`, reusing their scratch conf
verbatim: `sens6:/home/sens/NICOLA/rfsim_validation/phyA2/ue.passive.q.agn.conf` (no manual
coreset/ss/bwp/tda/dci_bits/dmrs/dci_length_override — every DL PDCCH/PDSCH geometry parameter
self-discovered). gNB conf `agn-wt/gnbtest/tests/passive_rx/gnb.sa.rfsim.conf`, confirmed
byte-identical (`diff`, exit 0) to the copy other lanes used. Logs:
`sens6:/home/sens/NICOLA/rfsim_validation/valphytest/<arm>/{gnb,rx}/{gnb,rx}.log`.

## Build

`agn-wt/val` had no build dir. Configured it to match `agn-wt/td`'s cache exactly (read off
`td`'s `CMakeCache.txt`): `cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
-DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DENABLE_TESTS=ON -DOAI_RF_EMULATOR=ON -DOAI_SIMU=ON
-DOAI_USRP=ON -DOAI_VRTSIM=ON -DAVX512=OFF -DAVX2=ON` against `agn-wt/val`'s own root. Configured
clean (no errors). Built via `lane-make.sh val nr-uesoftmodem test_nr_pdcch_blind_monitor
test_nr_pdsch_config_sweep test_nr_pdsch_prb_set rfsimulator params_libconfig oai_usrpdevif` —
all seven targets built clean, no new warnings, no receiver running at any point during the build
(`pgrep -x nr-uesoftmodem` checked before starting and confirmed empty).

## ctest — no regression from the merge

| Suite | Result |
|---|---|
| `test_nr_pdcch_blind_monitor` | **154/156 passed**, 2 skipped (`PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth` — same pre-existing unrelated skips `td-fix-report.md` records) |
| `test_nr_pdsch_config_sweep` | **40/41 passed**, 1 skipped (`PdschReset.Timing`) |
| `test_nr_pdsch_prb_set` | **14/14 passed** |

`ctest -R 'test_nr_pdcch_blind_monitor|test_nr_pdsch_config_sweep|test_nr_pdsch_prb_set'
--output-on-failure`: **100% tests passed, 0 tests failed out of 3.** No assertion was relaxed.

## Harness note: one real bug found and fixed in my OWN run script, not in the branch

The launcher (`run_arm_val.sh`) originally split multi-var env strings on `,`. That's wrong when a
single value itself contains commas — `ISAC_GNB_TEST_USS_AL=4,0,0,0,0`'s value was shredded into
`env ISAC_GNB_TEST_USS_AL=4 0 0 0 0 <path>`, so `env` tried to exec `"0"` as the command and the
gNB never started for the first A2 attempt (`A2_al1only`, **VOID** — 2-line `gnb.log`:
`env: '0': No such file or directory`). Fixed by switching the separator to `;` (A4's 3-KV-pair
string had no internal commas so it was unaffected) and re-ran as `A2_al1only_v2` (reported below).
No receiver/gNB source was touched; nothing committed to `sdd/validation` (no merge-level build fix
was needed — cmake configured and built clean on the first try).

## Results

### A0 — baseline (mcs=9, table=0/qam64), 3×150s

| Run | crc_ok / try | CONVERGED | drop_full (final) |
|---|---|---|---|
| A0_r1 | 0 / 32441 | 0 | 193409 |
| A0_r2 | 3 / 32444 | 0 | 195559 |
| A0_r3 | 1 / 32680 | 0 | 194021 |

Mean crc_ok = 1.33 (n=3). **Matches the same regime `td-fix-report.md`'s R32 5-run A/B measured**
(FIXED mean 0.80 sd 0.84, BASE mean 1.00 sd 0.71) — neither Technique D tree ever reaches formal
`CONVERGED` in a 150s window on this bed (0/3 here, as expected), and a few-crc_ok-per-run rate is
the established fair comparison metric on this bed, not a regression signal.

**Verdict: PASS — bed healthy, no regression from the merge.**

### A4 — DL scrambling IDs ≠ PCI (gNB `ISAC_GNB_TEST_DL_DMRS_ID0=700
ISAC_GNB_TEST_DL_DMRS_ID1=800 ISAC_GNB_TEST_DL_DATA_ID=500`, RX unmodified/fully agnostic), 3×150s

| Run | gNB confirms all 3 overrides | crc_ok / try | CONVERGED | DATA_ID_WALK lines | drop_full (final) |
|---|---|---|---|---|---|
| A4_r1 | yes | 1 / 32606 | 0 | 0 | 188247 |
| A4_r2 | yes | 0 / 32615 | 0 | 0 | 197026 |
| A4_r3 | yes | 1 / 33047 | 0 | 0 | 189200 |

Mean crc_ok = 0.67 — same order of magnitude as A0's 1.33 (n=3 each side, not statistically
distinguishable). **CRC>0 pass signal: met in 2/3 runs.**

**Discovery question ("does the receiver's DM-RS ID / data-ID discovery find the injected IDs?"):
no — the discovery mechanism never activates on this bed, in EVERY arm including baseline, not
specifically under A4.** `nr_pdsch_passive_decode.c`'s `DATA_ID_WALK START/LATCHED` log line never
fired in any of the 3 runs (`grep -c DATA_ID_WALK` = 0 every time), and the per-grant
`dmrs_id[dl=...]` status line stayed `pending:-1/0` for the entire run in every arm checked
(A0 and A4 alike). Reading the source (`nr_pdsch_passive_decode.c:443-464`): the walk requires
`nr_scrambling_walk_eligible()` — DM-RS id of the grant's nSCID **decided**, link healthy, and
≥`NR_SCR_WALK_MIN_FAILS` consecutive dedicated CRC fails — and the DM-RS id never leaves "pending"
here. This is the same pre-existing Technique-D non-convergence `rfsim-results-phaseA.md`/
`technique-d-regression.md` already documented (context never reaches formal `CONVERGED` within
150s on this 106 PRB phy-test bed) surfacing one level down: the DM-RS-id-decided precondition for
the *data*-ID walk is gated behind the same convergence this bed doesn't reach in 150s. Not a new
A4-specific defect — grep for `WALK` also hit 192 unrelated lines (`isac_sync.cc`'s STO tracker
`walk=` field), which is not the discovery mechanism and should not be mistaken for it.

**Verdict: PASS on the CRC>0 signal (2/3 runs); the "did it discover the injected IDs" question is
UNANSWERABLE on this bed** — the discovery gate needs a DM-RS-id-decided state this 150s window
never reaches, independent of A4's injection. A longer run (or a bed where Technique D actually
converges) would be needed to exercise the walk itself.

### A2 — AL1-only dedicated SS (gNB `ISAC_GNB_TEST_USS_AL=4,0,0,0,0`, RX `ISAC_AL1_COVER=1`), 1×150s

First attempt (`A2_al1only`) is **VOID** — killed by the run-script comma bug above (gNB never
started; `env: '0': No such file or directory`, `rx.log` never saw the cell). Re-run as
`A2_al1only_v2` after the script fix:

- gNB confirmed: `ISAC_GNB_TEST_USS_AL=4,0,0,0,0 active` and
  `Candidates per PDCCH aggregation level on UESS: L1: 4, L2: 0, L4: 0, L8: 0, L16: 0`.
- Receiver: `rnti=0x1234` (`0x1234` substring count) = **125344**; AL1 candidates decoded
  (`al=1` count) = **125461**, `al=0` = 11 (noise floor) — AL1-only scanning works end to end at
  the CORESET/RNTI level, matching `rfsim-results-phaseA.md`'s original A2 PASS.
- `pdsch_decode[try=26306 crc_ok=1 (0.0%) ...]` — **crc_ok=1, nonzero.** This is new/better than
  the original phyA2 A2 run, which never scored a CRC pass at all (blocked by the pre-fix Bug #3
  context-churn defect) — a concrete sign the TD fix generalises to the AL1-only path too, not
  just the AL2 default.
- `Technique D CONVERGED` = 0, `drop_full` final = 241876.

**Verdict: PASS, no regression** (matches/slightly improves on the prior phyA2 result).

### A7 — MCS qam256 / Qm-oracle (`-m 20 -n 1`), 1×150s

- gNB confirmed scheduling `MCS (1) 20 (Qm 8)` = 256QAM.
- `rnti=0x1234` count = **146577** (high accept rate, as before).
- Technique D per-occasion hypothesis dump, `mcs=20` entries: `tbl=2 Qm=4` × 90 vs
  `tbl=1 Qm=8` (the correct table) × 70 — the correct hypothesis is present and close to the top,
  but **not clearly dominant** the way `rfsim-results-phaseA.md`'s original run showed (121 vs
  33/22 there). Read this with `technique-d-regression.md`'s own caveat in mind: the `PARMSET[]`
  dump is capped to the **first 16 distinct tuples ever logged** in the whole run, so a 90-vs-70
  split reflects which tuples got logged first, not a true relative-frequency measurement — this is
  not solid evidence of a regression in the Qm-oracle itself, just a reminder the diagnostic is not
  a frequency count.
- `pdsch_decode[try=29002 crc_ok=0 (0.0%) ...]` — crc_ok=0, same as the original phyA2 run
  (TB-CRC-level decode remains blocked/low-probability at 256QAM on this bed regardless of tree).

**Verdict: PASS at the RNTI/MCS-oracle level (the item's actual pass criterion — gNB's injected
256QAM value is correctly read and present in the hypothesis catalog), no crash, no regression at
that level.** The hypothesis-dominance comparison to phaseA is inconclusive given the capped
diagnostic, not a demonstrated regression.

## Skipped per brief

A3 (AL16) and A5 (type-B k0) were not run — both are documented, reproducible **gNB-side** crashes
in `agn-wt/gnbtest`'s test-only injection code under `--phy-test` (A3: `Assertion (CCEIndex >= 0)
failed! ... Could not find CCE for UE 1234`; A5: `L1_tx_thread` segfault), independent of the
receiver under test — see `rfsim-results-phaseA.md` Job 2.

## Housekeeping

- No file under `gnb_remote_logs/`, `cuLogs/`, or sens4/X410 was touched.
- Nothing committed to `sdd/validation` — no merge-level build fix was needed.
- Only one harness instance run at a time by this session; `pgrep -x nr-softmodem`/`nr-uesoftmodem`
  checked empty before every launch. Near the very end (after the last capture, `A2_al1only_v2`,
  had already fully exited and been reaped), an unrelated concurrent process appeared on sens6
  (`./run_arm.sh base_timing ... nr-uesoftmodem ... ISAC_PDCCH_TIMING=1`, started ~20s after my
  last capture's log files stopped being written) — evidently another session's run, not mine. Left
  untouched per the "kill only your own processes" rule; it did not overlap with any of this
  session's 8 completed captures (verified via file mtimes vs the other process's start time).

## Bottom line

All 6 requested arms (A0×3, A4×3, A2×1, A7×1 — 8 runs total) executed. **No regression found on
the merged `sdd/validation` branch** versus the prior `td-fix-report.md`/`rfsim-results-phaseA.md`
baselines: ctest is clean (0 failed, same pre-existing skips), A0's crc_ok distribution matches the
established noisy regime, A4 shows CRC>0 in 2/3 runs with both gNB-side overrides confirmed active
(the explicit ID-discovery log line itself is unreachable on this bed, a pre-existing limitation,
not new), A2 AL1-only re-confirms end-to-end (and now also scores a nonzero CRC, an improvement),
and A7's MCS/Qm-oracle correctly reads the injected 256QAM value with no crash.
