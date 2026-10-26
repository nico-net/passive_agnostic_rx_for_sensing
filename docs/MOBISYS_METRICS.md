# MobiSys metrics dictionary (tools/mobisys)

Date: 2026-10-20. Branch: claude/optimistic-euler-mpdyvh.

Evidence label for every claim in this file: `[CODE-READ, branch claude/optimistic-euler-mpdyvh]`.
Nothing here is measured. Nothing was built, run, or tested. No DGX, sens6, or OTA data was used.
The C part is an uncommitted working-tree diff. It may still change (section 15).

Column names, modes, enums, and privacy classes come from `tools/mobisys/policy.py`. A column that is not in that file is not listed here.

## 0. Sources and limits

Sources (the only evidence):

- `tools/mobisys/policy.py`: datasets, columns, modes, enums, privacy classes, K, window.
- `tools/mobisys/parse.py`: line parsers and the format strings they match.
- `tools/mobisys/agg.py`: formulas, windows, suppression, run summary.
- `tools/mobisys/hist.py`: histogram.
- `tools/mobisys/groundtruth.py`: gNB matching and scores.
- `tools/mobisys/export.py` and `store.py`: pipeline, 1 Hz sampler, file writer.
- `docs/MOBISYS_TELEMETRY_AUDIT.md`: existing metrics (read in full).
- Working-tree diff of `openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c`, `nr_passive_metrics.h`, `nr_passive_metrics_json.c`. The getters were checked in `nr_pdsch_recovery.c`, `nr_pdsch_passive_decode.c`, and `nr_passive_cfg_epoch.c`.

Limits:

- `PROJECT_MEMORY.md` (4696 lines) was not read in full. This file read its headings, sections 0.1, 11.11, 11.13, 26, 27, and 28, and a grep for telemetry terms. CLAUDE.md asks for a full read. That step is still open.
- `tools/mobisys/make_synthetic.py` is a fixture. It is not evidence (see K10).
- `tools/mobisys/plots.py` and the tests are not used as evidence.
- Where this file corrects the audit, the code wins (see K16).

## 1. Conventions

### 1.1 Clock domains

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

| Tag | Meaning |
|---|---|
| R-MONO | Receiver CLOCK_MONOTONIC. Used by EVMGRANT stamps (e2e fields, dec_us), PDTIM, ISAC `t_mono_ns`. |
| R-REAL | Receiver CLOCK_REALTIME. Used by `rnti_seen` / `ul_seen` `utc_ns` and ISAC `t_real_ns`. |
| R-SFN | Receiver frame and slot counter (0..1023 frames). Not time. Hyperframe is process-local (audit section 4.10). |
| E-ARR | Exporter arrival time. Default: `time.monotonic()` minus the exporter start time. With `--line-time unix`: the harness stamp minus the first stamped line. |
| E-WALL | Exporter CLOCK_REALTIME arrival time, floored to a 60 s boundary. Used only for `window_start_s`. |
| E-SAMP | Exporter sampler, monotonic, 1 Hz. |
| GNB | gNB MAC log or CSV. No clock. Frame and slot only. Validation only. |

Rounding of `t_s`: 6 decimals in lab event rows, 3 decimals in milestones and sampler rows.

### 1.2 Granularity terms

- per grant: one EVMGRANT line (one decode call). Probes are included and flagged.
- per event: one matching log line.
- 1 s: exporter sampler.
- per ISAC line: one `SENSING: ISAC_METRICS` line. The audit gives 20 s; with the GPU budget on, the audit's code reading gives 10 s (audit 4.11). Not measured.
- 60 s window: exporter window (closes lazily, see 1.4).

### 1.3 Null and sentinel rules

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- Missing values are None. They are parquet null or JSON null. They are never 0.
- E2E fields that the C macro sets to -1 become None (parse.py `_us`).
- ISAC `pci` is -1 before PBCH lock. ISAC `abs_slot` is -1 when unknown. The exporter writes these -1 values as they are. Consumers must treat -1 as unknown.
- `gpu_*` and `rx_*` columns are None when the source is missing.

### 1.4 Windows

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- Windows are 60 s long. The window index is `floor(arrival unix time / 60)`. `window_start_s` is that index times 60, a unix epoch second.
- A window closes only when a line from a later window arrives, or at exit (`agg.py:99-104`, `close`). A quiet receiver delays its window rows. The 1 Hz sampler does not close windows.
- A line with an earlier time than the open window is added to the open window. The window does not reopen.

### 1.5 Privacy classes and modes

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- `ident` (policy class): identifies one UE by RNTI. Lab only. Commercial mode refuses it.
- `agg` (policy class): aggregate, timing, or cell or receiver metadata.
- Lab datasets can hold `agg` columns in the same row as an `ident` column. The `agg` column is then still lab only, and it is linked to the UE through that row.
- Commercial mode accepts only the four datasets marked "lab, commercial" below. It accepts only `agg` columns in them.
- `system_telemetry` is not k-suppressed. Its rows are cell-level counters with no UE grouping.

## 2. Datasets

Modes: `lab` = lab export only. `lab, commercial` = both.

Dataset list and modes (policy.py `DATASETS`):

| Dataset | Modes | Columns | Row meaning |
|---|---|---|---|
| rx_grants | lab | 43 | one EVMGRANT line (DL, probes included and flagged) |
| dci_attempts | lab | 11 | one DCI accept line (DL and UL) |
| discovery_events | lab | 9 | one lifecycle line |
| configuration_events | lab | 9 | one CONFIG_EPOCH line |
| retry_events | lab | 19 | one RECOVLATE, UL_RETRY, or LAYOUT_PROBE line |
| slot_health | lab | 14 | one RF health line |
| system_telemetry | lab, commercial | 55 plus 2 prefixes | one host sample (1 Hz) or one ISAC line |
| latency_histograms | lab, commercial | 7 | one non-empty bucket of one window |
| cell_aggregates | lab, commercial | 13 | one group of one window |
| discovery_summary | lab, commercial | 7 | one window |

