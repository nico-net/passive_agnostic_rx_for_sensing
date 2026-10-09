# SCAN-FAST — PDCCH slot scan

**Result: target not met.** The best safe combined replay on
`raw_pci415_12s_b` measured dedicated-only p99 1,127 us (baseline 1,200 us),
and the same combination measured 1,687 us on `raw_pci415_12s_20261008`
(baseline 1,397 us). A 0.66 candidate cutoff gave a faster exploratory run,
but replay audit proved it discards admitted DCIs, so it is not a valid
solution. Per-RNTI no-drop is also not established; the full counts are in
[SCANFAST_COUNTS.json](SCANFAST_COUNTS.json).

## Implementation

All new switches default off. The existing `ISAC_PDCCH_FEP_CACHE=1`,
`ISAC_PDCCH_KNOWN_PRESCREEN=1` and `ISAC_PDCCH_USS_HASH=1` remain separate.

| Switch | Change |
|---|---|
| `ISAC_PDCCH_LLR_CACHE=1` | A scan thread reuses bit-identical LLRs for an exact `(absolute slot, source samples, FO, CORESET bitmap, RB extent, BWP start, nID, symbol span, FFT geometry)` match across passes. Different overlapping extents are demodulated separately because channel-estimation edge filters and P90 autoscaling depend on the extent. `ISAC_PDCCH_LLR_CACHE_VERIFY=1` recomputes hits with the CPU reference and counts any byte mismatch as `llr_cache bad`, falling back to the new LLR. |
| `ISAC_PDCCH_SHARED_PRESCREEN=1` | Computes each discovered CORESET candidate's amplitude-consistency and absolute energy once. Its safe default cutoff is zero: no candidate is rejected by the new screen. `ISAC_PDCCH_SHARED_PRESCREEN_MIN` is an explicit experimental cutoff. The 0.66 cutoff rejected admitted DCIs on replay; see below. Anonymous discovery is unchanged. |
| `ISAC_PDCCH_SHARED_DMRS=1` | Experimental second signal for the shared screen: build a DM-RS grid once per pass and retain a low-amplitude candidate when its correlation exceeds `ISAC_PDCCH_SHARED_DMRS_MIN` (default 0.5). Fail open without a grid. Replay audit found near-zero correlation on exact CRC hits, so this flag is not recommended without further validation. |
| `ISAC_PDCCH_LEN_MEMORY=1` | An unresolved known RNTI tries a CRC-locked peer length from the same CORESET, with a full-range probe every 128 feeds. Each RNTI must still earn its own recurrent CRC/payload evidence; an unfamiliar width can be found on a later probe. |
| `ISAC_PDCCH_NEON_SCORE=1` | Uses AArch64 NEON for the QPSK prescreen and candidate absolute-energy sums, with exact integer accumulation and the original double result. Scalar code remains the default. |
| `ISAC_PDCCH_TASK_CAP64=1` | Reserves per-scan-thread task storage for 64 known RNTIs and two widths each, avoiding the previous 128-task truncation of later CCEs. The reservation happens once per scan thread. |
| `ISAC_PDCCH_GPU_FEP=1` | Loads `libpdcch_gpu.so` independently of PDSCH's `NR_GPU_FEP`. The uncalibrated kernel is checked against CPU LLRs byte for byte; a mismatch or error keeps CPU LLRs. GPU timing and a device-calibrated fast path are deferred. |

## Validation

[OFFLINE VERIFIED, DGX aarch64 sandbox] `nr-uesoftmodem`,
`test_nr_pdcch_scan_fast`, and `test_nr_pdcch_dci_length_sweep` built with
`heavy.sh ninja -C cmake_targets/ran_build/build -j6`; the two focused ctests
passed (2/2). The build used `CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp` because
the sandbox cannot write the shared ccache. Configuration used
`-DCPM_SOURCE_CACHE=/tmp`, with already cached dependencies copied there.

### Replay comparison

