# SOLVE — dedicated PDSCH reliability (2026-10-09)

## Status and evidence

The implementation is built and the focused solver test passes on the DGX aarch64 sandbox. **The requested replay
acceptance gate is unmeasured here.** This sandbox has no usable GPU device; the dedicated CORESET nID pass cannot
publish the CORESETs that feed the anonymous DCI 1_1 search. A common-PDSCH replay cannot establish dedicated RNTI
discovery, five solves, or post-solve CRC counts. No OTA or radio run was attempted. [OFFLINE VERIFIED build/test;
DEFERRED GPU MEASUREMENT for dedicated replay]

The first capability check used a shortened sample count and stopped at the raw-IQ loader's exact-file check after
GPU initialization had already reported unavailable. It is only evidence of the device boundary, not a valid capture
replay. The full-count replay `replays/replay_solve_b1_151039/run.log` reached `RAW_IQ_EOF`: 113 common PDSCH jobs,
85 CRC-OK, zero DCI 1_1 length locks and zero solver grants. Its PDCCH nID GPU initialization failed before sync.
These common CRCs do not count toward the dedicated acceptance gate. [OFFLINE VERIFIED replay raw_pci415_12s_b,
DGX aarch64 sandbox]

## Changes

* `nr_pdcch_blind_monitor_rt.c` carries parsed DCI 1_1 DM-RS ports, popcount-derived layer count, and each reading's
  MCS to the solver. `ISAC_PDSCH_SOLVER_SCR_CLEAN=1` sends only the decided non-PCI DM-RS id as an alternate PDSCH
  scrambling hypothesis; a decision equal to the PCI leaves the default identity alone. The old runner-ups 105/122
  and 490/390 had no CRC support and consumed two-thirds of the serving search after 48 grants. [CODE-READ; T26d
  replay measurement for the runner-ups]
* `nr_pdsch_solver.c/.h`: `ISAC_PDSCH_SOLVER_NL_PREF=32` uses only narrow readings that parse as one layer and QPSK
  during search and candidate confirmation. If no compatible reading appears for 32 grants, the next grant searches
  all narrow readings. The table variants and Qm are calculated from **that reading's** MCS. A preferred grant bypasses
  the old 200-probe backoff; the global probe admission budget still bounds work. When both passes discriminate MCS
  tables, confirmation also requires the same table. `ISAC_PDSCH_SOLVER_THREAD_CTX=1`
  isolates the per-grant CORESET key, scrambling candidates, and absolute slot for each scan consumer. The previous
  process-global setters could interleave across the two consumers and attach another grant's scrambling identity
  to a plan. [CODE-READ]
* `nr_pdcch_blind_monitor_rt.c`: the anonymous top-K selector read `sc[topk-1]` before writing it when the list first
  filled. This was an uninitialized-stack source of differing CCE choices on identical replays. Under
  `ISAC_PDCCH_JOINT_ANON_STABLE=1`, the selector waits until the list is full, then samples anonymous occasions and
  full-length refreshes by a hash of (CORESET nID, SFN, slot), increases top-K from six to eight and queue capacity
  from 64 to 256, and holds 4096 worker hits instead
  of 256 before overflow. This removes worker/order-dependent token-bucket sampling. The five-distinct-payload lock
  rule and joint-solver mismatch bar are unchanged. [CODE-READ]

All four new flags default off. The legacy top-K decision is preserved when the stable flag is off. No GPU, PDSCH FEP,
channel-estimation, `nr_pdsch_passive_decode.c`, or `nr_dlsch_demodulation.c` file was edited. [CODE-READ]

## Validation