### 2.1 rx_grants (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per EVMGRANT line. DL only. UL grants write no row here (`agg._on_ul_crc` writes nothing).

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter. One counter for all datasets. | count | per row | none | agg |
| t_s | E-ARR of the EVMGRANT line | s | per grant | E-ARR | agg |
| ext_slot | SfnUnwrap(frame, slot) with slots_per_frame (default 20). First value equals the first frame seen. | slot | per grant | R-SFN, process-local | agg |
| frame | EVMGRANT `rnti=... <frame>.<slot>` | frame | per grant | R-SFN | agg |
| slot | EVMGRANT `<frame>.<slot>` | slot | per grant | R-SFN | agg |
| rnti | EVMGRANT `rnti=0x...`, as int | RNTI | per grant | none | ident |
| dir | Always "dl" | enum | per grant | none | agg |
| outcome | `crc=1` gives crc_ok. Any other value gives crc_fail. | enum | per grant | none | agg |
| probe | EVMGRANT `probe=`, as bool. Probe rows are excluded from counters, windows, and gNB matching. | bool | per decode | none | agg |
| mcs | EVMGRANT `mcs=` | MCS index | per grant | none | agg |
| tbl | EVMGRANT `tbl=` | table id | per grant | none | agg |
| qm | EVMGRANT `Qm=` | bits per symbol | per grant | none | agg |
| nl | EVMGRANT `Nl=` | layers | per grant | none | agg |
| rb_start | first value of EVMGRANT `rb=a+b` | PRB index | per grant | none | agg |
| rb_n | second value of EVMGRANT `rb=a+b` | PRB count | per grant | none | agg |
| nsym | EVMGRANT `nsym=` | OFDM symbols | per grant | none | agg |
| snr_db | EVMGRANT `snr=` (float) | dB by policy name. The unit is not stated in the C line. | per grant | none | agg |
| evm_pct | EVMGRANT `evm=` (float) | percent by policy name. The unit is not stated in the C line. | per grant | none | agg |
| rv | EVMGRANT `rv=` | RV index | per grant | none | agg |
| ndi | EVMGRANT `ndi=` | flag | per grant | none | agg |
| harq_pid | EVMGRANT `pid=` | HARQ process id | per grant | none | agg |
| tbs | EVMGRANT `tbs=` | as printed. The unit is not stated in parse.py. | per grant | none | agg |
| nbrx | EVMGRANT `nbrx=` | RX antennas | per grant | none | agg |
| cb_ok | EVMGRANT `cb_ok=`: code blocks that passed their CRC (empty in logs older than 2026-10-26, and 0 when no LDPC ran or on a probe) | count | per grant | none | agg |
| cb_tried | EVMGRANT `cb_tried=`: code blocks the LDPC decoder was started on. Decoding stops after 5 failed blocks (NR_CB_FAIL_LIMIT), so a failing TB is biased toward its early blocks. CB CRC = sum(cb_ok) / sum(cb_tried) | count | per grant | none | agg |
| cb_total | EVMGRANT `cb_total=`: code blocks in the TB (C) | count | per grant | none | agg |
| early | True when EVMGRANT `es=` is present (early-start grant) | bool | per grant | none | agg |
| bodies | e2e `bodies` (decode bodies in the call) | count | per grant | none | agg |
| lag_slots | e2e `lag` (producer slots ahead at completion) | slot | per grant | R-SFN difference | agg |
| span_inconsistent | True when serial_us > e2e_tot_us + 4 (SPAN_TOL_US) | bool | per grant | derived | agg |
| legacy_dec_us | EVMGRANT `dec_us=` (float), stored as int (truncated) | us | per grant | R-MONO (dec_t0 to dec_t1) | agg |
| e2e_tot_us | e2e `tot` = E2E_US(arr, dec_t1) | us | per grant | R-MONO | agg |
| arr2enq_us | e2e `arr2enq` = E2E_US(arr, enqueue) | us | per grant | R-MONO | agg |
| qwait_us | e2e `qwait` = E2E_US(enqueue, taken) | us | per grant | R-MONO | agg |
| pre_us | e2e `pre` = E2E_US(taken, dec_t0) | us | per grant | R-MONO | agg |
| dec_span_us | e2e `dec` = E2E_US(dec_t0, dec_t1) | us | per grant | R-MONO | agg |
| serial_us | arr2enq_us + qwait_us + pre_us + dec_span_us. None if any part is None. | us | per grant | derived | agg |
| first_body_us | e2e `first_body` (first decode body wall) | us | per grant | R-MONO | agg |
| fep_us | EVMGRANT `stg=fep:w/c/g`, wall part (w). First body only. | us | per grant | R-MONO | agg |
| chest_us | EVMGRANT `stg=chest:w/c/g`, wall part. First body only. | us | per grant | R-MONO | agg |
| demod_us | EVMGRANT `stg=demod:w/c/g`, wall part. First body only. | us | per grant | R-MONO | agg |
| ldpc_us | EVMGRANT `stg=ldpc:w/c/g`, wall part. First body only. | us | per grant | R-MONO | agg |
| es_lead_us | EVMGRANT `es=sym,lead`, lead part (None if -1) | us | per grant | R-MONO | agg |

Note: `dec_span_us` and `legacy_dec_us` cover the same interval. They differ by at most 1 us (rounding against truncation).

### 2.2 dci_attempts (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per DCI accept line. Both DL (`rnti_seen`) and UL (`ul_seen`). Duplicate accepts give duplicate rows.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter | count | per row | none | agg |
| t_s | E-ARR of the accept line | s | per event | E-ARR | agg |
| ext_slot | SfnUnwrap(sfn, slot) of the accept line | slot | per event | R-SFN | agg |
| frame | `sfn=` | frame | per event | R-SFN | agg |
| slot | `slot=` | slot | per event | R-SFN | agg |
| rnti | `rnti=0x...`, as int | RNTI | per event | none | ident |
| dir | "dl" when fmt is 1_0 or 1_1. Otherwise "ul". | enum | per event | none | agg |
| fmt | `fmt=`. The C prints 1_0, 1_1, or 0_x for `rnti_seen` and 0_1 for `ul_seen`. Unknown strings become "0_1" (last enum value). | enum | per event | none | agg |
| cce | `cce=` | CCE index | per event | none | agg |
| al | `al=` | aggregation level | per event | none | agg |
| utc_ns | `utc_ns=`. Read at accept. The log line is deferred. | ns | per event | R-REAL | agg |

Note: `utc_ns` is the only per-DCI wall-clock value. It is R-REAL. Do not subtract it from R-MONO values.

### 2.3 discovery_events (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per lifecycle line. The event names are in section 8.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter | count | per row | none | agg |
| t_s | E-ARR of the line | s | per event | E-ARR | agg |
| event | Name from parse.py (enum `disc_event`). `acq_state` for ACQ_STATE lines. | enum | per event | none | agg |
| pci | Last PCI known to the exporter. Set by `sync_ok` and by ISAC `pci` when it is 0 or more. | PCI | per event | none | agg |
| rnti | Set for first_dci, first_crc_ok, dci_len_locked, td_converged. Otherwise None. | RNTI | per event | none | ident |
| ival | dci_len_locked: `len`. td_converged: `tda`. Otherwise None. | bits (len) or tda index | per event | none | agg |
| ival2 | td_converged: `S`. The `L` value is not stored. | index | per event | none | agg |
| state_from | ACQ_STATE: previous state (enum `acq_state`) | enum | per event | none | agg |
| state_to | ACQ_STATE: new state (enum `acq_state`) | enum | per event | none | agg |

### 2.4 configuration_events (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter | count | per row | none | agg |
| t_s | E-ARR of the line. CONFIG_EPOCH lines carry no time. | s | per event | E-ARR | agg |
| kind | "epoch_bump" for `CONFIG_EPOCH a -> b`. "continuity_gap" for `CONFIG_EPOCH continuity`. | enum | per event | none | agg |
| epoch_from | `CONFIG_EPOCH <a>` (epoch_bump rows only) | epoch | per event | none | agg |
| epoch_to | `-> <b>` (epoch_bump rows only) | epoch | per event | none | agg |
| cls | `class=` (SOFT, HARD_REVERIFY, HARD_RESET, UNKNOWN). Unknown strings become UNKNOWN. | enum | per event | none | agg |
| cause | `cause=` (10 values in policy). Unknown strings become UNKNOWN. | enum | per event | none | agg |
| gap_samples | `CONFIG_EPOCH continuity gap_samples=` (continuity_gap rows only) | samples | per event | receiver duration | agg |
| gap_ms | `gap_ms=` (continuity_gap rows only) | ms | per event | receiver duration | agg |

### 2.5 retry_events (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

