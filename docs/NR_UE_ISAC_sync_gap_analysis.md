# NR_UE_ISAC — OTA STO/CFO/SFO/CPE Sync Gap Analysis (Phase 0)

Task: `.claude/tasks/ota_sync_passive_ue.md`, Phase 0. This document is the required
deliverable before any implementation. It records, per impairment, what exists today
(with file/function citations), what is missing, the exact proposed hook point in the
real pipeline, and RT-thread-safety concerns. It ends with a proposed file layout for
Phases 1–6 and a list of things that must be resolved before writing code.

Repo facts (verified this session):
- Git repo root: `/home/sens/NICOLA/openairinterface5g`, current branch `develop`.
  The entire `openair1/PHY/NR_UE_ISAC/` module and `nr_csirs_monitor.{c,h}` are still
  **untracked** (`git status` shows `??`); the RT-tap edits to `csi_rx.c`,
  `phy_procedures_nr_ue.c`, `CMakeLists.txt`, `nr-uesoftmodem.c` are unstaged `M`.
  → Before Phase 1, the ISAC work should be committed onto a feature branch so the
  "one reviewable commit per phase" constraint is meaningful. Flagged for the user.
- Cell/RF (from CLAUDE.md + `nrue.uicc.conf`): n78, fc = 3414.99 MHz, SCS 30 kHz
  (numerology 1, 20 slots/frame, 0.5 ms/slot), 51 PRB = 612 subcarriers, B210 sample
  rate 23.04 MHz, PCI 2, CSI-RS period 20 ms (= 40 slots), no PRS/TRS.
- Invariant to protect: `nof_range × df = total BW` → `range_res = c/(2·BW) = 8.16 m`;
  fused grid uses `comb_spacing = 1`, so `range_max = c/(2·SCS) = 5000 m`
  (`range_doppler::process`, `defs_nr_UE_ISAC.h`).

---

## 0. Answer to Phase 0 step 2 — `--ue-fo-compensation` scope (READ FIRST; Phases 3/7 depend on it)

**There are two independent frequency-offset facilities in the OAI UE, controlled by
two different flags. The documented sensing run uses only the first.**

### (a) `--ue-fo-compensation` (`nrUE_params.UE_fo_compensation`, `PHY_VARS_NR_UE::UE_fo_compensation`)
- **Consumed in exactly one place:** `nr_initial_sync()` passes it as
  `.foFlag = ue->UE_fo_compensation` into the per-GSCN SSB scan
  (`openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:397`). It gates whether PSS-based
  fractional-CFO estimation runs during **initial cell search only**
  (`pss_nr.c:269`, `.freq_offset = ffo_est * subcarrier_spacing`).
- **How the estimate is applied:** in the *non-continuous* path
  (`executables/nr-ue.c:220-223`) the one-shot `common_vars.freq_offset` is pushed into
  the RF front-end by **retuning the LO** (`nrue_ru_set_freq(...)`). It is applied
  **once, at sync**, and never revisited.
- **Scope verdict:** one-shot, acquisition-time, **coarse CFO only**. It does **not**
  run per-slot, does **not** track, and touches **nothing SFO-adjacent** (no sample-clock
  correction). A residual CFO after this coarse correction is exactly what the
  resolved-artifact note observed smearing the LOS across the whole Doppler axis.

### (b) `--cont-fo-comp` (`nrUE_params.cont_fo_comp`) + `--freq-sync-P/-I` — a *separate* flag, default 0
- When set, initial sync stores the offset digitally instead of retuning
  (`nr-ue.c:218-219`, `freq_offset = freq_offset − dl_Doppler_shift`), and **every**
  received OFDM symbol is de-rotated in software by `nr_fo_compensation()`
  (`openair1/PHY/MODULATION/slot_fep_nr.c:105-115`, calling
  `nr_phy_common.c:433`). The estimate is refined by a PI controller from PBCH DM-RS
  residual FO (`phy_procedures_nr_ue.c:1112-1125`, `nr_ue_pbch_freq_offset()`),
  but that update fires **only when a PBCH/SSB is decoded**, i.e. at **SSB
  periodicity = 20 ms** for this cell.
