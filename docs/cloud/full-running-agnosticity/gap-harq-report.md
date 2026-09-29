# gap-harq report

Lane `harq`, worktree `sens6:/home/sens/NICOLA/agn-wt/gap-harq`, branch `sdd/gap-harq`, forked from
`sdd/validation` @ `c295fa18f1`. Build dir configured fresh (it did not exist yet) with the same
cache options as `agn-wt/val`: `CMAKE_BUILD_TYPE=RelWithDebInfo ENABLE_ISAC_SENSING=ON OAI_USRP=ON
ENABLE_TESTS=ON ENABLE_LDPC_CUDA=ON OAI_VRTSIM=ON OAI_RF_EMULATOR=ON OAI_SIMU=ON NAS_BUILT_IN_UE=ON
NB_ANTENNAS_RX=4 NB_ANTENNAS_TX=4 T_TRACER=ON`.

## Status: DONE_WITH_CONCERNS (updated after G5 review fix round + worktree cleanup)

Gap 1 (HARQ retransmission with reserved MCS) is implemented, tested, and built clean, with a G5
review fix round addressing a blocking evidence-gating defect, a wrong UL MCS boundary in the
prose, and an undersized table (see "G5 fix round" below). Gap 2 (data-ID sweep eligibility counter)
was found ALREADY FIXED by the earlier final-review fix wave — verified, no action needed. Gap 3
(residual item 2 / scramblingID0≠PCI) is investigated and documented rather than fixed, per the
brief's own escape valve ("otherwise quantify and document") — see below for why. The cross-worktree
contamination is now fully resolved: the coordinator confirmed the foreign UL antenna-ports/DM-RS-
type-2 rework was committed on lane `gap-dmrs2` (`8940602423`), so the uncommitted copy left in this
worktree was discarded (`git checkout --`); `git status` is now fully clean and `test_nr_pdcch_blind_
monitor` is **155/155 pass, 0 failed** (the 2 pre-existing OTA-only skips unchanged) — see "Worktree
cleanup" below. Remaining concern: Gap 3 is documented, not fixed, by design (see that section).

## Commits (gap-harq worktree only)

- `a7dd19efb1` — "HARQ retransmission with reserved MCS: passive DL/UL decoders now resolve it"
  - New: `openair1/PHY/NR_UE_TRANSPORT/nr_harq_init_tx.h` (pure, header-only, no PHY/UE deps),
    `openair1/PHY/NR_UE_TRANSPORT/tests/nr_harq_init_tx_test.cc` (8 gtest cases).
  - Modified: `nr_pdcch_blind_monitor.c` (3 hunks only — see "Cross-worktree contamination" below),
    `nr_pdsch_passive_decode.c`, `nr_pusch_passive_decode.c`, `tests/nr_pdcch_blind_monitor_test.cc`
    (3 tests renamed/rewritten, 1 new test added), `CMakeLists.txt` (new test target).
- `7a9d2a23ca` — "G5 fix round: gate init-tx writes on CRC, correct UL MCS boundaries, size/counters"
  — see "G5 fix round" below for the full detail. Touches only `nr_harq_init_tx.h`,
  `nr_pdsch_passive_decode.c`, `nr_pusch_passive_decode.c`, `tests/nr_harq_init_tx_test.cc` (1 new
  test). Does NOT touch `nr_pdcch_blind_monitor.c` or `nr_pdcch_blind_monitor_test.cc` — the G5
  findings were all inside the two passive-decode files and the header.

## Gap 1: HARQ retransmission with reserved MCS

**What was there before**: a DL MCS of 29-31 (28-31 on a qam256 mcs-Table) or a UL MCS of 29-31 for
tables 0/2 (qam64/qam64LowSE) / 28-31 for tables 1/3/4 (qam256 and the two transform-precoded
tables) carries no code rate of its own by design — TS 38.214 5.1.3.1 (DL) / 6.1.4.1 (UL) require the
UE to reuse the initial transmission's modulation order and TBS. (The UL boundary above is corrected
from an earlier version of this report and the code it described, which wrongly said "27-31" — see
"G5 fix round" below.) This receiver had no history to reuse it from, so:
- `nr_pdcch_blind_monitor.c`'s DCI extraction (all three call sites: DCI 1_0, DCI 1_1, and the shared
  UL 0_0/0_1 tail) hard-rejected any grant with a reserved MCS, so such a grant never even became a
  `plausible=true` result.
