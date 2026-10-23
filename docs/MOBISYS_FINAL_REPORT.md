# MobiSys final report: telemetry exporter and ISAC metric keys

Date: 2026-10-20. Branch claude/optimistic-euler-mpdyvh. HEAD 36d64e7. Base 3f0d921.
Host for every result in this file: cloud x86 (4 cores, no GPU), unless a line says DGX. No DGX, sens6, or OTA data was used.

## Evidence labels used here

| Label | Meaning |
|---|---|
| `[CODE-READ, <commit>]` | Read in code or docs at that commit. Not run. |
| `[OFFLINE VERIFIED, cloud x86]` | Built or unit-tested on cloud x86. No radio. |
| `[SIM VERIFIED, cloud x86 rfsim]` | Run on cloud x86 with the rfsim gNB. Upper bound, not OTA. |
| `[SYNTHETIC, cloud x86]` | Synthetic fixture or synthetic lines. Says nothing about the receiver. |
| `[PLANNED]` | Design proposal. Not done unless a row says so. |
| `[UNCHECKED]` | The sources do not say, or the check was not run. |

Sources: `docs/MOBISYS_TELEMETRY_AUDIT.md`, `docs/MOBISYS_METRICS.md`, `docs/MOBISYS_PRIVACY.md`, `docs/MOBISYS_EXPERIMENT_PROTOCOL.md`, `tools/mobisys/` (code, README, tests), `tools/mobisys/examples/rfsim_cloud_x86/` (README, summary JSON), `git log 3f0d921..HEAD`, and `git diff 3f0d921 HEAD -- openair1`.

Limits of this report:

- `PROJECT_MEMORY.md` was not read. CLAUDE.md asks for a full read. Nothing here relies on it.
- The sources do not define "Phase 2" or "Phase 3". This report reads them as follows. Phase-2 metric groups are the ten dataset groups in `MOBISYS_METRICS.md` section 2 (2.1 to 2.10). The Phase-3 classification is the gap list in `MOBISYS_TELEMETRY_AUDIT.md` section 3 (3.1 to 3.24).
- The sens6 frozen gate could not be run. The tag `sens6-frozen-2026-09-30` does not exist in this clone. This report edits no frozen path. It writes one new file under `docs/`.

## 1. Summary and the four evaluation questions

The exporter and the new ISAC keys are in place. The exporter runs offline on the receiver's stdout. The receiver gained only additive JSON keys. No new flag, env var, or config key was added. No decoder-thread or queue file was touched.

What is verified: the C unit test, the Python tests, the build, and a real rfsim run on cloud x86. The rfsim run reached DCI-level ground truth only. The receiver never reached TB decoding on this host. So no CRC, TB, or latency result exists yet.

Answers to the four evaluation questions in `MOBISYS_EXPERIMENT_PROTOCOL.md` section 1:

1. Accuracy. Answerable only at DCI level, on cloud x86 rfsim. DCI precision 1.000 and recall 0.9962 against gNB MAC scheduled grants `[SIM VERIFIED, cloud x86 rfsim]`. TB or CRC accuracy is not answerable. Over the air accuracy is not answerable.
2. Speed and autonomy. Partly answerable. One rfsim run gives cold-start milestone arrival times `[SIM VERIFIED, cloud x86 rfsim]`. Time to first CRC OK is not answerable, because no TB decodes. Failure counts over repeated starts are not measured.
3. Sustainable real-time throughput. Not answered. The longest run was 401 s, with no decoding. Overhead is not measured. The 30 to 60 minute question needs DGX or OTA runs.
4. Commercial generalization within privacy limits. Not answered. No commercial cell was measured. The only commercial output is a 106 s rfsim export, checked for leaks `[SIM VERIFIED, cloud x86 rfsim]`.

Main gaps: TB-level results, DGX overhead, the 4-RX gate on DGX, and OTA ground truth. Section 6 lists the next steps.

## 2. Implemented

