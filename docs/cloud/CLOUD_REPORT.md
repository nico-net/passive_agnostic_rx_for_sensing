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
  `REVOKED` after 2 of them, a true ZP logs no contradiction on a clean cell (see known limit (b)), and the 20000-slot status line reports
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
`test_nr_ssb_rate_match_prod` Z1–Z15) for every ZP (csi_type 2) entry it was handed for rate matching: energy on the
ZP REs inside the grant's PRBs on the CSI-RS symbol(s) (REs of other rate-matching entries excluded, extractor's
CRB-parity bitmap, BWPStart) vs every RE of the grant on ONE data-only reference symbol (no DM-RS/CSI-RS, not an SSB
symbol; the one nearest the ZP symbol), all antennas summed. A grant only counts when:
- it is dedicated-class (`grant->scr_dedicated`): p-ZP-CSI-RS is UE-dedicated PDSCH-Config, common PDSCH need not
  respect it;
- its first DM-RS symbol carries **this cell's own DM-RS** (scrambling id, nSCID, the grant's ports incl. fd-OCC) on
  its PRBs: `nr_csirs_blind_pilot_coherence/presence` — the coherent fraction of the pilot-RE power over 4-pilot
  blocks, >= 0.5, from >= 8 blocks (RB x antenna; chance pass for noise or a foreign signal at 8 blocks, G5 round-3
  Monte Carlo: < 5e-6 with one port, ~1.1e-3 with two fd-OCC ports).
  This is what makes a false DCI accept, a wrong PRB/symbol/k0 hypothesis, and a co-channel neighbour (which does
  not rate-match our ZP) no evidence; raw energy cannot tell them from our PDSCH (G5 round 2, I1);
- its reference symbol is at least 1/4 of the own-DM-RS power per RE (DM-RS may exceed the PDSCH EPRE by at most
  4.77 dB), so a length hypothesis reaching past the real PDSCH is no evidence;
- the ZP symbol is not an SSB symbol.
No guard-band or absolute-energy test remains (round 1 used a guard-band noise floor; round 2 measured that it reads
the adjacent operator's carrier at 80/100 MHz and then refuses every grant — removed, RED/GREEN pinned by
`AdjacentCarrierInTheFftGuardDoesNotDisableRevocation`). The score is posted to a mutex queue
(`nr_csirs_blind_rt_zp_grant_evidence`); `nr_csirs_blind_rt_slot()` drains it into `nr_csirs_blind_zp_grant_feed()`:
a separate window of the last 8 distinct predicted slots with evidence (out-of-order arrival allowed; evidence from
before the current export is stale and ignored), a slot is a contradiction only if NO scorable grant of it supports
the ZP, 2 contradicted slots revoke, and a qualified full-band hole does not clear it. The full-band path keeps
ocudu-dl's rules unchanged on purpose: in a fully idle symbol its score reads ~0 exactly like data on the pattern,
so without the hole-clears-debt rule a TRUE ZP would be revoked by idle occasions
(pinned: `TrueZpSurvivesIdleOccasionsAndDecodedGrants`).
Cost (micro-benchmark of the production scorer incl. DM-RS generation, RelWithDebInfo, pinned core, 273 PRB
full-band grant): 13.0 µs with 1 antenna, 48.0 µs with 4, per ZP entry per decode call (the first cut read all data
symbols with a per-RE modulo: 325.8 µs, reviewer). Not deduplicated across hypothesis retries of one grant.

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
| 1/3 of RBs, NZP +6 dB, co-channel neighbour INR 10 dB | never | **2** |
| 1/3 of RBs, NZP +6 dB, adjacent carrier -3 / 0 / +10 dB in the FFT guard | never | **2** |
| synth (real generator chain): NZP 0 dB / +6 dB, grants on 1/1 and 1/3 of RBs | +6 dB: never | **2** each |

Conditions of that bound (it is not unconditional): the grant is dedicated-class, crosses the ZP symbol, carries our
own DM-RS on >= 8 RB x antenna blocks at >= 0 dB SINR on its pilots, coherent over a 4-pilot block (residual timing
offset or delay spread below ~1-1.5 µs at 30 kHz; round-3 probe: evidence disappears at >= 1.75 µs at high SNR,
>= 1.5 µs at ~3 dB), has a data-only symbol in the FFT'd range, and the ZP symbol is not an SSB symbol. Outside
these conditions the grant is no evidence (fails safe for a true ZP; a false ZP falls back to full-band-only
revocation) — G4 must check that `zp_grant_evidence` counts rise on a macro cell. "Wrong occasions" are counted synchronously; on the receiver add the decodes
already queued or in flight when the revoking evidence is drained (decode-queue latency / ZP period), so the harm
bound is 2 + in-flight. GPU-LLR decodes never carry CSI rate matching, so they are not a gap.

