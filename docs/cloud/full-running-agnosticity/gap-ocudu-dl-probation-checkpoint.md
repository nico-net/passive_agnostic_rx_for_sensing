# OCUDU-DL probation/history RED checkpoint — proposed, not implemented

Base: `f09c1bcf48100a2e4bae672e69b0de446624779b`, owned sens6 `agn-wt/gap-ocudu-dl`. Production source is unchanged. The only receiver-tree edit is five offline RED tests in `nr_csirs_blind_search_test.cc`.

## Exact source map

- `nr_csirs_blind_search.h:166–202`: candidate evidence and exported `conf_*` bank. Rejected-phase bitmap is22 words/176 bytes per candidate, covering1399 legal period/phase combinations.
- `search.c:80` / `:137`: `nr_csirs_blind_infer_period{,2}` choose largest compatible legal period from positive hits. They cannot distinguish an unmeasured smaller divisor.
- `search.c:395`: `nr_csirs_blind_occurring()` examines exported `conf_*` only.
- `search.c:412`: `zp_restart_epoch()` retains only latest hit and clears every phase-rejection bit.
- `search.c:423` / `:436`: `zp_record_rejection()` and `zp_phase_rejected()` remember occupied phases only within that resettable epoch.
- `search.c:458`: `record_hit()` infers at three distinct hits and writes `conf_*` immediately at491–507. This is the earliest export eligibility.
- `search.c:642`: `nr_csirs_blind_zp_feed_pair()` drops invalid/repeated samples, derives qualified/occupied outcomes, maintains exports, then invokes discovery. Revocation at675–693 compacts `conf_*` and clears hits, population counts, and rejected phases.
- `rt.c:329–335`: guaranteed maintenance scoring is only for `nr_csirs_blind_occurring(&g_zp,...)`; unexported probation/divisor measurements need scheduling here. Shared ordinary rotation is driven by NZP `g_st`, so a pure-core gate alone is insufficient.
- `rt.c:354` onward `score_candidate()`: scoring and terminal diagnostic outcomes; `rho<0` exits before ZP scoring near460. This checkpoint does not change that separate scoring contract.
- `rt.c:575`: real ZP feed; `:610–624` logs newly exported entries.
- `rt.c:819`: `nr_csirs_blind_rt_rate_match_all()` copies only occurring exported ZP resources into PDSCH rate-match PDUs. Preserve this boundary: probation must never enter `conf_*`.

## Rejected first draft and impossibility boundary

The initial draft “three discovery plus three held-out positives” is rejected. Unsupported live candidates accumulated400 qualified and333 occupied maintenance observations; three new positives do not establish stability. The RED burst case has eight qualified hits (three discovery plus five held-out), then two occupied, repeatedly.

No causal finite-data rule can guarantee no export before an arbitrary future contradiction while also accepting an identical finite positive prefix. Zero false exports is an empirical validation requirement, not a mathematical guarantee on unseen future traffic. Neither score traces nor current diagnostic logs establish independent Bernoulli samples; do not label a fixed hit count a calibrated false-alarm probability.

## Concrete candidate rule for review

This is a conservative stability proposal, not an approved policy or a claim it will pass live G4. It introduces no gNB/SIB/config input or deployment-specific knob. The legal-period horizon is a policy choice derived from the existing supported period table, and needs explicit review.

