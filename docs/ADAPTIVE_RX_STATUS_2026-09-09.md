# Adaptive UL/DL passive RX — session record 2026-09-09 (later)

Branch/worktree: `adaptive-rx-UL-DL` on sens6. Supersedes nothing; extends
`ADAPTIVE_RX_VALIDATION_2026-09-09.md`. Receiver-only OTA, authorised.

## Headline

DL is autonomous and works. **UL is not, and the reason is NOT what the previous
sessions' architecture assumed.** Everything the UL width/interpretation search
needs is now in place and correct, and it still never sees a single format-0_1
grant, on a cell that the gNB's own log proves is sending them.

## Fixed this session

1. **The tree did not build.** `nr_pusch_passive_decode.c` included
   `nr_passive_uci_demux.h` and called `nr_passive_uci_ack_demux()`; neither exists
   in the tree or as an untracked file. Reverted that hunk, kept the six real
   fixes in the same uncommitted diff. Reasoning: `docs/UL_UCI_DEMUX_PARKED.md`.
2. **UL joint initialisation** (`fe74ce576c`). `nr_pdcch_ul_discovery_grant()`
   refused every grant unless `bwp_size` and `tda_count >= 1` were configured, and
   nothing supplied either in full_auto. Two defects: `tda_count == 0` is the
   TS 38.214 default 16-entry table, a COMPLETE interpretation that
   `nr_pdcch_blind_dci01_size()` already reads as a 4-bit field, so refusing it
   treated a resolved case as unresolved; and SIB1's common initial UL BWP and
   `pusch-TimeDomainAllocationList` were already decoded and published, with only
   `ul_mu` being read off them. Now seeded as a HYPOTHESIS -- the TB CRC stays the
   authority, so a differing dedicated config fails to converge exactly as an
   unseeded search would, and the seed can only add reach.
   **Live: `UL discovery seeded from SIB1: UL-BWP=0+273 TDAs=6`.**
3. **DCI 0_0 recovered off the 1_0 scan** (`1fd296e9b4`). `nr_pdcch_blind_extract_00()`
   had existed since 2026-08-25 with no caller: 0_0 is size-aligned with 1_0, so
   the polar decode already produced its payload and the 1_0 path rejected it at
   the identifier bit. Also fixed a real defect the new test found -- extract_10's
   class-retry loop `memset`s the result between hypotheses and restored only
   rnti/format/mismatches, wiping the decoded payload.
   **Live: `dci00[accepts=2 rejects=1310]` over 480 s.** It works.

## RETRACTED, same session — read this before reusing the 0_0 rationale

The 0_0 commit message argues this cell "schedules with fallback formats", from
the census showing 13,610 format-1_0 accepts and zero 1_1. **That is wrong.**
Two independent measurements refute it:

- `dci00[accepts=2 rejects=1310]`: if the cell were scheduling UL with 0_0, that
  accept count would be in the thousands, not 2.
- The gNB's own log, last 20 MB: **746 `format=1_1` and 147 `format=0_1`, both at
  `al=2`, and no fallback at all.** Payload sizes across the last 200 MB:
  `payload_size=47` x3813 (DL 1_1), `=43` x1005 (UL 0_1), `=45` x11, `=39` x18.

So the receiver's 13,610 "dci10 accepts" are not what the gNB is sending, and the
0_0 work -- while a genuine gap, correctly closed, and live-verified -- is NOT the
reason UL sits at zero. Keep the code; discard the rationale.

## The actual open blocker, stated precisely

Over 480 s the receiver scans ~5.4M format-0_1 candidates and accepts **zero**,
while on the same cell, in the same CORESET, the same UE-specific search space,
the same aggregation level and the same RNTI, it converges Technique D off the
format-1_1 stream and produces CRC-verified PDSCH.

Everything the 0_1 path needs has now been checked against the gNB and is right:

| input | receiver | gNB log | agree |
|---|---|---|---|
| DCI 0_1 payload size | 43 (locked automatically) | `payload_size=43` | yes |
| target RNTI | 0x4656 (bootstrapped) | `rnti=0x4656`, 5306 sightings | yes |
| aggregation level | candidate list (1_1 works on it) | `dci_aggregation_level=1` = AL2 | yes |
| CORESET | 1 symbol, full 273, non-interleaved | `symb=[0..1)`, 45 ones, NON INTERLEAVED `reg_bundle_sz=6` | yes |
| DM-RS / data scrambling | PCI 2 | `nid_pdcch_data=2 nid_pdcch_dmrs=2` | yes |
| UL BWP | 0+273 (SIB1) | `bwp=[0..273)` | yes |

Active UEs on the cell: 0x4656 (5306), 0x464f (133), 0x4690 (125).