| Item | Files | Evidence |
|---|---|---|
| Offline exporter: stdin pipe, lab and commercial modes, `--experiment-mode` (default commercial), bounded queue, parquet or jsonl writer, 1 Hz host sampler | `tools/mobisys/export.py`, `store.py`, `parse.py`, `agg.py`, `hist.py`, `policy.py` (commit 2d87833) | `[CODE-READ, 2d87833]`. `[OFFLINE VERIFIED, cloud x86]` tools/mobisys unittest 32/32 OK. |
| Fail-closed privacy policy: allow-lists, enum coercion, k-suppression at K=10 (commercial), 60 s windows, complementary suppression, no RNTI in commercial | `tools/mobisys/policy.py`, `agg.py`, `test_privacy.py` (2d87833) | `[CODE-READ, 2d87833]`. Commercial export of rfsim run: RNTI 4660, 0x1234 and 1234 absent from every file, checked by parsed values and raw bytes `[SIM VERIFIED, cloud x86 rfsim]`. |
| gNB MAC ground truth matcher (lab only). Reports precision, recall, Wilson CI, F1, false alarms per 1000 slots, offset | `tools/mobisys/groundtruth.py` (2d87833) | `[SIM VERIFIED, cloud x86 rfsim]`. Output is `groundtruth/summary.json` and `matches.csv`. This differs from the planned `ground_truth.parquet` names. |
| Plots, with missing inputs listed, never synthesized | `tools/mobisys/plots.py` (2d87833) | `[SIM VERIFIED, cloud x86 rfsim]`. 3 plots drawn (cpu_rss, discovery_milestones, groundtruth_classes). 9 listed as missing in `plots/MISSING.txt`. |
| Synthetic fixture and examples (provenance "synthetic") | `tools/mobisys/make_synthetic.py`, `examples/` (2d87833) | `[SYNTHETIC, cloud x86]`. Not evidence. |
| Protocol, privacy, metrics, and audit docs | `docs/MOBISYS_*.md` (c11402f, 6033ad3, 6e5f076, 58b260f) | `[CODE-READ]`. Protocol and overhead procedure are not run. |
| ISAC_METRICS additive keys: `t_real_ns`, `proc_utime_us`, `proc_stime_us`, `proc_maxrss_kb`, `proc_minflt`, `proc_majflt`, `cfg_epoch`, `cfg_identity_gen`, `recov_*` (7 counters and 2 late percentiles), `retry_yield_*` (3 counters). Keys are omitted, not 0, when the weak getter is absent. | `openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c`, `nr_passive_metrics.h`, `nr_passive_metrics_json.c`, `tests/nr_passive_metrics_test.cc` (b91ca51) | `[OFFLINE VERIFIED, cloud x86]` test_nr_passive_metrics 14/14. nr-uesoftmodem builds and links. Keys present in real rfsim output `[SIM VERIFIED, cloud x86 rfsim]`. |
| Example export on real rfsim lines, with a README that states the limits | `tools/mobisys/examples/rfsim_cloud_x86/` (36d64e7) | `[SIM VERIFIED, cloud x86 rfsim]`. |

Diff check: `git diff 3f0d921 HEAD -- openair1` touches 4 files, 95 insertions and 8 deletions. None is a decoder, queue, or config file.

Constraint check:

- No new receiver flags, env vars, or config keys `[CODE-READ, b91ca51]`.
- Experiment mode lives in the exporter, not the receiver `[CODE-READ, 2d87833]`.
- Only new ISAC_METRICS keys were added to the receiver `[CODE-READ, b91ca51]`.
- Tail keys have their own 768-byte buffer. A line that does not fit is dropped whole with `LOG_W` `[CODE-READ, b91ca51]`.

## 3. Reused existing infrastructure

