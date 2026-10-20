# DASHBOARD-20261013 — self-explaining receiver dashboard (`csd/dashboard`)

Branch `csd/dashboard` from `origin/adaptive-rx-UL-DL` (`d21811d5`). Not merged into `adaptive-rx-UL-DL` (waits for the user's permission).
Code: `tests/passive_rx/monitor/`. No receiver C code changed. Screenshots: `docs/superpowers/reports/csd/dashboard-20261013/`.

Evidence labels used here (PROJECT_MEMORY §0.1):
- `[OFFLINE VERIFIED, cloud x86 Xeon 4c, 2026-10-13]` — `test_monitor.py` + `test_health.py`, 46/46 pass.
- `[MEASURED, cloud x86 Xeon 4c, synthetic fixtures, 2026-10-13]` — parser cost and dashboard CPU against `make_fixtures.py` data.
- Every number on a fixture screenshot is `[SYNTHETIC FIXTURE, make_fixtures.py, not a receiver result]` (the label the page itself shows).
- **Not done:** the rfsimulator run feeding live mode. The cloud build stopped at the SIMDE headers (the distro `libsimde-dev` is
  incompatible with `tools_defs.h`; fetching the pinned SIMDE commit was refused by the session's permission policy). Live mode was
  validated instead with a fake receiver that streams a fixture into a run directory (`make_fixtures.py stream`). No `[SIM VERIFIED]`
  and no DGX/OTA claim is made.

## 1. How to run it

```
cd tests/passive_rx/monitor
python3 monitor.py --run RUN_DIR [--offline] [--runs-root 'GLOB' ...] [--scan cells.json ...] [--label '[...]'] [--port 8080] [--bind 127.0.0.1]
python3 monitor.py --follow '/home/nicola/NICOLA/ota_runs/*'        # always the newest run directory (live campaigns)
python3 monitor.py --log run.log --metrics metrics.jsonl --obs obs.jsonl [--records rec.jsonl --cfr cfr.bin]
```

A run directory is discovered by file name: `run.log` | `rx.log` | `*.log[.gz]`, `metrics.jsonl`, `obs.jsonl`, `records.jsonl` | `*records*.jsonl`,
`cfr.bin` | `*.nrcf`, `cmd.txt`, `env.txt` | `*.env` (single-line OTA env), `label.txt` (evidence label, first line), `cells.json` (scanner).
Python 3 standard library only; one HTML file with plain JS and inline SVG charts (no chart library, nothing loaded from the network,
works offline on aarch64). Binds 127.0.0.1 by default; open it through `ssh -L 8080:localhost:8080 <host>`.

Read-only contract: the server only opens files for reading, never writes into a run directory (tested: `Fixtures.test_read_only`), never
signals a process, and has no endpoint that changes anything. Runs are addressed by id; only the primary run and directories matched by
`--runs-root` are reachable (an unknown id is a 404, tested).

## 2. Pages: what each shows and from which data

| Page | Content | Data (first available wins) |
|---|---|---|
| **Home** | Six tiles (Radio, Sync, Downlink, Uplink, Decode health, Compute), each with value, colour **plus** icon and word (OK / Check / Problem / No data), one plain sentence and a link to the page that explains it; sync chain Searching → PSS/SSS → MIB → SIB1 → CORESET → RNTIs → DL converged → UL locked with the time each was reached; DL CRC-OK and decoded/s over time; event timeline (sync gained/lost, state changes, reconfiguration, new RNTI, CORESET verified/stale, CSI-RS confirmed, UL lock, stalls, breaker trips); alert log; one-click run summary (markdown with the run's evidence label). | all sources below |
| **Downlink** | PDCCH: occasions, candidates, accepted DCIs, per RNTI class, held-back counters, DCIs/s; table per CORESET / search space / scrambling id with how and evidence state. PDSCH: grants, CRC-OK, grants/s, throughput recovered (TBS × CRC-OK / s), RNTI count; CRC-OK by MCS bucket (0-4, 5-9, 10-16, 17-27) and by layers (Nl 1-4) with n; MCS histogram; modulation mix and MCS table; per-RNTI table (grants, CRC-OK, MCS buckets, layers, PRB mean, modulation, SNR, throughput, last seen); decode latency p50/p90/p99/max (+source) and over time against the target line; decode-time histogram; drops by queue (scan vs PDSCH) with full/stale % and max lag, PDSCH drop reasons, drops over time; recovery/retry share; the receiver's own per-RNTI census. | `ISAC_METRICS`; `obs.jsonl`; `EVMGRANT`; summary / `PDSCHQ` / `PDTIM*` lines |
| **Uplink** | Lock state badge and the reason in words (not synchronised / no UL DCI seen yet / searching: N votes over H hypotheses, BWP start unknown / provisional / locked via …); latest lock statistics; DCI 0_1 length; PUSCH attempts vs CRC-OK and over time; UCI trials/rescued; per-RNTI UL grants; PRACH/SRS activity. | `ULLAYOUT *` lines, `pusch_passive[...]`, `PUSCHQ`, metrics `pusch_*`, `obs.jsonl` dir UL, sense records |
| **Sensing export** | Banner "raw export, no sensing algorithm runs in the receiver"; record count and rate; per record type and per source (SSB/CSI-RS/DM-RS/data/PRACH/SRS) records and CFR frames; frame-id gaps; export health (`sense_*` drops, I/O errors, no-sink); session record; CFR viewer (magnitude or phase vs absolute subcarrier per antenna, antenna toggles) and a magnitude heat map over the last 64 frames of the chosen source; CSI-RS resources (row, ports, density, period, offset, first symbol, slots to confirm) and the log confirmations. | `records_path` (schema 2), `cfr_path` (NRCF v1, strided to 256 points), metrics `sense_*`, `csirs_*` |
| **Cell information** | Cell selector (one entry per PCI seen, labelled "PCI p @ f MHz"; never pooled); per item value, **how it was obtained** and evidence state (confirmed / provisional / unknown): PCI, PLMN, NCI, TAC, tuning centre, SSB frequency, GSCN, SSB offset, k_SSB, CFO, SCS, carrier N_RB / centre / Point A / end / verdict, SIB1 DL/UL bandwidth, TDD pattern, barred flag, DM-RS type-A position, CORESET#0 index and geometry, SS#0, dedicated CORESETs, search spaces, PDCCH scrambling ids, DCI lengths, CSI-RS resources, PDSCH DM-RS scrambling ids and MCS table of CRC-OK grants, UL BWP; table of Technique D convergences. | log lines in §3 |
| **Settings** | Refresh period, latency unit, theme, which Home tiles, alert thresholds (p99 target 0.5 ms, stale 10 %, scan full 2.5 %, CRC amber/red 80/50 %, GPU 95 %, silence 60 s, RAM 16 GB, 80 °C); stored in the browser only. Read-only receiver configuration: key arguments (`-C`, `--ssb`, `-r`, gain, `--initial-fo`, …), full command line, config file (shown when readable), `ISAC_*` environment grouped by area with a one-line description per flag ("unknown" when none was found or the flag is not read by this tree). | `CMDLINE:` log line, `cmd.txt`, `/proc/<pid>/cmdline`; `env.txt` / `*.env`, `/proc/<pid>/environ`; `isac_flags.json` |
| Performance | Decode stage split (FEP, channel estimation, demodulation, LDPC: wall/CPU mean, p50/p90/p99, max); real/retry/probe classes with > 5 / > 20 ms bodies; queue lag and depths; queue-stage latency histogram; GPU counters and GPU budget over time; host: receiver CPU, per-thread CPU, temperature, free RAM, the dashboard's own CPU (live only). | `PDTIM`, `PDTIMPCT`, `PDTIMCLS`, metrics, `/proc`, `/sys` |
| Spectrum & scan | Cells found by the blind scanner (PCI, SSB MHz, GSCN, metric, SSS / DM-RS ratios, CFO, SIB1 status in this run); per-antenna power bars and over time; X410 channel table; RF stalls; PBCH census. | `--scan cells.json` (fastscan/live_chain), `CHPOW`, `RFCHAN`, `RFSTALL`, `RFCENSUS` |
| Run history & compare | Run list (`--runs-root`), pick ≥ 2: side-by-side KPIs (cell, stage, duration, DL n and CRC-OK, CRC-OK per MCS bucket with n, stale %, p50/p99 and their source, PUSCH, RNTIs, sync losses, stalls) with each run's evidence label in its column header; grouped bar chart; JSON export. | full parse of each run (cached, 6 runs) |
| Logs | Regex search, key-lines filter (same prefixes as the parser), highlighting, download of the log. Searches the last 16 MB. | the run's log |
| Glossary | Plain-language terms (PCI, SSB, GSCN, MIB, SIB1, PLMN, CORESET, search space, DCI, RNTI, PDSCH/PUSCH, MCS, layers, CRC, HARQ, TBS, stale/full drop, CFR, CSI-RS, DM-RS, BWP, TDD, Technique D, evidence labels, dBFS) and every metric help text. | — |

Everywhere: an (i) button on every metric (what, unit, good/bad range, source; hover, focus or tap), empty states that say what is missing and
why ("No PDSCH yet: waiting for an RNTI to be solved", "needs ISAC_METRICS=1", …), CSV/JSON export of every table, PNG export of every chart,
light/dark (auto from the OS, or forced in Settings / header), works at 1280 px and at phone width (390 px screenshots).
Alerts: banner + session log for sync lost, RF stall, p99 above target, stale above threshold, scan full drops, GPU budget exhausted, LDPC CUDA
breaker trip, receiver silent, RAM floor, temperature. Replay: any finished run, slider + play at 1×/10×/60× (server re-parses up to t).

## 3. What the dashboard depends on (do not change these silently)

Parsers: `tests/passive_rx/monitor/rxparse.py` (`RX` table); each has a unit test with the line built from the C format string
(`test_monitor.py`, file named per line). Changing a format string below must come with a fixture/test update.

| Log line prefix (after `SENSING: ` unless noted) | Source file | Used for |
|---|---|---|
| `Initial sync successful, PCI: %d` (no prefix) | `nr_initial_sync.c` | sync gained, current cell |
| `Cell Detected with GSCN: %d, SSB SC offset: %d, SSB Ref: %lf, PSS Corr peak: %d dB, PSS Corr Average: %d` | `nr_initial_sync.c` | GSCN, SSB offset/frequency |
| `[UE %d] Measured Carrier Frequency offset %d Hz` | `nr_initial_sync.c` | CFO |
| `MIB fields sfn= k_ssb= scs_common= coreset0= ss0= dmrs_typeA= barred=` | `nr_ue_procedures.c` | MIB facts |
| `SIB1 decoded` (NR_RRC) and `SIB1 CELL mcc= mnc= cell_identity= tac= n_plmn=` | `rrc_UE.c` | SIB1 stage, PLMN, NCI, TAC |
| `PASSIVE: cell config extracted from SIB1 -- PCI= DL: N PRB @ SCS= offsetToPointA= UL: …` | `config_ue.c` | SIB1 bandwidths, SCS |
| `ACQ carrier CONFIRMED from SIB1: …derived carrier centre %f MHz`, `… CONFIRMED without SIB1 …`, `… MISMATCH: SIB1 says …`, `… HYPOTHESIS …` | `nr_passive_acq_state.c` | carrier geometry + verdict |
| `ACQ_STATE %s -> %s`, `ACQ_EVENT pbch_locked`, `TDD from SIB1 DERIVED|ABSENT…` | `nr_passive_acq_state.c`, `config_ue.c` | state machine, LOST, TDD |
| `CONFIG_EPOCH %u -> %u class=%s cause=%s` | `nr_passive_cfg_epoch.c` | reconfiguration events |
| `ISAC_ACQ_RETUNE {…}` | acq | retune events |
| `CSS0 autoconf from MIB/SIB1 -- coreset(groups= dur= … shift= scramb=) bwp=[a..b) … ss(period= offset= dur= symb=)` | `nr_pdcch_blind_monitor.c` | CORESET#0, SS#0 |
| `multi-CORESET bank add index= offset= span= symbol= mapping= len=`, `CORESET bank STALE index=`, `CORESET bank REMOVED index=` | `nr_pdcch_coreset_bank.c` | dedicated CORESETs |
| `SEARCH_SPACE INFERRED [%s] … monitored AL={…} … total_confirmed=` | `nr_pdcch_blind_monitor_rt.c` | search spaces |
| `PDCCH_SCRAMBLING_ID CONFIRMED [%s] n_id=` | `nr_pdcch_blind_monitor_rt.c` | scrambling ids, RNTIs stage |
| `DCI 1_1 length locked coreset= rnti= len=`; `UL automatic DCI length locked: %d rnti=` (no prefix) | `nr_pdcch_blind_monitor_rt.c` | DCI lengths |
| `blind PDCCH monitor summary: occasions= candidates= accepts= dci10[…] dci01[…] dci00[…] … held[…] … scanq[…]` | `nr_pdcch_blind_monitor_rt.c` | PDCCH card, scan queue |
| `PDSCHQ queued= decoded= crc_ok= (…) dropped[full= stale=] max_lag_slots=a/b`, `PDSCHQ per-rnti 0x..:ok/dec(..%)` | `nr_pdcch_blind_monitor_rt.c`, `nr_pdsch_passive_queue.c` | queue (when no metrics), census |
| `PUSCHQ queued= decoded= crc_ok= dropped[full= stale=` | `nr_pusch_passive_monitor_rt.c` | UL queue |
| `LDPCDIAG ok= seg_fail= tb_fail= zero_tb=` | `nr_pdsch_passive_decode.c` | LDPC (when no metrics) |
| `pusch_passive[try= crc_ok= … uci[trials= rescued=` | `nr_pusch_passive_decode.c` | PUSCH, UCI |
| `EVMGRANT rnti=0x%04x f.s crc= mcs= tbl= Qm= Nl= rb=a+b … snr= … dec_us= … tbs=` | `nr_pdsch_passive_queue.c` | per-grant DL (fallback), decode-time histogram |
| `PDTIM calls= stage[n= mean=us cpu=us max=us …]`, `PDTIMPCT us stage[n= p50= p90= p99=] … sum[…]`, `PDTIMCLS us real|retry|probe[n= mean= p50= p90= p99= max= gt5ms= gt20ms=` | `nr_pdsch_passive_decode.c` | latency, stage split, classes |
| `ULLAYOUT LOCK len= bwp=n@s`, `ULLAYOUT BWP_LOCK bwp=n@s`, `ULLAYOUT_CRC_LOCK [RELOCK] len= bwp= … crc_ok= trials=`, `ULLAYOUT PRIOR_CONFIRM|ADOPT bwp=`, `ULLAYOUT STATS bwp=searching|PROVISIONAL|LOCKED n@s probes[booked= fed=] votes= … hyps=`, `ULLAYOUT reset:` | `nr_pdcch_ul_layout_lock.c` | UL lock state and reason |
| `Technique D CONVERGED rnti= tda= S= L= mask= table=`; `SWEEP: new per-RNTI context rnti=` (no prefix) | §11.8, `nr_pdsch_config_sweep.c` | DL converged, solver table |
| `CSIRS_BLIND CONFIRMED #%d after %llu slots -- csirs_monitor = "…"` | `nr_csirs_blind_rt.c` | CSI-RS events/resources |
| `CHPOW sample= dBFS chN=…` (needs `ISAC_USRP_CHPOW_S=N`), `RFCHAN chN … gain= … freq= MHz rate= Msps` (no prefix) | `usrp_lib.cpp` | antenna power, tuning |
| `RFSTALL …`, `RFCENSUS slots= ssb_slots= pbch_ok= pbch_fail=` | `usrp_lib.cpp`, `nr-ue.c`, `nr-ue-ru.c` | stalls, PBCH census |
| `ISAC_METRICS {json}` | `nr_passive_metrics.c` | metrics when no metrics.jsonl; **time anchors of the log** |
| `CMDLINE: "argv0" "arg" …` (stdout, no prefix) | `config_load_configmodule.c` | command line |
| `synch Failed` | upstream | sync attempt events |

`ISAC_METRICS` keys read (schema 1; unknown keys ignored, missing keys = n/a): `t_mono_ns`, `abs_slot`, `pci`, `acq_state`, `acq_sync_losses`,
`acq_pbch_locks`, `acq_sib1_decodes`, `pdcch_occasions`, `pdcch_candidates`, `pdcch_accepts`, `pdcch_accepts_c`, `scanq_{queued,processed,drop_full,
drop_stale,drop_epoch,max_lag}`, `pdschq_{queued,decoded,crc_ok,drop_full,drop_stale,drop_epoch,drop_narrow,stale_after_decode,max_lag}`,
`pdschq_reasons`, `pdschq_latency` (bounds 100…16000 µs), `ldpc_{ok,seg_fail,tb_fail,zero_tb,tb_cpu,tb_cuda}`, `ldpc_cuda_{breaker_trips,fallbacks}`,
`gpu_fep_selfcheck_fail`, `gpu_fep_disabled`, `gpu_budget_used_pct`, `rt_gpu_wait_max_us`, `rt_gpu_fallbacks`, `discovery_overruns`, `pusch_try`,
`pusch_crc_ok`, `sense_{cfr_drop,rec_drop,io_errors,cfr_no_sink,rec_count,write_count}`, `prach_drops`, `prach_skipped`, `srs_jobs_dropped`,
`csirs_{candidates,confirmed,revoked}`, `zp_{exported,revoked}`, `csirs_time_to_confirm_resources[]`.
Observation keys (schema 1, §21): `t_mono_ns`, `dir`, `rnti`, `pci`, `mcs`, `mcs_table`, `qm`, `nl`, `nb_rb`, `tbs`, `crc`, `snr_db`,
`dmrs_scrambling_id`, `carrier_hz`. Sensing records (schema 2): `schema`, `type`, `t_mono_ns`, `source`, session fields `rx_id n_rx rx_gain_db
carrier_dl_hz fs_hz scs_khz n_rb_dl build`. CFR: NRCF v1 header `<4sHHQBBHIQffII`, `k_abs u16`, `l_sym u8`, pad, float32 IQ (antenna-major).

Time: log lines carry no clock. The reader maps a log byte offset to time by interpolating between the `t_mono_ns` of consecutive
`ISAC_METRICS` lines (every 20 s); lines before the first snapshot get no time ("start" on the timeline) rather than a guess. Run time 0 = the
earliest `t_mono_ns` in any file. Without `ISAC_METRICS` there are no times and no time series (the page says so).

## 4. Validation

| Check | Result | Label |
|---|---|---|
| `python3 -m unittest test_monitor.py test_health.py` | 46 tests OK: every line parser in §3, metrics window/counter reset, histogram percentiles, tiles and alerts against thresholds (incl. host-only alerts), empty states, five fixtures through the real readers, replay ≡ full run at the end and holds future lines, live partial lines, CFR framing (good/partial/bad), read-only, env parsing, HTTP API end to end (runs, state with thresholds, other run + cell, replay, compare with labels, summary.md, log search, scan, flags, /health), unknown run id refused | `[OFFLINE VERIFIED, cloud x86 Xeon 4c, 2026-10-13]` |
| Fixtures (`make_fixtures.py build`) | healthy (all tiles OK, 0 alerts), sync_loss (RFSTALL then LOST, re-sync), stale_tails (38 % stale, p99 19 ms, scan full 6 %, GPU 99 %, breaker trip, UL provisional), ul_unlocked (UL searching), multi_cell (PCI 508 then PCI 4 on one frequency + 3-cell scan file; cells never pooled) | synthetic |
| Headless Chromium, every page × light/dark at 1280 px + Home/DL/Cell at 390 px, plus problem fixtures | 34 PNGs, 0 page-script errors (`screenshots.js` fails on any) | synthetic |
| Live mode with `make_fixtures.py stream` (fake receiver writing run.log/metrics/obs) | tiles and counters advance with the stream; 5× real time: dashboard 0.5-1.1 % of one core; 50× (≈6 k lines+records/s for 6 s) with a client polling every 0.5 s: 5.9 % | `[MEASURED, cloud x86, synthetic stream]` |
| Parser cost | unhandled line 0.3-0.5 µs, `EVMGRANT` line 6.2 µs, `obs.jsonl` record 5.6 µs (+ read/json), snapshot 0.3 ms, `/api/state` 5-9 ms / 186 kB | `[MEASURED, cloud x86, synthetic]` |
| rfsimulator run feeding live mode | **NOT RUN** (build blocked at SIMDE, see top) | — |

CPU budget estimate for the DGX (not measured there): cost ≈ 0.5 µs × log lines/s + 6 µs × EVMGRANT/s + 9 µs × obs records/s + 2.5 ms/s
for one open page at the 2 s refresh. At 10 k log lines/s with 1 k decodes/s and obs on, that is about 2-2.5 % of one core on this x86 core;
above 8 MB of backlog the live reader skips to the present and the page shows the skipped bytes. **Measure it on the DGX** (Performance
page "Dashboard CPU", or `pidstat -p <monitor pid> 10`).

## 5. Open gaps (KPIs the receiver does not export yet)

- `P99AGE` (job age / scan lag line) does not exist in this tree (`git grep P99AGE` is empty); the page uses `scanq_max_lag`, `pdschq_max_lag`
  and the T21 `pdschq_latency` histogram instead. If `P99AGE` lands, add a parser + fixture line.
- No clock on log lines: event times are 20 s-window interpolations (§3); events before the first metrics snapshot have no time.
  A `t_mono_ns` (or frame/slot) on `ACQ_STATE`, `CONFIG_EPOCH`, `multi-CORESET bank add`, `ULLAYOUT LOCK` and `RFSTALL` would make the timeline exact.
- `ISAC_METRICS` carries one `pci` and no frequency: multi-cell queue/PDCCH counters cannot be split per cell. Per-grant data is split by the
  obs `pci`; the cell key is "PCI @ receiver tuning centre". A per-cell metrics object (keyed by PCI + SSB frequency) is needed for N-cell runs.
- No per-CORESET / per-search-space DCI counters (only global accepts and the `SEARCH_SPACE INFERRED` census).
- `obs.jsonl` has no decode time; decode-time histograms need `EVMGRANT` lines (`ISAC_PDSCH_EVM_GRANT`, T26c; whether the current OTA env sets it was not checked here).
- PRACH/SRS activity only through the sensing export records; UCI only through the `pusch_passive` line.
- CPU per thread, temperature, RAM only from the host while live; finished runs have none (no `ISAC_METRICS` key for them).
- GPU utilisation itself (SM %) is not exported; only `gpu_budget_used_pct` and RT wait/fallback counters.
- Operator name: only MCC-MNC is shown (no operator constants in the dashboard by design).
- CFR frames are not time-mapped in replay (frames carry `abs_slot`); replay hides the CFR viewer.
- Flag catalogue: 591 `ISAC_*` literals, 442 with an automatically extracted description (`gen_flags.py`; code comments and PROJECT_MEMORY),
  149 shown as unknown. Descriptions are heuristic; re-run `python3 gen_flags.py > isac_flags.json` after flag changes.

Nothing here required a receiver change, so the metrics JSON schema was not touched.

## 6. Exact DGX commands

On the DGX (`spark-74c3`), from the worktree that has `csd/dashboard` checked out (or after the merge), as the normal user:

```
cd <worktree>/tests/passive_rx/monitor
python3 -m unittest test_monitor.py test_health.py                    # 46 tests, aarch64 check

# live, next to an OTA run (the OTA scripts write one directory per run):
python3 monitor.py --follow '/home/nicola/NICOLA/ota_runs/*' \
    --runs-root '/home/nicola/NICOLA/ota_runs/*' --scan '<live_chain OUT>/cells.json' --port 8080
#   laptop:  ssh -L 8080:localhost:8080 nicola@spark-74c3   then open http://localhost:8080/

# a finished run (post-mortem, replay, compare):
python3 monitor.py --run /home/nicola/NICOLA/ota_runs/<run> --offline --runs-root '/home/nicola/NICOLA/ota_runs/*' --port 8080
```

What the receiver run should have for full pages (all optional; every page explains what is missing): `ISAC_METRICS_PATH=<run>/metrics.jsonl`
(also enables the time series), `ISAC_OBS_PATH=<run>/obs.jsonl`, `ISAC_USRP_CHPOW_S=5` (per-antenna power), and, only if already part of the
chosen env, `ISAC_PDCCH_TIMING` / `ISAC_PDTIM_CLASS` (decode timing lines; these are diagnostics, do not add them to an A/B arm for the dashboard).
Put `label.txt` with the run's evidence label and `env.txt` (or the single-line env file) into each run directory. The dashboard does not need
`nr-uesoftmodem` to be restarted and never touches the X410; stopping the receiver stays SIGINT as always.

## 7. Handover note

Files (all under `tests/passive_rx/monitor/`): `monitor.py` (server, readers, HTTP API), `rxparse.py` (parsers, tiles, alerts, summary),
`monitor.html` (the page), `isac_flags.json` + `gen_flags.py` (flag catalogue), `make_fixtures.py` (synthetic runs + live stream),
`screenshots.js` (headless Chromium), `test_monitor.py` (rewritten), `test_health.py` (unchanged, still passes). Removed: `demo_scene.py`,
`paper_stats.py`, the ZeroMQ DetectionReport store and the tracking/AoA tabs (they served the sensing algorithms removed by `c7c6fa8bb3`;
`/state`, `/fast`, `/paper` are gone, `/health` is kept). Report and screenshots: this file and `dashboard-20261013/`.

Start: `python3 monitor.py --run <dir>` (or `--follow '<glob>'`), open the printed URL through an SSH tunnel.

Check on the DGX: (1) the unit tests on aarch64; (2) the dashboard CPU during a live 4-RX OTA run stays < 2 % of one core (Performance page,
"Dashboard CPU"); (3) the real OTA run directories match the discovery names (`run.log`, `metrics.jsonl`, `obs.jsonl`, `env.txt`, `label.txt`)
and are readable by the monitor user (root-owned receivers usually make `/proc/<pid>/environ` unreadable: keep `env.txt`); (4) the line formats
of `csd/otatest` / the OTA binary match §3 (this branch was built against `adaptive-rx-UL-DL` `d21811d5`); (5) one real run per cell type
(SIB1 cell, NSA-style SIB1, no SIB1) to see the Cell page states; (6) the rfsim live check that could not run here.
