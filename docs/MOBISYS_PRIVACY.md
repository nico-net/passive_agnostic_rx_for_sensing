# MobiSys telemetry: privacy rules for commercial mode

Date: 2026-10-20. Code read at HEAD 2d87833 (tools/mobisys has no uncommitted changes).
Status: operator and developer policy. This file does not approve any measurement.

Sources for this file:

- tools/mobisys/policy.py, store.py, export.py, agg.py, test_privacy.py, test_core.py (read in full), README.md.
- docs/MOBISYS_TELEMETRY_AUDIT.md, section 5 (receiver fields that are privacy-sensitive). The audit read receiver code at commit 3f0d921. Its facts were not re-checked here.
- CLAUDE.md (project hard rules).

Scope note: PROJECT_MEMORY.md (4696 lines) was not read in full for this file. Only its section 0.1 (evidence labels) was used. It has no MobiSys or privacy section.

## 0. Evidence labels

| Label | Meaning |
|---|---|
| `[CODE-READ, 2d87833]` | Read in tools/mobisys at HEAD 2d87833. |
| `[OFFLINE VERIFIED, cloud x86, @2d87833]` | Unit test run on this cloud host (x86_64). test_privacy: 5 tests OK. test_core: 26 tests OK. |
| `[CITED, audit §5.n]` | From docs/MOBISYS_TELEMETRY_AUDIT.md. Receiver code read at 3f0d921. Not re-checked here. |
| `[CITED, task brief]` | Stated in the request that created this file. Not checked against the sources. |
| `[CITED, CLAUDE.md]` | Project rule. |
| `[KNOWN ISSUE]` | Reproducible gap with code evidence (PROJECT_MEMORY §0.1 label). |
| `[UNCHECKED]` | Not verified from the sources. Operations advice. Confirm before relying on it. |

## 1. Scope

### 1.1 Mode choice is not authorization

- Selecting commercial mode is NOT legal or ethical authorization to intercept or process third-party communications. [CITED, task brief]
- Commercial mode only limits what the exporter writes to disk. It does not decide whether a measurement is allowed. [CITED, task brief]
- Institutional or operator approval is required before any run on a network or cell that others use. Get it in writing before the run. [CITED, task brief]
- Lab mode is only for an authorized test gNB, a test SIM and test traffic. [CITED, task brief]
- The exporter does not check approval. It cannot tell an approved run from an unapproved one. [CODE-READ, 2d87833]
- gNB logs and configs are validation ground truth only. They are never receiver input. groundtruth.py is lab only. [CITED, CLAUDE.md] [CODE-READ, 2d87833]

### 1.2 Approved measurement scope: what commercial mode persists

Commercial mode writes four datasets and two JSON files. Nothing else is written. [CODE-READ, 2d87833, policy.py:62-94, store.py:34-47]

| Dataset | Row unit | Columns | Window and grouping | Suppression |
|---|---|---|---|---|
| system_telemetry | `src=host` sampler row (about every 1 s) and `src=metrics` row (one per receiver metrics line) | t_s, src, acq_state, host_cpu_pct, mem_avail_kb, rx_cpu_pct, rx_rss_kb, rx_threads, gpu_util_pct, gpu_mem_mb, gpu_temp_c, plus the receiver counters in the list below | per sample, not windowed | none |
| latency_histograms | one row per histogram bucket | window_start_s, pci, dir, metric, bucket_lo_us, bucket_hi_us, count | 60 s window; per dir and metric | series with fewer than 10 samples are not written |
| cell_aggregates | one row per group | window_start_s, pci, dir, mcs_bucket, nl_bucket, n_grants, n_crc_ok, n_crc_fail, snr_db_mean, e2e_tot_p50_us, e2e_tot_p99_us, legacy_dec_p50_us, legacy_dec_p99_us | 60 s window; per (dir, MCS bucket, layers bucket) | groups with fewer than 10 grants are not written; complementary rule (section 5) |
| discovery_summary | one row per window | window_start_s, pci, acq_state, ue_bucket, n_dci_accepts, n_grants, n_crc_ok | 60 s window | dropped only when n_dci_accepts and n_grants are both below 10 (known gap, section 5) |

[CODE-READ, 2d87833, policy.py:62-94, agg.py:297-329]

The receiver counters allowed in system_telemetry are exactly these names. [CODE-READ, 2d87833, policy.py:43-57]