`dci01[accepts=...]` is counted AFTER the discovery controller, so it could not
distinguish "the polar decode never recovered the RNTI" from "it did, and the
width search has not converged". New counters
`ulscan[sched= crc_hit= disc=]` split exactly that. Result below.

## Method notes worth keeping

- The X410 100 GbE data link had **no IPv4 address** again (not reboot-persistent,
  as always). Every capture would have run over the 1 GbE mgmt path and been
  invalid. Re-check `ip -br -4 addr show enp129s0f0np0` before every session.
- All three captures VALID: exit 124 as designed, NIC `rx_missed_errors` delta 0,
  zero RXDISCONT/RFSTALL.
- sens4's clock is ~29 min behind sens6. Do not compare timestamps across them.

## ROOT CAUSE FOUND — it was an array bound, not the radio

Capture `adaptive_ul_dl_mrc2.AFIAsQ` (480 s, VALID) with the new counters:

```
dci01[accepts=0 rejects=1887904]
ulscan[sched=1887904 crc_hit=19094 disc=19094]
UL raw sample rnti=0x4656 len=43 payload=0x48604222a1 sample=1/8 ... sample=8/8
UL discovery width refused: raw=400 classes/error=-2
```

Read in order:

1. **The polar decode works.** 19,094 real format-0_1 DCIs for rnti 0x4656 at length 43
   were recovered and handed to the discovery controller. `crc_hit == disc`, so not one
   was lost between decode and controller.
2. **The SIB1 seed works.** The controller collected all 8 raw samples it needs.
3. **`error=-2` is `NR_HYP_SWEEP_CLASS_OVERFLOW`.** With SIB1's real 6-entry
   `pusch-TimeDomainAllocationList` (a 3-bit TDA field) at DCI length 43, the generator
   emits **400** admissible width vectors, and against those 8 real payloads they do NOT
   collapse below `NR_HYP_SWEEP_MAX_CLASSES = 64`. The search refuses, and `refused` is
   sticky for the rest of the run.

So autonomous UL was never a decode, length, RNTI, aggregation-level or DCI-format problem.
`dci01[accepts=]` being counted after the controller is what hid it: it reported the same
zero whether the polar decode failed or the search declined to start.

**Fix**: `NR_HYP_SWEEP_MAX_CLASSES` 64 -> 512, sized from the measurement above and with it
recorded in the header. This is NOT the "raise the caps to manufacture convergence" that
`ADAPTIVE_RX_VALIDATION_2026-09-09.md` warns against: `MIN_TRIALS`, `WIN_RATIO` and
`MIN_RATE` are untouched, so nothing converges on weaker evidence than before. It only lets
the search RUN instead of declining to start.

## The next wall, predicted before spending the capture

`nr_hyp_sweep_feed()` requires EVERY class to reach `NR_HYP_SWEEP_MIN_TRIALS = 300` before
any winner is declared, and those trials are PUSCH transport-block CRCs. So convergence
costs `MIN_TRIALS * n_classes` decoded uplink TBs -- 512 x 300 = 153,600, roughly an hour
at this cell's ~40 UL grants/s -- **and it needs a non-zero pass rate.**

The gNB runs `pusch: max_ue_mcs: 25` and the UL DCIs carry `mcs=25`. This receiver has
previously measured **0/58,829 at MCS 25** and **76 % with PUSCH pinned to MCS 10**
(`[[passive-pusch-decode-works-mcs-limited]]`). If the pass rate is zero the search
correctly never converges, because there is genuinely no evidence to converge on.

`pusch_book[parked=0]` and `pusch_passive[try=0]` in every capture so far: with the
controller refusing, **the passive UL decode chain has not executed once on this branch.**
Measure its real rate before proposing any gNB change.

## The class cap is raised — and the first run after it was VOID, for an unrelated reason

`NR_HYP_SWEEP_MAX_CLASSES` 64 -> 512 landed (`e182373203`), and the very next capture
looked like a regression caused by it. It was not. **Bisect before blaming your own
changes** paid for itself here.

Back to back, same rig, same binary class:

| | VALID (`AFIAsQ`) | VOID (`pl9Wpp`) |
|---|---|---|
| CFO settled at | **-14965 Hz** | **-24334 Hz** |
| `SIB1 common facts` | 1 | **0** |
| CSS0-autoconf lines | 284 | **474,202 in 4 min** |
| census lines / occasions | 749 / 751k | **0 / 0** |
| pbch_ok | 50/50 | 50/50 |
| RFSTALL / RXDISCONT / NIC missed | 0 / 0 / 0 | 0 / 0 / 0 |

