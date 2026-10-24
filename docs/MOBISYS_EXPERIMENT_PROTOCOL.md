# MobiSys experiment protocol: lab and commercial runs of the passive receiver

Date: 2026-10-20. Status: protocol only. No experiment in this document has been run.

## 0. Scope, sources, and labels

- Sources, the only evidence used here:
  - `tools/mobisys/README.md`, and the argparse code in `tools/mobisys/export.py`, `groundtruth.py`, `plots.py`, `make_synthetic.py`.
  - `tools/mobisys/policy.py` and `tools/mobisys/store.py` (dataset and output layout).
  - `tests/passive_rx/campaign/campaign.py` and `verdict.py` (run layout, verdicts).
  - `tests/passive_rx/dgx/rfsim_regress.sh` (4-RX regression gate).
  - `docs/MOBISYS_TELEMETRY_AUDIT.md` (existing metrics, privacy, gaps).
  - `CLAUDE.md` (hard rules).
  - `PROJECT_MEMORY.md` section 0.1 (evidence labels) and section 12 (gates). The rest of `PROJECT_MEMORY.md` was not read for this document.
- Labels. Each claim carries one label.
  - `[CODE-READ]`: read in the source file named next to it. Not run.
  - `[PROTOCOL]`: a rule this document sets. It is not a measurement.
  - `[PLANNED]`: no code exists yet, or the audit lists it as a plan.
  - `[UNCHECKED]`: the sources do not say.
  - Results use the PROJECT_MEMORY section 0.1 labels, for example `[MEASURED, DGX rfsim 106 PRB 4 RX, <commit>]`. No such result exists yet.
- Host names. Every result names its host, for example `DGX aarch64` or `cloud x86`. Never merge results across hosts, cells, bandwidths, or commits.

## 1. Purpose: the four evaluation questions

1. Accuracy. How well does the receiver recover control (DCI) and data (TB) from the air? Measure CRC pass rate, DCI accepts, and, in lab only, agreement with gNB MAC ground truth.
2. Speed and autonomy. How fast and how reliably does the receiver discover the cell and the UE RNTIs with no operator hints? Measure cold-start time to sync, SIB1, and first converged context, and count the failures.
3. Sustainable real-time throughput. Does the receiver keep up for 30 to 60 minutes? Measure queue drops, decode latency, and CPU and memory.
4. Commercial generalization within privacy limits. Does the receiver work on commercial cells? Only aggregate outputs are kept. No ground truth is available, so no recall is claimed.

## 2. Common setup

- Host profile. Each session records the host profile in its manifest. The campaign manifest has these fields: `hostname`, `arch`, `kernel`, `uhd_version`, `lscpu`, `nvidia`, `cmdline`, `ulimit_r` (`[CODE-READ]`, `campaign.py` `cmd_new`).
- Software commit. Each session records `git_commit`, `git_branch`, and `git_dirty` (`[CODE-READ]`, `campaign.py`). Record the commit of each build arm.
- Frozen-data gate. Before any run that touches sens6 material, run:

  ```
  cd /home/user/passive_agnostic_rx_for_sensing
  git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
  ```

  `campaign.py` records the result as `sens6_frozen_ok` in the manifest (`[CODE-READ]`).
- Idle host and lock. Run every measured session on an idle host, under the exclusive lock. `[PROTOCOL]` (CLAUDE.md, PROJECT_MEMORY section 12):

  ```
  flock -x /tmp/td_measure.lock <command>
  ```

  `campaign.py run` does not take this lock (`[CODE-READ]`). The operator wraps it.
- Build hygiene. Never build while `nr-uesoftmodem` runs. `[PROTOCOL]` (CLAUDE.md):

  ```
  if pgrep -x nr-uesoftmodem >/dev/null; then echo "receiver running, do not build"; exit 1; fi
  ```

  DGX (aarch64), from `cmake_targets/ran_build/build`:

  ```
  ninja nr-uesoftmodem nr-uesoftmodem-optional-modules oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4
  ```

  Cloud x86, from the same build directory, with `-DOAI_USRP=OFF` set at configure time:

  ```
  ninja nr-uesoftmodem nr-uesoftmodem-optional-modules rfsimulator params_libconfig nr-softmodem tests && ctest -j4
  ```

  sens6 (x86 with a discrete GPU, CUDA 12.4, gcc 15), configured with `-DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON`, then `ninja -j4 nr-uesoftmodem nr-uesoftmodem-optional-modules rfsimulator params_libconfig nr-softmodem tests` and `ctest -j1`. Label the build note with `sens6` and the GPU tier.

  Known intermittent ARM ctest failures: `dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam256`, `test_nr_pusch_ra0_qam64` (CLAUDE.md). Record them in the build note.