- `nr_fo_compensation()` applies a **uniform per-sample phase ramp** (same phase
  increment for every sample, `phase_inc = −fo_Hz/(samples_per_ms·1000)`). This is a
  pure carrier de-rotation — it corrects CFO only. It is **not** a resampler and does
  **nothing for SFO** (a sample-clock error would need a time-base rescale, not a phase
  ramp).

### Consequence for this task
- The CLAUDE.md "standard run" and "sample collection run" pass **`--ue-fo-compensation`
  but not `--cont-fo-comp`**. So during the live captures behind the resolved-artifact
  note, the UE had **only the coarse acquisition-time LO correction active and no
  continuous FO tracking at all**. That is consistent with the observed residual-CFO
  Doppler smear.
- **Even with `--cont-fo-comp` on**, its 20 ms-rate, PBCH-driven, scalar-CFO PI loop is
  far too coarse for the sensing-grade `vel_res ≈ 0.009 m/s/bin` (≈ 0.2 Hz/bin at this
  fc), and it still leaves SFO uncorrected.
- **Therefore Phase 2 (CFO) adds an independent, ISAC-path tracking/refinement loop.**
  It must *not* modify `nr_fo_compensation()` / the PBCH PI loop (those feed PDSCH
  demod, which the taps depend on). Because the sensing correction operates on the
  **accumulated CFR grid off the RT path**, it is naturally decoupled from whichever of
  (a)/(b) is active — it measures and removes whatever residual survives into the CFR,
  regardless of the front-end flag. Phase 3 (SFO) has **no** existing mechanism to
  extend and is a fully new loop.

---

## 1. Confirmed RT tap → CPI-accumulation data path (Phase 0 step 3)

### Tap points (RT-safe, both call `nr_isac_submit_cfr`)
| Source (bit) | Call site | What is captured | Comb on the abs-subcarrier axis |
|---|---|---|---|
| `csi_rs` (0) | `csi_rx.c:938` (own RRC CSI-RS, in `nr_ue_csi_rs_procedures`) and `csi_rx.c:1077` (UE-agnostic `nr_ue_csi_rs_sensing_capture`), both via `nr_isac_submit_csirs_ls()` (`csi_rx.c:802-833`) | raw per-**RB** LS estimate `ls[rb*12]`, port0/ant0, one RE per RB | **comb-12** (`k = rb*12`) |
| `pdsch_dmrs` (1) | `phy_procedures_nr_ue.c:636-677` (`nr_ue_pdsch_procedures`, before buffer free) | DM-RS-interpolated `pdsch_dl_ch_estimates` sub-sampled `j += 2` | **comb-2** |
| `pdsch_data` (2) | same block, `want_data` → `comb = 1`, `j += 1` | the **same** DM-RS-interpolated estimate at every allocated subcarrier | **comb-1** |

