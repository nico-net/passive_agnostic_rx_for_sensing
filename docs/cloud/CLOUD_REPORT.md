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

## Task 2 — ssb — DONE (offline-complete, merged)

Branch `sdd/gap-ssb` = WIP `d4d807a7d3` + **`af8213a`** (local; push blocked).

- The WIP did not build: the production-path check had a VLA/goto compile error, link gaps and a LOG segfault — fixed
  (test-side only). Production integration unchanged by that step.
- G5 round 1 (NOT approved): Critical — the data-aided tap re-encoded newly decodable SSB grants with a hole-less G
  (garbage CFR); Important — decoder wiring untested, detector sensitivity untested. Round 2 (NOT approved): Important —
  the new RE-budget invariant exposed a pre-existing CSI-RS count bug (CRB/BWP index mix-up for BWPStart>0; overlapping
  resources summed) that would have turned CRC-passable grants into ERROR. Round 3: **Approved** (Minor only).
- Final lane content: per-slot PSS/SSS detector (Bernstein bound, 1e-9/trial; ≥99 % at 8 dB, 2-tap channel), SSB
  mask in extraction and G, fail-closed DM-RS/PT-RS/SI-RNTI, `PDSCH SSB-OBS` named line; RE-budget invariant;
  `nr_dlsch_csi_unav_res()` (union, CRB parity) + always-mask for the passive path (attached UE unchanged); tap
  `decode_G` guard.
- G3 RED evidence (logs `/home/user/wt/logs/ssb/`): 4 production breaks; decoder-wiring breaks (G without `+ssb_unav`
  → G 31800 vs 29880 bits; early FEP dropped; mask not handed → ERROR, 0 LDPC calls); tap guard off; old Hoeffding
  gate (243/1000 at 8 dB); partial-edge PRB; antenna-0-only; CSI BWPStart 10 (ERROR) and overlap (G 27648 vs 28224).
- G1 clean (12 ISAC link failures only; `dmrs_first` warning is at base). G2 ctest 112/125 (baseline + 2), shuffle
  1/3/5 green (ssb_rate_match 8, blind_monitor 195+2, config_sweep 43+1, prb_set 17, scrambling 11, prod 0 failures).
- Follow-ups (not blocking): density-0.5 odd-BWPStart CSI scene; PT-RS on CSI-RS REs double-counted (pre-existing);
  attached-UE CSI count still has the BWPStart>0 / overlap bug (pre-existing, outside the passive path); early FEP
  runs on time-only overlap (deliberate, for the G4 line) — measure cost live.
- G4 pending (lab): named line `PDSCH SSB-OBS`.

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
| 2 | `418999d` | `sdd/gap-ssb` @ `af8213a` | clean merge, no conflicts; build clean (12 ISAC link failures only) | ctest 113/126 (13 = baseline set); shuffle 1/3/5 green (18/18 runs: + ssb_rate_match 8, mib_handoff 6) |

`sdd/gap-csirs` is NOT merged (G5 not approved). Expect a textual conflict between it and `sdd/gap-ocudu-dl` in
`nr_csirs_blind_search.c/.h` and the RT ZP block of `nr_csirs_blind_rt.c`; the ssb lane touches the PDSCH CSI-RS
count (`nr_dlsch_csi_unav_res`) but not the CSI search, so ssb × csirs should merge cleanly.

## Task 5 — ocudu-dl (optional) — NOT DONE

Skipped to conserve the shared usage budget; no micro-benchmark was run.

## Branch deletions

**None performed.** `sdd/offline-validated` is now pushed and contains the snapshot below
(`git merge-base --is-ancestor` true), so it meets every deletion rule, but the delete was refused by this session's
permission policy (destructive remote git op). Left for the operator:

| Branch | Tip SHA | Why it may go | Command |
|---|---|---|---|
| `wip/2026-09-28/gap-ssb` | `d4d807a7d375c9726e11194e466480651a87d3e5` | ancestor of `sdd/offline-validated` | `git push github --delete wip/2026-09-28/gap-ssb` |

Kept: `wip/2026-09-28/gap-cbg` (`7e1fce4318`, out-of-scope work) and every non-WIP branch.

## Push status

After GitHub access was restored, all of this session's branches were pushed:

| Branch | Tip | Content |
|---|---|---|
| `cloud/ue-localization` | this report | report commits |
| `sdd/gap-ssb` | `af8213a` | ssb lane (new branch) |
| `sdd/gap-csirs` | `cb5c358` | csirs lane (G5 not approved), fast-forward from `621a91dabe` |
| `sdd/gap-nsa-mib` | `70516af` | nsa-mib lane (new branch) |
| `sdd/offline-validated` | `418999d` | staging: nsa-mib + ssb (new branch) |

## What the lab must still run (G4 per lane)

- **ssb** (`sdd/gap-ssb` / `sdd/offline-validated`): OCUDU or OAI arm with PDSCH overlapping SSB symbols, n ≥ 3,
  alternated with baseline; bar: `PDSCH SSB-OBS` events with `overlap_re>0`, crc_ok ≥ baseline, no `RE-budget
  mismatch` lines, `UNSUP@ssb` counts reported.
- **nsa-mib** (`sdd/gap-nsa-mib` / `sdd/offline-validated`): a no-SIB1 / kSSB ≥ 24 cell with dmrs-TypeA-Position pos3
  (needs the repaired NSA bed, see `gap-nsa-report.md`); bar: `SENSING: MIB dmrs-TypeA-Position pos3 -> blind monitor`,
  blind discovery converges without CSS0, decodes use DM-RS symbol 3. Also check RNTI bootstrap without CSS0 (not
  provable offline).
- **csirs** (merged in `060edd290c`, post-review fixes below): repeat the 8-port AWGN ×3 phy-test arm — expect 0 ZP
  exports. Then an arm where a ZP export overlaps decoded PDSCH (OCUDU CSI-RS bed, `ISAC_CSIRS_BLIND=1`,
  `pdcch_blind_monitor_pdsch >= 1`): bar = `SENSING: CSIRS_BLIND ZP_GRANT_EVIDENCE` lines appear, a false export is
  `REVOKED` after 2 of them, a true ZP never logs a contradiction, and the 20000-slot status line reports
  `zp_grant_evidence=` > 0 with `dropped=0`. **Known false negative to check on the lab**: a cell with an 8-RE ZP
  (row-6-like) or two holes in one CSI-RS symbol (ZP + CSI-IM on different REs) gets NO ZP export by design, so
  every PDSCH crossing that symbol is decoded without that rate matching — expect a CRC deficit on exactly those
  slots (compare crc_ok on CSI-RS slots vs other slots).
- **Not triaged** (never pushed): `wip/2026-09-28/rfsim-*`, `ocudu-bed-matrix`, `ocudu-dl-*` — push them for a later
  session.

## Post-review fixes — G5 review of merge `060edd290c` (csirs × ocudu-dl), 2026-09-28

Review: `docs/superpowers/sdd/full-running-agnosticity/review-060edd290c.md` (NOT approved: 2 Important, 5 Minor).

**Correction to the `060edd290c` merge note (history not rewritten).** The note said the new revocation test
"covers csirs's open G5 findings". It did not: the merged revocation (a contradiction needs the full-band score
<= 0.5, and any qualified hole clears the debt) only withdrew a false ZP under full-band PDSCH on consecutive
occasions with the NZP at the PDSCH EPRE. Measured (reviewer probe, reproduced as RED here): PDSCH on 1/3, 1/5 or
1/10 of the RBs, on alternate occasions, or beside an NZP boosted +6 dB — never revoked in 200 occasions. The note
also did not disclose that the synth `nzp8_no_data` / `nzp8_light_load` negatives were relaxed from "no export" to
"may export, must be revoked", with an `if (w < 0) continue;` that let the revocation go unexercised.

