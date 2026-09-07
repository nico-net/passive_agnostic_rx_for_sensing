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

## PDSCH 0% CRC after the 2026-09-06 17:03 gNB restart — ELIMINATION TABLE

### CORRECTION 2026-09-07 — the premise of this table is WRONG. Read this before using it.

The table below is framed as "PDSCH broke at the 2026-09-06 17:03 gNB restart". Measured today,
that framing does not survive: what changed is the **offered DL load**, not a gNB PDSCH parameter.

Measured from the LIVE gNB log (resolved from the running process via `/proc/<pid>/fd` — the
`/home/sens/gnb.log` on sens4 is a STALE July 8 file and reads as "no traffic"):

| quantity | 82.8% run (xcheck1, 14:25) | 2026-09-07 |
|---|---|---|
| gNB DL grant rate | ~50-57 /s (receiver-side proxy) | **1385 /s** (7362 grants in 5.315 s, gNB timestamps) |
| receiver PDSCH decode attempts | 11,446 / 200 s = 57 /s | 308,144 / 200 s = **1540 /s** |
| crc_ok | 82.8 % | 0.1-0.2 % |

The receiver is tracking essentially ALL of the real grants (1540/s observed vs 1385/s scheduled),
so the 27x rise in `try` is NOT a flood of false accepts — it is real traffic. ~1400 grants/s is the
already-root-caused pathological regime (see memory `passive-crc-bimodality-is-offered-load` and
`bimodality-is-the-timing-runaway`: healthy 8/8 at 165 grants/s, and at ~1500 grants/s PBCH will not
acquire at all). The gNB yaml is UNCHANGED since 2026-09-03 14:27, consistent with this: nothing
about the cell config changed, the phone's iperf simply got heavier.

Corrections to specific rows:
- Row 7 ("4-antenna RT budget, PARTIAL") and row 9 ("residual CFO, PARTIAL") were reading a
  load-limited receiver; their small improvements are real but were never going to close a 25x
  overload.
- The "best remaining lead" (a PDSCH-specific gNB parameter changed at the restart) is
  DE-PRIORITISED — there is no evidence any PDSCH parameter changed, and the load delta is measured.

Also retracted from this session, so it is not repeated:
- "All recent runs are VOID, so the 0% is unreadable" — WRONG. `VOID_NO_CPI` is VACUOUS for these
  confs: `sensing.enable = 0` forces `cpis=0`, and the 82.8% reference run xcheck1 carries the same
  VOID_NO_CPI verdict. Do not use the run verdict to accept or reject a manual-conf CRC number.
- "The RV field is read at the wrong bit offset (100% skip_rv)" — WRONG, and it came from reading
  `try=0` runs that never got started. On runs that actually ran, `skip_rv` is <=2% of accepts
  (578/26920, 52/307967), the gNB sends rv=0 on 4005/4011 grants, and gNB `payload_size=47` matches
  the receiver's `dci_length=47`. The DCI layout is fine.

NOT yet confirmed: that dropping the offered load back to ~150 grants/s restores ~80%. That is the
one experiment that would close this, and it needs the traffic source throttled, not a code change.
Acquisition CFO also varies -12711..-15156 Hz across these runs (~2.4 kHz spread, and
`cfo-estimate-is-the-bimodality-root-cause` records >2600 Hz error giving exactly 0%), so CFO
mis-lock remains a co-factor the load test must control for.

