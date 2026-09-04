# Phase 1 — self-configuring common search space (CSS0) — HANDOVER

**Status: derivation works, grants not yet recovered. One index is wrong and it is identified.**
**Branch:** `total-passive-rx-UL-DL-graphics` · **Commits:** `b60fa7ba3b`, `924d1bf863`
**Host:** sens6 · **Repo:** `/home/sens/NICOLA/openairinterface5g-total-passive-ue`
**Written:** 2026-09-04

---

## 0. Why this exists

The passive sensing receiver works today only because it is handed **this** gNB's parameters,
read out of that gNB's own logs and pasted into `pdcch_blind_monitor_coreset/_ss/_bwp/_tda/_dci_bits`.
Point it at any other cell and it decodes nothing.

Those pasted values describe the **dedicated (UE-specific)** configuration, which the gNB delivers
in `RRCReconfiguration` over a **ciphered SRB**. A passive listener can never read it. What a cell
*does* broadcast in the clear — PSS/SSS, MIB, SIB1 — is enough to describe the **common** search
space (CORESET#0 / SearchSpace#0), and that is what Phase 1 makes the receiver derive for itself.

Trade-off, stated up front: the common path is sparser than the dedicated one. On air this cell
gave **438 SIB1 decodes in 120 s** against **0–1 dedicated grants per 100 s**, so the common path is
also the *more reliable* stream — but its ~20 ms cadence caps unambiguous velocity at
**±2.17 m/s** (λ = 86.9 mm at 3450 MHz). Phase 3 of the roadmap is what lifts that; Phase 1 is what
makes the receiver work on an unknown cell at all.

---

## 1. What is already built and verified

`pdcch_blind_monitor_autoconf = 1` (default `0`, so every existing deployment is untouched).

| item | location |
|---|---|
| `nr_pdcch_blind_monitor_autoconf_css0()` | `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` |
| `nr_pdcch_blind_monitor_autoconf_wanted()` | same file |
| call site | `openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c`, right after `fill_searchSpaceZero()` (~line 526) |
| conf param registration | `nr_pdcch_blind_monitor.c`, params table (~line 725) |
| test conf | `/home/sens/NICOLA/nrue.passive_rx.autoconf.conf` |

The call site matters: it is the point where MAC has *just* computed CORESET#0 from the MIB, and
where the pre-existing `ISAC_OTA_CFG` probe already printed these values as a config to paste by
hand. Phase 1 applies what that probe printed.

**Verified live** (single antenna, nothing taken from the conf file):

```
SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(groups=8 dur=1 bundle=6 interleaver=2 shift=2
         scramb=2) bwp=[0..48) ss(period=40 offset=0 dur=2 symb=0) dci10(mux=1 sib1=1)
SENSING: blind PDCCH ladder: num_cces=8 ncand=3/64 re=864/8192 (AL1=0 AL2=0 AL4=2 AL8=1)
SENSING: blind PDCCH monitor summary: occasions=7000 candidates=199 accepts=0
         dci10[accepts=0 C=0 TC=0 SI=0 RA=0 P=0] dci01[accepts=0 rejects=0]
         held[energy=20801 persist=0 snr=0 mismatch=0] efloor=1.10
```

`num_cces=8` is exactly 48 REGs / 6, `scramb=2` is the PCI, `dci_len` auto-computes to **39** —
all correct. Scanning happens (7000 occasions). Values fixed by TS 38.211 for CORESET#0
(REG bundle 6, interleaver 2, interleaved, shift = scrambling = PCI) are set, not derived.

Three real defects were found and fixed in `924d1bf863` — do not reintroduce them:

1. **SI-RNTI was excluded by the RNTI filter.** Default range is `1..0xFFEF`; SI-RNTI is `0xFFFF`.
   The monitor scanned CORESET#0 correctly and discarded every SIB1 grant. Autoconf now pins the
   range to SI-RNTI alone (also a much stronger gate).
2. **Monitoring-slot offset dropped the frame term.** `fill_searchSpaceZero()` uses
   `slot + slots_per_frame * sfn_c` for mux pattern 1. Harmless on this cell (`sfn_c = 0`), wrong
   in general.
3. **Format 0_1 scanning stayed on and did real harm.** It consumed *all 199* candidates surviving
   the energy gate and rejected them, at this cell's *dedicated* 0_1 payload length. 0_1 is a
   UE-specific-search-space format and cannot appear in CORESET#0.
   **General rule: autoconf must switch dedicated config OFF, not merely stop reading it.**

---

## 2. THE OPEN DEFECT — one index

`ISAC_PDCCH_CFGTRACE=1` output (log tag is **`PDCCHCFG`**, not `CFGTRACE`) compared against the
known-good configuration recorded when this path last produced 438 real SIB1 decodes:

| index | known-good | autoconf now |
|---|---|---|
| type | 0 | 0 ✓ |
| n_rb | 48 | 48 ✓ |
| cset_start / rb_offset | 0 / 0 | 0 / 0 ✓ |
| dmrs_ref | 0 | 0 ✓ |
| scr_id | 2 | 2 ✓ |
| dur / bundle / ilv / shift | 1 / 6 / 2 / 2 | 1 / 6 / 2 / 2 ✓ |
| **BWPStart** | **1** | **0** ✗ |
| ncand | 12 blind / 3 normal | 3 |

Live line:

```
PDCCHCFG f=26 s=1 ss=0 type=0 n_rb=48 cset_start=0 rb_offset=0 dmrs_ref=0 scr_id=2
         BWPStart=0 BWPSize=48 dur=1 bundle=6 ilv=2 shift=2 ncand=3
```

`BWPStart` is the frequency origin used for extraction and for the RIV. Getting it wrong puts the
DM-RS sequence and the RIV origin in the wrong place while every log line still looks healthy —
the same failure class as the DCI field-width bug in `PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md` §C,
where the total length was right, two field offsets were wrong, and the result was 0 % CRC with
plausible-looking output.

---

## 2b. REFUTED AND ALREADY ELIMINATED — do not re-litigate any of these

Each line below cost a measurement to establish. They are the reason the search space for the
remaining defect is small. Sources in §8.

**The blind PDCCH chain itself WORKS.** Measured 2026-08-19:
`438 x FULLCRC L=4 dci_len=39 crc=0xffff upper=0x0 in_range=1`. Extraction, LLR generation,
unscrambling, polar decode and CRC/RNTI recovery are all correct on real SIB1. Any statement that
"the receive chain is broken" is retracted — it was an artefact of reading `accepts` (§4).

| hypothesis | verdict | evidence |
|---|---|---|
| PDCCH receive chain / equalisation broken | **RETRACTED** | 438 real SIB1 decodes; and an active UE completed RA → Msg4 → RRCSetup → PDU session on the same cell with `cumulated bad DCI 0` |
| FFT-window / time-tracking defect ("frequency coherence destroyed") | **RETRACTED** | that 2026-08-04 offline analysis shared the blind path's own extraction assumptions, so it was measuring this defect, not the receiver. Do not spend time here |
| Config derivation differs between paths | **EXONERATED** | `PDCCHCFG` traces from blind and normal paths are identical except `ncand` (12 vs 3) |
| LLR generation differs between paths | **EXONERATED** | for matched `(frame, slot, cce, L)` the two LLR buffers are byte-identical — `sum`, `nz`, first 8 soft bits all equal (`f=0 s=1 cce=0 L=4 → crc=0xffff sum=2614 nz=432` on both) |
| `nr_pdcch_blind_llr_autoscale` is a blind-only difference | **ELIMINATED** | its comment says "enabled only for this path", but `dci_nr.c:184` initialises the global to 1 and nothing ever clears it. The normal path runs with it ON too. Comment is stale — fix the comment, don't retest |
| `nr_slot_fep(..., 0, ...)` sample-offset argument | **NOT the cause** | the normal path passes `0` as well (`phy_procedures_nr_ue.c:956`) |
| per-symbol `memcpy` into `rxdataF_symb` | **NOT the cause** | byte-for-byte the same pattern the normal path uses |
| `CceRegMappingType = NON_INTERLEAVED` hardcode at `nr_pdcch_blind_monitor_rt.c:234` | **INERT** | only gNB-side files read it; the UE deinterleaver switches on `reg_bundle_size != 0`, which the monitor does pass. Interleaving IS applied |
| RNTI value / C-RNTI churn | **EXONERATED for SIB1** | SI-RNTI is a fixed known answer; pinning the range makes the accept test a known-RNTI test |
| Noise gates suppressing real decodes | **EXONERATED** | the oracle ran with all gates off (`held[energy=0 persist=0 snr=0]`) |
| X410 overflows / RF drops on sens6 | **NOT a factor** | 0 overflow / 0 `ERROR_CODE_TIMEOUT` / 0 short-read across every run; `benchmark_rate` clean at 4×122.88 MS/s. The `dropped=` lines in logs are the SENSING CFR-queue counter, not RF |

**One real bug found and FIXED along the way, keep it:** `CoreSetType` was hardcoded to
`NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG`. Its only effect in the UE RX path (`dci_nr.c:338`) is
`dmrs_ref = BWPStart` for PDCCH-Config versus `0` for MIB/SIB1, so any CORESET#0-style config with a
non-zero `bwp_start` generated its DM-RS sequence offset by that many RBs. Now an optional 7th
`pdcch_blind_monitor_coreset` field (`...:1` = MIB_SIB1); autoconf sets it. Note the current trace
shows `dmrs_ref=0`, i.e. this is behaving correctly — **but it also means `BWPStart` and `dmrs_ref`
are coupled through this field, so changing one without checking the other is a trap.**

**Two measurement artefacts that have already produced false conclusions:**

- **`dci_length` sweeps are not free.** `nr_pdcch_blind_dci_size_ex()` rejects any `dci_length`
  smaller than its computed DCI-1_1 field sum *before decoding*, so a sweep shows `accepts=0` for
  everything below 47 and ~130 random accepts above it. That is the validator, not the air. Zero the
  optional field widths when testing a 39-bit payload.
- **`grep -c "a|b|c"` without `-E`** matched a literal string and reported "0 overflows" while
  testing nothing. Always `-E`, and sanity-check any zero.

**AL4 is required.** SIB1 lands at `L=4`. An AL1+AL2 ladder could never find it; the AL4/AL8 sweep
is what made the 438 decodes visible. Autoconf pins SS0's TS 38.213 Table 10.1-1 values
(AL1/AL2 unused, AL4 = 4, AL8 = 2). Related: srsRAN's `dci_aggregation_level` is very likely
**log2(L)**, not L — do not quote "99.997 % at AL1" as fact without re-checking units.

---

## 3. TASK 1 — resolve `BWPStart` from first principles

**Do not just set it to 1.** The known-good value of 1 was measured on 2026-08-19, when the cell
was on **ARFCN 627666**. The cell has since moved to **ARFCN 630000 / 3450 MHz**, so the frequency
plan is different and 1 may or may not still be correct. Copying it would be inheriting a number,
which is the failure mode `verify-before-asserting-never-inherit-a-number` exists to prevent.

Derive it. The relevant facts:

- The origin is **CRB-relative to point A**. On the Aug cell: `offsetToPointA 26`,
  `locationAndBandwidth 1099` → initial DL BWP = 273 RB at CRB 0, and CORESET#0 occupied
  `[1..49)` → `BWPStart = 1`.
- `mac->type0_PDCCH_CSS_config` carries both `cset_start_rb` **and** `rb_offset`
  ("offset from SSB RB0"). Autoconf currently passes only `cset_start_rb` and ignores `rb_offset`.
  Both are 0 in the current trace, which is *itself* worth explaining — if the true origin is
  non-zero, one of these should be carrying it.
- **There is a disagreement to understand, not paper over:** OAI's own normal path sets
  `rel15->BWPStart = mac->type0_PDCCH_CSS_config.cset_start_rb` at
  `openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c:225`, which is 0 here — yet the known-good
  blind config used 1. Establish which is right for the *current* cell before changing anything.

Suggested method — **run both paths in ONE process on identical samples.** Attach normally (not
`--passive-rx`) with the blind monitor pointed at the same CORESET#0, so the normal path, which
demonstrably decodes SIB1, traces alongside the blind one. Two lines differing in `BWPStart` is the
answer; identical lines close this task as a non-issue.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=bwpstart CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=150 TRIES=1 NANT=1 CFGTRACE=1 FULLCRC=1 ./run_arm.sh

# compare the two paths' derived indices (log tag is PDCCHCFG, NOT CFGTRACE):
L=$(ls -dt /home/sens/NICOLA/captures/bwpstart_*/ | head -1)/run.log
grep -aoE 'PDCCHCFG .{0,160}' "$L" | sort -u
```

Expect one line per path per traced slot. If only one distinct line appears, both paths agree and
`BWPStart` is not the defect — say so and move to §5 rather than changing the value on suspicion.

> `ISAC_DISC_NO_RESYNC=1` is REQUIRED for any attached-UE run on this tree, or it re-acquires
> endlessly and never attaches. `run_arm.sh` already sets it.

---

## 4. TASK 2 — score with FULLCRC, never with `accepts`

**`accepts` cannot see a successful DCI 1_0 decode.** Everything after the CRC parses the payload
as DCI **1_1** and rejects on format-indicator / antenna-ports / reserved-MCS. SIB1 is format
**1_0**, so those fields are garbage and rejection is *correct*. `accepts` measures "valid 1_1
payload", not "decoded a DCI". Reading `accepts=0` as failure cost this project a full debugging
round twice, including the one that produced this document.

Use `ISAC_PDCCH_FULLCRC=1`. A **real** decode requires all three of:

1. `upper=0x0` — the untruncated 24-bit CRC's top 8 bits are zero.
   **On its own this is only a 1-in-256 test.** 199 candidates × 1/256 ≈ 0.8 expected false hits
   per run, and 2.28 M candidates once produced ~42 k chance passes that were briefly mistaken for
   success.
2. **The decoded payload VARIES between instances.** An invariant payload is a degenerate polar
   fixed point, not traffic. A previous session reported 3744 "decodes" that were all byte-identical
   (`rv=3 mcs=17 ndi=1 harq=8 tda=0 startrb=62 numrb=129`) and were entirely artefact.
3. **The candidate survives the energy gate.** A real signal does not vanish when a noise gate is
   enabled; the artefact above did, which is what exposed it.

Also: `nr_pdcch_blind_result_t::rnti` is `(uint16_t)crc`, so logging *that* hides the upper bits and
makes a coincidental low-16 match look real. Log the full CRC.

**Target to beat:** `438 x FULLCRC L=4 dci_len=39 crc=0xffff upper=0x0 in_range=1` over 120 s.
That is what this path produced when it last worked. Current state is **one** FULLCRC event,
`L=8 dci_len=39 crc=0x8603 upper=0x0 in_range=0`, which is consistent with chance and is **not**
evidence of anything.

> Full-rate CFGTRACE/LLRPROBE **breaks the attach** that provides the control arm. Always gate to
> the target slot (`ISAC_PDCCH_CFGTRACE_SLOT=1`; SIB1 is at slot index 1 on this cell).

---

## 5. TASK 3 — offline capture-and-replay instead of live runs

Live 150 s runs are a poor instrument here: the cell has gone down unannounced mid-capture, and
this rig's run-to-run spread is wide enough that single runs cannot resolve small differences
(see `passive-rx-needs-5-runs-per-arm` — adjacent runs have swung 0 %↔82 % with config held fixed).

`ISAC_PDCCH_CAPTURE=1` sets `nr_pdcch_blind_capture`, which writes a replay fixture. Score that
fixture offline against gNB ground truth. This is the method that actually resolved the equivalent
problem in August; more live runs did not.

Ground truth requires the gNB at **debug** log level. Traps:

- The routinely-synced `gnb_remote_logs/gnb.log` is **WARNING** level — no config dump, no grants.
  "It's in debug mode" can be true of intent and false of the file you receive. Check the level
  before planning around a log.
- Resolve the gNB's live config and log path from the **running process** (`pgrep -a gnb` on sens4,
  read its `-c` argument). Both have moved mid-session before, and a stale log reads as "no traffic".
- The C-RNTI churns on every re-attach. Re-read it at capture time; never reuse one.

```bash
# capture a fixture (env must be added to run_arm.sh's sudo env allowlist first -- see section 6)
ARM=fixture CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=120 TRIES=1 NANT=1 CAPTURE=1 FULLCRC=1 ./run_arm.sh

# gNB ground truth -- resolve BOTH paths from the RUNNING process, never assume:
ssh sens4 "pgrep -a gnb"                    # read its -c argument for the live config
ssh sens4 "grep -c 'SI-RNTI' <that log>"    # needs log: all_level: debug
```

`ISAC_PDCCH_CAPTURE` is **not yet in the `run_arm.sh` allowlist** — add it before expecting `CAPTURE=1`
to do anything (§6). Once the fixture replays with a known answer, `BWPStart` becomes a one-line
sweep with an unambiguous pass/fail instead of a 150 s round trip.

---

## 6. Build, run, verify

```bash
# BUILD  (never while a capture is running -- 8 compile jobs starve the UEs and the
#         capture silently becomes a dead-cell measurement)
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make nr-uesoftmodem -j8

# VERIFY THE BINARY ACTUALLY CONTAINS YOUR CHANGE. A clean git tree is not proof.
strings nr-uesoftmodem | grep -c 'CSS0 autoconf from MIB'

# RUN
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=<name> CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=150 TRIES=1 NANT=1 FULLCRC=1 CFGTRACE=1 ./run_arm.sh
# results land in /home/sens/NICOLA/captures/<ARM>_<HHMMSS>/{run.log,verdict.txt}
```

```bash
# WHAT CHANGED, and why -- the commit messages carry the measurements, not just the diff
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue
git log --oneline -4               # d3521885cd this doc | 924d1bf863 fixes | b60fa7ba3b first cut
git show b60fa7ba3b 924d1bf863     # the whole Phase 1 diff
git log -1 --format=%B 924d1bf863  # the three defects and why each one mattered
```

`run_arm.sh` passes env through a **`sudo env` allowlist** — a variable not listed there is silently
dropped and your setting does nothing. Add new knobs to that list. Already wired: `FULLCRC`,
`CFGTRACE`, `RXBRANCH`, `NVARFIX`, `BRFO`, `MRC`, `BRMIN`, `GAINTRIM`, `NANT`, `CARRIER`, `SSB`,
`SCAN`.

**Single antenna (`NANT=1`) for this work.** Receive branches 1–3 on this rig are individually
undecodable (a paired in-run test rescued 0 of 15 411 transport blocks on them); that is a separate
open problem tracked in `branch-2-is-strong-and-undecodable`, and it only adds noise here.

---

## 7. Cell under test

ARFCN 630000 · 3450 MHz · PCI 2 · 273 PRB / 100 MHz · SCS 30 kHz · TDD (8 DL / 2 UL, 10-slot period)
· λ = 86.9 mm · SSB via GSCN 7783, `ssb_start_subcarrier` 150 · CORESET#0 = 48 RB / 1 symbol / 8 CCEs
· SIB1 at slot index 1 · DL `mcs_table qam256`, UL `qam64`, both `max_ue_mcs 25`.

Derived rather than configured: `--ue-scan-carrier` finds GSCN 7783 / offset 150 unaided, and the
TDD pattern comes from SIB1 automatically.

---

## 8. Where the rest of the state lives

This document is self-contained for the task. For everything around it:

| what | where |
|---|---|
| The SIB1 oracle: full history, the 438-decode resolution, every eliminated hypothesis in §2b | `/home/sens/.claude/projects/-home-sens-NICOLA/memory/sib1-oracle-proves-pdcch-rx-chain-broken.md` |
| Memory index — grep it for a subsystem name **before** forming any hypothesis | `/home/sens/.claude/projects/-home-sens-NICOLA/memory/MEMORY.md` |
| Passive receiver state, measured numbers, eliminated-hypothesis list | `/home/sens/NICOLA/PASSIVE_RX_ONLY_HANDOVER.md` |
| Blind PDCCH on X410 | `/home/sens/NICOLA/X410_BLIND_PDCCH_HANDOVER.md` |
| PDCCH coherence investigation (note: its conclusion is RETRACTED, see §2b) | `/home/sens/NICOLA/PDCCH_COHERENCE_HANDOVER.md` |
| Passive UL decode | `PASSIVE_UL_HANDOVER.md` (repo root) |
| Harness usage and caveats | `tests/passive_rx/README.md`, `README_OTA.md` |
| Receive-branch problem (why `NANT=1` here) | memory `branch-2-is-strong-and-undecodable.md` |
| Roadmap this is Phase 1 of, incl. Phases 2-3 | https://claude.ai/code/artifact/e1e6ae5d-25f6-4c30-97cb-09f2c0f239a2 |

**Read the memory index first.** Not doing so is what produced this document's own detour: three
fixes were built for a problem already recorded as resolved, using a metric already recorded as
unable to show success. That cost a full build-and-run round; reading the entry cost 30 seconds.

---

## 9. Definition of done

1. `BWPStart` derived — with the reasoning recorded, not a copied constant.
2. FULLCRC decodes of SI-RNTI at a rate comparable to the historical 438 / 120 s, passing all three
   parts of the §4 test.
3. Autoconf produces those decodes on the test conf with **no** `pdcch_blind_monitor_coreset/_ss/_bwp`
   lines present at all — that is the actual claim being made, and it is not proven while a
   hand-written config is still in the file.
4. The default (`pdcch_blind_monitor_autoconf = 0`) remains bit-identical to today for every
   existing deployment.
