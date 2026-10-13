You are working on the passive, fully blind, cell-agnostic 5G NR receiver (OAI UE fork) in this repository.
Your task: turn the existing receiver monitor into a CLEAR, USER-FRIENDLY, SELF-EXPLAINING dashboard that a person who
did not write the receiver can open and understand in one minute: what the radio sees, what is decoded, how well, and why not.

READ FIRST, completely: CLAUDE.md; PROJECT_MEMORY.md (single source of truth; §0.1 evidence labels, §21 observation API
schema v1, the 2026-10-12 and 2026-10-13 blocks in §25); tests/passive_rx/cloud_run_2026-10-01/a5_dashboard/README.txt.

STARTING POINT (it exists, do not rewrite from scratch)
- The monitor was deleted from adaptive-rx-UL-DL by c7c6fa8bb3 (sensing algorithms removed). Restore it from the parent:
  `git checkout c7c6fa8bb3^ -- tests/passive_rx/monitor/` (monitor.py = stdlib HTTP server + log/JSONL tails, monitor.html =
  single-page vanilla JS, test_health.py, test_monitor.py, paper_stats.py, demo_scene.py). Drop what depended on the removed
  sensing algorithms (tracking/range/Doppler/LOS tabs, demo_scene) unless it can be fed from the current sensing export.
- Existing tabs: Health, DL, UL, Tracking, DCI deep-dive, Paper stats. Endpoints: /, /state, /fast, /health, /paper.
- Data sources the receiver produces today (no new receiver code needed for most of the dashboard):
  1. metrics JSONL (ISAC_METRICS=1, writer openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c): acq_state, pci,
     acq_* counters, pdcch_*, scanq_* and pdschq_* (queued/decoded/crc_ok/drop_stale/drop_full/latency histogram/max_lag),
     pusch_crc_ok/pusch_try, ldpc_*, gpu_budget_used_pct, gpu_fep_*, csirs_* discovery counters, sense_* counters, cpu.
  2. per-grant observation JSONL (nr_passive_obs.c, schema v1, §21): rnti, slot, PRB, MCS, layers, CRC, DL/UL.
  3. run.log lines (stable prefixes, parse them, do not require new ones): "Initial sync successful", MIB/SIB1 lines,
     CSIRS_BLIND CONFIRMED, CHPOW (per-channel dBFS), PDTIM/PDTIMCLSST (decode timing), P99AGE (job age, scan lag),
     PDSCHQ per-RNTI, UL layout lock lines, RFSTALL, Technique D CONVERGED, SEARCH_SPACE INFERRED.
  4. sensing export (nr_sense_api.h / nr_sense_records.h): per-slot sensing records and CFR snapshots (records_path,
     cfr_path or endpoint). There is NO sensing algorithm in the receiver anymore: the dashboard shows what is exported.
  5. offline: an OTA run directory (run.log + metrics.jsonl + obs.jsonl) given with --run <dir> for post-mortem viewing.

REQUIRED SECTIONS (left navigation or top tabs, in this order)
1. Home — one-glance status. Big traffic-light tiles: Radio (streaming, overruns/RFSTALL, CHPOW per antenna),
   Sync (state machine position: searching -> PSS/SSS -> MIB -> SIB1 -> CORESET -> RNTIs -> DL converged -> UL locked),
   DL (grants/s, CRC-OK %, decoded/s), UL (PUSCH CRC-OK %), Decode health (p50/p99 decode time vs the 0.5 ms target,
   stale %), Compute (GPU budget %, CPU, temperature if available). Each tile: value, colour, one plain sentence
   ("DL is decoding 92 % of grants it sees"), and a click-through to the section that explains it. A short event
   timeline (sync gained/lost, cell reconfiguration, new RNTI, CSI-RS confirmed, stalls).
2. DL — PDCCH: occasions, candidates, accepted DCIs, per-CORESET/search-space. PDSCH: per-RNTI table (grants, CRC-OK %,
   MCS histogram, layers Nl 1-4, PRB use, modulation), CRC-OK by MCS bucket (0-4, 5-9, 10-16, 17-27) and by layers,
   decode latency histogram + p50/p90/p99/max over time, stale/full drops and their reason (scan queue vs PDSCH queue),
   recovery/retry share. Throughput estimate (TBS * CRC-OK / s) per RNTI and total.
3. UL — layout lock state and how it was obtained, PUSCH tries vs CRC-OK, per-RNTI UL grants, MCS/PRB, PRACH/SRS
   activity, UCI. When UL is not locked, say why in words (e.g. "no UL DCI seen yet", "BWP start unknown").
4. Sensing — what the sensing export publishes: record rate, drops, CFR snapshot viewer (magnitude/phase vs subcarrier
   per antenna, heat map over time), per-source (SSB/CSI-RS/DM-RS/SRS) counts, CSI-RS resources confirmed and their
   periodicity/ports/density, export health (sense_rec_drop, sense_io_errors). Clearly marked: "raw export, no
   sensing algorithm runs in the receiver".