`kind` is one of recovlate (RECOVLATE line), ul_retry (UL_RETRY line), layout_probe (LAYOUT_PROBE line). Fields not set for a kind are None.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter | count | per row | none | agg |
| t_s | E-ARR of the line | s | per event | E-ARR | agg |
| kind | recovlate, ul_retry, or layout_probe | enum | per event | none | agg |
| dir | recovlate: "dl". ul_retry: "ul". layout_probe: None. | enum | per event | none | agg |
| rnti | recovlate and ul_retry: `rnti=0x...` | RNTI | per event | none | ident |
| frame | recovlate: `<frame>.<slot>` | frame | per event | R-SFN | agg |
| slot | recovlate: `<frame>.<slot>` | slot | per event | R-SFN | agg |
| ext_slot | recovlate only: SfnUnwrap(frame, slot) | slot | per event | R-SFN | agg |
| harq_pid | recovlate: `pid=` | HARQ id | per event | none | agg |
| tbs | recovlate: `tbs=` | as printed | per event | none | agg |
| recovered_cfg | recovlate: `recovered_cfg=` as bool | bool | per event | none | agg |
| tot_us | recovlate and ul_retry: `tot_us=`. -1 gives None. | us | per event | not stated by the audit (UNCHECKED) | agg |
| attributed | recovlate: True only when it matches a failed DL grant. Otherwise False. | bool | per event | none | agg |
| orig_seq | recovlate and attributed: seq of the matched rx_grants row | count | per event | none | agg |
| cdm | ul_retry: `cdm_groups=` | count | per event | none | agg |
| s | ul_retry: `S=` | index | per event | none | agg |
| l | ul_retry: `L=` | index | per event | none | agg |
| n | layout_probe: `n=` | count | per event | none | agg |
| cb0_ok | layout_probe: `cb0_ok=` | count | per event | none | agg |

Note: `orig_seq` joins to `rx_grants.seq`. The seq counter is shared by all datasets.

### 2.6 slot_health (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

`kind` is one of rxdiscont, rftsdisc, rfcensus, usrp_ring_recover, usrp_ring_restart, rfstall, rx_recover.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| seq | Exporter row counter | count | per row | none | agg |
| t_s | E-ARR of the line | s | per event | E-ARR | agg |
| kind | Health kind (enum `health_kind`) | enum | per event | none | agg |
| abs_slot | rxdiscont `abs_slot=` (process-local) | slot | per event | process-local | agg |
| frame | rxdiscont `frame=`; rfcensus `frame=` | frame | per event | R-SFN | agg |
| slot | rxdiscont `slot=` | slot | per event | R-SFN | agg |
| delta | rxdiscont `delta=`; rftsdisc `delta=` | samples | per event | receiver duration | agg |
| n | rxdiscont `n=`; rftsdisc `n=` | count | per event | none | agg |
| pbch_ok | rfcensus `pbch_ok=` | count per census | per 1 s census | none | agg |
| pbch_fail | rfcensus `pbch_fail=` | count per census | per 1 s census | none | agg |
| slots | rfcensus `slots=` | slot count | per 1 s census | none | agg |
| attempt | usrp_ring_recover `attempt=`; usrp_ring_restart first number | count | per event | none | agg |
| gap_samples | rx_recover `gap_samples=` | samples | per event | receiver duration | agg |
| resync_ms | rx_recover `resync_ms=` | ms | per event | audit says R-MONO (not re-checked) | agg |

rfstall has no fields. Its row has only seq, t_s, and kind.

### 2.7 system_telemetry (lab and commercial)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

Two sources write rows.

- `src = "host"`: the exporter sampler, once per second. It sets t_s, src, host_cpu_pct, mem_avail_kb, rx_cpu_pct, rx_rss_kb, rx_threads, gpu_util_pct, gpu_mem_mb, gpu_temp_c. All other columns are None.
- `src = "metrics"`: one row per ISAC_METRICS line. It sets t_s, src, acq_state, and each ISAC column that is in the policy list.

Sampler columns:

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| t_s | E-ARR at the sample | s | 1 s | E-SAMP | agg |
| src | "host" | enum | 1 s | none | agg |
| host_cpu_pct | 100 times (1 minus delta idle over delta total) from the first line of /proc/stat. Idle includes iowait. First sample is None. | percent, all CPUs | 1 s | sampler delta | agg |
| mem_avail_kb | /proc/meminfo `MemAvailable` | kB | 1 s | none | agg |
| rx_cpu_pct | 100 times delta (utime plus stime) over delta monotonic time for the nr-uesoftmodem PID (found by /proc comm, re-searched every 10 s). First sample is None. | percent of one core | 1 s | E-SAMP | agg |
| rx_rss_kb | Current RSS: /proc/PID/stat field 24 times page size, in kB | kB | 1 s | none | agg |
| rx_threads | /proc/PID/stat field 20 (num_threads) | count | 1 s | none | agg |
| gpu_util_pct | nvidia-smi `utilization.gpu`, first GPU only. Queried at most every 5 s and held between queries. | percent | 5 s | nvidia-smi | agg |
| gpu_mem_mb | nvidia-smi `memory.used`, first GPU only | MiB | 5 s | nvidia-smi | agg |
| gpu_temp_c | nvidia-smi `temperature.gpu`, first GPU only | degrees C | 5 s | nvidia-smi | agg |

If any GPU field does not parse, all three GPU values are None for that query (export.py:134-137).

ISAC columns (`src = "metrics"`). Each value is passed through from the JSON key. Counters are cumulative since process start. Clock is none unless the row says otherwise.

| Column | ISAC key and meaning | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| acq_state | Exporter's last acquisition state (enum `acq_state`) | enum | per ISAC line | none | agg |
| abs_slot | ISAC `abs_slot` | RF slot, -1 unknown | per ISAC line | process-local | agg |
| pci | ISAC `pci`. -1 before PBCH lock. | PCI | per ISAC line | none | agg |
| t_mono_ns | ISAC `t_mono_ns`, read at the top of collect | ns | per ISAC line | R-MONO | agg |
| t_real_ns | ISAC `t_real_ns` (new, section 15) | ns | per ISAC line | R-REAL | agg |
| cfg_epoch | ISAC `cfg_epoch` (new, section 15) | epoch counter | per ISAC line | none | agg |
| cfg_identity_gen | ISAC `cfg_identity_gen` (new, section 15) | counter | per ISAC line | none | agg |
| proc_utime_us | ISAC `proc_utime_us` (new). getrusage user time, all threads. | us, cumulative | per ISAC line | receiver CPU time | agg |
| proc_stime_us | ISAC `proc_stime_us` (new). getrusage system time, all threads. | us, cumulative | per ISAC line | receiver CPU time | agg |
| proc_maxrss_kb | ISAC `proc_maxrss_kb` (new). Peak RSS. | kB, peak | per ISAC line | none | agg |
| proc_minflt | ISAC `proc_minflt` (new) | count, cumulative | per ISAC line | none | agg |
| proc_majflt | ISAC `proc_majflt` (new) | count, cumulative | per ISAC line | none | agg |
| acq_transitions | ISAC `acq_transitions` | count | per ISAC line | none | agg |
| acq_sync_losses | ISAC `acq_sync_losses` | count | per ISAC line | none | agg |
| acq_pbch_locks | ISAC `acq_pbch_locks` | count | per ISAC line | none | agg |
| acq_sib1_decodes | ISAC `acq_sib1_decodes` | count | per ISAC line | none | agg |
| pdcch_occasions | ISAC `pdcch_occasions` | count | per ISAC line | none | agg |
| pdcch_candidates | ISAC `pdcch_candidates` (aggregate only; no per-candidate events) | count | per ISAC line | none | agg |
| pdcch_accepts | ISAC `pdcch_accepts` | count | per ISAC line | none | agg |
| pdcch_accepts_c | ISAC `pdcch_accepts_c` | count | per ISAC line | none | agg |
| scanq_queued | ISAC `scanq_queued` | count | per ISAC line | none | agg |
| scanq_processed | ISAC `scanq_processed` | count | per ISAC line | none | agg |
| scanq_drop_full | ISAC `scanq_drop_full` | count | per ISAC line | none | agg |
| scanq_drop_stale | ISAC `scanq_drop_stale` | count | per ISAC line | none | agg |
| scanq_drop_epoch | ISAC `scanq_drop_epoch` | count | per ISAC line | none | agg |
| scanq_max_lag | ISAC `scanq_max_lag`. Maximum over the run; never decays. | slots | per ISAC line | none | agg |
| pdschq_queued | ISAC `pdschq_queued` | count | per ISAC line | none | agg |
| pdschq_decoded | ISAC `pdschq_decoded`. Includes layout probes. | count | per ISAC line | none | agg |
| pdschq_crc_ok | ISAC `pdschq_crc_ok`. Excludes layout probes. | count | per ISAC line | none | agg |
| pdschq_drop_full | ISAC `pdschq_drop_full` | count | per ISAC line | none | agg |
| pdschq_drop_stale | ISAC `pdschq_drop_stale` | count | per ISAC line | none | agg |
| pdschq_drop_epoch | ISAC `pdschq_drop_epoch` | count | per ISAC line | none | agg |
| pdschq_stale_after_decode | ISAC `pdschq_stale_after_decode` (IQ expired after decode) | count | per ISAC line | none | agg |
| pdschq_max_lag | ISAC `pdschq_max_lag` | slots | per ISAC line | none | agg |
| puschq_drop_epoch | ISAC `puschq_drop_epoch` | count | per ISAC line | none | agg |
| pusch_try | ISAC `pusch_try` | count | per ISAC line | none | agg |
| pusch_crc_ok | ISAC `pusch_crc_ok` | count | per ISAC line | none | agg |
| ldpc_ok | ISAC `ldpc_ok` (code-block outcomes) | count | per ISAC line | none | agg |
| ldpc_seg_fail | ISAC `ldpc_seg_fail` | count | per ISAC line | none | agg |
| ldpc_tb_fail | ISAC `ldpc_tb_fail` | count | per ISAC line | none | agg |
| ldpc_zero_tb | ISAC `ldpc_zero_tb` | count | per ISAC line | none | agg |
| discovery_yields | ISAC `discovery_yields` (GPU budget) | count | per ISAC line | none | agg |
| discovery_overruns | ISAC `discovery_overruns` (GPU budget) | count | per ISAC line | none | agg |
| rt_gpu_fallbacks | ISAC `rt_gpu_fallbacks` (GPU budget) | count | per ISAC line | none | agg |
| gpu_budget_used_pct | ISAC `gpu_budget_used_pct`. Logical admission budget, not hardware use. Zero unless the budget is on. | percent | per ISAC line | none | agg |