```
pci acq_transitions acq_sync_losses acq_pbch_locks acq_sib1_decodes pdcch_occasions pdcch_candidates pdcch_accepts
pdcch_accepts_c scanq_queued scanq_processed scanq_drop_full scanq_drop_stale scanq_drop_epoch scanq_max_lag
pdschq_queued pdschq_decoded pdschq_crc_ok pdschq_drop_full pdschq_drop_stale pdschq_drop_epoch
pdschq_stale_after_decode pdschq_max_lag puschq_drop_epoch pusch_try pusch_crc_ok ldpc_ok ldpc_seg_fail ldpc_tb_fail
ldpc_zero_tb discovery_yields discovery_overruns rt_gpu_fallbacks abs_slot t_mono_ns t_real_ns cfg_epoch
cfg_identity_gen proc_utime_us proc_stime_us proc_maxrss_kb proc_minflt proc_majflt
gpu_budget_used_pct
recov_* and retry_yield_* (any numeric key with these prefixes)
```

Other keys in an ISAC_METRICS line are dropped silently before validation. [CODE-READ, 2d87833, agg.py:271-289]

The two JSON files are written by code, not by row validation. [CODE-READ, 2d87833, store.py:75-81, export.py:150-170]

- summary.json: run totals, rates, latency percentiles, cold-start timing, clock offset, telemetry loss counts, and the mode. Section 9 lists its gaps.
- manifest.json: schema, mode, provenance, host_label, output format, pyarrow and Python versions, K, window length, slots per frame, rotate rows, started_unix, duration_s, part counts, rows written, telemetry loss, clock offset, and input source (`stdin`).

Commercial mode never writes:

- any RNTI, in any form (plain, hex, or hashed). [CODE-READ, 2d87833, policy.py:41, 128; agg.py:58-70]
- per-grant, per-DCI or per-UE rows. [CODE-READ, 2d87833, agg.py:152-164, 196-201]
- DCI or PDSCH payload bytes or hex. No dataset has a payload column. [CODE-READ, 2d87833, policy.py:62-94]
- raw log text. [CODE-READ, 2d87833, policy.py:116-144]
- lab datasets (section 4.8).
- IQ, Sensing v2 records, CFR, replay captures or GPU dumps. The exporter does not read them. [CODE-READ, 2d87833]

## 2. Modes and defaults

- The default mode is commercial. [CODE-READ, 2d87833, export.py:185]
- The mode is fixed when the exporter starts. The Sink and the Aggregator take it in their constructors. No option or message changes it during a run. [CODE-READ, 2d87833, store.py:22-25, agg.py:72-75]
- The mode is written to summary.json and manifest.json as `mode`. [CODE-READ, 2d87833, export.py:155, 162]
- Lab mode turns on the per-grant, per-DCI, discovery, configuration, retry and slot-health datasets. It sets K = 1, so nothing is suppressed. [CODE-READ, 2d87833, policy.py:62-94, agg.py:75]
- Lab output holds RNTIs per grant and per DCI attempt. Treat lab output as sensitive. [CODE-READ, 2d87833, policy.py:64-79]
- Lab mode may read a regular file with `--input`. Commercial mode refuses it. [CODE-READ, 2d87833, export.py:204-207]
- Raw payload and raw IQ recording are off by default. This includes lab mode. The exporter cannot write payload bytes in any mode. [CODE-READ, 2d87833, policy.py:62-94]
- The receiver has its own raw-capture options. They are opt-in and must be explicitly set. Section 3 lists them. [CITED, audit §5.8-5.11]

## 3. The persistence boundary

### 3.1 The problem

- The receiver's stdout and stderr carry C-RNTIs, DCI payload hex, a CMDLINE line with full argv, and cell identity. [CITED, audit §5.1, §5.3, §5.5, §5.6]
- In commercial mode these bytes must never be written to a file. [CITED, task brief]

### 3.2 The only supported launch line

```
nr-uesoftmodem <receiver args> 2>&1 | python3 tools/mobisys/export.py --experiment-mode commercial --out DIR --host-label "DGX aarch64"
```

[CODE-READ, 2d87833, tools/mobisys/README.md line 5, export.py docstring]

- Stdout and stderr both go into the pipe. [CODE-READ, 2d87833, README line 5]
- The exporter never writes to its own stdout. Its stderr holds fixed messages only. [CODE-READ, 2d87833, export.py:32-37, 206, 213, 243]

