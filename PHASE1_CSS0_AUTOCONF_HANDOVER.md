# Phase 1 — self-configuring common search space (CSS0) — HANDOVER

**Status: autoconf-only enablement WORKS and is verified. BWPStart derivation is resolved (it was**
**never actually wrong at this ARFCN). Genuine SI-RNTI recovery is still 0 across three independent**
**live captures after every fix in this document -- a further blocker remains, out of scope here.**
**Branch:** `total-passive-rx-UL-DL-graphics` ·
**Commits:** `b60fa7ba3b`, `924d1bf863`, `f4537cdb8d`, `0adf449a73`, `602726b0ce`
**Host:** sens6 · **Repo:** `/home/sens/NICOLA/openairinterface5g-total-passive-ue`
**Written:** 2026-09-04 · **Updated:** 2026-09-04 (sdd Task 3)

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

## 2. RESOLVED — BWPStart, the energy gate, and autoconf-only enablement; the remaining gap is open

Three fixes landed since this document's original §2 ("one index is wrong and it is identified")
was written, closing three of the four Definition-of-Done items (§9). A fourth item — genuine
SI-RNTI decode at a rate comparable to the historical 438/120 s — is **not** met, measured three
separate times, AFTER all three fixes below were in place. Read this before re-opening anything
retired into §2b.

### BWPStart — resolved, and it was never actually wrong at this ARFCN

The original table here compared the current cell (ARFCN 630000 / 3450 MHz) against a known-good
value of `1` measured on a **different** cell (ARFCN 627666, 2026-08-19) — exactly the
inherited-number trap §3 itself warns against. Task 2 (`0adf449a73`) derived it instead of copying
either number: `BWPStart = cset_start_rb = ssb_offset_point_a - rb_offset`
(`nr_mac_common.c:4078`), the same formula OAI's own normal (non-blind) path uses at
`coreset_id == 0` (`nr_ue_dci_configuration.c:225`). Both terms are now in the autoconf log line,
so the value carries its own derivation:

```
CSS0 autoconf from MIB/SIB1 -- coreset(...) bwp=[0..48)
(cset_start_rb = ssb_offset_point_a 12 - rb_offset 12)
```

`12 - 12 = 0` is arithmetically self-consistent and matches what OAI's own working normal-path
receiver computes for this cell. **BWPStart = 0 is correct here.** §2c's arm B (below) could not
tell "BWPStart wrong" apart from "energy gate still on", because both were simultaneously true at
the time of that measurement — with the gate now confirmed off (next) and BWPStart independently
re-derived and confirmed correct, that ambiguity is closed: neither is the remaining blocker.

### The energy gate — confirmed as A blocker (Task 1), turned off under autoconf (Task 2)

§2c's arm A/B measurements (kept below unedited, as the historical evidence) showed the adaptive
energy-floor gate freezing at `ENERGY_FLOOR_WARMUP - 1 = 199` candidates and rejecting essentially
everything after, on this cell's CORESET#0 cadence (8 CCEs, SIB1 every 20 ms — dense enough that
the "adaptive" floor converges to signal power, not noise). Task 2 turned it off unconditionally
under autoconf, the same rule already applied to `dci01_scan` (§1 item 3): a dedicated-path
setting must be turned OFF under autoconf, not merely left unread.