Prefix columns (accepted by prefix, no whitelist; policy.py:52, 111-112). Each is a float column in the file.

| Column | Source | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| recov_submitted | RECOV text, `submitted=` (new, section 15) | count, cumulative | per ISAC line | none | agg |
| recov_rescued | RECOV `rescued=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_stale | RECOV `stale=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_expired | RECOV `expired=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_dropped_full | RECOV `dropped_full=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_dropped_old | RECOV `dropped_old=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_yielded | RECOV `yielded=` (new) | count, cumulative | per ISAC line | none | agg |
| recov_late_ms_p50 | RECOV `late_ms_p50=` (new). Integer bins. See K8. | ms | per ISAC line | none | agg |
| recov_late_ms_p99 | RECOV `late_ms_p99=` (new). Saturates at 255 ms. | ms | per ISAC line | none | agg |
| retry_yield_chains | retry-yield getter `chains` (new) | count, cumulative | per ISAC line | none | agg |
| retry_yield_yields | retry-yield getter `yields` (new) | count, cumulative | per ISAC line | none | agg |
| retry_yield_gated | retry-yield getter `gated` (new) | count, cumulative | per ISAC line | none | agg |

Other ISAC keys are dropped silently. `agg._on_metrics` copies only the keys in the policy list and the two prefixes. Section 13 lists them.

### 2.8 latency_histograms (lab and commercial)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per non-empty bucket of one window, direction, and metric.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| window_start_s | Window index times 60 | unix s | 60 s window | E-WALL | agg |
| pci | Exporter `pci` at window close (section 12) | PCI | 60 s window | none | agg |
| dir | "dl" only. No UL latency rows. | enum | 60 s window | none | agg |
| metric | Enum `lat_metric`. Produced: e2e_tot, legacy_dec_us, arr2enq, qwait, pre, dec_span, serial. `legacy_pdtim_real` is in the enum but is never written here. | enum | 60 s window | none | agg |
| bucket_lo_us | Bucket lower bound, inclusive (hist.py `bounds`) | us | 60 s window | none | agg |
| bucket_hi_us | Bucket upper bound, exclusive | us | 60 s window | none | agg |
| count | Samples in the bucket | count | 60 s window | none | agg |

Commercial: a (dir, metric) window is dropped when its total count is below 10. Each bucket count is published as it is (K5).

### 2.9 cell_aggregates (lab and commercial)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per window and group (dir, mcs_bucket, nl_bucket). DL groups use non-probe grants. UL groups use UL CRC lines.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| window_start_s | Window index times 60 | unix s | 60 s window | E-WALL | agg |
| pci | Exporter `pci` at window close | PCI | 60 s window | none | agg |
| dir | "dl" or "ul" | enum | 60 s window | none | agg |
| mcs_bucket | MCS index to "0-4", "5-9", "10-16", "17-27", or "na" (DL `mcs=`, UL `mcs=`) | enum | 60 s window | none | agg |
| nl_bucket | DL `Nl` to "1", "2", "3-4", or "na". UL is always "na" (no layers). | enum | 60 s window | none | agg |
| n_grants | Count of DL non-probe grants plus UL CRC lines in the group | count | 60 s window | none | agg |
| n_crc_ok | Count with crc=1 (DL) or ok=1 (UL) | count | 60 s window | none | agg |
| n_crc_fail | n_grants minus n_crc_ok | count | 60 s window | none | agg |
| snr_db_mean | Mean of snr over grants in the group that have a value (DL `snr=`, UL `snr_db=`). None if no value. | dB by policy name | 60 s window | none | agg |
| e2e_tot_p50_us | Histogram quantile 0.5 of e2e_tot_us. DL only. | us | 60 s window | R-MONO | agg |
| e2e_tot_p99_us | Histogram quantile 0.99 of e2e_tot_us. DL only. | us | 60 s window | R-MONO | agg |
| legacy_dec_p50_us | Histogram quantile 0.5 of legacy_dec_us. DL only. | us | 60 s window | R-MONO | agg |
| legacy_dec_p99_us | Histogram quantile 0.99 of legacy_dec_us. DL only. | us | 60 s window | R-MONO | agg |

Commercial: a group with n_grants below 10 is dropped. If exactly one group in a direction is small, the smallest shown group in that direction is also dropped (complementary suppression, agg.py:295-302).

### 2.10 discovery_summary (lab and commercial)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

One row per window with at least one accept or one grant.

| Column | Source or formula | Unit | Granularity | Clock | Privacy |
|---|---|---|---|---|---|
| window_start_s | Window index times 60 | unix s | 60 s window | E-WALL | agg |
| pci | Exporter `pci` at window close | PCI | 60 s window | none | agg |
| acq_state | Exporter's last acquisition state at window close. "UNKNOWN" if never set. | enum | 60 s window | none | agg |
| ue_bucket | Distinct RNTIs in the window (DCI accepts, DL and UL, plus non-probe grants) to "0", "1-9", "10-49", or "50+". The set is kept in memory (cap 65536) and discarded at close. | enum | 60 s window | none | agg |
| n_dci_accepts | DCI accept lines in the window, DL and UL | count | 60 s window | none | agg |
| n_grants | DL non-probe grants plus UL CRC lines | count | 60 s window | none | agg |
| n_crc_ok | Of n_grants, the ones with CRC ok | count | 60 s window | none | agg |

Commercial emit rule: the row is written when n_dci_accepts is at least 10 OR n_grants is at least 10. Both counts are then written, even if one is below 10 (K1).

## 3. Mode summary

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

| Dataset | Lab | Commercial | Suppression |
|---|---|---|---|
| rx_grants | yes | no | none (lab K = 1) |
| dci_attempts | yes | no | none |
| discovery_events | yes | no | none |
| configuration_events | yes | no | none |
| retry_events | yes | no | none |
| slot_health | yes | no | none |
| system_telemetry | yes | yes | none. Cell-level rows, not grouped by UE. |
| latency_histograms | yes | yes | window total below 10 dropped; buckets not suppressed |
| cell_aggregates | yes | yes | group below 10 dropped; complementary rule |
| discovery_summary | yes | yes | OR rule (K1) |

In commercial mode the exporter reads only from a pipe. A regular file is refused (export.py:204-215). `--input` is refused. Commercial mode has no RNTI, per-grant, per-UE, payload, or raw-log output.

Exit codes (export.py docstring): 0 ok, 2 input or usage refused, 3 policy violation (rows refused), 4 internal error.

## 4. Run files (not policy datasets)

These files are not policy datasets. Their keys are not columns. Nothing in policy.py checks them. Commercial mode writes them.

### 4.1 summary.json

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

| Key | Content | Notes |
|---|---|---|
| schema | "mobisys-telemetry/1" | |
| mode | lab or commercial | |
| provenance | live, replay, or synthetic | Set by the operator with `--provenance`. Not checked by code. |
| counts | Counters from agg (section 11) | Whole-run totals. Not k-suppressed in commercial mode (K12). |
| grants | counts.grants | DL non-probe grants plus UL CRC lines (pooled) |
| crc_ok, crc_fail | counts | Pooled DL and UL |
| rescued_unique | Distinct DL keys matched by a RECOVLATE | DL only (section 10) |
| crc_ok_after_retry | crc_ok plus rescued_unique | Mixed directions (K2) |
| crc_ok_rate | crc_ok over grants. None when grants is below K. | Pooled DL and UL (K2) |
| crc_ok_rate_after_retry | crc_ok_after_retry over grants | Pooled DL and UL (K2) |
| latency_us | Per metric (e2e_tot, legacy_dec_us, arr2enq, qwait, pre, dec_span, serial): n, p50, p90, p99 | Run-level histogram. Section 6. |
| latency_defs | Text definitions for e2e_tot, legacy_dec_us, legacy_pdtim_real, serial | Copied from agg.py |
| legacy_pdtim_real | Last printed PDTIMCLS `real` values: n, p50, p90, p99. None if none printed. | Cumulative since receiver start (section 5.4) |
| cold_start | attempts, observed_from_start, success, time_to_first_crc_ok_s, milestones_s, note | Section 8 |
| clock_offset_real_minus_mono_ns | Median of the last 64 (t_real_ns minus t_mono_ns) values | Section 15 keys |
| last_metrics | Run-end values of pdschq_queued, pdschq_decoded, pdschq_crc_ok, scanq_queued, scanq_drop_full, scanq_drop_stale, pdschq_drop_full, pdschq_drop_stale | Cumulative |
| telemetry_loss | Section 11 | |

### 4.2 manifest.json

| Key | Content |
|---|---|
| schema, mode, provenance | As above |
| host_label | Operator string from `--host-label`. Default "unlabelled". Not from the receiver. |
| format | parquet or jsonl |
| pyarrow | pyarrow version, or None |
| python | Python version |
| k | 1 in lab, 10 in commercial |
| window_s | 60 |
| slots_per_frame | Default 20 |
| rotate_rows | Default 50000 |
| started_unix | Exporter wall-clock start. Unix seconds. |
| duration_s | Exporter monotonic duration |
| datasets | Part count per dataset |
| rows_written | Total rows written |
| telemetry_loss | Section 11 |
| clock_offset_real_minus_mono_ns | As in summary.json |
| input | "file" or "stdin" |

### 4.3 groundtruth outputs (lab only)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

`summary.json` keys are in section 7. `matches.csv` has the columns frame, slot, rnti, class. It contains an RNTI, so it is lab only. Both files are written into the lab export directory.

## 5. Latency definitions

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

### 5.1 How the spans fit together

For one DL decode call, the C code prints these spans (nr_pdsch_passive_queue.c:1631-1634):

- Interval A: slot arrival to decode end. This is e2e_tot_us.
- A splits into four contiguous parts: arr2enq_us, qwait_us, pre_us, dec_span_us. Their sum is serial_us.
- Inside dec_span_us: first_body_us (first decode body). Inside first_body_us: fep_us, chest_us, demod_us, ldpc_us (stage walls of that first body only).
- legacy_dec_us is the same interval as dec_span_us.
- legacy_pdtim_real (summary only) is a cumulative PDTIMCLS summary of first bodies over all calls. It is not per grant.

Rule: do not add these values. serial_us and e2e_tot_us describe the same interval. first_body_us and the stage walls are inside dec_span_us. The e2e and dec values come from the same grants.

### 5.2 Names

| Name | Where | Interval | Population | Nested or overlapping | Sum with others? |
|---|---|---|---|---|---|
| e2e_tot (e2e_tot_us) | rx_grants, latency_histograms, cell_aggregates, summary | slot arrival to dec_t1 | DL non-probe grants | Contains arr2enq, qwait, pre, dec | No |
| legacy_dec_us | rx_grants, latency_histograms, cell_aggregates, summary | dec_t0 to dec_t1 (dec_us). Includes retry bodies. | DL non-probe grants | Same as dec_span | No |
| arr2enq | rx_grants, latency_histograms, summary | arrival to enqueue | DL non-probe grants | Part of e2e_tot | Only inside serial |
| qwait | rx_grants, latency_histograms, summary | enqueue to taken | same | Part of e2e_tot | Only inside serial |
| pre | rx_grants, latency_histograms, summary | taken to dec_t0 | same | Part of e2e_tot | Only inside serial |
| dec_span (dec_span_us) | rx_grants, latency_histograms, summary | dec_t0 to dec_t1 | same | Same as legacy_dec_us | No |
| serial | rx_grants (serial_us), latency_histograms, summary | arr2enq plus qwait plus pre plus dec_span | same | Same interval as e2e_tot | No |
| first_body_us | rx_grants | first decode body wall | per grant | Inside dec_span | No |
| fep_us, chest_us, demod_us, ldpc_us | rx_grants | stage walls of the first body | per grant | Inside first_body_us | No |
| legacy_pdtim_real | summary only | PDTIMCLS real p50, p90, p99 (10 us bins) | first bodies of all calls since start | Inside body timing; not per grant | No |
| es_lead_us | rx_grants | early-start lead as printed | early grants only | Not a span of the chain | No |

### 5.3 Missing and early values

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- The C macro `E2E_US(a, b)` returns -1 when either stamp is 0 or when b is less than a (nr_pdsch_passive_queue.c:1631). The parser maps every negative value to None (parse.py:37-38).
- An early-start grant whose enqueue stamp is before slot arrival therefore prints arr2enq as -1. The exporter stores None. It does not store a negative value (K16).
- When arr2enq_us is None, serial_us is None.

### 5.4 Old dashboard definitions

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`. Source: audit sections 4.1 to 4.7. The old values are kept here so they are not mixed up with the new ones.

