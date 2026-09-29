# Overnight gap-fix lanes (2026-09-27)

Read `context.md` in this directory first (rules, commit trailer, local-vs-sens6 paths). Deltas:

- Your lane worktree on sens6: `/home/sens/NICOLA/agn-wt/gap-<lane>`, branch `sdd/gap-<lane>`, forked from
  `sdd/validation` @ c295fa18f1 (= adaptive-rx-UL-DL + SA discovery fix + Technique D fix). Commit ONLY there.
- No build dir yet: replicate `/home/sens/NICOLA/agn-wt/val/cmake_targets/ran_build/build` (same cmake cache
  options/generator, new binary dir), then build with `/home/sens/NICOLA/agn-wt/lane-make.sh gap-<lane> <targets>`.
- Method: TDD. Write the failing gtest first (existing suites: test_nr_pdcch_blind_monitor,
  test_nr_pdsch_config_sweep, test_nr_pdsch_prb_set, test_nr_scrambling_id_sweep, …; add a new one only if needed).
  Root-cause fixes, not symptom patches. Opt-in `ISAC_*` knobs only if a behaviour must be switchable.
- Live runs on sens6 (phy-test bed, recipe in `rfsim-results-phaseA.md` / `rfsim-results-val-phytest.md`):
  ONE harness at a time on sens6. Before launching, `pgrep -x nr-uesoftmodem`; if anything runs, wait (Monitor,
  not foreground sleep). Kill only your own processes (pgrep -x, never pkill -f).
- Live validation with the OCUDU gNB happens on sensnuc3 in a separate lane — do NOT start radio processes on
  sensnuc3. If your fix can only be validated live with OCUDU, finish offline tests, then state exactly which
  OCUDU config knob + expected log line proves it.
- Never seed from gNB config/logs/SIB1-dedicated assumptions: the receiver must discover everything blindly.
- Do not touch the X410, sens4, `gnb_remote_logs/`, `cuLogs/`. Don't push.
- Report: append to `gap-<lane>-report.md` in this directory (LOCAL, Write tool). Reply: Status
  (DONE / DONE_WITH_CONCERNS / BLOCKED), commits, tests run + counts, live evidence (if any), open concerns.

## ADDENDUM (00:50): sens6 load was 25 on 12 cores
Before launching ANY capture on sens6 also wait until no compile runs (`pgrep -x make; pgrep -x cc1; pgrep -x cc1plus; pgrep -x ninja` all empty) and the 1-min load avg < 8. Record the load avg at capture start in your report. A capture that overlapped a build is VOID. lane-make.sh already makes builds wait for running captures.