- EVMGRANT lines (one per DL decode) are parsed for rx_grants, latency, and CRC `[CODE-READ, 2d87833]`.
- `rnti_seen` and `ul_seen` lines (CLOCK_REALTIME `utc_ns`) feed discovery and first-DCI milestones. `utc_ns` is not used as a milestone time `[CODE-READ, 2d87833]`.
- The existing `ISAC_METRICS` JSON line, emitted every 10 s or 20 s from the summary emitter `[CODE-READ, 3f0d921]`. The 10 s versus 20 s cadence is `[UNCHECKED]`.
- Lifecycle lines (`Starting sync detection`, `ACQ_EVENT pbch_locked`, `CORESET VERIFIED`, `DCI 1_1 length locked`, `ULLAYOUT LOCK`, and others) `[CODE-READ, 2d87833]`.
- gNB MAC `LOG_D` lines (`[DLSCH/PDSCH/PUCCH]`). They need `--log_config.nr_mac_log_level debug`. They are validation only `[CODE-READ, 2d87833]`.
- Campaign run-directory layout and verdicts `[CODE-READ, tests/passive_rx/campaign]`, cited by the protocol. Not re-checked here.
- `tests/passive_rx/rxparse` test_monitor: 31 OK `[OFFLINE VERIFIED, cloud x86]`. The parser was not changed.

## 4. Metric status

### 4.1 Phase-2 metric groups (MOBISYS_METRICS.md section 2)

Status key: implemented, reused, deferred, unsupported. "Exercised" means real rfsim lines reached this dataset `[SIM VERIFIED, cloud x86 rfsim]`.

| Group | Dataset (mode) | Status | One-line reason | Exercised on real rfsim lines? |
|---|---|---|---|---|
| 2.1 | rx_grants (lab) | implemented | One row per DL EVMGRANT. UL writes no row. | No. Zero DL grants, because TB decode never ran. Tested on synthetic data only. |
| 2.2 | dci_attempts (lab) | implemented | One row per DL and UL DCI accept. | Yes. About 100,943 accepts in the 401 s lab run. |
| 2.3 | discovery_events (lab) | implemented | One row per lifecycle line. Arrival time only. | Yes. 2 parts. |
| 2.4 | configuration_events (lab) | implemented | One row per CONFIG_EPOCH line. | No. Directory absent from the shipped example. |
| 2.5 | retry_events (lab) | implemented | RECOVLATE, UL_RETRY, and LAYOUT_PROBE rows. | No. No TB decode, so no retries. |
| 2.6 | slot_health (lab) | implemented | RF health rows (RFCENSUS, RXDISCONT, and related). | Yes. 81 health lines parsed. |
| 2.7 | system_telemetry (lab and commercial) | implemented | 1 Hz host sample plus one row per ISAC line. | Yes, both modes. Commercial rows contain no RNTI. |
| 2.8 | latency_histograms (lab and commercial) | implemented | One row per non-empty bucket per 60 s window. | No. No grants, so no histogram. |
| 2.9 | cell_aggregates (lab and commercial) | implemented | One row per group per window, k-suppressed in commercial. | No. No grants. |
| 2.10 | discovery_summary (lab and commercial) | implemented | One row per window. Uses the OR rule (K1). | Yes. Lab 6 parts, commercial 2 parts. |

Notes on the table:

- "Implemented" means the code exists and passed its tests `[OFFLINE VERIFIED, cloud x86]`. It does not mean the value is correct on real hardware.
- Group 2.9 and the lab GPU values are not exercised. GPU values need a GPU host `[UNCHECKED]`.
- The commercial suppression rule for discovery_summary can publish a count below 10 (K1, section 6).

### 4.2 Phase-3 classification (MOBISYS_TELEMETRY_AUDIT.md section 3)