**Important 1 — fixed (root cause).** Contradictions are now also scored on the PRBs the receiver's own decoded
grants occupy. After FEP, `nr_pdsch_passive_decode()` calls `nr_pdsch_passive_zp_grant_score()` (exported, pinned by
`test_nr_ssb_rate_match_prod` Z1–Z9) for every ZP (csi_type 2) entry it was handed for rate matching: energy on the ZP
REs inside the grant's PRBs on the CSI-RS symbol(s) (REs of other rate-matching entries excluded, extractor's CRB-parity
bitmap, BWPStart) vs every RE of the grant on ONE data-only reference symbol (no DM-RS/CSI-RS, not an SSB symbol; the
one nearest the ZP symbol), all antennas summed. **Noise-floor gate**: the reference must be >= 6 dB over the
hypothesis-free noise floor of that symbol (guard-band FFT bins next to the carrier edges,
`nr_csirs_blind_guard_energy`), else the grant is no evidence — a true ZP reads noise, and a grant decoded where nothing
was sent (false DCI accept, wrong PRB/symbol/k0 hypothesis) would otherwise read noise-vs-noise = a contradiction
(RED during this fix: such a true ZP was revoked after 2 grants; pinned `GrantDecodedUnderAWrongPrbHypothesisIsNoEvidence`,
incl. a phantom whole-carrier grant). The score is posted to a mutex queue (`nr_csirs_blind_rt_zp_grant_evidence`);
`nr_csirs_blind_rt_slot()` drains it into `nr_csirs_blind_zp_grant_feed()`: a separate window of the last 8 distinct
predicted slots with evidence (out-of-order arrival allowed; evidence from before the current export is stale and
ignored), a slot is a contradiction only if NO scorable grant of it supports the ZP, 2 contradicted slots revoke, and a
qualified full-band hole does not clear it. The full-band path keeps ocudu-dl's rules unchanged on purpose: in a fully
idle symbol its score reads ~0 exactly like data on the pattern, so without the hole-clears-debt rule a TRUE ZP would
be revoked by idle occasions (pinned: `TrueZpSurvivesIdleOccasionsAndDecodedGrants`).
Cost (micro-benchmark of the production scorer, RelWithDebInfo, pinned core, 273 PRB full-band grant): 9.7 µs with
1 antenna, 38.1 µs with 4 (the first cut read all data symbols with a per-RE modulo: 325.8 µs, reviewer). Runs only for
grants carrying a ZP entry, per decode.

| Case (false ZP k8..11 beside 8-class NZP k0..7) | before (full-band only) | after: wrong occasions until revoked |
|---|---|---|
| full band, every occasion | 2 | **2** |
| 1/2 of RBs | 2 (borderline 0.499) | **2** |
| 1/3, 1/5, 1/10 of RBs | never (200 wrong) | **2** |
| full band, alternate occasions | never (100 wrong) | **2** (revoked at occasion 4) |
| random 30 % duty, 200 trials | 200/200, mean 4.1 wrong | **2 in every trial** (mean 6.6 occasions) |
| NZP +3 dB, full band | 2 (score ~0.5, fragile) | **2** |
| NZP +6 dB, full band | never | **2** |
| 1/10 of RBs + NZP +6 dB + alternate | never | **2** |
| 1/3 of RBs, other UEs' PDSCH on the rest, NZP 0 / +6 dB | 2 / never | **2 / 2** |
| synth (real generator chain): NZP 0 dB / +6 dB, grants on 1/1 and 1/3 of RBs | +6 dB: never | **2** each |

"Wrong occasions" = decoded grants rate-matched around the still-exported false ZP, counted synchronously. On the
receiver add the decodes already queued or in flight when the revoking evidence is drained (decode-queue latency /
ZP period); the bound is therefore 2 + in-flight, not 2. Grants with no evidence: no data-only symbol in the FFT'd
range, or a reference under the noise gate. (GPU-LLR decodes never carry CSI rate matching, so they are not a gap.)