- `nr_pdsch_passive_decode.c` / `nr_pusch_passive_decode.c` had matching (now-redundant, since the
  extraction gate meant they were unreachable in the reserved-MCS case) `qamModOrder==0||R==0` /
  `tbs==0` refusals.
- One correction to my own initial reading: DL's modulation order was NEVER the missing piece.
  `nr_get_Qm_dl()`/`nr_get_Qm_ul()` already return the correct value straight from the spec table for
  a reserved row (verified against source: `Table_51311[29..31] = {2,0},{4,0},{6,0}`,
  `Table_51312[28..31] = {2,0},{4,0},{6,0},{8,0}`) — only the code rate is genuinely zero there. This
  matters for the design: the fix supplies TBS/base-graph from history, not modulation order.
- DL already has a HARQ soft-combining mechanism (`harqc_entry_t`/`g_harqc`, keyed by (rnti, pid),
  with a TBS-override for a same-NDI retransmission) — but it was *unreachable* for a reserved-MCS
  grant specifically, because the pipeline refused earlier, before ever calling into it. UL has no
  combining mechanism at all (`harq_to_be_cleared` hardcoded `true`, `ulsch->harq_pid` hardcoded `0`,
  one shared `ulsch` context regardless of pid).

**Fix**:
1. `nr_harq_init_tx.h` (new): a small per-(RNTI, HARQ pid) record — `{ndi, qm, nl, bg, tbs,
   code_rate}` — of the last transport-block parameters this receiver could resolve on its own.
   `nr_harq_init_tx_record()` writes it whenever a grant's own MCS was resolvable; `_lookup()` returns
   it only if the stored NDI matches the current grant's NDI (TS 38.321 5.3.2.2's own definition of
   "same HARQ process, same data" — a mismatch, including "never recorded", means the true initial
   transmission was missed, and the caller must refuse, exactly as before this fix). Fixed-size (64,
   raised from an initial 16 — see "G5 fix round" below), LRU-evicting ("wraps" rather than growing).
   One instance each in `nr_pdsch_passive_decode.c`
   (`g_dl_harq_init`, sharing the existing `g_harqc_lock`) and `nr_pusch_passive_decode.c`
   (`g_ul_harq_init`, new small mutex — no per-RNTI mutable state existed in that file before), so a
   UL and DL HARQ pid can never collide.
2. `nr_pdcch_blind_monitor.c`'s three reserved-MCS hard rejects no longer refuse unconditionally:
   - DCI 1_0: relaxed **only** for `klass == NR_BLIND_RNTI_CLASS_C || NR_BLIND_RNTI_CLASS_TC`. SI-/
     RA-/P-RNTI never read an NDI/HARQ-pid field at all (`dci10_parse`'s switch never fills them for
     those classes) and the spec's reserved-codepoint rule is itself scoped to the dedicated classes,
     so the reject stays a hard invariant there — confirmed correct behaviour, not a gap.
   - DCI 1_1: relaxed unconditionally (format 1_1 is always a dedicated C-RNTI grant).
   - UL 0_0/0_1 shared tail (`blind_ul_finish`): relaxed unconditionally (every UL grant this receiver
     decodes addresses a C-/TC-RNTI; there is no SI/RA/P-equivalent ambiguity on the UL side).