| Gap | Status | One-line reason |
|---|---|---|
| 3.1 DCI-accept timestamp on PDSCH job | unsupported | No accept time on the job. Only arrival time at the exporter. |
| 3.2 Accept-time clock (rnti_seen utc_ns, REALTIME) | reused, join not done | Parsed. No calibrated join to MONOTONIC stamps in the exporter. |
| 3.3 Per-candidate PDCCH events | unsupported | ISAC has aggregate counts only. |
| 3.4 UHD overflow counter in metrics | unsupported in ISAC; reused in lab | RFTSDISC and RXDISCONT log lines reach lab slot_health. No ISAC counter. |
| 3.5 CPU and GPU utilisation | implemented (exporter); GPU deferred | /proc sampler and nvidia-smi sampler in the exporter. GPU not run on GPU hardware. |
| 3.6 Lifecycle timestamps | partial | Arrival time only, not event time. Use only within one run. |
| 3.7 PHY-emitted gNB ground truth | unsupported; reused as MAC | MAC LOG_D grants used, labelled "scheduled, not PHY-emitted". |
| 3.8 Per-DCI join key with gNB | implemented (lab) | `groundtruth.py` joins on frame, slot, RNTI with a modal offset. |
| 3.9 Wall-clock field in ISAC | implemented | `t_real_ns` (b91ca51). Median offset over the last 64 values. |
| 3.10 Async job ids and times | unsupported | Counters only. recov_* gives counts, not per-job times. |
| 3.11 Windowed histograms | implemented (exporter) | 60 s mergeable histograms in the exporter. Receiver PDTIM stays cumulative. |
| 3.12 Heartbeat while unsynced | unsupported (receiver) | No ISAC line while unsynced. The exporter's 1 Hz sampler is not a receiver heartbeat. |
| 3.13 Process CPU, RSS, faults | implemented | `proc_*` keys (b91ca51). `proc_maxrss_kb` is a peak (K13). |
| 3.14 Queue occupancy and GPU in-flight | partial | `scanq_*` and `pdschq_*` counters are exported. These are cumulative, not occupancy. No GPU in-flight. |
| 3.15 CUDA LDPC timing | unsupported | `ldpc_cb0` timing disabled in code. Not in the policy list. |
| 3.16 Epoch and identity generation | implemented | `cfg_epoch`, `cfg_identity_gen` (b91ca51). Lab `configuration_events`. |
| 3.17 Recovery counters | implemented | `recov_*` keys (b91ca51). Open issues K8, K9, K14. |
| 3.18 Per-grant data in ISAC | unsupported in ISAC; reused in lab | Per-grant data comes from EVMGRANT lines, lab only. |
| 3.19 obs.jsonl | unsupported | Not produced by default. Not used. |
| 3.20 HARQ combining events | unsupported | HARQC is counters only and is not parsed. |
| 3.21 PDSCH schedule batches | unsupported | Batch line not parsed. |
| 3.22 Hyperframe from the air | unsupported | `abs_slot` and `ext_slot` are process-local. |
| 3.23 Timeout counter | unsupported | Timeouts are only in scattered log lines. |
| 3.24 TB payload bytes | unsupported by design | Privacy. `tb=` and `payload=` fields are dropped. |

## 5. Validation results (with labels)

Host for all rows: cloud x86 (no GPU, no DGX).

### 5.1 Build and unit tests

- `[OFFLINE VERIFIED, cloud x86]` test_nr_passive_metrics: 14/14 pass.
- `[OFFLINE VERIFIED, cloud x86]` nr-uesoftmodem builds and links.
- `[OFFLINE VERIFIED, cloud x86]` tools/mobisys unittest: 32/32 OK.
- `[OFFLINE VERIFIED, cloud x86]` rxparse test_monitor: 31 OK.
- `[OFFLINE VERIFIED, cloud x86]` Full ctest: 91% passed, 21 failed of 239.
  - Link errors in common/config `uniqCfg` and `exit_function` for test_nr_sense_api, test_nr_sense_records, test_nr_csi_rb_extent, test_nr_ssb_rate_match_prod, and nr_pusch_ra0sim. These tests were never built here.
  - Other failures: test_nr_pdcch_nid_worker (and _duty), test_nr_srs_blind, nr_td_sim_test, test_nr_core_plan, and nr_cuup_functional_test.
  - None of these links the changed metrics files.
  - The parent commit was not tested, so "pre-existing" is not proven `[UNCHECKED]`.
