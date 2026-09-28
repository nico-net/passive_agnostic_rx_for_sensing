# Round 3 lanes (2026-09-27 evening) — shared rules

Read first: `context.md` (this dir), the handover `ssh sens6 cat /home/sens/NICOLA/adaptive-rx-UL-DL/HANDOVER_AGNOSTICITY.md`
(sections 1-4: hard rules, gates G1-G6, test procedures for OAI phy-test / OAI SA / OCUDU ZMQ), and
`ocudu-harness.md`, `ocudu-test-knobs.md`.

- Lane worktree on sens6: `/home/sens/NICOLA/agn-wt/gap-<lane>`, branch `sdd/gap-<lane>`, forked from
  `sdd/integration` @ 239eb144ac (all merged lanes). Commit only there. Build with
  `/home/sens/NICOLA/agn-wt/lane-make.sh gap-<lane> <targets>` (replicate a build dir from
  `agn-wt/integ/cmake_targets/ran_build/build` via an init-cache, NOT cp -a).
- **Radio beds are serialized with flock.** Every radio run (gNB/UE/srsUE/broker/passive, phy-test or SA or
  OCUDU or NSA) AND every heavy build on the same host must be wrapped:
  `flock -w 14400 /home/sens/NICOLA/radio_bed.lock <script>` (the lock file exists on each host separately:
  sensnuc3 for SA/OCUDU/NSA, sens6 for phy-test). Hold it only for the run itself; write the run as a script
  with logs. Before a capture also check load avg < 8 and no compile running; record it.
- sensnuc3 (this machine) local trees: OCUDU gNB `/home/sens/NICOLA/repos/ocudu` (stock) and
  `/home/sens/NICOLA/repos/ocudu-test` (branch isac-test-knobs, built), srsUE `/home/sens/NICOLA/repos/srs-ue`,
  OAI test gNB tree `/home/sens/NICOLA/rfsim-local`, passive receiver tree to use for live runs:
  `/home/sens/NICOLA/rfsim-integ` — update it to your lane's code by fetching your branch from sens6
  (`git -C /home/sens/NICOLA/rfsim-integ fetch sens6:/home/sens/NICOLA/adaptive-rx-UL-DL sdd/gap-<lane>`)
  into a NEW local worktree `/home/sens/NICOLA/rfsim-<lane>` (never edit rfsim-integ itself), build with ninja
  under the flock. OCUDU harness script: `tests/passive_rx/run_ocudu_passive.sh` (see ocudu-harness.md
  Procedure; env knobs PX_ENV, SRSUE_ENV, GNB_ENV, GNB_BIN, PX_FIRST).
- Gates G1-G6 before asking for merge; n ≥ 3 live runs for G4; truth only from gNB logs/pcaps (validation,
  never seeded). Never `git stash`, never `pkill -f`, never systemctl open5gs, never touch X410/sens4/
  gnb_remote_logs/cuLogs. Do NOT test the sensing pipeline (operator has not approved). Don't push.
- Do not edit the handover yourself; append progress to `gap-<lane>-report.md` in this directory after every
  step (so a cutoff loses nothing). Reply: Status, commits, tests + counts, live evidence, open concerns.
