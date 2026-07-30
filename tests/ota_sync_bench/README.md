# OTA sync bench (office-friendly, no gNB/RU/B210+X410 rig needed)

**Status (2026-07-30): live-validated on real hardware.** CFO and SFO are both confirmed
against known injected ground truth (not just cross-checks agreeing with each other) — see
"Live validation results" below. STO locks and corrects cleanly but its own accuracy is only
plausibly (not rigorously) confirmed so far.

Validates the NR_UE_ISAC OTA sync stack (`openair1/PHY/NR_UE_ISAC/isac_sync.{h,cc}` --
STO/CFO/SFO tracking, `ota_sync_passive_ue.md` Phases 1-3) against a **real RF link between
two independent, free-running SDRs**, decoupled entirely from the full n78 cell
(`OTA_RADIO_TEST_PLAN.md`'s LiteOn gNB + B210 + X410 rig). No attach, no RRC, no DU/CU --
just a cable (or a few metres of air) between two boards on the same desk.

Two independent free-running clocks is not a compromise here -- it's the actually-realistic
case: CLAUDE.md's OTA sync task target is **autonomous UEs**, i.e. exactly "no shared hardware
clock reference." A GPSDO-disciplined pair would under-test this stack.

## What this proves, and what it doesn't

`isac_sync.cc`'s trackers estimate/correct a sub-bin delay (STO), a residual carrier offset
(CFO), and a sample-clock drift (SFO) from the CFR grid built at the CSI-RS/DMRS reference
REs. That estimation/correction MATH is entirely carrier- and cell-agnostic -- it only needs a
row-major `[cpi_rows][nof_subc]` CFR grid with an occupancy mask and per-row timestamps (see
`isac_sync.h`'s `cpi_sto_tracker`/`cpi_cfo_tracker`/`cpi_sfo_tracker::process()` signatures).
This bench builds exactly that grid from a real RF capture instead of the synthetic
hand-injected one `openair1/PHY/NR_UE_ISAC/tests/isac_sync_test.cc` uses, and runs the
IDENTICAL production tracker classes against it.

It does **not** validate: real 5G NR framing/PDSCH/CSI-RS extraction (no MIB/SIB/RRC anywhere
in this bench), the real cell's actual comb/PRB/bandwidth (use whatever's convenient --
see Tier 1 below), or anything about range-Doppler/CFAR/detection downstream of sync. That
still needs Phase 6b (`runbook.md` §8.9) once the full rig is available.

## Two tiers

**Tier 0** (minutes, no C++ build): TX a plain CW tone with a deliberately mistuned LO,
measure the baseband frequency at RX via a simple phase-unwrap. Confirms the RF link and gain
settings work and sanity-checks CFO sign/magnitude. Does **not** exercise STO/SFO at all -- a
bare tone has no comb structure for those trackers to lock onto.

**Tier 1** (the real thing): TX a repeating, fully-known OFDM reference symbol, capture it,
reconstruct Ĥ = Y/X at every subcarrier exactly like the real CSI-RS/PDSCH taps do, and feed
that into the actual `cpi_sto_tracker`/`cpi_cfo_tracker`/`cpi_sfo_tracker` C++ classes via a
small new tool (`isac_sync_replay`). Functionally this is Phase 6a's offline self-test, but
fed by a real RF capture instead of synthetic injection.

## Setup

Cable + attenuator between the two SDRs is strongly preferred over open air in an office --
repeatable SNR, no interference into/from other 2.4/5 GHz gear. Pick an unlicensed-adjacent or
otherwise safe test frequency and keep TX gain low; a directional coupler or two fixed
attenuators (20-30 dB each) in series is enough to avoid saturating the RX front end at close
range.

Files in this directory:

| File | Role |
|---|---|
| `ofdm_common.py` | Shared pilot sequence + FFT-bin mapping (TX and RX MUST agree -- imported by both sides, never duplicated) |
| `gen_comb_waveform.py` | Tier 1: builds the repeating known OFDM reference waveform |
| `gen_tone.py` | Tier 0: builds a plain CW baseband file |
| `tx_uhd.py` / `rx_uhd.py` | UHD (B2xx/X4xx/N2xx/...) TX loop / RX capture, via `python3-uhd` |
| `tx_bladerf.sh` / `rx_bladerf.sh` | bladeRF-cli equivalents (verify `set frequency`/`set samplerate` syntax against your installed `bladeRF-cli --help-interactive` first -- see the scripts' header comments) |
| `convert_iq.py` | fc32 (this bench's common format) <-> bladeRF's SC16Q11 `bin` format |
| `process_capture.py` | Tier 1: Y/X CFR extraction + independent "trusted" CFO/STO/SFO cross-check + grid-dump writer. `--max-rows` picks the CPI size (see the window-size tradeoff below); `--los-bin-offset` (default 20, see "Known gotchas") parks the CIR peak away from the tracker's search-window wrap boundary |
| `grid_dump.py` | Binary format shared between `process_capture.py` and the C++ tool |
| `tone_cfo_check.py` | Tier 0: phase-unwrap CFO measurement |
| `../../openair1/PHY/NR_UE_ISAC/tools/isac_sync_replay.cc` | Runs the REAL production trackers against `process_capture.py`'s grid dump |

If your two SDRs are different driver families (e.g. one USRP + one bladeRF), TX from one and
RX from the other using whichever pair of scripts matches each board -- everything downstream
(`process_capture.py`, `isac_sync_replay`) only cares about the resulting fc32 file, not which
tool produced it.

## Tier 0 walkthrough

```bash
# On the TX box:
python3 gen_tone.py --rate 2e6 --duration 30 --out /tmp/tone.fc32 --meta-out /tmp/tone.json
python3 tx_uhd.py --args "<tx device args>" --file /tmp/tone.fc32 --meta /tmp/tone.json \
    --center-freq 2400e6 --cfo-hz 500 --gain 20 --duration 30

# On the RX box, at the same time:
python3 rx_uhd.py --args "<rx device args>" --center-freq 2400e6 --rate 2e6 --gain 30 \
    --duration 25 --out /tmp/tone_capture.fc32

# Analyze:
python3 tone_cfo_check.py --capture /tmp/tone_capture.fc32 --rate 2e6 --injected-cfo-hz 500
```

Expect `measured baseband tone` close to 500 Hz. If it's wildly off (kHz-scale error, wrong
sign, or garbage), fix the RF link (gain/frequency/cabling) before moving to Tier 1 -- Tier 1's
failure modes are much harder to debug on top of a broken link.

## Tier 1 walkthrough

```bash
# 1. Generate the reference waveform (do this once, copy the .fc32 + .json to the TX box).
#    Start LOW-RATE -- 30.72 Msps (--nof-prb 52 --fft-size 1024) reliably overflowed on the
#    hardware this was validated on (USB/CPU couldn't keep up, both TX underflow and RX
#    overflow). --nof-prb 12 --fft-size 256 --scs-hz 30000 (7.68 Msps) ran clean and is what
#    every real result below used. Only raise the rate once a run shows zero U/O prints on
#    either side.
python3 gen_comb_waveform.py --nof-prb 12 --fft-size 256 --scs-hz 30000 \
    --nof-symbols 400000 --out /tmp/tx_waveform.fc32 --meta-out /tmp/tx_waveform.json

# 2. TX continuously (baseline run, no deliberate impairment):
python3 tx_uhd.py --args "<tx args>" --file /tmp/tx_waveform.fc32 --meta /tmp/tx_waveform.json \
    --center-freq 2400e6 --gain 30 --duration 15

# 3. RX capture, at the same time:
python3 rx_uhd.py --args "<rx args>" --center-freq 2400e6 --rate 7.68e6 --gain 30 \
    --duration 15 --out /tmp/rx_capture.fc32
# Watch both consoles for 'U' (TX) / 'OERROR_CODE_OVERFLOW' (RX) -- if you see either, the
# capture has a timing discontinuity and shouldn't be trusted. Lower the rate further or fix
# the USB link (USB3 direct, not a hub; nothing else CPU/USB-heavy running) and retry.

# 4. Build the CFR grid + trusted cross-check. --max-rows picks the CPI size -- see the
#    window-size tradeoff below; 256 is a good default for a first check.
python3 process_capture.py --capture /tmp/rx_capture.fc32 --meta /tmp/tx_waveform.json \
    --out /tmp/grid_dump.bin --max-rows 256

# 5. Run the real trackers:
cd /home/sens/NICOLA/openairinterface5g-isac-ota-sync/cmake_targets/ran_build/build
./isac_sync_replay --grid /tmp/grid_dump.bin
```

`isac_sync_replay` prints, in order: `process_capture.py`'s independent trusted CFO/SFO/delay
estimate (a plain frequency-domain phase-difference method -- deliberately NOT the production
code path, so it's a genuine cross-check, not the same bug reproduced twice), the real
`cpi_sto_tracker`/`cpi_cfo_tracker`/`cpi_sfo_tracker` output, a side-by-side delta, and a
post-correction residual pass (re-running fresh trackers on the now-corrected grid, mirroring
`isac_sync_test.cc`'s convention -- residuals should collapse well below the first-pass
numbers if the correction is doing real work).

### Controlled (Tier 1b) ground-truth injection

For a cleaner pass/fail than "whatever the two boards' free-running clocks happen to drift by
today", inject a known impairment and compare an injected run against a baseline run taken
immediately before/after it (so native drift mostly cancels out of the delta):

- **CFO**: `tx_uhd.py --cfo-hz 300` on top of a `--cfo-hz 0` baseline. Compare
  `(injected run's cfo_hz) - (baseline run's cfo_hz)` against `300` -- not the injected run's
  absolute number alone, which still carries the native offset underneath it.
- **SFO**: `tx_uhd.py --sfo-ppm 2.0`, same paired-delta comparison. The parameter's sign matches
  `isac_sync.h`'s convention directly (a requested `+X` ppm should measure as a `+X` ppm delta) --
  see "Known gotchas" if you're on a version predating that fix.
- **STO**: `gen_comb_waveform.py --sto-samples 1.0` bakes the delay into the waveform itself (not
  a `tx_uhd.py` flag), so it needs two separate waveform files/captures, e.g. `--sto-samples 0`
  vs `--sto-samples 1.0`. Because the tracker only measures a *sub-bin residual* (coarse
  symbol-boundary alignment absorbs the integer part), the expected delta is
  `(sample_period / bin_period)`, taken **mod 1 bin** -- it can legitimately show up as either
  that value or `that value - 1`, whichever `coarse_align()`'s independent per-capture alignment
  choice happens to land closer to. This test is inherently noisier than CFO/SFO's (see "Live
  validation results" below) since the two captures aren't simultaneous.

Pass `--injected-sto-samples` / `--injected-cfo-hz` / `--injected-sfo-ppm` to
`process_capture.py` (whatever you used above) and `isac_sync_replay` will print them
alongside its own measurements.

## Live validation results (2026-07-30, real two-SDR hardware)

**CFO and SFO are both confirmed against known injected ground truth**, each at the CPI size it
needs (see the window-size tradeoff below) -- not just cross-checks agreeing with each other:

| | ground truth | tracker delta | trusted delta |
|---|---|---|---|
| CFO (256 rows) | +300 Hz | +305.3 Hz (1.8%) | +267.5 Hz (10.8%) |
| SFO (140000 rows) | +2.0 ppm | -1.991 ppm\* (0.4% mag.) | -1.908 ppm\* (4.6% mag.) |

\* measured before the `tx_uhd.py` sign fix below; magnitude confirms the estimator, the
run that produced this predates the corrected sign convention.

STO locks cleanly (0% flywheel once seeded correctly) and its correction visibly reduces the
residual on re-measurement, but a dedicated ground-truth delta test came out directionally
plausible rather than a clean confirmation -- see "Known gotchas" for why that specific test is
noisier than CFO/SFO's.

### Window-size tradeoff (why one CPI size doesn't fit all three)

This bench's "row" is one OFDM symbol (~35.7 us at 7.68 Msps/12 PRB) -- about **14x finer** than
production's one-row-per-slot cadence (~0.5 ms). That mismatch creates a real tradeoff:

| CPI size | real time span | CFO | SFO |
|---|---|---|---|
| 256 rows | ~9 ms | accurate (validated above) | unresolvable (drift too small to fit, `r_squared` near 0, correctly withheld) |
| 140000 rows | ~5 s | diverges (sequential unwrap breaks down over this many rows -- see `isac_sync.h`'s own documented limitation) | accurate (validated above, `r_squared > 0.999`) |

Pick a short `--max-rows` (a few hundred) to validate CFO, and a long one (spanning several real
seconds) to validate SFO. Neither failure mode is a bug: CFO's divergence is this bench's row
density hitting a documented unwrap limitation no real (per-slot-cadence) CPI gets near; SFO's
short-window silence is the estimator correctly refusing to fit noise.

## Known gotchas (found the hard way, all in this bench's harness -- none in `isac_sync.cc`)

1. **`range_res_m` unit mismatch.** `isac_sync.cc`'s `nominal_los_bin()` uses the *bistatic*
   convention (no `/2`); an earlier version of `process_capture.py` used the monostatic one
   (`/2`), silently mis-seeding every tracker's search window by 2x. Fixed -- `process_capture.py`
   now matches the bistatic convention throughout.
2. **Negative residuals need a circular wrap, not `abs()`.** The CIR the tracker searches is
   built via IFFT, which is inherently circular: a `-9.5` bin residual is the same physical point
   as `nof_subc - 9.5`, not `+9.5`. Using `abs()` reflects it onto the wrong side of the search
   window (measured: 99.8% flywheel, tracker never locking). `process_capture.py` now wraps with
   Python's `%` (which already yields the correct non-negative representative).
3. **The tracker's search window is not circular, and coarse sync can land the residual right on
   that boundary.** `estimate_row()`'s window is `[max(1, c-halfwin), min(M-2, c+halfwin)]` --
   bins 0 and `M-1` are structurally unreachable. `process_capture.py`'s coarse alignment is
   *good enough* to sometimes land the true residual within a fraction of a bin of exactly 0,
   which is the one place the tracker can't search (measured: `n_valid=0` across STO/CFO/SFO at
   once). Fixed via `--los-bin-offset` (default 20 bins), which deliberately parks the CIR peak
   mid-array with a frequency-domain phase ramp -- matching real deployments, where the LOS tap
   naturally sits at a non-zero bin, not working around a real tracker limitation.
4. **`tx_uhd.py --sfo-ppm`'s sign was inverted relative to `isac_sync.h`'s convention.** The old
   `rate*(1+ppm*1e-6)` formula requests a *faster* TX clock for positive ppm, which makes the
   RX-referenced delay *decrease* over time -- the opposite of `isac_sync.h`'s "positive ppm =
   delay increasing" definition. Confirmed empirically (both the tracker and the independent
   trusted estimator measured ~-2.0 ppm for a requested +2.0). Fixed: now `rate*(1-ppm*1e-6)`, so
   a requested `+X` ppm measures as a `+X` ppm delta.
5. **`coarse_align()`'s reference waveform is exactly periodic**, so a large search window has
   many near-equal-magnitude correlation peaks (one per symbol period) -- searching too far past
   the first few periods risks landing on a later replica that's drifted or faded more than an
   earlier one. Default search window is capped at a handful of symbol periods for this reason;
   don't widen it without a reason.

## Building `isac_sync_replay`

```bash
cd /home/sens/NICOLA/openairinterface5g-isac-ota-sync/cmake_targets/ran_build/build
cmake -G Ninja -DENABLE_TESTS=ON -DENABLE_WEBSRV=OFF -DENABLE_TELNETSRV=OFF ../../..
ninja isac_sync_replay
```

(`-DENABLE_WEBSRV=OFF -DENABLE_TELNETSRV=OFF` avoid an unrelated `libulfius` dependency pulled in
by `build_oai --build-lib all`; not needed if you configure directly with cmake as above, which
only builds what `isac_sync_replay` actually depends on.) It links against the same `NR_UE_ISAC`
static lib as everything else in `openair1/PHY/NR_UE_ISAC/` -- no gNB/UE softmodem dependency.