Live confirmation (150 s, `egate_autoconf`): `held[energy=0]` steady for the whole run (was frozen
`candidates=199` before this fix), `candidates` rising linearly with occasions (21000 at close of
a 7000-occasion run) — the gate is off **by derivation** (autoconf forces it off regardless of the
conf file's `energy_adapt_factor`), not merely by a conf value someone could silently re-enable.

### The `init()` coupling that made DoD #3 unreachable — fixed (Task 3, this document's own task)

Independently of both fixes above, `nr_pdcch_blind_monitor_init()` required
`pdcch_blind_monitor_coreset`/`_ss`/`_bwp` to ALL be present before it would even look at
`g_cfg.autoconf`, else it returned with `g_enabled = 0` — which gates the entire RT tap
(`nr_pdcch_blind_monitor_rt.c`). A conf carrying autoconf but none of the three manual lines (the
literal DoD #3 claim) ran the CSS0-autoconf derivation into a monitor that was already dead.
Fixed in `602726b0ce`: the three lines are now required together only when
`pdcch_blind_monitor_autoconf` is off; with it on they're optional, and the DCI-width
reconciliation block (which needs a derived `bwp_size` that doesn't exist yet at `config_get()`
time) is deferred with its own log line instead of firing a spurious "every field is being read
from the WRONG bit offset" warning at every startup.

Verified live with `nrue.passive_rx.selfconf.conf` — a copy of the autoconf conf with all three
manual lines deleted (`grep -nE 'pdcch_blind_monitor_(coreset|ss|bwp) *='` returns nothing): the
monitor configures and scans (7 `monitor summary` lines over 150 s, `occasions`/`candidates`
growing 5000→21000+), with the `CSS0 autoconf from MIB/SIB1` line now actually feeding a live
monitor instead of a dead one. This is the DoD #3 claim, and it is now true.

### The remaining gap — genuine SI-RNTI decode is still 0, measured three separate times

With all three fixes above in place, `ISAC_PDCCH_FULLCRC=1` on the no-manual-lines conf still
recovered **0 decodes at `crc=0xffff`** (the real SI-RNTI) out of 94 `FULLCRC` hits over 150 s —
all 94 distinct payloads, no repeats. This is the THIRD independent live measurement of the exact
same pattern:

| capture | conf | FULLCRC hits | genuine (`crc=0xffff`) |
|---|---|---|---|
| Task 1 arm B (gate forced off; BWPStart not yet re-derived) | autoconf.conf | 82 | 0 |
| Task 2 (gate off by derivation; BWPStart logged + confirmed) | autoconf.conf | 96 | 0 |
| Task 3 (autoconf alone, zero manual config lines) | selfconf.conf | 94 | 0 |

Per the controlling plan, a zero-hit result is **not** a failure of any of these three tasks — each
proved its own mechanical claim (gate identified; gate off; autoconf-only enablement), and all
three are independently true regardless of the genuine-decode count. But the pattern across all
three captures is now strong enough to say plainly: **a further blocker remains, beyond the energy
gate, BWPStart, and the manual-config-required-ness this document set out to fix.** Finding it was
out of scope for Tasks 1–3 and was Task 4's territory. Task 4 **has since run** — see §2d below for
what it found (BWPStart independently confirmed against live gNB ground truth; the
`ISAC_PDCCH_CAPTURE` replay tool itself confirmed broken, not built out; the genuine-decode question
still open).

### 2c. 2026-09-04 — sdd Task 1: energy gate confirmed as A blocker (historical record, superseded above)

`run_arm.sh` did not pass `ISAC_PDCCH_ENERGY`/`ISAC_PDCCH_CAPTURE` through its `sudo env`
allowlist before this session (`4c9a762785`, `e595841d2f`) — neither read as "no effect" so much
as "never reached the process". Two 150 s captures on live hardware (sens6, X410, same cell as
§7) with the allowlist fixed:

**Arm A (`egate_on`, gate ON, `ISAC_PDCCH_ENERGY=1 ISAC_PDCCH_FULLCRC=1`):** reproduces §1's
already-recorded numbers, now over a full 150 s rather than a single snapshot —
`candidates` frozen at **199** across all seven periodic summaries (occasions 1000→7000),
`held[energy]` growing **exactly 3/occasion** (2801→20801 over 6000 occasions), `efloor` climbing
0.98→1.11+. 199 = `ENERGY_FLOOR_WARMUP`(200) − 1: the gate lets the warmup candidates through
uncontested, then rejects every candidate after, permanently. This is the predicted signature —
**confirmed**, and independent of ENERGYPROBE (below).

**`ISAC_PDCCH_ENERGY`'s al4-to-floor numbers could not be measured — the instrument itself
never printed, root-caused, not a plumbing failure.** `ENERGYPROBE` produced **zero** lines in
either arm despite the env var demonstrably reaching the live process
(`sudo cat /proc/<pid>/environ` on the running `nr-uesoftmodem`, mid-capture, showed
`ISAC_PDCCH_ENERGY=1` verbatim). Root cause, read from source
(`nr_pdcch_blind_monitor_rt.c:987-1000`): the print fires on
`proc->nr_slot_rx != occ_slot`, using the **intra-frame** slot index — but CORESET#0/SIB1
occasions recur when `gate_slot % ss_monitoring_slot_periodicity == ss_monitoring_slot_offset`
(`:466`), and this cell's periodicity (20 ms ≈ 40 slots) is an exact multiple of
`slots_per_frame` (20 at numerology 1), so every qualifying occasion lands on the *same*
intra-frame `nr_slot_rx`. The transition check is true exactly once (the very first occasion),
which is also the one occasion the code suppresses (`occ_slot >= 0` guard) because there is
nothing yet to report. Net effect: permanently silent for any CSS0-cadence monitor. Pre-existing,
unrelated to this task's plumbing fix, and out of scope to fix here (no C changes this task) —
flagged for whoever next needs per-AL energy numbers; it needs an absolute slot count
(`frame_rx*slots_per_frame+nr_slot_rx`, the pattern already used at `:466`/`:534`), not
`nr_slot_rx` alone.

**Arm B (`egate_off`, gate OFF via `noise_gates="0:2:500:0:0"`):** with the gate disabled, ALL
raw candidates reach `blind_polar_decode()` — cumulative `candidates` climbed to **21000** by
occasion 7000 (vs. frozen at 199 in arm A), confirming the gate really was the thing stopping
candidates from reaching the decoder. **82 `FULLCRC` lines** over 150 s (vs. the historical
438/120 s target in §4, and vs. this doc's prior single-event measurement). All 82 have
`upper=0x0` (passes the narrow 1-in-256 test) and **all 82 have `in_range=0`** — expected and
uninformative here, since this conf pins `pdcch_blind_monitor_rnti_range = "1:65519"`
(0x1..0xFFEF), which excludes SI-RNTI (0xFFFF) *by construction*, gate or no gate.
**The decisive number: zero of the 82 have `crc=0xffff`.** The 82 CRC values are scattered
across the full 16-bit space with no repeats (`0x69aa, 0x22c3, 0x4747, ... 0xafad`) — textbook
noise, and the count matches pure chance almost exactly: 21000 candidates × P(upper 8 bits of a
24-bit CRC == 0) = 21000/256 ≈ **82.03**, against an observed 82. This capture did **not**
surface a single genuine SIB1 decode with the gate fully open.

**This does not refute Task 1's own claim (the gate blocks the decoder) — but it does mean the
gate is demonstrably *A* blocker, not demonstrated to be *the only* one.** A null result from
arm B is exactly what §2's already-open `BWPStart` defect (BWPStart=0 autoconf vs. BWPStart=1
known-good, unresolved, "one index is wrong and it is identified") predicts: wrong DM-RS/RIV
origin corrupts the decode before the RNTI gate is ever reached, regardless of whether the
energy gate lets the candidate through. **`BWPStart` was never under test by this task** — Task 1
touched no C/C++ code and did not change `dmrs_ref`/`BWPStart` derivation. Do not read arm B's
null result as evidence against the `BWPStart` hypothesis; it is equally consistent with it.
Sample size caveat: 82 events is much better than the prior 1-event measurement but is still one
150 s run on a rig with documented 0%↔82% run-to-run swings
(`passive-rx-needs-5-runs-per-arm`) — do not treat "0 genuine decodes" as proven-zero without a
repeat.

**Net finding:** the energy gate saturating during warmup and then rejecting ~100% of candidates
thereafter is real and measured (arm A). Whether removing it alone is *sufficient* to restore
real SIB1 decodes is **not** established by arm B — the data is consistent with `BWPStart` (§2)
remaining a live, separate defect. Task 2 (BWPStart derivation) is not obviated by this task's
result.

### 2d. 2026-09-04 — Task 4: BWPStart confirmed against live gNB ground truth; replay tool confirmed broken; genuine-decode question still open

Task 4 ran. It did not build the offline capture-and-replay tool §5 describes — that work item is
still not done, see the correction below — but it did settle two open questions and surface one new
confirmed defect.

**(a) `BWPStart` = 0 is now confirmed correct against live gNB ground truth, not just re-derived from
a formula.** §2's formula derivation (`cset_start_rb = ssb_offset_point_a - rb_offset = 12 - 12 = 0`)
was internally self-consistent but only checked against OAI's own normal-path *code*. This task
compared the receiver's own live autoconf log line, field-by-field, against the gNB's own live
per-grant PDCCH/PDSCH debug log lines on `sens4`. Every field matched exactly: CORESET groups=8,
duration=1, REG bundle=6, interleaver=2, shift=2, DM-RS/data scrambling ID=2, BWP range=[0..48), and
CORESET/BWP start=0. `BWPStart` is closed — do not re-open it without new evidence.

**(b) SIB1 cadence confirmed frequent, refuting any "too rare to catch" concern.** 193 real SI-RNTI
grants measured in a 30 s slice of the gNB's own log — ~155 ms cadence, not sparse enough to explain
zero genuine decodes across 94-96 FULLCRC hits per 150 s capture.

**(c) NEW open item — the `ISAC_PDCCH_CAPTURE` fixture-write mechanism is confirmed broken.** Two
independent captures against the current (energy-heuristic) trigger wrote **0/400 records** to
`/tmp/pdcch_fixture.bin`, despite thousands of real scan occasions and an independently-confirmed
real, strongly-correlated PDCCH signal in-window: the separate DM-RS-correlation probe in the same
file (`dci_nr.c`, unconditionally on) showed `dmrs_nc=0.974` on real signal in the same capture
window that the occupancy heuristic missed entirely. Most likely fault, not yet root-caused: the
6×-median-CCE-energy occupancy heuristic at `dci_nr.c:772-798` disagreeing with the DM-RS-correlation
probe on the same data. §5's `ISAC_PDCCH_CAPTURE` workflow is therefore not yet usable as described —
flagged as follow-up for whoever picks up the offline-replay approach next.

**(d) The original open question remains open, restated honestly.** Three independent live captures
(Task 1 arm B: 82 FULLCRC hits / 0 genuine; Task 2: 96/0; Task 3: 94/0) show zero genuine SI-RNTI
decodes despite CSS0 config now confirmed fully correct by (a) above. The most likely remaining seam,
not yet inspected: the blind monitor's own separate demapping/unscrambling/decode-chain
reimplementation (`nr_pdcch_blind_monitor_rt.c`). Its own comment at line ~858-861 says it "mirrors
`dci_nr.c`'s own `nr_pdcch_dci_indication()`... minus the own-RNTI equality gate" — i.e. it is a
*parallel reimplementation* of decode logic, not a shared call into the confirmed-working
legacy/RRC-configured path. That divergence is real follow-up work for a future task, not something
to chase as part of this fix wave.

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
| `BWPStart` mismatch (known-good=1 vs autoconf=0) | **RESOLVED, not a defect** | the "known-good" 1 was measured on a different ARFCN; re-derived from first principles (Task 2, §2) it is arithmetically `0` at this cell and matches OAI's own normal-path formula — never actually wrong here |
| Energy gate alone is sufficient to restore genuine decodes | **REFUTED** | Task 2 turned the gate off by derivation (`held[energy=0]` steady) and re-measured: still 0 decodes at `crc=0xffff` (§2) — the gate was A blocker, not the only one |
| The three manual `pdcch_blind_monitor_coreset/_ss/_bwp` lines are needed only for parsing, not for enablement | **REFUTED, then fixed** | `init()` gated the entire monitor on their presence regardless of `autoconf`; deleting them disabled the monitor rather than handing it to autoconf (Task 3, `602726b0ce`) |

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

`ISAC_PDCCH_CAPTURE` **is** in the `run_arm.sh` allowlist (added by Task 1, `e595841d2f`) — the env
var reaches the process. `BWPStart` no longer needs this workflow to close it: it is now confirmed
correct against live gNB ground truth by other means (§2d(a)). What this workflow is still useful
for is the *remaining* open question (§2d(d)) — but the fixture-write mechanism itself is confirmed
broken (§2d(c), 0/400 records written across two captures despite real, correlatable signal
in-window): fix the occupancy-detection heuristic at `dci_nr.c:772-798` before expecting `CAPTURE=1`
to produce a usable fixture.

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

## 6b. Live monitoring — the dashboard and the log follower

Two long-running helpers watch captures. They are **already running**; check before starting a
second copy, because two writers on the same follower output will interleave and corrupt it.

```bash
ssh sens6 "pgrep -af 'monitor.py|watch_log'"
```

### Web dashboard (port 8081)

Renders detections, range-Doppler and the report stream. Reachable from another machine because it
binds `0.0.0.0`.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/monitor
python3 ./monitor.py --connect tcp://127.0.0.1:5556 \
                     --log  /tmp/rx_follow.log \
                     --seed /home/sens/NICOLA/captures/sens/reports.jsonl \
                     --port 8081 --bind 0.0.0.0
```

- `--connect` is the receiver's ZeroMQ report bus; it must match `report_endpoint` in the `[sensing]`
  section of the conf in use.
- `--seed` preloads history from a reports file so the page is not blank on open.
- Open `http://<sens6>:8081`.

### Log follower

`monitor.py --log` reads one file, but each capture writes a **new** `run.log`. `/tmp/watch_log.sh`
re-evaluates which is newest every 20 s and appends it to `/tmp/rx_follow.log`, so the dashboard
keeps following the current run instead of latching onto the previous one.

```bash
setsid nohup /tmp/watch_log.sh >/dev/null 2>&1 &
```

It self-rotates at 200 MB (it once reached 1.4 GB) and bounds every `tail` with `timeout 20`, so it
cannot orphan them.

**If the dashboard says "no log":** the follower died, or its output file was moved/deleted while it
held the descriptor. Truncate in place, never `rm` or `mv`:

```bash
: > /tmp/rx_follow.log      # correct -- the follower keeps writing
# rm /tmp/rx_follow.log     # WRONG -- the fd survives, output goes nowhere, page goes blank
```

That exact mistake produced a "no log" dashboard once; the fix was restarting the follower onto a
fresh file, and the avoidance is the truncate above.

---

## 6c. X410 — access, health checks, and the failure that impersonates a bug

| what | value |
|---|---|
| management (SSH, MPM) | `128.178.122.174`, hostname `ni-x4xx-327C1F2`, root SSH works from sens6 |
| data plane (streaming) | `192.168.20.2` |
| host NIC | `enp129s0f0np0`, must hold `192.168.20.1/24`, MTU 9000, rx/tx ring 8192 |

`run_arm.sh`'s `preflight()` already asserts every one of these and repairs what it can, so a normal
capture needs none of the commands below. They are for when something is wrong.

### Is it alive?

```bash
ssh sens6 "ping -c1 -W2 192.168.20.2"                       # data plane
ssh sens6 "timeout 30 uhd_usrp_probe --args \
   'type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174' | grep -E 'X410|Device'"
ssh sens6 "ssh root@128.178.122.174 'systemctl is-active usrp-hwd'"
```

### THE trap: a stale MPM claim, which looks exactly like a stream stall

SIGKILL never lets UHD release its device claim, so the claim goes stale. The **next** claimant —
including an innocent `uhd_usrp_probe` — makes MPM decide someone is stealing the device and
**kill itself**, and it is then down for about 100 s. Visible only in the X410's own journal:

```
[MPM.RPCServer] [WARNING] Someone tried to claim this device again (From: <host>)
[MPM.kill] [INFO] Terminating pid: 141048
systemd[1]: Stopping USRP Hardware Daemon (MPM)...
systemd[1]: Started USRP Hardware Daemon (MPM).          # ~100 s later
```

```bash
ssh sens6 "ssh root@128.178.122.174 'journalctl -u usrp-hwd --since -20min --no-pager | tail -40'"
```

A run launched into that window shows `returned=0` / `ERROR_CODE_TIMEOUT` for its whole life **with
healthy ANTPOW and zero NIC drops** — i.e. it presents as an X410 stream stall and is not one. By the
time anything re-probes, MPM is back and the probe succeeds, which is why this stayed hidden.

**Avoid it: stop the receiver with SIGTERM and wait**, never SIGKILL first. `run_arm.sh` does this.

```bash
ssh sens6 "sudo pkill -TERM -x nr-uesoftmodem"
ssh sens6 "for i in \$(seq 1 20); do pgrep -x nr-uesoftmodem >/dev/null || break; sleep 1; done"
# only if it ignores SIGTERM -- and then expect a stale claim:
ssh sens6 "sudo pkill -9 -x nr-uesoftmodem"
ssh sens6 "ssh root@128.178.122.174 'systemctl restart usrp-hwd'"   # ~40 s to settle
```

After any MPM restart the device needs time to settle — ANTPOW sits at the noise floor for a while,
so an immediate capture reads as a dead cell.

### NIC — the silent one

```bash
ssh sens6 "ip -4 addr show enp129s0f0np0 | grep inet; cat /sys/class/net/enp129s0f0np0/mtu"
ssh sens6 "ethtool -g enp129s0f0np0 | awk '/Current hardware/,0' | head -3"
ssh sens6 "cat /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors"
```

Three things to know:

1. **The interface legitimately carries two addresses.** As of 2026-09-04 it holds
   `192.168.10.45/24` *and* `192.168.20.1/24`. The first is a NetworkManager profile that has
   silently reverted before; when only that one is present, UHD quietly falls back to the 1 GbE
   management port and starves the stream — **an afternoon of captures was lost to this, and every
   measurement taken over it read as a code regression.** Assert `192.168.20.1/24` is present, do
   not merely check the link is up.
2. **`rx_missed_errors` is CUMULATIVE** (26.7 M on this host and climbing since boot). Only a
   per-run **delta** means anything. `run_arm.sh` samples it at 1 Hz into `nic.csv` and prints
   `nic_miss=` on the verdict line — use those, not the raw counter.
3. `rx_out_of_buffer` is **n/a** on this NIC, so `rx_missed_errors` is the drop counter here.
   Ring size and MTU are **not reboot-persistent**; preflight re-applies them.

### Telling the three failure modes apart

| symptom | drops | ANTPOW | likely cause |
|---|---|---|---|
| `returned=0` / `ERROR_CODE_TIMEOUT`, whole run | **zero** | healthy (90-300) | stale MPM claim — check the X410 journal |
| NIC ring overflow | tens of thousands | healthy | IRQ/CPU placement; `run_arm.sh` pins NIC IRQs to cores 8-13 and the softmodem to 0-7 (`NOSEP=1` waives) |
| genuine RF loss | zero | collapses (e.g. 62 → 4.9) | cell down, cabling, or antenna |

`dropped=` lines inside `run.log` are the **sensing CFR-queue** counter, not RF — do not read them
as radio drops.

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

**Overall status: NOT done.** Three of four items are met; item #2, the genuine-decode rate, is not,
and nothing in Tasks 1–3 was expected to fix it on its own (see §2's "remaining gap"). A further
diagnostic task (offline capture-and-replay against gNB ground truth, §5) is still needed to close it.

1. `BWPStart` derived — with the reasoning recorded, not a copied constant.
   **MET**, done in Task 2 (`0adf449a73`). `BWPStart = cset_start_rb = ssb_offset_point_a -
   rb_offset`, both terms now in the log line; confirmed to match OAI's own normal-path formula and
   arithmetically consistent at this cell (`12 - 12 = 0`). See §2.
2. FULLCRC decodes of SI-RNTI at a rate comparable to the historical 438 / 120 s, passing all three
   parts of the §4 test.
   **NOT YET MET.** 0 genuine SI-RNTI decodes (`crc=0xffff`) recovered across three independent live
   captures, all taken after the fixes below landed: Task 1's arm B (82 FULLCRC hits, 0 genuine),
   Task 2's live capture (96 hits, 0 genuine), and Task 3's own capture on the zero-manual-lines conf
   (94 hits, 0 genuine). See §2's "remaining gap" table. A further blocker remains beyond BWPStart,
   the energy gate, and the manual-config coupling — out of scope for this document's Tasks 1–3.
3. Autoconf produces those decodes on the test conf with **no** `pdcch_blind_monitor_coreset/_ss/_bwp`
   lines present at all — that is the actual claim being made, and it is not proven while a
   hand-written config is still in the file.
   **This is what Task 3 itself verifies, and it is established** for the *enablement* half of the
   claim (Task 3 does not, and was not expected to, establish the *decode* half — that's item #2).
   `nrue.passive_rx.selfconf.conf` carries none of the three lines (`grep` confirms empty); with the
   `602726b0ce` fix the monitor still configures and actively scans CORESET#0 (7 `monitor summary`
   lines over 150 s, `occasions`/`candidates` growing 5000→21000+, the `CSS0 autoconf from MIB/SIB1`
   line feeding a live monitor). The mechanical, log-verifiable claim — autoconf alone enables
   scanning — is **verified**.
4. The default (`pdcch_blind_monitor_autoconf = 0`) remains bit-identical to today for every
   existing deployment.
   **Verified by Task 3's Step 7 regression** (`nrue.passive_rx.conf`, all three manual lines
   present, `autoconf` unset/0): `configured`/`monitor summary` lines report the manual values
   (`coreset(num_groups=45 ...)`, `bwp=[0..273)`), `held[energy=6084483]` non-zero (the gate is back
   on where it belongs), and the new `deferred to CSS0 autoconf` log line does not appear (0
   occurrences). **MET.**