1. A discovery fit selects `(geometry,P,offsets)` from at least three distinct qualified slots. Freeze the proposal time. Do not add to `conf_*`.
2. Keep a separate admitted bank shared between confirmed and probation candidates, total at most existing `NR_CSIRS_BLIND_MAX_CONF=8`. Admission is round-robin when capacity permits, never by truth. Probe each admitted geometry on the union of its proposed occasions and all viable strict-divisor occasions. Multiple due hypotheses for one geometry require only one score. Total remains at most8 maintenance/probation scores plus1 ordinary discovery score per runtime call.
3. Per predicted offset, accumulate only distinct qualified observations strictly after the frozen proposal. Duplicate, invalid/setup/rho failure, population-unknown and population-suppressed raw holes are non-votes. Do not turn missing data into occupancy or credit it as coverage. An occupied predicted observation invalidates the proposal immediately and updates durable failed-run history; no export occurs from that proposal.
4. Define `L=640`, the largest currently supported legal period; `B(P)=max(CSIRS_MIN_HITS,ceil(L/P))`. Let `F[g]` be the largest **held-out** clean run previously contradicted for this geometry, including a revoked proposal's promotion count, but not an arbitrarily long established resource lifetime. Proposed promotion requires each offset to have `N=max(B(P),F[g]+1)` clean held-out observations and at least `L` absolute-slot span since proposal. `F[g]` survives discovery epoch resets, eight-hit-buffer rollover, revocation, and bank re-admission; it is cleared only with the existing cell/search reset. This is adaptive record-length stability, not a calibrated statistical test. It cannot prove zero false resources on all possible traces.
5. Retain occupied-observation timestamps for all1399 legal phases of an admitted geometry. Epoch changes do not clear these timestamps. A fit cannot reuse positive support preceding an occupied observation on one of its predicted phases. New qualification must be supported by new measurements after that contradiction, then complete independent probation. Historical contradiction does not permanently forbid that phase. The old one-bit bitmap alone cannot express this recovery safely.
6. Before promotion, enumerate every strict legal divisor `d<P` whose collapsed offsets predict additional occasions. A divisor remains viable unless an actually occupied divisor-only occasion was measured within the current supporting/held-out evidence span. If such an occasion is unknown/unobserved, withhold both the harmonic and any unsupported extrapolation to the divisor, regardless of held-out count. Qualified divisor-only observations invalidate the harmonic fit and refit from measured hits; start the smaller-period proposal's independent probation anew. Never choose a smaller period solely because it divides a larger one. If all divisor-only phases coincide with existing offsets, canonicalize the equivalent tuple instead of inventing extra occasions.
7. After promotion, retain current two-occupied maintenance withdrawal for this checkpoint. A revoked promotion contributes its accepted held-out count to `F[g]`, so restarting three-hit discovery cannot immediately regain export. Measurements of a new phase/period can recover using finite new evidence; no permanent blacklist or fixed-time expiry clears contradictions.

For two-offset resources, support/probation is required separately on both phases; negative evidence on either invalidates the joint model. Normalized equivalent periodic representations must be compared by predicted occasion sets, not raw period numbers.

## Bounds and caveats

- Worst extra phase-timestamp storage:1399×4×8=44768 bytes. Durable `F[g]` as64-bit counters for1024 geometries:8192 bytes. Proposal/hit/offset/slot metadata is bounded by8 admitted entries; no unbounded observation FIFO. Existing rejection bitmap remains176×1024 bytes until deliberately replaced. Exact struct-size assertions must accompany implementation.
- Under fully scorable observations, promotion delay after a correct proposal is at least `max(640,N×P)` slots, plus any refit; initial discovery adds at least2P for a one-offset resource. For fresh P20, N32 and delay640slots; for P40, N16 and640slots; for P640, N3 and1920slots. These are algorithmic bounds, not measured runtime convergence.
- Missing/duplicate/unscorable occasions, unresolved divisors, bank saturation or repeated contradictions give **no finite wall-clock upper bound**. Pending stays unexported. Stable reconfiguration after finite failed history requires finite N; lifetime counter saturation must fail closed, not wrap. No field measurement establishes that the chosen stability horizon is sufficient.
- This policy can reject more legitimate intermittent/noisy ZP observations and increases scheduling pressure on admitted hypotheses. Fresh integration/live evidence must measure those costs; never infer gain from diagnostic CRC.
- A future contradiction can follow any accepted stable prefix. The proposed rule addresses measured reset churn and unresolved aliasing, not that fundamental impossibility.

## Falsifiable RED tests

All call the existing production core API, not a mock policy or gNB-derived input:

1. Three P20 discovery hits do not export; a long stable positive trace eventually does.
2. Duplicate seed calls and a long interval of unknown predicted samples do not complete probation; later consistent measured samples recover.
3. Twenty eight-hit bursts (three discovery plus five held-out), each followed by two occupied occasions, never export. This falsifies the rejected three-held-out rule as well as current immediate export.
4. A previously stable P20 resource is contradicted; fresh P40/off17 measurements must reacquire without exporting at the third new hit or permanently blacklisting the geometry.
5. P80-visible positives with unknown P40-only interstitial occasions remain unexported, even after more than three held-out positives. Once P40-only occasions become qualified, a long stable continuation must choose P40, not preserve P80.

These tests set bounded positive-control deadlines to prevent a never-export implementation passing. They do not establish live zero-false performance or approve the proposed N rule. Further implementation tests must cover runtime probation scheduling, memory bounds, two offsets, overflow, geometry-union scoring and unaffected NZP behavior.