The replay commands are in `tests/passive_rx/dgx/scanfast_replay.sh`: one
`heavy.sh` invocation per run, speed 0.25, full 1,474,560,000 samples,
`CAP_TICK=208029025` for `raw_pci415_12s_b` and `210036277` for
`raw_pci415_12s_20261008`, `ISAC_MLOCK=0`. The T26d feature environment is
used apart from GPU NID discovery: this sandbox has no GPU device, and an
attempt with the literal T26d environment yielded zero bank passes and zero
DCIRAW. The replays therefore set `ISAC_PDCCH_NID_GPU=0` and feed the two
previously verified nID 62922 and 10163 CORESET records from
`scanfast_coresets.txt` through the existing discovery-file handoff. Both
still require normal on-replay CRC verification before banking. Host-safe
replay core assignments are scan 8,9 and PDSCH 10–14 via
`ue.passive.ota.scanfast.dgx.cfg`; no thread is pinned to 5–7 or 15–19.
`ISAC_PDCCH_SCAN_TIMING=1 ISAC_PDCCH_TIMING=1 ISAC_DCI_RAW_LOG=1` are set on
every run. Each run must contain SCANTIME, and DCI counts are from DCIRAW
grouped by RNTI. These measurements are replay-only, not OTA. The parallel
comparison uses one scan consumer on 8 and its bank helper on 9.

`SCANTIME_FINE` uses 1 us bins up to 10 ms; the max is the exact SCANTIME
maximum. All rows below used the final binary, both nID banks verified, and
the 0.0 safe shared-screen cutoff. `combined` adds the four CPU flags,
`ISAC_PDCCH_TASK_CAP64=1`, and the existing `ISAC_PDCCH_PAR_BANKS=1` helper.
`over500` is the number of dedicated-only slots over 500 us.

| Capture | Variant | Occasions | p50 / p99 / max (us) | over500 | DCIRAW (RNTIs) |
|---|---|---:|---:|---:|---:|
| `raw_pci415_12s_b` | baseline | 20,000 | 459 / 1,200 / 7,263 | 7,120 | 5,733 (11) |
| `raw_pci415_12s_b` | safe shared screen | 20,000 | 688 / 1,525 / 7,303 | 18,490 | 4,680 (4) |
| `raw_pci415_12s_b` | length memory | 20,000 | 472 / 1,384 / 10,570 | 7,892 | 2,477 (13) |
| `raw_pci415_12s_b` | combined | 20,000 | 469 / 1,127 / 8,253 | 6,693 | 6,430 (4) |
| `raw_pci415_12s_20261008` | baseline | 16,000 | 373 / 1,397 / 8,862 | 2,151 | 1,342 (2) |
| `raw_pci415_12s_20261008` | safe shared screen | 20,000 | 655 / 1,605 / 8,774 | 17,828 | 1,439 (2) |
| `raw_pci415_12s_20261008` | length memory | 16,000 | 397 / 1,338 / 7,956 | 2,687 | 1,349 (2) |
| `raw_pci415_12s_20261008` | combined | 20,000 | 430 / 1,687 / 7,136 | 4,498 | 1,662 (3) |

The earlier single-lever runs, before fine timing was added, only have the
coarse `SCANTIME` p99 bound of <=2,097 us for cache and NEON on each capture.
The cache had **zero hits**: 48,820 misses on `b`, 44,644 on `20261008`;
the final combined runs likewise had zero hits (48,599 and 43,846 misses).
Both CORESETs use different nIDs and the LLR computation depends on each
CORESET extent, so an exact-output cache cannot reuse their demodulated
values. The existing FFT cache already shares the upstream transform. The
NEON score is bit-identical to scalar in the focused unit test, but its
coarse replay p99 did not change. The cache and NEON runs and their companion
baselines are also preserved in [SCANFAST_COUNTS.json](SCANFAST_COUNTS.json).