Known limits, pinned: (a) full-band score alone (grants the receiver does not decode, or that fail the conditions
above — harmless to decoding only for the former): 1/3 of RBs never, alternate never, +6 dB never, full band 2
(`KnownLimitationWithoutDecodedGrantEvidence`); (b) interference on a TRUE ZP (CSI-IM) at or above the grant's own
signal (SINR <= 0 dB on the ZP REs) reads as a contradiction (unit-pinned in `GrantScoreSeparates…`) — including a
neighbour signal present on the ZP REs only (e.g. the neighbour's NZP-CSI-RS/TRS that the ZP protects, while the
neighbour is idle on our DM-RS symbol), which the own-DM-RS gate cannot see; G4 check on a macro cell; (c) a true ZP
that is UE-specific (not cell-wide) can be contradicted by another UE's correctly decoded grant of this cell — the
receiver applies one exported ZP set to every RNTI, so it cannot represent per-UE ZP sets anyway.

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
  `grant_revoke` (data on the false ZP not counted). Wrong-hypothesis RED (first cut, no gate): 125/128, a true ZP
  revoked at occasion 2 / 4. Round-2 RED on the guard-band gate: 128/130 — co-channel neighbour INR 6/10/20 dB
  contradicts a true ZP (revoked after 2); adjacent carrier -3/0 dB in the guard: false ZP never revoked (200 wrong).
  Mutants, each caught: round 1 — no support-wins, no stale floor, no other-entry exclusion (Z4), antenna 0 only
  (Z8), skip mask ignored (Z9), no even/odd bitmap split (Z5 x2); round 2 — no own-DM-RS gate (3 gtests + Z3, Z10),
  no reference-vs-DM-RS check (Z9), dedicated ignored (Z11), ZP in SSB symbol scored (Z12), BWP-relative pilot base
  (Z6). GREEN: `test_nr_csirs_blind_search` 131/131 (118 existing + 13 new), synth PASS, `test_nr_ssb_rate_match_prod`
  PASSED (existing SSB checks + Z1–Z15, DM-RS from an independent TS 38.211 Gold generator). Logs:
  `…/build/evidence/` (not committed).
- G1: `nr-uesoftmodem` and `tests` build clean; the only warning in a touched file (`dmrs_first` maybe-uninitialized in
  `nr_pdsch_passive_decode.c`) is pre-existing on the parent.
- G2: full ctest 125/126 on the parent `a13c2b9a06` and 125/126 with the fix — same single failure
  (`test_vrtsim_cirdb`, environment), delta 0. Shuffle seeds 1/3/5 green: csirs_blind_search 131, blind_monitor
  195+2 skipped, config_sweep 44+1 skipped, prb_set 17, scrambling 11, ssb_rate_match 8, mib_handoff 6.
- G5: round 1 NOT approved (3 Important: phantom/wrong-hypothesis whole-carrier grants, an undisclosed locality
  gap, 325.8 µs; 4 Minor). Round 2 NOT approved (2 Important: co-channel neighbour passes a raw-energy gate;
  guard-band floor reads the adjacent carrier at 80/100 MHz; 5 Minor: SSB on the ZP symbol, non-dedicated grants,
  per-hypothesis cost, dropped-counter race + window static assert, report overclaims). All addressed above except
  the per-hypothesis cost dedup (documented).
- G5 round 3 (fresh reviewer, `a13c2b9a06..9c5c5cec30`): **Approved**, 0 Critical / 0 Important, 4 Minor — all
  addressed in the follow-up commit: timing/delay-spread condition stated, chance-pass per port count, prod check
  Z14 (slot 7, DM-RS symbol 3, wrong slot/symbol refused) and Z15 (DM-RS type 2), stale test comment and the
  ZP-only neighbour signal added to known limit (b). Independent probe on the production scorer (own TS 38.211
  generator): type 1 slot 7/BWPStart 10, DM-RS symbol 3, refPoint 1, ports {2,3} and {0,2}, type 2 ports {0}, {2,3},
  {4,5}: false ZP ~0.00 / true ZP 1.00 in every case.
- G4: pending (lab) — see the csirs entry in "What the lab must still run".