3. `nr_pdsch_passive_decode.c` / `nr_pusch_passive_decode.c`: when `qamModOrder==0||R==0`
   (DL)/`pdu.qam_mod_order==0||pdu.target_code_rate==0` (UL), look up the init-tx record by
   `(rnti, harq_pid, ndi)`; on a hit, use its `tbs`/`bg` (and log, non-fatally, if its `nl` disagrees
   with this grant's own antenna-ports-derived layer count — the CURRENT grant's own layer count is
   kept, since it is what the transmitter's RE mapping THIS occasion actually used, not replayed from
   history); on a miss, refuse exactly as before. **Updated by the G5 fix round**: the record is
   written/refreshed only once the grant's own TB CRC has actually PASSED (not merely once its MCS
   was resolvable, as the original version of this commit did — see "G5 fix round" below), and DL
   additionally guards this with the existing `rnti_sweepable()` so SI/RA/P grants — which can never
   key a lookup anyway, since they have no NDI/pid — don't churn the table.
4. UL soft-combining is **deliberately not built**: this fix resolves TBS/base-graph for one
   independent decode attempt, which is materially smaller and safer than building a persistent
   per-(RNTI, pid) LLR buffer plus wiring it through a UL receive path that currently has no such
   concept at all (see "what was there before"). DL's *existing* HARQC combining logic needed **no**
   code changes — it already keys off the same `(rnti, pid, ndi)` triple this fix now lets it observe
   for a reserved-MCS grant in the first place, so combining across a reserved-MCS retransmission
   works as a byproduct of the gate no longer refusing the grant upstream.

**Tests**: 8 new cases in `nr_harq_init_tx_test.cc` (all passing) covering exactly what the brief
asked for — NDI toggle invalidates a stale record, a reserved-MCS lookup succeeds only against a
matching NDI (and fails on the realistic "old record, then a fresh initial TX with a new NDI, then a
retransmission" sequence unless re-recorded), missed-initial-TX is refused rather than guessed at,
distinct (rnti,pid) and (pid,rnti) pairs don't collide, and the bounded table wraps (LRU-evicts)
rather than growing, including that re-recording an *existing* key updates in place rather than
evicting.

Three pre-existing `nr_pdcch_blind_monitor_test.cc` tests pinned the now-superseded "reserved MCS is
always rejected" premise and were rewritten to the new deliberate behaviour (matching this project's
own established practice — see `progress.md` Ruling R15's precedent — rather than left broken or used
to justify reverting the fix):
- `Dci10RejectsTheReservedMcsRange` → split into
  `Dci10CrntiAcceptsTheReservedMcsRangeAsAPossibleRetransmission` (asserts the new accept) and
  `Dci10RaRntiRejectsTheReservedMcsRange` (new, pins that the non-dedicated-class invariant still
  holds — this is the one genuinely NEW assertion, since the old test never checked a non-C-RNTI
  class at all).
- `Dci01RejectsTheReservedUlMcsRange` → `Dci01AcceptsTheReservedUlMcsRangeAsAPossibleRetransmission`.
- `UlMcsBoundaryUsesActualSelectedTable` → `UlMcsExtractionNoLongerGatesOnTheReservedRange` — keeps
  pinning `nr_get_code_rate_ul()`'s own per-table boundary as a plain library-level check (that
  function is untouched and still correct), while no longer expecting *extraction* to gate on it.