Known limits, pinned: (a) full-band score alone (grants the receiver does not decode — harmless to its own decoding):
1/3 of RBs never, alternate never, +6 dB never, full band 2 (`KnownLimitationWithoutDecodedGrantEvidence`);
(b) interference on a TRUE ZP (CSI-IM) at or above the grant's own signal (SINR <= 0 dB) reads as a contradiction
(unit-pinned in `GrantScoreSeparates…`); (c) a phantom grant only escapes the gate if its reference is >= 6 dB over the
guard noise, i.e. something (another UE's PDSCH) was actually sent there — then a cell-wide true ZP supports.

**Important 2 — fixed.** Synth `nzp8_no_data` / `nzp8_light_load` now `assert(w >= 0)` (export must happen), then
assert full-band revocation (0 dB) AND decoded-grant revocation after exactly 2 wrong occasions. New boosted case
(NZP +6 dB): exported; full-band revocation pinned as NOT happening; decoded-grant revocation after 2. The NZP rescale
to data EPRE is no longer what makes revocation pass.

**Minor 1 — fixed.** `WideHolesUnderDataAreStillHoles`, `DarkPorts…ReadAsAHole`, `CompleteWideHoleSurvivesLowSnr`
renamed `ScorerOnly…`, and each asserts its wide shapes ((6,6), (5,7), (4,8), (2,6)) are NOT in the production
enumeration (per-RB footprint masks), so a new row cannot silently turn them into production claims.

**Minor 2 — documented + pinned (declared capability loss).** 8-RE ZP holes and two holes in one symbol (ZP +
CSI-IM on different REs) are refused by design: no rows-1..5 candidate explains them, so no ZP is exported and on
such a cell every PDSCH crossing that CSI-RS symbol is decoded without that rate matching (known false negative;
lab check in the G4 list above). Pinned by `EnumeratedShapesRefuseEightReAndTwoHoleSymbols` (all 22 distinct
enumerated per-RB masks score <= 0.5 on both) and the existing synth "not exported, as designed" asserts.

**Minor 3 — pinned as known limitation.** Renamed `WideNzpBesideUnusedResWithoutPdschSubsetIsNotAHole`; it now
also asserts the complete (8,4) candidate scores > 0.9 on the same input (exported), covered by the decoded-grant
revocation bound above (2 wrong occasions).

**Minor 4 — covered** by the boosted-NZP rows (+3/+6 dB) in the gtest table and the synth +6 dB case.

**Minor 5 (informational).** Row-3 density-0.5 raises the default enumeration from 683 (reviewer, parent 1) to
767 candidates (re-measured here: synth `ENUM: 767 candidates`), +12 % discovery rotation; FFT budget unchanged (RT identical to ocudu-dl, FEP-count test passes); the `n_cls <= 3`
unscorable guard is unreachable for rows 1–5 (it only makes results more conservative).

**Evidence (sens6, `agn-wt/cloud`, build dir `cmake_targets/ran_build/build` inside the worktree (gitignored, created
from an init-cache of `agn-wt/integ`), every build under `flock radio_bed.lock`, no receiver running, load < 1).**
- G3 RED (stubs, tests written first): `test_nr_csirs_blind_search` 121/126 — 5 new tests fail, the load-shape table
  reproducing the reviewer's probe exactly (1/3, 1/5, 1/10, alternate, +6 dB: never revoked); synth aborts at
  `grant_revoke` (data on the false ZP not counted). Wrong-hypothesis RED (first cut without a gate): 125/128, a true ZP
  revoked at occasion 2 / 4. Review-round mutants, each caught: no noise gate (2 tests fail), no support-wins (1), no
  stale-evidence floor (1), no other-entry exclusion (Z4), antenna 0 only (Z8), skip mask ignored (Z9), no even/odd
  bitmap split (Z5 x2). GREEN: `test_nr_csirs_blind_search` 128/128 (118 existing + 10 new), synth PASS,
  `test_nr_ssb_rate_match_prod` PASSED (existing checks + Z1–Z9). Logs: `…/build/evidence/` (not committed).
- G1: `nr-uesoftmodem` and `tests` build clean; the only warning in a touched file (`dmrs_first` maybe-uninitialized in
  `nr_pdsch_passive_decode.c`) is pre-existing on the parent.
- G2: full ctest 125/126 on the parent `a13c2b9a06` and 125/126 with the fix — same single failure
  (`test_vrtsim_cirdb`, environment), delta 0. Shuffle seeds 1/3/5 green: csirs_blind_search 128, blind_monitor
  195+2 skipped, config_sweep 44+1 skipped, prb_set 17, scrambling 11, ssb_rate_match 8, mib_handoff 6.
- G5: round 1 (fresh reviewer) NOT approved — 3 Important (whole-carrier phantom/wrong-hypothesis grants still
  contradicted a true ZP; the first-cut locality test hid false ZPs when other UEs' data surrounded the grant; 325.8 µs
  per grant) + 4 Minor (untested production hook, SSB in the reference, stale evidence, report inaccuracies). All
  addressed above; round 2 below.
- G4: pending (lab) — see the csirs entry in "What the lab must still run".