| Old definition | Source | Mobisys equivalent | Difference |
|---|---|---|---|
| PDTIMCLS `real` p50, p90, p99 (dashboard first choice) | PDTIMCLS line, 10 us bins, cumulative | summary `legacy_pdtim_real` (p50, p90, p99 only) | Last printed line only. No window. |
| PDTIMPCT pooled decode p50 and p99 (dashboard second choice) | PDTIMPCT line | Not parsed | None |
| EVMGRANT dec_us, exact percentile of last 4000 grants (dashboard third choice) | EVMGRANT `dec_us` | latency_histograms `legacy_dec_us`, cell_aggregates `legacy_dec_p50_us` and `p99` | Log-linear histogram (section 6). 60 s windows. Not last 4000. |
| goal_metrics: p99 under 3000 us on e2e tot. RECOVLATE tot_us replaces tot for recovered grants. | EVMGRANT `tot`, RECOVLATE `tot_us` | e2e_tot_us unchanged. retry_events `tot_us` kept as its own column (lab). | No replacement in mobisys. |
| Dashboard thresholds: p50 under 500 us, p99 under 3000 us | rxparse DEFAULT_THRESHOLDS (audit 4.1) | Not used | Not applied here |
| pdschq_latency: enqueue_dequeue, enqueue_fep, fep_ldpc, enqueue_ldpc, gpu_wait | ISAC, gated by csirs_disc, not in policy list | Not exported | gpu_wait is never written (audit 4.6). enqueue_fep includes queue wait. |