- Same IQ input for A/B. Compare two builds on the same input only. The IQ replay inputs are `ISAC_PASSIVE_REPLAY_INPUT` and `ISAC_RAW_IQ_*` (`[CODE-READ, audit section 2.5]`).
- Stop with SIGINT. Stop every receiver with `kill -INT <pid>`. Never send SIGKILL. `[PROTOCOL]` (CLAUDE.md).
- Session. One session is one receiver process, from start to SIGINT stop. Sessions and days are the experimental units. Runs inside a session are not independent.
- Feature flags. `ISAC_RX_BRANCH_FO` stays unset. `[PROTOCOL]` (CLAUDE.md).
- Gate for regression. Before a lab arm counts as a valid build, run the 4-RX gate. `[PROTOCOL]` (CLAUDE.md):

  ```
  flock -x /tmp/td_measure.lock env OUT=/path/to/gate_out tests/passive_rx/dgx/rfsim_regress.sh 2
  ```

  The script takes N as `$1` (default 2). It runs 420 s arms with `--ue-nb-ant-rx 4` by default (`[CODE-READ]`). The gate is `GATE_MODE=postconv` by default. Its limits are on the PASS/FAIL line. Its values are provisional (`[CODE-READ, rfsim_regress.sh header]`). A gate run is a SIM result (rfsim). It is an upper bound, not OTA.

## 3. Controlled lab procedure

### 3.1 Test bed

- Test gNB. Use an authorized test gNB only. Either an OAI rfsim gNB, or an X410 OTA lab cell. Record the bed in the manifest notes (`campaign.py new --notes`).
- Test UEs. Run 1, 2, or 4 test UEs. No tool flag selects the UE count. Record it in the notes. `[UNCHECKED]` how the UE count is set on the test bed.
- Ground truth. The gNB must run with the existing option `--log_config.nr_mac_log_level debug`. The gNB log holds the MAC scheduler decisions (`[DLSCH/PDSCH/PUCCH]` lines). This is what the MAC scheduled, not what the PHY transmitted (`[CODE-READ, groundtruth.py docstring]`). Mismatches can be gNB-side non-transmissions. Use gNB logs and configs as validation ground truth only, never as receiver input (CLAUDE.md).

### 3.2 Run directory

Create one campaign per site, cell, and day:

```
python3 tests/passive_rx/campaign/campaign.py new --name NAME --site SITE --cell CELL --notes "NOTES" [--root DIR]
```

`new` prints the campaign directory. Its name starts with the date (`YYYYMMDD_NAME`). `--root` overrides the default root set in `campaign.py` (`[CODE-READ]`).

Run one arm:

```
python3 tests/passive_rx/campaign/campaign.py run CAMPAIGN_DIR --arm ARM --secs SECS [--nic IFACE] [--expect-sib1] -- COMMAND ARGS...
```

- `--arm` names the build or ablation (for example `merged`, `frozen`, `abl_<name>`).
- `--secs` is the planned window. The runner sends SIGINT at `--secs` plus `CAMPAIGN_GRACE_S` (default 30 s). The receiver is not told `--secs`. Use `wall_s` in `run.json` as the real duration (`[CODE-READ]`).
- `--nic` samples `rx_missed_errors` and `rx_packets` once per second into `nic.csv`.
- `--expect-sib1` makes a run with no SIB1 `VOID_NO_SIB1`.
- Everything after `--` is the child command. The receiver stdout is written to `rx.log` in the run directory.

Summarize the campaign:

```
python3 tests/passive_rx/campaign/campaign.py summarize CAMPAIGN_DIR
```

This writes `summary.json` and `summary.md` in the campaign directory (`[CODE-READ]`).

### 3.3 Export, ground truth, and plots

Lab export reads the receiver stdout as a pipe. The exporter accepts a pipe or a regular file in lab mode (`[CODE-READ, export.py]`). A live run is one command. `tee` keeps `rx.log` for the verdict and feeds the exporter:

```
flock -x /tmp/td_measure.lock python3 tests/passive_rx/campaign/campaign.py run CAMPAIGN_DIR --arm ARM --secs SECS -- bash -c 'nr-uesoftmodem RECEIVER_OPTIONS 2>&1 | tee >(python3 tools/mobisys/export.py --experiment-mode lab --out EXPORT_DIR --host-label "DGX aarch64")'
```

`RECEIVER_OPTIONS` is the operator's receiver command line. This protocol does not list it. `[PROTOCOL]`. `EXPORT_DIR` is a new directory per run, for example under the run directory. `[PROTOCOL]`.

Export options, as found in `export.py` `build_parser()`:

```
python3 tools/mobisys/export.py --experiment-mode {lab,commercial} --out DIR [--input FILE] [--provenance {live,replay,synthetic}] [--host-label LABEL] [--format {auto,parquet,jsonl}] [--rotate-rows 50000] [--flush-s 10.0] [--queue-size 100000] [--slots-per-frame 20] [--line-time {arrival,unix}] [--no-sampler]
```

- `--host-label` defaults to `unlabelled`. Always pass it. `[PROTOCOL]`.
- `--input` is lab only. Commercial mode refuses it (exit 2).
- `--line-time unix` expects a `<unix seconds> ` prefix on each line. Use it for replay and synthetic input only (`[CODE-READ]` help text). `campaign.py` writes an elapsed-seconds prefix for lines that do not start with a digit (`_reader`). So `--line-time unix` on a campaign `rx.log` is wrong unless checked first. `[UNCHECKED]`.
- Exit codes: 0 ok, 2 input refused, 3 policy violation, 4 internal error (`tools/mobisys/README.md`).

Ground truth (lab only). Pass exactly one of `--gnb-log` and `--gnb-csv`. Passing neither is not checked by the code (`[CODE-READ]`: `read_gnb_csv(None)` would fail):

```
python3 tools/mobisys/groundtruth.py --rx LABDIR --gnb-log gnb.log [--out DIR]
python3 tools/mobisys/groundtruth.py --rx LABDIR --gnb-csv sched.csv [--out DIR]
```

The script refuses a non-lab export (`mode` in `manifest.json` must be `lab`). The CSV needs columns `frame,slot,rnti[,dir]`. The default output is `LABDIR/groundtruth/summary.json` and `matches.csv`.

Plots:

```
python3 tools/mobisys/plots.py --lab LABDIR --commercial COMDIR --out PLOTS [--sessions DIR DIR ...]
```

`--out` is required. `--lab` and `--commercial` are optional. `--sessions` plots cold-start across sessions (`cold_start_success`). Each figure is written as `<name>.csv`, `<name>.json`, and `<name>.pdf`, `.svg`, `.png` (`[CODE-READ, plots.py draw_all]`).

### 3.4 Lab figures and the runs that feed them

| Lab figure | Runs needed | Tool support |
|---|---|---|
| Latency CDF, merged vs frozen vs ablation | One arm per build, n >= 5 per arm, arms alternated A/B/A/B (PROJECT_MEMORY section 12) | `latency_cdf` (`latency_histograms`) |
| CRC by MCS | Sweep over MCS | `crc_by_mcs` |
| Grants over time | Sustained runs | `grants_timeline` |
| CPU and RSS | Every session | `cpu_rss` (`system_telemetry`) |
| Cold-start discovery | Repeated cold starts | `discovery_milestones`, `cold_start_success` |
| Ground-truth classes | Lab runs with gNB log | `groundtruth_classes` |
| Retry attribution | Any lab run | `retry_attribution` |
| SNR sweep | Sweep of gNB or channel SNR | No tool. `[PLANNED]` |
| MCS / TBS / PRB grid | Grid of gNB settings | No tool. `[PLANNED]` |

Notes:

- The SNR sweep and the MCS / TBS / PRB grid have no figure in `plots.py` (`FIGS`, `[CODE-READ]`). Any such figure is built outside the tools. Label it as analysis outside `tools/mobisys`.
- Sustained stability (30 to 60 minutes). Run `--secs 1800` to `--secs 3600`. Read queue drops (`scanq_drop_full`, `pdschq_drop_stale`) and `grants_timeline` (`[CODE-READ, audit section 1.4 and 1.11]`).
- Reconfiguration events. Use the `configuration_events` dataset and the `CONFIG_EPOCH` lines (`[CODE-READ, audit section 1.9]`). The sources do not give a way to trigger a reconfiguration on the test bed. `[UNCHECKED]`.
- Ablations. Use only existing build variants and environment variables. No new flag was added for this protocol (`[PLANNED]`: none). Record every `ISAC_`, `NR_`, and `LDPC` variable. `campaign.py` writes them to `env.txt` in each run directory (`[CODE-READ]`).
- Cold start with failures counted. `[PROTOCOL]`. Every cold start is one run. A run with `VOID_NO_SYNC` or `VOID_NO_SIB1` counts as a failed cold start. An `INTERRUPTED` run is reported and left out of the denominator. The count of failures goes in the report.

## 4. Commercial procedure

### 4.1 Authorization first

Do not start a commercial session until the following are in writing. `[PROTOCOL]`:

- Written permission from the cell operator or site owner to receive on that cell.
- The legal basis for collecting aggregate telemetry on that cell, reviewed by the project owner.
- A named scenario label for each session (see 4.4).

The sources do not describe these approvals. This list is a gap to close before any commercial run. `[UNCHECKED]`.

### 4.2 Operator checklist: what must be off

Check each item before the launch and record the check in the session notes. `[PROTOCOL]`, built from `[CODE-READ, audit section 2 and 5]`.

- Dashboard. The audit says the dashboard starts on every `--passive-rx` start and serves the raw log on `127.0.0.1:8080` (audit sections 2.9 and 5.13). The sources give no verified switch to turn it off. Do not start a commercial session until the operator confirms how it is disabled. `[UNCHECKED]`.
- Sensing API v2 (records and CFR). Off by default. Do not pass `--sensing.enable 1`, `--sensing.records_path`, or `--sensing.cfr_path` (audit section 2.4).
- Replay capture. Do not set `ISAC_PASSIVE_REPLAY_CAPTURE` (audit section 2.5).
- GPU dumps. Do not set `ISAC_PDSCH_GPU_DUMP`, `ISAC_PDCCH_GPU_DUMP`, or `ISAC_GPU_DUMP` (audit section 2.5, 5.11).
- Raw IQ input or capture. Do not set `ISAC_RAW_IQ_*` (audit section 2.5).
- Stdout redirection. Do not redirect the receiver to a file. The receiver stdout goes only into the exporter pipe. The exporter refuses regular files in commercial mode (`[CODE-READ, export.py]`).
- Core dumps off. Run with `ulimit -c 0` unless the operator sets `CORE` (`[CODE-READ, audit section 5.20]`). Check `ulimit -c` before launch.
- Campaign runner. Do not use `campaign.py run` in commercial mode. It writes `rx.log` to disk (`[CODE-READ]`).

### 4.3 Launch line

Launch line, from `tools/mobisys/README.md`:

```
nr-uesoftmodem RECEIVER_OPTIONS 2>&1 | python3 tools/mobisys/export.py --experiment-mode commercial --out COMDIR --host-label "DGX aarch64"
```

- `RECEIVER_OPTIONS` is the operator's command line, minus the checklist items in 4.2.
- `--experiment-mode commercial` is the default. Pass it anyway. `[PROTOCOL]`.
- Commercial mode keeps 60-s windows and k-suppression at K=10 (`policy.py`, `agg.py`, `[CODE-READ]`).
- Exit 3 means a policy violation: a row was refused. A session with exit 3 is not valid (section 6). Exit 4 is an internal error. Exit 2 means input refused.
- Stop the session with SIGINT. The exporter handles SIGTERM and KeyboardInterrupt and writes `summary.json` and `manifest.json` on finish (`[CODE-READ, export.py finish]`).

### 4.4 Per-cell sessions and scenario labels

- One session per cell per day. Sessions on different days are the experimental units.
- The scenario label is not a flag. `export.py` has no scenario option (`[CODE-READ]`). Keep the label in the operator notes next to `COMDIR`, not in the exported data. `[PROTOCOL]`. This is a gap: no tool stores the label in the output.
- Each session notes the host label, the commit, and the checklist result (4.2).

### 4.5 What commercial data can and cannot claim

- Commercial output is aggregate only. It has `system_telemetry`, `latency_histograms`, `cell_aggregates`, and `discovery_summary` (`policy.py`, `[CODE-READ]`).
- There is no ground truth. `groundtruth.py` refuses non-lab exports (`[CODE-READ]`). Never claim recall, precision, or any accuracy against the air.
- Claims are about the receiver output only: decode rate, latency, stability, and cold-start success on that cell, on that day.

