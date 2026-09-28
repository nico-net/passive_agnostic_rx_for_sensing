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

## Task 3 — csirs — DONE_WITH_CONCERNS (G5 not approved; not merged)

Branch `sdd/gap-csirs`: `621a91dabe` → **`cb5c358`** (local; push blocked, see bottom).

- Root cause: `nr_csirs_blind_zero_score_shift()` compared the pattern energy with the MEAN of all other REs of the
  touched RBs; the lab's lone port-0 pilot (k=0,1; 1-RX identity channel, ports 1–7 dark) inflated it, so every
  candidate on k=2..11 scored ≈0.9999 on each CSI-RS slot → periodic hits → `zp_feed` exported one false ZP per run.
  The row-3 d0.5 hypotheses only added candidates to a class that all scored ≈1.
- Fix (3 review rounds): per-subcarrier-class baseline dropping the 3 brightest classes (keeps 8-RE holes and ZP+CSI-IM),
  plus a slot score requiring the pattern's own subcarriers to be ≥4× brighter in a neighbouring symbol; the RT loop
  FFTs l±1 only when the in-symbol score > 0.5.
- G3: RED→GREEN each round via the production chain (enumerate → `nr_generate_csi_rs` → score → `zp_feed`):
  lab case exported `1:0:106:4:13:0:0:3:0:160:0` (1.000) → no export (0.000); wide holes (median round 0.06–0.17) →
  exported; 8-class NZP / two NZP without PDSCH (0.93–0.9999) → not exported. Logs `/home/user/wt/logs/csirs/`.
- G1 clean (only the 12 ISAC link failures). G2 ctest 110/123 = baseline; shuffle 1/3/5 green (csirs_blind_search 44,
  blind_monitor 195+2, config_sweep 43+1, prb_set 17, scrambling 11, synth PASS).
- **G5 NOT APPROVED** (round 1: median kills wide holes → fixed; round 2: ≥4 bright classes without PDSCH → fixed;
  round 3: 2 Important open): (1) PDSCH ending just before a CSI-RS symbol with ≥4 visible classes — the OAI gNB's
  default CSI-slot TDA (PDSCH 1..12, CSI-RS 13) — still scores ≈0.997; (2) neighbour energy that is not PDSCH (another
  NZP of the set, SSB/PBCH, PDCCH DM-RS) is accepted. Neither is a regression vs the base. Coordinator decision:
  stop single-slot score iterations — every remaining case leaves a ZP on REs that carry no PDSCH in the slots it was
  learned from, and the harm only appears when later data lands there, which needs contradiction-based revocation
  (already in `sdd/gap-ocudu-dl`: probation, contradiction debt). Documented in the code as NOT GUARANTEED.
- Merge note: textual conflict with `sdd/gap-ocudu-dl` in `nr_csirs_blind_zero_score_shift` / header / RT ZP block.
- Wide path still opt-in. G4 pending (lab): repeat the 8-port AWGN ×3 arm — expect 0 ZP exports on the lab bed.

## Task 4 — nsa-mib — DONE (offline-complete, merged)

Branch `sdd/gap-nsa-mib` (from `239eb144ac`): **`70516af`** (local; push blocked).

- Fix: `nr_pdcch_blind_monitor_set_mib_dmrs_typeA_position()` called from `nr_ue_decode_mib()`; writes only
  `g_cfg.dmrs_typeA_position` (CSS0 state and learned geometry untouched). Manual `pdcch_blind_monitor_bwp` DM-RS value
  is now an initial value the MIB overrides (help text updated).
- G3: new `test_nr_ue_mib_blind_handoff` (6 tests, real MAC MIB path → CSS0 gate → real `nr_pdcch_blind_extract_11`).
  RED: NSA kSSB31 pos3 got DM-RS mask 0x884 (symbol 2), cfg enum 0 vs MIB 1; repeated-MIB and no-CSS0 scenarios failed.
  GREEN 6/6. Controls: NSA pos2, CSS0 cell pos2/pos3, repeated MIB keeps learned geometry byte-identical.
- **CSS0-reachability verdict: CONFIRMED (retraction holds).** Blind CORESET discovery converges with no CSS0 at all
  (test 6; source: `nr_pdcch_blind_monitor_rt.c` discovery step before the periodicity gate, CORESET declaration sets
  periodicity 1). Not proven offline: RNTI bootstrap without CSS0 (help text still says it comes from the CSS0 path).
- G1 clean; G2 ctest 111/124 (baseline + new suite); shuffle 1/3/5 green. G5 independent: **Approved** (Minor only;
  help text + test comment fixed; coreset bank keeps a stale copy on a cell change — pre-existing, follow-up).
- G4 pending (lab): NSA / no-SIB1 cell with pos3 — named line `SENSING: MIB dmrs-TypeA-Position pos3 -> blind monitor`.

## Merges into `sdd/offline-validated` (created from `sdd/integration` @ `239eb144ac`)

| # | Merge commit | Lane | Post-merge G1 | Post-merge G2 |
|---|---|---|---|---|
| 1 | `13d76a2` | `sdd/gap-nsa-mib` @ `70516af` | clean (12 ISAC link failures only) | ctest 111/124 (13 = baseline set); shuffle 1/3/5 green (blind_monitor 195+2, config_sweep 43+1, prb_set 17, scrambling 11, mib_handoff 6) |