**Verification**:
- `nr-uesoftmodem` builds clean (no new warnings; the two pre-existing ones this session's build log
  shows — `passive_ul_unav_res` unused-function, `dmrs_first` maybe-uninitialized — are documented in
  `final-fix-report.md` as present in the baseline too, confirmed by grepping this build's own log).
- `test_nr_harq_init_tx`: 8/8 pass.
- `test_nr_pdcch_blind_monitor`: 154/155 pass on the live (contaminated, see below) working tree; the
  one failure, `UlAntennaPortsCodePointOutsideItsTableIsRejected`, is caused entirely by the
  unrelated foreign antenna-ports change described below (it iterates `gt.mcs=10`, a non-reserved
  value my change never touches) and was **not modified** by me.
- Full `ctest`: 110/114 pass. Of the 4 failures: `test_nr_ue_ra_procedures` (Not Run) and
  `test_vrtsim_cirdb` are the two pre-existing failures `final-fix-report.md`/`progress.md` already
  document at this exact base commit; `test_nr_pdcch_blind_monitor` is the one foreign-caused failure
  above; `nr_cuup_functional_test` is new versus that baseline but is a loopback-UDP-port (2152/2153)
  functional test wholly unrelated to any file I touched (CU-UP), most likely flaky under the same
  shared-host contention this whole campaign has been fighting (other lanes' processes/ports on
  sens6) — flagged, not chased further given the lack of any plausible causal path from my changes.

## G5 fix round (commit `7a9d2a23ca`)

The coordinator relayed a G5 review of `a7dd19efb1` with three findings, all addressed:

1. **(blocking) Evidence gating.** `nr_harq_init_tx_record()` was called immediately after a grant's
   TBS/Qm/base-graph were computed — before any LDPC decode was attempted. A single blind DCI
   false-accept (a random payload whose CRC happens to mask to an in-range RNTI — the same residual
   risk `nr_pdcch_blind_monitor.c`'s mismatched-bits gate exists to suppress, not eliminate) could
   seed a bogus `(rnti, pid, ndi)` record that a genuinely later reserved-MCS grant on that process
   would then trust blindly. Fixed by moving the write to the single point each function commits to a
   verified success: `out->status = NR_PDSCH_PASSIVE_DECODE_CRC_OK` in `nr_pdsch_passive_decode.c` and
   `out->status = NR_PUSCH_PASSIVE_OK` in `nr_pusch_passive_decode.c`. I confirmed by grep that each of
   these is assigned in **exactly one place** in its file (so there is no other exit path that could
   still bypass the gate), and that the UL success point sits **after** the existing all-zero-TB guard
   (an all-zero TB passes its own CRC by construction and is explicitly excluded before reaching this
   point). A grant that itself *used* a stored record does not refresh it either — only a freshly,
   independently resolvable MCS (`have_init_tx`/`have_ul_init_tx == false`) that also then passes CRC
   does. The DL write site needed `g_harqc_lock`/`g_dl_harq_init` declared earlier in the file (see
   item 3's log-line extension below), since `nr_pdsch_passive_ldpc_stats_dump()` — which now reads
   their hit/evict counters — is defined textually before their old declaration point; moved with a
   comment explaining why, no functional change to anything else already using them.
2. **UL MCS boundary wording.** My original comments said UL reserved MCS was "28-31 (27-31 for a
   qam256 mcs-Table)" — there is no 27-31 boundary anywhere; that was a plain error on my part, not a
   simplification. Checked directly against `nr_get_Qm_ul()`/`nr_get_code_rate_ul()`'s own switch
   statement in `nr_mac_common.c`: UL table indices 0/1/2 literally reuse the DL arrays
   `Table_51311`/`Table_51312`/`Table_51313`, and indices 3/4 are the UL-only transform-precoded
   `Table_61411`/`Table_61412`. Reading each array's own reserved rows: **29-31 for tables 0/2**
   (qam64, qam64LowSE) and **28-31 for tables 1/3/4** (qam256 and both transform-precoded tables) —
   exactly matching what the coordinator's review stated. Fixed in `nr_harq_init_tx.h`'s file header
   and `nr_pusch_passive_decode.c`'s inline comment. Since `a7dd19efb1` can't be amended under this
   lane's workflow, the correction is recorded in `7a9d2a23ca`'s own commit message (and here) rather
   than silently rewriting history.
3. **Table size and observability.** `NR_HARQ_INIT_TX_N` 16 → 64: a single DL `harq-ProcessNumberSizeDCI-1-1`
   field is up to 5 bits, i.e. up to **32** live HARQ processes for a single RNTI alone — 16 shared
   across *every* RNTI and pid in the whole receiver was undersized as soon as more than one RNTI was
   live at all, before even considering one RNTI using most of its own process space. 64 matches this
   file's own `RNTI_DEC_MAX`-style sizing rationale (2 UEs at the 5-bit maximum, with headroom; cost is
   trivial — the struct is ~24 bytes). Added `hits`/`evicts` counters directly to
   `nr_harq_init_tx_table_t` (a hit on a successful lookup; an eviction only when `record()` displaces
   a *different*, already-`used` `(rnti, pid)` — filling an empty slot or refreshing an entry's own key
   are both explicitly NOT evictions) and surfaced both on each direction's existing periodic stats
   line: DL's `SENSING: HARQC ...` gains `init_tx hit=%lu evict=%lu`; UL's
   `SENSING: pusch_passive[...]` gains `init_tx[hit=%lu evict=%lu]`.

**Tests**: 1 new case, `HarqInitTx.HitAndEvictCountersMeasureExactlyWhatTheyName`, pins the exact
hit/evict semantics described above (9 total in the file now, was 8). The evidence-gating fix (item 1)
is PHY-coupled production-code policy with no standalone pure-module unit test — this matches the
codebase's own established practice for this exact class of gate (see `final-fix-report.md`'s I4:
"no test: the state is static in the PHY-coupled decode file. Verified by build and by reading"), and
is why I did not attempt to bolt an artificial test onto the pure `nr_harq_init_tx.h` module for a
policy that lives entirely in the calling PHY files. What IS verified, and stated precisely rather
than asserted: the grep confirming each success status is assigned exactly once per file (so the gate
cannot be bypassed by an untested code path), and that the pure table's own existing tests
(`MissedInitialTxIsRefused`, `NdiToggleInvalidatesTheRecord`) already demonstrate the table half of
the contract — a record that was never written because the caller correctly withheld the call is
unusable by construction, regardless of why the caller withheld it.