## 5. Instrumentation overhead procedure (deferred)

Status: `[PLANNED]`. Not run. Needs the DGX (audit section 7.10). No overhead number exists in the sources (audit section 6: "No overhead number in this section is measured").

Method. `[PROTOCOL]`.

1. Fix one IQ replay input. Use `ISAC_PASSIVE_REPLAY_INPUT` or `ISAC_RAW_IQ_*` (read only).
2. Build the before arm (commit A, without the planned telemetry change) and the after arm (commit B, with it). Build each only when no receiver runs.
3. Run both arms on the same replay, alternated A/B/A/B, under `flock -x /tmp/td_measure.lock`, with an idle host (load check before each run).
4. Compare:
   - grant throughput (grants per second, from `rx_grants` or `EVMGRANT` counts);
   - end-to-end latency p50 and p99 (from `latency_histograms`);
   - CPU percent and max RSS (from `score_rx.py` `cpu_pct`, `max_rss_kb` via `/usr/bin/time -v`, or from `system_telemetry`);
   - queue stability (`scanq_drop_full`, `pdschq_drop_stale`, `scanq_max_lag`).
5. Target: overhead under 5 % on each metric. Report the number with its host, bed, and commit label, for example `[MEASURED, DGX replay, <commit A> vs <commit B>]`.

Exporter benchmark. `tools/mobisys/test_bench.py` measures exporter lines per second on synthetic lines with a floor of 2000 lines per second (`[CODE-READ]`). Those numbers are `cloud x86` and `SYNTHETIC` only. They say nothing about the receiver, the DGX, or real overhead. Label them `[SYNTHETIC, cloud x86]`.

Also unmeasured: the cost of the PDTIM clock reads and the dlsym calls in the metrics collector (audit section 6.1, `[UNCHECKED]`).

## 6. Validity and voiding rules

### 6.1 Campaign verdicts (lab runs)

`verdict.py` gives one verdict per run (`[CODE-READ]`). Use these names only:

| Verdict | Rule in `verdict.py` |
|---|---|
| `VALID` | None of the rules below apply |
| `INTERRUPTED` | `run.json` status is `interrupted` (runner got SIGINT/SIGTERM/SIGHUP), or `running` (runner died) |
| `VOID_RFSTALL` | The receiver log matches `RFSTALL`, `RFTSDISC`, `RXDISCONT`, `rx xport timed out`, `No CHDR`, `device_init failed`, or `Assertion` |
| `VOID_NIC_LOSS` | NIC `rx_missed_errors` increased during the run (needs `--nic`) |
| `VOID_NO_SYNC` | No `Initial sync successful` line |
| `VOID_NO_SIB1` | `--expect-sib1` was set and no `SIB1 decoded` line exists |

Rules added by this protocol. `[PROTOCOL]`:

- A VOID run is reported, not dropped. This follows PROJECT_MEMORY section 12 ("VOID runs reported not discarded silently"). The report lists every VOID with its reason.
- A run with `escalation_stage` above 1 in `run.json` is reported. Stage 1 is SIGINT only. Stage 2 means SIGTERM was needed. Stage 3 means SIGKILL was sent, which breaks the CLAUDE.md rule. `campaign.py` does not void these runs (`[CODE-READ]`).
- A lab arm needs n >= 5 runs (PROJECT_MEMORY section 12). Arms alternate.
- Compare arms only inside one host, bed, bandwidth, cell, and commit set.

### 6.2 Commercial sessions

- `verdict.py` reads `rx.log`. A commercial session writes no `rx.log`, so `verdict.py` does not apply (`[CODE-READ]`). `[UNCHECKED]` whether the exporter detects receiver faults at all.
- A commercial session is `VOID_EXPORT_POLICY` if the exporter exit code is 3. It is `VOID_EXPORT_INTERNAL` if the exit code is 4. It is `INVALID_INPUT` if the exit code is 2. These three names are proposed here. `[PROTOCOL]`. They are not in `verdict.py`.
- A session stopped with anything but SIGINT is `INTERRUPTED` (`[PROTOCOL]`).
- A checklist item (section 4.2) that was not off makes the session void. `[PROTOCOL]`.

### 6.3 Run-level rules that never change