The JSON lists **every DCIRAW count by RNTI** for each run. These replays do
not demonstrate the requested no-drop condition: for example, on `b`, the
safe shared-screen run has 4 observed RNTIs versus 11 in its baseline; on
`20261008`, RNTI `0x8f5a` has 547 versus 554. Async anonymous discovery and
bank/persistence timing also cause large variation between unmodified
baselines (the two `b` baselines observed 19 and 11 RNTIs), and some rows
cover different numbers of dedicated-only occasions. A strict causal
per-RNTI A/B comparison therefore needs a deterministic known-RNTI replay
harness. The safe new screen rejects zero candidates, and the NEON/cache/GPU
reference unit tests check bit identity, but that is narrower evidence than
full end-to-end no loss for 64 simultaneous RNTIs. No 64-RNTI capture was
available.

### Prescreen detection audit

[MEASURED replay, raw_pci415_12s_b] With the experimental amplitude cutoff
0.66 and `ISAC_PDCCH_SHARED_PRESCREEN_AUDIT=1`, 851 rejected candidates
passed an exact known-RNTI polar CRC in one full run. The audit restored those
candidates to the normal pipeline; **828 became admitted DCIRAW detections**
after persistence and duplicate checks. The lowest admitted amplitude score
was 0.462671, below the noise mean near 0.637. This proves 0.66 is unsafe on
this capture. The audit intentionally performs extra polar decodes, so its
SCANTIME does not represent production latency. The default shared-score
cutoff is therefore zero. An earlier replay of the second capture found 83
exact CRC hits below 0.66; that run did not yet count subsequent admission.

[MEASURED replay, raw_pci415_12s_b] An audit that built the DM-RS grid saw
correlation as low as 0.0015 on recovered exact-CRC candidates. The existing
DM-RS correlation statistic also cannot be given a useful fixed cutoff from
these data. `ISAC_PDCCH_SHARED_DMRS` remains experimental and default off.

## Exact T26d OTA environment for operator validation (not run here)

Config: `tests/passive_rx/dgx/ue.passive.ota.t26d.dgx.cfg`. The original
T26d core assignments are for OTA, with scan cores 5,6 and
`ISAC_PDSCH_CORES=7,8,9,15,16`. They were not used by the sandbox replays.
The following is T26d's OTA feature set plus the candidate CPU switches under
test. It is an **unvalidated operator candidate**, since the replay target
was not met. `ISAC_PDCCH_GPU_FEP` stays off until device calibration. `ISAC_RX_BRANCH_FO`
and `ISAC_PDSCHQ_SIGNAL_TIME` stay unset.

```text
ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0
ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1
ISAC_COREMAP_IDSWEEP_DUR=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1
ISAC_GPU_SELFCHECK=0 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1
ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1
ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300
ISAC_PDCCH_SCAN_TIMING=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1
ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 ISAC_PDSCH_SOLVER_REUSE=1
ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1
ISAC_PDSCH_SOLVER_PROBE_RATE=0.3 ISAC_PDSCH_SOLVER_PROBE_BURST=16
ISAC_PDSCH_SOLVER_PROBE_FILL=25 ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1
ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1
ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1
ISAC_CHEST_DELAY_SHARE=1 ISAC_PDSCH_SOLVER_BACKOFF=200
ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1
ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2
ISAC_PDSCH_CORES=7,8,9,15,16
ISAC_PDCCH_LLR_CACHE=1 ISAC_PDCCH_SHARED_PRESCREEN=1
ISAC_PDCCH_LEN_MEMORY=1 ISAC_PDCCH_NEON_SCORE=1 ISAC_PDCCH_TASK_CAP64=1
```

## Deferred measurements

* [DEFERRED MEASUREMENT] GB10 GPU FEP comparison and timing with
  `ISAC_PDCCH_GPU_FEP=1 ISAC_PDCCH_TIMING=1 ISAC_PDCCH_SCAN_TIMING=1` using
  the same replay command. Confirm `gpu_equal`, `mismatch`, `error` and DCI
  counts before testing speed. The sandbox has no GPU device.
* [DEFERRED MEASUREMENT] OTA p99 and DCI coverage using the exact T26d OTA
  environment above. No radio or hardware command was run in this task.