## 6. Histogram format and quantile error

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`. Source: hist.py.

- Log-linear. Each power of two is split into 32 sub-buckets. Values below 32 us are exact.
- Values are clamped to the range 0 to 2^40 minus 1 us.
- A bucket is `[lo, hi)`. For a value v in `[2^e, 2^(e+1))` with e of 5 or more, the width is `2^(e-5)`.
- Quantile: nearest rank, `rank = max(1, ceil(q times n))`. The result is the bucket midpoint `(lo + hi - 1) / 2`, clamped to the exact minimum and maximum. It is None when empty.
- Quantile error: the absolute error is at most `(hi - lo - 1) / 2` us. The relative error is at most 1/64 (about 1.56 %) for values of 32 us or more. It is zero below 32 us.
- Merging two histograms is exact. Counts add, because the bucket grid is the same.
- Rebuilding from exported rows (`from_rows`): min and max become bucket edges, and the sum uses midpoints. The exact sum is not in the rows. The mean and the quantile clamp are therefore looser.
- Exported rows give buckets, not raw values. Run-level summary quantiles use the same class over the whole run.
- For even n, the p50 is the lower of the two middle samples (nearest rank).

## 7. Derived correctness metrics

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`. Source: groundtruth.py, agg.py. Lab only. gNB logs are validation ground truth only. They are never receiver input (CLAUDE.md).

Symbols used by groundtruth.py (one run, one matched span):

- G: gNB DL grants in the span (`gnb_grants`).
- tp: gNB grants matched to a DL DCI accept (`tp_dci`). Equals correct_tb plus dci_ok_tb_failed plus dropped_undecidable.
- fp: DL DCI accepts in the span that match no gNB grant (`fp_dci`).
- fn: G minus tp (`fn_dci`). Equals missed_tb.
- correct_tb: matched, and the EVMGRANT is crc_ok.
- dci_ok_tb_failed: matched, and the EVMGRANT is crc_fail.
- dropped_undecidable: matched, and no EVMGRANT for that key.
- missed_tb: not matched.
- span: from max of the first DCI and first gNB slot, to min of the last DCI and last gNB slot, after the offset. Measured in slots. Not the whole run.

The offset is the modal value of `ext_slot(DCI) minus ext(gNB)` over key-equal pairs. It is reported as `offset_slots` with `offset_votes`.

Table (formulas from groundtruth.py `_metrics` and `match`):

| Metric | Formula | Numerator | Denominator | Emitted as | Note |
|---|---|---|---|---|---|
| DCI precision P | tp / (tp + fp) | gNB grants matched to a DL accept | DL DCI accepts in the span (tp + fp) | `precision` (None if no accepts) | |
| DCI recall R | tp / G | matched gNB grants | gNB DL grants in the span | `recall` (None if G is 0) | |
| Recall CI | Wilson score, z = 1.96, on (tp, G) | | | `recall_ci95` | Only recall has a CI. |
| F1 | 2 P R / (P + R) when P and R are both above 0. 0.0 when both are defined and one is 0. None otherwise. | | | `f1` | |
| False alarms per 1000 slots | 1000 times fp / span_slots | fp | span_slots (hi minus lo plus 1) | `false_alarms_per_1000_slots` | A rate per slot, not a fraction of accepts. |
| False-accept fraction | fp / (tp + fp), which equals 1 minus P | fp | DL DCI accepts in the span | Not emitted | Arithmetic from the same counts. |
| TB success over gNB grants | correct_tb / G | correct_tb | G (misses stay in the denominator) | `tb_success_over_gnb_grants` | |
| Conditional TB CRC pass given DCI match | correct_tb / (correct_tb plus dci_ok_tb_failed) | correct_tb | matched grants that have an EVMGRANT | Not emitted | Excludes missed and undecidable grants. |
| CRC pass fraction, first decode | crc_ok / grants | DL and UL crc_ok | DL non-probe grants plus UL CRC lines | summary `crc_ok_rate` | Pooled DL and UL (K2). |
| After-retry rate | (crc_ok plus rescued_unique) / grants | crc_ok plus DL rescues | same | summary `crc_ok_rate_after_retry` | Pooled. Rescues are DL only (K2). |
| Conditional TB recovery | Count of DL non-probe rx_grants rows with outcome crc_fail whose seq is an orig_seq of an attributed retry_events row, divided by the count of DL non-probe rx_grants rows with outcome crc_fail | rescued first failures | DL first-decode failures | Not emitted | Lab only. Affected by the 8192 pending bound and orphans (section 10). |

Rules:

- CRC pass fraction and recovery are never equal in general. They have different numerators and different denominators.
- A rescued TB is counted in crc_fail and in rescued_unique. It is never counted in crc_ok.
- DL and UL are pooled in `crc_ok_rate` and `crc_ok_rate_after_retry`. Do not read those two numbers as DL results.
- Do not pool groundtruth numbers from different runs or different offsets.