### 3.3 What the exporter enforces

- In commercial mode, stdin must be a FIFO. A regular file, a terminal or a socket is refused with exit 2. The message says "refused in commercial mode". [CODE-READ, 2d87833, export.py:210-215]
- `--input` is refused with exit 2 in commercial mode. [CODE-READ, 2d87833, export.py:204-207]
- Test `test_regular_file_stdin_refused` checks both refusals. [OFFLINE VERIFIED, cloud x86, @2d87833]

### 3.4 What the exporter cannot enforce

- It cannot see upstream of the pipe. A FIFO that a `tee`, a `>` redirect, a supervisor log or another reader writes to disk still passes the FIFO test. [CODE-READ, 2d87833, export.py:210-215]
- Pipe discipline is therefore the operator's job. The checklist below covers the rest. [CODE-READ, 2d87833]

### 3.5 Operator checklist for a commercial run

Check each item before the run starts. [CITED, task brief] Items marked with an audit section come from the audit.

- [ ] Nothing redirects or copies receiver stdout or stderr to a file. No `tee`, `>`, `>>`, `script`, or log wrapper sits between the receiver and export.py. [CITED, task brief]
- [ ] No service manager, supervisor or CI job starts the receiver while capturing stdout or stderr (journald, syslog, nohup.out, a log file). [CITED, task brief] [UNCHECKED, how each tool stores output]
- [ ] Dashboard off. monitor.py starts automatically and serves the raw log over local HTTP. Confirm nothing listens on 127.0.0.1:8080 (endpoints /api/log and /api/log/download). The switch that turns it off is not named in this document. [CITED, task brief] [CITED, audit §5.13] [UNCHECKED, switch]
- [ ] Dashboard run-dir log (dashboard.log) is not created. It is covered by the dashboard being off. [CITED, audit §5.14]
- [ ] Sensing v2 records and CFR off. Keep `sensing.enable = 0`, or leave the output path unset. Records hold per-grant RNTIs; CFR is per-UE channel response. [CITED, task brief] [CITED, audit §5.8, §5.9]
- [ ] `ISAC_PASSIVE_REPLAY_CAPTURE` unset. The replay file holds slot IQ, DCI payloads and RNTIs. [CITED, task brief] [CITED, audit §5.10]
- [ ] GPU dump environment variables unset. They write demod, LLR and IQ inputs of up to 128 grants. The names are not listed in this document. Search the receiver source for the dump options before the run. [CITED, task brief] [CITED, audit §5.11] [UNCHECKED, names]
- [ ] No raw IQ capture. Raw sc16 files are operator-run and hold full-bandwidth IQ of all UEs. [CITED, audit §5.12]
- [ ] Campaign launchers not used for commercial runs. They write run.log, cmd.txt, env.txt and source.patch into a run directory. [CITED, audit §5.5, §5.21, §5.22]
- [ ] `ISAC_METRICS_PATH`: allowed. It is aggregate-only. Set it only if needed. [CITED, task brief] [CITED, audit §5.17]
- [ ] `ISAC_SIB1_CACHE_DIR`: the SIB1 cache holds cell-level configuration, not UE data. The default /tmp/passive_rx is world-writable (mode 01777). Use a private directory. [CITED, audit §5.7]
- [ ] Core dumps off (section 7.1).
- [ ] Output directory is on encrypted disk or tmpfs (section 7.5).

### 3.6 Stop order

- Stop the receiver with SIGINT. Project rule. [CITED, CLAUDE.md]
- Stopping the receiver closes the pipe. The exporter reads end-of-file, writes summary.json and manifest.json, and exits by itself. [CODE-READ, 2d87833, export.py:59-67, 225-234]
- SIGTERM and Ctrl-C also end the exporter cleanly. [CODE-READ, 2d87833, export.py:218, 228-234]

## 4. Fail-closed behaviour

### 4.1 Two layers

- Layer 1, agg.py: decides what to build. In commercial mode it builds only window aggregates and the run summary. It never builds per-grant, per-UE or RNTI rows. [CODE-READ, 2d87833, agg.py:152, 196, 213, 225, 233]
- Layer 2, policy.validate_row through store.Sink.write: checks every row before it is written. A failing row is dropped and counted. [CODE-READ, 2d87833, store.py:34-41, policy.py:116-144]

### 4.2 Row rules (policy.validate_row)