- Never merge a VOID or INTERRUPTED run into a VALID summary.
- Never merge results across hosts, cells, bandwidths, or commits (CLAUDE.md).
- Synthetic fixtures (`make_synthetic.py`, provenance `synthetic`) are never data. They carry the SYNTHETIC watermark (`plots.py`) and are for tests only.

## 7. Data handling and outputs layout

### 7.1 Campaign tree (lab)

From `campaign.py` (`[CODE-READ]`):

```
CAMPAIGN_DIR/
  manifest.json                 host, commit, dirty flag, frozen gate, notes, cmdline
  index.jsonl                   one line per run: run, arm, verdict, score
  summary.json                  per arm: n, valid, crc_pct, ttc_s, drop_full_pct, verdicts
  summary.md                    the same as a table
  runs/NNN_ARM/
    cmd.txt                     child command
    env.txt                     ISAC_, NR_, LDPC variables
    run.json                    status, rc, timed_out, escalation_stage, wall_s
    rx.log                      receiver stdout (raw, lab only)
    metrics.jsonl               ISAC_METRICS_PATH output
    obs.jsonl                   ISAC_OBS_PATH output (the obs writer is not used, audit section 1.12)
    nic.csv                     only with --nic
    verdict.json                verdict, reasons, score
```

### 7.2 Export tree

From `store.py` and `export.py` (`[CODE-READ]`):

```
EXPORT_DIR/
  manifest.json                 mode, provenance, host_label, format, slots_per_frame
  summary.json                  written on finish
  DATASET/part-NNNNN.parquet    or .jsonl; one folder per allow-listed dataset
  .tmp/                         staging, same filesystem; removed on finish
```

Lab datasets include `rx_grants`, `dci_attempts`, `discovery_events`, `configuration_events`, `retry_events`, `slot_health`. Commercial datasets are only `system_telemetry`, `latency_histograms`, `cell_aggregates`, and `discovery_summary` (`policy.py`).

### 7.3 Ground truth and plots

```
LABDIR/groundtruth/summary.json    gNB grants, tp/fp/fn counts, Wilson intervals, provenance
LABDIR/groundtruth/matches.csv     per-class matches
PLOTS/<figure>.csv|.json|.pdf|.svg|.png
PLOTS/cold_start_success.*         only with --sessions
```

Each figure JSON records the source, mode, provenance, host label, data format, and matplotlib version (`plots.py`, `[CODE-READ]`). Keep the figure JSON with the figure.

### 7.4 Privacy

- Lab raw logs hold RNTIs, DCI payload hex, and cell identity (audit sections 1.3, 5). They stay on the lab host. Do not copy them off the host unless the project owner approves.
- Commercial output is aggregate and allow-listed only. Unknown datasets, columns, or string values are refused and counted (`policy.py`, `[CODE-READ]`). Exporter errors do not echo input in commercial mode (`[CODE-READ]`).
- Do not publish any artifact with RNTIs, cell identity, or payload bytes.
- The `rfsim_regress.sh` default output directory is under `/tmp`. Set `OUT` explicitly for each gate run.

## 8. What is not yet validated

- OTA. No OTA result is cited in this protocol. Any OTA claim needs its own log, date, and commit.
- DGX. The overhead procedure (section 5) has not run. The 4-RX gate values are provisional (`rfsim_regress.sh` header).
- The exporter and policy code. `tools/mobisys` has `test_core.py`, `test_privacy.py`, and `test_bench.py`. None of them was run for this document. The audit notes that the Python tests are run by hand, not by ctest (audit section 2 and section 8).
- GPU fields. The C code has no CPU or GPU utilisation sampler (audit section 3.5). The exporter sampler reads `/proc` (`export.py` `sampler`). No GPU utilisation value is validated. `[PLANNED]`.
- Dashboard switch-off (section 4.2). `[UNCHECKED]`.
- Commercial approvals and scenario labels (sections 4.1 and 4.4). `[UNCHECKED]`.
- SNR sweep and MCS / TBS / PRB grid figures (section 3.4). `[PLANNED]`.
- The `--line-time` prefix on campaign `rx.log` (section 3.3). `[UNCHECKED]`.
- How the UE count and reconfiguration events are set on the test bed (section 3.4). `[UNCHECKED]`.
- Sensing v2 (records and CFR) is off by default. Its effect on the receiver is not measured here (audit section 2.4).