**Thread placement (the 2026-09-07 instruction "move every kind of decoding and printing OUT of the
PHY receive thread") is DONE for the manual conf and is NOT the cause:** `nrue.passive_rx.conf` now
carries `pdcch_blind_monitor_pdsch = "1:1:0:1:16:3:20:9"` (3 consumers, depth 20, cores 9-11) and
`pdcch_blind_monitor_scan_thread = "1:8:8"`, both confirmed live (`scanq[queued=316116 done=314999]`
and the PDSCH pool start line). It carries no `pdcch_blind_monitor_dci01`, so no UL decode runs
inline; `sensing.enable = 0`, so no ISAC tap runs; and the remaining `printf`s on that path are all
one-shot or env-gated diagnostics. CRC stayed ~0% with all of it deferred, which is what rules the
thread budget out as the current limiter.

**The break is gNB-side and cannot be diffed.** `gnb.log` was RECREATED at the restart (its first
line is `2026-09-06T17:03:47`), so the pre-restart PDSCH parameters are gone. Everything below is
what was swept on the RECEIVER side, all with the same binary and conf that scored 82.8% before
the restart. **None of it restores decoding** — do not re-run these.

| # | hypothesis | test | result |
|---|---|---|---|
| 1 | wrong DL/UL traffic present | gNB slot-decision counters | REFUTED — DL grants present, ~30% of slots |
| 2 | aggregation level (AL1 vs AL2) | AL1-only vs AL2/4/8 conf | REFUTED — `dci_aggregation_level` is log2(L); real grants are L=2, AL1-only recovers the C-RNTI ZERO times |
| 3 | wrong `dci_length` | gNB `payload_size` + FULLCRC | REFUTED — 47 both sides, decodes land at `dci_len=47` |
| 4 | wrong DCI 1_1 field widths | payload solve vs gNB log | REFUTED — format=1, RIV=9521=`vrbs=[33..273)`, MCS=25, RV=0 all correct at the assumed offsets |
| 5 | extraction geometry / pointing | `PDCCHCFG` trace + per-slot energy | REFUTED — every derived index matches; energy peaks on the gNB's grant slots |
| 6 | 256QAM MCS 23-25 too demanding | decoded-MCS histogram | REFUTED — `xcheck1` decoded the SAME mcs=25/24 grants at 82.8% |
| 7 | 4-antenna RT-budget / combining | `NANT=1 MRC=0` | PARTIAL — moves CRC off zero (0 -> 38 -> 182) but stays ~0.1% |
| 8 | front-end overload (level rose ~9-16x) | `RXG=25`, level restored to 75.7 vs 53.9 when working | REFUTED — CRC still 0.0% |
| 9 | residual CFO across symbols 1-13 | `CONTFO=1` | PARTIAL - best post-restart result (ok=317/344, vs 182 without it and 0 at four antennas) AND it eliminated the CFO mislocks entirely (2/2 tries clean, cfotrk 7 and 0, where most other arms today hit VOID_CFO_MISLOCK). Still only ~0.1%, so it is not the fix. **Use CONTFO=1 NANT=1 MRC=0 as the capture default going forward** - strictly better on both lock stability and decode count. |

**What remains TRUE and load-bearing:** PDCCH/DCI decode is healthy and in fact BETTER than when
PDSCH worked (26,496 genuine C-RNTI recoveries vs 11,475 accepts in the working run). PDCCH lives
on symbol 0; PDSCH spans symbols 1-13 at 256QAM. Real PDSCH decodes fell ~50x while the LDPC path
is flooded with noise (`seg_fail` 7,880 -> 307,761), i.e. the accept gate now admits mostly false
candidates AND genuine ones no longer decode.

**Best remaining lead (NOT tested):** something in the PDSCH-specific chain — channel estimation
from PDSCH DM-RS, or a PDSCH parameter the conf pins that the restarted gNB changed. Current gNB
values, all 3916/3916 consistent, for whoever diffs against a future known-good period:
`ref_point=0 nid_pdsch=2 nscid=0 num_dmrs_cdm_grps_no_data=1 pdsch_dmrs_scrambling_id=2`,
`dl_dmrs_symb_pos=0x884` (symbols 2/7/11), `mcs_table=1`, `symb=[1..14)`, `num_layers=1`,
`precoding pm_index=11 prg_size=273`.

**Method note worth keeping:** judge PDSCH health by `LDPCDIAG ok=`/`seg_fail=` and PDCCH health by
`FULLCRC` with BOTH `upper=0x0` AND `crc` equal to the C-RNTI read from the gNB log at capture
time. `accepts=` and the run verdict cannot see either correctly.

## RESOLVED 2026-09-06: PDCCH+DCI work; PDSCH 0% CRC is the gNB's 256QAM MCS 25, not a bug

End state of the whole "zero decode" investigation, each step measured against the gNB's own log:

1. **PDCCH/DCI decode WORKS.** 795 genuine payloads recovered at the live C-RNTI in one 200 s
   capture (`FULLCRC ... crc=0x4604 upper=0x0`), all at `L=2 dci_len=47`.
2. **The DCI 1_1 field layout is CORRECT.** Solved by hand from the raw payload dump
   (`payload=0x494c599e2280`, 47 bits MSB-first) against gNB ground truth:
   `format identifier = bit46 = 1`; `MCS = bits[28:24] = 0b11001 = 25` (gNB: `mcs_index=25`);
   `RV = bits[22:21] = 0` (gNB: `rv_idx=0` on 3560/3560); and `RIV = bits[45:30] = 9521`, which by
   the TS 38.214 formula at N_BWP=273 is `RB_start=33, L=240` -- i.e. the gNB's own
   `vrbs=[33..273)`, an allocation it used 134 times in the same window (its distribution is
   3743x `[0..273)`, 134x `[33..273)`, 17x `[10..273)`, 2x `[0..240)`). NOTE: do not expect RIV=545
   on every grant -- that is only the full-band value, and allocations vary. The widths also sum to
   exactly 47 and each matches srsRAN's defaults. **There is no field-width bug** — the `skip_rv=249` that
   suggested one came from NOISE accepts in the AL1-only run, not from real grants.
3. **PDSCH-side config is CORRECT too**: `mcs_table=1` (both sides), TDA `S=1/L=13`
   (gNB `symb=[1..14)`), DM-RS `0x884` = symbols 2/7/11 (gNB `dl_dmrs_symb_pos`).
4. **RETRACTED 2026-09-06 (same evening): the MCS explanation below is WRONG.** `xcheck1`, which
   decoded at **82.8% PDSCH CRC**, was decoding the SAME grants -- its own decoded-MCS histogram is
   `mcs=25` x16, `mcs=24` x6, identical to the failing runs. So 256QAM MCS 25 is demonstrably
   decodable by this receiver and is NOT the differentiator. Keep the paragraph below only as a
   record of the wrong turn.
   **What IS established:** PDSCH CRC broke exactly at the gNB restart (17:03:46) and the break is
   NOT autodiscover-specific -- the MANUAL conf shows the same cliff: xcheck1 14:25 = 82.8%,
   control1 15:26 = 48.6%, then manualbase 18:40 = 0.0% and solve 19:38 = 0.0%, with the receiver
   binary and its conf unchanged across that boundary. PDCCH/DCI decode is unaffected and still
   perfect. So a gNB-side PDSCH parameter changed at the restart that the conf no longer matches.
   Current gNB values, all 3916/3916 consistent, for whoever picks this up:
   `ref_point=0 nid_pdsch=2 nscid=0 num_dmrs_cdm_grps_no_data=1 pdsch_dmrs_scrambling_id=2`,
   `dl_dmrs_symb_pos=0x884` (symbols 2/7/11), `mcs_table=1`, `symb=[1..14)`. The pre-restart values
   could not be compared because the gNB log appears to have been truncated at restart. NOT yet
   root-caused -- do not assume MCS, and do not assume link budget.
   (superseded) Original claim: the gNB serves this UE at 256QAM MCS 23-25 — measured
   3344x `mod=256QAM mcs_index=25`, 408x MCS 24, 7x MCS 23, i.e. 100% of grants. MCS 25 on
   `mcs_table=1` is ~0.78 code rate at 8 bits/symbol, needing roughly mid-20s dB SNR. The gNB picks
   it because the SERVED phone is close with excellent SNR; a passive receiver at a different
   position does not have that margin. This is the SAME wall already documented for the uplink in
   `passive-pusch-decode-works-mcs-limited` ("the wall is ~20 dB of link margin, not a bug").

**So this is an operating-point limitation, not a defect.** The documented lever, already used by
this project before, is to cap the gNB's `max_ue_mcs` (see `harq-combining-has-nothing-to-combine`,
which capped it at 10) — a gNB-side config change. Do NOT keep hunting receiver bugs for this
symptom.

**Caution on one earlier claim in this document:** an `ENERGYPROBE` measurement was used to
"refute link budget". That measured PDCCH CANDIDATE energy, which is a different question from
PDSCH decodability at rate 0.78 — PDCCH (AL2, low rate) decodes fine while PDSCH at MCS 25 does
not. Both facts are consistent; the refutation was scoped too broadly.

## THE RECEIVER IS DECODING. "accepts=0" NEVER MEANT "no decode". READ THIS FIRST.

**Measured 2026-09-06 late evening, and it retracts the AL1 section below plus most of this
document's "zero accepts" framing.** Using `ISAC_PDCCH_FULLCRC=1` — the only metric that can see a
real decode — against the C-RNTI read from the gNB log at capture time (`0x4604`):

| capture | conf | genuine `crc=0x4604` decodes |
|---|---|---|
| `manualbase_184004` | manual (AL2/4/8) | **26,496** |
| `step234j_182511` | **autodiscover** | **509** |
| `step234g_180043` | autodiscover | 26 |
| `al1test_184356` | AL1-only | **0** |