5. Cell information — everything learned blindly: PCI, PLMN/operator (from SIB1), ARFCN/centre frequency, SSB offset,
   N_RB/bandwidth, SCS, TDD pattern, CORESET#0 and dedicated CORESETs, search spaces, DM-RS/scrambling IDs found, MCS
   table, CSI-RS config, carrier edges. For each item show HOW it was obtained (MIB, SIB1, blind discovery, solver) and
   a confidence/evidence state (confirmed / provisional / unknown). Multi-cell ready: a cell selector (one cell today,
   N cells when the multi-PCI work lands; key everything by PCI + frequency).
6. Settings — dashboard preferences (refresh rate, units, theme light/dark, which tiles on Home, alert thresholds
   such as p99 target 0.5 ms, stale < 10 %, RAM floor 16 GB, temperature limit). A READ-ONLY view of the receiver
   configuration of the running/selected run: command line (-C, --ssb, -r, gain, initial-fo), config file, and the
   ISAC_* environment grouped by area with a one-line description of each flag (extract from the code/docs; mark
   unknown ones). The dashboard must NOT start/stop the radio or change receiver flags.

ADDITIONAL FEATURES (implement in this priority order, after the six sections work)
a. Explanations everywhere: an (i) tooltip on every metric (what it is, unit, good/bad range, where it comes from), a
   Glossary page (PCI, SSB, CORESET, DCI, RNTI, MCS, layers, CRC, HARQ, stale drop, CFR...), and empty states that
   explain ("no PDSCH yet: waiting for an RNTI to be solved") instead of blank charts.
b. Alerts: a banner + log when a threshold is crossed (sync lost, RFSTALL, p99 > target, stale > threshold, GPU budget
   exhausted, receiver silent > N s, LDPC CUDA breaker trip).
c. Performance/timing page: decode stage split (FEP, chest, demod, LDPC, recovery), P99AGE job age, scan lag, queue
   depths, GPU/CPU per thread, temperature, free RAM.
d. Run history and A/B compare: list run directories (e.g. ~/NICOLA/ota_runs/*), pick two or more, show the same KPIs
   side by side (CRC-OK by MCS bucket, stale %, p99), with n and the evidence label of each run.
e. Spectrum/scan page: cells found by the blind scanner (fastscan/live_chain output) with PCI, frequency, SSB power,
   SIB1 status; per-antenna power.
f. Logs page: filtered raw log with search and highlighting of the key lines; download.
g. Export: CSV/JSON of any table, PNG of any chart, a one-click "run summary" (markdown) with evidence labels.
h. Replay mode: play an offline run directory at 1x/10x with a time slider.

CONSTRAINTS
- Keep the stack: Python 3 standard library server (no Flask/Node build step), one HTML file + plain JS; at most one
  vendored chart library loaded from a pinned CDN version or vendored file. Must work offline on the DGX (aarch64).
- Read-only with respect to the receiver: it tails files/endpoints; never writes into run directories; bind to
  127.0.0.1 by default (--bind to change). Cheap: < 2 % of one CPU core while a receiver runs; never block the receiver.
- Receiver C code: avoid it. If a KPI truly needs a new field, add it additively to the metrics JSON writer (new key,
  same schema number bump rules as §21) and say so; no change to decode paths. AVOID the PDSCH demod/MMSE/MRC files,
  GPU discovery/NID sweep, PDCCH dwell code (actively edited on the DGX).
- Fully generic: no operator/PCI/frequency constants; everything learned from the data.
- Clarity: plain-language labels with the technical term in brackets, consistent units (dB, dBFS, µs/ms, %), colour
  never the only signal (icons/text too), works on a 1280 px laptop and a phone, light and dark theme.
- Git: branch `git checkout -b csd/dashboard origin/adaptive-rx-UL-DL`; never merge into adaptive-rx-UL-DL; git add explicit
  paths, never -A, never stash. sens6 is FROZEN (tests/passive_rx/captures/*, tests/passive_rx/*.conf,
  tests/passive_rx/sens6_host_snapshot_2026-09-30/*). New config files under tests/passive_rx/ use .cfg.
- Cloud host: no X410, no GPU, no OTA captures. Validate with: the unit tests (extend test_health.py/test_monitor.py:
  parsers for every log line and JSON key you use, threshold/alert logic, empty states), synthetic fixture run
  directories you generate (include one healthy run, one with sync loss, one with stale drops and fat tails, one
  with UL not locked, one multi-cell placeholder), the rfsimulator run (build per CLAUDE.md x86 variant) feeding the
  live mode, and headless Chromium screenshots of every page in both themes. Label evidence
  "[SIM VERIFIED, cloud x86 ...]" / "[MEASURED, cloud x86 ...]"; never claim DGX/OTA results.

OUTPUT
- Commits on csd/dashboard; tests passing; screenshots in docs/superpowers/reports/csd/dashboard-20261013/.
- Report docs/superpowers/reports/csd/DASHBOARD-<date>.md: what each page shows and from which data, the list of
  log lines/JSON keys it depends on (so receiver changes do not silently break it), open gaps (KPIs the receiver does
  not export yet), and the exact DGX commands to run it live next to an OTA run and on a finished run directory.
- A short handover note: files changed, how to start it, what must be checked on the DGX.