- The mode must be `lab` or `commercial`. [CODE-READ, 2d87833, policy.py:116-119]
- The dataset must exist and must allow the mode. [CODE-READ, 2d87833, policy.py:120-124]
- Every column must be on the dataset's allow-list. An unknown column is refused. [CODE-READ, 2d87833, policy.py:126-130]
- In commercial mode, any column marked as identifying (`ident`) is refused. Only `agg` columns pass. [CODE-READ, 2d87833, policy.py:128]
- int: a Python int (not bool), in signed 64-bit range. [CODE-READ, 2d87833, policy.py:134-135]
- float: an int or float (not bool), and finite. [CODE-READ, 2d87833, policy.py:136-137]
- bool: a real bool. [CODE-READ, 2d87833, policy.py:138-139]
- enum: a string that is in the list in policy.ENUMS. [CODE-READ, 2d87833, policy.py:140-141]
- None is allowed for any column. [CODE-READ, 2d87833, policy.py:132-133]
- One bad column refuses the whole row. [CODE-READ, 2d87833, policy.py:142-144]

### 4.3 Enum coercion

- Layer 1 does not refuse unknown receiver strings. It maps them to the last value of that enum. [CODE-READ, 2d87833, agg.py:50-51]
- Examples: an unknown acq_state becomes `UNKNOWN`. An unknown fmt becomes `0_1`. [CODE-READ, 2d87833, policy.py:12-33]
- This hides input errors. It does not leak data. [CODE-READ, 2d87833]

### 4.4 Refused rows

- A refused row is never written. [CODE-READ, 2d87833, store.py:35-41]
- The exporter counts refused rows and fields. The counts go to summary.json under `telemetry_loss`. [CODE-READ, 2d87833, export.py:150-152]

### 4.5 PolicyViolation messages

- Messages are fixed text. Examples: `row refused by policy (dataset NAME)`, `unknown mode`, `unknown dataset`, `unknown file`, `dataset NAME not permitted in MODE mode`. [CODE-READ, 2d87833, policy.py:116-144, store.py:24, 77]
- The message holds only internal names and a count of bad fields. It holds no row values and no input text. [CODE-READ, 2d87833, policy.py:98-102]

### 4.6 Exit codes

| Code | Meaning | Source |
|---|---|---|
| 0 | OK | export.py:172 |
| 2 | Input or usage refused: stdin not a pipe, `--input` in commercial mode, or argparse error | export.py:204-215 |
| 3 | Policy violation: at least one row refused | export.py:172 |
| 4 | Internal error. Only the exception class name is printed. | export.py:237-244 |

[CODE-READ, 2d87833]

- argparse usage errors also exit 2. They echo the operator's own command-line arguments. They never echo receiver data. [CODE-READ, 2d87833, export.py:183-202]

### 4.7 Error text never echoes input

- The exception hooks print only the exception class name. No traceback is printed. [CODE-READ, 2d87833, export.py:32-37]
- Per-line parse failures are counted, not printed. [CODE-READ, 2d87833, export.py:75-79]
- The top-level handler prints only the class name and exits 4. [CODE-READ, 2d87833, export.py:237-244]
- Test `test_forced_exception_paths_do_not_echo_input` checks this with injected failures. [OFFLINE VERIFIED, cloud x86, @2d87833]

### 4.8 Lab datasets are refused in commercial mode, even when called directly

- A commercial Sink refuses rx_grants, dci_attempts, discovery_events, configuration_events, retry_events and slot_health. [CODE-READ, 2d87833, policy.py:62-94, 123-124]
- A direct call to `store.Sink(dir, "commercial").write("rx_grants", ...)` raises PolicyViolation and writes nothing. [OFFLINE VERIFIED, cloud x86, @2d87833, test_core `test_lab_dataset_refused_in_commercial_sink_direct`]
- An unknown mode is refused when the Sink is built. [CODE-READ, 2d87833, store.py:23-24]

## 5. Aggregation

### 5.1 Windows and threshold

- The window is 60 s (`policy.WINDOW_S`). The window index is the exporter arrival time divided by 60. [CODE-READ, 2d87833, policy.py:10, agg.py:99]
- A window closes when a line from a later window arrives, or at shutdown. [CODE-READ, 2d87833, agg.py:100-109]
- K is 10 in commercial mode (`policy.K_COMMERCIAL`). [CODE-READ, 2d87833, policy.py:9, agg.py:75]

### 5.2 cell_aggregates