| Check | Result | Evidence |
|---|---|---|
| RelWithDebInfo receiver and solver test build, CUDA sm_121 modules | pass | [OFFLINE VERIFIED, DGX aarch64 sandbox] `heavy.sh ninja -C cmake_targets/ran_build/build_solve -j6 nr-uesoftmodem test_nr_pdsch_solver raw_iq params_libconfig pdcch_nid_gpu csirs_disc_gpu ldpc_cuda`; ccache read-only, CPM cache `/tmp/csd-solve-cpm` |
| Focused CTest | 3/3 pass: solver, blind monitor, DCI length sweep (18.68 s total); new solver cases cover one-layer QPSK probes, bounded fallback, concurrent scan contexts, and incompatible-table confirmation | [OFFLINE VERIFIED, DGX aarch64 sandbox] `heavy.sh ctest --test-dir cmake_targets/ran_build/build_solve -R '^(test_nr_pdsch_solver\|test_nr_pdcch_blind_monitor\|test_nr_pdcch_dci_length_sweep)$' --output-on-failure -j3` |
| GPU discovery capability check before replay | `GPU_PLATFORM ... cc=0.0 ... model=unavailable`; `PDCCH_NID_GPU init failed; capture disabled` | [OFFLINE VERIFIED, DGX sandbox, `replays/replay_solve_gpucheck_150613/run.log`] |
| Full raw capture `raw_pci415_12s_b` (0.25 replay speed) | reached EOF, 85/113 common PDSCH CRC-OK; zero dedicated DCI locks/solver grants after GPU nID init failed | [OFFLINE VERIFIED replay, DGX aarch64 sandbox, `replays/replay_solve_b1_151039/run.log`; dedicated gate DEFERRED] |
| `raw_pci415_12s_20261008`, main RNTIs | unmeasured | [DEFERRED GPU MEASUREMENT] |

### Requested six-run A/B replay table

The following cells are **not results**. Each pair must be run sequentially on the host with a usable GPU and the
same binary/configuration, using the command below. A = T26d recommended environment with all new flags off;
B = A plus `ISAC_PDSCH_SOLVER_NL_PREF=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=1
ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1`.

| Paired run, capture b | A: solved RNTIs / full CRC per RNTI | B: solved RNTIs / full CRC per RNTI | RNTI-set equality, probe false-lock check |
|---|---|---|---|
| 1 | deferred | deferred | deferred |
| 2 | deferred | deferred | deferred |
| 3 | deferred | deferred | deferred |
| 4 | deferred | deferred | deferred |
| 5 | deferred | deferred | deferred |
| 6 | deferred | deferred | deferred |

The acceptance criteria remain **open**: at least 9/10 capture-b replays solving at least five RNTIs, the old
capture's main RNTIs solving, no false locks, and per-RNTI full CRC-OK counts at least T26d's best runs. T26d measured
3/4 solves without MCS-only preference versus 2/4 with it; those older runs are context, not a six-run baseline or a
claim about these changes. [T26d, MEASURED replay raw_pci415_12s_b; acceptance DEFERRED]

## DEFERRED GPU MEASUREMENTS — exact host commands

Build on the GPU-capable host using the build settings in `CSD_COMMON_1009.txt`. Then run this **offline** paired
campaign. The replay configuration pins scan consumers to 10–11 and PDSCH consumers to 8,9,12,13,14; no core 15–19,
radio, or X410 is used. `ISAC_PDSCH_SOLVER_PROBELOG=1` is common to both arms so every credited probe records
`dci`, signal slot, `crc`, `Nl`, MCS, ports, table and scrambling id. Check `SOLVED by 2 independent CRC passes`
against CRC-OK probes on different DCI IDs with the same RNTI, scrambling id, and overlapping layout IDs, and reject
any lock that lacks subsequent full-TB
CRC passes. Count full CRC per RNTI from `PDSCH_SOLVER per-rnti` and EVMGRANT lines. [DEFERRED GPU MEASUREMENT]