- The ARM ctest failures listed in CLAUDE.md are DGX-only. They do not apply here.

### 5.2 Real rfsim example (cloud x86)

Bed: rfsim gNB with virtual UE RNTI 0x1234, 106 PRB, 4 RX, numerology 1, band 78. Commit b91ca51 build. Full bed in `tools/mobisys/examples/rfsim_cloud_x86/README.txt`.

- `[SIM VERIFIED, cloud x86 rfsim]` Lab run 401 s, commercial run 106 s.
- `[SIM VERIFIED, cloud x86 rfsim]` Exporter counters, lab full run: 127,249 lines, 101,480 rows written, 0 parse errors, 0 queue drops, 0 policy refusals, 0 k-suppressed. Commercial: same zero counters.
- `[SIM VERIFIED, cloud x86 rfsim]` Receiver never reached TB decoding. DCI 1_1 was found and length-locked. `pdschq_queued` stayed 0 in every run (1 RX and 4 RX). So: 0 rx_grants, 0 retry_events, 0 cell_aggregates, 0 latency_histograms. TB, CRC, and e2e latency figures are N/A.
- `[SIM VERIFIED, cloud x86 rfsim]` Scan queue at end of lab run: `scanq_queued` 303,698, `scanq_drop_full` 215 (about 0.07%). This is a cumulative counter, not a rate over time.

Ground truth, full 401 s lab export, against gNB MAC scheduled grants (labelled "scheduled, not PHY-emitted"):

- `[SIM VERIFIED, cloud x86 rfsim]` gNB DL grants 86,998. True positives 86,667. Missed 331. False positives 0.
- `[SIM VERIFIED, cloud x86 rfsim]` DCI precision 1.000. DCI recall 0.9962 (Wilson CI95 0.9958 to 0.9966). F1 0.998.
- `[SIM VERIFIED, cloud x86 rfsim]` False alarms 0 per 1000 slots. Offset 0 slots, 86,667 votes.
- `[SIM VERIFIED, cloud x86 rfsim]` Classes: dropped_undecidable 86,667 (DCI found, no TB decode). missed_tb 331. correct_tb 0.
- `[SIM VERIFIED, cloud x86 rfsim]` Truncated copy (first 3 parts): 6,290 gNB grants, 5,980 true positives, 310 missed, 0 false positives.

Cold-start milestones (arrival time at exporter, single run, not event time):

- `[SIM VERIFIED, cloud x86 rfsim]` sync_start 22.9 s, sync_ok 23.4 s, pbch_ok 23.4 s, pbch_locked 23.5 s.
- `[SIM VERIFIED, cloud x86 rfsim]` dci_len_locked 67.7 s, coreset_verified 67.8 s, first_dci 67.8 s.
- `[SIM VERIFIED, cloud x86 rfsim]` first_crc_ok: none. `cold_start.success` is false.

Privacy:

- `[SIM VERIFIED, cloud x86 rfsim]` Commercial export contains no RNTI, in any form, in any file. Checked by parsed values and raw bytes.
- `[CODE-READ, 2d87833]` Lab export does contain RNTI (rx_grants rows, and the groundtruth `matches.csv`). Lab output is not for sharing.
- `[CODE-READ, 6e5f076]` The application cannot stop an operator from redirecting stdout to a file. This is an operator control.

### 5.3 Exporter throughput (synthetic)

- `[SYNTHETIC, cloud x86]` About 86,000 lines/s, in-process aggregator.
- `[SYNTHETIC, cloud x86]` About 32,000 lines/s, end to end (includes interpreter start).
- `[SYNTHETIC, cloud x86]` Test floor is 2,000 lines/s (`test_bench.py`). These numbers say nothing about the receiver or DGX overhead.