**The void run passes every check the harness had.** PBCH is robust enough to ride out a
~9 kHz offset; PDCCH and SIB1 are not. With no SIB1 there is no dedicated config, so the
blind monitor never leaves CORESET#0 and re-derives the CSS0 config **every slot** --
~500 kB/s of logging, which then starves the very thread that might have recovered.

This is the known acquisition CFO mis-lock (`[[cfo-estimate-is-the-bimodality-root-cause]]`,
`[[passive-rx-runs-need-a-validity-verdict]]`): estimated once at acquisition, and roughly
half of runs on this rig get it wrong. My first three captures were the lucky draws.

**Fix (`03af4c581d`): an acquisition-validity watchdog.** SIB1 is the go/no-go and it lands
in the first seconds or not at all, so the runner probes for it and on failure kills and
RETRIES instead of spending the whole DURATION on a dead capture. Each void attempt keeps
its own log; `validity.txt` carries VALID / VOID_NO_SIB1. Deliberately NOT a live CFO
retune -- that killed the radio 2/2 times previously
(`[[cfo-mislock-abort-dont-retune]]`).

Without this gate the failure reads exactly like a code regression, on the first run after
a commit. Any future "it broke after my change" on this rig must check `validity.txt` first.

## The search ARMS — measured class count is 108

Capture `adaptive_ul_dl_mrc2.wFHFMQ` (DURATION=1800, watchdog acquired SIB1 on its
attempt):

```
UL discovery seeded from SIB1: UL-BWP=0+273 TDAs=6
UL discovery width armed: raw=400 classes=108 rnti=0x4728
UL discovery width refused: raw=4145 classes/error=-2   <- a DIFFERENT UE, at length 45
```

**400 admissible width vectors collapse to 108 equivalence classes** against 8 real
captured payloads. That is the number the old cap of 64 was rejecting, and it settles the
root cause with a measurement rather than an inference. Convergence therefore costs
108 x 300 = **32,400** transport-block CRCs, not the 153,600 worst case.

Still open in the same line: a second UE whose DCI 0_1 length is **45** generates **4145**
raw hypotheses and still overflows 512 classes. Per-length behaviour, expected from the
documented width-vector table (length 45 admits far more layouts than 43). It refuses
cleanly rather than guessing, which is correct, but that UE cannot converge as things
stand. Do not "fix" it by raising the cap again -- 4145 raw would need thousands of
classes and millions of TB-CRCs. It needs the hypothesis space CONSTRAINED, not the cap
lifted.

## First passive UL transport-block decodes on this branch

`pusch_passive[try=3440 crc_ok=14 (0.4%)]` at ~4 min into `wFHFMQ`.

Every prior capture read `pusch_book[parked=0]` / `pusch_passive[try=0]` -- with the
controller refusing every grant, the passive UL decode chain had never executed once. It
is now running end to end: blind DCI 0_1 -> discovery controller -> grant book -> PUSCH
FEP/chest/equalise/descramble/LDPC -> TB CRC -> feedback.

**Do not read 0.4 % as a link-quality figure.** It is an aggregate over 108 competing
width hypotheses, of which at most ONE is correct; the other 107 are deliberately wrong
and must fail. If the successes belong to a single class, that class's own rate is
roughly 14 / (3440/108) = 44 %, which is what the oracle actually scores against
`NR_HYP_SWEEP_MIN_RATE = 0.02` and `WIN_RATIO = 3.0`. The per-class split is the number to
report, not the aggregate.

Convergence needs every class at 300 trials = 32,400, and the observed rate is ~14 trials/s,
so ~38 min against this run's 1800 s. This capture may fall just short.

## AUTONOMOUS UL GRANT RECOVERY WORKS (2026-09-09, capture `wFHFMQ`)

At ~13 min into the 1800 s run:

```
dci01[accepts=28728 rejects=13927488]
pusch_book[parked=28728 claimed=28724 expired=4]
pusch_passive[try=5524 crc_ok=20 (0.4%)]
```

**28,728 UL grants accepted, against exactly 0 in every capture before today.** The whole
chain now runs unattended, with no dedicated-configuration input:

  SSB/MIB -> SIB1 (UL BWP + TDA list) -> CORESET/DCI-length autodiscovery ->
  blind DCI 0_1 polar decode -> width-hypothesis controller -> grant book ->
  PUSCH FEP / channel estimate / equalise / descramble / LDPC -> TB CRC -> feedback

`parked=28728 claimed=28724 expired=4` says the k2 slot bookkeeping is right: grants are
parked in the DL slot and claimed in the uplink slot they point at, with 4 lost to
expiry (0.01 %).

### What is NOT yet established

- **The width search has not converged.** It needs every one of its 108 classes at 300
  trials = 32,400; the observed rate is ~7 decode attempts/s, so ~77 min against this
  run's 30. This capture will end short. Not a failure -- a duration.
