# Offline baseline alternatives, 2026-09-11

The radio is no longer needed to read or process either recording. The original
short baseline was not replaced. No gNB logs, scheduler hints, externally shared
clock, or custom coordination were used to capture the longer alternative.

## A. Decoder oracle: 160 ms, four RX channels

`/home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.xRBLS0`

- Native same-build `replay.bin`, receiver-frame IQ exports, config, source hashes,
  source patch, frozen executable and local shared libraries.
- Two independent offline DL replays: 55/55 TBs byte-identical, zero failed controls.
- Supplemental UL replay: 3/8 CRC passes at DCI width 43 and 3/9 at width 45.
- Each UL decode was repeated; zero status/length/TB-hash mismatches. This is not
  comparison against live UL TB bytes, which this recorder does not save.
- Live whole-run UL count was 26244/30699 CRC passes. Do not substitute that rate
  for the 6/17 offline short-window measurement or infer their discrepancy's cause.
- The first UL diagnostic attempt was VOID due to missing monitor initialization.
  It is retained as `ul_validation/void_initialization_*.log`, never scored.
- The original immutable manifest predates supplemental UL validation. Consult
  `ul_validation/result.json` for the additional evidence and unresolved identity.

```sh
base=/home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.xRBLS0
bash "$base/replay.sh"
bash "$base/ul_validation/replay_ul.sh" 43
bash "$base/ul_validation/replay_ul.sh" 45
```

UL waveform hypotheses came from this receiver's learned-width log and decoded
SIB1 common BWP/TDRA. They are offline reference hypotheses, not an automatic
configuration oracle. HARQ/NDI/DAI identity remains ambiguous across equivalent
waveforms. Successful CRCs must not be used to claim that metadata is known.

## B. Continuous raw RF: 4 seconds, four RX channels

`/home/sens/NICOLA/captures/raw_fullband_4s.p67kVf`

- `rx0.sc16` through `rx3.sc16`: little-endian interleaved signed int16 I,Q.
- 491520000 complex samples per channel, 122880000 samples/second.
- 3450000000 Hz measured/tuned center, all four receive inputs, RX1, gain 40 dB.
- 7864320000 data bytes total, 25 times the short recording's duration.
- Full receiver passband covering the current 100-MHz carrier. No resource,
  symbol, DMRS, CSI-RS or decoded-message selection/filtering was applied.
- Capture starts independently of NR acquisition. No receiver CFO/STO correction,
  frame alignment, MIB/PCI/BWP or DCI interpretation is applied to the saved IQ.
- `timestamps.tsv`: exact sample-offset/count/device-tick records for all 7500
  receive blocks. All blocks and channels passed timestamp/alignment checks.
- Zero accepted RX metadata errors, no timestamp jumps, equal file extents and
  unchanged NIC missed counter. `validation.json` records this transport proof.
- `signal_quality.json` is a decimated amplitude check: all channels nonzero and
  no sampled near-full-scale components. This is not PHY decode coverage.
- Capture source, executable, UHD version, device arguments, actual RF settings,
  logs, independent validator and SHA256 checksums are saved alongside the IQ.

```sh
cd /home/sens/NICOLA/captures/raw_fullband_4s.p67kVf
sha256sum -c SHA256SUMS
```

Read any bounded interval without loading the entire file or accessing hardware:

```python
import numpy as np
path = '/home/sens/NICOLA/captures/raw_fullband_4s.p67kVf/rx0.sc16'
iq = np.memmap(path, dtype='<i2', mode='r').reshape(-1, 2)
window = iq[:1228800].astype(np.float32)  # 10 ms; RF capture epoch, NOT NR frame identity
samples = window[:, 0] + 1j * window[:, 1]
```

There is no native full-receiver replay adapter for this unframed format yet.
The existing native job-replay reader must NOT be fed these raw files or be
silently seeded with the short recording's frame/configuration metadata.

## Coverage of the requested work

| Work | Saved evidence | Limit still requiring offline work |
| --- | --- | --- |
| DCI 1_0 / 1_1 interpretation | Raw full-band IQ; short receiver logs/jobs | Decode/index the longer recording; multiple configurations require synthetic vectors or other cells |
| Hypothesis scoring / ambiguity | Positive DL/UL decoder controls plus unfiltered signal | HARQ/NDI/DAI metadata is not established merely by waveform CRC |
| Dynamic configuration learning | Four seconds of unbiased raw observations | Actual BWP/configuration transitions are not guaranteed in this cell/window |
| Acquisition | Unframed IQ, hardware timestamps and measurement geometry only | SSB/PBCH/SIB1 cold-start coverage must be decoded and measured offline |
| Reacquisition | Long continuous reference allows deterministic erasure, gaps and CFO/STO perturbations | Label derived interruptions synthetic; natural sync-loss recovery was not captured/validated |
| Health metrics | Original four-minute native run log and saved-IQ test logs | Integrate the same metrics into raw replay |
| CSI-RS / dedicated layouts | Every transmitted waveform in the recorded passband is preserved | CSI-RS transmission/configuration and dedicated RRC presence have NOT been confirmed |
| Regression | Frozen short-baseline replay, CRC/byte controls, raw hashes | All-capture/manual field equality and generalized layouts remain incomplete |

"All messages" means no selective loss of the received in-band waveform. It
cannot guarantee that every optional NR message, CSI-RS resource, RRC procedure,
configuration change or network layout was transmitted in these four seconds.
Dedicated RRC may be absent or encrypted; recording longer does not make that
information automatically observable. Unsupported/ambiguous cases must remain
explicit. This baseline is not a declaration that the agnostic-RX gates pass.

## Recorder validation and reproduction

The standalone tool does not change the native receiver, thread placement or
legacy recorder format. It uses bounded preallocated RAM and writes to disk only
after releasing RF. The maximum buffer budget is 8 GiB. Timestamp/overflow/
alignment failures abort and return VOID; existing IQ files are never overwritten.

```sh
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/raw_baseline
c++ -std=c++17 -O2 -Wall -Wextra capture_raw.cpp -o /tmp/capture_raw_baseline $(pkg-config --cflags --libs uhd)
/tmp/capture_raw_baseline --self-test
python3 test_validate_raw.py -v
```

These tests open no device. They cover continuity, overlap/gap, invalid metadata,
empty blocks, partial channel files and NIC-loss rejection. Hardware capture itself
requires separate permission, an unclaimed X410, the radio lock and sufficient RAM.
UHD metadata contract: https://files.ettus.com/manual/structuhd_1_1rx__metadata__t.html
