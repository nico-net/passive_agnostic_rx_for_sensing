# Prompt for the PUCCH session on sens6 (paste as the first message)

You are starting the passive blind PUCCH work on **sens6** (x86_64, RTX 4060 Ti 8 GB, OCUDU lab gNB + COTS UEs).
Read `CLAUDE.md` and `PROJECT_MEMORY.md` completely first; they are the single source of truth.

**Branch:** check out `twomodes/design` at the commit named in the operator's message (it includes traffic mode
E1-E6, sensing S1-S8 and the 415-collapse lock fix). Create a new branch `pucch/sens6` from it in its own worktree.
Do not push, do not merge; the operator merges.

**Read, in this order:**
1. `docs/superpowers/specs/2026-11-05-pucch-extraction-design.md` (the design; binding).
2. `docs/superpowers/plans/2026-11-05-pucch-extraction.md` (the plan, tasks P1-P6).
3. `docs/superpowers/specs/2026-11-03-two-modes-design.md` and `docs/superpowers/specs/2026-11-04-param-extraction-addendum.md`
   (traffic mode, DCI records, the E1 field prover that gives you K1/PRI/DAI, the never-guess and no-loss rules).

**First job, before any PUCCH code: prove the port is general.**
- Build on sens6 with the cloud/x86 recipe from CLAUDE.md, adapted to this host:
  `cmake -G Ninja -DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON ...`, then
  `ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests && ctest -j4`. Use the CUDA arch of the
  4060 Ti (sm_89).
- Report every compile error and every new test failure as portability bugs. Keep that list separate from the
  known ARM failures listed in CLAUDE.md.
- Fix only general portability issues; never host-specific tuning, never fixed cores.
- Run the traffic-mode and sensing-mode receivers on the sens6 lab cell. Compare with the gNB log (validation only,
  never receiver input):
  - DCI records against scheduled DCIs;
  - fields against the gNB values;
  - CFR records with quality.
- Label everything `[MEASURED OTA sens6]` or `[SIM VERIFIED sens6]`.

**Then execute the PUCCH plan** with superpowers:subagent-driven-development (Sonnet agents by default; a review per
task; a final whole-branch review). P1-P3 are pure and run anywhere. P4-P5 need the lab bed. P6 is the validation.

**Hard rules (also in CLAUDE.md):**
- sens6 frozen paths untouched.
- New configs use `.cfg`.
- gNB logs are ground truth only.
- No new env, CLI or -D flags. No per-cell or per-operator tuning. No fixed cores.
- Never build while nr-uesoftmodem runs. Stop receivers with SIGINT only. `ISAC_RX_BRANCH_FO` unset.
- `git add` explicit paths, never `-A` or stash.
- Evidence labels name the host. Never merge results across hosts, cells or bandwidths.
- Never guess a field: null plus raw data until it is proven.
- Traffic mode only for PUCCH. Legacy and sensing outputs stay byte-identical.
- The PUCCH worker never runs on the RX or scan threads and never takes g_phase2_mu or the UL-layout lock (a lock
  convoy on those caused scan collapses on 2026-11-04).

**Report to the operator, briefly:**
- the portability result first;
- then per-task progress with evidence;
- and finally the P6 numbers per format: detection, false alarm, ACK/NACK/DTX confusion, resource correctness and
  CPU.
