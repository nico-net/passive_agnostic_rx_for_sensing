# Shared implementer context (read before your brief)

Project: a fully agnostic passive 5G NR receiver built on the OAI UE (`--passive-rx`). It discovers every
RRC-configured parameter blindly from the air instead of being told.

## Where the code is
- The code lives ONLY on host **sens6**: `R=/home/sens/NICOLA/adaptive-rx-UL-DL` (branch `adaptive-rx-UL-DL`).
  Build dir `B=$R/cmake_targets/ran_build/build` (Makefile generator: `make -j12 <target>`, tests with
  `ctest -R <name> --output-on-failure` from `$B`).
- Run every command via `ssh sens6 "…"`. **Never edit `/home/sens/NICOLA/adaptive-rx-UL-DL` on the local
  machine** — that is a stale copy holding someone else's work.
- Editing remote files: either `scp sens6:$R/path /local/scratch/`, edit with your Edit tool, `scp` back; or pipe
  a script: `ssh sens6 "python3 -" <<'EOF' … EOF` (quoted heredoc, stdin passes through ssh). Nested
  shell quoting inside `ssh "…"` breaks easily — prefer the scp or piped-script route for multi-line edits.
  Your scratch dir: `/tmp/claude-1000/-home-sens-NICOLA/bfd7f449-077b-4a5a-9dc0-b60f22c01ef8/scratchpad/`.

## Rules
- **Never build while a capture is running**: before any `make`, run
  `ssh sens6 "pgrep -x nr-uesoftmodem || echo none"`. If a receiver is running, STOP and report BLOCKED.
- Match OAI style of the file you edit; `LOG_I/LOG_W/LOG_E/LOG_A(PHY, …)`, never `printf` in library code.
- Never relax a test assertion the brief specifies to make it pass. If one fails, stop and report the measured
  value (DONE_WITH_CONCERNS or BLOCKED).
- Opt-in `ISAC_*` env knobs default off unless the brief says otherwise.
- Do not seed anything from lab ground truth or SIB1-dedicated assumptions; gNB logs are for validation only.
- Do not touch `gnb_remote_logs/`, `cuLogs/`, or anything on sens4.
- Do not push. Commit on the current branch only. Commit messages end with exactly:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
  ```
  Commit via `ssh sens6 "cd $R && git add <files> && git commit -F -" <<'EOF' … EOF`.
- Stage only the files your task touches (`git add <explicit paths>`), never `git add -A`.
- **Report files and all paths under /home/sens/NICOLA/docs/ are on the LOCAL machine** — write them with your Write tool, never via ssh on sens6.