```bash
cd /home/nicola/NICOLA/wt/rr-csd-p
B=$PWD/cmake_targets/ran_build/build_solve
O=/home/nicola/NICOLA/wt/rr-orchestration
export REPLAY_PREFIX='' REPLAY_OUT=$PWD/replays REPLAY_LAST=$PWD/replays/last
export BASECFG=$PWD/tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg
export CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 ISAC_MLOCK=0
base=(ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0
  ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1 ISAC_COREMAP_IDSWEEP_DUR=1
  ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1 ISAC_GPU_SELFCHECK=0
  ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1
  ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_PDCCH_SCAN_TIMING=1
  ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24
  ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1
  ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_BURST=16 ISAC_PDSCH_SOLVER_PROBE_FILL=25
  ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1 ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1
  ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1 ISAC_PDSCH_SOLVER_BACKOFF=200
  ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 ISAC_PDCCH_JOINT_ANON_RATE=0.1
  ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_RX_MRC_PER_RNTI=1
  ISAC_RX_MRC_ALIGN=2 ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0 ISAC_PDSCH_SOLVER_PROBELOG=1)
solve=(ISAC_PDSCH_SOLVER_NL_PREF=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=1
  ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1)
for i in 1 2 3 4 5 6; do
  "$O/heavy.sh" "$O/csd_replay_user.sh" "solve_A${i}" "$B" 0.25 "${base[@]}"
  "$O/heavy.sh" "$O/csd_replay_user.sh" "solve_B${i}" "$B" 0.25 "${base[@]}" "${solve[@]}"
done
export CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_TICK=210036277
for i in 1 2 3 4 5 6; do
  "$O/heavy.sh" "$O/csd_replay_user.sh" "solve_old_B${i}" "$B" 0.25 "${base[@]}" "${solve[@]}"
done
```

For each log, record GPU nID initialization, CORESETs, `DCI 1_1 length locked` RNTIs, `JOINT_ANON_ASYNC`
submitted/done/dropped, `PDSCH_SOLVER probe crc=1`, `CANDIDATE`, `SOLVED`, per-RNTI full CRC, stale/full queue drops,
and the input signal window. A GPU initialization failure invalidates the dedicated solve row. Compare repeated B runs'
RNTI sets for identical coverage windows; different startup or discovered CORESETs do not establish a deterministic
PDCCH result. The probe log's CRC pass is code-block 0 evidence; only full TB CRC after solve validates the lock.

## Exact OTA environment for operator validation

`tests/passive_rx/dgx/ue.passive.ota.t26d.dgx.cfg` with the normal OTA wrapper's loader/device section, and the
T26d recommended flags below plus the four SOLVE flags. This is **not OTA-validated**; the X410 is unavailable.
The T26d OTA core map is scan 5,6 and `ISAC_PDSCH_CORES=7,8,9,3,4`; this OTA setting is separate from the safe
offline replay core map above. [INFERRED from T26d + implementation]

```text
ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0
ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1 ISAC_COREMAP_IDSWEEP_DUR=1
ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1 ISAC_GPU_SELFCHECK=0
ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1
ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_PDCCH_SCAN_TIMING=1
ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24
ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1
ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_BURST=16 ISAC_PDSCH_SOLVER_PROBE_FILL=25
ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1 ISAC_PDSCH_SOLVER_SCR=1
ISAC_PDCCH_JANON_ALL_CORESETS=1 ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1
ISAC_PDSCH_SOLVER_BACKOFF=200 ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1
ISAC_PDCCH_JOINT_ANON_RATE=0.1 ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDSCH_CORES=7,8,9,3,4
ISAC_RX_MRC_PER_RNTI=1 ISAC_RX_MRC_ALIGN=2 ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0
ISAC_PDSCH_SOLVER_NL_PREF=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=1
ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1
```

Leave `ISAC_RX_BRANCH_FO`, `ISAC_PDSCHQ_SIGNAL_TIME`, `ISAC_PDSCH_SOLVER_PREF`, solver leader/fair and AB diagnostics
unset. The CSI-RS discovery contract in `nr_csirs_disc.h` was not modified. [CODE-READ]