### 5.4 Not measured

- Receiver overhead before and after the new keys. No measurement exists.
- CRC, TB, and latency results on real signals.
- DGX build, ctest, and 4-RX gate.
- OTA results of any kind.

## 6. Open issues and next steps

Next steps, in order:

1. DGX overhead, before and after, on the same IQ replay. Follow the protocol in `MOBISYS_EXPERIMENT_PROTOCOL.md` section 5. Run arms alternately (A/B/A/B) under `flock -x /tmp/td_measure.lock`, with an idle host. Label as `[MEASURED, DGX replay, <commit A> vs <commit B>]`. Target: under 5% per metric.
2. DGX 4-RX gate. Run `tests/passive_rx/dgx/rfsim_regress.sh` with 106 PRB, `--ue-nb-ant-rx 4`, and 420 s arms. The gate is PROVISIONAL (2026-10-04). Check postconv, ttc, CRC floor, and drop_full. Not run in this work. The gate values are in CLAUDE.md.
3. Investigate why HEAD does not reach PDSCH decode in cloud rfsim. The receiver finds DCI 1_1 but never resolves "DL layout requires TB-CRC evidence", so `pdschq_queued` stays 0 `[SIM VERIFIED, cloud x86 rfsim]`. Without this, no TB or latency result exists on cloud x86. The C code was not touched in this work.
4. OTA lab run with gNB ground truth. Protocol section 3 applies. Needs its own log, date, and commit label.
5. Commercial run on an approved cell, after the authorization steps in `MOBISYS_EXPERIMENT_PROTOCOL.md` section 4.1. Not started.

Known code issues (open, not fixed). Source: `MOBISYS_METRICS.md` section 14.

- K1. `discovery_summary` OR rule can publish a count below 10 in commercial mode (`agg.py:322-329`). Intent is not stated.
- K2. `crc_ok_rate` pools DL and UL. The after-retry rate is not a DL or UL rate.
- K5. Latency histogram buckets are not suppressed one by one. A bucket can show 1 sample.
- K8. `recov_late_ms_p50` can read 1 when the true value is 0 ms (`nr_pdsch_recovery.c:156-160`). p99 is correct. 0 and 0 mean "no samples", which cannot be told apart from a real 0 ms.
- K9. `recov_late_ms` bins saturate at 255 ms.
- K11. `DYN_PREFIXES` accepts any `recov_*` or `retry_yield_*` key without a whitelist.
- K12. `summary.json` run totals are not k-suppressed in commercial mode.
- K13. `proc_maxrss_kb` is the peak, not the current RSS.
- K14. If `getrusage` fails, `proc_*` read 0, not "omitted".
- K15. `t_mono_ns` and `t_real_ns` are read at different points. The offset carries that gap. NTP steps are not handled beyond the median.
- K10. `make_synthetic.py` writes `recov_attempts`, which the C code does not emit. The fixture is not evidence.
- Stale text: `MOBISYS_METRICS.md` section 15 says the C keys are uncommitted and not built. That is out of date. The keys are committed in b91ca51, and the unit test passes `[OFFLINE VERIFIED, cloud x86]`.
- Audit text: the storage audit lists two more frozen paths than CLAUDE.md. CLAUDE.md is used here. The sources do not reconcile the two lists.

Other limits:

- The audit's recommendation to put the new keys in a separate buffer is done. The 768-byte tail was not checked against the 16384-byte limit on a real line with all keys set `[UNCHECKED]`.
- Lab `rx_grants` on a real TB decode is not validated. Its synthetic tests are not evidence.
- The GPU sampler path, the dashboard switch, and the monitor start-up path are `[UNCHECKED]`.
- Multi-cell is not ready. The receiver and exporter hold one PCI (`MOBISYS_METRICS.md` section 12).
- Throughput (bits per second) is not computed. The metrics doc gives a definition only (section 9).