**Verification**: `test_nr_harq_init_tx` 9/9 pass under `--gtest_shuffle`. `nr-uesoftmodem` rebuilds
clean (same two pre-existing warnings, unchanged: `passive_ul_unav_res` unused-function, `dmrs_first`
maybe-uninitialized). `test_nr_pdcch_blind_monitor` under `--gtest_shuffle`: 154 pass, 2 skipped
(pre-existing, unrelated `PdcchReplay.*` OTA-only cases), 1 failed
(`UlAntennaPortsCodePointOutsideItsTableIsRejected`) — same single foreign-caused failure as before,
confirmed still isolated to the uncommitted antenna-ports change described below (my G5 fix-round
commit does not touch `nr_pdcch_blind_monitor.c` at all, verified via `git diff --stat` before
committing). Full `ctest`: **111/114** — one better than the previous run's 110/114, because
`nr_cuup_functional_test` PASSED on this re-run: this confirms my earlier read of it as environmental
flakiness (shared-host port/process contention on sens6) rather than anything connected to my changes,
since nothing in my G5 fix round touches CU-UP code either. The 3 remaining failures are exactly
`test_nr_ue_ra_procedures` (Not Run, pre-existing) / `test_vrtsim_cirdb` (pre-existing) /
`test_nr_pdcch_blind_monitor` (the one foreign-caused failure).

## Gap 2: data-ID sweep eligibility counter — ALREADY FIXED, no action taken

The brief named this as: "RNTIs decoded via the in-line decode path never become eligible — make
both paths update it." I verified against the current tree (both `nr_pdsch_passive_queue.c:971` and
`nr_pdcch_blind_monitor_rt.c:6543` call `nr_pdsch_passive_crc_note()`) and confirmed this is **already
fixed** — `final-fix-report.md`'s I1 section explicitly records it ("This folds in the T13 deferred
item"), and `progress.md`'s Task 13 entry that first flagged it as deferred predates that fix wave.
No code change needed or made. UL has no equivalent dual-path split to begin with (`nr_pusch_passive_
ul_crc_note()` has exactly one call site, inside `nr_pusch_passive_decode.c` itself), so there is no
analogous UL gap.

