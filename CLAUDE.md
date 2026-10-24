# CLAUDE.md — passive agnostic 5G NR receiver (OAI UE based)

Read `PROJECT_MEMORY.md` completely before doing anything; it is the single source of truth.

Hard rules:
- sens6 is FROZEN: never edit tests/passive_rx/captures/*, tests/passive_rx/*.conf, tests/passive_rx/sens6_host_snapshot_2026-09-30/*.
  Check: git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
- New config files under tests/passive_rx/ use the .cfg extension (the frozen gate pathspec tests/passive_rx/*.conf matches across /).
- gNB logs/configs are validation ground truth only, never receiver input.
- Evidence labels (PROJECT_MEMORY §0.1) on every claim; never merge results across hosts/cells/bandwidths.
- Never build while nr-uesoftmodem is running (pgrep -x nr-uesoftmodem).
- Stop a receiver with SIGINT, never SIGKILL (X410 claim hygiene).
- ISAC_RX_BRANCH_FO must stay unset (never enable at rank > 1).
- git add <explicit paths>; never -A, never stash.

Build (DGX, aarch64): cmake_targets/ran_build/build, Ninja:
  ninja nr-uesoftmodem nr-uesoftmodem-optional-modules oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4
Build (cloud x86): same without oai_usrpdevif, with -DOAI_USRP=OFF: ninja nr-uesoftmodem nr-uesoftmodem-optional-modules rfsimulator params_libconfig nr-softmodem tests && ctest -j4
Build (sens6, x86 + discrete GPU, CUDA 12.4, gcc 15): cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON <src>; ninja -j4 nr-uesoftmodem nr-uesoftmodem-optional-modules rfsimulator params_libconfig nr-softmodem tests; ctest -j1 (host RAM 14.8 GB). PROJECT_MEMORY §9.
Evidence labels for non-DGX hosts must name the host (e.g. cloud x86, sens6 x86 discrete GPU).
Known ARM ctest failures: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256, test_nr_pusch_ra0_qam64 (intermittent).
Known sens6 ctest failures: nr_td_sim_test, test_nr_ssb_rate_match_prod, test_vrtsim_cirdb (PROJECT_MEMORY §25, 2026-10-24).
GPU tiers: GPU_PLATFORM tier=shared|discrete|cpu is chosen per device at runtime (no per-host config). NR_GPU_FORCE_MODEL=discrete|cpu is a diagnostic override. NR_GPU_MEM_BUDGET_MB and NR_CSIRS_DISC_SEGMENT_SCRATCH are diagnostic/test hooks (keep or remove: user decision pending). Evidence labels name the host and the tier.
Regression: tests/passive_rx/dgx/rfsim_regress.sh, 4-RX gate (PROVISIONAL 2026-10-04; 4 RX for all tests): 106 PRB --ue-nb-ant-rx 4, 420 s arms, merged-main defaults ON, idle host under flock -x /tmp/td_measure.lock; postconv unchanged (2 contexts, 0 reopens, post-conv CRC >= 99.8 over >= 5000 grants), ttc tda0 <= 39 s / tda2 <= 77 s, CRC floor 93.4, drop_full <= 2.5 % (scan queue, K45). The 1-RX 150 s gate (8.6 / 35.3 s, 94.5, 1 %) is historical; GATE_MODE=legacy = old CRC >= 98 %. See PROJECT_MEMORY §12, dgx/README.txt. Since 0fb22ecbd0 the old scorer score_rx.py cannot pass (no `Technique D CONVERGED` lines). Proposed, not adopted (user decides): tests/passive_rx/dgx/score_rx_solver.py, the scorer used for sens6 runs.
Plan in progress: sens6 portability (PROJECT_MEMORY §25, 2026-10-24 sens6 portability block: 3-tier GPU platform, portable build, b2-fix on sens6; work in progress and T10 DGX regression pending); earlier: levers + acceleration merged 2026-10-04 (PROJECT_MEMORY §25, 2026-10-04 block); next docs/superpowers/plans/2026-10-01-reconfiguration-robustness.md; DGX follow-ups in docs/superpowers/plans/2026-10-01-dgx-next-steps.md