Every one of those runs was reported as a total failure (`accepts=0`, `crc_ok=0.0%`, verdict
`VOID_DL_ZERO`). They were decoding real dedicated grants the whole time. **The `accepts` counter
measures "a valid DCI 1_1 payload", not "a DCI was decoded"** — everything after the CRC parses the
payload as 1_1 and rejects on format-indicator / antenna-ports / reserved-MCS. This is already
documented in the `sib1-oracle-proves-pdcch-rx-chain-broken` memory; it was not applied here in
time.

**Aggregation level: srsRAN's `dci_aggregation_level` is log2(L), NOT L.** The gNB's
`dci_aggregation_level=1` means **L=2 (AL2)**. Confirmed receiver-side: every genuine C-RNTI
recovery is at **L=2** (36x `L=2 dci_len=47` DL 1_1, 9x `L=2 dci_len=43` UL 0_1). An AL1-only conf
recovers the live C-RNTI **zero** times. **So the original conf (`AL1=-1`, AL2/4/8 auto) was
correct all along**, the manual conf and `nrue.passive_rx.al1.conf` edits have been reverted, and
the AL1 section below is RETRACTED — kept only as a record of the wrong turn.

**What is actually still open, correctly framed:** PDCCH/DCI decode WORKS (correct CRC, correct
C-RNTI, correct L=2, correct dci_len=47). What fails is parsing that correctly-decoded 47-bit
payload into fields: `skip_rv=249` (rv!=0 on a link with no retransmissions) is the signature
CLAUDE.md section 12 documents for **wrong per-field widths**, and it is why PDSCH CRC is 0%. The
next step is the field-width solve described there (that history fixed `bwp_indicator` 1->0 and
`time_domain_assignment` 4->2), scored with `ISAC_PDCCH_DCIGT=1` against the gNB's own logged
h_id/ndi/rv/mcs. Do NOT re-investigate geometry, slot alignment, aggregation level, link budget or
the receive chain — all are measured correct or exonerated.

## RETRACTED — AGGREGATION LEVEL section (2026-09-06 evening; kept as a record of the wrong turn)

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

## CFO pre-seeding (`INITFO` / `--initial-fo`) — 2026-09-07

### The defect

This rig's true carrier offset is **~-13.6 kHz** (measured -13580..-13774 Hz on 4 consecutive good
locks; -13.6 kHz at 3.45 GHz = **3.9 ppm**, which is large for an X410 and suggests an undisciplined
reference on one end). A PSS/SSS estimator is unambiguous only within **+/- SCS/2 = +/-15 kHz** at
30 kHz SCS, so the receiver was acquiring at **91 % of its estimator's unambiguous range** — the
most fragile operating point available.

Measured consequence, one 5-run arm at healthy load (2026-09-07, `throttle10`):

| run | acquisition CFO | dl_tb |
|---|---|---|
| 1 | -13635 | 49 % |
| 2 | -13580 | 83 % |
| 3 | **-4933** | **0.0 %** (auto-rejected `VOID_NO_SIB1`, `cfotrk=4`) |
| 4 | -13618 | 46 % |
| 5 | -13774 | 42 % |

and, from earlier arms the same day, two estimates that landed **outside** +/-15 kHz entirely
(-15043, -15156), both scoring ~0.1 %. One run in five is lost to a mis-lock.

### The fix, and why the sign is what it is

`--initial-fo <hz>` (`executables/nr-uesoftmodem.h:78` -> `nrUE_params.initial_fo` ->
`UE->initial_fo` -> `ssbInfo->freqOffset` at `nr_initial_sync.c:614`).

It is NOT a hardware pre-tune. It is the CFO **accumulator seed**, and it means "the correction
already applied". Two lines decide the semantics, and both were read before choosing the sign:

- `nr_initial_sync.c:426-427` — `if (ssbInfo->freqOffset) compensate_freq_offset(rxdata, ...)`:
  the scan buffer is **derotated by the seed** before the PSS/SSS search runs.
- `nr_initial_sync.c:483` — `ssbInfo->freqOffset += pss_res.freq_offset + sss_res.freq_offset`:
  the measured value **accumulates onto** the seed; it does not replace it.

So the seed carries the **same sign as the offset the log reports**: `--initial-fo -13600` derotates
the buffer by -13600, the search then measures a near-zero **residual** in the centre of its range,
and the total comes out at ~-13600. Setting the opposite sign would drive the total to ~-27 kHz,
far outside the unambiguous range — so do not "correct" the sign without re-reading those two lines.

Harness knob (`captures/run_arm.sh`): `INITFO=-13600`, emitted as `${INITFO:+--initial-fo $INITFO}`.
Unset = previous behaviour exactly.

### Making it ADAPTIVE (not yet done — this is the point of the doc)

The -13600 is a **pinned constant for this rig at this carrier**, and it is exactly the kind of
hand-set number this project has been burned by before (see `[[los-bin-must-always-be-adaptive]]`
and the `zero_range_guard` 7-vs-2 episode: a constant that was correct when written and silently
wrong after the axis changed). It MUST NOT be inherited into another rig, carrier, or X410, and it
will drift with temperature and with any reference change on either end.

The value is already measured on every single run, so the adaptive version is cheap:

1. Every acquisition prints `[UE %d] Measured Carrier Frequency offset %d Hz`
   (`nr_initial_sync.c:767`), which the harness already greps as `Frequency offset <n> Hz`.
2. **Derive, don't pin**: on the first try of an arm, run with no `INITFO`, read that number, and
   use it as `INITFO` for the remaining tries of the same arm. That is a 3-line change in
   `run_arm.sh`'s retry loop and needs no receiver change at all.
3. **Reject the outlier, don't average it**: seed from the MEDIAN of the good locks, never the mean
   — run 3 above (-4933) would drag a mean by ~1.7 kHz. A lock is "good" if it is within ~2.6 kHz
   of the running median (the error beyond which decoding is exactly 0 %, per
   `[[cfo-estimate-is-the-bimodality-root-cause]]`).
4. **Persist across sessions** in a small file next to the harness (e.g. `captures/.cfo_seed`),
   keyed by carrier + device serial, refreshed whenever a run's good-lock median moves more than a
   few hundred Hz. Keyed, so it cannot silently follow the operator to a different cell.
5. **The real fix is upstream of all of this**: the CFO is estimated ONCE at acquisition and never
   revisited (`[[cfo-estimate-is-the-bimodality-root-cause]]`). Seeding makes acquisition land in
   the robust centre; it does not track drift during the run. Continuous tracking off CSI-RS/DM-RS
   is the structural answer, and `--cont-fo-comp` is only a partial one.