- A (dir, MCS bucket, layers bucket) group with fewer than 10 grants in the window is not written. [CODE-READ, 2d87833, agg.py:297-313]
- Complementary suppression: if exactly one group of one direction is suppressed, the smallest published group of that direction is also hidden. Ties go to the lower key. This stops subtraction in that one case. [CODE-READ, 2d87833, agg.py:295-302]
- Test `test_complementary_suppression` covers the one-group case. [OFFLINE VERIFIED, cloud x86, @2d87833, test_core]

### 5.3 latency_histograms

- A (dir, metric) series with fewer than 10 samples in the window is not written. [CODE-READ, 2d87833, agg.py:314-321]
- Bucket counts inside a written series are not suppressed one by one. A bucket can hold 1 sample. [CODE-READ, 2d87833, agg.py:319-321]

### 5.4 discovery_summary

- A row is dropped only when n_dci_accepts is below 10 and n_grants is below 10. [CODE-READ, 2d87833, agg.py:322-329]
- If either count is 10 or more, the row is written with both exact values. The small one is shown. [KNOWN ISSUE, CODE-READ, 2d87833, agg.py:323-329]
- Example: a window with 3 grants and 40 DCI accepts writes `n_grants = 3`. [CODE-READ, 2d87833, agg.py:323-329]

### 5.5 Distinct UE count

- The exporter keeps a set of RNTIs per window, capped at 65,536. The set is discarded at window close. [CODE-READ, 2d87833, agg.py:58, 170-171, 193-194, 292-294]
- Only a bucket is written: `0`, `1-9`, `10-49` or `50+`. [CODE-READ, 2d87833, agg.py:33-34, 329]
- Bucket `1-9` can still mean one UE. The buckets are not suppressed. [CODE-READ, 2d87833]

### 5.6 RNTIs live in memory only

- Two run-lifetime structures hold RNTIs: a map of recent failed grants (up to 8192 entries of RNTI, frame, slot and HARQ pid), and a set of rescued keys (up to 8192). [CODE-READ, 2d87833, agg.py:12, 82, 142-145, 246-255]
- Neither is written to disk. [CODE-READ, 2d87833, store.py, export.py]
- RNTIs are never hashed. No hash function is used in tools/mobisys. [CODE-READ, 2d87833, grep of tools/mobisys for hashlib, hash, sha, md5 found none]

## 6. Retention and deletion (operator policy)

- The exporter never deletes anything. It writes part files and never merges or removes them. [CODE-READ, 2d87833, store.py:53-72]
- Nothing is automated. There is no timer and no cleanup on exit. The only removal is the empty `.tmp` directory at close. [CODE-READ, 2d87833, store.py:83-88]
- The operator must set a retention period for each run. [CITED, task brief]
- The operator must name who may open the output and who deletes it. Record the deletion date. [CITED, task brief]
- Delete receiver-side files too: the items in section 3.5, run directories and shell history (section 7.4). [CITED, audit §5.5, §5.21, §5.22]
- Overwriting a file does not reliably erase it on SSD or copy-on-write storage. Rely on encryption at rest and on destroying the key. [UNCHECKED]

## 7. OS-level controls (the application cannot enforce these)

### 7.1 Core dumps

- A core dump holds the whole receiver memory: IQ rings, RNTIs, transport blocks and LLRs. The launchers set `ulimit -c 0` unless CORE is set. [CITED, audit §5.20]
- Run `ulimit -c 0` in the shell that starts the receiver. Check that `ulimit -c` prints 0. [UNCHECKED, command behaviour not run here]
- Read /proc/sys/kernel/core_pattern. A pattern that starts with `|` (a pipe to a handler) or that writes into the output directory must be changed. [UNCHECKED]
- If systemd-coredump is used, set `Storage=none` in /etc/systemd/coredump.conf. Also set `ProcessSizeMax=0`. [UNCHECKED, config keys not verified here]

### 7.2 Swap and memory locks

- RNTIs and sample buffers in RAM can reach swap. Use `swapoff -a`, or encrypted swap only. [UNCHECKED]
- Disable hibernation, or encrypt it. Hibernation writes all RAM to disk. [UNCHECKED]
- Check the memlock limit with `ulimit -l`. Check RLIMIT_MEMLOCK in limits.conf. This document does not state whether the receiver locks memory. [UNCHECKED]

### 7.3 journald and syslog

