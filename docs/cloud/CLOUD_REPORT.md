# CLOUD_REPORT — offline validation of the passive agnostic receiver lanes (cloud session 2026-09-28)

Coordinator: Claude Code (cloud container, 4 cores / 15 GB). Scope and rules: `docs/cloud/CLOUD_SESSION_PROMPT.md`.
Offline only: no live beds were started; **G4 is pending (lab)** for every lane below.

Remote name: in this container the GitHub remote is `origin` (the prompt calls it `github`).

## Environment

- Build: per-lane worktrees under `/home/user/wt/<lane>`, one build dir each (`cmake -G Ninja -DENABLE_TESTS=ON
  -DAUTO_DOWNLOAD_ASN1C=ON`, RelWithDebInfo, ccache shared through `CCACHE_BASEDIR`), heavy builds serialised by
  `flock` (`/home/user/wt/bin/lane-build.sh`). System packages installed: libconfig, libsctp, gtest/gmock,
  benchmark, blas/lapack(e), fftw3, libcap, zmq, ccache.
- Baseline: `sdd/integration` @ `239eb144ac` built in `/home/user/wt/integ` (targets `nr-uesoftmodem rfsimulator tests`,
  `ninja -k 0`, 12207 steps). Pre-existing on the baseline in this container:
  - 12 sensing binaries fail to LINK with the default `ENABLE_ISAC_SENSING=OFF` receiver-only build (test_isac_sync,
    isac_sync_replay, test_eca_clutter, test_target_tracker, test_matrix_complete, test_multi_target_tracker,
    test_det_quality, test_isac_aoa, test_sparse_doppler, test_nr_isac_ssb_source, test_clean_deconv, test_occ_clean)
    → 11 ctest "Not Run". Sensing is out of scope; not touched.
  - `test_thread-pool` aborts (`pthread_getaffinity_np` ret 22 — container CPU-affinity restriction).
  - `nr_cuup_functional_test` fails (`SCTP socket creation failed: Protocol not supported` — no SCTP in the container kernel).
  - Baseline ctest: **110/123 passed, 13 failed = exactly the 13 environment/out-of-scope entries above**. G2 for every
    lane = the same 110 pass + any new tests, with no other failure.

## Task 1 — WIP snapshot triage

`git fetch origin 'refs/heads/wip/*:refs/remotes/origin/wip/*'` found only two snapshots on the remote.

| Branch | Tip SHA | Content | Action / outcome |
|---|---|---|---|
| `wip/2026-09-28/gap-ssb` | `d4d807a7d375c9726e11194e466480651a87d3e5` | SSB rate-match detector + production-path test, 9 files, parent `239eb144ac` | Task 2 → `sdd/gap-ssb` (see below) |
| `wip/2026-09-28/gap-cbg` | `7e1fce4318dda9aca7428d95611490835c4dd2fd` | `cbg_contract_test.py` (1 file), parent `239eb144ac` | Listed only (CBG out of scope); branch KEPT |
| `wip/2026-09-28/rfsim-integ` | — | receiver files + OCUDU ZMQ harness | **NOT PUSHED** — cannot be triaged from the cloud |
| `wip/2026-09-28/rfsim-val` | — | older harness copy | **NOT PUSHED** |
| `wip/2026-09-28/rfsim-local` | — | older harness copy | **NOT PUSHED** |
| `wip/2026-09-28/rfsim-base` | — | older harness copy | **NOT PUSHED** |
| `wip/2026-09-28/ocudu-bed-matrix` | — | matrix planner + 4 tests | **NOT PUSHED** |
| `wip/2026-09-28/ocudu-dl-clean` | — | old OCUDU-DL iteration (superseded by `sdd/gap-ocudu-dl` @ 02aa0cb5a3) | **NOT PUSHED** |
| `wip/2026-09-28/ocudu-dl-g4` | — | old OCUDU-DL iteration | **NOT PUSHED** |
| `wip/2026-09-28/ocudu-dl-r3` | — | old OCUDU-DL iteration | **NOT PUSHED** |

Consequence: the `rfsim-integ` receiver-file port, the OCUDU harness (`sdd/ocudu-harness`), the `rfsim-*` dedupe,
the bed-matrix planner (`sdd/gap-bed`) and the OCUDU-DL old-iteration diff could not be done; the lab must push
those snapshots (`git push github 'refs/heads/wip/2026-09-28/*'` from the hosts that hold them) for a later session.

Other remote branches seen (not WIP, not triaged, untouched): `adaptive-rx-UL-DL` 25a8699a64, `sdd/validation`
c295fa18f1, `sdd/integration` 239eb144ac, `sdd/gap-bwp` 8e9fdbf8b5, `sdd/gap-csirs` 621a91dabe,
`sdd/gap-ocudu-dl` 02aa0cb5a3.

## Task 2 — ssb

In progress.

## Task 3 — csirs

In progress.

## Task 4 — nsa-mib

In progress.