## 8. Discovery and lifecycle timing

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`. Source: parse.py `_LIFE`, agg.py `_MILESTONES`.

| Milestone name | Log needle | In `milestones_s` | Receiver time in the line |
|---|---|---|---|
| sync_start | "Starting sync detection" | yes | none |
| cell_detected | "Cell Detected with GSCN" | no | none |
| sync_ok | "Initial sync successful, PCI:" (sets pci) | yes | none |
| pbch_ok | "pbch decoded sucessfully" (spelling as in code) | yes | none |
| pbch_locked | "ACQ_EVENT pbch_locked" | yes | none |
| sib1_decoded | "ACQ_EVENT sib1_decoded" | yes | none |
| css0_autoconf | "CSS0 autoconf from" | yes | none |
| coreset_verified | "CORESET VERIFIED" | yes | none |
| coreset_bank_add | "multi-CORESET bank add" | no | none |
| dci_len_locked | "DCI 1_1 length locked" | yes | none |
| bwp_resolved | "BWP RESOLVED" | no | none |
| td_converged | "Technique D CONVERGED" | yes | none |
| fieldbook_promoted, fieldbook_withdrawn | "FIELDBOOK PROMOTED" / "FIELDBOOK WITHDRAWN" | no | none |
| ul_locked | "ULLAYOUT LOCK" or "ULLAYOUT BWP_LOCK" | yes | none |
| first_dci | First `rnti_seen` or `ul_seen` line | yes | utc_ns (R-REAL), not used for the milestone |
| first_crc_ok | First non-probe EVMGRANT with crc=1 | yes | E2E durations only, no absolute time |
| acq_state changes | ACQ_STATE lines | not a milestone | none. time_in_prev is an update count (audit 4.9). |

Rules for discovery timing:

- Every milestone time is E-ARR. It is the arrival time at the exporter, not the event time.
- Logging is asynchronous (ISAC_LOG_ASYNC). The pipe adds its own delay. Neither delay is measured.
- Differences between milestones are arrival differences. Use them only inside one run, with one exporter start.
- `cold_start.time_to_first_crc_ok_s` counts from the exporter start, not from the receiver start or from sync_start.
- `cold_start.attempts` is always 1. It is hard-coded and is not a count of attempts (agg.py:352).
- `cold_start.observed_from_start` is True when a sync_start line was seen. It does not prove the exporter started before the receiver.
- `cold_start.success` is True when a first_crc_ok was seen.
- `cold_start.note` says that t_s is the exporter arrival time.
- CONFIG_EPOCH lines have no time. gap_ms is a duration, not a timestamp.

## 9. Throughput

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- Not implemented. No code computes bits or bytes per second. The summary has no bytes field.
- Commercial mode cannot compute it. `tbs` is not in any commercial dataset.
- Lab mode has `tbs` in rx_grants and retry_events only. The unit is not stated in the source.
- UL: no TB size in any dataset. UL throughput is not supported.

Rule for a future implementation (a definition, not code):

- Count each TB once, at its first CRC pass.
- A DL non-probe rx_grants row with outcome crc_ok counts at its own t_s.
- A rescued TB counts at its attributed retry_events row (kind recovlate, tbs, at its t_s). A TB whose first decode passed is never rescued, so it is never counted twice.
- Grants with no EVMGRANT (queue drop, epoch drop, undecidable) count zero. Probes count zero. Failed grants that are not rescued count zero.
- Divide the sum by the window length in seconds.

## 10. Retry attribution

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`. Source: agg.py `_on_retry` and `_on_evmgrant`.

- A failed DL non-probe grant is stored under the key (rnti, frame, slot, harq_pid). The store holds 8192 keys. The oldest is evicted first.
- RECOVLATE with a pending key: `retry_recovlate` counter plus `rescued`. The row gets attributed True and orig_seq. The key moves to the rescued set.
- RECOVLATE with a key already rescued: `retry_dup`. The row is not attributed. It is counted once.
- RECOVLATE with an unknown key: `retry_orphan`. It is not in any success count.
- UL_RETRY: counter `retry_ul_retry` only. It is not attributed. It does not add to UL crc_ok. The UL first failure stays in crc_fail.
- LAYOUT_PROBE: counter `retry_layout_probe` only. It is not a TB outcome.
- `rescued_unique` is the number of distinct attributed keys. `crc_ok_after_retry` is crc_ok plus rescued_unique.
- Lab rows keep attributed and orig_seq. Join orig_seq to rx_grants.seq.
- Receiver recov_* keys (section 15) count the receiver's async worker. They are not the exporter's attribution. Do not mix them with `rescued_unique`.

## 11. Telemetry loss counters

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

Exporter side (summary.json `telemetry_loss` and counts):

| Counter | Meaning |
|---|---|
| queue_drops (per dataset) and queue_drops_total | Rows dropped by `put_nowait` because the 100000-row queue was full. Silent in the data. Counted here. |
| policy_refused_rows and policy_refused_fields | Rows refused by `policy.validate_row`. Exit code 3. |
| parse_errors | Exceptions while feeding a line. Only the class name is kept. |
| k_suppressed_rows and k_suppressed_events | Commercial suppression: dropped groups and the sum of their n. |
| counts.lines | Lines fed to the aggregator. |
| counts.parsed_<kind> | Lines parsed per kind (evmgrant, dci_seen, ul_crc, metrics, pdtimcls, retry, config, health, life). Lines with no parser are not counted. |
| counts.span_inconsistent | Grants with serial_us above e2e_tot_us plus 4 |
| counts.probe_decodes | Probe decodes (not in the grant counts) |
| counts.retry_orphan, retry_dup, retry_ul_retry, retry_layout_probe | Retry handling (section 10) |
| counts.health_<kind>, config_<kind>, suppressed_rows, suppressed_events | As named |

Receiver side (C). The exporter cannot see most of these except as a gap.

| Loss | Where | Visible to the exporter? |
|---|---|---|
| ISAC line too big: `LOG_W "ISAC_METRICS buffer too small"`. The buffer is 16384 bytes. The tail is 768 bytes. A line that does not fit is dropped whole. | nr_passive_metrics.c:259-262; json.c tail | Gap in src=metrics rows only |
| `ISAC_LOG_ASYNC dropped N records` (log ring full) | audit 1.11 | No |
| scanq_drop_full, scanq_drop_stale, scanq_drop_epoch | ISAC, exported | Yes |
| pdschq_drop_full, pdschq_drop_stale, pdschq_drop_epoch, pdschq_stale_after_decode, puschq_drop_epoch | ISAC, exported | Yes |
| recov_dropped_full, recov_dropped_old, recov_expired | ISAC (new), exported | Yes |
| retry_yield_gated | ISAC (new), exported | Yes |
| acq_sync_losses | ISAC, exported | Yes |
| sense_cfr_drop, sense_rec_drop, sense_io_errors | ISAC, not in policy list | No |
| rxdiscont, rftsdisc, rx_recover events | Log lines, lab slot_health | Lab only |

A zero in an exported counter does not prove there was no loss. The line that holds it may have been dropped.

## 12. Multi-cell readiness

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

Keys that carry pci:

- system_telemetry `pci` (ISAC `pci`, -1 before lock).
- discovery_events `pci` (lab).
- cell_aggregates `pci`, latency_histograms `pci`, discovery_summary `pci`.

Findings:

- The receiver holds one PCI in one atomic (`nr_passive_metrics_pci`, nr_passive_metrics.h/.c).
- The exporter holds one `self.pci` (agg.py:78, 221, 277).
- Window state is not keyed by PCI.
- Window rows take the PCI at window close (agg.py:309, 320, 327).
- A PCI change inside a window gives the wrong PCI to that window.

Verdict: not ready for multi-cell. Nothing in the code keeps per-cell state.

Needed, not implemented: a PCI on every line, window state keyed by PCI, and a per-cell window close.

## 13. Metrics not supported

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