- **0.4 % aggregate CRC is not a link figure** and must not be quoted as one. At most one
  of 108 competing hypotheses is correct and the other 107 are required to fail. The
  per-class split is the number that matters and is not yet instrumented (staged).
- **`try=5524` against `claimed=28724`**: only ~19 % of claimed grants become decode
  attempts. `pdcch_blind_monitor_ul_pusch = "1:1:0"` caps decodes at 1 per slot, and the
  decoder also declines unsupported modes. Raising that cap is the obvious lever on
  convergence time and changes no oracle semantics -- more trials, same evidence rule.

## The width search NEVER CONVERGES — it is torn down every ~30 s

Measured over the 1800 s `wFHFMQ` capture:

```
UL discovery width armed:            26
UL width classes split on payload:   25
UL width search converged:            0
class counts across re-arms:  108 -> 122 -> 130 -> 134 -> 135 -> 138 -> 222
```

The controller keeps admitting novel payloads into its 8-entry sample ring, and ANY payload
that distinguishes two previously-merged hypotheses discards **all** accumulated evidence for
**every** class. Live traffic is an endless supply of distinct payloads, so that condition
fires forever and the search restarts before it can reach `MIN_TRIALS` on any class. The
class count also drifts (108 -> 222) because the ring rotates underneath the classing.

The original design assumed a finite sample set. It is not one.

**This, not the trial rate, is why convergence is at zero** -- so stopping one of the two
active UEs (which would roughly double the per-UE trial rate) would not have helped: 2x zero
is zero. Worth recording, because that was the obvious next lever and it was wrong.

**Fix**: freeze the sample set once the width search arms, and treat a later distinguishing
payload as a DIAGNOSTIC (`late_splits`) rather than grounds to discard evidence. Two things
keep that honest: the counter makes an unreliable classing visible instead of silent, and a
merged-but-wrong winner is self-limiting because every grant it emits is scored by that
grant's own PUSCH transport-block CRC. Equivalence was always documented as finite-sample
evidence, never proof.

Also measured, full UL decode census at 7819 attempts:
`seg_fail=2698 zero_tb=44 ta_refined=6139 unsup=2257 setup_fail=0` -- `ta_refined` shows the
per-grant timing-advance refinement working, and `unsup=2257` are modes the passive decoder
declines (transform precoding, DM-RS type/length outside its support). Those are correctly
NOT counted as CRC failures.

## gNB logging dropped to warning level (2026-09-09, mid-session)

Debug-level logging crashed the gNB after a while, so it now runs at warning. Every gNB-side
cross-check in this report -- the 1_1/0_1 format census, `payload_size=43`/`47`, the CORESET
dump, per-RNTI sighting counts -- came from debug-level lines and is **no longer
re-derivable**. Those measurements stand as a record; do not plan new ones.

Consequences to carry forward:
- `[[gnb-rnti-recheck-every-prompt]]` cannot be satisfied from gnb.log any more. The
  receiver's own `rnti_seen` lines are the substitute.
- A gNB crash mid-capture voids a run exactly as a CFO mis-lock does, and the acquisition
  watchdog only checks receiver-side SIB1. **Known gap, not yet closed.**

## The length-45 overflow is half the fleet, not one odd UE

Over `wFHFMQ` the receiver bootstrapped four UEs and locked a DCI 0_1 length for each,
unaided:

| RNTI | locked length | raw hypotheses | outcome |
|---|---|---|---|
| 0x4728 | 43 | 400 | armed (108 -> 222 classes across re-arms) |
| 0x472a | 43 | 400 | armed |
| 0x4690 | 45 | 4145 | CLASS_OVERFLOW at 512 |
| 0x461e | 45 | 4145 | CLASS_OVERFLOW at 512 |

So the 4145-hypothesis case is **half the UEs on this cell**, not an outlier. Raising the
cap again is the wrong answer -- 4145 raw would need thousands of classes and millions of
TB-CRCs at `MIN_TRIALS` each. That path needs the hypothesis space CONSTRAINED. Note also
that the RNTIs turn over as UEs re-attach, so per-RNTI search state is repeatedly rebuilt
from scratch; long convergence budgets and short RNTI lifetimes are in tension, and that
tension is not yet addressed.

Run hygiene confirmed for this capture despite a gNB restart earlier in the session:
`Initial sync successful` x1 and `SIB1 common facts` x1, i.e. the receiver never lost the
cell mid-run, and all 26 arm / 25 split events belong to a SINGLE RNTI's context. The churn
diagnosis is therefore payload-driven, not an artefact of re-acquisition or RNTI turnover.