- If receiver stdout or stderr reaches journald or syslog, the lines are stored. The `2>&1 |` pipe avoids this only when the operator starts the receiver from a shell. [CODE-READ, 2d87833, README line 5] [UNCHECKED, unit configuration]
- After a run, search the journal for `rnti=` and `payload=`. Expect no hits. [UNCHECKED, search method not run here]

### 7.4 Shell history and command lines

- The receiver prints a CMDLINE line with full argv. Shell history keeps the same command line. [CITED, audit §5.5]
- Start the command with a leading space, with HISTCONTROL=ignorespace. Or turn history off for the session. [UNCHECKED]
- Remove stored command files in run directories (cmd.txt, env.txt). [CITED, audit §5.5, §5.22]

### 7.5 tmpfs or disk for the output directory

- The exporter writes all output under `--out`, including its `.tmp` subdirectory. It does not use the system temp directory. [CODE-READ, 2d87833, store.py:32, 63]
- tmpfs keeps output in RAM only. It is lost at reboot. It can be swapped out unless swap is off or encrypted. [UNCHECKED]
- A disk output directory must be on encrypted storage (section 7.6). [CITED, task brief]
- Pick one for each run and record it in the run notes. [CITED, task brief]

### 7.6 Filesystem encryption and backups

- Put every disk that holds output or receiver logs under filesystem encryption. [CITED, task brief] [UNCHECKED, method]
- Exclude output and run directories from backups, or encrypt the backups. A backup keeps data that was deleted from the source. [UNCHECKED]

### 7.7 File modes and local readers

- The exporter does not set file modes. Its parts get the default umask. [CODE-READ, 2d87833, store.py:69, 79]
- Some receiver files are written mode 0644 (Sensing records, CFR). The SIB1 cache directory is mode 01777. Set `umask 077` in the operator shell. [CITED, audit §5.7, §5.8, §5.9]
- The dashboard binds 127.0.0.1. Any local user on the host can still read it. [CITED, audit §5.13]

## 8. Tests that enforce this

Run from tools/mobisys: `python3 -m unittest -v`. [CODE-READ, 2d87833, tools/mobisys/README.md line 10]

Run on cloud x86 at HEAD 2d87833: test_privacy 5 tests OK; test_core 26 tests OK. [OFFLINE VERIFIED, cloud x86, @2d87833]

### 8.1 test_privacy.py

All five tests run the real exporter as a subprocess with a real pipe. The input is hostile: C-RNTIs 4601, 4602, 17921 and 17922 in several forms, `DEADBEEF`, `PAYLOADMARKER` and `payload=`. [CODE-READ, 2d87833, test_privacy.py:17-38]

- `test_end_to_end_all_files_clean`: runs in jsonl and auto format, with rotation every 3 rows. It scans every file for byte markers. It also scans text for the RNTI and payload patterns, but not parquet. It checks that stdout and stderr are clean. It checks exit 0, no rx_grants or dci_attempts directory, mode commercial, and a cell_aggregates directory. It checks that nothing was written to TMPDIR. [CODE-READ, 2d87833, test_privacy.py:71-83]
- `test_metric_values_are_not_persisted_as_strings`: checks system_telemetry rows. For this input, string values may only be `metrics`, `host` or `UNKNOWN`. It does not test every enum value. [CODE-READ, 2d87833, test_privacy.py:85-91]
- `test_regular_file_stdin_refused`: a regular file on stdin exits 2 with "refused" and creates no output directory. `--input` in commercial mode (the default) exits 2 and its stderr has no marker. [CODE-READ, 2d87833, test_privacy.py:93-105]
- `test_forced_exception_paths_do_not_echo_input`: injects three failures. (a) A per-line error that carries the input line: exit 0, parse errors counted, no marker in output or streams. (b) A fatal error at shutdown: exit 4, class name only, no Traceback. (c) A writer-thread OSError: no marker in output or streams. Case (c) does not check the exit code. [CODE-READ, 2d87833, test_privacy.py:113-135]
- `test_unknown_field_fails_closed_end_to_end`: injects a rogue cell_aggregates row with an `rnti` column and a rogue rx_grants row. Expect exit 3, 2 refused rows, no rx_grants directory and no marker. [CODE-READ, 2d87833, test_privacy.py:137-152]

### 8.2 test_core.py (related checks)

