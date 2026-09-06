# Phase 3 — recover the dedicated config by search — HANDOVER

**Status (REVISED 2026-09-06 — READ THIS FIRST, IT REVERSES THE ORIGINAL CONCLUSION BELOW):
Technique A DOES converge on live air once the convergence CRITERION is fixed. The original
"Technique A does not converge" finding was correct as a symptom but wrong on cause: it blamed
the correlation math / receive chain, when the actual defect was the convergence ALGORITHM
(required one 6-RB window to win 5 consecutive calls, which a busy wideband CORESET's real,
CCE-hopping occupancy pattern essentially never does). See "REVERSAL (2026-09-06)" below before
reading the rest of this document, which is the ORIGINAL (now superseded) investigation record,
kept for the method and the still-valid eliminations.**
**Branch:** total-passive-rx-UL-DL-graphics
**Host:** sens6 · **Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue

## FULL ADAPTIVE PASSIVE RX (DL + UL) — COMPLETION STATUS (2026-09-06)

**Overall: ~25-30% of a fully automatic, no-manual-config, no-ground-truth-log passive receiver.**
DL is the more advanced half (~55-60% of ITS OWN scope); UL adaptive discovery has not been
started at all (0%) — everything UL-side today is hand-solved against this one cell's own gNB
scheduler log, which will not exist on a real deployment. This section is the authoritative,
detailed status; the sections below it are the investigation history that produced it.

### DL (dedicated PDCCH → DCI format 1_1 → PDSCH), by step

| Step | What it does | Status |
|---|---|---|
| 1. CORESET footprint | DM-RS correlation, histogram-accumulated across occasions (Technique A) | **IMPLEMENTED, LIVE-VALIDATED.** Converges to the exact known-good footprint (`rb_offset=0`) repeatedly; span coverage varies with how much real traffic is seen in the observation window (216-252/270 RB in good captures). |
| 2. dci_length discovery | Accumulating CRC-based length sweep (Technique C, rewritten 2026-09-06) | **IMPLEMENTED, UNIT-TESTED (5/5), STILL NOT LIVE-CONFIRMED FINDING A REAL LENGTH -- and "waiting for DL traffic" is no longer the explanation.** Real DL PDSCH traffic is CONFIRMED present (~30% of slots, see "Traffic-generation architecture" below -- an earlier "zero DL traffic" claim here was a grep bug, retracted), yet the sweep still gives up after 500 occasions on every capture. Genuine open problem, not an external dependency. |
| 3. C-RNTI bootstrap | Persistence-based RNTI confirmation from `run_occasion()`'s own accept path (Technique B) | **IMPLEMENTED, STRUCTURALLY SOUND, NEVER OBSERVED FIRING LIVE.** Cannot produce a sighting until Step 2 has already found the right length and produced at least one real accept — a real chicken-and-egg gap, mitigated (Step 2 no longer *needs* it, since it can bootstrap from CRC alone) but not eliminated. |
| 4. Genuine accept / FULLCRC decode counting at the bootstrapped RNTI | Shared decode/accept machinery, same code path the manual conf already proves works | **CODE PATH EXISTS AND IS PROVEN IN ISOLATION** (the manual conf gets 20-48% TB decode rates through the identical decode/accept logic) **but the full autodiscover chain (1→2→3→4 all succeeding together, unattended) has never been observed end to end.** This is the actual "is Phase 3 done" question, and it is open. |

**What's needed to close DL, UPDATED 2026-09-06 — the "just needs real DL traffic" hypothesis is
now falsified.** Real DL PDSCH traffic was confirmed present (~30% of slots) for a fresh 3x200s
capture, and Step 2 STILL gave up after 500 occasions with zero accepts on every try. The
accumulation redesign itself needs revisiting: candidates the chance-pass-rate assumption baked
into `CHANCE_PASS_RATE=1/256` may not hold for this exact CORESET/candidate config; the
observation window/give-up cap may need retuning against a REAL traffic pattern rather than the
synthetic one the unit tests use; or there is a genuine bug elsewhere in the autodiscover chain
(e.g. the candidates the sweep is fed may not actually correspond to slots/CCEs carrying the real
DL traffic that IS present, an indexing or timing mismatch between what Technique A/the RT tap
scans and what the scheduler is actually using this occasion). Not yet root-caused.

### UL (DCI format 0_1 → PUSCH), by step

| Step | What it does | Status |
|---|---|---|
| 1. CORESET footprint | — | **IMPLICITLY SHARED with DL** (Technique A's correlation doesn't distinguish DCI format), but never separately exercised/scored for UL specifically. |
| 2. UL DCI length discovery | — | **NOT IMPLEMENTED.** No UL equivalent of Technique C exists. Mechanically straightforward to add (port the same accumulating-sweep mechanism to format 0_1 candidates — needs a `decode_one_candidate` adapter for 0_1, mirroring the existing 1_1 one; the sweep/significance-test code itself is format-agnostic). |
| 3. UL field-BOUNDARY / semantic recovery (which bits are MCS vs antenna-ports vs TDRA-index, etc.) | — | **NOT IMPLEMENTED, AND NOT FEASIBLE with the CURRENT method on a real commercial cell.** Today's UL config (`pdcch_blind_monitor_ul_dci_bits` etc.) was solved OFFLINE by replaying 17812 real payloads and cross-checking candidate field-width assignments against the gNB's OWN logged scheduler ground truth (h_id/ndi/rv/mcs/tpc/dai/mimo/ant) until one assignment matched consistently. A real/commercial gNB exposes no such log. See "Why this is structurally hard" below. |
| 4. UL C-RNTI bootstrap / genuine accept counting | — | **NOT WIRED**, though Technique B's existing bootstrap RNTI could in principle feed UL scoring too (same RNTI, same UE) once (2)+(3) exist. |

