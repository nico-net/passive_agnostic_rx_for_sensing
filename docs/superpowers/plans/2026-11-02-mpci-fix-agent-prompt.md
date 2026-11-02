# Agent prompt: multi-PCI fixes (start 2026-11-02)

You are fixing the multi-PCI (one process, several cells) receiver of the passive agnostic 5G NR RX (OAI nr-uesoftmodem --passive-rx).
Host: DGX Spark (GB10, aarch64) with an X410 34B2AF1 over DPDK. Work offline (replays) only; the operator runs OTA.

## Read first
- `CLAUDE.md` (hard rules) and `PROJECT_MEMORY.md` §0.1 (evidence labels), §36 (multi-PCI OTA 10-31), §38, §39 (OTA 2026-11-02; §39.7 is
  the multi-PCI diagnosis, §39.9 the open items).
- `docs/superpowers/reports/fix1102/mpci2/MPCI2.md` with its replay recipe `wide.sh` (wideband arm; `CTL=1` = control arm), `solo1.sh`, `an.py`.

## Base and branches
- Base: `mpci-main` (worktree `/home/nicola/NICOLA/wt/rr-mpci-main2`, build dir `cmake_targets/ran_build/build` there).
- One branch + worktree per fix: `fix1102/mpci-<topic>` in `/home/nicola/NICOLA/wt/rr-fix-mpci-<topic>`, from `mpci-main`.
- Do not merge into mpci-main and do not push. The operator decides after OTA.

## Inputs
- 2-cell wideband capture: `/home/nicola/NICOLA/ota_runs/raw_v415t923_245_2s_1102_1147` (2 s, 245.76 Msps, centre 3720 MHz, PCIs 415 + 923).
- Direct captures (4 s, 122.88 Msps): `ota_runs/raw_pci415_4s_1102_0951`, `raw_pci923_4s_1102_0951` (and `raw_pci508_4s_1102_0952`).
- OTA log of the 2-PCI run: `ota_runs/o1B_1_094318/run.log`; solo runs `ota_runs/o{1,2}S{415,923}_1_*`.
- Env `MPCI_20261025.env` + `ISAC_SIB1_CACHE=0`, cfg `ue.passive.ota.sense.dgx.cfg`, cells inventory `mpci_cells/v415_t923.json`.

## Facts you start from (all in §39.7)
1. Decode tail: 415 dec p99 solo ~3.9 ms, two direct cells in one process ~3.9-5.6 ms, through the wideband channelizer path 11.8-50 ms.
   It was already 28 ms in OTA before cell 923 locked. Channelizer blocks stall 25-36 ms (chan_work_max), overruns 0.
2. Second cell: 923 fails at PBCH (PSS strong, 25-33 dB) much more with two cells in one process, even WITHOUT the channelizer
   (control arm: 0 / 10 / 0 grants decoded vs ~20k solo). First scans show fo_pss ~ -fo_seed (+4.4..5.5 kHz); the CFO ladder step that
   locks solo does not lock with two cells.
3. Cell-0 bad lock (wideband only): 415 locks an SSB (index 2 or 5) whose tracking PBCH then fails -> RFSTALL -> "full device re-init",
   which restarts BOTH cells (3x in the first 30 s of the OTA run; replay exits rc=3 in 7 of 10 runs).

## Tasks, in this order (each: root cause -> failing test first -> general fix -> before/after replay n >= 3, warm runs)
A. Second-cell PBCH acquisition with two cells in one process (fact 2). Start from the control arm (`CTL=1 wide.sh`), no channelizer.
   Find what the second cell shares with or loses to the first: search/PBCH state, CFO seed or ladder state, SFO/timing state, the scan
   queue, CPU leases. Done when 923 in the control arm locks and decodes within 2x of solo lock time in 3 of 3 runs, with 415 unchanged.
B. Per-cell recovery (fact 3). A PBCH loss on one cell must re-acquire that cell only, never restart the device or the other cell; and
   a lock whose tracking PBCH fails must be rejected before it is accepted. Done when the wideband arm has 0 rc=3 exits in 5 runs and
   cell 415 never triggers a device re-init.
C. Wideband decode tail (fact 1). First run the single experiment of MPCI2.md: channelize the 11:47 capture offline with the receiver's own
   channelizer (iq_chan, one output per cell), replay the outputs through the control arm, n = 3.
   - Looks like the direct control -> samples are fine; the cost is the live channelizer runtime: profile its GPU stream/priority, block
     size and the 25-36 ms stalls, then fix.
   - Looks like the wideband arm -> the channelized samples are degraded: bench SSB/PDSCH SNR and phase through iq_chan against the direct
     captures (test-first), then fix.
   Done when 415 small-grant (TBS < 20 kB) dec p99 through the wideband path is within +30 % of the control arm.
Stop after A-C, or earlier if a task needs a decision only the operator can make (then report facts and ONE plan with a stop condition;
no shifting hypotheses).

## Rules (from CLAUDE.md and the operator; violations have cost whole nights)
- Evidence label on every number: `[MEASURED]` (OTA), `[OFFLINE VERIFIED, DGX replay]`, `[CODE-READ]`, `[HYPOTHESIS]`. Never pool numbers
  across cells, hosts, bandwidths or builds.
- Fixes must be general: no per-cell or per-operator tuning, no new flags, env variables or CLI options. Never trade CRC for latency
  (decoded PDSCH payload feeds sensing).
- 4 RX everywhere. sens6 files are frozen (see the CLAUDE.md git diff check). New config files under `tests/passive_rx/` use `.cfg`.
- Build: `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh`, under `flock -s /tmp/td_measure.lock`, `ninja -k 0`; a fresh build dir needs
  `-DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON "-DLDPC_CUDA_ARCH=89;121" -DOAI_USRP=ON -DENABLE_TESTS=ON` and the
  `liboai_device.so -> liboai_usrpdevif.so` symlink. Check the GPU modules exist (libpdcch_gpu, pdcch_nid_gpu, td_cb0_gpu, csirs_disc_gpu,
  shm_iq_chan_gpu, shm_iq): without them the receiver silently decodes no dedicated grants.
- Replays and benches: `flock -x /tmp/td_measure.lock`, idle host. Never build while `pgrep -x nr-uesoftmodem` finds a receiver (another
  user's docker UE may match: check the PID owner; use a private `CUDA_MPS_PIPE_DIRECTORY`). Never wait with `pgrep -f` on a pattern in your own command line.
- Stop a receiver with SIGINT, never SIGKILL. `ISAC_RX_BRANCH_FO` stays unset.
- `git add <explicit paths>`; never `-A`, never stash. Commit per logical step.
- Pre-existing ctest failures (not yours): test_nr_ssb_rate_match_prod, test_ldpc_cuda_pool_cpu, test_nr_gpu_pass_cells, test_nr_pusch_ul_disc,
  nr_td_sim_test, dft_test, test_nr_modulation, test_nr_pusch_ra0_qam64/256. Anything else failing is yours.

## Report
`docs/superpowers/reports/fix1102/MPCI-FIX.md` on your branch: per task the root cause with evidence, the diff summary, the before/after table
(per cell: locks / failed searches, grants, CRC, dec p50/p99, tot p50/p99, drops, rc), what is not covered, and the OTA test the operator
should run (cells, duration, what to look at). Keep it short.