Confirmations of already-documented facts:
- `pdsch_data` is **not** a TB-re-encode data-aided path — it is the same
  DM-RS-interpolated estimate taken at comb-1 (matches CLAUDE.md's "important
  correction"). So the real distinct RE sets are `csi_rs` (comb-12) and the shared PDSCH
  path (comb-2 or comb-1). No UE-side re-encoder exists.
- Each tap fills `__thread` scratch (`isac_h/k/l`) and calls `nr_isac_submit_cfr`, which
  converts interleaved float→`icf_t` into a `thread_local` vector and hands a pooled
  `sensing_slot_t` to the engine (`nr_isac.cc:299-325`, `sensing_engine::submit`). RT
  side does only recycle+copy; **no DSP, no allocation after warm-up** — the
  "no heavy math on the PHY critical path" rule is currently honoured and must stay so.
- `k_abs` is **CRB0-absolute** in all three taps (CSI-RS `rb*12`; PDSCH
  `base_sc = (BWPStart+first_rb)*12 + j`), which is what makes cross-source fusion land
  on one grid.

### CPI accumulator (`sensing_engine.cc`, engine thread only)
- Grid: **full per-subcarrier**, row-major `h_cpi[cpi_slots][nof_subc]`,
  `nof_subc = carrier.nof_prb*12` (= 612). Column = CRB0-absolute subcarrier
  (`accumulate_cpi`, lines 206-284).
- Rows are **absolute-slot-indexed** via an unwrapped `this_span`
  (lines 239-266); a submission sharing the current row's real slot **merges**
  (last-write-wins per column, line 255/281); otherwise a new slow-time row opens.
  Per-row occupancy is tracked in `occ_all` (deferred freq-interp).
- On CPI close (`cpi_row >= cpi_slots`, lines 286-312): (1) per-row native comb
  `cpi_row_comb[rr] = row_native_comb(occ_all…)`, (2) **Stage-4b part 1** freq gap-fill
  `interp_freq_row`, (3) **Stage-4b part 2** non-uniform→uniform slow-time resample
  `resample_slow_time()` (irregular row times in `cpi_row_time`, linear interp;
  blended row carries the coarser comb, lines 358-396), then `process_cpi()` →
  `range_doppler::process(...)`.
- `cpi_row_time[r]` holds each row's **actual** unwrapped slot position — this is the
  authoritative irregular-timestamp source Phases 2/3 must use (do **not** assume
  uniform spacing).

### Where sync correction must insert — ordering verdict
```
RT tap → submit → accumulate_cpi (raw grid, occ_all)
        └─ CPI close:
             [A] per-row native comb (row_native_comb, from raw occ)  ← keep
             [B] Stage-4b/1 freq gap-fill (interp_freq_row)
             [C] Stage-4b/2 slow-time resample (resample_slow_time)
             process_cpi → range_doppler::process:
                  clutter mean-subtract
                  freq-Hann → range IFFT
                  [D] per-row de-alias taper (row_comb, valid = nof_range/comb) ← keep untouched
                  slow-Hann → Doppler FFT → |·|² → zero-D/zero-R notch → CA-CFAR → NMS
```
- **STO / SFO delay correction (Phases 1,3,4): must go BEFORE [A]/[B]/[C].** The
  correction operates per **real** row (native comb known from `occ_all`), and
  interpolation should act on already-timing-corrected samples. Confirmed the current
  code does *not* already do any timing correction there, so this is a clean insertion
  point (a new stage between `accumulate_cpi`'s raw fill and the Stage-4b block, i.e. at
  the top of the CPI-close branch, computing per-row CIR peaks from the **raw** occupied
  columns before gap-fill destroys the native-comb structure).
- **CFO / CPE phase correction (Phase 2):** per-row phase de-rotation; can be applied on
  the raw grid immediately after the delay correction and before Stage-4b (it commutes
  with linear interpolation up to interpolation error, and keeps a single time-domain
  pass). State the exact placement in Phase 2/3 code.
- **De-alias taper [D] stays byte-for-byte untouched.** If SFO correction needs a row's
  native comb, it must **reuse** `cpi_row_comb[]` / `row_native_comb()` (already computed
  at [A]) rather than recomputing — the same value feeds both.
- **Invariant guard:** none of the above changes `nof_subc`, `comb_spacing`(=1 into the
  DSP), or `df`. A frequency-domain phase-ramp SFO correction (per-subcarrier phase
  ∝ k·drift) preserves `nof_range × df`; a resampling correction that changed the column
  count would break `range_res = 8.16 m` and must be avoided / flagged (Phase 4 c).

---

## 2. Per-impairment gap table

Definitions on the CFR grid (row n = slow-time slot, column k = subcarrier):
STO ↔ linear phase across **k** (constant over rows); CFO ↔ linear phase across **rows**
(constant over k); SFO ↔ CIR-peak **delay drift linear in row index** (couples k and
row); CPE ↔ per-row common phase after de-trending.

| Impairment | What exists today | What is missing | Proposed hook point | RT-safety |
|---|---|---|---|---|
| **STO (fine, sub-sample)** | Integer sample/timing sync only, off SSB: `nr_adjust_synch_ue()` (`phy_procedures_nr_ue.c:1108`), PBCH-gated, 20 ms rate, corrects the RF sample index — not the per-CPI-row CFR phase. No sub-sample estimate reaches the CFR grid. | Per-row CIR-peak parabolic sub-sample delay estimate; distinguish constant STO (correct once) vs linear drift (→ SFO). | New stage at **CPI-close, before Stage-4b [B]**, IFFT each row's occupied columns via `isac_fft`, locate LOS tap near its known bin (~bin 6, verify stability), parabolic peak. | Engine thread only; `isac_fft` already lives there. Zero RT impact. |
| **CFO (residual, sensing-grade)** | (a) one-shot LO retune at sync (`nr-ue.c:222`); optionally (b) 20 ms PBCH PI loop + per-sample `nr_fo_compensation` (`slot_fep_nr.c`). Both too coarse; (b) off in the documented run. | Decision-directed residual-CFO loop on the CFR: `φ[row]=angle(CIR[los_bin,row])`, unwrap over **actual** `cpi_row_time`, LS line-fit slope → CFO; alpha-beta across CPIs. | Engine thread, per CPI, **after STO correction, before Stage-4b**; de-rotate rows. Must **not** touch (a)/(b). | Engine thread only. No new RT-path code. |
| **SFO (dominant Doppler-smear cause)** | **Nothing.** No sample-clock estimate or correction anywhere in UE or ISAC. `nr_fo_compensation` phase-ramp does not address it. | LOS delay-drift-vs-row fit (irregular timestamps), per-row within-source ISI check, → SFO ppm; correction via cubic Farrow fractional-delay resampler **or** per-row freq-domain phase ramp (∝ k·drift). | Engine thread, per CPI, **before Stage-4b [C]**, reusing `cpi_row_comb[]`. Freq-domain phase-ramp variant preferred (no extra IFFT/FFT round trip); Phase 4 must justify. | Engine thread only. Farrow order / method is an explicit tradeoff to document. |
| **CPE (common phase error)** | No per-row common-phase estimate. No PT-RS assumed (CLAUDE.md); no PRS/TRS on cell. LOS tap is the **only** available phase reference. | Per-row residual common phase after CFO de-trend → de-rotate all subcarriers of that row using LOS-tap phase. | Engine thread, per CPI, folded into the Phase 2 CFO/CPE stage (after slope removal). | Engine thread only. |

Common RT note: **all** new estimation/correction stays on the engine thread and
operates on the accumulated grid. The RT taps are not modified except (later, Phase 7a)
extending the self-test injection surface. The "no heavy math on the PHY critical path"
rule is preserved by construction.

---

## 3. CFAR / guard-band surface (Phase 0 step 4)

Parsed in `nr_isac.cc:184-210` into `nr_isac_args_t` (`defs_nr_UE_ISAC.h:66-89`),
consumed in `range_doppler::cfar` and the notch in `range_doppler::process:251-261`:
`cfar_guard(4)`, `cfar_train(8)`, `cfar_pfa(1e-3 default)`, `zero_doppler_guard(3)`,
`zero_range_guard(2 default)`, `nms_range_bins(3)`, `nms_doppler_bins(3)`,
`max_detections(32 default)`.

Live `nrue.uicc.conf` (as of this session, NOT the values in the resolved-artifact note):
- `zero_range_guard = 4` — **not 9**. The file's comment block (lines 107-121) documents
  the history: 9 was tuned for `source=csi_rs` comb-12/416 m axis; after the
  full-per-subcarrier fusion grid pushed `range_max` to ~5000 m it was set to **4**
  (notches ~0–33 m at the bottom and the top 4 bins). The task text assumes `= 9`; the
  ground truth is `= 4`. **Narrowing `zero_range_guard` back toward its pre-fix value is
  Phase 7b's pass/fail lever — not something Phases 0–4 change.** The baseline for the
  guard-narrowing test is therefore "current value 4", and its comment already flags that
  the symmetric notch cannot kill the far-edge CFAR row without blinding close range (an
  asymmetric top-notch is explicitly deferred — relevant context for Phase 7b, but do not
  fold a CFAR-edge redesign into this task; the constraints forbid changing CFAR/NMS
  internals — flag if it seems required).
- `cfar_pfa = 1e-4`, `max_detections = 16`.
- **Minor pre-existing bug flagged, not in scope:** line 123 reads
  `nms_doppler_bins = 3inv;` — a stray `inv` that libconfig will likely reject or
  mis-parse. Worth fixing before the next live run, but it is not part of this task.

---

## 4. Self-test injection mechanism (Phase 0 step 5) — and the missing offline harness

Mechanism (`range_doppler.cc:36-118`):
- `args.selftest_targets = "DELAY_US:DOPPLER_HZ:GAIN,..."` parsed in the ctor into
  `targets_`; `args.selftest=1` with an empty list injects one default target at 25%
  range/velocity.
- `inject_selftest()` **adds** each target as a complex sinusoid
  `exp(−j2π·m·df_comb·τ + j2π·fd·n·t_slow)` on top of the working CFR, amplitude scaled to
  the CFR RMS. It runs inside `process()` on the live/accumulated `work` grid
  (lines 160-162).

**Gap for Phase 7a (confirmed):** the mechanism injects **targets only, on top of
whatever CFR arrives** — it has **no way to impose a controlled STO/CFO/SFO on the
reference/LOS path itself**, and it has **no synthetic clean background**: with no RF it
runs only when real taps drive a CPI. So it cannot today produce a self-contained,
SDR-free STO/CFO/SFO validation. Phase 7a must extend this surface (a `selftest_los = …`
/ impairment field in the same config style) **and** provide a way to synthesize a clean
LOS-bearing CPI offline.

**Missing test harness (important):** CLAUDE.md and the task both reference
`scratchpad/dealias_test.cc` and `scratchpad/fusion_test.cc` as the offline-only tests to
*extend* (Phase 6a: "wherever `dealias_test.cc`/`fusion_test.cc` currently live"). **These
files do not exist anywhere in the repo** (searched whole tree; no `scratchpad/` dir).
The de-aliasing/fusion work was "offline-verified" but those harnesses were not committed.
→ Phase 6a cannot "extend" them; it must **create** a tracked offline test target under
the module (proposed below) that links `range_doppler`/`isac_fft`/`sensing_engine`
directly, drives synthesized CPIs, and asserts. Flagged for the user as a deviation from
the task's stated assumption.

---

## 5. Source-fusion CFR contract (Phase 0 step 6)

- Enabled set is a bitmask `sources_mask` (`nr_isac.cc:parse_sources_mask`), gated on the
  RT side by `nr_isac_source_enabled(int source)` (`nr_isac.cc:291-297`). `sensing.sources`
  (comma list) overrides the legacy single `sensing.source`. Live conf:
  `sources = "csi_rs,pdsch_dmrs"`.
- Fused `ref_type` = `nr_isac_sources_to_ref_type()` → `"fused(csi_rs+pdsch_dmrs)"`
  (single source keeps its plain name; `detection_report.cc:41-62`).
- **Design rule for Phases 2/4:** all new per-row CIR/peak/phase tracking must be written
  against **"whatever real rows are present, using each row's own native comb"**
  (`cpi_row_comb[]`), never hardcoded to a specific RS. This cell has no PRS/TRS, so
  CSI-RS (comb-12, 20 ms) + PDSCH DM-RS/data (comb-2/1, scheduling-dependent) is the only
  source set — do **not** add a PRS-dependent primary path. SFO delay-drift is physical
  and **source-independent** (fit across *all* rows), but the ISI/noise-floor gating must
  be **within-source** (comb-12 and comb-1/2 rows have different floors) per Phase 3.2.

---

## 6. Proposed file layout for Phases 1–6

New code under `openair1/PHY/NR_UE_ISAC/` (per constraints), engine-thread only, no RT
surface. Register each new `.cc` in `CMakeLists.txt` `NR_UE_ISAC_SRC` (lines 1035-1040)
and verify the build picks it up.

| File | Phase(s) | Contents |
|---|---|---|
| `isac_sync.h` / `isac_sync.cc` | 1,2,3,4 | New `class cpi_synchronizer`: per-row LOS CIR-peak extraction (parabolic sub-sample STO), CFO line-fit + alpha-beta across CPIs, CPE de-rotation, SFO delay-drift fit with within-source ISI gating, and the closed-loop LOS-residual bias. Operates on the raw accumulated grid + `occ_all` + `cpi_row_time` + `cpi_row_comb`, **before** Stage-4b. State PLL/alpha-beta bandwidths and SFO outlier-rejection method in comments. |
| `isac_resample.h` / `isac_resample.cc` | 3 | Cubic Farrow fractional-delay resampler **and/or** the freq-domain per-row phase-ramp SFO corrector. Phase 4 picks and justifies (default: freq-domain ramp to avoid a second FFT round trip). |
| `sensing_engine.cc` (edit) | 1–4 | Call the synchronizer at CPI-close before `interp_freq_row`/`resample_slow_time`; feed corrected rows onward. No change to `range_doppler` inputs' shape. |
| `range_doppler.cc` (edit, Phase 7a only) | 6a | Extend `inject_selftest`/config to impose a known STO/CFO/SFO on a synthetic LOS + clean background. Clutter/CFAR/NMS internals untouched. |
| `detection_report.{h,cc}` (edit) | 5 | Add per-CPI STO/CFO/SFO + LOS-residual as **new optional** JSON fields, only after confirming `repos/isac` `isac-track replay` tolerates unknown fields (serde default). If unsafe, emit a sibling `<out_path>_sync.csv` instead. |
| `tests/isac_sync_test.cc` (new) + `tests/isac_dsp_test.cc` (new, folds in the missing dealias/fusion coverage) | 6a | Standalone offline harness linking `NR_UE_ISAC`; synthesizes CPIs with injected STO/CFO/SFO + targets, asserts LOS pinning within named bin tolerances and Doppler-smear-with/without-correction. Add a CMake test target; document its invocation in `runbook.md` §6.1. |
| `runbook.md` §8 (edit), CLAUDE.md "Implemented so far" (edit) | 6b, wrap-up | New §8 subsection for the OTA baseline → corrections-on → guard-narrowing → CPI-duration sweep → regression A/B procedure. |

Phasing → commits (one reviewable commit each, per constraints):
Phase 1 STO · Phase 2 CFO+CPE · Phase 3 SFO · Phase 4 closed-loop LOS pinning ·
Phase 5 instrumentation · Phase 6 tests+runbook. Do not combine 2–5.

---

## 7. Open items to resolve before Phase 1 (need a decision / confirmation)

1. **Doc/commit location:** this doc is at `openairinterface5g/docs/` (the git repo).
   The other design docs (`runbook.md`, `CFR_FUSION_HANDOVER.md`) live one level up at
   `/home/sens/NICOLA/`. Confirm you want the new doc + the ISAC module committed on a
   feature branch off `develop` (module is currently untracked).
2. **Missing offline harness:** Phase 6a will **create** `tests/` targets, not extend the
   non-existent `scratchpad/*_test.cc`. Confirm that's acceptable.
3. **`zero_range_guard` ground truth is 4, not 9** — Phase 7b baseline/narrowing criteria
   should be written against 4. Confirm.
4. **`nms_doppler_bins = 3inv;` typo** in the live conf — fix now or leave to the run
   operator? (Not part of this task's diff.)
5. **LOS baseline bin (~bin 6):** to be re-confirmed empirically at Phase 1 on a clean
   capture before it is treated as "the" LOS bin; not assumed constant across configs.