### What this does NOT fix

Nothing about the 42-83 % spread among runs that already locked correctly (runs 1/2/4/5 above), and
nothing about full-band allocations, where a TB carries ~30 code blocks and TB-CRC collapses as
q^n. Seeding removes the ~1-in-5 total-loss run; it does not raise the ceiling.

### VALIDATED 2026-09-07 — `INITFO=-13600` is the standing interim fix

5-run arm `initfo` (manual conf, `NANT=1 MRC=0 CONTFO=1 INITFO=-13600`, DUR=200), against the
unseeded `throttle10` arm run immediately before it at the same load:

| | unseeded | seeded |
|---|---|---|
| good locks | 4/5 (one at **-4933 Hz**, 8.7 kHz off, 0.0 %) | **5/5** |
| acquisition CFO spread | 8.7 kHz outlier | **117 Hz** (-13710 .. -13827) |
| mis-lock total-loss runs | 1 in 5 | **0 in 5** |

**The mis-lock failure mode is eliminated.** Use `INITFO=-13600` on every run on this rig until the
adaptive derivation below is built.

Do NOT read the `dl_tb` columns of the two arms as a comparison: the unseeded arm's first two runs
ran at the 10M traffic level (`dl_ldpc_ok` 13374 / 75413) and the seeded arm entirely at 2M
(2348-2855). At matched load it is unseeded 42/46 % vs seeded 35-43 % — overlapping, and n is far
too small to claim a difference either way. Seeding buys LOCK RELIABILITY, which is what it was for;
it does not change decode quality and was never expected to.

**Blocking measurement gap found in the same arm:** every run reports
`pdsch_decode[try=0 crc_ok=0 (0.0%)]` because the deferred PDSCH consumer pool does not increment
the in-line counters. Per-TB CRC — the ONLY honest score here, since `dl_tb` is segment-level and
inflated by false-candidate decodes — is therefore unmeasurable on any deferred run. Fix that
counter before running another arm, or every future result reads 0.0 % regardless of quality. This
already cost most of 2026-09-07.

## Adaptive DL chain — 2026-09-07 session record

### FIXED (measured, reproducible)

**1. CFO mis-lock — see the "CFO pre-seeding" section above.** `INITFO=-13600`, 5/5 good locks vs
4/5, spread 117 Hz. Standing interim fix; adaptive derivation designed, not built.

**2. Technique A convergence — was deciding on noise.** The observation window was a fixed 1000
CALLS, but this function is invoked every DL occasion (~2000/s) and only accumulates a hit when a
PDCCH is actually present. At 38 grants/s that is ~2 % of calls, so 1000 calls yielded `total_hits`
of 94 and 4 in two consecutive windows against 45 windows to populate. Three consecutive runs
converged to three DIFFERENT footprints (84/168, 240/12, 12/240) against a truth of 0/270.
Fixed by making termination HIT-driven (`AUTODISCOVER_HITS_PER_WINDOW`, mean hits/window before
deciding) with a call cap as a safety stop. Result: **0/270 exact, reproducible 2/2**, dwell
self-scaling to 7,602-22,906 calls depending on load.

**3. The energy gate could not be turned off from the conf.** `nr_pdcch_blind_monitor_autodiscover_step()`
hardcoded `g_cfg.energy_adapt_factor = 3.0f` AFTER convergence, silently overriding
`pdcch_blind_monitor_noise_gates`. MEASURED with the override live: `held[energy]=17,296,703`,
`accepts=0` for a whole 240 s run, while the SAME run's polar decode recovered **158,899 genuine
C-RNTI payloads**. Removed the override; the gate is now an operator decision. After the fix
`held[energy]=0`.

### STILL BROKEN — the single remaining blocker

**Payloads pass the polar CRC but are never ACCEPTED.** With every gate off
(`held[energy/persist/snr/mismatch]` all 0), `accepts=0` — yet FULLCRC shows 158,899 decodes whose
CRC equals the live C-RNTI. The loss is inside `nr_pdcch_blind_decode_and_extract_ex()`, between
"CRC recovered" and "accepted". The `dci_length` sweep fails at the identical place
(`len47_passes=0/21648`, `best_len=-1`) even though every genuine decode in the same run lands at
`dci_len=47` -- so these are very likely ONE bug, and fixing it unblocks steps 3, 4 and 5 together.

Eliminated so far, do not re-test: the `plausible` field-sanity check (`implausible=0`, never
fires); uninitialised LLRs (`pdcch_e_rx` filled at :1085, sweep reads at :1196); candidate stride
(both walks use the same `n_re_cand`); in-place equalisation (the `eqp`/`eq` pointers are read-only
diagnostics); the extraction field model (`g_cfg.extract` is NOT reset by autodiscover, `bwp_size`
=273, `tda_count`=2 -> 47 bits, matching the manual conf exactly); and the RNTI range check
(`out->rnti` truncates to 16 bits in logs while the check uses the full 24-bit CRC -- a noise
candidate's nonzero upper bits are correctly rejected, so a log line showing an in-range `rnti=`
with reason "outside plausible RNTI range" is NOT a contradiction).

