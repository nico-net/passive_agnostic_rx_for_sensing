# Reference SSB checker and additional raw baselines

## Numerical fix

FFT correlation roundoff in empty synthetic windows was divided by a fabricated
1e-30 energy denominator, producing impossible normalized scores above 1e13.
Compute correlation and energy in complex128/double precision, and assign zero
score to zero-energy windows. Detection thresholds remain unchanged.

The original failing synthetic test is retained. Five checker tests now pass:
standard PSS initial bits; PCI/CFO recovery for PCI 0, 2, 503 and 1007 with
unknown timing and -12345 Hz CFO; noise/missing-SSS rejection; empty input;
and bounded normalized correlation for sparse signals.

## Capture results (sens6)

All three recordings contain four unfiltered sc16 RX channels, each four seconds
at 122.88 MSps. Each recording contains 7,864,320,000 IQ bytes. Exact file extents,
continuous hardware timestamps and unchanged NIC missed counters passed.
These are independent windows, not a continuous stitched twelve-second capture.

| Directory under /home/sens/NICOLA/captures | Reference sequence evidence |
| --- | --- |
| raw_fullband_4s.p67kVf | PCI 2, two observations, 20.000130 ms separation |
| raw_batch.vKkQwC/window_1 | PCI 2, two observations, 20.000130 ms separation |
| raw_batch.vKkQwC/window_2 | PCI 2, two observations, 20.000000 ms separation |

Only the first 40 ms of channel 2 were analyzed. The 3408960000 Hz SSB reference
is an explicit hypothesis from earlier receiver geometry, not blind acquisition.
No PCI is injected. Measured CFO spans approximately -14.61 to -14.78 kHz.
Per-recording `ssb_reference_coverage.json` preserves scores and observations.
The batch contains frozen checker sources, hashes and `offline_tests.log`.
No radio access is performed by these checks. The capture processes finished and
released their UHD device handles before offline analysis.

## Reproduce offline

From `tests/passive_rx/raw_baseline` on sens6:

```sh
OPENBLAS_NUM_THREADS=1 python3 -m unittest -v \
  test_ssb_reference test_ssb_normalization test_validate_raw
OPENBLAS_NUM_THREADS=1 python3 check_ssb_reference.py \
  /home/sens/NICOLA/captures/raw_batch.vKkQwC/window_1 \
  --ssb-center-hz 3408960000 --channel 2 \
  --output /tmp/ssb_window_1_new_report.json
```

Report output must not already exist. Original capture manifests/checksums were
not rewritten; the SSB reports are supplementary, not substitutes for integrity
validation.

## Limitations and acceptance gates

PBCH/MIB CRC, SIB1, DCI, PDSCH, PUSCH and CSI-RS coverage in these raw windows
remain unvalidated. Raw sample completeness does not establish message coverage.
The native full-receiver adapter for unframed raw IQ is still required.
The earlier short native replay remains the DL/UL decoding reference; its UL
result is 6/17 positive CRCs with repeatable diagnostic hypotheses, not fully
agnostic UL interpretation. No additional agnostic acceptance gate is declared
passed by this reference-frequency checker or these recordings.