- `test_k_suppression_commercial`: 9 grants publish nothing. 10 grants publish cell_aggregates. [CODE-READ, 2d87833, test_core.py:168-179]
- `test_complementary_suppression`: one small group next to one large group hides the small one. [CODE-READ, 2d87833, test_core.py:181-189]
- `test_commercial_datasets_have_no_identifying_columns`: every commercial column is `agg` and none has `rnti` in its name. Lab-only datasets are not allowed in commercial. [CODE-READ, 2d87833, test_core.py:313-319]
- `test_fail_closed`: an rnti column, an enum value outside the list, an unknown dataset and a wrong type are each refused. [CODE-READ, 2d87833, test_core.py:321-331]
- `test_lab_dataset_refused_in_commercial_sink_direct`: a direct write of rx_grants to a commercial Sink is refused, and the counts are 1 row and 1 field. [CODE-READ, 2d87833, test_core.py:333-342]

### 8.3 What the tests do not check

- They do not run nr-uesoftmodem. The receiver binary is never tested here. [CODE-READ, 2d87833]
- They check only the listed markers. A different RNTI value is not caught. [CODE-READ, 2d87833, test_privacy.py:17-18]
- They do not check the discovery_summary small-count gap (section 5.4). [CODE-READ, 2d87833]
- They do not check run totals in summary.json (section 9). [CODE-READ, 2d87833]
- They do not check system_telemetry counters (section 9). [CODE-READ, 2d87833]
- They do not check any OS-level control in section 7. [CODE-READ, 2d87833]

## 9. Known limitations

1. Privacy relies on the operator not redirecting stdout or stderr. The exporter sees only what it is given. [CODE-READ, 2d87833, export.py:210-215] See section 3.4.
2. The receiver binary is unchanged. It still prints C-RNTIs and DCI payload hex to stdout. [CITED, audit §5.1, §5.3]
3. discovery_summary can write a small exact count (section 5.4). [KNOWN ISSUE, CODE-READ, 2d87833, agg.py:322-329]
4. summary.json run totals are not suppressed. These include grants, crc_ok, crc_fail, rescued_unique, the counts map, last_metrics and legacy_pdtim_real. Rates and latency percentiles are withheld below K. Run totals are one number per run, not per window. [KNOWN ISSUE, CODE-READ, 2d87833, agg.py:341-358]
5. system_telemetry carries cumulative cell-level counters per metrics line. They are not windowed and not K-suppressed. With few UEs, the change over time follows one UE's activity. [KNOWN ISSUE, CODE-READ, 2d87833, policy.py:43-57, agg.py:271-289]
6. t_mono_ns and t_real_ns are in system_telemetry. t_real_ns is the receiver's absolute wall-clock time at nanosecond resolution. manifest.json also has started_unix. These are not location. They can still be matched against outside records. [CODE-READ, 2d87833, policy.py:43-49, export.py:162]
7. Time is exporter arrival time, not event time (`--line-time arrival` is the default). Jitter can move a row into another window. This lowers precision. It is not a leak. [CODE-READ, 2d87833, export.py:65-66, 195; agg.py:99]
8. ue_bucket `1-9` can mean one UE (section 5.5). [CODE-READ, 2d87833, agg.py:33-34]
9. A latency bucket can hold 1 sample in a series with 10 or more samples (section 5.3). [CODE-READ, 2d87833, agg.py:319-321]
10. Complementary suppression covers only the one-group case per direction. Several hidden groups can be bounded by subtracting window totals. [CODE-READ, 2d87833, agg.py:295-302]
11. No geolocation is stored. No column holds a position, GPS fix or cell location. The manifest host_label is free text and is not checked, so an operator can type a location in it. [CODE-READ, 2d87833, policy.py:43-94, export.py:189]
12. host_label goes verbatim into manifest.json. [CODE-READ, 2d87833, export.py:163]
13. Enum coercion hides input errors (section 4.3). [CODE-READ, 2d87833, agg.py:50-51]
14. summary.json counter keys come from parser kind names. parse.py was not read for this file. Whether those names are a fixed set is not verified. [UNCHECKED]
15. The exporter cannot check that a run is approved (section 1.1). [CODE-READ, 2d87833]
16. The tests check listed markers only (section 8.3). [CODE-READ, 2d87833]
17. Audit facts about the receiver come from commit 3f0d921. They were not re-checked at HEAD 2d87833. The receiver may have changed. [CITED, audit §0]
18. The GPU dump names, the dashboard switch and the monitor.py start-up path are not verified here. [UNCHECKED]