Next diagnostic: log `reject_reason` on the MAIN path (not just the sweep's SCOREDIAG) for
candidates whose CRC upper bits are 0 -- i.e. only for genuine decodes -- so the reason is read off
real grants rather than the empty CCEs that dominate any unfiltered sample.

### Scoring traps that cost time today

- **Re-read the live C-RNTI before EVERY scoring pass.** It changed 0x4625 -> 0x462d mid-session;
  a stale grep reported "0 genuine decodes" on a run that had 158,899. See
  `[[gnb-rnti-recheck-every-prompt]]`.
- **`accepts=` is not a decode count and `try=0` is not "nothing decoded"**: under thread deferral
  the in-line `pdsch_decode[try=/crc_ok=]` counters stay 0 because the consumer pool does not
  increment them, so every deferred run reads 0.0 % regardless of quality. Fix that counter before
  running another scored arm.
- `VOID_NO_CPI` is vacuous when `sensing.enable = 0` -- the 82.8 % reference run carries it too.

### Status

DL adaptive ~65 %: steps 1 (sync) and 2 (CORESET position) fixed and reproducible; step 3 (DCI
decode) recovers 158,899 genuine payloads but nothing downstream consumes them; steps 4 (RNTI
bootstrap) and 5 (PDSCH) have never run end to end. UL adaptive 0 %.

**Caveat on step 2, deliberately not hidden:** the full-carrier extent is a HEURISTIC (snap to the
carrier when `first_w == 0` and the span covers >= 3/4 of it), not a measurement -- PDCCH DM-RS
exists only where a PDCCH was actually transmitted, so the configured CORESET width is not
observable at low load (windows 8-11, 16-19, 24-29, 32-35, 42-44 measured HARD ZERO at 1350 hits
while the gNB scheduled every grant at cce=0/4). It is correct only for a full-band CORESET, and it
does not fire at all when `first_w != 0` -- which happened in 1 of 2 runs once the energy gate was
disabled and noise entered the histogram. The principled version sweeps the extent and scores by
genuine decodes, like Technique C does for `dci_length`.

---

# EXTENT VERIFICATION + THE PDSCH RATE, 2026-09-07 (latest — read before the section below)

## CORESET extent: the 3/4 snap is GONE, replaced by verification against real decodes

The extent is not observable (the histogram measures OCCUPANCY -- PDCCH DM-RS exists only where a
PDCCH was actually transmitted), but it IS tightly CONSTRAINED: the true CORESET must contain every
observed window, so the only admissible hypotheses are `(f <= first_w, l >= last_w)`. On this cell
that is **two** candidates, not a search. Implemented as a candidate list scored by real decodes.

- **Candidate 0 is the old heuristic's own answer**, so a cell where it was already right locks in
  exactly the same time. This can only improve on the previous behaviour, never regress.
- **No new threshold.** A candidate is accepted iff Technique B confirms a C-RNTI under it -- the
  right oracle precisely because noise does not repeat (confirmation needs the same RNTI twice).
- Bounded: once every candidate is tried it keeps candidate 0 and says the extent is probably not
  the fault, instead of re-testing forever.

Live: `extent VERIFIED rb_offset=0 span_rb=270 (candidate 1/2, confirmed by C-RNTI 0x462d)`.

**`first_w != 0` did NOT reproduce in any run this session** (3/3 converged to `rb_offset=0
span_rb=270`), so this is insurance for a narrow-CORESET cell that cannot be tested here. What it
buys today is that a wrong extent announces itself instead of failing silently.

## The PDSCH decode rate: two corrections, and the `q^n` explanation is REFUTED

**Correction 1 -- there is NO "blocking measurement gap".** This document records
`pdsch_decode[try=0 crc_ok=0 (0.0%)]` under thread deferral as a blocker that must be fixed before
any PDSCH arm can be scored, and says it "already cost most of 2026-09-07". The consumer has been
counting `decoded`/`crc_ok` all along and PRINTING them, on the **`PDSCHQ`** line
(`nr_pdcch_blind_monitor_rt.c`, gated on `nr_pdsch_passive_queue_running()`). It is a
line-lookup problem, not a missing counter. Score deferred runs from `PDSCHQ`.

**Correction 2 -- do NOT quote a single-run PDSCH number, including any in this document.** Four
runs this session, all with GOOD CFO locks (-13675 .. -14061 Hz, far inside the +/-2600 Hz mis-lock
threshold) and near-identical PDCCH performance (accepts 213-214 k, genuine decodes 212-213 k):

| run | acq CFO | TB CRC | max_lag_slots |
|---|---|---|---|
| `dcifix_114300` | -13675 | 59.3 % | 23/20 |
| `bootrnti_115245` | -13925 | 59.1 % | 12/20 |
| `sweepauto_114636` | -13903 | 23.0 % | 96/20 |
| `extsweep_120828` | -14061 | **17.5 %** | 16/20 |

A 3.4x swing with the same binary, same conf, good locks, and (except `sweepauto`, whose
`max_lag_slots=96` against `slots_per_frame=20` makes its 23 % a CAPACITY artifact -- rxdata
overwritten before the consumer read it) no capacity problem. This is exactly
`[[passive-rx-needs-5-runs-per-arm]]`. **Any PDSCH conclusion here needs >= 5 runs per arm.**

**The `q^n` full-band-ceiling explanation does not survive.** This document attributes the ceiling
to `TB-CRC = q^n` with `n -> ~30` on full-band allocations. The failure census says the opposite,
in all four runs:

| | SEG_FAIL | DECODED |
|---|---|---|
| mean PRB (4 runs) | 11.9 / 12.0 / 17.3 / 17.0 | 21.2 / 21.5 / 18.4 / 20.8 |
| code blocks C | 1.93 | 2.66 |

**Failures are the SMALLER allocations with FEWER code blocks.** Under `q^n` they would have more.

What the census does establish:
- `tb_fail = 0` in every run -> reassembly, TBS and CRC type are correct; the fault is upstream.
- On failing TBs only **18.5 %** of segments decode (30,936/167,270) -- 0.36 of a C=1.93 TB. This is
  catastrophic non-convergence, NOT one marginal code block in a long TB.
- `LLRDIAG` eliminates two of the three branches in that code's own triage: **not saturated**
  (0.0000 %) and **not crushed** (zero = 0.10 %), magnitude only 25 % down (272 vs 363). That leaves
  the third -- the SIGNS, i.e. descrambling -- consistent with `pos = 54.70 %` on failures vs
  `51.50 %` on successes. **Caveat: TBs that decode are SELECTED for having balanced LLRs, so the
  sign skew may be an effect of conditioning rather than a cause.** It narrows the search; it does
  not close it.

**Strongest lead, and it is a shape argument rather than a margin one.** Across the 3.4x CRC swing
the DECODED population barely moves (mean_prb 21.2/21.5/18.4/20.8, K 6151/6124/5808/6123,
F 107/109/108/109, C ~2.65) while the FAILED population's size climbs (11.9 -> 17.0). If this were a
link-margin threshold, a bad run would keep only the strongest grants and DECODED's mean size would
shift UP. It does not. That is the signature of a FIXED DECODABLE SUBSET of grant shapes plus a
variable remainder -- pointing at a code/shape bug, not link budget and not MCS. **Confirm with a
per-TB histogram of num_rb bucketed by outcome** (the `SHAPE` census currently reports means only);
if decodability is a step function of allocation shape rather than a gradient, that is the bug.

---

# RESOLVED 2026-09-07 (later) — "THE ONE QUESTION" is ANSWERED, and steps 1-4 now run unattended

**Read this before the section below it, which is the (still-accurate) statement of the problem
this section closes.**

## Root cause: `dmrs_typeA_position` is an ASN.1 ENUM, not a symbol index

`NR_MIB__dmrs_TypeA_Position_pos2 = 0`, `pos3 = 1` — and `mac->dmrs_TypeA_Position` carries exactly
that enum into `autoconf_css0()`. The autodiscover convergence block "fixed up" a zero to **2**,
believing 2 meant symbol 2 (`if (g_cfg.dmrs_typeA_position == 0) g_cfg.dmrs_typeA_position = 2;`).
`blind_fill_dmrs_mask()` matches neither enum branch on 2 and returns `-1`, so EVERY genuine grant
died at `"DM-RS symbol mask undefined for this TDRA entry / additional-position"`. The manual conf
pins **0** through `pdcch_blind_monitor_bwp = "0:273:0:47"` (field 3) — that single value is the
entire manual-vs-auto difference.

**None of this document's three ranked suspects was it.** `tda_count` was 2 in both confs, the size
check passed, and the field-value checks were never reached. Suspect 3 named the right FIELD for the
wrong reason.

Two sibling defects from the same units confusion, fixed in the same pass:
- `blind_ul_dmrs_mask()` used `(dmrs_typeA_position == 2) ? 2 : 3`, i.e. it mapped pos2 (enum 0) to
  `l0 = 3` — the same bug INVERTED, on the UL path.
- Two comments asserting the field is "2 or 3".

## The diagnostic that found it, and why the existing one could not

New `ISAC_DCI_WATCH_RNTI=0x<rnti>` (a thin wrapper around
`nr_pdcch_blind_decode_and_extract_ex()`): prints `out.reject_reason` plus the extracted fields ONLY
for candidates whose CRC-recovered RNTI matches the live C-RNTI. **This filter is the whole point** —
noise candidates that clear the RNTI range check outnumber real grants ~100:1, which is why the
pre-existing `last_reject=` periodic summary always reported a noise RNTI and was useless here.
Wired into `captures/run_arm.sh` as `DCIWATCH=<rnti>` (plus `FORCEDCILEN=`, which was previously
not passed through `sudo env`'s scrub).

Measured, before and after, same rig, same cell, same C-RNTI `0x462d`:

| | before | after |
|---|---|---|
| genuine candidates | **100 % rejected**, `mcs=0 rv=0 rb=[0..0)` (rejected before any field populated) | **100 % ACCEPT**, `mcs=25 rv=0 ant=1` |

`mcs=25 rv=0` matches the gNB's own logged grants.

## Technique C (dci_length) needed NO fix — it was the same bug

The sweep calls the same extractor with the same `cfg->dmrs_typeA_position`, so it inherited the fix
exactly as this document predicted ("treat them as ONE bug"). Was `len47_passes=0/21648`,
`best_len=-1`. Now, with NO `dci_length_override` and NO bootstrap RNTI:

```
SENSING: Phase 3 autodiscover -- CORESET footprint rb_offset=0 span_rb=270 bootstrap_rnti=0x0
SENSING: Phase 3 autodiscover -- dci_length locked at 47 (bootstrap_rnti=0x0, occasions_fed=50)
```

`47` matches the gNB's own `payload_size=47`, found in 50-63 occasions from the accumulated
chance-floor significance test ALONE. The chicken-and-egg with Technique B is therefore not on the
critical path: the sweep does not need a bootstrap RNTI on this cell.

## Technique B had NO CONSUMER — that, not the chicken-and-egg, is why it was inert

`bootstrap_rnti` was read in exactly two places (`nr_pdcch_dci_length_sweep_feed()`'s call site and
the CORESET-footprint log line), and **both run BEFORE it can possibly latch**. So even once accepts
started working it confirmed an RNTI and nothing ever used it. Fixed by adding the missing consumer
in `run_occasion()`: once confirmed, DCI **1_1 and 0_1** acceptance narrows from the 65518-wide
plausibility range to an **equality check** — the same test the live non-blind path uses. Format 1_0
is deliberately left wide (it carries SI-/RA-/P-RNTI, which are not this UE's C-RNTI).

**Self-healing by construction, so it needs no knob and cannot be left pinned**: only a real sighting
refreshes the confirmation, so when the UE re-attaches under a new C-RNTI the old one stops being
seen, goes stale after `RNTI_BOOTSTRAP_STALE_SLOTS` (~10 s), and the scan reverts to the configured
wide range and re-bootstraps.

Live, fully automatic (300 s, `nrue.passive_rx.autodiscover.conf`, no override of any kind):

```
SENSING: blind PDCCH -- C-RNTI bootstrapped to 0x462d (class=0); DCI 1_1/0_1 acceptance
         narrowed from [0x1..0xffef] to an equality check
```

`0x462d` is EXACTLY the live C-RNTI read from the gNB log at capture time — self-discovered with no
gNB log and no manual config.

| | auto, before the consumer | auto, with the consumer |
|---|---|---|
| `accepts` | 232,573 | 213,639 |
| genuine `crc=0x462d upper=0x0` | 215,241 | 212,755 |
| **false accepts** | 17,332 (7.5 %) | **884 (0.41 %)** — 18x fewer |
| `held[persist]` | 8,379 | 171 |
| `held[mismatch]` | 9,156 | 777 |
| `LDPCDIAG ok / seg_fail` | 49,337 / 165,091 | 125,735 / 86,533 |

**Read the LDPC row with care.** The false-accept collapse is directly attributable (same binary,
same conf, only the consumer differing). The LDPC improvement is CONSISTENT with no longer feeding
PDSCH garbage allocations derived from false DCIs, but these are two separate captures and this rig's
offered load moves between runs — do NOT quote it as a measured effect size without an alternating
A/B. See `[[passive-rx-needs-5-runs-per-arm]]`.

## Status after this session

Steps 1-4 (footprint -> dci_length -> C-RNTI bootstrap -> genuine accept + PDSCH decode) now run
**end to end, unattended, with every discovered value matching ground truth exactly** — the "is
Phase 3 done" question this document opens with. What remains open is unchanged and listed below:
the full-carrier extent is still a HEURISTIC (not a measurement), `pdsch_decode[try=]` still reads 0
on any deferred run (the consumer pool does not increment the in-line counter, so score PDSCH by
`LDPCDIAG` only), the full-band TB-CRC ceiling (q^n) is a separate problem, and UL adaptive discovery
is still 0 %.

Files changed (worktree `openairinterface5g-total-passive-ue`):
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` — the enum fix; the inverted UL twin in
  `blind_ul_dmrs_mask()`; two corrected comments; the `ISAC_DCI_WATCH_RNTI` wrapper.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` — the Technique B consumer.
- `tests/passive_rx/captures/run_arm.sh` — `DCIWATCH` / `FORCEDCILEN` passthrough
  (backup `run_arm.sh.bak-dciwatch`).

71/71 offline tests pass; `nr-uesoftmodem` builds and links clean.

---

# HANDOVER 2026-09-07 — THE ONE QUESTION: why does MANUAL accept and AUTO not, when AUTO's extracted values are correct?

**Read this section first. Everything else in this document is context for it.**

## The observation, measured back-to-back on the same rig, same cell, same minute

| | manual (`nrue.passive_rx.conf`) | autodiscover (`nrue.passive_rx.autodiscover.conf`) |
|---|---|---|
| run | `captures/manctl_112113` | `captures/final_111450` |
| `accepts=` | **29,331** | **0** |
| genuine decodes (`FULLCRC upper=0x0` AND `crc==live C-RNTI 0x462d`) | 27,434 | **127,242** |
| `held[energy]` | 8,934,876 (gate ON) | **0** (every gate OFF) |
| `held[persist/snr/mismatch]` | 1013 / 0 / 0 | **0 / 0 / 0** |
| `dci_length` | 47 (pinned by conf) | 47 (`ISAC_FORCE_DCI_LEN=47`, confirmed in log: "dci_length FORCED to 47") |
| PDSCH | `LDPCDIAG ok=0 seg_fail=28197` | `ok=0 seg_fail=0` |

**AUTO DECODES 4.6x MORE GENUINE PAYLOADS THAN MANUAL AND ACCEPTS NONE OF THEM.** 127,242 payloads
whose polar CRC equals the live C-RNTI, at the known-correct `dci_length=47`, with NOT ONE gate
enabled (every `held[]` counter is zero), produce `accepts=0`. The rejection is therefore INSIDE
`nr_pdcch_blind_decode_and_extract_ex()` (`nr_pdcch_blind_monitor.c`, ~line 2180-2260), between
"CRC recovered" and "accepted", and it is NOT a gate, NOT the radio, NOT the signal, NOT the CFO,
and NOT the decoder.

## What this rules out (do not re-test — each was measured this session)

- **The radio / rig / signal.** Manual accepts 29,331 on the SAME radio minutes apart. Closed.
- **CFO.** `INITFO=-13600` gives 5/5 good locks; both runs locked at ~-13.7 kHz.
- **The energy gate.** Now genuinely off in auto (`held[energy]=0`, was 17,296,703 — see the
  override bug fixed this session). Auto still accepts 0 with it off; manual accepts 29,331 with it
  ON. The gate is not the differentiator in either direction.
- **`dci_length`.** Forced to 47 in auto and confirmed in the log; every genuine decode in every run
  lands at 47. The formula fallback of 48 is NOT what these runs used.
- **The `plausible` flag.** `implausible=0` — that check never fires.
- **Uninitialised / stale LLRs.** `pdcch_e_rx` is filled at `nr_pdcch_blind_monitor_rt.c:1085`; the
  sweep reads it at :1196; the main path at :1274+.
- **Candidate stride.** The sweep's walk and the main path's `e_rx_cand_idx` walk use an identical
  `n_re_cand`.
- **In-place equalisation.** The `eqp`/`eq` pointers near :1362 are read-only diagnostics.
- **The RNTI range check.** `out->rnti` is TRUNCATED to 16 bits for logging while the check uses the
  full 24-bit CRC, so a log line showing an in-range `rnti=` beside reason "outside plausible RNTI
  range" is NOT a contradiction — that is a noise candidate with nonzero CRC upper bits, correctly
  rejected.
- **`g_cfg.extract` being reset by autodiscover.** It is NOT reset; the conf's `tda`/`dci_bits`/
  `dmrs` survive into the auto path.

## The remaining suspect list, in priority order

The extractor's own rejection points are at `nr_pdcch_blind_monitor.c:2125, 2130, 2155, 2186, 2199,
2226`, and EVERY ONE sets `out->reject_reason`. The reason string for a GENUINE candidate is the
single fact that closes this. Candidates, most likely first:

1. **`"configured DCI field widths exceed dci_length"`** (:2199). Fires when
   `nr_pdcch_blind_dci_size_ex(bwp_size, opts) > dci_length`. With the conf's opts this computes
   15 + riv 16 + tda 1 + harq 4 + dai 2 + pdsch_to_harq 3 + ant_ports 4 + srs 2 = **47**, which is
   NOT > 47 and should pass. **But that arithmetic assumes `opts->tda_count == 2`.** If the auto
   path reaches the extractor with `tda_count == 0`, `f.tda` DEFAULTS TO 4 (`blind_field_bits()`),
   the total becomes 50 > 47, and EVERY genuine candidate is rejected with this exact reason. This
   fits the symptom perfectly (systematic, 100 %, independent of signal quality).
2. **Field-value checks after extraction** — `riv_to_prb_alloc()` bounds
   (`num_rb < 1 || num_rb > n_RB_DLBWP - start_rb`), the TDA index vs `tda_count`, and the
   antenna-ports table (4 bits = 0..15 indexing a 12-row table `g_table_7_3_2_3_3_1`, so 12-15 are
   invalid). A wrong `bwp_size` or `tda_count` makes these reject systematically.
3. **`dmrs_typeA_position`.** Auto hardcodes `2` (spec default) at ~`nr_pdcch_blind_monitor.c:533`;
   manual sets `pdcch_blind_monitor_dmrs = "2:1"`. CONFIRM these resolve to the same value — they
   are parsed by different code paths and have not been compared.

## The next two steps, in order (both are prepared)

**Step 1 — diff the EFFECTIVE derived config, manual vs auto.** The `PDCCHCFG` trace already exists
(`CFGTRACE=<slot>` in `captures/run_arm.sh`). Run both confs with it and diff field by field:
`bwp_size`, `bwp_start`, `dci_length`, `tda_count`, `dmrs_typeA_position`, `coreset_freq_domain`,
`coreset_duration`, `coreset_reg_bundle_size`, `ss_*`. They should agree; one will not, and that is
the bug. This is cheaper than any instrumentation and most likely names the field outright.

**Step 2 — if the diff is clean, log the reject reason for the GENUINE candidate only.** A patch is
written but NOT built:
`/tmp/claude-1000/-home-sens-NICOLA/245fb5e5-eead-4728-a059-9c3a60dae92f/scratchpad/patch_watch.py`
(also reproducible in ten lines). It adds `ISAC_DCI_WATCH_RNTI=0x462d` at
`nr_pdcch_blind_monitor_rt.c:1541` and prints `out.reject_reason` plus the extracted fields ONLY for
candidates whose recovered RNTI matches. **This filter is essential**: noise candidates outnumber
genuine ones ~100:1, which is exactly why the existing `last_reject=` periodic summary is useless
here (it reported an empty reason and a noise RNTI `0x548a`).

Repro:
```
# auto  (0 accepts)
ARM=x CONF=/home/sens/NICOLA/nrue.passive_rx.autodiscover.conf DUR=180 TRIES=1 \
  NANT=1 MRC=0 CONTFO=1 INITFO=-13600 FORCEDCILEN=47 FULLCRC=1 DISCOVERDIAG=1 bash run_arm.sh
# manual (29,331 accepts)
ARM=y CONF=/home/sens/NICOLA/nrue.passive_rx.conf DUR=180 TRIES=1 \
  NANT=1 MRC=0 CONTFO=1 INITFO=-13600 FULLCRC=1 bash run_arm.sh
```

## Scoring rules the next agent MUST follow (each of these cost real time today)

- **Re-read the live C-RNTI from the gNB log before EVERY scoring pass.** It changed 0x4625 ->
  0x462d mid-session and a stale grep reported "0 genuine decodes" on a run that had 158,899.
  Resolve the log from the RUNNING PROCESS (`/proc/<pid>/fd` on sens4 -> currently
  `/home/sens/NICOLA/gnbLogs/gnb.log`); `/home/sens/gnb.log` is a STALE July 8 file that reads as
  "no traffic".
- **`accepts=` is not a decode count, and `try=0` does not mean "nothing decoded".** Under thread
  deferral the in-line `pdsch_decode[try=/crc_ok=]` counters stay 0 because the consumer pool never
  increments them, so a deferred run reads 0.0 % regardless of quality. **Fixing that counter is a
  prerequisite for any scored PDSCH arm.**
- **Score decodes ONLY by `FULLCRC` with BOTH `upper=0x0` AND `crc == live C-RNTI`.**
- **`VOID_NO_CPI` is vacuous when `sensing.enable = 0`** — the 82.8 % reference run carries it too.
- Never build during a capture; `pgrep -x nr-uesoftmodem` must be empty.

## PIPELINE STATUS

### Working
1. **Sync / CFO acquisition** — `INITFO=-13600` seeds acquisition off the +/-SCS/2 ambiguity edge
   (this rig sits at 91 % of it). 5/5 good locks vs 4/5, spread 8.7 kHz -> 117 Hz. Documented in the
   "CFO pre-seeding" section, including the adaptive derivation (not built).
2. **Technique A — CORESET position** — hit-driven termination replaced the fixed 1000-CALL window.
   Converges to `rb_offset=0 span_rb=270` EXACTLY and reproducibly (2/2), dwell self-scaling
   7,602-22,906 calls with load. Was three runs / three different wrong answers.
3. **DCI decode (raw)** — 127,242-158,899 genuine C-RNTI payloads per 180-240 s run in AUTO,
   4.6x manual's rate. The decoder is not the problem.
4. **Manual DL path** — 29,331 accepts, still works end to end at the DCI level.

### Missing / broken
1. **AUTO accept: 0.** THE blocker above. Blocks 2, 3 and 4 below.
2. **RNTI bootstrap (Technique B)** — `bootstrap_rnti=0x0` always. It is fed at
   `nr_pdcch_blind_monitor_rt.c:1609`, which is AFTER the accept path, so it cannot latch while
   accepts are 0. Expected to fix itself once 1 is fixed; unverified.
3. **`dci_length` sweep (Technique C)** — `len47_passes=0/21648`, `best_len=-1`, while every genuine
   decode in the same run is at 47. It fails at the SAME extractor call as 1, so treat them as ONE
   bug. Currently bypassed with `ISAC_FORCE_DCI_LEN=47`.
4. **PDSCH under autodiscover** — `LDPCDIAG ok=0 seg_fail=0`, never invoked (no accepts to feed it).
5. **PDSCH generally** — 0 % on the manual control too, but that run had a bad lock
   (`cfotrk=7 nack=1075 VOID_NO_SIB1`), so it is a degraded capture and NOT evidence. Needs a clean
   manual rerun. Note the seed reduces mis-locks but does not eliminate them.
6. **UL adaptive** — 0 %, not started.
7. **Full-band PDSCH ceiling (separate problem, do not conflate).** Per-code-block quality is
   q ~= 0.82 and has been stable all along; TB-CRC = q^n, and n goes 1 -> ~30 when the scheduler
   switches to full-band allocations under load, which is why TB CRC collapses 82.8 % -> 0.15 %
   while the receiver is unchanged. Throttling traffic hides this by shrinking TBs; it does not fix
   it. The real fix is PER-CODE-BLOCK data-aided reconstruction (accept the CBs that pass their own
   CRC, mask the rest) — for sensing you need X at the REs, not a delivered TB, so 29/30 good CBs
   are 97 % usable. NOT built.

### Caveat that must not be lost
**Technique A's full-carrier extent is a HEURISTIC, not a measurement** (snap to the carrier when
`first_w == 0` and the span covers >= 3/4 of it). PDCCH DM-RS exists ONLY where a PDCCH was actually
transmitted, so the CONFIGURED CORESET width is not observable at low load: at 1350 hits, windows
8-11, 16-19, 24-29, 32-35 and 42-44 measured HARD ZERO while the gNB scheduled every grant at
cce=0/4. It is correct only for a full-band CORESET, and it does NOT fire when `first_w != 0` —
which happened in 1 of 2 runs once the energy gate was disabled and noise entered the histogram.
The principled version sweeps the extent and scores by genuine decodes, exactly as Technique C is
meant to do for `dci_length`.

## Code changed this session (worktree `openairinterface5g-total-passive-ue`, uncommitted)
- `nr_pdcch_blind_monitor.c` — hit-driven autodiscover termination
  (`AUTODISCOVER_HITS_PER_WINDOW=30`, `AUTODISCOVER_MAX_OBS_CALLS`); `DISCOVERHIST` per-window
  histogram dump; full-carrier extent snap; **removed the `energy_adapt_factor = 3.0f` override that
  silently ignored `pdcch_blind_monitor_noise_gates` in the auto path**.
- `nr_pdcch_blind_monitor_rt.c` — `SCOREDIAG` sweep rejection breakdown (now inert when
  `ISAC_FORCE_DCI_LEN` is set, since that skips the sweep).
- `captures/run_arm.sh` — new `INITFO` knob (`--initial-fo`) and `DCIFIELDS` knob. Backups:
  `run_arm.sh.bak-initfo`, `.bak-dcifields`.
- `nrue.passive_rx.autodiscover.conf` — energy gate disabled (`noise_gates = "0:2:500:0:0"`).
  Backup: `.bak-gate`.
