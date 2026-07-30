# OTA sync bench (office-friendly, no gNB/RU/B210+X410 rig needed)

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
| `process_capture.py` | Tier 1: Y/X CFR extraction + independent "trusted" CFO/STO/SFO cross-check + grid-dump writer |
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
# 1. Generate the reference waveform (do this once, copy the .fc32 + .json to the TX box):
python3 gen_comb_waveform.py --nof-prb 52 --scs-hz 30000 --fft-size 1024 \
    --nof-symbols 4000 --out /tmp/tx_waveform.fc32 --meta-out /tmp/tx_waveform.json
# Lower --nof-prb / --fft-size (e.g. --nof-prb 25 --fft-size 512) if your SDRs can't do
# 30.72 Msps -- the tracker math doesn't care about the real cell's actual carrier size.

# 2. TX continuously (baseline run, no deliberate impairment):
python3 tx_uhd.py --args "<tx args>" --file /tmp/tx_waveform.fc32 --meta /tmp/tx_waveform.json \
    --center-freq 2400e6 --gain 30 --duration 20

# 3. RX capture, at the same time:
python3 rx_uhd.py --args "<rx args>" --center-freq 2400e6 --rate 30.72e6 --gain 30 \
    --duration 15 --out /tmp/rx_capture.fc32

# 4. Build the CFR grid + trusted cross-check:
python3 process_capture.py --capture /tmp/rx_capture.fc32 --meta /tmp/tx_waveform.json \
    --out /tmp/grid_dump.bin

# 5. Run the real trackers:
cd <build dir>   # cmake_targets/ran_build/build, ENABLE_TESTS=ON
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
today", inject a known impairment and compare against a baseline run:

- **STO**: `gen_comb_waveform.py --sto-samples 3.0` pre-shifts the whole waveform by a known
  fractional-sample delay. Because the tracker only measures a *sub-bin residual* (the coarse
  symbol-boundary alignment absorbs the integer part -- see `isac_sync.h`'s scope note), don't
  compare this run's absolute `mean_frac_bin` to `3.0` directly. Instead run once with
  `--sto-samples 0` and once with e.g. `1.0`, and confirm `mean_frac_bin` shifts by the
  corresponding fraction-of-a-bin amount between the two.
- **CFO**: `tx_uhd.py --cfo-hz 200` is a clean, directly-comparable absolute ground truth --
  compare the tracker's `cfo_hz` (or `process_capture.py`'s `trusted_cfo_hz`) straight against
  `200 + <baseline run's own residual CFO>`.
- **SFO**: `tx_uhd.py --sfo-ppm 1.5` requests an actual device sample rate offset from nominal
  -- same comparison pattern as CFO.

Pass `--injected-sto-samples` / `--injected-cfo-hz` / `--injected-sfo-ppm` to
`process_capture.py` (whatever you used above) and `isac_sync_replay` will print them
alongside its own measurements for easy comparison.

### Validated offline before you touch hardware

`process_capture.py`'s DSP chain (coarse sync, Ĥ = Y/X extraction, the trusted cross-check
estimators) was validated end-to-end against a software channel emulator (delay + CFO + SFO +
AWGN injected in Python, no SDR involved) before this bench was handed off: trusted CFO/SFO/
delay estimates matched known-injected values to within ~1% on a 30 dB SNR synthetic capture.
`isac_sync_replay` was built and run against that same synthetic grid dump -- it links clean
against `NR_UE_ISAC` and produces sane, right-order-of-magnitude output. **None of this
replaces a real RF run** (it can't validate the actual TX/RX chain, gain settings, or a real
two-SDR clock relationship), but it means the scripts themselves are debugged, not just typed
and hoped-for.

One finding worth carrying into your first real run: in that same offline validation, the
production `cpi_cfo_tracker`'s fitted CFO differed from the independent trusted estimate by
~9% under combined STO+CFO+SFO (SFO tracked accurately; CFO less so) -- consistent with
`isac_sync_test.cc`'s own `combined_impairment_and_target_survive_correction` test needing
looser tolerances than the isolated single-impairment tests for exactly this reason (documented
there as a real interaction effect between the estimators, not a bug). Don't be surprised if
Tier 1's CFO number is the noisiest of the three under combined impairments; SFO and STO have
historically been the more robust of the three in this codebase's own test suite.

## Building `isac_sync_replay`

```bash
cd /home/sens/NICOLA/openairinterface5g/cmake_targets/ran_build/build
cmake -DENABLE_TESTS=ON .   # only needed if ENABLE_TESTS wasn't already on
ninja isac_sync_replay
```

It links against the same `NR_UE_ISAC` static lib as everything else in
`openair1/PHY/NR_UE_ISAC/` -- no gNB/UE softmodem dependency.