### Why UL field-boundary recovery is structurally harder than DL length recovery

Total-length recovery (DL Step 2, and the UL Step 2 that could be built the same way) has a hard,
free oracle: a polar-coded payload's CRC-24 either verifies or it doesn't, and that is a property
of `(received signal, hypothesised length)` ALONE — accumulating trials and comparing against the
expected chance-pass rate is enough to find the right length with no external ground truth needed
(this is exactly what the 2026-09-06 rewrite does).

Field BOUNDARIES have no equivalent oracle. The CRC is computed over, and verifies, the SAME raw
bit sequence regardless of how that sequence is later carved into named fields — two different
field-width assignments of an IDENTICAL, CRC-verified bit sequence produce two different
"MCS"/"HARQ ID"/etc. values that are EQUALLY CRC-valid. This project's own prior UL solve
demonstrates this directly: "of 3963 assignments summing to 43, exactly 60 satisfied every gNB
invariant... and all 60 decode IDENTICALLY, because they differ only in where zero-valued padding
sits" — i.e. even WITH gNB ground truth to filter against, multiple splits were provably
indistinguishable and had to be broken by spec-plausibility assumption, not measurement. Without
that ground truth at all, there is nothing to filter the search with.

DL 1_1 mostly avoids this problem because TS 38.212's canonical field ORDER is public and most
field WIDTHS are derivable from carrier parameters a passive receiver can already discover (BWP
size via Technique A, antenna config from what it observes) — the sweep only has to patch a small
residual (1-3 bits, from the ONE thing that formula can't know: the RRC-configured TDRA table
size). UL 0_1 has MORE such RRC-configured-only unknowns (SRS-resource-count-dependent SRI width,
CSI-trigger-state-dependent CSI-request width, frequency-hopping flag presence, antenna-port table
selection), so the un-derivable residual is bigger, not smaller, and total length alone is not
enough information to pin every field boundary uniquely.

### Possible path to closing UL (not started, real engineering effort, not a small extension)

1. **UL length discovery** (tractable, same mechanism as DL): port the 2026-09-06 accumulating
   sweep to DCI format 0_1 candidates. No algorithmic changes needed.
2. **Field-boundary recovery without a gNB log — replace the ground truth, don't remove the
   check.** The gNB's logged scheduler decisions were used to verify a candidate split's decoded
   field VALUES were self-consistent over many grants. The same self-consistency check can be
   built from OBSERVATIONS THE RECEIVER CAN MAKE ITSELF, using the PUSCH transmission that a UL
   grant actually triggers as the ground truth instead of an internal log:
   - Blind RB-occupancy detection on the resulting PUSCH: does the frequency range the receiver
     actually sees ENERGY/DM-RS on match the RB allocation a candidate RIV-field-width
     interpretation predicts?
   - Blind modulation/MCS classification: does the observed constellation/TBS on that PUSCH match
     a candidate MCS-field interpretation?
   - DMRS pattern cross-check: does the observed DMRS symbol position/sequence match a candidate
     antenna-ports/DMRS-config field interpretation?
   None of these DSP primitives exist in this codebase today. This is comparable in scope to the
   existing NR_UE_ISAC channel-estimation/CFR work, not a small addition — it is the single
   largest remaining gap for a genuinely commercial-capable (no ground truth, no manual per-cell
   config) adaptive receiver, DL or UL.