| Metric | Why not supported | Ref |
|---|---|---|
| Throughput (DL or UL) | Not computed. No bytes field. UL has no TB size. | section 9 |
| Commercial per-grant, per-UE, per-RNTI data | Refused by design | policy.py |
| Per-candidate PDCCH cost | ISAC has aggregate counts only | audit 3.3 |
| UHD overflow count | No ISAC counter. isac_usrp_ring_traffic is never called. | audit 3.4 |
| DCI-accept time on the PDSCH job, job-level async latency (RECOV, DMRS scan, UL NID, CB0) | No job timestamps in the lines | audit 3.1, 3.10 |
| Receiver lifecycle timestamps | Only arrival times at the exporter | audit 3.6 |
| PHY-emitted gNB ground truth | Only MAC scheduled grants (GNB source) | audit 3.7 |
| Absolute time across hyperframes | ext_slot and abs_slot are process-local | audit 4.10, 3.22 |
| Per-event HARQ combining | HARQC is not parsed | audit 3.20 |
| PDSCH schedule batch structure | PDSCHQ batches line not parsed | audit 3.21 |
| TB payload or DCI payload bytes | Dropped by design. parse.py drops tb= and payload= fields. | audit 5.3, 5.4 |
| UL per-grant rows (lab) | agg writes no UL rx_grants row | agg.py `_on_ul_crc` |
| SCAN_E2E, GPUPASS, DECSLOW, PDTIMPCT, PDTIMCLSST, UL_E2E, PDSCH solver lines | Lines exist. parse.py has no parser for them. | parse.py |
| CUDA LDPC times, GPU stage spans, ldpc_tb_cpu, ldpc_tb_cuda, ldpc_cuda_* | Not in the policy list. Dropped. | audit 3.15 |
| Technique D (td_*), field book, CB0, sense_*, dl_gpu_*, gw_pool_refused, gpu_fep_*, obs_* | Not in the policy list. Dropped silently. | agg.py `_on_metrics` |
| srs_us, PRACH and SRS timing, CSI-RS discovery counters | Not in the policy list. Dropped. | agg.py |
| records.jsonl and .cfr sensing output | Not parsed | audit 2.3 |
| Receiver heartbeat while unsynced | No ISAC line while unsynced. The host sampler still writes 1 Hz rows. | audit 3.12 |
| Receiver CPU and GPU utilisation from C | Not in C. Supported only from the exporter sampler (host and process CPU, nvidia-smi GPU at 5 s). | section 2.7 |
| Multi-cell | Not ready | section 12 |
| Multi-operator | No code | none |
| Exporter overhead | Not measured | none |
| Discovery summary per RNTI | Not in any dataset | policy.py |

## 14. Known issues from this read (not fixed here)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh]`

- K1. `discovery_summary` uses an OR rule (agg.py:322-329). A row is written when accepts or grants reach 10. Both counts are then written, so a count below 10 can be published. Whether this is intended is not stated in the sources.
- K2. `crc_ok_rate` and `crc_ok_rate_after_retry` pool DL and UL (agg.py:335-345). The numerator includes UL crc_ok. The rescues are DL only. The after-retry rate is therefore not a DL or UL rate.
- K3. `window_start_s` is the arrival wall-clock time floored to 60 s (agg.py:99, 293). It is not run time. Windows close only when a later line arrives (agg.py:99-104). Quiet periods delay rows.
- K4. One `self.pci` is used for all windows, taken at close (agg.py:276-277, 309, 320, 327). See section 12.
- K5. Latency histogram buckets are not suppressed one by one. Only the window total is checked (agg.py:314-321). A bucket can show a count of 1 in commercial mode.
- K6. Unknown enum strings are not refused. `_enum` recodes them to the last value in the enum (agg.py:50-51). Examples: unknown dir becomes "ul". Unknown acq_state becomes "UNKNOWN". Unknown fmt becomes "0_1". The C can print "0_x", which is in the enum, so this does not fire today.
- K7. The sampler's GPU fields are all None if any one field fails to parse. For example, a field printed as "[N/A]" nulls all three (export.py:134-137). Only the first GPU is read.
- K8. `nr_pdsch_recovery_stats` p50 loop (nr_pdsch_recovery.c:156-160). Once p50 is set to 0 (the first bin holds at least half the samples), the next loop step sets it to 1, because the test is `!p50`. Result: `recov_late_ms_p50` reads 1 when the 0 ms bin holds at least half the samples and p99 is in a later bin. p99 is correct. Both values read 0 when there are no samples, which cannot be told apart from a real 0 ms.
- K9. `recov_late_ms` bins saturate at 255 ms (nr_pdsch_recovery.c:107). `recov_late_ms_p99` cannot read above 255.
- K10. `tools/mobisys/make_synthetic.py` writes `recov_attempts`. The C code does not emit that key. The fixture is not evidence.
- K11. `DYN_PREFIXES` (policy.py:52, 111-112) accepts any numeric key starting with `recov_` or `retry_yield_`, with no whitelist. A future key with those prefixes would be persisted without review.
- K12. `summary.json` counts and last_metrics are whole-run totals. Commercial mode does not k-suppress them.
- K13. `proc_maxrss_kb` is the peak, not the current RSS. Use `rx_rss_kb` for current size.
- K14. If getrusage fails, the proc_* keys read 0, not None (nr_passive_metrics.c collect). The return value is checked, but the fields are not omitted.
- K15. `t_mono_ns` is read at the top of collect. `t_real_ns` is read after the NID capture block. The clock offset carries that gap. CLOCK_REALTIME can also step (NTP). The exporter uses the median of the last 64 offsets.
- K16. The audit (sections 3.1 and 4.4) says arr2enq "can be zero or negative" for early-start grants. The C macro prints -1 in that case, and the parser stores None (section 5.3). Negative values are not printed. This file follows the code.
- K17. `legacy_pdtim_real` is in the lat_metric enum but no histogram is written for it (agg.py:85).
- K18. Recovery `late_ms` in `recov_*` and `retry_yield_*` have no units in the key names. The C header states ms for late values only.

## 15. New ISAC keys (working-tree diff, uncommitted)

`[CODE-READ, branch claude/optimistic-euler-mpdyvh, working tree, uncommitted]`

Status: source only. Not built, not run, not in HEAD. The diff also changes `openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_metrics_test.cc` (not run). The keys are appended after all existing keys in the JSON (nr_passive_metrics_json.c, tail buffer 768 bytes).

| Key or keys | C source | Unit | Omitted when | Notes |
|---|---|---|---|---|
| t_real_ns | clock_gettime(CLOCK_REALTIME) in nr_passive_metrics_collect, after the NID block | ns | never | Read after the NID block. t_mono_ns is read at the top. The gap is part of the offset (K15). |
| proc_utime_us, proc_stime_us | getrusage(RUSAGE_SELF), ru_utime and ru_stime | us, cumulative | never (reads 0 if getrusage fails, K14) | All threads of the process |
| proc_maxrss_kb | ru_maxrss | kB, peak | never | Peak, not current (K13) |
| proc_minflt, proc_majflt | ru_minflt, ru_majflt | count, cumulative | never | |
| cfg_epoch | nr_cfg_epoch_current() (nr_passive_cfg_epoch.c:187) | epoch counter | when the getter is absent (weak symbol) | Same epoch as CONFIG_EPOCH lines |
| cfg_identity_gen | nr_cfg_epoch_identity_gen() (nr_passive_cfg_epoch.c:188) | counter. Increments on HARD_RESET (header comment). | when the getter is absent | |
| recov_submitted, recov_rescued, recov_stale, recov_expired, recov_dropped_full, recov_dropped_old, recov_yielded | RECOV text from nr_pdsch_recovery_stats (nr_pdsch_recovery.c:150-163), parsed by sscanf for 9 fields | count, cumulative | when the getter is absent or the parse fails | Parse failure is silent (has_recov stays 0) |
| recov_late_ms_p50, recov_late_ms_p99 | Same RECOV text | ms, integer bins | Same | 256 bins of 1 ms. The last bin saturates at 255 ms (K9). p50 defect (K8). 0 and 0 when no samples. |
| retry_yield_chains, retry_yield_yields, retry_yield_gated | nr_pdsch_retry_yield_stats (nr_pdsch_passive_decode.c:4164) | count, cumulative | when the getter is absent | Same counters as P99AGE retry_chains, retry_yields, retry_gated (audit 1.7) |

Each key is also a policy column (section 2.7). Keys in the table map to the column names in section 2.7.

Notes:

- The serializer returns -1 when the tail does not fit. The whole ISAC line is then dropped, with LOG_W "buffer too small" (nr_passive_metrics.c:261-262).
- The RECOV text buffer is 256 bytes. Its longest possible content is about 241 bytes, so it always fits.
- No build and no test run was done. The unit test in the diff is not evidence.