One side-effect worth recording precisely, since I checked it rather than assumed it: `nr_pdsch_
passive_crc_note()` is called **only** when `st != NR_PDSCH_PASSIVE_DECODE_ERROR && st !=
..._UNSUPPORTED` (`nr_pdsch_passive_queue.c:962`) — an `UNSUPPORTED` refusal (which is what a
reserved-MCS grant produced, before AND after Gap 1's fix, whenever no initial-TX record exists) was
and remains excluded from the walk-eligibility/link-health counters entirely, neither counted as a
pass nor a fail. Gap 1's fix does not change this exclusion rule; what it changes is that some grants
that used to be unconditionally `UNSUPPORTED` (any reserved-MCS grant with a warm init-tx record) now
reach a real decode attempt and contribute a real CRC pass/fail — strictly more, and more accurate,
signal into those same counters, never less.

## Gap 3: residual item 2 (dedicated-class failure streak, non-scrambling cause) and scramblingID0≠PCI

**Dedicated-class failure streak with a non-scrambling cause** (`final-fix-report.md` Concern 2): I
traced the counting path precisely (see Gap 2 above) and confirmed this concern is about grants that
*do* reach a full decode attempt and *do* fail CRC for a reason other than a wrong scrambling ID
(Technique D still exploring a wrong DCI 1_1 layout, or an MCS/rank limit) — reserved-MCS grants are
excluded from the counter regardless, both before and after Gap 1, so Gap 1's fix does not touch this
concern's root cause. Root-causing "Technique D still converging" as a *specific, fixable* trigger
would need either (a) plumbing Technique D's own convergence state into the scrambling-walk gate
(reintroducing exactly the circularity the final review already rejected once — convergence needs CRC
passes, which a wrong scrambling ID never gives) or (b) a live capture on a cell exhibiting the
failure mode, to distinguish it from ordinary noise. Neither is available in this lane (no OCUDU
access, `--do-ra`/phy-test/rfsim harnesses don't reproduce Technique-D-convergence-vs-scrambling-walk
interaction realistically). The original review already weighed this and accepted it as "bounded and
logged" (worst case 1024 TBs per RNTI, every step logged) rather than blocking — I did not find new
evidence to overturn that judgement, so I left the gate as `final-fix-report.md` describes it and are
documenting it here per the brief's own instruction to quantify rather than force a fix.

**scramblingID0 ≠ PCI, 1_0-in-CSS case** (`final-fix-report.md` I2's documented deviation): the code
applies N_ID^cell (PCI) rather than a decided dedicated DM-RS identity to a C-RNTI fallback DCI 1_0 in
a common search space, because — as the code's own comment states — "the monitor cannot reliably tell
a TC-RNTI from a C-RNTI in a CSS". This is not an oversight; it is a real information-theoretic limit
of blind decoding at this layer, not a bug with an available fix: the DCI 1_0 payload for a C-RNTI/
TC-RNTI class carries nothing that distinguishes the two (same field layout, same class flag). One
candidate mitigation I considered and rejected: trusting the dedicated identity once the SAME RNTI
value has strong "evidence" of being an established C-RNTI (`rnti_dec_evidence()` already exists and
tracks exactly this). I did not implement it because (a) it introduces a new, unvalidated heuristic
with its own failure mode (a TC-RNTI value re-used shortly after an established C-RNTI released it —
rare, but exactly the scenario the original conservative choice was protecting against), (b) it
touches a widely-used, already-twice-reviewed core policy function (`nr_scrambling_dedicated()`) that
also gates DM-RS ID learning, not just this one grant class, and (c) I have no way to live-validate
the change against a real scramblingID0≠PCI cell in this lane. Per the brief's escape valve, I am
quantifying rather than fixing: the failure mode costs exactly the C-RNTI-fallback-DCI-1_0-in-a-CSS
grants on such a cell (every other grant type is unaffected), and is bounded/self-limiting rather than
cascading (it does not corrupt or block any other grant class).

## Cross-worktree contamination (found by me before the coordinator's warning, then confirmed by it)

Partway through editing `nr_pdcch_blind_monitor.c` I noticed my downloaded copy (via
`scp sens6:.../nr_pdcch_blind_monitor.c` into this session's **shared** scratchpad directory —
`/tmp/claude-1000/.../scratchpad/`, which is shared across every subagent spawned by the same parent
session, evidenced by dozens of unrelated leftover files from other lanes already present there)
already contained an unrelated, uncommitted UL antenna-ports/DM-RS-type-2 rework
(`decode_dci_antenna_ports_val()`) that had NOT been present when I first read the file directly over
`ssh` minutes earlier, and was not attributable to any commit in `gap-harq`'s history. I treated this
as evidence of a filename collision in the shared scratchpad with a sibling lane's own edit-in-flight
(almost certainly `gap-dmrs2`, per its own antenna-ports/DM-RS-type-2 scope) and, **before making any
further edits**, isolated my own three intended hunks precisely: fetched the pristine
`git show HEAD:<path>`, re-applied only my 3 known transformations to it, staged that reconstructed
blob directly into git's index via `git hash-object -w` + `git update-index --cacheinfo` (bypassing
the contaminated working-tree file entirely), and committed. I verified before committing that the
staged diff contained exactly my 3 hunks and zero mentions of the foreign code, and after committing
that the only remaining working-tree modification to that file was the foreign antenna-ports hunks,
untouched and undisturbed, left for its owning lane. I did not use `git stash` at any point.

The coordinator's mid-task message confirmed the same incident from the other side: `gap-dmrs2`'s
agent found MY reserved-MCS hunks (referencing `nr_harq_init_tx.h`) inside **its** worktree's copy of
`nr_pdcch_blind_monitor.c`, reverted them there, and stashed the reverted state (commit
`f3767c9c0e`, visible from `gap-harq` too since worktrees of the same repo share one object store).
I inspected that stash (`git -C gap-harq show f3767c9c0e -- openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_
blind_monitor.c`) and confirmed it contains exactly my 3 mcs-check hunks, nothing more and nothing
I don't already have committed here — so **nothing is missing from `gap-harq`**; the contamination was
one-directional in effect (my edits leaked into `gap-dmrs2`'s copy) and `gap-dmrs2` has already
cleaned its own side up. I separately confirmed the other 6 files this commit touches
(`nr_pdsch_passive_decode.c`, `nr_pusch_passive_decode.c`, `nr_harq_init_tx.h`, its test,
`nr_pdcch_blind_monitor_test.cc`, `CMakeLists.txt`) carry **only** my own changes — checked file by
file via `git diff` line-count and content review before staging each.

**Root cause, for whoever owns the scratchpad/harness design**: the scratchpad directory is shared
across sibling subagents of the same parent session, and I used a generic filename
(`nr_pdcch_blind_monitor.c`) that a sibling lane working the same file was very likely to also use at
an overlapping time. I never used `git stash` (confirmed, per the coordinator's request). Going
forward in this lane I did not repeat the collision: every subsequent file I round-tripped through the
scratchpad (including the four files touched by the G5 fix round below) used a lane-qualified `.v2.`
suffix specifically to rule this out again, and I re-checked `git status` for a clean state in
`gap-harq` immediately before every write. Confirmed again after the G5 fix round: `nr_pdcch_blind_
monitor.c` still shows only the original foreign hunks (`git diff` unchanged from before this section
was written), and my G5 commit (`7a9d2a23ca`) does not touch that file at all — verified via
`git diff --stat` on the fix-round changes before committing them.

## Worktree cleanup (resolved, no commit made in this lane)

The coordinator confirmed the foreign hunks were `gap-dmrs2`'s own committed work, not merely
in-flight: `git -C /home/sens/NICOLA/agn-wt/gap-dmrs2 log --oneline` shows `8940602423` ("Passive
UL/DL: decode DM-RS type 2 via the shared antenna-ports reverse table") on top of the same
`c295fa18f1` base gap-harq forked from, and `git -C gap-dmrs2 show 8940602423 --stat` confirms it
touches exactly `nr_pdcch_blind_monitor.c`, `nr_pdcch_dci11_layout_sweep.c`, `nr_pdcch_ul_discovery.c`
and their tests — matching what I'd found uncommitted in `gap-harq`. That commit's own message
independently corroborates the whole incident from the other side: it explicitly notes finding "unrelated,
uncommitted HARQ-retransmission-aware MCS changes already present (referencing a nr_harq_init_tx.h
that does not exist in this tree)" and states they were carefully excluded from `gap-dmrs2`'s commit,
not reverted or committed — consistent, on both sides, with a shared-scratchpad filename collision
and nothing else.

With the foreign work safely committed on its own lane, I discarded the now-redundant uncommitted
copy from `gap-harq`'s working tree:
```
git -C /home/sens/NICOLA/agn-wt/gap-harq checkout -- openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
```
`git status` in `gap-harq` is now **fully clean** (no output at all — `git log` still shows only the
two commits from this lane, `7a9d2a23ca` and `a7dd19efb1`, on top of `c295fa18f1`; no new commit was
made for this cleanup step, per the coordinator's instruction). Rebuilt `test_nr_pdcch_blind_monitor`
(`ninja`/`lane-make.sh` picked up the reverted file and relinked the library + test binary cleanly, no
warnings). Result: **155 tests pass, 0 failed** (2 pre-existing `PdcchReplay.*` OTA-only skips,
unchanged) — up from 154 pass/1 failed. The previously-failing
`BlindPdcchTest.UlAntennaPortsCodePointOutsideItsTableIsRejected` now passes on its own, confirming
it really was the foreign, now-removed antenna-ports code causing it and nothing in either of
`gap-harq`'s two commits.

## Live validation (OCUDU gNB, sensnuc3 — not run from this lane; no radio processes started there)

**Which knobs plausibly force retransmissions**: per this project's own prior measurement
([[harq-combining-has-nothing-to-combine]], `BRANCH_IMBALANCE_HARQ_PLAN.md`), the live OCUDU config's
`max_ue_mcs: 10` against `olla_target_bler: 0.1` was measured to drive the retransmission rate to
~0% (615/615 grants at rv=0 in a 30 MB log window) because OLLA never needs to back off from a
capped, easily-achievable MCS. Raising `max_ue_mcs` toward 25 (removing the cap) should let OLLA push
MCS high enough that real transport blocks fail and get retransmitted, at the cost of moving into the
MCS range this project's own decoder is separately known to be weaker at (per that same memory entry)
— the two effects are not independent, so expect retransmissions to appear alongside some drop in
overall CRC rate, not for free.

**The open, unverifiable-from-this-lane question**: whether the OCUDU scheduler actually signals the
*reserved* MCS codepoints (29-31, or 28-31 on qam256) on a retransmission, versus simply re-signalling
the same real MCS value again (both are spec-legal; only the former exercises this fix at all). I do
not have OCUDU scheduler source access in this task and did not find this documented elsewhere in the
repo's docs. This is the single most important thing for whoever runs live validation to check FIRST
— if the scheduler never emits a reserved codepoint at all on this deployment, this fix is validated
only in the negative (it changes nothing, since the old and new code paths agree on every grant this
cell ever sends) and a positive test would need a scheduler/deployment that does use them.

**Expected log-line evidence, if reserved-MCS retransmissions do occur**:
- Before this fix: every such grant hits `SENSING: PDSCH UNSUP@1423` (DL) with no corresponding
  `SEGDIAG`/`TBPARM` line ever printed for that grant (it never reached that far in the pipeline).
- After this fix, with a warm initial-TX record for that (RNTI, pid): the grant instead reaches full
  decode, and with `ISAC_PDSCH_TBPARM=1` set, a `SENSING: SEGDIAG mcs=29 ... A=<nonzero> ...` (or
  `TBPARM ... mcs=29 ... tbs=<nonzero> ...`) line appears — something that could not happen at all
  before. A `SENSING: PDSCH reserved-MCS retx rnti=... layer count changed ...` warning would appear
  only if that specific grant's own antenna-ports field implies a different layer count than the
  recorded initial transmission — informational, not an error.
- Without a warm record (e.g. the very first grant on a HARQ process is itself, implausibly, a
  reserved MCS — or this receiver missed the real initial transmission): still
  `SENSING: PDSCH UNSUP@1423`, unchanged from before.
- UL mirrors this with `"reserved UL MCS with no known initial transmission on this HARQ process"` as
  the `reject_reason` on a miss, and the same `SENSING: PUSCH reserved-MCS retx ...` warning on a
  layer-count mismatch.

No live captures were run against this deployment from this lane (rule: no radio processes on
sensnuc3 from here).