3. **Residual RRC-only unknowns even after (2)**: some UL field widths (SRS-resource-count,
   CSI-trigger-state-count) depend on configuration a passive receiver cannot observe even via
   PUSCH cross-checking (e.g. an SRS resource set's own configuration, or CSI-RS trigger states) —
   closing this fully would need blind discovery of THOSE configs too, an even deeper problem,
   flagged here but not scoped further.

### Traffic-generation architecture (baseline, corrected 2026-09-06)

The active UE is a real phone attached to this gNB's cell (not an OAI test-harness UE), running a
bidirectional/reverse-mode iperf3 test to an EXTERNAL server on the internet — there is no local
iperf3 server in this picture (a local `iperf3.service` on sens4 was found unused throughout this
whole investigation and has been stopped).

**"Zero DL PDSCH traffic" (an earlier version of this section, and of "Steps 2-4 attempt" below)
IS RETRACTED 2026-09-06 — it was a measurement bug, not a fact about the cell.** The grep pattern
used to check `gnb.log`'s own `Slot decisions ... (N PDSCHs, N PUSCHs, ...)` summary line required
the LITERAL plural `PDSCHs` (trailing 's'); this gNB's log uses ENGLISH SINGULAR/PLURAL GRAMMAR
(`"1 PDSCH,"` with no 's', `"0 PDSCHs,"`/`"2 PDSCHs,"` with one) — so every slot carrying EXACTLY
ONE PDSCH silently failed to match and was dropped from the count entirely, rather than counted.
Re-measured with a corrected pattern (`PDSCHs?`) over a properly time-bounded window (a
BYTE-count `tail` on this log can span under 2 seconds of real time despite being tens of MB, due
to extremely verbose per-symbol fronthaul debug lines — size the window by checking its own first/
last timestamp, not by assuming N bytes = N seconds): **~30% of sampled slots (2181/7372 in one
21-second window) DO carry a real DL PDSCH grant.** DL traffic was very likely present through
some or all of this investigation's earlier "zero DL traffic" captures too — that framing should
not be trusted for anything captured before 2026-09-06's correction.

**Re-run after the correction: STILL zero accepts (`VOID_DL_ZERO`, 3/3 tries, `accepts=0`,
`dci_length sweep gave up after 500 occasions`) with DL traffic now CONFIRMED present.** This
means the actual remaining blocker is a genuine decode/config problem, not absent traffic as
previously concluded — re-opened, not yet root-caused. See the top status callout of this document
for the current investigation state.

---

## REVERSAL (2026-09-06)

Re-opened this investigation with live gNB+iperf traffic available, working through the two
"Still open item 1" experiments below in priority order.

1. **Branch-FO hypothesis (still-open item 1's second bullet): REFUTED, measured.** Added a
   diagnostic logging `nr_ue_get_branch_fo_hz(0)` (the per-branch FO term `nr_slot_fep_ant()`
   applies — added 2026-09-03 for an unrelated branch-coherence fix — that plain `nr_slot_fep()`,
   the proven-working candidate-scan path, never applies). It read exactly `0.00` throughout a
   live run. Not the cause.
2. **Direct cross-check, decisive: Technique A's correlation math is CORRECT.** Added a second
   diagnostic (`XCHECK`, in `nr_pdcch_blind_monitor_rt.c`'s `run_occasion()`) that runs
   `nr_pdcch_coreset_map_scan()` — the SAME function Technique A calls — on the FEP output of the
   proven-working MANUAL-conf decode path (known-correct config, genuinely decodes real DCIs).
   Result: correlation **0.88-0.999**, constantly, across the whole run. This directly contradicts
   the original finding below ("0.02-0.39... at or below the pure-noise floor") — the math, the
   pilot generation, and the RE-indexing formula are all fine.
3. **Re-ran the actual autodiscover path with a finer per-call diagnostic: it is ALSO finding
   strong hits, not zero.** `DISCOVERDIAG` (unchanged instrument) now shows `n>0` with
   `top_corr` in the 0.9-0.999 range on most calls — a different picture from the original
   capture's flat `n=0`. The signal-chain health that the original investigation blamed evidently
   improved between that capture and this one (plausibly the CFO/branch-coherence work visible
   elsewhere in this project's history around the same window) — but that turned out not to be
   the reason it still failed to converge.
4. **Root cause of the non-convergence, found: the winning window legitimately changes call to
   call, so "same window 5 times running" (`AUTODISCOVER_STABLE_VOTES`) essentially never fires.**
   Measured over one 90s capture (19,663 calls): the `top_rb` distribution spans ~20 distinct 6-RB
   windows, all multiples of 6 inside the real CORESET's true 0-269 RB span, with the single most
   common window (RB 204) topping only ~19% of calls (3807/19663) — several others (RB 6, 48, 192,
   174, 144, 210) close behind. RB 0, the location the old stability-vote design implicitly favours
   as "the" footprint, was the LEAST-hit real position (39/19663). At a ~19% per-call top-win rate,
   a 5-in-a-row streak has probability ~0.19^5 ≈ 2.5e-4 per call — consistent with never being
   observed in a 90-200s capture. This is exactly what a real, BUSY, WIDE (270 RB) dedicated
   CORESET looks like: different grants use different CCEs, so no single 6-RB window dominates a
   short window of calls, even though the underlying signal is real and strong.
5. **Fix: replaced the stability vote with histogram accumulation.** Instead of requiring one
   window to win `AUTODISCOVER_STABLE_VOTES` consecutive calls, `nr_pdcch_blind_monitor.c` now
   accumulates a per-window hit count over `AUTODISCOVER_OBS_CALLS=1000` calls (~4-5s dwell) and
   declares the confirmed span as `[first, last]` of every window that cleared the threshold
   `AUTODISCOVER_MIN_HITS=3` times or more in that window — the same "instantaneous vote" →
   "observed history" shift this project's own `rnti_persistence_check()` already uses for RNTI
   sightings, just with a longer dwell (a CORESET's occupied windows shift call to call; an RNTI
   does not).
6. **Live-validated: converges, and lands on the exact known-good answer.** Two consecutive
   90-150s captures (`xcheck3_143723`, `xcheck3_144011`) both produced
   `SENSING: Phase 3 autodiscover -- CORESET footprint rb_offset=0 span_rb=216 bootstrap_rnti=0x0`
   — `rb_offset=0` is an EXACT match to ground truth (dedicated CORESET starts at RB 0);
   `span_rb=216` (36 of the true 45 windows) is a partial-coverage under-estimate, not a wrong
   answer — the 1000-call/~4-5s observation window does not necessarily see every one of the 45
   windows get used at least 3 times, so the confirmed span is a (correct, conservative) LOWER
   BOUND on the true footprint rather than its exact extent. `bootstrap_rnti=0x0` at the moment of
   convergence is expected and inert (see still-open item 5 below, unchanged): Technique B can only
   start accumulating sightings once `run_occasion()` starts running, which happens only AFTER
   Step 1 converges — a one-time chicken-and-egg gap at the exact convergence instant, not a
   standing defect.
7. **Both validating captures were `VOID_DL_ZERO`** (this rig's own verdict for zero PDSCH
   decodes that window — a known, Technique-A-unrelated CFO/link-health bimodality this project's
   CLAUDE.md documents extensively) — SIB1 still decoded in both (`sib1=1`), consistent with the
   receive chain being healthy enough for PDCCH/CORESET#0 but not for this particular window's
   dedicated PDSCH. **Steps 2-4 (dci_length score, FULLCRC decode count at the bootstrapped RNTI)
   still need a VALID run to actually exercise** — Step 1 (footprint) is now proven; Steps 2-4 are
   unblocked in principle (the code path is reachable) but not yet scored on a healthy capture.
   One `dci_length sweep found nothing significant` line was observed on the `VOID_DL_ZERO`
   captures, consistent with the sweep needing real PDSCH decode attempts it didn't get that
   window, not a new defect.

**What this means for "Still open" below**: item 1 is RESOLVED (branch-FO refuted, root cause
found and fixed, live-validated). Item 5 stands largely as originally written but is now less
severe — bootstrap_rnti reaching a nonzero value is now actually reachable post-convergence rather
than permanently inert. Item 6 (unbounded per-slot cost while unconverged) is UNCHANGED and, if
anything, more relevant now that convergence takes a deliberate ~1000-call/4-5s dwell rather than
failing fast. Items 2-4 stand as written, with item 2's "cannot run until (1) is resolved" now
literally true in the good sense — (1) is resolved, so Steps 2-4 are unblocked, just not yet
scored on a VALID run.

## AGGREGATION LEVEL — the real blocker, found 2026-09-06 evening. READ THIS FIRST.

**This cell sends 100% of its DL DCI 1_1 grants at AGGREGATION LEVEL 1, and the scanner was not
looking at AL1 at all.** Ground truth is the gNB's own PDCCH PDU log line:

```
cce_index=4 dci_aggregation_level=1 payload_size=47 nid_pdcch_data=2 nid_pdcch_dmrs=2
nrnti_pdcch_data=0 freq_domain_resource=<45 ones> NON INTERLEAVED reg_bundle_sz=6
```

`dci_aggregation_level=1` on **5277 of 5277** sampled DL grants (payload_size=47). Zero at any
other level. This matches CLAUDE.md's own memory `blind-pdcch-needs-al1-scanning` ("cell sends
99.997% at AL1") — which an earlier revision of this document wrongly overrode.

**Two distinct defects, both confirmed live:**

1. **AL1 was explicitly DISABLED** in both the manual ground-truth conf
   (`pdcch_blind_monitor_ss = "1:0:1:0:-1:0:0:0"`, AL1=-1) and — after a mistaken "match the manual
   conf" edit earlier the same day — in the autodiscover path too. With AL1 disabled the scanner
   cannot see a single real grant on this cell. Symptom: **zero accepts across 312,000 occasions**
   while Technique A simultaneously reported 0.98-0.99 DM-RS correlation. That combination —
   strong correlation, zero accepts — is the signature of this exact mistake.
2. **`ss_al_candidates[AL1] = 0` ("auto") is NOT sufficient, and this is a real allocator bug.**
   The adaptive budget splitter allocates in the order AL2, AL4, AL8, **AL1 last**, against a
   64-candidate / 8192-RE cap. Measured ladder line with AL1 on "auto":
   `(AL1=0 AL2=13 AL4=6 AL8=3)` — **AL1 received ZERO candidates**, i.e. "auto" silently produced
   the same behaviour as "disabled". Only by disabling AL2/4/8 does AL1 get the budget:
   `(AL1=45 AL2=0 AL4=0 AL8=0) accepts_per_al=[593 0 0 0]`.

**Conf that actually scans this cell** (`nrue.passive_rx.al1.conf`):
`pdcch_blind_monitor_ss = "1:0:1:0:0:-1:-1:-1"` — AL1 auto (full 45-CCE sweep), AL2/4/8 disabled.

**Timeline that explains "it worked before":** every working capture predates the gNB restart at
**17:03:46** (xcheck1 14:25 → 11475 accepts / 82.8% PDSCH CRC; control1 15:26 → 2801 / 48.6%).
After that restart the scheduler settled on AL1 and every capture — manual conf included — dropped
to zero. The receiver did not regress; the cell changed.

### The 2026-08-20 "pointing problem" is RESOLVED — both halves re-measured 2026-09-06

The memory `four-rx-mrc-skip-blocks-passive-pdcch` concluded on 2026-08-20 that "the
dedicated-CORESET extraction is not reading the REs that carry the grants... a pointing problem:
wrong REs, wrong symbol, or wrong slot alignment", and prescribed one never-executed next step.
Both halves have now been measured and **neither reproduces**:

**Slot alignment — correct.** Correlating the gNB's own dedicated-grant slot distribution against
this receiver's per-slot `ENERGYPROBE` ratio (452 samples/slot):

| slots | gNB grants each | our mean AL1 energy |
|---|---|---|
| 0-7, 10-17 (DL) | 294-471 | **10.9-16.2x floor** |
| 8, 9, 18, 19 | **none** | **4.2-7.1x** |

Elevated energy lands exactly on the grant-bearing slots and drops in the slots with no grants.
The 2026-08-20 observation (flat energy at the gNB's busiest slots 14/15, elevated only at
`csirs_monitor` offsets) does NOT reproduce — slot 15 is now 13.45x and slot 14 is 11.44x.

**RE-level indices — correct.** The prescribed `ISAC_PDCCH_CFGTRACE=1` comparison (log tag is
`PDCCHCFG`, not "CFGTRACE") against the gNB's own PDCCH PDU line:

```
PDCCHCFG s=5 ss=0 type=1 n_rb=270 cset_start=0 rb_offset=0 dmrs_ref=0 scr_id=2
         BWPStart=0 BWPSize=273 dur=1 bundle=0 ilv=0 shift=0 ncand=45
```

Every field matches ground truth: 270 RB / 45 groups, BWP [0..273), symbol 0 (dur=1),
non-interleaved (bundle=0, ilv=0, shift=0), `nid_pdcch_dmrs=2` -> scr_id=2, type=1
(PDCCH_CONFIG = dedicated), 45 AL1 candidates. **Stop suspecting the extraction geometry.**

### Still open after the AL1 fix (do NOT re-close these as "traffic" or "link budget")

With AL1 swept (45/45 CCEs every occasion), accepts appear at AL1 (`accepts_per_al=[593 0 0 0]`)
but **PDSCH CRC stays at 0.0%** and only a handful of accepts reach PDSCH decode
(`pdsch_decode[try=14 crc_ok=0 skip_rv=249]`). Two explanations were tested and BOTH REFUTED:

- **Not a wrong TDA field width.** The gNB does use a third time-domain allocation
  (`symb=[2..14)`), but it belongs to `rnti=0xffff` (SI-RNTI) on `bwp=[0..48)` — the CORESET#0
  COMMON path, not the dedicated 273-RB BWP. The dedicated TDRA list is still 2 entries, so the
  TDA field is 1 bit and `dci_length=47` (which matches the gNB's own `payload_size=47`) is right.
- **Not a link-budget limit.** `ENERGYPROBE` measured AL1 candidate energy at **7.2-15.5x the
  noise floor** (`floor=1.16 al1=15.51`). The AL1 PDCCH is strong and unambiguously present.

So: correct geometry, correct length, correct scrambling, strong signal, AL1 swept — and the
decode still fails. That points at the blind PDCCH RX chain for AL1 specifically (demap →
unscramble → polar decode), and is the next thing to investigate. Related prior art:
CLAUDE.md's `sib1-oracle-proves-pdcch-rx-chain-broken` memory. Note `sib1=1` in the run verdicts
does NOT exonerate this path — SIB1 is acquired by the stock OAI receive chain, not by the blind
monitor.

## Steps 2-4 attempt (2026-09-06, same day): three more real bugs found and fixed, then blocked
## on missing DL traffic — read this before attempting Steps 2-4 again

Disabled `[sensing] enable` (ISAC+AoA pipeline) in `nrue.passive_rx.autodiscover.conf` per this
file's own header comment (it had been left ON from an earlier, unrelated measurement, and its own
header already documented the cost: 10847 RF discontinuities / 672 SIB1 NACKs per 120s with it on
vs 100/0 off). This alone did not fix the `VOID_DL_ZERO` (zero dedicated PDSCH decodes) result
every autodiscover capture was hitting — three further real defects were found and fixed in the
same investigation, all live-measured, before the actual remaining blocker (missing DL traffic,
external to this code) was identified:

1. **`coreset_shift_index` was leaking `pci` instead of the manual conf's ground-truth `0`**
   (already committed as a small preceding fix, `5808e7397f`-equivalent commit in this same
   session) — confirmed inert for Technique A (not one of its parameters) but real drift from the
   established manual config, same class as the `coreset_interleaver_size` fix from the prior
   review wave.
2. **AL1 was left "auto" instead of matching the manual conf's explicit AL1-DISABLED setting.**
   `nr_pdcch_blind_monitor.c`'s autodiscover block set `ss_al_candidates = {0,0,0,0}` (all-auto)
   with a comment asserting AL1 is this cell's dominant level — but the PROVEN-WORKING manual conf
   (`tests/passive_rx/ota/nrue.passive_rx.conf`) explicitly sets AL1=-1 (disabled), and CLAUDE.md's
   own later §10 records this gNB's dedicated SS as `nrofCandidates: AL2=2, AL1/4/8/16 all 0` — the
   comment this replaces was stale. On a 270 RB/45-CCE CORESET, auto-scanning AL1 (up to 45
   candidates) can alone exhaust the shared candidate/RE budget (see the allocation-order comment
   in the code), starving AL2/4/8 where real grants land. Fixed to `{-1,0,0,0}`, matching ground
   truth exactly. Live-measured before the fix: zero genuine PDCCH accepts across three 200s
   captures despite Technique A confirming strong, real DM-RS energy throughout.
3. **The dci_length sweep (Technique C) was structurally unable to succeed on live air, root cause
   found via the gNB's own scheduler log, not guessed:** `nr_pdcch_blind_dci_size()`'s fallback
   formula returns 48 for this bwp_size, but the PROVEN-WORKING manual conf needs 47 (this cell's
   DCI field widths shifted after the documented 2026-08-28 4x4/QAM256 reconfiguration — an older
   CLAUDE.md entry's "48 confirmed via the gNB's own log" predates that change and no longer
   applies). Since the sweep never overrides the fallback, EVERY decode attempt in an entire
   200s/461000-occasion capture used the wrong length — measured via the monitor's own summary
   line: `held[energy=10063801 persist=0 snr=0 mismatch=0]` (the energy gate passes plenty of real
   candidates through, proportionally matching the working manual-conf run) yet `accepts=0` and
   `last_reject="CRC-recovered value outside plausible RNTI range"` on literally every one of
   78199 real decode attempts — the exact signature of a length-misaligned decode, not a dead
   channel.
   - **Deeper cause of why the sweep itself never found 47: it was a ONE-SHOT test scored from a
     SINGLE occasion's ~20-40 candidates**, with `trial_idx % n_cand` cycling the SAME small real
     set to fill a fixed 64-trial budget. Without a bootstrap RNTI (which cannot exist yet — see
     item 5 above), significance required >=3 real decodable candidates in that ONE occasion; at
     this cell's own measured ~2% real accept rate (2801/138000 on the working manual-conf path),
     one occasion essentially never contains enough real exposure.
   - **Fixed by rewriting Technique C to accumulate across many occasions**, mirroring the same
     "instantaneous vote" -> "observed history" shift already applied to Technique A:
     `nr_pdcch_dci_length_sweep_feed()` replaces the old one-shot `nr_pdcch_dci_length_sweep()`,
     called once per candidate-bearing occasion, accumulating per-length `(trials, passes,
     bootstrap_hits, distinct payload hashes)` in a persistent `nr_pdcch_dci_length_sweep_state_t`.
     This introduces a NEW trap the old design never had to guard against: a FIXED absolute pass
     floor (the old `MIN_SIGNIFICANT_PASSES=3`, calibrated for ~64 total trials) is trivially
     clearable BY CHANCE ALONE once accumulated trials per length reach the hundreds (at this
     project's own measured ~1/256 chance-pass rate for the `plausible` gate, ~700 accumulated
     trials already has an expected ~2.7 chance passes). Fixed by scoring against a floor that
     SCALES with accumulated trial count — `mean + 6*sigma` of the expected chance-binomial
     distribution — rather than a constant. Bootstrap-hit significance is unaffected (still
     near-certain regardless of sample size). 5/5 offline tests pass, including two new regression
     cases added specifically for this rewrite: one proving accumulation finds a real-but-sparse
     signal a single occasion could not, one proving the scaled test does NOT false-trigger even
     after ~800 accumulated pure-noise trials/length (the exact scenario that would have broken the
     old fixed floor). Bounded by a new `AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS=500` give-up cap,
     replacing the old one-shot "try exactly once" bound.
4. **Also disabled the UL DCI 0_1 / passive-PUSCH pipeline** (`pdcch_blind_monitor_dci01` and
   everything gated behind it) in the autodiscover conf, matching the manual conf's own leaner
   config, after measuring 0/10 valid captures on the autodiscover conf across four attempts in
   this session (2 explicit CFO mislocks, the rest zero-accept) vs 4/4 valid manual-conf control
   captures in the SAME session window — pointing at conf-specific load as a live confound, not
   pure rig-luck. This did NOT, by itself, change the outcome (see below) — the actual remaining
   blocker was found separately.

**Live-validated after items 1-2 (AL1 + shift_index): Step 1 still converges correctly**
(`rb_offset=0, span_rb=252` on one capture — even wider coverage than the earlier reversal's
216 — confirming the AL1 fix does not regress Technique A). **Step 2's rewrite is unit-tested
(5/5) but NOT yet live-confirmed finding a real length** — every attempted live capture after the
rewrite hit `VOID_DL_ZERO`/`VOID_CFO_MISLOCK`/gave up after the bounded 500-occasion cap.

**"ZERO dedicated DL PDSCH traffic" (this section's own claim, several paragraphs, dated
2026-09-06 earlier the same day) IS RETRACTED.** It was a grep bug, not a fact about the cell: the
pattern used required the literal plural `PDSCHs` (trailing 's'), but this gNB's log uses English
singular/plural grammar (`"1 PDSCH,"` with no 's'), so every slot with EXACTLY ONE PDSCH silently
failed to match and was dropped from the count rather than counted as nonzero. Re-measured with a
corrected pattern (`PDSCHs?`) over a properly time-bounded window (a byte-count `tail` on this log
can span under 2 seconds of real time despite being tens of MB -- verify the window's own first/
last timestamp, never assume N bytes ~ N seconds on this log): **~30% of sampled slots (2181/7372
in one 21-second window) DO carry a real DL PDSCH grant.** DL traffic was very likely present
through some or all of the "declining traffic" narrative above too -- do not trust any DL-traffic
absence/decline claim from earlier in this document dated before this correction.

**Traffic-generation architecture (this part of the correction stands): the active UE is a real
phone attached to this gNB's cell, running a bidirectional/reverse-mode iperf3 test to an EXTERNAL
server** -- no local iperf3 server on sens4 is involved (one WAS found running there, unused
throughout this whole investigation, confirmed via `ss`/`systemctl` to have zero established
connections; stopped 2026-09-06, left enabled at boot).

**Re-run after the traffic correction: STILL zero accepts.** Three fresh 200s captures, all
`VOID_DL_ZERO`, `accepts=0`, `dci_length sweep gave up after 500 occasions` -- with DL traffic now
CONFIRMED present via the corrected measurement. **This means the actual remaining blocker is a
genuine decode/config problem, not absent traffic. Re-opened, not yet root-caused** -- do not
re-close this as an external/traffic issue without new evidence.

## Files changed by this follow-up (2026-09-06, on top of the reversal commit)

- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` — AL1 disable fix (item 2 above).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h`/`.c` — accumulate-across-occasions
  rewrite (item 3 above); `nr_pdcch_dci_length_sweep()` (one-shot) replaced by
  `nr_pdcch_dci_length_sweep_reset()`/`nr_pdcch_dci_length_sweep_feed()` (stateful).
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc` — rewritten for the
  stateful API; 5 tests (was 3), including the two new regression cases described above.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` — call site updated to feed the
  sweep every candidate-bearing occasion via persistent static state, with the new
  `AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS=500` bounded give-up.
- `/home/sens/NICOLA/nrue.passive_rx.autodiscover.conf` (NOT in git -- a runtime conf file outside
  the repo) — `[sensing] enable=0` and `pdcch_blind_monitor_dci01` commented out.

Verified: 71/71 offline tests pass (was 69 -- +2 net from the sweep test rewrite), nr-uesoftmodem
builds and links clean.

---

## ORIGINAL INVESTIGATION RECORD (2026-09-05, SUPERSEDED BY THE REVERSAL ABOVE)

The section below is preserved for its method (the elimination list is still valid; the specific
measured correlation values were an artifact of that capture's own receive-chain state, not of a
Technique A defect) and because "Still open" items 2-6 below are still accurate. Do not treat its
headline conclusion ("Technique A does NOT converge... root cause investigated, not found") as
current — see the REVERSAL above.

## What this phase adds

`pdcch_blind_monitor_autodiscover`: intended to recover the dedicated CORESET geometry
(Technique A, DM-RS correlation across the whole carrier) and DCI 1_1 payload length
(Technique C, histogram sweep) by search, keyed by a bootstrapped C-RNTI (Technique B, from
Phase 1's common search space) rather than requiring `pdcch_blind_monitor_coreset`/`_ss`/`_bwp`/
`dci_length_override` to be hand-derived from a gNB log. Default off; requires
`pdcch_blind_monitor_autoconf=1` (Phase 1) already on. Tasks 1-4 (bootstrap accessor, correlation
scanner, length sweep, orchestration wiring) are implemented, unit-tested (69/69 offline tests),
and code-reviewed clean (two fix rounds on the integration task caught and fixed a real stack
buffer overflow and a `g_cfg`-leakage bug — see `.superpowers/sdd/2026-09-04-phase3-dedicated-
config-recovery/progress.md` for the full review record).

## Measured (Task 5, live validation on sens6's own cell, known-good ground truth)

**Ground truth** (already manually configured and log-confirmed, from Phase 1's work): dedicated
CORESET `num_groups=45` (270 RB) starting at RB 0, `bwp=[0..273)`, 1 symbol duration, PCI=2 as the
DM-RS scrambling ID. This is the answer key every measurement below is checked against.

- **CORESET footprint discovery: FAILED TO CONVERGE.** Zero `"Phase 3 autodiscover -- CORESET
  footprint"` lines across several live captures (45-200+ seconds each, both with confirmed heavy
  DL/UL iperf traffic and during a quieter traffic period), single-antenna (`NANT=1`/`MRC=0`,
  per commit `5b519a1429`'s finding that 4-branch MRC gives 0% CRC on this deployment).
- **Root cause investigated, not (yet) found.** Added two temporary (now permanent, env-gated:
  `ISAC_DISCOVER_DIAG=1`) diagnostics to trace exactly where the pipeline stalls:
  1. `nr_pdcch_blind_monitor_process()` IS being entered (confirmed: `enabled=1`,
     `autodiscover=1`, `pdsch_decode=2` all correct) and `nr_pdcch_blind_monitor_autodiscover_step()`
     IS being called continuously (thousands of calls observed).
  2. `nr_pdcch_coreset_map_scan()` (Technique A) never once found a candidate above the 0.836
     significance bar — not merely on the sampled calls: the diagnostic prints unconditionally
     (not rate-limited) on every call where `n>0`, falling back to the rate-limited periodic
     sample only when `n==0`, and `n==0` is all that was ever observed.
  3. **Decisive measurement**: instrumented the raw, per-window correlation value regardless of
     threshold. At the KNOWN-GOOD location (RB 0, symbol 0), live correlation measured
     **0.02-0.39** across ten samples spanning a full capture — scattered around and below the
     code's own documented PURE-NOISE floor (~0.209 for an 18-pilot window), not clustered near
     the threshold. The carrier-wide maximum (best of 45 windows) measured **0.35-0.51**, with the
     winning `rb_offset` jumping unpredictably between samples (18, 108, 42, 36, 216, 174, 36, 222,
     228, 12...) — the signature of extreme-value noise statistics (max-of-45 trials), not a real,
     stable transmission. **This is not a threshold-calibration problem** (the code's own comment
     claims 0.8-0.95 for a real hit; the RB-0 series never got within 2x of the 0.836 bar, and
     while the carrier-wide maximum series does come within roughly 1.6-2.4x, its own instability —
     a different winning `rb_offset` almost every sample — is the disqualifying evidence there, not
     distance from the bar) — something is preventing the reference DM-RS from correlating with
     whatever is actually on the air at
     that location, or the assumed (RB, symbol) location itself does not carry a live dedicated
     PDCCH transmission as often as needed for this scan to see one.
  4. **Eliminated as causes** (checked directly against this file's own already-proven-working
     code, not assumed): DM-RS reference-generation call parameter order (`nr_gold_pdcch`'s
     `(N_RB_DL, symbols_per_slot, nid, ns, l)` matches exactly, `ns`/`l` not swapped);
     `nr_pdcch_dmrs_ref`'s count-parameter semantics (already fixed correctly in Task 2, confirmed
     against the real function `nr_dmrs_rx.c:124-129`); the FEP call's `sample_offset` argument
     (`0`, identical to the proven-working candidate-scan FEP at this file's own line 982);
     correlation being scale-invariant (rules out any amplitude/normalization bug, since the metric
     divides out any constant gain).
  5. **Not yet checked, flagged for the next session**: whether `disc_symbol=0` (hardcoded, per a
     `ponytail:` comment assuming "this deployment's dedicated CORESETs are always 1 symbol
     starting at 0") is actually correct under autodiscover specifically — the proven-working scan
     path (`run_occasion()`) reads `symbol = cfg->ss_first_symbol` from CONFIG rather than
     hardcoding it, and while CSS0 autoconf's own `ss_first_symbol=0` happens to match the manual
     ground truth's assumed value, this was inferred, not independently re-confirmed against a
     live `ISAC_OTA_CFG=1`/`ISAC_PDCCH_CFGTRACE=1` dump during an autodiscover run specifically.
     Also not checked: whether a residual timing offset (STO) — the exact class of impairment
     this project's separate OTA-sync stack (CLAUDE.md section 6/7) exists to correct — imposes a
     per-subcarrier phase ramp across the 6-RB window that would decorrelate this SPECIFIC
     magnitude-of-complex-sum measurement while leaving unrelated, phase-insensitive measurements
     (RFCENSUS's `rf_pow`, CSI-RS SNR) unaffected; `nr_slot_fep_ant()`'s own STO/CFO handling
     relative to what the rest of the receive chain applies has not been traced end to end.
- **dci_length sweep: NEVER REACHED** (gated on Step 1's discovery succeeding first — never got
  past the `disc_n_cand > 0` check per the fix-round-1 design). Cannot be scored.
- **Genuine FULLCRC decodes at the bootstrapped RNTI: N/A**, blocked by the above.
- **Time-to-discovery: N/A** (never converged within any tested window, up to ~200s).

## What no amount of this phase's search recovers (per the roadmap's own caveat)

TDRA table contents, MCS table selection, DM-RS additionalPosition, rate-matching patterns —
Techniques A-C recover WHERE and HOW LONG, not how to INTERPRET a payload whose CRC has already
passed. A second, TB-CRC-oracle search stage is flagged, not attempted, by this plan — see the
roadmap's "What no amount of search recovers" section and `PHASE1_CSS0_AUTOCONF_HANDOVER.md`'s
section 12 for the exact prior bug shape (total DCI length correct, two field widths wrong, 0%
CRC while RNTI cross-checks still passed). This remains moot until Technique A converges.

## Still open

**CURRENT STATUS (2026-09-06): the only real remaining blocker is external — see "Steps 2-4
attempt" above.** The list below is the ORIGINAL (superseded) investigation's own still-open
list; item 1 is resolved (see the REVERSAL section), and Steps 2-4 have their own up-to-date
status in the section above this one, including the confirmed-against-the-gNB-log missing-DL-
traffic blocker. Read that section before treating anything below as current.

1. **The actual root cause of Technique A's live-air non-convergence.** Two concrete next
   experiments, in priority order:
   - Confirm `cfg->ss_first_symbol` and `cfg->ss_monitoring_slot_periodicity`/`_offset` under a
     live `autodiscover=1` run (they are inherited from CSS0 autoconf, never set by Technique A/B/C
     themselves) actually match the manual ground truth's assumed values, rather than trusting the
     inference in this doc.
   - Trace `nr_slot_fep_ant()`'s STO/CFO handling against what the proven-working candidate-scan
     path effectively benefits from (it runs downstream of the RT tap's own timing-tracking loop
     applied elsewhere in the receive chain) — if the discovery tap's single-antenna FEP is missing
     a correction the normal path gets "for free" from shared receiver state, that would explain
     noise-level correlation at a location known to carry real reference signal energy.
2. Task 5's Steps 2-4 (footprint/length/decode scoring) cannot run until (1) is resolved.
3. The known dual-frame-of-reference issue in `bwp_start` (flagged during Task 4's review,
   parked as latent/inert on this specific cell since its CORESET starts at RB 0) remains
   unexercised — it would only matter once a real footprint with a nonzero `rb_offset` is
   ever discovered, which has not happened.
4. `dci01_scan` is still leaked from CSS0's config into the (currently never-reached) dedicated
   search — parked in the review record, harmless for the DL-1_1 goal this feature targets.
5. **Technique B (the bootstrapped C-RNTI) is currently INERT as wired, independent of whether
   Technique A ever converges.** In `nr_pdcch_blind_monitor_rt.c`, the Technique A block
   (`if (cfg->autodiscover && !nr_pdcch_blind_monitor_autodiscover_done())`) unconditionally
   `return`s before `run_occasion()`'s candidate-accept loop is ever reached, and
   `nr_pdcch_blind_rnti_bootstrap_record()` — the only place a sighting gets recorded — is called
   from deep inside that loop. So for as long as autodiscover is on and unconverged (measured
   above to be the entire duration of every capture so far), `bootstrap_rnti` is `0` at both of
   its consumers (Technique C's sweep and the CORESET-footprint log line), and the sweep silently
   falls back to its payload-variance-only significance floor. Do not assume Technique B is
   contributing evidence just because `autodiscover=1` — check the `bootstrap_rnti=0x...` value
   actually logged; a future engineer should not spend a session debugging Technique A while
   believing Technique B is already helping it.
6. **Technique A's per-slot cost while unconverged is unbounded, unlike Technique C's sweep.**
   Every DL slot spent unconverged runs a full-carrier FEP plus a 45-window correlation scan, and
   per Task 5's own measurement this never converges on the current cell — so, as currently wired,
   this cost runs forever, not once. Technique C's sweep was deliberately capped to run exactly
   once, success or fail, in an earlier fix round; Technique A has no equivalent cap. Worth
   bounding in a future session, given this project's own repeated findings elsewhere about how
   tight the RT-thread budget is on this receive path.

## Diagnostics added (kept, env-gated, negligible cost when off — a cached env-var check plus a
counter increment per call, same convention as this project's other `ISAC_*` debug flags)

- `ISAC_DISCOVER_DIAG=1` (wired into `tests/passive_rx/captures/run_arm.sh` as `DISCOVERDIAG=1`):
  prints `DISCOVERDIAG ENTRY`/`CFG` once, then `DISCOVERDIAG calls=N n=...` every 200 calls (or
  immediately whenever `n>0`), from `nr_pdcch_blind_monitor_rt.c`/`nr_pdcch_blind_monitor.c`.
  Also enables `COREMAPDIAG calls=N rb0_corr=... max_corr=... max_rb=... thresh=...` from
  `nr_pdcch_coreset_map.c`, printing the raw correlation at the known-good RB 0 and the
  carrier-wide maximum regardless of whether either clears the significance bar — the instrument
  that produced this handover's decisive measurement.
