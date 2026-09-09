# bladeRF SC8/oversample isolation probes (2026-09-09)

## `oversample_repro.c` -- the upstream-ready reproducer

Single-file, no OAI dependency. Captures at a real strong signal ("cell" freq) and 150 MHz
away ("noise" freq), once via plain SC16_Q11 (no oversample, 30.72 Msps), once via SC8_Q7 +
`BLADERF_FEATURE_OVERSAMPLE` (122.88 Msps), and prints the cell/noise power ratio for both.

Build: `gcc -O2 -o oversample_repro oversample_repro.c -lbladeRF -lm`
Run:   `sudo ./oversample_repro <serial> <cell_freq_hz> <gain_db>`

**Measured result on this unit** (libbladeRF 2.5.0-0.2024.05, firmware 2.4.0-git-a3d5c55f,
tested on both FPGA v0.15.3 and v0.16.0 -- identical result on both):

```
SC16 (no oversample, 30.72 Msps): cell/noise power ratio = 25.75 dB  -> PASS (real signal seen)
SC8  (oversample,   122.88 Msps): cell/noise power ratio = -3.77 dB  -> FAIL (indistinguishable from noise)
```

## Upstream context (Nuand/bladeRF)

- Firmware 2.4.0-git-a3d5c55f (exactly ours) already has an open, unresolved report of oversample
  behaving incorrectly: [Nuand/bladeRF#940](https://github.com/Nuand/bladeRF/issues/940)
  ("Sample Rate is Halved When Using BLADERF_FEATURE_OVERSAMPLE").
- [Nuand/bladeRF#980](https://github.com/Nuand/bladeRF/issues/980): the driver hardcodes a 56 MHz
  analog bandwidth ceiling even when the sample rate is set to 122.88 Msps in oversample mode.
- Community report (forum, no official Nuand reply): "on bladeRF 2.0, the 8-bit mode only reduces
  USB bandwidth and does not raise the RFIC clock limits" -- the AD9361's real ADC ceiling stays
  ~61.44 Msps regardless of 8-bit packing; 8-bit only makes a rate the RFIC ALREADY supports
  cheaper to move over USB, it does not extend what the RFIC can actually digitize.
- libbladeRF CHANGELOG shows oversample/SC8 support landing once, in 2.5.0 (2023-02-13), with no
  subsequent fix entries for any of the above.

**Conclusion**: this looks like a genuine RFIC/ADC-level limitation of the bladeRF 2.0 micro at
100 MHz instantaneous bandwidth, not an OAI or firmware-version-specific bug -- consistent across
two FPGA versions and matching community reports. 106 PRB / ~50 MHz (61.44 Msps, plain SC16, no
oversample) is the validated-working ceiling on this hardware; see the passive-rx run notes.

## Other probes here (earlier, superseded by oversample_repro.c above)
- `sc8_oversample_probe.c` / `sc16_control_probe.c`: the original two-file version of the same test.
