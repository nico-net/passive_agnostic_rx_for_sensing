# PROJECT_MEMORY — Passive Agnostic 5G NR Receiver (OAI-UE based)

**This file is the single source of truth for this project.** It was consolidated on **2026-09-30** on host
`sens6`, branch `adaptive-rx-UL-DL`, for the migration to a new development machine (**NVIDIA DGX Spark**).
Every other project-authored Markdown file was retired in the commit that follows the one introducing this file
(see §0.4 for how to recover any of them from git history). Do **not** assume access to any earlier chat,
agent, terminal or human conversation: everything needed is here or in the repository.

> **Update 2026-09-30 (first session on the DGX Spark `spark-74c3`, Milan).** Environment recorded (§4.2), fresh
> aarch64 build done, offline suite and the phy-test rfsim bed re-run on ARM (§13.1, §14.1). New known issues K21–K26
> (§24). **Site change:** the old lab testbed (sens4 gNB, PCI 2, PLMN 001/06) and the Swiss commercial cells are no
> longer reachable; **all next OTA tests are on a new, unknown cell seen from the DEIB building, Politecnico di Milano**
> (§15.0). No X410 is connected yet, so nothing OTA has run on the DGX.

> **Update 2026-10-01 (cloud Track-A session, branch `cloud/dgx-next-steps`, x86 cloud container — NOT the DGX).**
> Tasks A0–A7, A11 (code), A13 (x86 path) done in a cloud container (Intel Xeon @2.80 GHz, 4 cores, 15 GB, Ubuntu
> 24.04, gcc 13.3, no IPv6/SCTP, affinity EINVAL). New: `ISAC_METRICS` JSON line + file (§11.13), per-grant
> observation API (§21), rfsim regression gate/scorer, campaign runner, Receiver-health dashboard tab, thread-safe
> N-consumer blind-PDCCH scan, scan-scratch budget knob, DGX core-map launcher (dry-run only), arch-aware offline
> sync script. **Every cloud number is labelled `cloud x86` and is never merged with DGX numbers** (§14.5). Gate
> thresholds re-baselined on cloud (crc >= 93.0 %, drop_full <= 2.5 %) vs DGX (98 / 1). New known issues K28–K31 (§24);
> DGX follow-ups in §25. Final review fix commit: `6528bd0cfc`..`343d1f062a` (merged with the base in `d6cb580ce5`).

> **Update 2026-10-01 (DGX, operator session).** Merged into `adaptive-rx-UL-DL`: `port/multirx-rx-fixes` (scan-confirm
> CFO fix, BRANCHFO thread safety, P39; §14.4), `cloud/dgx-next-steps` (`54bbf03b91`) and the A7 race follow-up
> `claude/elegant-davinci-jlrfol` (`7f2fb28acf`). Merged tree verified **on the DGX** (§13.3, §14.7): ctest 126/129 (known
> ARM set only), DGX rfsim gate PASS, 2 scan consumers OK, obs ≡ metrics exactly, A13 aarch64 5/5. GPU modules built
> for `sm_121` and tested (K17). New designs (§0.6): Technique D convergence levers + compute acceleration, and
> reconfiguration robustness. New known issues K32–K37 (verified bugs: chest cache, stale credit, GPU LDPC false pass,
> GPU FEP staleness, CPU-vs-CUDA LDPC sensitivity, DCI/CORESET adaptiveness).

> **Update 2026-10-02 (DGX, branch `td/convergence-levers`, worktree `/home/nicola/NICOLA/wt/td-levers`, HEAD
> `83ca2b9d6c`, base `85748c6e1f`). LOCAL ONLY: NOT pushed, NOT merged into `adaptive-rx-UL-DL`; the merge waits for
> operator approval.** Two plans executed subagent-driven (ledgers under
> `.superpowers/sdd/2026-10-01-technique-d-{convergence-levers,blind-convergence}/progress.md`):
> (1) **Technique D convergence levers** Tasks 1–4, 4b, 4c (pure modules: grant gate, ordering score, field book,
> legality/signatures), **F1** (K32 chest cache, fixed), **F2** (K33 stale credit, fixed), Tasks 5–7 (simulator
> `nr_td_sim`; P1 probes/ordering **inert**, P2 failure-only probe evidence **failed** → off);
> (2) **blind-case convergence** BC0–BC5, BC2/BC2b (levers E/C/P, all default off), BC7 (**K39** DM-RS oracle no longer
> pins k0), BC8 (simulator v2), BC9 (DL DCI history, TDD per-hypothesis exclusion, DCI-adjacency exclusions; **K40** TDD
> slot direction fixed incl. the PDCCH gate), BC7b (**K42** observation thrash fixed), BC9d (**K43** hard exclusions
> only from CRC-confirmed DCIs), BC12a (SIB1 common-TDRA census, log only), reduced BC6 gate (simulator).
> **What runs in the receiver today:** K39 fix, BC9/BC9d exclusions (live only when SIB1 TDD is known), K40, K42, F1,
> F2, BC12a census, the `ISAC_TD_PROBE_EQUIV_CHECK` harness. **Engine/simulator only (no runtime caller; plan R2 not
> built):** grant gate, ordering, `next_k`/`feed_k`, field book, dormant masks/fail-open, levers E/C/P, sibling guard,
> `certify_k0`. **Regression gate changed** to the post-convergence mode (§12): the K39 fix lengthens the search phase,
> so the old overall-CRC ≥ 98 % criterion fails while post-convergence CRC is 100 %. Gate PASS ×2 at `4db43db6f5` and
> at the merged `ca1cbc5470` `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]` (§14.8). **BC6b landed** (`2588a83cc9` code, `ee5a805423` evidence; review APPROVED, minor items only; K44 resolved, simulator-validated only, no runtime wiring; fb2 conditionally recommended). **Open:** K38, K41, BC9c/BC10/BC11/BC12b, lever S, R2 runtime
> wiring, gate option (b), first OTA of all of this on the X410 on **2026-10-03** (§25).

> **Update 2026-10-04 (DGX, branch `td/convergence-levers` HEAD `003b8c3f93`, 34 commits after `5cc9d518e9`; about to be
> merged locally into `adaptive-rx-UL-DL`, not pushed). Levers + compute-acceleration round (ledger
> `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/progress.md`, task reports `task-{G1,G1-bler,K38,R2fb,ELIM,GW,BATCH,CB0WIRE,CB0GPU}-report.md`).**
> **K34 RESOLVED** (G1: CUDA LDPC pool safe, 200 ms timeout, N = 4 breaker, warm-up, `decoder_used`; CUDA is ~1 dB more
> sensitive than CPU, so CRC evidence is **not exchangeable across decoders**). **K38 FIXED** (LLR-norm shift over
> ceil(G/C)). New runtime modules: `nr_llr_norm.h`, R2 fb2 field-book wiring, CB0 elimination engine (premise check,
> per-batch decoder dominance), GrantWork-lite, CB0 batch (CPU pool, GPU dematch), CB0 CPU wiring (backend, hash subset,
> budget), dedicated GPU CB0 entry (async, mixed Z, own breaker, unified/explicit memory, sm_89 + sm_121) (§3.3.1).
> **New defaults on the merged main (operator 2026-10-04, "fastest combination", applied on `td/fast-defaults` by the
> controller; at `003b8c3f93` the code defaults are still OFF):** `ISAC_TD_GRANTWORK`, `ISAC_TD_CB0_ELIM`,
> `ISAC_TD_FIELDBOOK=2` ON, `ISAC_TD_CB0_BACKEND=auto`, `ISAC_TD_TB_CPU_WHILE_ACQ=1`; levers C/P/E OFF (§10.2).
> `[MEASURED, DGX rfsim 106 PRB 4 RX]` ttc tda0/tda2: main 14 / 79–87 s, levers OFF 56–109 / 139–143 s (K39 cost at
> 4 RX), CPU ON 15–24 / 42–43 s, GPU ON 14.4 / 47.4 s (n = 1, loaded host); postconv CRC 100 %, 0 premise alarms, 0 wrong
> winners (§14.10). **4 RX only for all tests from 2026-10-04 (operator).** Regression gate is now the **4-RX gate**
> (provisional: 420 s arms, ttc tda0 ≤ 39 s, tda2 ≤ 77 s, CRC floor 93.4 %, drop_full ≤ 2.5 %; §12). New known issues
> K45–K50 (§24); deferred work and the OTA checklist additions in §25 (2026-10-04 block). Hosts corrected (§0.3: sens6
> has an NVIDIA sm_89 GPU, model to verify).

---

## 0. How to read this document

### 0.1 Evidence labels (used everywhere below — never blur them)

| Label | Meaning |
|---|---|
| `[OTA VERIFIED]` | Observed over the air with the X410 in a capture whose log is identified. Always carries a date and the binary commit. |
| `[OFFLINE VERIFIED]` | Passed a deterministic offline test (ctest/gtest/python/C fixture) whose result is identified. |
| `[SIM VERIFIED]` | Passed a live **simulated** bed (OAI rfsimulator phy-test/SA, or OCUDU gNB over ZMQ). Not OTA. rfsim/ZMQ channels are ideal and every client receives identical samples, so CRC rates there are an **upper bound**. |
| `[IMPLEMENTED, NOT VALIDATED]` | Code exists in HEAD; no test/run shows it works. |
| `[PARTIAL]` | Works only for a declared subset of the NR configuration space. |
| `[PLANNED]` | Design target, no code. |
| `[KNOWN ISSUE]` | Reproducible defect with evidence. |
| `[HYPOTHESIS]` | Plausible explanation, not established. |
| `[STALE]` / `HISTORICAL / NOT CURRENT` | True at the time recorded, about a different binary/host/X410/cell, or superseded. Never quote as current state. |
| `[MEASURED, <host> <bed> …]` | (added 2026-10-01, branch `td/convergence-levers`) A number read from a run on the named host and bed, e.g. `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @<commit>]` or `[MEASURED, DGX unit test]`. An rfsim measurement is a `[SIM VERIFIED]`-class result (upper bound, not OTA). |
| `[SIMULATED, <host>, nr_td_sim @<commit>]` | Output of the Technique D Monte-Carlo simulator (`nr_td_sim`, §3.3.1) that links the real engine. A model result: never merged with rfsim or OTA numbers, and only valid for the modelled effects. |
| `[CODE-READ]` | Derived by reading the code (review), not by running it. |
| `[ANALYTICAL]` | (added 2026-10-04) Derived by calculation from tables/formulas (e.g. the CB0 dedup ratio from the OAI MCS tables), no run. |
| `[MEASURED, DGX GB10 …]` | (added 2026-10-04) A GPU measurement on the DGX Spark's GB10 (sm_121, CUDA 13.0, unified memory), e.g. a benchmark or a BLER harness. Never pooled with a future `[MEASURED, sens6 …]` GPU number (discrete sm_89 over PCIe). |

Rules that produced this document (and that you must keep following):

1. A comment in the code is not evidence. A Markdown claim is not evidence. A branch that contains a feature
   does not mean the runtime binary contains it.
2. An offline pass is not OTA validation. A simulated (rfsim/ZMQ) pass is not OTA validation.
3. An old OTA success is not the current state. **The current HEAD has never run over the air** (§15).
4. Never quote an inherited number without re-measuring it in the configuration under test.
5. Results from different X410 units, hosts, bandwidths, cells, operators or commits are never merged into one
   claim.

### 0.2 Repository identity at consolidation time

| Item | Value |
|---|---|
| Remote used for the migration | `github` = `git@github.com:nico-net/passive_agnostic_rx_for_sensing.git` |
| Other remotes | `mygitlab` = `git@gitlab.com:pvnis/openairinterface5g.git` (older pushes, incl. `x410-100MHz`); `origin` = `https://gitlab.eurecom.fr/oai/openairinterface5g.git` (upstream OAI); `sensing` = local path on sens6 (ignore on the DGX) |
| Branch | `adaptive-rx-UL-DL` |
| HEAD before this file | `457c24fac5` (2026-09-29) "docs: sync the live agnosticity handover…". Its **source code is byte-identical to `621bdc32a2`** (only one doc differs) |
| Upstream OAI base | last upstream merge contained in HEAD: `fb944fbad6` "Merge remote-tracking branch 'origin/integration_2026_w32' into develop" (2026-08-07). Project diff = `git diff fb944fbad6 HEAD` |
| Size of project delta | ~913 commits since the 2026-07-04 fork point; ~700 files changed vs `fb944fbad6` (many are sensing/test scenes) |
| Primary authors | Nicola Gallucci (+ AI agents: Claude Code, Codex). Dan Mihai Dumitriu (pavonis) authored the ISAC sensing lineage that was ported in. |
| Last binary that ran OTA from this lineage | `aa872eba3b` (built 2026-09-24 15:18 UTC), 136 commits **behind** HEAD |

### 0.3 Hosts that existed on the old setup (none of them move to the DGX)

| Host | Role (as of 2026-09-30) |
|---|---|
| `sens6` | Development + OTA host. Only host with the X410 (on a 100 GbE ConnectX-5). All code lives here: `/home/sens/NICOLA/adaptive-rx-UL-DL` + lane worktrees `/home/sens/NICOLA/agn-wt/*`. **No 5G core** here, so only OAI `--phy-test` rfsim beds run here. **GPU (corrected 2026-10-04; this row omitted it): discrete NVIDIA, compute capability 8.9 (`sm_89`), PCIe, NOT unified memory.** Model: the 2026-09-30 snapshot `env_sens6.txt` shows a **GeForce RTX 4060 Ti 8 GB** with nvcc 12.4 (§4.1); the operator believes it is an **RTX 4070 (~12 GB)** — **verify with `nvidia-smi` on sens6** before any GPU test. Build there with `LDPC_CUDA_ARCH=89` (nvcc 12.4 cannot target sm_121). GPU runtime of the 2026-10-03/04 GPU code on sens6: **UNVERIFIED**. |
| `sensnuc3` | Operator's local machine. open5gs 5G core (AMF 127.0.0.1:38412, PLMN 001/06, MongoDB), OCUDU gNB + srsUE (`/home/sens/NICOLA/repos/`), OAI SA rfsim trees (`rfsim-local` = test gNB, `rfsim-val`, `rfsim-integ` = receiver under test). Holds the agent lock file `/home/sens/NICOLA/AGENT_OWNER` and non-repo design docs under `/home/sens/NICOLA/docs/` (their content is folded into §17–§20 here). **Its `/home/sens/NICOLA/adaptive-rx-UL-DL` is a stale copy — never use it.** |
| `sens4` | Lab gNB host (srsRAN/OCUDU-family `gnb -c /home/sens/gnb.yaml`, driving a radio at 3450 MHz). Live log `/home/sens/NICOLA/gnbLogs/gnb.log` (the old `/home/sens/gnb.log` is stale). **Ground truth for validation only; never an input to the receiver.** |
| `sens3` | Older OTA host (Aug 2026), different X410/NIC addresses. HISTORICAL. |
| **`spark-74c3`** | **NEW (2026-09-30): NVIDIA DGX Spark (GB10, aarch64), DEIB / Politecnico di Milano.** Development + future OTA host. §4.2. The Swiss hosts above (and the sens4 lab gNB) are **not reachable** from here. GPU: integrated GB10, `sm_121`, CUDA 13.0, **unified CPU/GPU memory** (121 GiB shared); the GPU CB0 entry runs there in UNIFIED mode (no host↔device copy), and its EXPLICIT-copy path is also tested there (§3.3.1). |

Clock note: `sens6` runs in UTC; sensnuc3 in CEST (UTC+2); sens4 vs sens6 were measured ~26–31 min apart at
one point. Cross-host timestamps are unreliable unless re-measured.

### 0.4 Where the retired documentation went

All ~146 project-authored `.md` files (handovers, lane reports, plans, the 120 KB architecture paper, the 356 KB
agnosticity handover log) were removed from the working tree in the commit immediately after the one that
introduced this file. They remain in git history. To read one:

```bash
git log --diff-filter=D --name-only --oneline -1 -- PASSIVE_AGNOSTIC_RECEIVER_ARCHITECTURE.md   # find the removal commit R
git show R^:PASSIVE_AGNOSTIC_RECEIVER_ARCHITECTURE.md | less
git show R^:docs/cloud/HANDOVER_AGNOSTICITY.md | less
```

The tag `docs-archive-2026-09-30` points at the last commit that still contains all of them. Several source-code
comments still reference those filenames (e.g. `docs/cloud/full-running-agnosticity/*.md`,
`PHASE1_CSS0_AUTOCONF_HANDOVER.md`); read them through the tag. Upstream OAI documentation (`doc/`, `README.md`,
`CHANGELOG.md`, …) was **not** touched.

### 0.5 Files added by this consolidation (only non-Markdown change)

`tests/passive_rx/sens6_host_snapshot_2026-09-30/` preserves files that existed **only on sens6 outside git** and
would otherwise be lost in the migration:

| Path | What it is |
|---|---|
| `discovery_tool/idsweep_offline.c`, `idsweep_gpu.cu`, `discover_live.sh` | The offline/GPU **stage-1/stage-2 dedicated-CORESET discovery tool** (blind PDCCH DM-RS scrambling-ID sweep over 65536 IDs × 14 symbols) and its live loop. The receiver consumes its output file `/tmp/coresets_discovered.txt`. Build line is in the `.c` header (`gcc -O3 -march=native -fopenmp -I<repo> …`; GPU: `-DUSE_GPU` + nvcc for `idsweep_gpu.cu`). |
| `ota_confs/*.conf` | The receiver configs used in the latest OTA campaign (`agnostic_ota_noprior.conf` = fully agnostic lab arm; `salt_ota_noprior.conf` = Salt macro arm) and older Salt configs (`cons6_*`). |
| `launchers/run_sw.sh`, `launchers/ota_loop.sh` | Host launchers (Swisscom arm; `ota_loop.sh` targets the *other* branch `feature/multirx-clean-adaptive`). |
| `host_network/*` | sens6 X410 host network scripts: mgmt-port dnsmasq toggle, NIC ring-buffer dispatcher hook, sysctl file. |
| `latest_ota_2026-09-25/<run>/` | `verdict.txt`, `bracket.txt`, a milestone-line extract and the last 40 lines of every run of the latest OTA campaign (full logs are 10–160 MB and stay on sens6 `/home/sens/NICOLA/captures/`). |
| `evidence_2026-09-30/` | Fresh ctest result (§13) and full `sens6` environment dump (§4). |

---

### 0.6 Design documents, plans and evidence created on the DGX (2026-09-30 → 2026-10-01)

| Kind | Path | Status |
|---|---|---|
| Plan | `docs/superpowers/plans/2026-10-01-dgx-next-steps.md` | Track A done in cloud (A0–A7, A11, A13) + DGX checks (§25); DGX-only items and Track B (X410) open |
| Spec | `docs/superpowers/specs/2026-10-01-technique-d-convergence-levers-design.md` | approved (incl. §9 compute acceleration, §9.4 verified prerequisites) |
| Plan | `docs/superpowers/plans/2026-10-01-technique-d-convergence-levers.md` | **Executed 2026-10-01/02 on `td/convergence-levers`** (operator scope: Tasks 1–4, 4b, 4c, 5–7, F1, F2); F3, G1–G4, R1–R4 not done (R2 = runtime wiring, deferred; fb2 conditionally recommended for it, operator decision after BC6b). Amended: K = 1, field book superseded by the blind addendum §4 |
| Spec | `docs/superpowers/specs/2026-10-01-technique-d-blind-convergence-design.md` | Blind-case addendum, approved 2026-10-01 (levers E/C/P/F/S/G); §8 holds the reduced BC6 decision (2026-10-02) |
| Plan | `docs/superpowers/plans/2026-10-01-technique-d-blind-convergence.md` | BC0–BC5, BC2/BC2b, BC7, BC7b, BC8, BC9, BC9d, BC12a, BC6 (reduced), BC6b done (landed `2588a83cc9`/`ee5a805423`, reviewed); BC9c, BC10, BC11, BC12b deferred |
| Notes | `docs/superpowers/specs/2026-10-01-technique-d-k0-speed-recovery-notes.md` | k0 speed-recovery investigation (DCI-adjacency stratum, fast-path stream, adaptive sibling test); origin of K39 |
| Evidence | `tests/passive_rx/dgx_host_snapshot_2026-09-30/{f1_chest_cache,f2_stale_credit,bc7_k39,bc7b_k42,bc9_dci_adjacency,bc9d_confirmed,final_gate_2026-10-02}/`, `tests/passive_rx/td_sim/` (baselines, `results_2026-10-0{1_p1,1_p2,2_bc}/`) | branch `td/convergence-levers` |
| Spec | `docs/superpowers/specs/2026-10-01-reconfiguration-robustness-design.md` | approved; plan `docs/superpowers/plans/2026-10-01-reconfiguration-robustness.md` executing on branch `rr/reconfig-robustness` (worktree `/home/nicola/NICOLA/wt/rr-robust`, Phase 1 first). SIB1/NSA correction `69ae438406` on `adaptive-rx-UL-DL` (not pushed) |
| Ledger + reports (2026-10-03/04 acceleration round) | `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/progress.md` (chronological, controller rulings, watchdog notes), `task-G1-report.md` + `task-G1-bler.md`, `task-K38-report.md`, `task-R2fb-report.md`, `task-ELIM-report.md` (+ fix round 1), `task-GW-report.md` (+ fix round 1), `task-BATCH-report.md` (rounds 1–2), `task-CB0WIRE-report.md` (rounds 1–2), `task-CB0GPU-report.md` (+ integration, `task-CB0GPU-paired.csv`, `task-CB0GPU-bench.csv`), `LOCK_POLICY.md` | levers spec §9 compute acceleration items 1–4 and 6 + K38; all landed on `td/convergence-levers` @`003b8c3f93` |
| Evidence (2026-10-03/04) | `tests/passive_rx/dgx_host_snapshot_2026-09-30/k38_llr_norm/`, `tests/passive_rx/td_sim/results_2026-10-03_elim/`, `results_2026-10-04_elim_r1/` (runner `gate_elim.py`, `gate_elim.json`, `gate_elim_decoder.json`). The 4-RX rfsim evidence of CB0WIRE/CB0GPU was written to `/tmp/cb0wire_4rx/`, `/tmp/cb0wire_r2/`, `/tmp/cb0gpu_rfsim/` and is **not kept in git** (numbers live in the task reports) | — |
| Onboarding | `CLAUDE.md` | rules for agents (sens6 frozen, evidence labels, build/test commands) |
| Evidence | `tests/passive_rx/dgx_host_snapshot_2026-09-30/` (env, ctest, phytest, port branch, `merge_2026-10-01/`), `tests/passive_rx/cloud_run_2026-10-01/` (cloud x86) | — |

## 1. Project purpose and system overview

### 1.1 Research goal

Build a **fully passive, agnostic 5G NR receiver** that autonomously discovers and processes an unknown 5G NR
cell **without associating to the network** and **without prior knowledge of cell-specific control
parameters**, and exports per-grant PHY observations (channel estimates, timing, CFO/Doppler, scheduled
resources) for downstream consumers — primarily **passive / multi-static ISAC sensing** (bistatic radar using
the gNB and the cell's UEs as illuminators), and in future UE localization and beam/CSI telemetry.

It is built on the **OpenAirInterface (OAI) NR UE** (`nr-uesoftmodem`), repurposed with a `--passive-rx` mode,
and runs on a **USRP X410** SDR (4 RX, up to 400 MHz per channel; currently used at 122.88 MS/s × 1–4 ch).

### 1.2 What PASSIVE means here (runtime contract)

- No SIM attachment, no RRC connection, no PRACH, no scheduling request, no HARQ-ACK, no SRS, no intentional
  transmission of any kind. In `executables/nr-ue.c`, `RU_write()` returns before the radio write in passive
  mode on real hardware `[IMPLEMENTED]`. The UHD backend still *creates* TX streamers (tx gain is set to 0);
  a full driver-path/physical-emission audit is **not done** `[KNOWN ISSUE: passivity audit open]`.
- `--passive-rx` is a modifier of SA mode: it cannot be combined with `--phy-test`, `--do-ra` or `--nsa`
  (`executables/softmodem-common.c:144`). NSA cells are received in "SA-shaped" passive mode.
- The receiver never asks the scheduler for anything. Traffic and scheduling come from other, independent UEs.
  **An idle cell produces nothing useful** beyond broadcast (SSB/PBCH/SIB1).
- Decoding a PDSCH/PUSCH transport block recovers ciphered higher-layer bits; it does not decrypt anything.
  The bits are used only to reconstruct the transmitted waveform (data-aided channel estimation).

### 1.3 What AGNOSTIC means here

For every parameter, the receiver must *derive* it from the received signal and record its provenance:

| Provenance class | Meaning |
|---|---|
| Broadcast-derived | Decoded from MIB/SIB1 and checked within a timing/config generation. |
| Signal-estimated | Estimated from IQ/CFR under stated assumptions (e.g. DM-RS scrambling ID by coherence). |
| CRC-supported | A hypothesis decoded real transport blocks within a declared search domain (e.g. Technique D). |
| Assumed | A default (e.g. dedicated BWP = full carrier) — **not learned**. |
| Unsupported / unsearched | Legal NR configurations the code cannot evaluate. |
| Unresolved / ambiguous | Insufficient evidence or several surviving explanations. |

**Receiver knowledge that is allowed** (not cell knowledge): device addresses, antenna geometry, sample format,
RF capability, compute budget, and a search domain (band / frequency window).

**The receiver should ideally NOT need to know beforehand:** PCI; exact frame/slot timing; carrier bandwidth,
Point A and centre frequency; SSB position; CFO; RNTIs; dedicated CORESET (extent, duration, mapping,
interleaving, shift, scrambling ID); search space (aggregation levels, candidates, periodicity); DCI sizes and
field layouts; dedicated BWP; TDRA tables; DM-RS configuration and scrambling IDs; MCS table; data scrambling
IDs; CSI-RS/ZP-CSI-RS resources; rank / layers; HARQ state; scheduling information; operator; gNB identity;
infrastructure position.

**Hard project rule (operator, repeated many times):** gNB configs/logs/scheduler output are **ground truth for
validation only**. They must **never** seed the receiver (no RNTI, no CORESET, no CSI-RS map, no TDRA from the
gNB). SIB1 may be used as an *ordering/cut prior with a wide margin* but must not hard-seed dedicated parameters.
Manual configs are allowed only as labelled **oracle arms**, never reported as autonomous results.

A CRC winner is not proof of every configuration field: distinct layouts can produce the same codeword, unused
TDRA entries are unobservable, and no finite receiver can infer unsignalled, unused configuration. The earlier
"29/35 parameters = 83 % agnostic" figure is **withdrawn** (it mixed categories).

### 1.4 CURRENT receiver vs FINAL TARGET receiver

| | CURRENT receiver (HEAD) | FINAL TARGET receiver (§17–§21) |
|---|---|---|
| Cells | **One** cell per process, one carrier, co-channel neighbours ignored (can capture timing: see PCI 244 vs 64 on Salt) | Many cells, many carriers, several operators, managed as `CellContext`/`CarrierContext` |
| Start-up | Operator supplies `-C` (centre), `-r` (PRB count), usually `--ssb <subcarrier>` (or `--ue-scan-carrier`), gain, antenna count | Band-wide discovery, derived geometry, reopen/retune automatically |
| Broadcast | PSS/SSS/PBCH/MIB/SIB1 (upstream OAI + passive fixes); SIB1-less path (SIB1 absent or not decoded; not the same as NSA, §11.5) partly handled | Same plus robust SIB1-less operation |
| Dedicated control | Blind CORESET discovery, C-RNTI recovery, DCI 1_1/0_1 length + layout resolution, bounded search families | Complete, fair, AL-complete, multi-CORESET, change-detecting |
| Data | PDSCH/PUSCH decode, rank ≤ 4 DL (passive), one-layer UL, Technique D config sweep, data-aided CFR | Same, GPU-resident, HARQ-aware |
| State | Evidence-label tracker (`nr_passive_acq_state.c`), local DL relearning, stream-gap LOST | Persistent per-cell state machine with degraded/local/global reacquisition |
| Output | Log lines, sensing CFR rows into the (separate) ISAC engine, JSONL reports | Per-cell observation API (§21) |
| Validation | Offline + simulated beds; **HEAD never OTA** | Gate model §12, OTA on multiple cells/operators |

---

## 2. Current high-level architecture (what actually exists in HEAD)

### 2.1 End-to-end pipeline

```text
 X410 (UHD 4.10, MPM 6.1, FPGA image UC_200, MCR 245.76 MHz; 1-4 RX ch @ 122.88 MS/s sc16, port RX1)
   |  100 GbE QSFP28 DAC -> host ConnectX-5 (MTU 9000, rings 8192)          radio/USRP/usrp_lib.cpp
   v                                                        (dlopen'd plugin liboai_usrpdevif.so!)
 RF lifecycle / sample reader thread (pinned core 2)  +  continuity/RFSTALL watchdogs
   |                                                        executables/nr-ue.c, nr-ue-ru.c
   v
 Initial sync: GSCN raster (or pinned --ssb) -> PSS corr -> SSS -> PCI -> PBCH DM-RS/polar/CRC -> MIB
   |   CFO estimate (+ --initial-fo seed)                   openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c,
   |                                                        pss_nr.c, sss_nr.c, nr_pbch.c
   v
 Tracking: per-frame PBCH timing loop (PI on max_pos_acc), CFO loop, rebase guards, RFCENSUS
   |                                                        NR_UE_ESTIMATION/nr_adjust_synch_ue.c, nr-ue.c
   v
 MIB -> (a) CORESET#0/SS#0 autoconf ("CSS0 autoconf")       nr_pdcch_blind_monitor.c
        (b) MIB dmrs-TypeA-Position handed to blind monitor (works without CORESET#0 = NSA path)
   |                                                        openair2/LAYER2/NR_MAC_UE/nr_ue_procedures.c
   v
 SIB1 (SI-RNTI on CORESET#0) -> PLMN/cell id/TAC, carrier geometry (Point A, N_RB), common TDRA,
   TDD pattern, PRACH, common search spaces  ("SIB1 CELL", "SIB1 PRIOR", "TDD from SIB1", "ACQ carrier")
   |   [SIB1 cache per PCI, /tmp/passive_rx, default ON]    openair2/RRC/NR_UE/rrc_UE.c, config_ue.c,
   |                                                        nr_pdcch_sib1_prior.c, nr_tdd_pattern.c
   v
 Blind PDCCH monitor (RT tap + scan thread + queues)        nr_pdcch_blind_monitor_rt.c (6.9 kLOC)
   |-- Technique A: dedicated CORESET occupancy/extent (DM-RS correlation) -> bank   nr_pdcch_coreset_map.c,
   |   (+ offline/GPU stage-1/2 nID sweep hand-off /tmp/coresets_discovered.txt)     nr_pdcch_coreset_bank.c
   |-- candidate decode: FEP, PDCCH DM-RS chest, REG/CCE demap, polar decode, CRC-mask -> RNTI
   |-- Technique B: RNTI bootstrap/persistence (+ trusted RAR anchor)               nr_pdcch_blind_rnti_bootstrap.c
   |-- Technique C: DCI payload length sweep (DL 1_1, UL 0_1)                       nr_pdcch_dci_length_sweep.c
   |-- DCI 1_1 / 0_1 field-layout resolution (+ AL1 map, joint GF(2) RNTI solver, opt-in)
   |                                                        nr_pdcch_dci11_layout_sweep.c, dci01_layout_sweep.c,
   |                                                        nr_pdcch_al1_map.c, nr_pdcch_joint_solve.c
   v
 DL: PDSCH job queue -> passive decode (FEP, DM-RS chest, MRC/MMSE rank<=4, LLR, descramble, LDPC, TB CRC)
   |   Technique D config sweep (TDRA S/L, DM-RS add-pos/max-len, MCS table, type B, k0, PRG, PTRS, LBRM...)
   |   SSB / CSI-RS / ZP-CSI-RS rate matching, HARQ init-TX history for reserved-MCS retx
   |                                                        nr_pdsch_passive_queue.c, nr_pdsch_passive_decode.c,
   |                                                        nr_pdsch_config_sweep.c, nr_ssb_rate_match.c,
   |                                                        nr_csirs_blind_search.c/_rt.c, nr_harq_init_tx.h
 UL: 0_1/0_0 grant book (slot+k2) -> passive PUSCH receive (gNB-side chain reused), UCI footprint learning,
   |   UL width/interpretation hypothesis sweep, RA type 0 segmented decode, transform precoding (limited)
   |                                                        nr_pdcch_ul_discovery.c, nr_pusch_passive_*.c
   v
 Evidence feedback (generation-tagged tickets) -> hypothesis controllers (nr_hyp_sweep.c, nr_crc_evidence.h)
   v
 Outputs: CFR submissions (SSB, PDSCH DM-RS, PDSCH data-aided, PUSCH DM-RS/data, CSI-RS) -> NR_UE_ISAC
   sensing engine (separate subsystem, out of scope for the receiver work) ; acquisition state log ;
   counters/summary log lines
```

### 2.2 Block-by-block reference (current implementation)

For each block: files → key functions → input → output/state → config → assumptions → log lines → validation
status. Validation status here is summarized; details are in §13 (offline) and §15 (OTA).

**B0. RF / UHD front end**
- Files: `radio/USRP/usrp_lib.cpp` (+1313/−77 vs upstream), `executables/nr-ue.c`, `executables/nr-ue-ru.c`,
  `radio/COMMON/raw_iq*.c` (file backend).
- Functions: `device_init()` (X4xx MCR by FPGA image: `CG_*`→491.52 MHz else 245.76 MHz; rx_bw 100 MHz at
  122.88 MS/s; per-device RX port `RX1` for X4xx, `RX2` for B2xx; `RFCHAN` readback), `trx_usrp_read()` (short
  read tolerance, `RFSTALL` detection), `usrp_set_rx_freq_all()` (retune **all** channels — fix `9d6a2340c5`),
  `ISAC_RX_CHAN_MAP` (OAI antenna i → UHD channel map[i]).
- Input: `--usrp-args type=x4xx,addr=<data-ip>,mgmt_addr=<mgmt-ip>`, `-r`, `-C`, `--ue-rxgain`,
  `--ue-nb-ant-rx/tx`. Output: sc16 sample buffers per antenna, timestamps.
- Assumptions: sc16 host format hard-coded (`stream_args_t("sc16","sc16")`); `set_rx_antenna("RX1")` for X4xx;
  the process runs as root (`sudo`), `mlockall`.
- Logs: `RFCHAN`, `RX antenna forced to RX1`, `Actual master clock`, `USRP_RX_START`, `RFSTALL`, `ADCPEAK`,
  `RFPOW`, `ANTPOW`, `nic_miss` (harness).
- Status: `[OTA VERIFIED 2026-09-25, aa872eba3b]` 1-RX and 4-RX streaming at 122.88 MS/s start and sync; stability
  is `[KNOWN ISSUE]` (RFSTALL overflow/timeout ended most runs in the latest campaign; §8, §15).

**B1. Cell search: PSS/SSS/PCI**
- Files: `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` (+408/−60), `pss_nr.c`, `sss_nr.c` (upstream).
- Mechanism: upstream OAI GSCN scan inside the RF window (`--ue-scan-carrier`) or a pinned SSB subcarrier
  (`--ssb`), PSS time-domain correlation (N_ID2), SSS (N_ID1), PCI = 3·N_ID1 + N_ID2; CFO from PSS; project
  additions: CFO hypothesis sweep, scan confirm pass (`ISAC_SCAN_CONFIRM`), per-antenna scan mask, diagnostics
  (`SYNCDIAG`, `Cell Detected with GSCN … PSS Corr peak … Average`).
- Output: SSB position, PCI, frame timing (`rx_offset`), CFO.
- Status: `[OTA VERIFIED]` (lab PCI 2; Salt PCI 64 on 2026-09-25). Blind GSCN scan at 273 PRB × 4 RX has
  historically segfaulted (unchecked `malloc16`, ~1.6 GB scan buffers) and starves the RX reader — **pin `--ssb`
  at 273 PRB when possible** `[KNOWN ISSUE]`.

**B2. PBCH / MIB**
- Files: `nr_pbch.c` (+1431: diagnostics, per-branch decode, capture/replay hooks, shift clamp, timing-mutation
  audit `TIMEMUT`), upstream polar decoder, `nr_ue_procedures.c` (`nr_ue_decode_mib`).
- Output: SFN, half-frame, SSB index, `ssb_SubcarrierOffset` (k_SSB), `subCarrierSpacingCommon`,
  `dmrs-TypeA-Position`, `pdcch-ConfigSIB1`, `cellBarred`, `intraFreqReselection`.
- Project additions: MIB `dmrs-TypeA-Position` is passed to the blind monitor even when there is no CORESET#0
  (`nr_pdcch_blind_monitor_set_mib_dmrs_typeA_position()`, NSA path); in passive mode a failed MIB re-decode
  no longer sets `RLM_out_of_sync` (PBCH tracking failures on loaded cells were killing captures).
- Logs: `Initial sync: pbch decoded sucessfully, ssb index N`, `pbch rx ok. rsrp:… adjust_rxgain`,
  `ERROR NR_PBCH_DECODE => polar decoding wrong`, `RFCENSUS … pbch_ok=50 pbch_fail=0`.
- Status: `[OTA VERIFIED 2026-09-25]` lab + Salt. `[OFFLINE VERIFIED]` NSA MIB hand-off (`test_nr_ue_mib_blind_handoff`, 6 tests).

**B3. Tracking (timing/CFO)**
- Files: `NR_UE_ESTIMATION/nr_adjust_synch_ue.c` (+362), `executables/nr-ue.c`.
- Mechanism: upstream PI loop on PBCH CIR peak (`max_pos_acc`, `--time-sync-I`), plus: out-of-window global
  peak detection and gated rebase (`TSYNC_OBS … e_win_frac peak_oow`, `ISAC_TSYNC_GLOBAL_REBASE`,
  `ISAC_TSYNC_REBASE_RATIO`), `SHIFTCENSUS`, CFO trim loop `CFOTRK` (measure always; apply only with
  `ISAC_CFO_TRACK_APPLY=1`), per-branch FO measurement `BRANCHFO`, `--initial-fo` seed (same sign as the
  reported offset; −15000 Hz is this X410's typical seed), `--ntn-initial-time-drift -4.25` (ppm-like seed of
  sampling drift used in all OTA launches).
- Status: `[OTA VERIFIED]` on the lab cell (healthy −4.14 ppm window motion equals physical clock drift).
  On the Salt macro the window lands ±341 samples off (co-channel PCI 244) → fixed by the gated rebase
  `[OTA VERIFIED 2026-09-19, HISTORICAL]`. Timing runaway under some loads `[KNOWN ISSUE]`.

**B4. Broadcast configuration: CORESET#0 / SS#0 / SIB1**
- Files: `nr_pdcch_blind_monitor.c` (`nr_pdcch_blind_monitor_autoconf_css0()`), `openair2/LAYER2/NR_MAC_UE/config_ue.c`
  (+427: passive SIB1 extraction, `nr_pdcch_blind_monitor_set_tda_common`, `nr_pdcch_sib1_prior_set`,
  `nr_passive_acq_note_sib1_tdd`), `rrc_UE.c` (`SIB1 CELL mcc=… mnc=… cell_identity=… tac=…`).
- Output: CORESET#0 geometry (groups, duration, bundle, interleaver, shift, scrambling), SS#0 period/offset,
  cell PLMN + identity, DL/UL initial BWP, common TDRA lists, TDD pattern, PRACH config, derived carrier centre.
- Carrier verification: `ACQ carrier CONFIRMED from SIB1 …` or a mismatch emitting `ISAC_ACQ_RETUNE {"n_rb":…,
  "centre_hz":…}`, which the launcher (`run_arm.sh`, `ADAPT=1`) uses to relaunch once at the derived geometry.
- SIB1 cache (`ISAC_SIB1_CACHE`, default ON, `/tmp/passive_rx/sib1_common_pci<PCI>.bin`): loaded only when the
  live SIB1 is absent, logged as CACHED. **Agnosticity caveat:** it persists across runs keyed only by PCI
  `[KNOWN ISSUE — delete /tmp/passive_rx before any agnostic acceptance run]`.
- Status: `[OTA VERIFIED 2026-09-25]` SIB1 decoded on lab (PLMN 001/06) and Salt (PLMN 228/03).

**B5. SIB1-less handling** (SIB1 absent or not decoded; independent of SA/NSA, §11.5) — see §11 "SIB1/NSA" and §23. Status `[PARTIAL]`, `[OFFLINE VERIFIED]` only.

**B6. Blind PDCCH monitor** — the core of the agnostic receiver. Files and techniques in §23; status
`[OTA VERIFIED on the lab cell, 2026-09-17..25, older binaries]`; `[SIM VERIFIED]` on OAI phy-test/SA and OCUDU.

**B7. DCI recovery/interpretation** — §23. `[OTA VERIFIED]` DCI 1_1 length 47 / 0_1 length 45 on the lab cell
(C-RNTI confirmed against the gNB log); `[SIM VERIFIED]` 42/38 on OCUDU (checked against F1AP truth).

**B8. PDSCH/PUSCH decoding + Technique D** — §23. `[SIM VERIFIED]` (phy-test TD converged 5/5 runs at 1.3 s,
98.9 % CRC; OCUDU 76 % C-RNTI PDSCH CRC). OTA DL TB rate is low (`VOID_DL_RATE`) `[KNOWN ISSUE]`. Branch `td/convergence-levers` (2026-10-02, not merged) reworks Technique D k0 handling and adds DCI-history evidence: §3.3.1, §23.9.

**B9. Reference-signal / CFR production** — `openair1/PHY/NR_UE_ISAC/nr_isac.cc` API
(`nr_isac_submit_cfr*`), producers in the decoders. The sensing engine itself is **out of scope** for the
receiver work (see §2.3).

**B10. Acquisition state** — `nr_passive_acq_state.c`: evidence labels SEARCHING, PBCH_LOCKED, SIB1_DECODED,
PDCCH_LOCKED, CORESET_VERIFIED, CELL_CONFIGURED, DL_CONVERGED, UL_CONVERGED, TRACKING, LOST (§11, §19).

### 2.3 Scope boundary: receiver vs sensing

`openair1/PHY/NR_UE_ISAC/` (range-Doppler, CFAR, clutter/ECA, AoA, trackers, sync STO/CFO/SFO for CPIs) is a
**separate consumer subsystem** ported from a srsUE sensing pipeline. It is compiled only with
`-DENABLE_ISAC_SENSING=ON` (default OFF in the root `CMakeLists.txt`; ON in the sens6 build dirs). The most
recent sensing development lives on the **unmerged** branch `feature/multirx-clean-adaptive` (§3.3). For the
DGX work, treat sensing as a downstream consumer of the receiver output API (§21); do not debug the receiver
through sensing metrics.

---

## 3. All important modified files

### 3.1 Branches

| Branch | State | Notes |
|---|---|---|
| **`adaptive-rx-UL-DL`** | **Current integration branch. HEAD `457c24fac5`.** Pushed to `github`. | Contains: plan Tasks 1–16+18 ("full-running agnosticity", merged at `25a8699a64`), `sdd/validation` (SA discovery-stall fix + Technique D fix), `sdd/integration` (lanes perf, misc, pusch, harq, dmrs2), `cloud/ue-localization` @ `621bdc32a2` (lanes ssb, nsa-mib, csirs + ocudu-dl, bwp harness, cbg test, OCUDU ZMQ harness). |
| `cloud/ue-localization` | Merged into HEAD. | Tip `621bdc32a2` = HEAD code. |
| `port/multirx-rx-fixes` | **Merged 2026-10-01** (`e5007ed501`). | 8 receiver fixes ported from `feature/multirx-clean-adaptive` after triage (K2), §14.4. |
| `cloud/dgx-next-steps` | **Merged 2026-10-01** (`54bbf03b91`). | Track A of the DGX next-steps plan, cloud x86 session (§13.2, §14.5/§14.6). |
| `claude/elegant-davinci-jlrfol` | **Merged 2026-10-01** (`7f2fb28acf`). | A7 follow-up race fixes incl. upstream `task_ans.c` acq_rel (K30). |
| `td/convergence-levers` | **Local only. 2026-10-04: HEAD `003b8c3f93` (base `85748c6e1f`); being merged locally into `adaptive-rx-UL-DL` (push needs an explicit operator OK).** Worktree `/home/nicola/NICOLA/wt/td-levers`. | Levers + blind-convergence plans (§0.6, §3.3.1) and the 2026-10-03/04 acceleration round. Sub-branches merged into it: `td/t6-ablation` (`f22d85dc12`), `td/bc1-equiv` (`5c38acf88a`), `td/bc4-fieldbook` (`cddc900813`), `td/bc3-dormant` (`4d450cb022`), `td/bc2-crc` (`25c4d5ac7e`), `td/bc8-simv2` (`355284580d`), `td/gate-postconv` (`b3c531b221`), `td/bc12a-sib1-census` (`ca1cbc5470`), `td/excl-restart-log` (`6e3d6c9a9a`); 2026-10-03/04: `td/g1-ldpc-safety` (`15f2492c6c`), `td/k38-rootcause` (`f929120297`), `td/r2-fieldbook-wiring` (`334c0bfca8`), `td/elim-channel` (`5074c5f61e`, fix round 1 `b922eb4fb3`), `td/grantwork-lite` (`e5c604d9c7`), `td/cb0-gpu-batch` (`4ca2ce2825`), `td/cb0-cpu-wiring` (`16035a1ccb`), `td/cb0-gpu-entry` (fast-forward to `003b8c3f93`). |
| `td/fast-defaults` | **2026-10-04, created at `003b8c3f93`, merged into `td/convergence-levers` at `73754d4c24`.** `f3c9e585de`: fastest combination ON by default (§10.2) + provisional 4-RX gate + 4-RX evidence (`tests/passive_rx/dgx_host_snapshot_2026-09-30/cb0_4rx_2026-10-04/`). `4f0158f046`: pre-merge review fixes (test baseline header `tests/nr_td_test_baseline.h` + new-default tests, order-independent; GPU adapter abandons its input buffer after a CB0 timeout/CUDA error; CPU TB forced only for predicted CPU-backend CB0 batches; `ISAC_LLR_SCALE`/`ISAC_TD_FIELDBOOK` semantics (only 0 disables fb2); in-place adapter access `nr_pdsch_config_sweep_with_context`; CPU-only `ENABLE_LDPC_CUDA=OFF` build 13/13). ctest 141/144 (known ARM). | Final gate below (§14.10 addendum). Known limit (speed only): decoder dominance is per CB0 epoch, so after a CUDA-decoded TB, CPU CB0 batches of that context are rejected until the epoch resets. |
| `rr/reconfig-robustness` | Local, in progress (worktree `/home/nicola/NICOLA/wt/rr-robust`). | Reconfiguration-robustness plan (K37, epochs, SIB1/CSI-RS change triggers); not covered by this file's 2026-10-02 update. |
| `sdd/integration`, `sdd/validation`, `sdd/gap-*`, `sdd/agn-*`, `sdd/t*` (receiver lanes) | Merged (ancestors of HEAD), except the ones listed below. | Lane worktrees on sens6 `/home/sens/NICOLA/agn-wt/<lane>`. |
| `sdd/rfsim-gnb-test` (`67bb0eaa11`) | **Unmerged by design. NOT on the `github` remote (checked 2026-09-30).** | Test-only gNB injection knobs `ISAC_GNB_TEST_*` for the phy-test bed. Never merge. Only on sens6 (`agn-wt/gnbtest`) — without it the DGX phy-test bed runs with a plain HEAD `nr-softmodem` (no knobs; the baseline arm needs none). |
| **`feature/multirx-clean-adaptive`** | **Unmerged, 238 commits ahead, diverged from HEAD at `51f7d3deac` (2026-09-23).** | Sensing (coherent fuser, CUDA RD, long dwell, trackers, monitor UI) **plus receiver fixes that are NOT in HEAD**: P39 single-branch PDSCH/PDCCH estimation, per-antenna work off the scan thread, BRANCHFO fixes (CRC-OK-only integration, reset at decode entry), learned LLR confidence / SNR gate, dedicated-SS AL1-only fix, `run_sensing.sh` launcher. The 2026-09-26 OTA runs (`sense_*`) used this branch (`173db3bf68`). **Merge debt: triage before resuming receiver work.** **NOT on the `github` remote (2026-09-30: `git ls-remote --heads origin` lists only `adaptive-rx-UL-DL`, `cloud/ue-localization`, `sdd/integration`, `sdd/validation`) → unreachable from the DGX until pushed from sens6 (K23).** |
| `sdd/coh-*`, `sdd/t5..t16`, `sdd/serial`, `sdd/rt` | Unmerged (sensing lanes of the branch above). | |
| `wip/2026-09-28/gap-cbg` | CBG contract test (out of scope). Also in HEAD via merge `867cfa1676`. | |
| `total-passive-ue`, `passive-rx-only`, `total-passive-rx-UL-DL*`, `total-passive-UL-DL-adaptive` | STALE predecessors. | `total-passive-UL-DL-adaptive` has 1 unmerged commit "Re-decode paths never descrambled" (check if superseded). |
| `bladerf-*`, `passive-bladerf-100MHz` | STALE (bladeRF front end experiments). | Not relevant to X410. |
| `rx-passive-sensing`, `merge/adaptive-sensing`, `mistaken-push` | STALE / accidental. | |
| `develop` | Local snapshot "OTA X410 passive-rx working state (restore point)" 2026-08-03. | HISTORICAL restore point only. |
| `mygitlab/x410-100MHz` | External (upstream w32 + 3 commits). | Its ideas were ported; never merge. |

### 3.2 Functional modifications to upstream OAI files

| File | Upstream role | Our modification | Why | Runtime role | Validation |
|---|---|---|---|---|---|
| `executables/softmodem-common.{c,h}` | Common CLI | `--passive-rx` flag; asserts SA-only | Receive-only mode | Mode switch | OTA |
| `executables/nr-uesoftmodem.{c,h}` | UE main/CLI | `--initial-fo`, `--cont-fo-comp`, `--sync-actor-core`, `--dl-actor-core-start`, `--ul-actor-core-start`, replay requires passive | CFO seed, core pinning | Startup | OTA |
| `executables/nr-ue.c` (+1175) | UE RT loop | Passive TX suppression in `RU_write`; RF stall/continuity handling; RFCENSUS/RFPOW/ANTPOW telemetry; CFO trim loop; rebase/timing audits; RT-core pinning (`ISAC_UE_RT_CORE`); blind-monitor/PUSCH taps | Survive X410 stream faults, observe RF health, passive | Every slot | OTA |
| `executables/nr-ue-ru.c` | RU re-init | `nrue_ru_reinit()` for CFO retune, seed of re-acquisition with applied correction | CFO apply path | Rare | OTA (forced retune 2026-09-13) |
| `radio/USRP/usrp_lib.cpp` (+1313) | UHD driver (**dlopen'd `liboai_usrpdevif.so`**) | X4xx MCR by FPGA image; rx_bw 100 MHz at 122.88 MS/s; per-device antenna names (X410 `RX1`); all-channel retune; channel map; short-read tolerance; bounded RX drain; RFCHAN readback; power reference; stall detection; sysctl buffer sizing | X410 support and robustness | Always | OTA |
| `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` (+408) | Initial sync | CFO sweep, scan confirm, diagnostics, per-antenna masks | Acquisition robustness | Start-up | OTA |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c` (+1431) | PBCH | Per-branch decode, diagnostics, capture/replay, shift clamp, TIMEMUT | PBCH failures on X410/loaded cells | Every SSB | OTA |
| `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c` (+1247) | UE PHY procedures | Passive hooks: blind monitor, PDSCH/PUSCH passive paths, data-aided tap (attached UE), no RLM out-of-sync on MIB failure in passive, TRACKLOCK, CFOTRK | Passive pipeline integration | Every slot | OTA/SIM |
| `openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c` (+362) | Timing loop | Out-of-window global rebase, TSYNC_OBS, census, clamps | X410 4-ch/macro window landing | Every frame | OTA |
| `openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c` (+380) | DL chest | Per-antenna threading, nvar fix (was last-antenna only, /N_ant), time interpolation (`ISAC_CHEST_TINTERP`), SFO correction (`ISAC_SFO_CORRECT`), DFT-window options | 4-RX decode, MCS 25 | Every decode | OTA (lab, older) |
| `openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c` (+655) | PDSCH demod | MRC headroom fix, branch policy (`ISAC_RX_MRC_MODE`), rank ≤ 4 passive MMSE (float option), log2_maxh from decoded branch, selection-diversity retries, masks for SSB/CSI-RS | Passive DL | Every decode | OTA/SIM |
| `openair1/PHY/nr_phy_common/src/nr_channel_compensation.c` | Channel compensation | Saturating adds in MRC accumulator | Sign-flip overflow with 4 RX | Every decode | Offline + OTA |
| `openair1/PHY/NR_UE_TRANSPORT/dci_nr.c` (+659) | PDCCH decode | Exported helpers for the blind monitor; PDCCH MRC skip at 4 RX (optional); fixtures | Blind PDCCH | Every candidate | OTA |
| `openair1/PHY/NR_UE_TRANSPORT/csi_rx.c` (+328) | CSI-RS | Passive CSI-RS capture path without CSI report; 4-port Type-I PMI codebook fix | CSI-RS as RS source | CSI slots | OTA (older) |
| `openair1/PHY/MODULATION/slot_fep_nr.c` (+190) | FEP | `nr_slot_fep_ant()` per-antenna parallel FEP | 4-RX budget | Every slot | OTA |
| `openair1/PHY/NR_TRANSPORT/nr_ulsch_demodulation.c` (+212) | gNB PUSCH demod | Reused by passive UL; branch/timing policy headers | Passive UL | UL slots | SIM/OTA |
| `openair1/PHY/CODING/nrLDPC_coding/...segment_decoder.c`, `nr_rate_matching.c` | LDPC | Diagnostics (`LDPCDIAG`), LBRM n_L handling | Rank-4 MCS-25 wall | Every TB | OTA (older) |
| `openair2/LAYER2/NR_MAC_UE/config_ue.c` (+427), `nr_ue_procedures.c`, `nr_ue_scheduler.c`, `nr_ue_dci_configuration.c` | UE MAC | Passive SIB1 extraction into blind monitor; MIB DM-RS pos hand-off; maxMIMO-Layers fallback instead of AssertFatal; no RA in passive | Agnostic config | Broadcast | OTA |
| `openair2/RRC/NR_UE/rrc_UE.c` | UE RRC | `SIB1 CELL` log (PLMN/cell id/TAC) | Cell identification | SIB1 | OTA |
| `common/utils/threadPool/task_ans.c` | Thread-pool join counter | `completed_many_task_ans()` decrements with `memory_order_acq_rel` instead of relaxed (2026-10-01, A7 follow-up) | Only the last completer posts the semaphore; with relaxed the other workers' outputs (LDPC segments) were unordered w.r.t. the joiner — TSAN on the passive PDSCH pool, real hazard on aarch64 | Every pooled task | TSAN rfsim (`tests/passive_rx/cloud_run_2026-10-01/a7_followup_races/`) |
| `radio/rfsimulator/apply_channelmod.c`, `simulator.cpp` | rfsim | Sparse-tap convolution; nb_tx from own TX count (layers>1 now reach receiver) | Simulation beds | Sim | SIM |
| `openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c` | gNB config | Test hooks for beds | Sim | Sim | SIM |
| `CMakeLists.txt` (+554), `openair1/PHY/CODING/CMakeLists.txt` | Build | New libraries/tests (below), `ENABLE_ISAC_SENSING`, `ENABLE_LDPC_CUDA`, GPU modules | | | |

### 3.3 New receiver modules (all in `openair1/PHY/NR_UE_TRANSPORT/` unless noted)

| File (LOC) | Role | Validation |
|---|---|---|
| `nr_pdcch_blind_monitor.c/.h` (5190/1077) | Config parsing (`pdcch_blind_monitor_*` keys), CSS0 autoconf, SIB1 facts/cache, candidate decode helpers (extract, mismatched-bit gate migrated from NRSniffer `dci_nr.c`), discovery hand-off poll | offline gtest `test_nr_pdcch_blind_monitor` (195+2 skips) |
| `nr_pdcch_blind_monitor_rt.c/.h` (6893/390) | **RT tap + scan thread**: occasions, candidate lists per AL, discovery pass, bank pass, CORESET#0-USS pass, RNTI gates (energy, persistence, SNR, mismatch, DM-RS coherence — 5GSniffer-style, default off), length sweep driving, Technique D arming, summaries, `PDCCH_SCRAMBLING_ID CONFIRMED`, `RFSTALL` watchdogs | SIM + OTA (lab) |
| `nr_pdcch_coreset_map.c/.h` (1043) | Technique A: PDCCH DM-RS occupancy windows, contiguous extent candidates (≤ 45 six-RB windows), verification by fresh DCIs; idsweep snapshot dump (`ISAC_COREMAP_IDSWEEP`) | `test_nr_pdcch_coreset_map` |
| `nr_pdcch_coreset_bank.c/.h` | Verified dedicated CORESET bank (`multi-CORESET bank add …`) | offline |
| `nr_pdcch_blind_rnti_bootstrap.c` | Technique B: RNTI persistence table (16 slots, 2 sightings, 20 000-slot staleness); `record_trusted` for RAR-anchored C-RNTI (NSA/CFRA) | `test_nr_pdcch_blind_rnti_bootstrap` |
| `nr_pdcch_dci_length_sweep.c/.h` | Technique C: DCI length statistics 30–63 bits, 6-sigma CRC-adjacent gate | `test_nr_pdcch_dci_length_sweep` |
| `nr_pdcch_dci11_layout_sweep.c/.h`, `nr_pdcch_dci01_layout_sweep.c/.h`, `nr_dci11_pin.c/.h` | DCI 1_1/0_1 field-width derivation under the length constraint; plausibility stage; pinned layout with atomic accessors | `test_nr_pdcch_dci11_layout_sweep` (40), `test_nr_pdcch_dci01_layout_sweep`, `test_nr_dci11_pin` |
| `nr_pdcch_al1_map.c/.h` | AL1 candidate geometry independent of CCE-to-REG mapping; AL1 cover (`ISAC_AL1_COVER=1`, opt-in) | `test_nr_pdcch_al1_map` |
| `nr_pdcch_uss_tracker.c/.h` | USS aggregation-level inference (candidates per AL) | offline |
| `nr_pdcch_joint_solve.c/.h`, `nr_pdcch_joint_live.c/.h`, `nr_pdcch_gf2_rnti.c/.h` | Joint RNTI + payload recovery (GF(2), 5GDescrambler idea) — opt-in, **not validated on air** | `test_nr_pdcch_joint_solve`, `_joint_live`, `_gf2_rnti`; SIM (rfsim, real C-RNTI scrambling) |
| `nr_pdcch_ul_discovery.c`, `nr_pdcch_ul_field_sweep.c`, `nr_pdcch_ul_interp_sweep.c` | UL 3-component discovery (length, widths, waveform/TDRA semantics; RA type 0; TP) | gtests; SIM |
| `nr_pdcch_ss_registry.c`, `nr_pdcch_sib1_prior.c`, `nr_tdd_pattern.c` | Search-space set registry, SIB1 prior, TDD slot direction (**K40 fix 2026-10-02**: TS 38.213 §11.1 placement with FLEXIBLE / MIXED slots; `nr_tdd_pdsch_last_symbol()`) | gtests (`TddSpec.*`) |
| `nr_pdcch_passive_queue.c`, `nr_pdcch_gpu_fep.cu` | Candidate queue; GPU DM-RS chest+LLR (optional) | offline; GPU value not established |
| `nr_hyp_sweep.c/.h`, `nr_crc_evidence.h` | Shared hypothesis engine (8192 raw/class cap) and KL/Bernoulli anytime bounds | `test_nr_hyp_sweep` |
| `nr_pdsch_passive_queue.c` (1414), `nr_pdsch_passive_decode.c` (3873) | DL job queue (IQ lifetime, k0, tickets), passive PDSCH decode (FEP→chest→demod→LDPC→CRC), LDPCDIAG/HARQC/BRANCHFO, data-aided CFR submit, ZP grant evidence | SIM/OTA |
| `nr_pdsch_config_sweep.c/.h` (1376 at consolidation; ~2650 on `td/convergence-levers`) | **Technique D**: per-RNTI/TDA context sweep over (S,L,mapping A/B), DM-RS add-pos/max-len/type, MCS table, k0, PRG, PTRS, LBRM; DM-RS mask oracle (since K39 fix: prunes on mask/last symbol only, never on k0); `ORACLE_RESTORE`; LRU contexts. Branch additions: §3.3.1 | `test_nr_pdsch_config_sweep` (44+1 skip at consolidation; 167 pass + 1 skip at BC7b `[MEASURED, DGX unit test]`) |
| `nr_pdsch_qm_oracle.c`, `nr_pdsch_xoverhead.c`, `nr_pdsch_ptrs_unav.c`, `nr_pdsch_prb_set.c`, `nr_pdsch_adaptive_config.h` | Modulation-order oracle; xOverhead; PT-RS RE accounting; non-contiguous PRB sets | gtests |
| `nr_pdsch_data_aided.c/.h`, `nr_pusch_data_aided.c/.h` | TB re-encode → X → Ĥ = Y/X at data REs (DL and UL) | gtests + SIM |
| `nr_ssb_rate_match.c/.h` | SSB-overlap detection per slot + rate matching (TS 38.214 5.1.4); `PDSCH SSB-OBS` | `test_nr_ssb_rate_match`, `test_nr_ssb_rate_match_prod` |
| `nr_csirs_blind_search.c/.h`, `nr_csirs_blind_rt.c/.h`, `nr_csirs_monitor.c/.h` | Blind CSI-RS/TRS/ZP-CSI-RS discovery (rows 1–5 default; 6–18 opt-in `ISAC_CSIRS_BLIND_WIDE`), scrambling-ID sweep, ZP export with probation/contradiction debt/decoded-grant revocation; configured monitor | `test_nr_csirs_blind_search` (131), `test_nr_csirs_blind_synth` |
| `nr_dmrs_id_estimate.c/.h`, `nr_scrambling_id_sweep.c/.h`, `nr_pusch_passive_ul_ids.c/.h` | DM-RS scrambling ID estimation (0..1023 always, 1024..65535 throttled), data-scrambling-ID walk (PARTIAL) | gtests |
| `nr_harq_init_tx.h` | Per-(RNTI,pid) initial-TX record for reserved-MCS retransmissions (64 entries) | `test_nr_harq_init_tx` |
| `nr_passive_bwp.c/.h` | Passive DL BWP tracking | `test_nr_passive_bwp` |
| `nr_passive_acq_state.c/.h` | Acquisition evidence state tracker, `ISAC_ACQ_RETUNE` | `test_nr_passive_acq_state` |
| `nr_passive_mac_ta.c/.h`, `nr_passive_delay_contract.h`, `nr_passive_sample_lifetime.h`, `nr_passive_ul_grant_book.h`, `nr_passive_uci_learn.h`, `nr_passive_uci_probe.h` | UL timing/TA, delay contracts, IQ lifetime, grant book (256), UCI footprint learning | gtests |
| `nr_pusch_passive_decode.c`, `nr_pusch_passive_queue.c`, `nr_pusch_passive_monitor_rt.c` | Passive PUSCH receive using a private gNB receive context; RA0 segmented decode; TP (PCI-default identity, no hopping) | `test_nr_pusch_ra0_*` (9) + SIM |
| `nr_passive_replay_capture.c`, `nr_pdcch_discovery_replay.h`, `radio/COMMON/raw_iq*.c` | Same-build job replay (oracle) and raw-IQ file backend | offline |
| `openair2/LAYER2/NR_MAC_UE/nr_passive_rrc_harvest.c` | Harvest dedicated config from an **unciphered** RRCSetup (Msg4) | Rarely useful (dedicated config usually arrives ciphered; never on NSA) |
| `openair1/PHY/CODING/nrLDPC_cuda/*`, `nrPolar_tools/cuda/*`, `nr_pdsch_gpu_fep.cu`, `nr_polar_gpu*` | CUDA LDPC decoder plugin (`--loader.ldpc.shlibversion _cuda`), CUDA polar SC, GPU FEP | Measured **20× slower wall-clock** than CPU for single TBs on the RTX 4060 Ti (HISTORICAL, K17) — not used by default for full TBs. **2026-10-03/04:** the plugin's TB pool was made safe (G1, K34 resolved) and gained a separate CB0 batch entry (`nrLDPC_cb0_cuda.h`, `ldpc_cb0.cu`) used by the CB0 elimination channel; see §3.3.1 |
| `openair1/SIMULATION/TOOLS/sensing_channel.c`, `SIMULATION/NR_PHY/pusch_ra0sim.c`, `hidden_waveform.h` | Sim channel for sensing scenes; RA0 regression fixture; hidden-DCI transmitter fixture | gtests |
| `nr_passive_metrics.c/.h`, `nr_passive_metrics_json.c` | `ISAC_METRICS` JSON snapshot (schema 1) emitted with the blind-monitor summary (§11.13); optional file `ISAC_METRICS_PATH`; pure serializer in `_json.c` (testable without the receiver). `pci` is `_Atomic`. (A2, `4cdb890759`/`6e9a96c476`) | `test_nr_passive_metrics` (5) `[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 6e9a96c476]` + rfsim `[SIM VERIFIED]` (§14.5) |
| `nr_passive_obs.c/.h` | **Per-grant observation API**: one JSONL record per decoded DL/UL grant to `ISAC_OBS_PATH`, non-blocking ring (16384 slots, drop-on-full), writer thread; header comment = schema v1 (§21). (A3, `10cc6f0058`, fixes `385e02b9cf`) | `test_nr_passive_obs` (8) `[OFFLINE VERIFIED, cloud x86 …, 385e02b9cf]`, also under TSAN 0 warnings (A7); rfsim `[SIM VERIFIED]` (§14.5). UL hook compiled, **not exercised** (K28) |
| `nr_pdcch_blind_phase2.c/.h` | **Phase-2 lock** (`g_phase2_mu`) for the blind-PDCCH occasion tail (accepts, dci_thres EMA, RNTI persistence ring, census, CFR/PDSCH submit); energy floor with its own leaf lock; production accept gate `nr_pdcch_blind_dl_accept_gate()`. Enables N scan consumers (K27). (A7, `b6e5fb27ac`, `1d6cbdf5c3`) | `Phase2Concurrent*` (2) in `test_nr_pdcch_blind_monitor` (197+2 skips) `[OFFLINE VERIFIED, cloud x86 …, 1d6cbdf5c3]`; TSAN 0 warnings; rfsim A/B §14.5 |
| `nr_initial_sync_budget.c/.h` | Pure parser/batcher for `ISAC_SCAN_SCRATCH_MB` (default 512, clamp 64..16384) used by `nr_initial_sync.c` (A11, `843e5cff49`) | `test_nr_initial_sync_budget` (5) `[OFFLINE VERIFIED, cloud x86 …, 843e5cff49]`; timing effect DGX-only |

**Algorithm origins:** blind PDCCH mismatched-bit gate and parts of candidate handling migrated from **NRSniffer**
(`/home/sens/NICOLA/NRSniffer` on sens6, an OAI-based sniffer); DM-RS coherence gate modelled on **5GSniffer**
(`correlate_DMRS()` + AL thresholds, but adaptive); joint GF(2) RNTI solver after **5GDescrambler** (source cites
arXiv:2609.07367 — citation not independently verified). Everything else is project-original on top of OAI.

### 3.3.1 Technique D blind-convergence modules (branch `td/convergence-levers`, 2026-10-01/02, NOT merged)

"Runtime" = called by the receiver binary today. "Engine only" = library + unit tests + simulator, **no runtime
caller**. Of levers plan R2 only the **field book (fb2) is wired** (2026-10-03, `ISAC_TD_FIELDBOOK`); the grant gate,
ordering and K > 1 remain engine only (`ISAC_TD_GATE` does not exist). All tests: `[MEASURED, DGX unit test]` on the branch.
The 2026-10-03/04 acceleration modules are in the second table below.

| Module / API | Role | Wired? | Tests |
|---|---|---|---|
| `nr_td_gate.{c,h}` (levers Task 1, `2c02cffc26`) | Pre-outcome grant trial gate: `NR_TD_GATED_PHYSICAL` (DCI layers > n_rx), `NR_TD_GATED_CHANNEL_QUALITY` (SNR estimate from earlier grants below `nr_td_required_snr_db(mcs, table)` − margin 6 dB; coarse table, not link-level validated), else ELIGIBLE | engine only | `test_nr_td_gate` |
| `nr_td_order.{c,h}` (Task 2 `095ee422ae`; BC12a `072d11abfb`) | Ordering score from side info (SIB1 common TDRA, TS 38.214 default table A, observables, promoted fields), neutral by default (`side == NULL` = today's order). BC12a: `nr_td_census_sib1/_deftab` predicates, SIB1 TDRA store with k0 (`nr_td_sib1_store_set/get`, filled by `config_ue.c`), per-DCI-format counters | ordering: engine only; **census: runtime** (§11.8) | `test_nr_td_order` (incl. `TdCensus` ×7) |
| `nr_td_fieldbook.{c,h}` (Task 3 `6c7e02788e`/`08b036c763`; BC4 `bcfeaf5819`/`14778ee104`) | `CellFieldBook`: per field (TDRA = packed (S,L,mapping,**k0**), DM-RS add-pos, max-len; MCS table never) states UNSEEN → CANDIDATE → PROMOTED (≥ 2 distinct independently converged RNTIs, same config epoch) → SUSPECT (1st contradiction) → reconfirmed or WITHDRAWN (2nd independent contradiction); epoch bump turns promoted values into hints needing 2 new-epoch supporters; bounded 4-row candidate table, 16 RNTIs per value. TDRA value packs k0, but **the TDRA dormancy prune matches (S, L, mapping) only and ignores k0** (BC6b `2588a83cc9`): k0 stays in votes and hints, and a k0-part tracker relearns k0 in place after `withdraw_rntis` distinct RNTIs agree. K44 resolved `[SIMULATED, DGX host, nr_td_sim @2588a83cc9]` | **runtime behind `ISAC_TD_FIELDBOOK=2` since R2fb (2026-10-03; default ON on the merged main, §10.2)**; see "R2 fb2 wiring" below | `test_nr_td_fieldbook`, `PdschFieldBook.*` |
| `nr_td_legal.{c,h}` (Tasks 4b `d0c00c72ee`..`b48424a7ee`, 4c `b66c87a2cc`..`cd5b4e9255`; BC1 `9f77faa01f`/`1fd70e78b9`) | Legality bitsets (`nr_td_mask_*`); `nr_td_rm_feasible` = the decoder's RX reject rules (C == 0, Foffset > Ncb), differentially tested on ≥ 2000 synthetic geometries against the real RX function (RX rejects need C ≳ 79, so rare in practice); `nr_td_signature` (computation signature, always includes dmrs_add_pos); `nr_td_equiv_key` (grant-equivalence key incl. LBRM class bit 60, rate unit R×1024×10); `nr_td_geom_key` (lever P geometry group = S, L, k0, mapping, DM-RS mask) | engine only | `test_nr_td_legal` |
| `nr_pdsch_chest_key.h` (F1, `e3ff6649cb`/`e6788850d8`) | Chest-cache key = full estimator input (K32) | **runtime** | `test_nr_pdsch_chest_key` |
| `nr_passive_sample_lifetime.h` `nr_passive_credit_allowed()` (F2, `9dae73156e`..`e3ce2d22a1`) | Post-decode lifetime re-check: stale outcome → INCONCLUSIVE, no TD/layout/Qm/data-id/BWP-CRC/crc_note/DM-RS-id learning (K33); CRC-OK TB still delivered | **runtime** | `test_nr_passive_sample_lifetime` |
| sweep engine: `next_k`/`next_k_ex`/`feed_k` (Task 4 `2e3e303341`/`29d1ffdcaa`) | K hypotheses per grant + probe statistics, score-ordered rounds; K = 1 and neutral side info bit-identical to today. `side`/`p2` survive catalog rebuilds | engine only (runtime still feeds one hypothesis per grant) | `test_nr_pdsch_config_sweep` |
| sweep engine: `feed_equiv` / `_ex` / `_cx`, `feed_attr` (BC1, BC2b, BC9) | Lever E (equivalence crediting, **default off, not recommended**: biased, slower) ; `_ex` = explore-vs-exploit tag (levers C/P count only outcome-independent explore picks); `_cx` = plus the BC9 certified flag; legacy wrappers fail safe (EXPLOIT, certified = false) | engine only | idem |
| sweep engine: dormant masks + fail-open (BC3 `35d040e94b`/`7b7b89b623`) | Reversible per-cause masks (`NR_TD_DORMANT_PRIOR`, `_FIELD_BASE`+field, `_GEOM`; 5 causes), `set/clear_dormant`, `set_fail_open`, `fail_open_due(alpha, p_min)`; every active-set change restarts lever-C evidence; rebuild clears PRIOR/FIELD masks when the catalogue changed (callers re-apply) | engine only | idem |
| levers C / P + sibling guard (BC2 `e10abc94a0`/`f17f9e7325`; BC2b `399d441112`, `65989f0179`, `77775006f2`) | C = CRC-pass acceptance (new-data, unique among the full equivalence class incl. dormant members, m* passes); P = geometry pin (m_P* over the group trial sum); both evidence only from explore picks (fix A) and blocked unless every k0 sibling has ≥ N_sib = ceil(ln(n_sib/ε)/p_min) trials with 0 passes (fix B, `sib_pmin` 0.05, `sib_eps` 1e-6). **Default off; inert under persistent traffic** (BC6) | engine only | idem |
| `observe(..., k0_plausible)`, `certify_k0`, `exclude_key`, `row_k0_allowed` (BC7 `b35c293c55`, `68cdd4729d`, `92788b43bc`; BC9) | K39: DM-RS presence marks k0 plausible only; k0 is removed only by `certify_k0` (per (config, RNTI, TDA) context, LRU 8 per RNTI, uint64 mask) or by `exclude_key` (merged monotone per row, `nr_td_excl_t` per-k0 last symbol). `ISAC_TD_K0_ORACLE_LEGACY=1` restores the old pin | `exclude_key`: **runtime** (BC9d); `certify_k0`: no runtime caller | idem |
| K42 lastset (BC7b `7b5c51b115`, follow-up `4db43db6f5`) | Monotone per-mask last-symbol set (`obs_set_t.lastset`), no restore when the mask was dropped, tail truncation without wipe (`prune_commit_tail`, `apply_cert_from`), type-B latch, g_obs promotion ORs sets | **runtime** | `PdschConfigSweepK42.*` |
| `nr_dci_history.{c,h}` (BC9 `ec67fcbc9f`, `3273bb25f5`, `0676c372b6`; BC9d `852b9bb86f`) | Per-RNTI DL DCI history ring (global, period from SIB1 TDD); accept hook records (`nr_dci_hist_on_accept`); a DCI becomes CONFIRMED when its own grant passes TB CRC on any hypothesis (`nr_pdsch_passive_bc9_confirm` → `nr_dci_hist_on_confirm`), which then applies the TDD per-hypothesis exclusion and DCI-adjacency exclusions against confirmed neighbours only; certified flag (k0-discriminative grant) for levers C/P; kill switch `ISAC_TD_DCI_ADJ=0` | **runtime** (exclusions need SIB1 TDD: inert on phy-test rfsim) | `test_nr_dci_history` (incl. `DciBc9d.SpuriousDciNeverExcludesTruth`) |
| `nr_tdd_pattern.c`, `nr_passive_acq_state.c` TDD (BC9 fix round 1 `b322d1b3e0`) | K40: §11.1 slot shape; `nr_tdd_pdsch_last_symbol`; `nr_passive_acq_tdd_slot_has_downlink` (PDCCH gate) true for flexible slots; SIB1 reference SCS checked by the PDSCH exclusion | **runtime** | `TddSpec.*`, `AcqStateTdd.FlexibleSlotsAreMonitored` |
| `nr_pdsch_passive_bc12_census()` (BC12a) | SIB1 common-TDRA / default-table census at first convergence of a context (log + metrics only) | **runtime** | `test_nr_td_order`, `test_nr_passive_metrics` |
| `tests/nr_td_sim.cc` + `nr_td_sim_test` (levers Task 5 `8c639ec5e7`..`00dd79eed4`; v2 BC8 `490d1536fe`..`f59112743e`, merged `355284580d`) | **Correlated shared-IQ Monte-Carlo simulator linking the real engine.** v1: production catalogue (`init_legal`, 750 entries), runtime oracles (`--oracle 1`; 0 = blind), runtime prior (`--prior`), physical twins (`--twins 2`), gate/K/weights/fieldbook/P2 arms, realism (`--oracle-miss/-wrong`, `--harq-trap`, `--crc-false`), lever arms (`--equiv`, `--fieldbook 0/1/2`, `--inject-wrong-field`, `--crc-accept`, `--geom-pin`, `--sib-pmin/--sib-eps`), traps (`--retx-trap`, `--k0-trap-adj`). v2 (`--slot-model 1`): slot-indexed traffic (`--persist`, `--adjacency`, `--mcs-change`, `--snr-rho`), physical shifted-slot k0 trap, DCI observation (`--dci-miss`, `--dci-false`), TDD (`--tdd none|DDDSU|…`, `--tdd-exclude`), own-slot oracle, other UE (`--other-ue-occ`), k0 ≥ 2 probe layers, `--k0-oracle-legacy`, `--obs-lastset` (K42 mirror, default 1), `--cert-evidence`, `--excl-unconfirmed`, `--cert-confirmed`. Byte-identity vs the v1 reference binary needs `NR_TD_SIM_REF=<path>` (built at `77775006f2`, copy in `/home/nicola/NICOLA/wt/td-bc8-ref/`), else that test is SKIPPED | tool | `nr_td_sim_test` (ctest) |

**Acceleration modules (2026-10-03/04, levers spec §9 items 1–4 and 6, merged on `td/convergence-levers` @`003b8c3f93`).**
Defaults: "branch" = code default at `003b8c3f93`; "main" = default on the merged main per the operator decision of
2026-10-04 (applied on `td/fast-defaults`, §10.2). Tests `[OFFLINE VERIFIED, DGX unit test]` unless stated.

| Module (commits) | Flag (branch → main default) | Purpose and rules | Tests / evidence |
|---|---|---|---|
| **G1 CUDA LDPC pool safety** — `nrLDPC_cuda/ldpc_decoder.cu`, `nrLDPC_coding_cuda_decoder.c`, `nrLDPC_cpu_fallback.c` (`2392d944f3`, `dd1fe1df40`, `312cac9fab`, `54bc0c49e4`; merged `15f2492c6c`) | none (active whenever `libldpc_cuda.so` is the TB decoder; CPU LDPC stays the default TB decoder). Knobs: `LDPC_CUDA_TIMEOUT_MS` (200), `LDPC_CUDA_BREAKER_N` (4), `LDPC_CUDA_BREAKER_S` (5 s), `LDPC_CUDA_WARMUP_ITERS`; test hooks `LDPC_CUDA_TEST_*` | **K34 resolved.** Per-request status (a TB uses GPU bits only when its request rc == 0; slots poisoned 0xA5 as defence in depth); > 512 CBs split over launches; bounded queue (256) → CPU fallback; all-or-nothing slot reservation; every wait timed (**200 ms**): a single timeout or error sends only that TB to the in-plugin CPU decoder (block-for-block identical to `libldpc`); **N = 4 consecutive** timeouts/errors open a 5 s bypass; sticky CUDA error or failed warm-up → disabled for good; a timeout overlapping a CUDA-graph capture counts for nothing. **Warm-up** at init pre-captures BG1 Z=384 / BG2 Z=96 for batch 1..512 and 2×{8,5,10} iterations (+ 2× the configured max). Fixed a 16-byte OOB write in the prep pack (BG1 Z=384) and a queued-timeout slot leak. **`decoder_used`** per TB in `nrLDPC_TB_decoding_parameters_t` (`NRLDPC_DECODER_CPU` = 1, `_CUDA` = 2, 0 unknown), read by `nr_pdsch_passive_decode.c` and counted (`ldpc_tb_cpu`/`ldpc_tb_cuda`). Known edge: a GPU that is always > 200 ms but completes resets the count by late successes, so the breaker never trips (each TB then waits 200 ms → CPU; visible in `ldpc_cuda_fallbacks`) | `test_ldpc_cuda_pool` 20/20 (fault injection, canary, timeouts, breaker) `[MEASURED, DGX GB10]`; BLER table K36 |
| **`nr_llr_norm.h`** (K38 fix `c005d19675`, merged `f929120297`) | none (part of `ISAC_LLR_NORM`, default on) | **K38 fixed.** `nr_llr_norm_num_cb(TBS, BG)`, `nr_llr_norm_span(G, C)` = ceil(G/C) (G when C = 1), `nr_llr_norm_shift(llr, span)`: the LLR-norm shift is estimated over code block 0's span, so a horizon-truncated probe and a full decode get the same shift and bit-identical CB0 inputs. Full multi-CB decodes now estimate the shift from segment 0 (still one uniform shift per TB). Residual: `ISAC_LLR_SCALE` (opt-in) still averages over all G; GPU-LLR probe paths unverified | `test_nr_llr_norm` 5/5; rfsim K38 (§24) |
| **R2 fb2 field-book wiring** — `nr_pdsch_config_sweep.c` (`991dcc1eba`, fix `de9edf9b9d`; merged `334c0bfca8`) | `ISAC_TD_FIELDBOOK=0\|2` (branch 0 → **main 2**; `=0` disables) | Module-level `nr_td_fieldbook_t` under the sweep lock. New contexts: cell/RNTI prior as dormant cause PRIOR, each PROMOTED field as a FIELD cause (TDRA never prunes k0); unsettled contexts drop a FIELD cause when the field leaves PROMOTED; converged contexts keep their winner and are marked untrusted; fail-open via `fail_open_due(1e-3, 0.05)`; one convergence vote per RNTI; probation failure and reopen clear PRIOR/FIELD and re-apply promoted fields. **Epoch bump** (`nr_pdsch_config_sweep_fieldbook_bump_epoch()`) is wired only at the RX-stream discontinuity edge in `executables/nr-ue.c`, so it fires on **every** sync-invalidating gap including SOFT (< 10 ms) ones — **R7 (robustness plan) must REPLACE that call site** with the SOFT/HARD classification (K48). CONVERGED gains ` fb_pruned=0x..` (fb2 only) | `PdschFieldBook.*` (bit-identity off, dormant pruning on, SUSPECT restore, fail-open, pruned context does not vote, TDRA never prunes k0, probation failure); single-UE rfsim cannot promote (no second RNTI): **runtime promotion never exercised** |
| **CB0 elimination engine** — `nr_pdsch_config_sweep.{c,h}` + simulator (`5ffe81aab8`, fix round 1 `2c51f9c4e4`; evidence `599fa8cad3`, `24e090ac1c`) | `ISAC_TD_CB0_ELIM=0\|1` (branch 0 → **main 1**) | Separate per-hypothesis CB0 counters (`cb0_trials`/`cb0_pass`, pass AND fail both count; schedule fixed before outcomes). **Rule:** every active non-leader h with `UB_cb0(h) < LB_tb(leader)` gets dormant cause `NR_TD_DORMANT_ELIM` (never deleted; the full-TB KL rule still elects). **Premise check:** TB PASS ∧ CB0 FAIL on the same (hypothesis, grant) on an admissible batch → `TD_CB0_PREMISE_ALARM`, channel disabled for that context (sticky), eliminations undone. **Per-batch decoder dominance** (replaces the round-0 per-context pin): a batch is admissible iff sens(CB0 decoder) ≥ sens(every TB decoder of the epoch), CPU < CUDA; unknown decoders never qualify; a new more sensitive TB decoder starts a new CB0 epoch. **Admissibility bits** `NR_TD_CB0_X_*` (not_new_rv0, gated, iq_stale, lbrm, rv_retry, prg_ptrs, member_stale, llr_scale, gpu_llr, ldpc_error, rank, + engine decoder/contract): any bit drops the whole grant; the leader's lower bound uses admissible-grant TB counters only (`tba_*`). **Trap-family exemption:** never eliminates a hypothesis sharing (S, L, mapping, DM-RS mask) with the leader (k0 siblings, MCS twins). **Fail-open:** ELIM-only dormancy never triggers fb2 fail-open; fail-open re-arms the channel (fresh CB0 epoch). **Budget split 3-way** (full-TB election, admissible-TB lower bound, CB0): classes × 3, 1e-6 total. Lever E is refused with CB0 elimination (incompatible) | `PdschSweepCb0.*`, `PdschCb0Ctx.*` (mutation-checked), `TdSimCb0.*`; simulator §14.9. **The fix-round-1 Opus re-review was not done** (ledger: usage limit, "TODO before runtime enable") |
| **GrantWork-lite** — `nr_td_grantwork.{h,c}` + decode/queue integration (`ab84175a14`, fix round 1 `480fbdc070`; merged `e5c604d9c7`) | `ISAC_TD_GRANTWORK=0\|1` (branch 0 → **main 1**); debug `ISAC_TD_GW_CHECK=N`, `ISAC_TD_GW_PROBE`, `ISAC_TD_GW_PROFILE`, `ISAC_TD_GW_UNIFIED=0` | One refcounted gw per (grant, k0): shared FEP/chest/demod per geometry signature (≤ 16 signatures), entries EMPTY → COMPUTING → READY (immutable) / FAILED (sticky). **Shared LLRs are descrambled but NOT normalised**; each hypothesis applies its own K38 shift k_h over ceil(G/C_h) (`nr_td_gw_cb0_input`). **Owner-thread rule:** a missing entry is computed lazily only on the gw's creating job thread, before `job_end`; other callers get `E_NOTOWNER`; READY entries are readable from any thread while a reference is held. **Admissibility flags** `NR_TD_GW_F_*` (HARQ, RV_RETRY, ARM, STALE, FULL, per-hypothesis LBRM) sticky on the gw. Owner FEP covers the same symbols as without GrantWork (per-symbol mask), no latency regression. Allocator: `cudaMallocManaged` (libcudart dlopen'd) in a CUDA build, else aligned malloc; pooled by size class. Not eligible: GPU-fed, segmented PRB, PT-RS, CSI-RS RM grants, fingerprint mismatch | `test_nr_td_grantwork` 16/16; rfsim equivalence §14.10 |
| **CB0 batch** — `nr_td_cb0_batch.{h,c,cu}` (`cdd7b594cd`, `b533272939`, round 2 `150bab84b0`, persistent pool `993f78aa71`; merged `4ca2ce2825`) | used by the wiring; `NR_GPU_CB0` / `NR_TD_CB0_CUDA_LDPC` (bench/test paths) | Decodes CB0 of many hypotheses per grant exactly as the receiver's probe (BG, segmentation, E, deinterleave, rate de-match, filler, int8, LDPC, CRC24B / TB CRC when C = 1, all-zero guards). **CPU persistent pool** (threads − 1 workers, no global lock while decoding). **GPU dematch** (`libtd_cb0_gpu.so`, gather kernel) bit-identical to the CPU on 512 mixed items. K38 `llr_shift` k_h applied as given. **Exact per-grant dedup** (key: LLR pointer, k_h, BG, Z, K, F, E, Qm, rv, Ncb, max_it, CRC, K′): ~0 gain on real catalogues `[ANALYTICAL]` (85/86 distinct (Qm, R) entries). **TBS twins (+48 bits at C = 11) pass CB0** — not eliminable (K46). Inconclusive (-1) items never guessed | `test_nr_td_cb0_batch` 9/9 |
| **CB0 CPU wiring** — `nr_td_cb0_backend.c`, `nr_td_cb0_sched.{h,c}`, `nr_td_cb0_adapter.c`, `nr_td_cb0_wire.c` (`e84e8ee35a`, `31bc1e9639`, `312047638b`, `2c4be2ec6f`, `993f78aa71`; merged `16035a1ccb`) | runs when `ISAC_TD_CB0_ELIM=1` and `ISAC_TD_GRANTWORK=1` (else counts `no_grantwork`). `ISAC_TD_CB0_BACKEND=auto\|cpu\|gpu` (auto), `ISAC_TD_CB0_B` (96 items/grant), `ISAC_TD_CB0_BUDGET_US` (20000, used when B = 0), `ISAC_TD_CB0_CPU_PCT` (30 % of online CPUs), `ISAC_TD_CB0_THREADS` (8), `ISAC_TD_CB0_GPU_BACKOFF` (64 batches), `ISAC_TD_CB0_RANK_MAX` (4), **`ISAC_TD_TB_CPU_WHILE_ACQ` (1)** | **Backend interface:** GPU backend if registered, healthy and not backing off, else the CPU backend (libldpc CPU decoder = the TB decoder). Any GPU failure or mixed decoder voids the whole batch (grant inadmissible, never re-decoded) and backs the GPU off; the next grant runs on the CPU. **Per-grant scheduler:** active set snapshotted before the main decode; two-level **hash subset** `mix(seed, slot, geo) % m1 == 0 ∧ mix(seed', slot, hyp) % m2 == 0` (seed = f(config, RNTI, TDA); index-free; a rotation was rejected because it aliases with DDDSU); the scheduled hypothesis always included; ≤ 5 geometries per grant (16-signature limit); hypotheses with another k0 are `not_testable`. **CPU budget:** token bucket on measured CPU-µs; a grant that does not fit is skipped whole. **`ISAC_TD_TB_CPU_WHILE_ACQ=1`** forces the CPU TB decoder while a context acquires, so a CUDA CB0 dominates it. The adapter re-validates against a fresh snapshot (moved context → `reindexed`, nothing credited) and calls `note_tb_decoder` (as an empty grant) for TB outcomes without a batch. CONVERGED gains ` cb0_grants= cb0_adm= cb0_items= cb0_elim= cb0_alarms=`. **Items per grant are ~3, not B** (k0 + geometry cap, K47). x86 build unverified | `test_nr_td_cb0_wire` 16/16 |
| **GPU CB0 entry** — `nrLDPC_cuda/nrLDPC_cb0_cuda.h`, `ldpc_cb0.cu`, `ldpc_cuda_bg.h`, `nr_td_cb0_gpu_backend.c` (`5a1a278452`, `9366e52544`, integration `003b8c3f93`) | registered at wiring init unless `ISAC_TD_CB0_BACKEND=cpu` or `ISAC_TD_CB0_GPU_AUTOREG=0`; `ISAC_TD_CB0_GPU_ITERS` (default 2 × max_iter = 16); `LDPC_CB0_MEM=explicit`, `LDPC_CB0_TIMEOUT_MS` (→ `LDPC_CUDA_TIMEOUT_MS`, 200) + `LDPC_CB0_TIMEOUT_US_PER_ITEM` (100), `LDPC_CB0_BREAKER_N` (4) / `_S` (5 s), `LDPC_CB0_SLOTS` (3) | Separate from G1's TB path. **Async** `ldpc_cb0_submit`/`collect` (3 in flight), **mixed Z** in one submission (sorted by (BG, Z, iters), L2-sized chunks, CUDA-graph cache, one host callback), CRC and all-zero guard on the GPU, low-priority stream. **Algorithm = G1's exactly** (normalised ×3/4 flooding min-sum, int8, 2× iterations), bit-identical to G1's pool; thread-per-node kernels (220 → 76–78 µs/CB0). **Own breaker** (4 consecutive → 5 s bypass; sticky → off), never touches G1's counters. **Memory chosen at run time:** integrated GPU (GB10) → UNIFIED (reads the caller's buffer in place, no copy); discrete (sm_89) or `LDPC_CB0_MEM=explicit` → EXPLICIT (device/managed in place, pinned via async 2D copies, pageable via double-buffered pinned staging); correctness never relies on unified memory (EXPLICIT tested on GB10). Builds for **sm_89 and sm_121** (`LDPC_CUDA_ARCH="89;121"`). **Paired dominance** (same codeword, 104 000 codewords, QPSK AWGN, 4 shapes): CPU-TB-pass ∧ CB0-fail = **0 at cap 16 (default) and 12**, 1 at cap 10, 836 at cap 8 → cap 16 is safe against both TB decoders; **cap 12 only against a CPU TB decoder** (empirical, honoured only while `ISAC_TD_TB_CPU_WHILE_ACQ≠0`; operator decision). Round-2's "CUDA max_it 4 is safe" (aggregate BLER only) is **withdrawn** | `test_ldpc_cb0_cuda` 13/13, `test_nr_td_cb0_gpu_backend` 6/6 `[MEASURED, DGX GB10]`; bench §14.10 |

### 3.4 Test harnesses and scripts (in-tree)

| Path | Purpose |
|---|---|
| `tests/passive_rx/captures/run_arm.sh` | **OTA launcher** for X410 captures: NIC preflight, IRQ pinning, stale-binary check, MPM claim handling, retries, verdicts (`VALID`, `VOID_CFO_MISLOCK`, `VOID_NO_SIB1`, `VOID_DL_ZERO`, `VOID_NO_CPI`, `VOID_DL_RATE`), ADAPT relaunch from `ISAC_ACQ_RETUNE`. **Defaults are sens6-specific** (§5). |
| `tests/passive_rx/captures/{ab_*,run*,rxg_sweep,keep_live,armval}.sh`, `score_ota.py` | A/B and sweep variants around run_arm; OTA scorer |
| `tests/passive_rx/run_passive_rx.sh` | OAI SA rfsim bed (gNB + active UEs + passive receivers, open5gs on sensnuc3) |
| `tests/passive_rx/run_ocudu_passive.sh`, `ocudu/*`, `ocudu_owned_process.py`, `ocudu_bed_matrix.py` | OCUDU gNB + srsUE + passive OAI receiver over ZMQ via a fan-out broker (sensnuc3) |
| `tests/passive_rx/run_bwp_switch.sh` | BWP-switch validation harness (needs telnetsrv) |
| `tests/passive_rx/gnb.sa.rfsim*.conf`, `ue.passive*.conf`, `ue.active*.conf` | rfsim bed configs (rank-4 bed: `gnb.sa.rfsim.100mhz.rank4.conf` + `ue.passive.pin49r4.100mhz.conf --ue-nb-ant-rx 4`) |
| `tests/passive_rx/auto_acquire.py` | Autonomous band/window launcher (probe → derive → reopen → revalidate). `[IMPLEMENTED, NOT VALIDATED]` |
| `tests/passive_rx/raw_baseline/*` | Raw 4-channel IQ recorder, validators, SSB reference checker (python unittest) |
| `tests/passive_rx/offline_sync_contract/*` | Offline sync contract gtest. `build_and_run.sh` is now **arch-aware** (x86 lines verbatim, aarch64 branch = the DGX port; A13, `a1a06e1f88`/`3b6119853f`/`2547ff90aa`); aarch64 branch verified only by an echo-diff (§13.2) |
| `tests/passive_rx/agnostic/run_dci_regression.sh` | CPU-only DCI regression wrapper around `test_nr_pdcch_blind_monitor` |
| `tests/passive_rx/ota/*` | Older OTA configs/scripts (catch-rate, repeatability) — STALE addresses |
| `tests/passive_rx/monitor/*` | Live web monitor (sensing oriented) |
| `tests/sensing_sim/*`, `tests/ota_sync_bench/*` | Sensing simulation scenes/scorers (sensing scope) |
| `tests/passive_rx/dgx/rfsim_arm.sh`, `rfsim_regress.sh`, `score_rx.py` (+`test_score_rx.py`, `fixtures/`) | **In-tree rfsim regression gate** (A1): one agnostic 106-PRB arm, JSON score (sync/ttc/CONVERGED/crc %/drop_full %/CPU/RSS); gate `GATE_CRC_MIN`/`GATE_DROP_MAX` + CONVERGED>=1. Knobs `V4SHIM`, `SCANTHREAD`, `COREMAP` (§10.2). Cloud thresholds 93.0/2.5, DGX 98/1 — **HISTORICAL for the DGX since 2026-10-02 on `td/convergence-levers`: default is now `GATE_MODE=postconv` (§12); the 98/1 rule survives as `GATE_MODE=legacy`.** `score_rx.py` (`a36eeee63e`, `ce0cdfb973`) adds `contexts` (dedup by (rnti, tda), first convergence), `reopens`, `ttc_by_tda`, `postconv_crc_pct`, `postconv_decoded`, `search_crc_pct`, `ldpc_zero_tb`; `python3 score_rx.py --gate <arm_dir>` re-scores a run; calibration + sensitivity in `tests/passive_rx/dgx/README.txt`. **2026-10-04: the 1-RX 150 s thresholds are superseded by the provisional 4-RX gate (420 s arms, §12), which the controller sets in `rfsim_regress.sh` on `td/fast-defaults`** |
| `tests/passive_rx/td_sim/campaign.py` (+`test_campaign.py`) | Runs `nr_td_sim` arm matrices in parallel (≤ 8 processes; never while an rfsim bed runs), writes per-run JSON + summaries (levers Task 5/6; `ablation_p1.json`) |
| `tests/passive_rx/td_sim/gate_p2.py` (+`gate_p2.json`) | Levers Task 7: P2 failure-only probe-evidence Monte-Carlo gate → `results_2026-10-01_p2/` |
| `tests/passive_rx/td_sim/gate_bc.py` (+`gate_bc.json`, `gate_bc_supp.json`) | Reduced BC6 gate runner → `results_2026-10-02_bc/{summary.md,analysis.md,rows.json,supp/}` |
| `tests/passive_rx/td_sim/bc9_campaign.py`, `bc9d_campaign.py` | BC9 simulator part (certified evidence, TDD exclusion) and BC9d (confirmed vs unconfirmed exclusion) campaigns → baseline file sections `## BC9 sim`, `## BC9d` |
| `tests/passive_rx/td_sim/gate_elim.py` (+`gate_elim.json`, `gate_elim_decoder.json`) | CB0 elimination simulator gate (main cells, supplementary, premise arm; incremental `cells.jsonl`, p95) → `results_2026-10-03_elim/`, `results_2026-10-04_elim_r1/` |
| `ldpc_cuda_pool_bler` (build dir; `paired` mode) | Bit-exact BLER harness through `nrLDPC_coding_decoder` (random payload, real CRC, OAI encoder, `crcTableInit`); `paired` feeds the same codeword to the CPU TB decoder, G1's CUDA TB path and the CB0 entry at several iteration caps (§3.3.1 dominance table) |
| `nr_td_cb0_bench`, `nr_td_cb0_gpu_bench` (build dir, not ctest) | CB0 batch throughput: CPU vs GPU, N items per grant, wrong/truth/mixed sets, 106/273 PRB; take the exclusive measurement lock per configuration (§26) |
| `tests/passive_rx/td_sim/baseline_2026-10-01.txt`, `baseline_bc0_2026-10-01.txt` | Simulator baselines (raw summary lines + derived cold/steady): Task 5 v1; sections BC0, BC1 equiv, BC8 v2, BC7b, BC9 sim, BC9d |
| `tests/passive_rx/dgx/v4only_shim.c` | LD_PRELOAD shim: rfsimulator `AF_INET6` server socket → `AF_INET` (container without IPv6) — host-specific (K31) |
| `tests/passive_rx/dgx/run_rx_dgx.sh`, `coremap_dgx.env`, `ue.passive.auto.100mhz.dgx.cfg` (+`test_run_rx_dgx.py`) | **DGX X925 core-map launcher** (A6): `ISAC_UE_RT_CORE`, `ISAC_PDCCH_USS_CORE`, thread-pool, scan/pdsch cores per §14.3, online-CPU checks. Dry-run tests only; **not run on the DGX** |
| `tests/passive_rx/dgx/thrprof.sh` | Per-thread CPU profile (20 s) used by the A7 A/B (same method as the §14.1 DGX profile) |
| `tests/passive_rx/campaign/{campaign.py,verdict.py}` (+`test_campaign.py`) | **Campaign runner** (A4): manifest (commit/branch/dirty/sens6-frozen check), per-run logs/metrics/obs/NIC counters, atomic `run.json`, machine verdicts (VALID / INTERRUPTED incl. stale `running` / …), `summarize`; SIGINT→SIGTERM→SIGKILL escalation |
| `tests/passive_rx/monitor/` `/health` + **Receiver health** tab, `test_health.py` | A5: tails `ISAC_METRICS_PATH`/`ISAC_OBS_PATH` (rotation/reset safe, finite-only rates): acq_state, grants/s, CRC % window, drop_full %, top RNTIs, PRB histogram |
| `CLAUDE.md`, `tests/passive_rx/requirements-dgx.txt`, `tests/passive_rx/.venv` (git-ignored) | A0: agent onboarding (hard rules, build, evidence labels) and the `pyzmq` venv for the monitor |

---

## 4. Build and software environment (§4.1 old machine sens6, §4.2 new DGX Spark)

### 4.0 Two host profiles — sens6 is FROZEN, not retired (operator rule, 2026-09-30)

sens6 **will be used again**. Its configuration and commands are frozen and must stay runnable as-is:

- Frozen reference: git tag **`sens6-frozen-2026-09-30`** (= commit `3b67eeee39`, code `621bdc32a2`) + this file's sens6
  sections: §4.1 (environment), §6.3 (X410 `327C1F2` channel map), §8, §10.3 (exact OTA commands and `run_arm.sh`
  variables), §10.4 (beds), §15.4 sens6 procedure, and `tests/passive_rx/sens6_host_snapshot_2026-09-30/`
  (`ota_confs/`, `launchers/`, `host_network/`).
- **Never edit the sens6 files in place for the DGX.** The in-tree `tests/passive_rx/captures/run_arm.sh`, the
  `tests/passive_rx/*.conf` files, the snapshot `ota_confs/`, `launchers/` and `host_network/` stay byte-identical to
  the tag. DGX-specific settings go into **new** files/directories (e.g. `tests/passive_rx/dgx_host_snapshot_2026-09-30/`,
  a future `run_arm_dgx.sh` or a host-profile env file selected by `HOST=dgx|sens6`), with sens6 as the default where
  a shared script must change. Check with `git diff sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` (must be empty).
- Results stay separated by host (§0.1 rule 5, §27).

### 4.1 The sens6 machine (FROZEN profile; host of all results before 2026-09-30)

Measured 2026-09-30 (full dump: `tests/passive_rx/sens6_host_snapshot_2026-09-30/evidence_2026-09-30/env_sens6.txt`).
This is the environment in which the current validated results were obtained. **Do not try to replicate it on
the DGX** — understand it.

| Item | sens6 value | Architecture-sensitive? |
|---|---|---|
| OS | Ubuntu 26.04.1 LTS | |
| Kernel | `7.0.0-34-realtime` (PREEMPT_RT), cmdline `iommu=pt intel_iommu=on hugepages=2048 isolcpus=domain,managed_irq,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1,4-13` | yes (RT, isolcpus layout is assumed by launchers) |
| Arch | **x86_64** | **YES** |
| CPU | Intel Core Ultra 5 235, 14 cores, 1 thread/core, 1 NUMA node; **AVX2 + FMA + GFNI, no AVX-512**; `nproc`=12 because cores 2–3 are isolated | **YES** |
| RAM | 14 GiB | yes (4-RX 273 PRB receiver is memory/CPU-bound) |
| GPU | NVIDIA GeForce RTX 4060 Ti 8 GB, compute capability **8.9**, driver 580.178.04 (snapshot 2026-09-30). **2026-10-03/04: the operator believes sens6 has an RTX 4070 (~12 GB); the snapshot says 4060 Ti — verify with `nvidia-smi` before GPU tests.** Either way: discrete, PCIe, `sm_89`, **no unified memory** (`cudaMallocManaged` buffers migrate pages over PCIe; the CB0 GPU entry picks its EXPLICIT copy mode there). Build with `LDPC_CUDA_ARCH=89`. GPU runtime of G1 / CB0 entry on sens6 **UNVERIFIED** (test plan §25) | yes |
| CUDA | nvcc 12.4 (`/usr/bin/nvcc`, distro packages under `/usr/lib/x86_64-linux-gnu`) | yes |
| Compilers | gcc/g++ 15.2.0 | |
| Build tools | cmake 4.2.3, ninja 1.13.2 (the sens6 build dirs use **Unix Makefiles**), Python 3.14.4, git 2.53.0 | |
| UHD | **4.10.0** (`UHD_4.10.0.HEAD-0-g2af4ddb9`, source-built into `/usr/local`, with DPDK 25.11) **and** distro UHD 4.9.0 in `/usr/lib/x86_64-linux-gnu`. OAI links `/usr/local/lib/libuhd.so` (cache `UHD_LIBRARIES`). `which -a uhd_*` finds `/usr/local/bin` first. UHD examples (`benchmark_rate`, `rx_samples_to_file`) are in `/usr/local/lib/uhd/examples/` (not on PATH). Images dir `/usr/local/share/uhd/images`. | yes (must match X410 MPM) |
| NICs | Mellanox ConnectX-5 dual port (`mlx5_core`, fw 16.34.1002) `enp2s0f0np0` (port 0, unused) / **`enp2s0f1np1` (X410 data, MTU 9000, rings 8192)**; Intel I219-LM `enp128s31f6` (X410 management, 192.168.1.1/24, dnsmasq); WiFi `wlx…` for internet | yes |
| Limits | `ulimit -l` 8192 kB for the user (receiver runs via `sudo`, root); `@usrp rtprio 99`; 2048 × 2 MiB hugepages | yes |
| OAI build dir | `/home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build` — **its `nr-uesoftmodem` is from 2026-09-26 11:45 and predates HEAD; do not reuse** | |
| HEAD-equivalent build | `/home/sens/NICOLA/agn-wt/cloud/cmake_targets/ran_build/build` (code `621bdc32a2`, built 2026-09-28 22:27) | |

CMake cache of the sens6 builds (both dirs identical in these keys):
`CMAKE_BUILD_TYPE=RelWithDebInfo`, `AVX2=ON`, `AVX512=OFF`, `GFNI=ON` (auto-detected), `ENABLE_TESTS=ON`,
`ENABLE_ISAC_SENSING=ON`, `ENABLE_LDPC_CUDA=ON`, `LDPC_CUDA_ARCH=89`, `CMAKE_CUDA_ARCHITECTURES=52` (root
`CMakeLists.txt` default when CUDA is enabled for SIMU is 90; the cache holds 52 — PTX JIT on the 4060 Ti),
`OAI_USRP=ON`, `OAI_SIMU=ON`, `OAI_RF_EMULATOR=ON`, `OAI_VRTSIM=ON`, `OAI_ZMQ=OFF` (the ZMQ device plugin is built
only on sensnuc3 for the OCUDU bed), `NB_ANTENNAS_RX=4`, `NB_ANTENNAS_TX=4`, `T_TRACER=ON`,
`UHD_LIBRARIES=/usr/local/lib/libuhd.so`, `UHD_INCLUDE_DIRS=/usr/local/include`, `AUTO_DOWNLOAD_ASN1C=OFF`.

The OAI commit is this repository (upstream base `fb944fbad6`, `integration_2026_w32`).

The X410 itself (measured 2026-09-25 by probe/log): product `x410`, serial **`327C1F2`**, FPGA image **`UC_200`**,
MPM **6.1**, FPGA **11.0**, UHD 4.10.0 on both sides, clock/time source internal.

### 4.2 The NEW machine: DGX Spark `spark-74c3` (measured 2026-09-30, the §5.2 commands)

Full dump: `tests/passive_rx/dgx_host_snapshot_2026-09-30/env_dgx.txt`. **This is now the development host.** Location:
DEIB building, Politecnico di Milano campus (Milan, Italy).

| Item | DGX value | Difference vs sens6 that matters |
|---|---|---|
| Product | `NVIDIA_DGX_Spark` (DMI), board P4242, hostname `spark-74c3`, user `nicola`, repo at `/home/nicola/NICOLA/passive_agnostic_rx_for_sensing` | new paths/user (every `/home/sens/...` default in scripts is wrong here) |
| OS / kernel | Ubuntu 24.04.5 LTS, `7.0.0-1019-nvidia` **PREEMPT_DYNAMIC (not RT)**; cmdline has no `isolcpus`/`nohz_full`/`rcu_nocbs`/hugepages | no RT kernel, no isolated cores: the `run_arm.sh` core map (reader on isolated core 2, IRQs 8–13, `taskset 0-7`) does not apply |
| Arch / CPU | **aarch64**, 20 cores, 1 thread/core, 1 NUMA node: **10× Cortex-X925 (big, 3.9 GHz, cpus 5–9 and 15–19) + 10× Cortex-A725 (little, 2.8 GHz, cpus 0–4 and 10–14)**; NEON/ASIMD + SVE/SVE2, no x86 SIMD | real-time threads (RF reader, scan thread, decode consumers) must be pinned to **X925 cores**; default CPU numbering puts little cores first |
| RAM | 121 GiB unified (CPU+GPU share it), 15 GiB swap | 8.6× sens6 |
| GPU | NVIDIA **GB10**, compute capability **12.1** (`sm_121`), driver 580.178.04, CUDA runtime 13.0; memory reported N/A (**unified**: `cudaDevAttrIntegrated = 1`, `PageableMemoryAccess = 1`); 48 SMs, 24 MB L2, LPDDR5x ~273 GB/s | CC 8.9 → 12.1; integrated vs discrete (the CB0 GPU entry chooses UNIFIED here, EXPLICIT on sens6) |
| CUDA toolkit | nvcc **13.0** (V13.0.88) at `/usr/local/cuda` (= `cuda-13.0`) | 12.4 → 13.0 |
| Compilers / tools | gcc/g++ **13.3.0**, cmake **3.28.3**, ninja 1.11.1, Python 3.12.3, git 2.43.0, ccache 4.9.1 | older gcc/cmake than sens6 (15.2 / 4.2.3) — the tree configures and builds fine with them |
| UHD | **4.11.0** (`UHD_4.11.0.HEAD-0-g0d7ed3b1`) source-built in `/usr/local` (`/usr/local/lib/libuhd.so.4.11.0`, tools in `/usr/local/bin`); **no distro UHD** (no shadowing trap) | 4.10 → 4.11: **the X410 MPM must be compat-checked (§7 step 5) before the first stream** |
| High-speed NICs | **2× ConnectX-7 dual-port** (`15b3:1021`, fw 28.45.4028, PCIe Gen5 x4 each = ~126 Gb/s per device): netdevs `enp1s0f0np0`/`enp1s0f1np1` (0000:01:00.x) and `enP2p1s0f0np0`/`enP2p1s0f1np1` (0002:01:00.x). At boot they probe and then **go "Link down" and are torn down ~10 s later (no cables); on 2026-09-30 they were absent from `lspci`/`ip link`** | the X410 data link will land on a CX-7 port: re-check `lspci`/`ip link` after cabling (power-managed when uncabled — `[HYPOTHESIS]`), then MTU 9000, rings, IRQ affinity to X925 cores. NB each CX-7 has only a Gen5 x4 host link (~126 Gb/s) — enough for 4 × 122.88 MS/s sc16 (15.7 Gb/s) |
| Other NICs | Realtek r8127 `enP7s7` 1 GbE (campus LAN, DHCP 10.79.1.144/23, default route), USB RTL8153 `enxfc1928615bb7` 1 GbE (down — candidate X410 **management** port), WiFi `wlP9s9`, `tailscale0`, docker bridges `oai-public` 192.168.71.129/26, `oai-traffic` 192.168.72.129/26, `e2` 192.168.73.1/24 | the `oai-*` bridges suggest an OAI-CN5G docker core exists on this host (not verified: docker needs sudo/group) — would enable the OAI **SA** rfsim bed here |
| Limits | `ulimit -l` unlimited, `ulimit -r` **0** (no SCHED_FIFO for the user); **no passwordless sudo** for the agent | rfsim beds run fine as the user (§13.1); OTA with the X410 will need root (or rtprio limits) for RT priority/`mlockall` |
| X410 | `uhd_find_devices` → *No UHD Devices Found* (2026-09-30): no X410 connected | — |
| Python deps | numpy 1.26.4 present; pyzmq/`libzmq` 4.3.5 present | — |

OAI build on this host (2026-09-30, fresh configure from a clean clone, **no build dir copied**):
`cmake_targets/ran_build/build`, generator **Ninja**, `CMAKE_BUILD_TYPE=RelWithDebInfo`, `ENABLE_TESTS=ON`, `OAI_USRP=ON`,
`OAI_SIMU=ON`, `ENABLE_ISAC_SENSING=ON` (kept ON to be comparable with the sens6 ctest baseline), `ENABLE_LDPC_CUDA=OFF`,
`OAI_ZMQ=OFF`, `NB_ANTENNAS_RX=4`, `UHD_LIBRARIES=/usr/local/lib/libuhd.so`. CMake detected `CPU architecture is aarch64`
and uses `-mcpu=native -march=native`; `AVX2/AVX512/GFNI=OFF`. gtest/benchmark are auto-downloaded (CPM cache
`~/.cache/cpm`), `asn1c` is installed. Targets `nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem`
built with **0 errors in 1 m 30 s** (`ninja -j16`); `ninja tests` built with 0 errors in 27 s (ccache-warm).

---

## 5. Migration to the DGX Spark

### 5.1 What is fundamentally different

The **DGX Spark** is an NVIDIA **GB10 Grace-Blackwell** system: **aarch64 (ARMv9) CPU**, unified CPU/GPU memory,
Blackwell GPU (compute capability 12.x — **verify with `nvidia-smi --query-gpu=compute_cap --format=csv`**), and a
ConnectX-7 class NIC (verify). Everything below that was tuned for x86 must be re-derived. The project's own
future-work spec (2026-09-27) already warned: *"Unified-memory ARM options (e.g. DGX Spark / GB10): strong for
channelization + GPU pipeline + sensing, weak CPU for today's CPU-bound decode; viable only after the decode
chain is on the GPU, or as a front end feeding x86 decode hosts. Needs ARM64 validation of UHD/DPDK/X410."*
Treat that as `[HYPOTHESIS]` to be measured, not a verdict: the GB10 CPU cores may or may not keep up with one
4-RX 100 MHz receiver instance — **measure it** (§5.4).

### 5.2 First commands on the DGX (record the output into this file, §4-style table)

```bash
uname -a; uname -m; cat /etc/os-release
lscpu; free -h; nproc
nvidia-smi; nvidia-smi --query-gpu=name,driver_version,memory.total,compute_cap --format=csv
nvcc --version || true
gcc --version; g++ --version; cmake --version; python3 --version; ninja --version || true; git --version
ip -br addr; ip route
lspci; lsusb
ethtool -i <each-nic>; ethtool <each-nic>
cat /proc/cmdline
ulimit -l; ulimit -r
which -a uhd_config_info uhd_find_devices uhd_usrp_probe; uhd_config_info --version || true
```

### 5.3 Components that may need adaptation (with the exact places in the repo)

| Component | sens6 assumption | Where | DGX action |
|---|---|---|---|
| CPU arch / SIMD | x86 AVX2/GFNI via SIMDE, `-march=native`, `-mgfni`, `-DSIMDE_X86_*_NATIVE` | root `CMakeLists.txt` ~l.159–222 (ARM branch uses `-march=armv8-a+simd` / `armv8.2-a` / native) | Configure from scratch (never copy a build dir: a copied x86 cache SIGILLs in `check_vcd`). Expect ARM64 SIMDE paths; run the full ctest before anything else. |
| Hand-written x86 flags | `tests/passive_rx/offline_sync_contract/build_and_run.sh` hard-codes `-mgfni -march=native -DSIMDE_X86_*` | that script | Rewrite for aarch64 or build through CMake. |
| `idsweep_offline.c` build line | `gcc -O3 -march=native -fopenmp` (fine on ARM) + CUDA `idsweep_gpu.cu` | snapshot `discovery_tool/` | Rebuild; set `-arch=sm_<cc>` explicitly. |
| CUDA arch | `LDPC_CUDA_ARCH=89` (default in `openair1/PHY/CODING/CMakeLists.txt:33`), `CMAKE_CUDA_ARCHITECTURES=52/90` | CMake cache | Set to the GB10 compute capability; CUDA 12.4 may be too old for Blackwell — use the DGX OS CUDA toolkit. `ENABLE_LDPC_CUDA` is optional (measured slower for single full TBs); since 2026-10-04 it also provides the GPU CB0 backend (§3.3.1, §9). nvcc 12.4 (sens6) cannot target sm_121: build per host. |
| UHD | Source-built 4.10.0 in `/usr/local` + distro 4.9 in `/usr` (shadowing trap) | CMake `UHD_LIBRARIES`/`UHD_INCLUDE_DIRS` | Install **one** UHD whose MPM compat matches the new X410's MPM/FPGA (probe first, §7). Point CMake at it explicitly. |
| UHD transport | kernel sockets (`DPDK` empty in run_arm; `use_dpdk` not configured) | `run_arm.sh` | Start with kernel sockets. DPDK on ARM is a later optimisation. |
| NIC names | `enp2s0f1np1` (data), `enp128s31f6` (mgmt); older scripts still carry `enp129s0f0np0`, `enp101s0f1np1` | `run_arm.sh` `NIC=`, `rxg_sweep.sh`, `armval.sh`, host scripts | Pass `NIC=` explicitly; fix defaults later. |
| IPs | data `192.168.20.2` (X410) / `192.168.20.1` + `192.168.20.65` (host); mgmt `192.168.1.140` (X410, dnsmasq lease) / `192.168.1.1` (host). Stale in older scripts: `192.168.10.2`, `128.178.122.3/.20/.174`, `192.168.10.45` | `run_arm.sh`, `tests/passive_rx/ota/*`, `ab_*.sh` | Discover the new X410's addresses (§7) and pass `DATA=`, `MGMT=`. |
| Paths | `/home/sens/NICOLA/...` in ~40 scripts (`run_arm.sh` `BASE=/home/sens/NICOLA/captures`, `REPO` default **`/home/sens/NICOLA/openairinterface5g-total-passive-ue`** (!), `cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets`), `ssh sens4 stat /home/sens/NICOLA/gnbLogs/gnb.log` | `tests/passive_rx/captures/*.sh`, `run_passive_rx.sh`, `run_ocudu_passive.sh`, `sensing_sim/_run_*.sh` | Always set `REPO=`/`BIN=`; remove or make optional the `ssh sens4` bracket. |
| Runtime tmp paths (C code) | `/tmp/passive_rx` (SIB1 cache, idsweep snapshots, reports), `/tmp/coresets_discovered.txt` (discovery hand-off), `/tmp/oaiue_sensing`, `/tmp/pbch_capture.bin`, `/tmp/pdcch_fixture.bin` | `nr_pdcch_blind_monitor.c`, `nr_pdcch_coreset_map.c`, `nr_isac.cc`, `phy_procedures_nr_ue.c`, `dci_nr.c` | Fine on Linux; `/tmp/passive_rx` is root-owned when the receiver runs as root — user tools cannot write there. |
| Usernames/hosts | `sens`, `sens4`, `sens6`, `sensnuc3`, `root@<mgmt>` for `systemctl restart usrp-hwd` | scripts | Adapt. |
| Real-time | PREEMPT_RT kernel, `isolcpus=2-3`, reader thread pinned to core 2 (`ISAC_UE_RT_CORE`), thread pool `--thread-pool 0,1,4,5,6,7`, NIC IRQs pinned to cores 8–13, softmodem `taskset 0-7`, governor `performance` | kernel cmdline, `run_arm.sh`, `cpu-performance-governor.service` | Re-derive a core map for the GB10 (big/little cores? check `lscpu`). An RT kernel may not be available on DGX OS; measure drops without it first. |
| Memory locking | receiver calls `mlockall(MCL_CURRENT|MCL_FUTURE)`; runs as root | `nr-uesoftmodem.c` | Root avoids the memlock limit; long raw-IQ replay must not inherit `MCL_FUTURE`. |
| Kernel net tuning | `rmem_max/wmem_max 250 MB`, `rmem_default 62.5 MB`, `netdev_max_backlog` 5000 (sysctl.d) / 250000 (run_arm) | `host_network/90-x410-net.conf`, `run_arm.sh`, usrp_lib calls `sysctl -w` | Re-apply persistently. |
| Hugepages | 2048 × 2 MiB reserved (for DPDK; unused by kernel-socket runs) | kernel cmdline | Optional. |
| NIC tuning | MTU 9000, rings 8192/8192, NM profile `x410-data` | `host_network/*` | Re-create; verify with `ping -M do -s 8972 <x410-data-ip>`. |
| Python deps | numpy (raw baseline tests, scorers), pytest/unittest; OCUDU harness uses pyzmq | `tests/passive_rx/*.py` | `pip install numpy pyzmq` as needed. |
| System libs | libconfig, libsctp, gtest/gmock, google-benchmark, blas/lapack(e), fftw3, libcap, zmq, ccache, boost (UHD) | OAI | `cmake_targets/build_oai -I` may not support aarch64 packages cleanly; install manually if needed. |
| Datasets | Raw OTA IQ recordings referenced by earlier docs (`/home/sens/NICOLA/captures/raw_fullband_4s.*`, `raw_batch.*`, `adaptive_ul_dl_mrc2.*`) **are no longer on sens6's mounted filesystems (checked 2026-09-30)**; an unmounted 1.8 TB disk `sda1` exists and was not inspected. A 13 GB synthetic hidden-DCI set exists at `sens6:/home/sens/NICOLA/hidden_dci_sim_20260922/`. OTA capture logs: `sens6:/home/sens/NICOLA/captures/` (4.5 GB). | — | Decide what to copy (rsync) before sens6 is repurposed. |

### 5.4 Migration acceptance (do these in order; stop and diagnose at the first failure)

1. Configure + build **from a fresh clone** (no copied build dir). `nr-uesoftmodem`, `oai_usrpdevif`,
   `rfsimulator`, `params_libconfig`, `tests`.
2. `ctest` — expect the §13 baseline (125/126, `test_vrtsim_cirdb` known failing). Any other failure on ARM is a
   **portability defect** to fix before OTA.
3. Measure the receiver's CPU cost on the DGX offline (phy-test rfsim bed §10.4 at 106 and 273 PRB), compare with
   sens6 numbers: phy-test TD converges in ~1.3 s with `drop_full` ≈ 0.3 %. A large `drop_full`/`over_slot`
   increase means the ARM CPU cannot carry the decode chain as it stands.
4. Characterize the new X410 and the link (§7) with `benchmark_rate` — 0 drops at 4 × 122.88 MS/s for 300 s.
5. Reproduce the latest known lab OTA milestones (§15.4) before any development.

**Status 2026-09-30:** 1 ✔ (fresh Ninja build, §4.2); 2 ✔ with 3 ARM failures triaged (§13.1, K21/K22); 3 ✔ at 106
and 273 PRB 1 RX (§14.1 — the ARM CPU keeps up; limits are serial threads, K27), 4-RX bed not synced (§14.2);
4 ✗ no X410 yet; 5 **replaced**: the lab is unavailable → Milan-cell survey (§15.4).

---

## 6. X410: how it works in this project

### 6.1 Generic X410 knowledge (applies to the new unit)

- **Architecture:** X410 = Xilinx RFSoC-based USRP (X4xx family, product `x410`), two ZBX daughterboards (**A**, **B**),
  each with 2 RX/TX chains → 4 RX channels. Runs its own Linux ("MPM", `usrp-hwd` systemd service) that UHD talks
  to over RPC on the **management** interface; samples flow as **CHDR over UDP** on the **QSFP28** data ports
  (up to 100 GbE each). `uhd_find_devices` shows both `addr` (data) and `mgmt_addr`.
- **Ports per channel:** RX-capable connectors `TX/RX0` and `RX1` (plus internal `CAL_LOOPBACK`, `TERMINATION`).
  TX only on `TX/RX0`. **B2xx names (`RX2`, `TX/RX`) segfault libuhd on an X410** — the driver switch handles this.
- **Clocking:** master clock rate (MCR) depends on the loaded FPGA image. `UC_200` (DDC present) → MCR 245.76 MHz; the
  requested sample rate is derived by DDC decimation: 122.88 MS/s = /2 (273 PRB @ 30 kHz), 61.44 = /4, 30.72 = /8
  (51 PRB). 23.04 MS/s is **not** reachable at this MCR. A `CG_400`-type image has no DDC, MCR 491.52 MHz fixed.
  Clock/time source: internal (no GPSDO by design; receivers are autonomous).
- **Bandwidth:** OAI config rx_bw 100 MHz at 122.88 MS/s (the device readback reports 400 MHz analog bandwidth).
- **Link budget:** sc16 = 4 B/sample. 1 ch × 122.88 MS/s = 3.93 Gb/s; 4 ch = 15.7 Gb/s → needs 10/25/100 GbE; the
  1 GbE management port **cannot** carry even 20 MHz (983 Mb/s at 30.72 MS/s).
- **Gain:** RX gain range 0–60 dB on ZBX; requesting ≥ 60 is silently clamped.
- **Multi-channel streaming:** one RX streamer with channels 0..N−1; OAI maps OAI antenna i → UHD channel i
  (override with `ISAC_RX_CHAN_MAP="a,b,c,d"`). RFNoC streams **halt** on an overflow (unlike B2xx); a new stream
  command is needed.

### 6.2 How OAI talks to the X410 (this repo)

- Command line: `--usrp-args "type=x4xx,addr=<data-ip>,mgmt_addr=<mgmt-ip>"`, `-r <PRB>` (sets sample rate),
  `--numerology 1`, `--band 78`, `-C <carrier Hz>`, `--ssb <subcarrier>` or `--ue-scan-carrier`,
  `--ue-rxgain <dB>`, `--ue-nb-ant-rx <1|2|4>`, `--ue-nb-ant-tx` (keep equal; TX is suppressed in passive mode),
  `--passive-rx`.
- `device_init()` builds args (`master_clock_rate` from the FPGA type reported by discovery), sets rates per
  channel, tunes all channels, sets gains with range check, forces `RX1`, logs `RFCHAN` readback per channel.
- Reader thread (`UEthread_0`) is pinned (`ISAC_UE_RT_CORE`, default 2 in run_arm); `trx_usrp_read()` requests
  61440 samples/slot at 273 PRB; short reads are tolerated; `ERROR_CODE_OVERFLOW`/`TIMEOUT` produce
  `RFSTALL …` and the process aborts cleanly ("UE main thread is ending") rather than decoding garbage.
- The driver is a **dlopen'd plugin** (`liboai_usrpdevif.so`). Rebuild it explicitly (`make oai_usrpdevif`)
  after any `usrp_lib.cpp` change, or the old code keeps running.

### 6.3 Channel mapping currently used (sens6 X410 `327C1F2`, from `RFCHAN` + UHD, 2026-09-25)

| OAI antenna | UHD channel | Daughterboard | Subdev | Port | DSP |
|---|---|---|---|---|---|
| 0 | 0 | A | 0 | RX1 | 0 |
| 1 | 1 | A | 1 | RX1 | 1 |
| 2 | 2 | B | 0 | RX1 | 2 |
| 3 | 3 | B | 1 | RX1 | 3 |

### 6.4 1 RX vs 2 RX vs 4 RX (what changed between them on the old unit)

- **1 RX:** the most reliable for acquisition and decode on the old unit (locked on every start at RXG 49 in the lab,
  2026-09-14). PDSCH decode uses branch 0 only.
- **4 RX:** needed for AoA (sensing) and MRC. On the old unit it exposed: acquisition window landing ±342 samples off
  (fixed by gated rebase), CORESET#0/SIB1 failures (0 SI hits at 4-RX on 2026-09-15), per-branch CFO after a
  retune (only ch0 was retuned — fixed `9d6a2340c5`), RX branch imbalance (ch1/ch3 8–15 dB weaker, physical),
  MRC fixed-point defects (fixed), and CPU overload (68 % PDSCH queue drops in one 7-min run). Current default
  decoder policy is branch 0 (`ISAC_RX_MRC_MODE=0`, run_arm `MRC=0`).
- **2 RX:** used only diagnostically (SIB1 found on the 47th SI candidate with the two weak antennas).

---

## 7. How to discover and characterize a NEW X410 (start from zero; do not flash anything first)

1. **Physical:** QSFP28 data port → host NIC (100 GbE DAC/AOC), RJ45 management → host management NIC or LAN.
   Antennas on the `RX1` ports of every channel you intend to use (the code forces `RX1`). Note the
   daughterboard/channel ↔ connector labels.
2. **Management address:** X410 mgmt gets DHCP. On sens6 it came from a host dnsmasq on the management NIC
   (`host_network/enp128s31f6-mode server` → 192.168.1.1/24, pool .50–.150). Find it:
   `uhd_find_devices` (broadcast) or the DHCP lease file; then `ssh root@<mgmt-ip>` works (MPM Linux).
3. **Discover:**
   ```bash
   uhd_find_devices                                   # expect type x4xx, product x410, serial, fpga=UC_200?, mgmt_addr, addr
   uhd_find_devices --args "type=x4xx,addr=<data-ip>"
   uhd_usrp_probe --args "type=x4xx,mgmt_addr=<mgmt-ip>,addr=<data-ip>"   # CLAIMS the device - never while a receiver runs
   ```
   Record: **serial**, **product**, **FPGA image type** (`fpga=` field; UC_200 was used), **MPM version**, **FPGA
   version**, UHD version on host and device (`mpm_version`, `fpga_version`, `rpc_version`), daughterboards, LO
   lock, clock/time source, temperatures (`ssh root@<mgmt> 'cat /sys/class/hwmon/*/temp*_input'` or probe output).
4. **Host networking:**
   ```bash
   ip -br addr; ip route; ip route get <x410-data-ip>
   ethtool <data-nic>            # Speed: 100000Mb/s, Link detected: yes
   ethtool -i <data-nic>; ethtool -g <data-nic>; ethtool -S <data-nic> | egrep -i 'miss|drop|discard|out_of_buffer|err'
   cat /sys/class/net/<data-nic>/statistics/rx_missed_errors
   ping -c3 -M do -s 8972 <x410-data-ip>     # proves MTU 9000 end to end
   ```
   Check: correct NIC (the one with carrier), link speed, subnet (host and X410 data port on the same /24),
   the route to the data IP goes out the data NIC (**not** the management NIC — a silent fallback to 1 GbE starves
   the stream), MTU 9000, rings at max, zero missed/out_of_buffer deltas during a test.
   Pin the address with a NetworkManager profile (on sens6 NM silently removed transient `ip addr add`s).
5. **UHD compatibility:** host UHD major/minor must match the device MPM compat; a mismatch aborts with
   `MPM major compat number mismatch`. On the old setup a stale UHD 4.8 in `/usr/local` shadowed the distro 4.10 —
   check `ldd nr-uesoftmodem | grep uhd`, `which -a uhd_find_devices`, `uhd_config_info --version`.
   Only if the image is incompatible: `uhd_image_loader --args type=x4xx,mgmt_addr=... --fpga-path ...` (after
   downloading matching images with `uhd_images_downloader`) — **ask before reflashing**.
6. **Streaming proof (no OAI):**
   ```bash
   /usr/local/lib/uhd/examples/benchmark_rate --args "type=x4xx,addr=<data-ip>,mgmt_addr=<mgmt-ip>" \
       --rx_rate 122.88e6 --rx_subdev ... --channels 0,1,2,3 --duration 300 --rx_cpu sc16
   # expect 0 dropped, 0 overruns, 0 sequence errors; NIC missed delta 0
   ```
7. **RF proof (antenna actually connected):** `rx_samples_to_file --args ... --freq <cell Hz> --rate 30.72e6
   --gain 43 --duration 1 --ant RX1 --channel N --file /tmp/chN.dat` per channel, then RMS in dBFS (file is
   **int16 I/Q pairs**, not complex float):
   `python3 -c "import numpy as np;d=np.fromfile('/tmp/ch0.dat',np.int16).astype(np.float32);iq=d[0::2]+1j*d[1::2];print(10*np.log10((abs(iq)**2).mean()/32768**2))"`.
   Compare against the `TERMINATION` port (noise floor) per channel to separate chain health from antenna
   signal. Record the per-channel imbalance for this unit; do not carry over the old unit's numbers.
8. **Settling:** after `systemctl restart usrp-hwd` (on the X410) wait **~180 s** before judging acquisition;
   after killing a capture wait **≥ 60 s** and check `uhd_find_devices` shows `claimed: False`.

---

## 8. X410 known problems and debugging (from the OLD unit(s); re-verify everything on the new one)

HISTORICAL context: two X410 units appear in the records — serial `327B872` (sens3, Aug 2026, 10 GbE/1 GbE
addresses, labelled "retired unit" in scripts) and `327C1F2` (sens6, Sep 2026). Nothing below is a property of
the new unit until measured.

| # | Symptom | Diagnosis / commands | Likely causes | How it was solved | Re-verify on new X410 |
|---|---|---|---|---|---|
| X1 | Device visible (`uhd_find_devices`) but streaming dead / every `recv()` timeout | `benchmark_rate`; `ip route get <data-ip>`; `ethtool <nic>` | Data traffic falling back to the **1 GbE mgmt port** (route/IP lost), wrong NIC, NIC down | Persistent NM profile for the data IP; preflight in `run_arm.sh` asserts IP/MTU/rings | Yes |
| X2 | Stream starts then `ERROR_CODE_OVERFLOW (Out of sequence)` storms, 61 % samples lost | `ethtool -S` `rx_missed_errors`/`out_of_buffer` delta | MTU 1500 + 1024-desc ring at 4 × 122.88 MS/s | MTU 9000, rings 8192, `netdev_max_backlog`, `rmem_default` (§5.3) | Yes |
| X3 | One overflow then the process sits alive and silent after a successful sync | log: one `ERROR_CODE_OVERFLOW` then endless `ERROR_CODE_TIMEOUT` | **RFNoC halts the stream on overflow** (B2xx doesn't); OAI never re-issued start | Code now detects `RFSTALL` and ends the run; launcher retries | Yes (behaviour is UHD/RFNoC-generic) |
| X4 | Periodic overflow + 5 s timestamp jump at 4 ch | pairs of truncated read + jump | Host not draining (IRQs on the reader's core, CPU starvation) | IRQs of the **live** port pinned to cores 8–13 (read from `/sys/class/net/$NIC/device/msi_irqs`, not a hard-coded list), reader pinned to isolated core 2, performance governor | Yes (host-specific) |
| X5 | `RFSTALL USRP_RX_START … ERROR_CODE_OVERFLOW (Out of sequence error)` at stream start | appears right after a previous run was SIGKILLed or after a NIC retune | Stale MPM claim; NIC link flap after MTU/ring change | Wait 60 s + check `claimed: False`; sleep 30 s after any NIC change; SIGTERM/SIGINT not SIGKILL | Yes |
| X6 | `device_init failed … Someone tried to claim this device again` | MPM journal `ssh root@<mgmt> journalctl -u usrp-hwd` | A second claimant (probe or a leftover process); **a second claim makes MPM kill itself (~100 s down)** | Never probe while a receiver runs; release gracefully; restart `usrp-hwd` only after a start-up death, then wait 180 s | Yes |
| X7 | Mid-run: IQ continues with perfect timestamps but constant near-zero level; DCI accepts drop to 0 | ANTPOW collapses to ~5 vs 70–300; mgmt RTT spikes > 2 s | **Lost MPM claim** (RPC timeout on a slow mgmt link) — UHD keeps returning zeros | Direct mgmt link (no campus LAN), watchdog on PBCH decode count | Yes |
| X8 | PSS found but PBCH never decodes, repeatedly, right after an MPM restart | back-to-back failures only after restart | Front end not settled | Wait ~180 s after `usrp-hwd` restart | Yes |
| X9 | 4-ch: SIB1 never found although PBCH 50/50 | `TSYNC_OBS e_win_frac≈0.06 peak_oow=1`; `ISAC_TSYNC_AUDIT` | Acquisition window lands ±342 samples (N/12) off the true CIR peak | Gated global rebase (`nr_adjust_synch_ue.c`), `ISAC_TSYNC_REBASE_RATIO` | Test on new unit |
| X10 | Channels 1–3 rotate against channel 0 (MRC destroys decode) after CFO retune | `BRANCHFO d_vs_br0=[0 588 653 652] Hz` | Driver retuned channel 0 only | `usrp_set_rx_freq_all()` (`9d6a2340c5`); rebuild the plugin | Verify BRANCHFO ≈ 0 |
| X11 | Branch imbalance: ch1/ch3 8–15 dB below ch0/ch2 | `ANTPOW`, `RFPOW`, `rx_samples_to_file` on RX1 vs TERMINATION | Physical (antennas/cables on those RX1 ports); chains themselves within 1 dB on TERMINATION | Not solved; decode on branch 0 (`MRC=0`) | Measure; do not assume |
| X12 | 4-RX decode near 0 % while 1-RX decodes | PDSCHQ drops, per-branch power | CPU overload (FEP/chest ×4) + imbalance + MRC defects | Per-antenna FEP/chest threading, saturating MRC, nvar fix, branch-0 decoding | Re-measure |
| X13 | MCS 25 (256QAM) grants 0 % at RXG 40 | `PRECLIP mean_mag≈4.9 LSB` | Integer quantisation at low digital level (~23 dB SQNR) | `RXG=49` (lab), `ISAC_CHEST_TINTERP=1 ISAC_SFO_CORRECT=1` | Gain is unit/cell/antenna specific |
| X14 | Gain request ≥ 60 silently clamped; wideband PBCH tracking broke | `RFCHAN gain=` readback | ZBX range 0–60 | Use readback, not request | Yes |
| X15 | Host SSH/network stalls under X410 traffic; mgmt RTT spikes | ping mgmt during a run | Shared/busy mgmt path | Direct mgmt segment | Yes |
| X16 | Data NIC at 99–108 °C (crit 105) | `cat /sys/class/hwmon/hwmon*/temp1_input`; kernel `temp_warn` | ConnectX-5 cooling | Not solved on sens6 (flagged) | Check the DGX NIC temperature under load |
| X17 | X410 mgmt IP disappears after host reboot | `systemctl is-enabled dnsmasq` → disabled | dnsmasq not enabled at boot | Manual `enp128s31f6-mode server` | Configure DHCP/static properly |
| X18 | Corrected-bit errors on one QSFP28 lane | `ethtool -S … rx_err_lane_2_phy` | Marginal DAC seat | Not an issue yet | Check |
| X19 | Wrong UHD picked up | `ldd`, `which -a`, `MPM major compat mismatch` | `/usr/local` vs `/usr` installs | Point CMake at one UHD | Yes |
| X20 | Blind GSCN scan at 273 PRB × 4 RX segfaults / starves the reader | backtrace in `nr_initial_sync.c` malloc16 | ~1.6 GB concurrent scan buffers; 6 scan workers starve reader | Pin `--ssb`; reader on isolated core | Yes |

Classifying an RF-level failure from logs: **RF problem** = RFPOW/ANTPOW at noise floor with a clean stream
(antenna/cable/frequency); **network problem** = NIC missed/out_of_buffer deltas, route/MTU wrong; **UHD/MPM
problem** = claim errors, RPC timeouts, compat mismatch, zeros with continuous timestamps; **OAI problem** =
clean `benchmark_rate` + healthy RFPOW but PBCH/PDCCH failure (then look at §11 sync logs).

---

## 9. How to build the receiver

There is one build system (OAI CMake). "Modes" are CMake options, not separate programs:

| Build | Options | Produces |
|---|---|---|
| Receiver (passive, no sensing) | `-DENABLE_ISAC_SENSING=OFF` (root default) | `nr-uesoftmodem` with a no-op sensing stub. **12 sensing test binaries fail to link in this mode** (expected). |
| Receiver + sensing (as on sens6) | `-DENABLE_ISAC_SENSING=ON` | full CFR→sensing engine |
| Offline tests | `-DENABLE_TESTS=ON`, target `tests` | gtest binaries + `ctest` |
| GPU LDPC (optional) | `-DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=<cc>` (DGX `121`, sens6 `89`; a list such as `"89;121"` is accepted and validated) | `libldpc_cuda.so` plugin (G1 TB pool + CB0 entry), `libtd_cb0_gpu.so` (CB0 GPU dematch), `pdsch_gpu`, `pdcch_gpu`, `polar_sc_cuda`, tests `test_ldpc_cuda_pool`, `test_ldpc_cb0_cuda`, `test_nr_td_cb0_gpu_backend`. **Needed for the GPU CB0 backend** (`ISAC_TD_CB0_BACKEND=auto` falls back to the CPU backend and logs "not available, CPU only" when the plugin is absent); in rfsim the receiver found the plugin with `LD_LIBRARY_PATH=<build dir>`. The acceleration round was built and tested with `-DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121` (§13.4) |
| rfsim beds | `-DOAI_SIMU=ON` + targets `rfsimulator` (+ `nr-softmodem` for the gNB) | `librfsimulator.so` (dlopen'd) |
| OCUDU/ZMQ bed (sensnuc3 only) | `-DOAI_ZMQ=ON`, target `oai_zmqdevif` | ZMQ device plugin |

Fresh configure (copy-paste; adjust UHD paths and CUDA arch on the DGX):

```bash
git clone git@github.com:nico-net/passive_agnostic_rx_for_sensing.git
cd passive_agnostic_rx_for_sensing && git checkout adaptive-rx-UL-DL
cd cmake_targets
# first time only (installs OAI deps; on aarch64 verify the package list): ./build_oai -I
mkdir -p ran_build/build && cd ran_build/build
cmake ../../.. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON -DOAI_USRP=ON -DOAI_SIMU=ON \
  -DENABLE_ISAC_SENSING=OFF \
  -DUHD_LIBRARIES=/usr/local/lib/libuhd.so -DUHD_INCLUDE_DIRS=/usr/local/include
ninja nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem
ninja tests            # offline suite
ctest -j4 --output-on-failure
```

Equivalent via OAI's script (what sens6 used originally): `./build_oai --nrUE -w USRP -c` then enable tests/options
with `cmake -D... .` in `ran_build/build`. The sens6 trees used `make -j12 <target>`; lane builds used
`/home/sens/NICOLA/agn-wt/lane-make.sh <lane> <targets>` (waits while a receiver runs, `make -j4`, shared ccache).

Expected artifacts in the build dir: `nr-uesoftmodem`, `nr-softmodem`, `liboai_usrpdevif.so`, `librfsimulator.so`,
`libparams_libconfig.so`, `libldpc*.so`, test binaries `test_nr_*`.

Known build facts:
- Physim targets `nr_psbchsim`, `nr_ulsim`, `nr_srssim`, `nr_ulsim_mu_mimo` do not link (pre-existing; physim
  links `libPHY_NR_UE.a` without the new libs). Build explicit targets, not `all`.
- **Stale-plugin trap:** `radio/USRP/usrp_lib.cpp` → `liboai_usrpdevif.so`, `radio/rfsimulator/*` and
  `SIMULATION/TOOLS/sensing_channel.c` → `librfsimulator.so`, ZMQ → `liboai_zmqdevif.so`. Rebuilding only
  `nr-uesoftmodem` leaves the old plugin running.
- **Never build while a capture is running on the same host** (build jobs starve the RX thread; captures become
  silently invalid). `run_arm.sh` refuses to start if any source is newer than the binary.
- A tree copied with its build dir from another CPU must be reconfigured (CMake caches CPU features).
- Copied build dirs can hit an nvcc link bug on stale `pdsch_gpu`/`pdcch_gpu` `.cu.o` — delete those objects.

---

## 10. How to run the receiver

### 10.1 Command-line arguments that matter (all exist in HEAD)

| Argument | Meaning | Agnostic status |
|---|---|---|
| `--passive-rx` | Receive-only mode (SA modifier). Mandatory. | — |
| `-r <N_RB>` | Carrier PRBs → sample rate (273 → 122.88 MS/s, 217 → 122.88, 106 → 61.44, 51 → 30.72). | **Operator-supplied at start**; SIB1 check emits `ISAC_ACQ_RETUNE` on mismatch (launcher relaunches once) |
| `--numerology 1` | SCS 30 kHz | Supplied (FR1 n78 cells here are all 30 kHz) |
| `--band 78` | NR band | Supplied (search domain) |
| `-C <Hz>` | RF centre = **carrier/BWP centre**, not the SSB frequency | Supplied (derived & verified from SIB1) |
| `--ssb <k>` | `ssb_start_subcarrier` in the started grid, **not an ARFCN**: `(f_SSB − f_PointA)/SCS − 120` for a grid starting at Point A; confirm with the UE's own `SSB SC offset` log | Supplied, or replaced by `--ue-scan-carrier` |
| `--ue-scan-carrier` | Blind GSCN scan inside the RF window | Agnostic, but risky at 273 PRB × 4 RX (§8 X20) |
| `--ue-rxgain <dB>` | RX gain (X410 0–60) | Receiver setting, cell/site dependent |
| `--ue-nb-ant-rx <1/2/4>`, `--ue-nb-ant-tx` | Antenna count (keep TX = RX) | Receiver setting |
| `--ue-fo-compensation` | Enable CFO compensation | — |
| `--initial-fo <Hz>` | CFO accumulator seed, **same sign as the reported offset** (this rig: −15000) | Receiver/oscillator knowledge; blind start also works but ~1 in 3 |
| `--cont-fo-comp <n> --freq-sync-P --freq-sync-I` | Upstream continuous digital FO compensation (no LO retune) | optional |
| `--time-sync-I 0.01` | Timing-loop integral gain | used in all OTA runs |
| `--ntn-initial-time-drift -4.25` | Initial sampling-drift seed (used in all OTA runs) | receiver knowledge |
| `-A 90` | Timing advance offset used by OAI UE (kept from upstream practice) | — |
| `--thread-pool 0,1,4,5,6,7` | Worker cores (leave isolated cores for the reader) | host specific |
| `--sync-actor-core`, `--dl-actor-core-start`, `--ul-actor-core-start` | Actor pinning | host specific |
| `--usrp-args "type=x4xx,addr=…,mgmt_addr=…"` | Device | host specific |
| `-O <conf>` | Receiver config (`sensing = { pdcch_blind_monitor_* … }`) | §10.2 |
| `--rfsim`, `-E` (three-quarter sampling), `--device.name oai_zmqdevif` | Simulation beds | — |
| `--loader.ldpc.shlibversion _cuda` | GPU LDPC plugin | optional |

### 10.2 The receiver config (`sensing = { … }` block)

Keys (parsed in `nr_isac.cc` / `nr_pdcch_blind_monitor.c`): `enable`, `pdcch_blind_monitor_autoconf` (CSS0 from
MIB/SIB1), `pdcch_blind_monitor_autodiscover` (dedicated CORESET discovery), `pdcch_blind_monitor_full_auto`,
`pdcch_blind_monitor_dci10`, `pdcch_blind_monitor_pdsch` (`"mode:…"`; mode 1 = decode+report, 2 = decode+submit
CFR; `mcs_table=0` = unknown → Technique D), `pdcch_blind_monitor_rnti_range`, `pdcch_blind_monitor_noise_gates`,
`pdcch_blind_monitor_scan_thread`, `pdcch_blind_monitor_dci01`, `pdcch_blind_monitor_ul_pusch`,
`pdcch_blind_monitor_ul_uci`, `pdcch_blind_monitor_ul_thread`, and manual/oracle keys
(`pdcch_blind_monitor_coreset/_ss/_bwp/_tda/_tda_common/_dmrs/_dci_bits/_ul_bwp/_ul_tda/_ul_dmrs/_ul_dci_bits/_ul_misc`)
— **manual keys make a run an oracle arm, not agnostic**. Plus sensing keys (`sources`, `cpi_slots`,
`report_path`, `out_path`, `report_endpoint`, `rx_id`, …) that only matter with sensing enabled.

**Environment variables added 2026-10-01 (cloud Track-A session):**

| Variable | Meaning | Default / notes |
|---|---|---|
| `ISAC_METRICS_PATH` | Also append every `ISAC_METRICS` JSON line to this file (§11.13) | unset = log line only; unopenable path → one LOG_W |
| `ISAC_OBS_PATH` | Enable the per-grant observation writer, JSONL appended to this file (§21) | unset = off (hooks cost one relaxed load) |
| `ISAC_SCAN_SCRATCH_MB` | Initial-sync scan scratch budget (decides how many GSCNs are scanned per batch) | 512; clamped to 64..16384; non-numeric → default + LOG_W once |
| `ISAC_OBS_TEST_WRITER_PAUSE_MS` | **Test hook**: stall the observation writer (overload test) | unset = inert |
| `ISAC_OBS_TEST_RING_SLOTS` | **Test hook**: ring slots, 1..2^20 (default 16384) | unset = inert |
| `V4SHIM`, `SCANTHREAD`, `COREMAP`, `GATE_CRC_MIN`, `GATE_DROP_MAX` | `rfsim_arm.sh`/`rfsim_regress.sh` knobs: IPv4 shim (auto if no IPv6), scan-thread override (auto `1:8:-1` when `nproc<=5`), DGX core-map mode (needs the online cores of `coremap_dgx.env`; rc=3 otherwise), gate thresholds (script defaults = DGX 98/1; cloud uses 93.0/2.5) — on `td/convergence-levers` these two apply to `GATE_MODE=legacy` only (`GATE_DROP_MAX` to both); see the next table | host-specific, §14.5, K31 |

**Environment variables added on `td/convergence-levers` (2026-10-01/02; not on `adaptive-rx-UL-DL` until merged):**

| Variable | Meaning | Default / notes |
|---|---|---|
| `ISAC_TD_K0_ORACLE_LEGACY` | `1` = pre-K39 behaviour: the DM-RS oracle records the hypothesised k0 as observed and hard-prunes the other k0 (A/B only; unsafe, K39) | unset/0 = K39 fix (k0 only plausible) |
| `ISAC_TD_DCI_ADJ` | `0` = full kill switch of BC9/BC9d (no DCI history, no TDD / DCI-adjacency exclusions, no certified census) | unset = on (inert without SIB1 TDD) |
| `ISAC_TD_CERT_CONFIRMED` | `1` = the BC9 certified flag (levers C/P only) counts only CRC-confirmed DCI occupants. Hard exclusions use confirmed DCIs regardless (K43) | unset/0 (keep 0: `=1` kills every lever event in the simulator) |
| `ISAC_TD_PROBE_EQUIV_CHECK` | Probe ≡ full harness (F1), samples 1 grant in 50: `1` = re-decode the probe's hypothesis over the whole slot and compare CB0 LLRs/CRC (`PROBE_EQUIV`, `PROBE_EQUIV_CHEST`); `2` = also the chest-cache-hit arm (`PROBE_EQUIV_HIT`). Diagnostic: re-runs perturb EMAs/latches/caches; costs CPU | unset/0 = off |
| `ISAC_TD_PROBE_EQUIV_BRANCH` | Force this RX branch in the harness's single-branch (chest-row) arm; a value ≥ nb_rx is silently ignored | unset = −1 (no forcing) |
| `NR_TD_SIM_REF` | Test-only: path of the v1 reference `nr_td_sim` (built at `77775006f2`) for the byte-identity test of `nr_td_sim_test` | unset = that test SKIPPED |
| `GATE_MODE` | `rfsim_regress.sh` / `score_rx.py --gate`: `postconv` (operator option a) or `legacy` (old overall-CRC ≥ `GATE_CRC_MIN`) | `postconv` |
| `GATE_NCTX_MIN`, `GATE_POSTCONV_CRC_MIN`, `GATE_POSTCONV_MIN_DEC`, `GATE_REOPENS_MAX`, `GATE_TTC_MAX_TDA0`, `GATE_TTC_MAX_TDA2` (any `GATE_TTC_MAX_TDA<n>`), `GATE_CRC_FLOOR`, `GATE_DROP_MAX` | postconv thresholds (§12) | 1-RX values at `003b8c3f93`: 2, 99.8, 5000, 0, 8.6 s, 35.3 s (other tdas unbounded), 94.5, 1.0. **Merged main (4-RX gate, provisional, set on `td/fast-defaults`): 2, 99.8, 5000, 0, 39 s, 77 s, 93.4, 2.5, with 420 s arms** |
| `GATE_CRC_MIN` | legacy-mode threshold | 98.0 (DGX); cloud x86 used 93.0 |

**Environment variables added 2026-10-03/04 (acceleration round; module details §3.3.1).**

**Defaults of the merged main (operator decision 2026-10-04, "fastest combination").** The controller applies them on
branch `td/fast-defaults`; **at `003b8c3f93` the code defaults are still the "branch" column**. Every ON default is
disabled with `=0`.

| Variable | Meaning | Branch default (`003b8c3f93`) | **Merged-main default** |
|---|---|---|---|
| `ISAC_TD_GRANTWORK` | GrantWork-lite (shared FEP/chest/LLR per grant and signature; prerequisite of CB0 elimination) | 0 | **1 (ON)** |
| `ISAC_TD_CB0_ELIM` | CB0 elimination channel (engine + runtime wiring; needs GrantWork, else counted `no_grantwork`) | 0 | **1 (ON)** |
| `ISAC_TD_FIELDBOOK` | `2` = reversible field book (fb2) wired into the runtime; `0` = off (only 0 and 2 exist) | 0 | **2 (ON)** |
| `ISAC_TD_CB0_BACKEND` | `auto` = GPU CB0 entry when registered and healthy, CPU backend otherwise / as fallback; `cpu`; `gpu` (= auto + one-time warning on fallback) | auto | **auto** |
| `ISAC_TD_TB_CPU_WHILE_ACQ` | Force the CPU TB decoder while a context acquires (so a CUDA CB0 dominates it, §3.3.1) | 1 | **1** |
| Levers C, P, E | Engine/simulator only, no runtime flag; stay **OFF**. Lever E is incompatible with CB0 elimination (the simulator refuses the combination) | off | **off** |

Other knobs (defaults identical on branch and main): `ISAC_TD_CB0_B` (96 items per grant; 0 = derive from
`ISAC_TD_CB0_BUDGET_US`, 20000), `ISAC_TD_CB0_CPU_PCT` (30 % of online CPUs, token bucket), `ISAC_TD_CB0_THREADS` (8),
`ISAC_TD_CB0_GPU_BACKOFF` (64 batches after a GPU failure), `ISAC_TD_CB0_RANK_MAX` (4; rank > 1 admissible since K38),
`ISAC_TD_CB0_GPU_AUTOREG` (1; 0 = never register the GPU backend), `ISAC_TD_CB0_GPU_ITERS` (unset = 2 × max_iter = 16;
a lower cap is honoured only while `ISAC_TD_TB_CPU_WHILE_ACQ≠0`, and only 12 has paired evidence — operator decision),
`ISAC_TD_GW_UNIFIED` (1 in a CUDA build; 0 = aligned malloc), debug `ISAC_TD_GW_CHECK=N` (`GW_EQUIV`, 1 decode in N),
`ISAC_TD_GW_PROBE` (GrantWork for layout probes), `ISAC_TD_GW_PROFILE`; GPU library knobs `LDPC_CUDA_TIMEOUT_MS` (200),
`LDPC_CUDA_BREAKER_N` (4), `LDPC_CUDA_BREAKER_S` (5), `LDPC_CUDA_WARMUP_ITERS`, `LDPC_CB0_MEM` (`explicit` forces
copies), `LDPC_CB0_TIMEOUT_MS`, `LDPC_CB0_TIMEOUT_US_PER_ITEM` (100), `LDPC_CB0_BREAKER_N`/`_S`, `LDPC_CB0_SLOTS` (3);
test hooks `LDPC_CUDA_TEST_*`, `LDPC_CB0_TEST_*`. Bench/test paths: `NR_GPU_CB0`, `NR_TD_CB0_CUDA_LDPC`.
**Never set `ISAC_RX_BRANCH_FO`** (unchanged rule; rank > 1). **Not measured together:** no rfsim run had all main
defaults ON at once (fb2 was never ON in the 4-RX CB0 arms; §14.10, §25 deferred "full-combo gate").

**Reconfiguration-robustness knobs (merged in `1ecf98d162`, completed through `c6fc03be7c`; all runtime behaviour
except 140-bit DCI capacity remains behind `ISAC_RECONF=1`).** Invalid values use the stated default; settings are
read once per receiver lifetime unless marked test-only.

| Variable | Default | Effect |
|---|---:|---|
| `ISAC_RECONF` | unset/0 | `1` enables the epoch authority and consumers; unset/0 retains the prior receiver behaviour. |
| `ISAC_RECONF_N_SUSPECT` | 200 occasions | Active-elsewhere misses before a LOCKED per-RNTI DCI length becomes SUSPECT and re-locks. |
| `ISAC_RECONF_DISCOVERY_DUTY` | 20 | After a bank entry exists, run continuous CORESET discovery one occasion in N (valid 1..10000). |
| `ISAC_DCI_LEN_MAX` | 140 | Explicit upper DCI payload search bound (legal range through `NR_DCI_MAX_PAYLOAD=140`); a seen wide length or SUSPECT re-lock widens as needed. |
| `ISAC_DCI_WIDE_PROBE_EVERY` | 1 | Minimum exhausted-sweep cadence for one 64..140 round-robin probe; automatic stride adjustment caps added scorer calls at 5%. |
| `ISAC_RECONF_GAP_HARD_MS` | 10 ms | Continuity gap at/above this is HARD_REVERIFY; a shorter gap is SOFT. |
| `ISAC_RECONF_SI_GRACE_MS` | 5000 ms | Grace after an SI-modification boundary while awaiting re-decoded SIB1. |
| `ISAC_RECONF_SI_BUMP_WITHOUT_SIB1` | 1 | At an SI boundary without a re-decodable SIB1, bump HARD_REVERIFY; `0` suppresses that conservative bump. |
| `ISAC_RECONF_SIB1_MAX_OCCASIONS` | 8 | Cap each periodic SIB1 monitoring window (also capped at one second); requests recur at the fixed five-second cadence. |
| `ISAC_TD_IGNORE_SIB1` | unset/0 | `1` is the independent SIB1-less/NSA-like test arm: suppress SIB1/SI/P-RNTI facts and epoch inputs, while retaining MIB-derived CORESET#0 for RAR/TC-RNTI and dedicated discovery. |
| `ISAC_UECTX_PATH` | unset | With `ISAC_RECONF=1`, append non-RT `uectx/1` JSONL snapshots/change/reconfiguration records to this path; unset leaves the writer off. |
| `ISAC_UECTX_PERIOD_S` | 5 s | UeContext periodic snapshot interval; transitions and shutdown also snapshot. |
| `ISAC_UECTX_TEST_WRITER_PAUSE_MS` | unset | Test-only writer stall for bounded-ring drop coverage; never use operationally. |

`pdcch_blind_monitor_scan_thread "N:depth:core"` now accepts N <= 4 consumers safely (K27); `core = -1` leaves every
consumer unpinned, `core >= 0` pins consumer i to core+i.

The fully agnostic OTA config used last: `tests/passive_rx/sens6_host_snapshot_2026-09-30/ota_confs/agnostic_ota_noprior.conf`.

### 10.3 OTA runs (X410)

Preferred: through the launcher (does preflight, IRQ pinning, verdicts, retries).

```bash
cd <repo>/tests/passive_rx/captures
# Lab cell (as on 2026-09-25): 273 PRB @ 3450 MHz, SSB subcarrier 150, 1 RX
REPO=<repo> ARM=agnostic_lab CONF=<repo>/tests/passive_rx/sens6_host_snapshot_2026-09-30/ota_confs/agnostic_ota_noprior.conf \
  DUR=200 TRIES=2 NANT=1 RXG=49 PRB=273 CARRIER=3450000000 SSB=150 SCAN=0 INITIALFO=-15000 ADAPT=0 \
  DATA=<x410-data-ip> MGMT=<x410-mgmt-ip> NIC=<data-nic> \
  XENV='ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1' ./run_arm.sh
```

**Before using `run_arm.sh` on a new host, fix these hard-coded lines** (they are sens6-specific): `BASE=/home/sens/NICOLA/captures`,
`REPO` default `openairinterface5g-total-passive-ue`, the `cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets`
line, the `ssh sens4 stat …gnb.log` bracket lines, the sysctl/IRQ core numbers (8–13), `CPUSET=0-7`,
`--thread-pool 0,1,4,5,6,7`, `RTCORE=2`, and `sudo … pkill -9` in the watchdogs (prefer SIGINT; the operator allowed
`sudo kill` of a hung receiver on sens6).

Direct equivalent (what `run_arm.sh` executed on 2026-09-25, lab arm):

```bash
sudo env ISAC_DISC_NO_RESYNC=1 ISAC_UE_RT_CORE=2 ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
  ISAC_CFO_TRACK_HZ=800 ISAC_CFO_TRACK_PERIOD=20 ISAC_UL_TA_SWEEP=0:0:0 ISAC_RX_MRC_MODE=0 ISAC_SENSE_COMB=1 \
  ISAC_TSYNC_RESET=0 ISAC_AUTO_ACQUIRE=0 ISAC_ACQ_CFO_MAX_HZ=60000 ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1 \
  timeout -k 20 200 taskset -c 0-7 ./nr-uesoftmodem \
    --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=192.168.1.140 \
    -O agnostic_ota_noprior.conf -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
    --ue-rxgain 49 --ue-nb-ant-rx 1 --ue-nb-ant-tx 1 --passive-rx --ue-fo-compensation --initial-fo -15000 \
    --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90
```

(`ISAC_DISC_NO_RESYNC=1` is a diagnostic override of loss recovery; drop it in an autonomous-acquisition acceptance arm.)

Salt macro arm (2026-09-25): same with `CONF=salt_ota_noprior.conf -r 217 -C 3540000000 --ssb 1182 --ue-rxgain 43`
(scan variants used `--ue-scan-carrier`).

Commercial cells recorded (HISTORICAL, 2026-09-19, same site): **Salt** PCI 64, 3540 MHz, 217 PRB (80 MHz),
SSB SC offset 1182, RXG 43–47 valid window; co-channel **PCI 244** shares the GSCN and N_ID2. **Swisscom** PCI 382,
SSB 3610.56 MHz (GSCN 7923), 273 PRB, `-C 3649980000`, SSB subcarrier 204, PLMN 228-01, TAC 105, RXG 43;
**Swisscom is NSA** (operator statement; consistent with 450+ RARs and no Msg4) **and still broadcast SIB1** (SIB1 presence
is independent of SA/NSA, §11.5). The site ("known-good spot")
matters: a second location on 2026-09-19 produced nothing usable.

### 10.4 Simulated beds (no radio)

**OAI phy-test bed (sens6-style, no core):**
```bash
# gNB (from the test-only branch sdd/rfsim-gnb-test, never merged):
sudo ./nr-softmodem --phy-test --noS1 -m <mcs> -n <mcs_table> -M <prb> -l <layers> -D 0xff \
     -O tests/passive_rx/gnb.sa.rfsim.conf --rfsim
# passive receiver:
sudo ./nr-uesoftmodem --passive-rx -O <fully agnostic ue.passive.*.conf> --rfsim -C <freq> -r <prb> \
     --numerology 1 --band 78 --ssb <k> [--ue-nb-ant-rx 4]
```
Virtual UE RNTI 0x1234. The phy-test gNB sends every TB 4× (rv 0,2,3,1) with no feedback — only 1/4 of grants are rv0.
Test-gNB knobs `ISAC_GNB_TEST_USS_AL=a1,a2,a4,a8,a16`, `ISAC_GNB_TEST_DL_DMRS_ID0/ID1`, `…_DL_DATA_ID`, `…_UL_*`,
`…_DL_TYPEB_K0`, `…_DL_TDA_IDX`, `…_VRB_IL`, `…_PRG`; crashes on AL16 and type-B k0=2. Rank-4 bed:
`gnb.sa.rfsim.100mhz.rank4.conf` (`-m 25 -n 1 -M 273 -l 4`) + `ue.passive.pin49r4.100mhz.conf --ue-nb-ant-rx 4`.
Score from the receiver log: accepts by RNTI class, `pdsch_decode[try= crc_ok=]`, `Technique D ARMED/CONVERGED`,
`scanq drop_full`.

**OAI SA bed (needs open5gs core):** `CONF_TAG=.agn NUM_RX=1 NUM_UE=<1|3> RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M
./tests/passive_rx/run_passive_rx.sh <secs> <outdir>`; receiver conf `ue.passive.agn.conf` (106 PRB, fully agnostic).
Touch/chmod `nr*_stats*.log` in the CWD first. Score C-RNTI-class accepts and C-RNTI PDSCH CRC (SIB1 decodes do not
validate the dedicated path).

**OCUDU bed via ZMQ (needs open5gs, OCUDU gNB, srsUE, `-DOAI_ZMQ=ON`):**
```bash
cd tests/passive_rx
./run_ocudu_passive.sh 240 /tmp/ocudu_passive/<name>     # gNB first, then broker/UEs; writes summary.txt
./run_ocudu_passive.sh stop
python3 ocudu/score_ocudu_run.py /tmp/ocudu_passive/<name>
# variants: PX_FIRST=1, PX=0, NO_UE=1, GNB_EXTRA='cell_cfg pdcch dedicated --ss2_n_candidates 0 0 4 2 0'
```
Passive side: `nr-uesoftmodem --passive-rx -E -r 51 --ssb 33 --device.name oai_zmqdevif -O ocudu/ue.passive.ocudu.conf`.
Rules: start the gNB first and wait for "Connection to AMF … completed"; never kill one side of a ZMQ link while the
other runs (restart all); never set a negative `al_cqi_offset` (crashes OCUDU) — use `ss2_n_candidates`; srsUE
`tx_gain=0` (50 gives 2.6 dB UL SINR). srsUE has no transform precoding; OCUDU has no EN-DC/NSA.

### 10.5 What remains non-agnostic at startup vs discovered

| Supplied at startup today | Discovered at runtime |
|---|---|
| Band, numerology, `-r` (sample rate/grid), `-C`, `--ssb` (unless scan), RX gain, antenna count, CFO seed, device args, core map, config mode flags | PCI, SSB index, frame/slot timing, CFO residual, MIB fields, CORESET#0/SS#0, SIB1 facts (PLMN, carrier, TDD, common TDRA, BWPs, PRACH), dedicated CORESET(s), PDCCH DM-RS scrambling ID (confirmed = PCI on tested cells; searched by the offline tool), RNTIs, DCI sizes and layouts, AL set, DL/UL TDRA semantics, DM-RS config, MCS table, rank, CSI-RS/ZP-CSI-RS resources, SSB rate matching, UCI footprint, BWP changes (partial), DM-RS/data scrambling IDs (partial) |

`tests/passive_rx/auto_acquire.py` (autonomous band/window launcher) exists to remove `-C`/`-r`/`--ssb` from the
supplied list but is `[IMPLEMENTED, NOT VALIDATED]` (never built/OTA-tested per its own notes; requires 4 RX; FR1
TDD, same numerology, shared Point A, zero offsetToCarrier only).

---

## 11. Complete logging reference

All receiver lines carry the OAI component tag (`[PHY]`, `[HW]`, `[NR_RRC]`, `[NR_MAC]`) and most project lines start
with `SENSING:` (historical name — they are receiver lines, not sensing). Summary lines are periodic. Read logs
**in order**: the first abnormal line matters, later ones are usually consequences. Strip ANSI colour with
`sed 's/\x1b\[[0-9;]*m//g'`. Always read lines *before* the first `RFSTALL`.

### 11.1 RF / UHD

| Message | Fields | Normal | Failure meaning → next step |
|---|---|---|---|
| `UHD version 4.10.0…`, `[INFO] [MPMD] Initializing 1 device(s)… serial=…, fpga=UC_200, claimed=False, addr=…, master_clock_rate=…` | device identity | serial/fpga as expected, `claimed=False` | `claimed=True` → another process holds it (§8 X6) |
| `device_init failed: RuntimeError: … Someone tried to claim this device again` | — | never | second claimant; wait, check `ps`, MPM journal; **UHD problem** |
| `Actual master clock: 245.760000MHz` | MCR | 245.76 (UC_200) | other → image differs, re-derive rates |
| `RFCHAN chN subdev=S port=RX1 gain=G range=[0..60] freq=… rate=122.88 Msps bw=…` | readback per channel | gain equals request, port RX1, same freq on all ch | gain mismatch → clamp; freq mismatch across ch → retune bug (§8 X10) |
| `RX antenna forced to RX1 on channel N` | | one per channel | missing → device type not X4xx |
| `USRP_RX_START scheduled=…` | | once | — |
| `SENSING: RFSTALL USRP_RX_START reason=UHD metadata error … ERROR_CODE_OVERFLOW (Out of sequence error)` | received/requested | never | stream dead at start: stale claim or NIC flap; wait 60 s (180 s after MPM restart), relaunch. **network/UHD** |
| `SENSING: RFSTALL USRP_RX_READ reason=… ERROR_CODE_OVERFLOW (Overflow)` / `ERROR_CODE_TIMEOUT` | received/requested | never | host didn't drain (IRQ/CPU) or link loss; check `nic.csv` missed counters, CPU, NIC temperature. **network/host** |
| `SENSING: RFSTALL radio read failed; refusing invalid IQ` → `UE main thread is ending` | | never | run over; the launcher kills and scores the run |
| `SENSING: RFSTALL PBCH lock lost (timing runaway)` / `RFSTALL … rf_pow=… ref=… pbch_ok=… pbch_fail=…` | power ratio vs reference | — | `rf_pow/ref < 0.25` on all branches = deaf stream (Mode A, UHD/MPM); `≈1` with 0 PBCH = Mode B (sync); §8 |
| `SENSING: RFCENSUS slots=2000 ssb_slots=50 pbch_ok=50 pbch_fail=0 rf_pow=… ref=… bad=0 shiftForNextFrame=… max_pos_acc=… frame=…` | every 2000 slots | `pbch_ok=50 pbch_fail=0`, `max_pos_acc` stable (hundreds, < 1024) | falling pbch_ok → sync loss; ramping `max_pos_acc` → timing runaway |
| `SENSING: RFPOW … chN=-30.73dBFS(rf=-65.93dBm,clip=0)` | absolute per-channel power | well above noise, `clip=0` | near noise → antenna/RF; `clip>0` → reduce gain. **RF** |
| `SENSING: ADCPEAK … chN=-12.0dBFS(hot=0.0000%)` | peak | < −3 dBFS, hot 0 % | hot > 0 → saturation |
| `SENSING: ANTPOW raw=[…] dB=[-0.9 -17.6 0.0 -11.3]` | relative branch power | balanced (±3 dB) | large spread = branch imbalance (§8 X11) |
| `nic.csv` (launcher) / `nic_miss=` in `verdict.txt` | NIC `rx_missed_errors` delta | 0 | > 0 = packet loss on the host NIC → run invalid |

### 11.2 PSS / SSS / PCI (initial sync)

| Message | Normal | Meaning |
|---|---|---|
| `[NR_PHY] Cell Detected with GSCN: G, SSB SC offset: K, SSB Ref: …, PSS Corr peak: 90 dB, PSS Corr Average: 72` | peak ≥ ~15 dB above average | Valid PSS detection at subcarrier offset K (use K as `--ssb` next time) |
| `SYNCDIAG symbolOffset=… ssbOffset=… rx_offset=… ssbIndex=… halfFrameBit=…` | once per acquisition | frame placement |
| `[PHY] [UE0] In synch, rx_offset N samples` | | |
| `[PHY] [UE 0] Measured Carrier Frequency offset -13101 Hz` | this rig: −13…−16 kHz with the −15 kHz seed family | CFO estimate; a large value alone is **not** a mis-lock |
| `[PHY] Initial sync successful, PCI: P` | once per acquisition | PCI = 3·N_ID1+N_ID2 validated by PBCH CRC |
| `Initial sync: pbch not decoded on any branch` → `synch Failed` → rescan | should not repeat | PSS/SSS found but PBCH CRC fails: settling after MPM restart, CFO mis-lock, wrong window |

**Valid acquisition** = one `Initial sync successful`, followed by `RFCENSUS pbch_ok=50 pbch_fail=0` windows and a
stable `max_pos_acc`. Repeated acquisitions (`synch Failed` loops, multiple `Initial sync successful`) = not locked.

### 11.3 Synchronization (timing / CFO)

| Message | Meaning / normal |
|---|---|
| `SENSING: TSYNC_OBS frame=… corr_pos=… d_per_frame=… drift_ema=… e_win_frac=0.98 peak_oow=0 reliable=1 max_val=… measure_only=0` | CIR peak position inside the FFT window. Healthy: `e_win_frac ≈ 0.99`, `peak_oow=0`. `e_win_frac ≈ 0.06–0.17` with `peak_oow=1` = window landed on the wrong peak (§8 X9) |
| `SENSING: TIMEMUT shiftForNextFrame(pbch) a -> b (delta …)` | timing-loop mutation audit; steady small deltas normal; large repeated jumps = runaway |
| `SENSING: TRACKLOCK frame=… ssb=… pbch_crc_ok=1 max_pos_acc a -> b sampleShift=…` | tracking re-lock events after acquisition |
| `SHIFTCENSUS frames=… applied_cum=… mean=… samples/frame (… ppm)` (`ISAC_SHIFT_CENSUS=N`) | healthy = physical clock drift (lab: −4.14 ppm); 6–15× that = runaway |
| `SENSING: CFOTRK ema=… Hz streak=… spread=… Hz stable=yes/no current=…` | CFO trim loop; `stable=yes` with large ema = mis-lock candidate (launcher aborts unless `CFOAPPLY`); `CFOTRK retuning a -> b Hz and re-initialising the device` = apply path |
| `SENSING: BRANCHFO d_vs_br0=[0.0 x y z] Hz` | per-branch residual FO vs branch 0; should be ≈ 0 after the all-channel retune fix |
| `Got synch: hw_slot_offset …, carrier off … Hz` | tracking start |

### 11.4 PBCH / MIB

| Message | Meaning |
|---|---|
| `Initial sync: pbch decoded sucessfully, ssb index N` | PBCH polar decode + CRC OK; N = SSB index in the burst |
| `pbch rx ok. rsrp:50 dB/RE, adjust_rxgain:0 dB` | PBCH RSRP; `adjust_rxgain` negative = too hot |
| `ERROR NR_PBCH_DECODE => polar decoding wrong` | PBCH CRC failure (normal occasionally during scan; persistent in tracking = loss) |
| `SENSING: ACQ_EVENT pbch_locked (n=1, state=SEARCHING)` | MIB applied |
| `SENSING: MIB dmrs-TypeA-Position posX -> blind monitor (was posY)` | MIB DM-RS type-A position handed to the blind monitor (the NSA/no-CORESET#0 path relies on this) |

MIB fields used downstream (decoded by upstream OAI `nr_ue_decode_mib`): **SFN** (frame numbering for all slot
arithmetic), **half-frame bit + SSB index** (frame placement), **subCarrierSpacingCommon** (SCS of CORESET#0/SIB1),
**ssb-SubcarrierOffset k_SSB** (Point A derivation; FR1 k_SSB ≥ 24 ⇒ **no CORESET#0 → no SIB1 on this SSB**),
**dmrs-TypeA-Position** (first PDSCH DM-RS symbol 2 or 3), **pdcch-ConfigSIB1** (controlResourceSetZero index →
CORESET#0 RBs/symbols/offset; searchSpaceZero index → SS#0 monitoring occasions), **cellBarred**,
**intraFreqReselection** (not used by the receiver). A MIB failure means no frame timing → nothing downstream
works; check §11.2/11.3 first.

### 11.5 SIB1 / NSA

| Message | Meaning |
|---|---|
| `SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(groups=8 dur=1 bundle=6 interleaver=2 shift=P scramb=P) bwp=[a..b) (cset_start_rb = …) ss(period=40 offset=… dur=2 symb=0) dci10(mux=1 sib1=1)` | CORESET#0/SS#0 derived from MIB; SI-RNTI monitoring active |
| `[NR_RRC] SIB1 decoded` | SIB1 PDSCH (SI-RNTI) CRC OK |
| `[NR_RRC] SENSING: SIB1 CELL mcc=228 mnc=03 cell_identity=0x… tac=… n_plmn=1` | **Operator evidence** (PLMN) |
| `[NR_MAC] PASSIVE: cell config extracted from SIB1 -- PCI=… DL: N PRB @ SCS=1 offsetToPointA=… UL: …` | carrier geometry |
| `SENSING: SIB1_SS …`, `SENSING: SIB1 PRIOR: bwp=… coreset… ss[AL1..AL16 …] ra_ss=… rach[prach_idx=…]` | common search spaces, RA SS and PRACH (used as a prior / RAR anchor) |
| `SENSING: TDA-COMMON from SIB1: N entries: [0]S=1,L=13,m=0 …` | common PDSCH TDRA list (used for RA/TC/C-in-CSS only) |
| `SENSING: TDD from SIB1 DERIVED: p1 period=… dl=…+…sym ul=…+…sym` | TDD pattern → slot direction |
| `SENSING: ACQ carrier CONFIRMED from SIB1: …` / `SENSING: ISAC_ACQ_RETUNE {"n_rb":…,"centre_hz":…}` | started geometry matches / mismatches the cell (launcher relaunches once when `ADAPT=1`) |
| `PASSIVE: SIB1 common facts … (CACHED)` | loaded from the per-PCI SIB1 cache (not live) |
| `Got NACK on NR-BCCH` | SIB1 decode attempt failed |

**SIB1 presence is independent of SA/NSA (corrected 2026-10-02, robustness spec commit `69ae438406`).** An NSA
cell may broadcast SIB1 (Swisscom PCI 382, NSA per operator statement, decoded SIB1 for 600 s runs on 2026-09-19,
`HISTORICAL`, Switzerland), and an SA-capable cell's SIB1 may simply not be decoded yet. The receiver uses SIB1
whenever it is decoded, whatever the network mode (CSS0 autoconf, carrier, TDD → BC9 exclusions and K40 PDCCH gate,
common TDRA → BC12a census). Read "SIB1-less" below as **"SIB1 absent or not decoded"**, never as "NSA"; earlier
wording "SIB1 absent (NSA …)" is `HISTORICAL`.

**SIB1 present:** MIB → CORESET#0 → SI-RNTI DCI 1_0 → SIB1 PDSCH → common facts (above). `[OTA VERIFIED]`.
**SIB1 absent or not decoded (k_SSB ≥ 24, or no SIB1 decode; some NSA cells):** there is no usable CORESET#0. What is implemented: (1) the MIB DM-RS type-A position
still reaches the blind monitor (`70516af6e3`, `[OFFLINE VERIFIED]`, `test_nr_ue_mib_blind_handoff` 6 tests);
(2) dedicated CORESET discovery runs without CSS0 (proven offline; the discovery step precedes the periodicity gate);
(3) the RAR path can anchor a C-RNTI as *trusted* at one sighting (`nr_pdcch_blind_rnti_bootstrap_record_trusted`)
— on NSA the NR-leg RA is contention-free, so RARs carry the UE's C-RNTI (observed on Swisscom: 450+ RARs, 100 % CRC,
~no Msg4, 2026-09-21, HISTORICAL); (4) the receiver refuses to assert missing SIB1 facts (carrier/TDD must come from
startup or the SIB1 cache). **Not implemented / not proven:** carrier geometry and TDD without SIB1 (must be
estimated from signal); RNTI bootstrap without CSS0 on air; any live NSA validation (no NSA bed: OAI removed the
NSA UE in `6289409f15`; OCUDU has no EN-DC). Dedicated RRC config on NSA travels ciphered over LTE —
`nr_passive_rrc_harvest.c` can never work there.

### 11.6 PDCCH (blind monitor)

Periodic summary (the most important line):

```
SENSING: blind PDCCH monitor summary: occasions=… candidates=… accepts=…
  dci10[accepts= C= TC= SI= RA= P=] dci01[accepts= rejects=] dci00[accepts= rejects=]
  ulscan[sched= crc_hit= disc=] held[energy= dmrs= persist= snr= mismatch= rnti_set=] efloor=… cfr_submits=…
  pdsch_decode[try= crc_ok= (%) skip_rv= unsup= over_cap= data_submits= k0_wait=]
  scanq[queued= done= drop_full= drop_stale= maxlag=] last_reject=… last_reject_rnti=0x…
```

- `occasions` should grow at the PDCCH monitoring rate (~1–2 k/s at 30 kHz with USS every slot). Flat = no slots
  reach the monitor (sync/RF). `candidates/occasions` = search width.
- `accepts` = CRC-mask candidates passing all gates; split by RNTI class for DCI 1_0 (C, TC, SI, RA, P).
  **SI accepts validate only CORESET#0; C-RNTI accepts validate the dedicated path.**
- `held[…]` = candidates held back by noise gates (energy, DM-RS coherence, persistence, SNR, mismatched bits).
- `scanq drop_full` = scan-thread queue overflow. Healthy ≤ ~1–3 %; 40 % = the consumer is saturated (the old
  per-occasion Technique D reset bug). `drop_stale` = IQ expired before processing.
- `STAGE0 n_RNTI=…:occ=…,acc=…,confirmed=…` and `blind PDCCH ladder: num_cces=… ncand=… (AL1= AL2= AL4= AL8=) accepts_per_al=[…]`
  = per-AL candidate budget and accepts (use it to detect AL starvation).
- `SENSING: PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)|CSS0(common)] n_id=N (assumed = PCI N) by CRC-recovered RNTI 0x…`
  = the PDCCH DM-RS/scrambling identity in use is confirmed (a wrong identity gives zero accepts). It is **not** a
  search of the 0..65535 domain — that search is the offline/GPU `idsweep` tool.

Discovery lines:

| Message | Meaning |
|---|---|
| `SENSING: Phase 3 autodiscover -- CORESET footprint rb_offset=… span_rb=… bootstrap_rnti=0x…` | Technique A candidate extent under trial |
| `SENSING: multi-CORESET bank add index=0 offset=0 span=270 symbol=0 mapping=0/0/0 len=47` | **dedicated CORESET verified** (fresh C-RNTI DCIs at different slots/payloads) and banked; `len` = DCI 1_1 length |
| `SENSING: DCI 1_1 length locked coreset=… rnti=0x… len=…`, `lookahead lane … dci_length locked at …`, `UL automatic DCI length locked: L rnti=…` | Technique C length decisions (DL 1_1, UL 0_1) |
| `SENSING: DCI length RELOCK rnti=0x… old=L new=L` / `DCI length unresolved after N occasions …` | A SUSPECT context has re-confirmed or replaced its old length; a changed DL re-lock target-reopens only the matching Technique D context (the pre-R10 global reset is gone). Unresolved is diagnostic: look for repeated lines together with activity elsewhere, not an idle RNTI. |
| `SENSING: multi-CORESET bank add …`, `CORESET bank STALE index=… slot=…`, `CORESET bank REMOVED index=… offset=…` | Bank life cycle: verified geometry added; absence while traffic is visible elsewhere demotes it to a still-searched hint; removal follows the re-verification timeout. An idle cell must not produce STALE/REMOVED churn. |
| `SWEEP: new per-RNTI context rnti=0x…` | a C-RNTI has its own Technique D context |
| `held[persist=…]` growing, `bootstrap_rnti=0x0` | RNTI persistence not yet established |

### 11.7 DCI (formats, gates, false positives)

- Formats: DL 1_0 (CSS/SI/RA/P/TC/C), DL 1_1 (dedicated), UL 0_0, UL 0_1. CRC-24 is XOR-masked with the RNTI;
  the receiver recovers the RNTI from the mask, so a random codeword "passes" with probability ~2^-8 per
  candidate on the non-masked bits → false positives are controlled by: RNTI range, **persistence** (≥ 2
  sightings, bootstrap table), **mismatched-bits re-encode gate** (adaptive, from NRSniffer), **energy floor**
  (`efloor`), optional **DM-RS coherence gate** (`ISAC_PDCCH_DMRS_GATE`, default off — it cut real weak grants,
  86.8 % vs 94 % budget, 2026-09-23), **DCI-length consistency**, **reserved-field checks** (e.g. RAR DCI reserved
  bits zero), and ultimately the **TB CRC** of the scheduled PDSCH/PUSCH.
- Field interpretation (widths depend on RRC): DL 1_1 layout sweep enumerates BWP-indicator width 0–2,
  TDRA-index width 0–4, FDRA type 0/1/dynamicSwitch, VRB-PRB interleaving bit, PRB bundling, rate-matching/ZP
  fields, antenna ports table (DM-RS type/max-length/rank), … constrained to the observed length, pruned by a
  decode-free plausibility stage; the winner is pinned (`nr_dci11_pin`). UL 0_1 uses the same resolver on
  TS 38.212 §7.3.1.1.2 order. HARQ PID, NDI, RV, DAI are parsed under the layout hypothesis; reserved MCS
  (DL 29–31 / UL 28–31 or 29–31 by table) resolve TBS from the per-(RNTI,pid) initial-TX record.
- Logs: `SENSING: UL_FDRA_CRC_RESOLVED n=… rnti=… candidate=i/n mode=… rb=a+b bitmap=0x…` (RA type 0/dynamic
  resolved by CRC), `UL_FDRA_ALTERNATIVES_BOOKED`, `HARQC first=… retx_combined=… init_tx hit=… evict=…`.
- The blind-monitor summary's `dci_wide_probes=N` is cumulative 64..140 low-duty probe occasions actually scored.
  A rapid rise is expected only after a narrow exhausted sweep; read it with `scanq_drop_full` / K45.

### 11.8 PDSCH / DM-RS / decoding

| Message | Meaning |
|---|---|
| `SENSING: Technique D ARMED: independent RNTI/TDA contexts, TB-CRC scoring` | DL waveform sweep active (must not appear repeatedly — it did 265 k times/150 s before the fix) |
| `SWEEP: rnti=0x… CONVERGED tda=… mapping=A k0=… mcs_table=… dmrs_add_pos=… dmrs_max_len=… (x/y trials)` / `SENSING: Technique D CONVERGED rnti=… tda=… S=… L=… mask=0x884 table=…` | converged PDSCH interpretation for that RNTI/TDA (mask 0x884 = DM-RS symbols 2,7,11) |
| `SENSING: Technique D CONVERGED rnti=… tda=… S=… L=… mask=… table=… k0=<k> map=<A\|B> dci=<1_0\|1_1\|?> sib1_row=<match\|mismatch\|none> deftab=<match\|mismatch\|na>` | **BC12a census (added 2026-10-02, log/metrics only, no behaviour change) `[IMPLEMENTED, NOT VALIDATED]` over the air; `[OFFLINE VERIFIED]` predicates.** At the first convergence of a context the winner's (k0, S, L, mapping) is compared with the SIB1 common-TDRA row selected by the DCI's TDA index (`sib1_row`) and with default table A row `tda+1` for the MIB dmrs-TypeA-Position (`deftab`). `none` = no SIB1 list decoded (always the case on the phy-test rfsim bed); `na` = pos unknown. A tda beyond the broadcast list is `mismatch`. `dci=` is the format of the grant that created the trial (always `1_1` today: the sweep only runs for `!is_dci10`, so `1_0` counters stay 0). SIB1 is used whenever decoded, whatever the network mode. Not merged across cells/hosts. |
| `SENSING: Technique D CONVERGED ... excl_restarts=N excl_truncs=T dci_phases=M` (suffix) and `SENSING: TD_EXCL_RESTART_ALARM` | **Exclusion-restart OTA diagnostic (`393b5c01b9`, merge `6e3d6c9a9a`; log/metrics only, no behaviour change) `[IMPLEMENTED, NOT VALIDATED]` over the air.** Per context: evidence restarts caused by exclusion tightening (`excl_restarts`), truncations (`excl_truncs`) and distinct DCI phases of the row (`dci_phases`). Expectation: `excl_restarts <= dci_phases`. `TD_EXCL_RESTART_ALARM` is rate-limited (once per context) and raised when restarts > phases. **Caveat:** a young context counts phases only from its creation, so it can raise a spurious alarm. `[MEASURED, DGX rfsim 106 PRB 1 RX]`: excl_restarts=0, dci_phases=14 (`slots_per_frame` fallback without SIB1); postconv gate PASS ×2 on that branch. |
| `SWEEP: ORACLE_RESTORE rnti=… tda=… mask=… last=… k0=… added=… plaus_k0=0x…` | a measured DM-RS mask re-exposed a pruned short-TDA candidate. **Semantics changed on `td/convergence-levers` (BC7b/K42):** restored entries are filtered by the context's certification/exclusion, so `added=` counts only admitted candidates, and a restore whose candidates are all filtered adds nothing and is **not logged**; `plaus_k0` = union of plausible k0 seen by the DM-RS oracle (K39) |
| `SWEEP: rnti=0x… CONVERGED tda=… mapping=… k0=… … (x/y trials on the winner, cfg=0x…, plaus_k0=0x…)` | per-RNTI convergence (branch adds `cfg` and `plaus_k0`) |
| `SWEEP: TYPEB_LATCH rnti=… tda=…: the type-B layer admits no observation; not re-appended until the observed sets change` | K42 follow-up M1: an observe-path type-B layer was truncated in full and is latched (no append/truncate loop) until a set changes, a reopen or a rebuild |
| `SENSING: BC9 DCIHIST dcis=… tdd_known=0/1 excl_removed[k0<2]=… excl_removed[k0>=2]=… excl_refused=…` | every 20 000 accepted DCI 1_1: DCI-history size and the cumulative exclusion counters (exclusions come from confirmed DCIs only, see DCICONF). `tdd_known=0` ⇒ no SIB1 TDD ⇒ TDD exclusions inert |
| `SENSING: BC9 DCICONF confirms=… missed_lookup=… repeat=… tdd_known=… tdd_dcis=… tdd_lock_calls=… adj_rows=… adj_removed=… adj_refused=… excl_removed[k0<2]=… excl_removed[k0>=2]=… excl_refused=…` | every 2000 confirmations (BC9d): confirmed DCIs (grant passed TB CRC), confirmations whose DCI was not found in the history (`missed_lookup`, should be ~0 unless overload), repeats, TDD exclusions applied, DCI-adjacency rows/removals/refusals. Read with host load (overload inflates `missed_lookup`) |
| `SENSING: BC9 DCIADJ confirmed DCI rnti=… slot=… tda=… -> rows=… removed=… refused=…` | first 20 confirmed DCIs whose adjacency rule removed or refused something |
| `SENSING: BC9 DCIADJ_CERT trials=… certified=… (f_S=…) certified_pass=… passes=… no_sibling=… no_dci_in_history=… certified_pass_by_k0[0,1,2,3,>=4]=… wrong_k0_alarms=…` | periodic census of the certified (k0-discriminative) grant stratum: f_S = certified share; per-k0 certified CRC passes |
| `SENSING: BC9 DCIADJ_CERT ALARM rnti=… tda=… converged on k0=K but certified passes were seen on k0 mask 0x…` (LOG_W) | **wrong-k0 alarm**: a context converged on a k0 while certified grants passed CRC on another k0 (assumption A1/A3 violation or a compatibility bug). Expected 0; any occurrence on air must be investigated before trusting that context |
| `SENSING: PROBE_EQUIV mismatches=a/b llr_mismatch=… skipped=… \| this: …`, `PROBE_EQUIV fo_moved=…`, `PROBE_EQUIV_CHEST mismatches=…`, `PROBE_EQUIV_HIT mismatches=… [skipped: …]` | only with `ISAC_TD_PROBE_EQUIV_CHECK` (§10.2): probe vs whole-slot decode of the same hypothesis (CB0 LLRs/CRC), chest rows, cache-hit arm. Healthy = 0 mismatches at Nl = 1; at Nl > 1 LLR mismatches remained until the K38 fix (`c005d19675`, 2026-10-03); since then 0 expected at any rank |
| `SENSING: GW_EQUIV n=… llr_mismatch ready=… today=… crc_mismatch=… \| cb0 n=… d_mismatch=… llr_mismatch=… crc_mismatch=… sibling_abort=… not_ready=…` / `GW_EQUIV skipped: …` | Only with `ISAC_TD_GW_CHECK=N` (debug): GrantWork READY path and no-GrantWork re-decode vs the main decode (all G TB LLRs, CRC) plus CB0 extraction/verdict. `sibling_abort` = CB0 verdict differs only because the full decode aborted its other segments after a failed one (not an LLR difference; excluded). Healthy = all mismatch counters 0. Samples only non-probe main decodes |
| `PROBE_EQUIV_GW …` (arm of `ISAC_TD_PROBE_EQUIV_CHECK` with GrantWork on) | probe hypothesis's CB0 from GrantWork vs the cache-free whole-slot reference: LLRs, de-matched input, CPU CB0 CRC (`ref_decoder`). Healthy = 0 |
| `SENSING: TD_CB0 wiring on: B=… budget=… us/grant cpu_pct=… ncpu=… threads=… B0=… rank_max=… tb_cpu_while_acq=… backend=… engine_wired=…` | once at start when `ISAC_TD_CB0_ELIM=1`: the CB0 wiring's effective settings |
| `SENSING: TD_CB0 GPU backend registered (CUDA CB0 entry)` / `… not available, CPU only (ISAC_TD_CB0_BACKEND=…)` | once: whether the GPU CB0 entry is in use. "not available" = `libldpc_cuda.so` not loadable (no CUDA build or not on the library path) → CPU backend only |
| `SENSING: TD_CB0_SCHED batches=… us_per_iter=… item_us=… sig_us=… tokens_us=… b_items=… g_target=… …` | periodic scheduler cost log (CPU-µs per LDPC iteration, per item, per new signature; token bucket). `us_per_iter` ~330 on the CPU backend at ~2–3 items/batch (fixed per-batch cost dominates), ~45 on the GPU backend `[MEASURED, DGX rfsim 106 PRB 4 RX]` |
| `SENSING: TD_CB0_PREMISE_ALARM rnti=… tda=… slot=… hyp=… S=… L=… k0=… mask=… table=… tb=PASS cb0=FAIL …` (wire) and `SWEEP: TD_CB0_PREMISE_ALARM hypothesis …: full TB PASS but CB0 FAIL on the same grant (cb0 decoder …, tb decoder …)` (engine, LOG_W) | **must never appear.** The CB0 ≥ TB premise failed on an admissible batch: the context's CB0 channel is disabled for good and its eliminations are undone (TB-only from then on). Any occurrence OTA is a finding to investigate (decoder mismatch, LLR path difference, admissibility bit not set) |
| `Technique D CONVERGED … cb0_grants=… cb0_adm=… cb0_items=… cb0_elim=… cb0_alarms=…` (suffix, `ISAC_TD_CB0_ELIM=1` only) | per context: CB0 grants seen, admissible, items decoded, eliminations, premise alarms (expected 0) |
| `SWEEP: … CONVERGED … fb_pruned=0x…` (suffix, fb2 only), `SWEEP: field book fb2 ENABLED (ISAC_TD_FIELDBOOK)` (once), `SWEEP: FIELDBOOK rnti=… tda=… FAIL-OPEN (no CRC pass in … trials on the active set; fb_pruned=0x… -> 0)` (LOG_W), promotion / SUSPECT / withdraw lines | fb2 runtime: which promoted fields pruned the context at creation; a fail-open restores every cause (a wrongly promoted field or a K41-type error). Read with `td_fb_*` metrics |
| `SENSING: SWEEPSTAT …` | per-context census |
| `SENSING: LDPCDIAG ok= seg_fail= tb_fail= zero_tb= iface_err= segs_decoded=a/b (%)` | LDPC health. **TB decode rate = ok/(ok+seg_fail)**; `segs_decoded` counts only failing TBs (do not score on it). `zero_tb` = CRC-valid all-zero TBs (gNB empty grants, 30 % on the lab gNB) |
| `pdsch_decode[try= crc_ok= …]` (in the summary) | CFR-producing passive decode path counters |
| `nr_rate_matching: invalid parameters (Foffset … > Ncb …)` / `Problem in rate_matching BG … C … E … F … K…` | a hypothesis produced an impossible TBS/rate-matching geometry (wrong MCS table/LBRM/layers) — expected during search, a flood means a stuck wrong hypothesis |
| `PDSCH SSB-OBS n=… frame= slot= pci= symbols=0x… crb=a..b overlap_re=… refused=… rnti=…` | PDSCH overlapped an observed SSB; rate-matched around it (`[OFFLINE VERIFIED]`, G4 pending) |
| `SENSING: DATA_ID_WALK START/STEP/LATCHED …` | data-scrambling-ID search (PARTIAL) |
| `SENSING: BRANCHFO …`, `SENSING: PDCCH_TIMING …`, `ISAC_PDSCH_TBPARM` `TBRESULT` lines | diagnostics |

### 11.9 PUSCH

`SENSING: pusch_passive[try= crc_ok= (%) seg_fail= zero_tb= (%) … health= unsup= setup_fail=] init_tx[hit= evict=]
ul_cfr[submits= re=] ta_refined= uci[trials= rescued=]`, `pusch_book[parked= claimed= expired= overwritten=]`,
`SENSING: UL_DMRS_PIN S= L= mapping=…`, `PUSCH TP scope: PCI-default nPUSCH-Identity=…` (transform precoding path
reached). UL CRC must be the real TB CRC (an early bug scored `rc==0` as CRC OK).

### 11.10 CSI-RS / ZP-CSI-RS

`SENSING: CSIRS_BLIND IDSWEEP pinned to rowR fdF lL …`, `CSIRS_BLIND IDSWEEP SOLVED rowR fdF lL scramb_id=N z=…`,
`SENSING: CSIRS_BLIND CONFIRMED #k after N slots -- csirs_monitor = "<11-field resource>"`, `CSIRS_BLIND ZP_GRANT_EVIDENCE …
REVOKED`, periodic `null_median=… confirmed=…`. Enabled with `ISAC_CSIRS_BLIND=1` (default off); rows 6–18 with
`ISAC_CSIRS_BLIND_WIDE=1`; `ISAC_CSIRS_BLIND_RANK=1` = diagnostic ranking (no rate matching). Status: `[OFFLINE VERIFIED]`
(131 gtests + synth); lab OTA 2026-09-25 IDSWEEP solved row1 fd4 l4 scramb_id=2 (z=5.1) but the confirmation loop did
not confirm; OCUDU SIM showed false ZP exports that were later fixed (probation + decoded-grant revocation) —
G4 on those fixes pending.

With reconfiguration enabled, `CSIRS_MAP_CHANGE` appears as the `cause` of `CONFIG_EPOCH … class=SOFT` after a
different confirmed NZP resource or four due missed occasions; a first/coexisting resource, a non-occasion slot, or
one miss must not trigger it. `CSIRS_TIMING …` and `ISAC_METRICS` confirmation counters are instrumentation, not a
live map-change validation.

### 11.11 Acquisition / reacquisition state

`SENSING: ACQ_STATE A -> B (updates=… time_in_prev=… regressions=…) evidence[len_found= coreset_ok= ul_bwp= dl_win= ul_width_win= ul_interp_win=]`
and `SENSING: ACQ_STATE X -> LOST (receive-stream discontinuity, no hysteresis; …)`. States (evidence labels, not a
strict chain): SEARCHING → PBCH_LOCKED → SIB1_DECODED → PDCCH_LOCKED → CORESET_VERIFIED → CELL_CONFIGURED →
DL_CONVERGED / UL_CONVERGED → TRACKING; LOST after 8 regressed updates or a stream discontinuity; LOST → SEARCHING.
Implemented recovery: stream-gap → clear sync, re-enter acquisition (SIB1 knowledge retained); local DL relearning
per context after ≥ 32 consecutive settled-ticket failures under a δ = 1e-6 bound. **There is no global loss
detector, no stale-evidence expiry, and no configuration-change detector** — a return to TRACKING can reuse latched
evidence from before the gap (`[KNOWN ISSUE]`).

**Superseded by the 2026-10-04 robustness path when `ISAC_RECONF=1`:** `SENSING: CONFIG_EPOCH old -> new
class=HARD_RESET|HARD_REVERIFY|SOFT cause=… scope=…` is the sole cell epoch authority. Causes include identity/MIB/
SIB1 change, SI boundary decision, continuity loss (the companion line gives `gap_samples`/`gap_ms`), BWP,
`CSIRS_MAP_CHANGE`, and dedicated-change suspicion. `scope` is the class-aware consumer mapping (R10b): narrow
short-gap/BWP/CSI-RS SOFT preserves per-UE dedicated facts; dedicated SOFT and HARD_REVERIFY turn them into hints;
HARD_RESET isolates the old identity. `CONFIG_EPOCH pending cause=SI_MODIFICATION_ANNOUNCED boundary=…` means wait
for the boundary/re-decode, not that an epoch already changed. SIB1 is periodically re-decoded (five-second cadence,
bounded window) and its canonical semantic change—not repeated bytes—causes `SIB1_CHANGE`. [IMPLEMENTED, NOT
VALIDATED on live reconfiguration, `c6fc03be7c`]

`SENSING: UECTX_STATS events=… written=… dropped=…` is the non-RT UeContext writer health line: `dropped` must be
zero in an operational arm. The JSONL records (`ue_snapshot`, `ue_change`, `ue_reconfig`, schema `uectx/1`) carry
RNTI/incarnation/epoch and trusted-only inferred per-UE changes; `ue_reconfig` is the source of the cell-wide
dedicated-change signal. `ISAC_UECTX_PATH set but writer could not be opened` is a configuration failure. [OFFLINE
VERIFIED, DGX aarch64, `b14317d382`; live rate/false-trigger behaviour unmeasured]

### 11.12 Launcher verdicts (`verdict.txt`)

`arm= try= verdict= sib1= nack= dl_ldpc_ok= dl_tb=% cpis= claimed= cfotrk= nic_miss=`. Verdict order:
`VOID_CFO_MISLOCK` (watchdog saw a stable large CFOTRK and killed the run) → `VOID_NO_SIB1` → `VOID_DL_ZERO` (no
LDPC OK) → `VOID_NO_CPI` (no sensing CPI; only meaningful with sensing) → `VOID_DL_RATE` (TB rate < 15 %) → `VALID`.
These verdicts were designed for **sensing captures**; for receiver work read the milestone lines directly. A
forced intentional CFO retune can be falsely stamped `VOID_CFO_MISLOCK` (known harness defect).

### 11.13 `ISAC_METRICS` and observation/scan-budget lines (added 2026-10-01)

`SENSING: ISAC_METRICS {json}` — one line per blind-monitor summary period (**every 20 s**), schema 1, also appended
to `ISAC_METRICS_PATH`. Flat object, cumulative counters unless noted; unknown = absent/`null`; consumers ignore unknown keys.

| Group | Keys |
|---|---|
| header | `schema` (1), `t_mono_ns`, `abs_slot`, `pci` (Nid_cell set at PBCH lock; 0 before) |
| acquisition | `acq_state` (name, §11.11), `acq_transitions`, `acq_sync_losses`, `acq_pbch_locks`, `acq_sib1_decodes` |
| PDCCH | `pdcch_occasions`, `pdcch_candidates`, `pdcch_accepts`, `pdcch_accepts_c` |
| scan queue | `scanq_queued`, `scanq_processed`, `scanq_drop_full`, `scanq_drop_stale`, `scanq_drop_epoch`, `pdcch_inline_drop_epoch`, `scanq_max_lag`; epoch keys count work rejected after a bump (queued / inline respectively). |
| PDSCH/PUSCH queues | `pdschq_queued`, `pdschq_decoded`, `pdschq_crc_ok`, `pdschq_drop_full`, `pdschq_drop_stale`, `pdschq_drop_epoch`, `pdschq_stale_after_decode`, `pdschq_max_lag`, `puschq_drop_epoch`; `*_drop_epoch` means old-epoch work received no credit, whereas `stale_after_decode` is F2 IQ expiry. A nonzero epoch-drop count at a real change is normal; any stale winner is not. |
| reconfiguration / UeContext | `dci_wide_probes`; UeContext writer health is the shutdown `UECTX_STATS events/written/dropped` line (not an `ISAC_METRICS` key). Check epoch-drop keys with the corresponding `CONFIG_EPOCH` line and `dropped=0`. |
| BC12a SIB1-TDRA census (2026-10-02) | `td_sib1_tdra_{match,mismatch,none}_{10,11,unk}` (per DCI format of the context; `_unk` = format not recorded), `td_deftab_{match,mismatch}_{10,11}`, `td_deftab_na`. Counted once per context convergence, same predicate as the CONVERGED-line suffix (§11.8). Cumulative, log/metrics only. `[OFFLINE VERIFIED]` serializer (`test_nr_passive_metrics`) and predicates (`test_nr_td_order`); OTA unmeasured |
| Exclusion-restart diagnostic (2026-10-02) | `td_excl_restarts`, `td_excl_truncs`, `td_excl_restart_alarms` (cumulative; see the CONVERGED suffix and `TD_EXCL_RESTART_ALARM`, §11.8). Log/metrics only. `[MEASURED, DGX rfsim 106 PRB 1 RX]` excl_restarts=0; OTA unmeasured |
| LDPC | `ldpc_ok`, `ldpc_seg_fail`, `ldpc_tb_fail`, `ldpc_zero_tb` |
| LDPC decoder provenance + G1 pool (2026-10-03) | `ldpc_tb_cpu`, `ldpc_tb_cuda` (TBs per `decoder_used`; CRC evidence of the two is **never pooled**); `ldpc_cuda_errors`, `ldpc_cuda_fallbacks` (TBs sent to the in-plugin CPU decoder), `ldpc_cuda_poisoned`, `ldpc_cuda_disabled` (0 closed, 1 bypassed, 2 permanent), `ldpc_cuda_breaker_trips` (monotonic). The `ldpc_cuda_*` keys read 0 when `libldpc_cuda.so` is not loaded |
| fb2 field book (2026-10-03) | `td_fb_promotions`, `td_fb_withdrawals`, `td_fb_failopens`, `td_fb_pruned_contexts` (contexts created with ≥ 1 pruned field), `td_fb_untrusted_ctx` (converged contexts whose pruning field later left PROMOTED) |
| CB0 elimination (2026-10-04) | `td_cb0_grants`, `td_cb0_batches`, `td_cb0_admissible`, `td_cb0_items`, `td_cb0_inadmissible{not_new_rv0, gated, iq_stale, lbrm, rv_retry, prg_ptrs, member_stale, llr_scale, gpu_llr, ldpc_error, rank, decoder, contract, budget, no_grantwork, reindexed}`, `td_cb0_budget_skips`, `td_cb0_not_testable` (other k0 etc.), `td_cb0_us_per_item` (CPU-µs incl. lazy signature computes), `td_cb0_backend{cpu, gpu}` (batches per backend), `td_cb0_premise_alarms` (expected 0), `td_cb0_eliminations`; `td_cb0_gpu{submits, items, ok, errors, timeouts, bypassed, sticky, trips, state, mode, failed, skipped}` (first ten = the GPU CB0 entry's own counters, `mode` 1 unified / 2 explicit; `failed`/`skipped` = batches voided / skipped by the backend failure rule and back-off). JSON line buffer raised to 6144 bytes |
| PUSCH | `pusch_try`, `pusch_crc_ok` |
| observations | `obs_pushed`, `obs_written`, `obs_dropped` |

Cross-check (rfsim): last JSON `pdschq_crc_ok` equals the text `PDSCHQ … crc_ok` (§14.5). The four queue counters are
loaded separately (not one atomic snapshot). Other lines:
`SENSING: per-grant observations -> <path> (ring N, drop-on-full)` (writer open) and
`SENSING: ISAC_OBS_PATH=… could not be opened` / `ISAC_OBS_TEST_RING_SLOTS=… invalid` / `ISAC_METRICS_PATH=… cannot be opened` (LOG_W);
`Scan scratch budget <N> MB (ISAC_SCAN_SCRATCH_MB), <x> MB per GSCN, <t> worker threads: scanning <n> GSCN in batches of <b>`
(every initial scan; LOG_W once on a bad env value); with N>1 scan consumers, pdsch decode on and no PDSCH queue, a
LOG_W warns that Phase 2 holds its lock across each in-line decode (A7).

---

## 12. Validation gates (the standard for all work on the DGX)

Every future change is attached to a gate. A gate passes only with the named log evidence, from the stated test
type, on the current commit. "Offline" = deterministic tests without radio; "SIM" = rfsim/ZMQ beds (upper bound);
"OTA" = X410 over the air. For OTA gates: n ≥ 5 runs per arm (the rig swings strongly between runs), arms
alternated A/B/A/B, VOID runs reported not discarded silently, never build during a capture.

| Gate | Input requirements | PASS condition | FAIL condition | Required log evidence | Offline test | OTA test | Depends on |
|---|---|---|---|---|---|---|---|
| **G0 RF** | X410 characterized (§7), NIC tuned | Continuous streaming for the run length at the expected rate; NIC missed delta 0; no overflow/timeout; channels active with plausible power (> noise floor by the expected margin; no clipping) | Any RFSTALL, NIC drops, claim errors, deaf channel | `RFCHAN` per channel, `Actual master clock`, `RFPOW`/`ANTPOW`, `nic.csv` flat, no `RFSTALL` | `benchmark_rate` 300 s × all channels | receiver 600 s capture without RFSTALL | — |
| **G1 PSS** | G0 | PSS detected at a consistent subcarrier/timing, repeatedly (≥ 10 acquisitions or continuous tracking) | detection jumps between PCIs/offsets, no peak | `Cell Detected … SSB SC offset`, stable `TSYNC_OBS e_win_frac≈0.99` | `raw_baseline` SSB checker (python) on recorded IQ; `nr_pbchsim`-style synthetic | lab cell, pinned `--ssb` and scan variants | G0 |
| **G2 SSS/PCI** | G1 | Same PCI every acquisition; matches independent evidence (SIB1 cell identity / gNB config for validation) | PCI flips (co-channel capture) | `Initial sync successful, PCI: P` | SSB checker PCI recovery tests (PCI 0/2/503/1007) | lab + one commercial cell | G1 |
| **G3 PBCH** | G2 | PBCH CRC succeeds on every SSB in tracking | `pbch_fail > 0` sustained | `RFCENSUS pbch_ok=50 pbch_fail=0` for ≥ 10 min | PBCH decode fixtures | 10 min OTA | G2 |
| **G4 MIB** | G3 | MIB fields consistent across frames; SFN increments; k_SSB/pdcch-ConfigSIB1/DM-RS pos stable | inconsistent MIB | CSS0 autoconf line (SA) or `MIB dmrs-TypeA-Position` line (NSA) | `test_nr_ue_mib_blind_handoff` | OTA | G3 |
| **G5A SIB1** | G4, k_SSB < 24 | SIB1 CRC OK repeatedly; PLMN/cell id/carrier/TDD/TDRA extracted; carrier geometry confirmed | no SIB1 in 60 s; geometry mismatch not handled | `SIB1 decoded`, `SIB1 CELL`, `ACQ carrier CONFIRMED`, `TDD from SIB1` | SIB1 prior/TDD tests | OTA lab + commercial | G4 |
| **G5B SIB1 absent / not decoded** (was "NSA / SIB1-less"; SIB1 presence is independent of SA/NSA, §11.5) | G4, k_SSB ≥ 24 (or SIB1 absent) | Receiver proceeds without CORESET#0: dedicated CORESET discovery converges, DM-RS pos from MIB used, RNTI anchored (RAR or persistence), no assert, no SIB1 fabricated | stall at SIB1_DECODED/PBCH_LOCKED, crash, cached SIB1 silently used as truth | `MIB dmrs-TypeA-Position posX -> blind monitor`, bank add, C-RNTI accepts, `ACQ_STATE` progression without SIB1 | `test_nr_ue_mib_blind_handoff` (done) + a synthetic no-SIB1 end-to-end fixture (**missing**) | a cell without decodable SIB1 (Swisscom NSA broadcast SIB1, so it does not test this path; a SIB1-less arm = ignore SIB1 on a SIB1 cell) | G4 |
| **G6 PDCCH acquisition** | G5A or G5B | Dedicated CORESET verified (geometry equals truth where available); PDCCH scrambling ID confirmed; per-AL candidate budgets cover all ALs in use | no bank add in the run budget; AL starvation; wrong geometry banked | `multi-CORESET bank add …`, `PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)]`, ladder `accepts_per_al` | coreset_map, al1_map, blind_monitor gtests; idsweep `--selftest` | OTA lab (truth from gNB log), then commercial | G5 |
| **G7 DCI recovery** | G6 | C-RNTIs recovered and confirmed against truth (lab/sim); DCI lengths locked = truth; layout pinned; false-accept rate measured with a chance baseline | RNTI churn, length flapping, accepts at chance level | `DCI 1_1 length locked … len=`, `UL automatic DCI length locked`, per-RNTI contexts, `held[…]` rates | dci_length_sweep, layout sweeps, bootstrap, joint solver tests | OTA with ≥ 2 UEs; grant census vs gNB log (match key (sfn,slot,rnti,dir)) | G6 |
| **G8 Scheduled-resource reconstruction** | G7 | PRB/symbol allocation, TDRA, DM-RS mask, MCS table, rank, rate matching (SSB/CSI-RS) reconstructed; TB CRC > 0 and ≥ baseline | CRC 0 with healthy constellation; flood of rate-matching errors | `Technique D CONVERGED …`, `LDPCDIAG`, `pdsch_decode[crc_ok]`, `pusch_passive[crc_ok]`, `PDSCH SSB-OBS` | config_sweep, prb_set, ssb_rate_match(_prod), pusch_ra0_*, harq_init_tx, csirs tests | OTA lab: CRC vs 1-RX baseline; commercial cell | G7 |
| **G9 RS / channel extraction** | G8 | CFR produced per source with correct RE mapping; data-aided reconstruction passes its RE/bit-count invariant; per-antenna CFR coherent | invariant violations, misaligned X | `cfr_submits`, `data_submits`, `ul_cfr[submits]`, no reconstruction-mismatch lines | data_aided tests, dlsch_fixed_point, sensing CFR tests | OTA: CFR SNR/coherence vs pilot-only | G8 |
| **G10 Persistent tracking** | G9 | ≥ 30 min continuous: sync held, contexts converged, CRC stable, no unbounded queues/memory | drift to LOST, queue growth, decode collapse | `ACQ_STATE … TRACKING` held, periodic summaries stable | — (soak test in SIM) | 30–60 min OTA soak | G9 |
| **G11 Reacquisition** | G10 | After an injected gap / cell change / RNTI change / config change: stale evidence rejected, fresh broadcast+TB evidence re-establishes TRACKING, outage measured in sample time | stale winners restore TRACKING, cross-UE evidence mixing | `ACQ_STATE … LOST` → fresh chain | raw-IQ replay with injected gaps (`ISAC_RAW_IQ_GAP_*`) | pull antenna / restart gNB / UE re-attach | G10 |
| **G12 Multi-cell** | G10 | Two co-channel PCIs tracked simultaneously, each within tolerance of its single-cell baseline at a stated SIR, no timing capture | weaker cell steals/loses timing | per-cell tagged logs | 2–3 gNB rfsim/ZMQ bed | Salt PCI 64 + PCI 244 class case | G10 |
| **G13 Multi-carrier** | G12 | Two carriers decoded concurrently (separate channels/LOs or channelizer), no added sample loss | loss/drops increase | per-carrier summaries | multi-gNB beds | two carriers OTA | G12 |
| **G14 Multi-operator** | G13 | ≥ 2 operators' cells processed within compute budget, persistent states, bounded rediscovery | full rediscovery loops, starvation | per-cell health + scheduler logs | — | OTA 2 operators | G13 |

**Regression gate (rfsim 106 PRB) — REPLACES the overall-CRC ≥ 98 % criterion on `td/convergence-levers` (operator
option a, 2026-10-02; task GATE `a36eeee63e`, fix round `ce0cdfb973`, merged `b3c531b221`).** Why: since the K39 fix
DM-RS presence no longer pins k0, so the receiver separates k0 candidates by decoding; the search phase is longer
(tda0 ttc ~2–6 s, tda2 ~20–23 s vs ~0.8 / ~12 s before) and its CRC failures pull overall CRC to ~95–97.6 %, while
CRC after convergence stays 100 %. Definition (per run, all must hold): `n_contexts ≥ 2` distinct (rnti, tda) contexts
(first convergence) and `reopens ≤ 0`; post-convergence CRC (window from the first periodic `PDSCHQ` sample at or
after the last context's first convergence to the end) `≥ 99.8 %` over `≥ 5000` grants; `ttc_tda0 ≤ 8.6 s`,
`ttc_tda2 ≤ 35.3 s` (1.5 × idle max); overall CRC floor `≥ 94.5 %` (search-phase sanity); `drop_full ≤ 1 %`. ttc =
CONVERGED time − first log line carrying that RNTI. Calibrated on 10 idle BC9 runs; catches a 2× search slowdown
10/10, 1.5× 5/10, a ~1 % post-convergence decode drop 10/10 (model on measured runs, README); loaded hosts fail the
floor (2/32 loaded runs) — **run the gate only on an idle host (no `nr_td_sim` campaigns, load < 2)**. Option (b), an
SA/SIB1 bed with several UEs, is a later task (§25). Original note: `tests/passive_rx/dgx/rfsim_regress.sh` now defaults to `GATE_MODE=postconv` (operator option a): n_contexts >= 2 with 0 reopens, post-convergence CRC >= 99.8 % over >= 5000 grants, per-context ttc tda0 <= 8.6 s / tda2 <= 35.3 s (1.5 x idle max), overall-CRC search floor 94.5 %, drop_full <= 1 %; `GATE_MODE=legacy` keeps the old overall-CRC >= 98 % criterion (which the K39 search phase fails at ~95-97.6 %). `score_rx.py` gained `contexts`, `ttc_by_tda`, `postconv_crc_pct`, `search_crc_pct`, `ldpc_zero_tb`. [MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @0232f351c3 (code 1a6be6155b)]: 10/10 BC9 idle runs have post-convergence CRC 100.00, ttc0 2.15-5.71 s, ttc2 19.70-23.48 s, overall CRC 95.25-97.57; derivation in `tests/passive_rx/dgx/README.txt`. Option (b), an SA bed broadcasting SIB1, is a later task.

**4-RX regression gate (PROVISIONAL, 2026-10-04) — replaces the 1-RX 150 s gate on the merged main.** Operator rule
2026-10-04: **4 RX for all tests.** At 4 RX the 1-RX thresholds do not apply: the first C-RNTI appears only at ~95–117 s
(on main too), so a 150 s arm ends before convergence. The controller sets these values in `rfsim_regress.sh` on
`td/fast-defaults`; this file only documents them. Bed: phy-test rfsim 106 PRB, receiver `--ue-nb-ant-rx 4`, gNB
`-m 9 -l 1`, **420 s arms**, merged-main defaults ON (§10.2), idle host under the exclusive measurement lock (§26).

| Criterion | Threshold | Origin |
|---|---|---|
| postconv (unchanged from 1 RX) | `n_contexts ≥ 2`, `reopens ≤ 0`, postconv CRC ≥ 99.8 % over ≥ 5000 grants | observed 100.00 % in all 16 arms, ≥ 7404 postconv grants |
| `GATE_TTC_MAX_TDA0` | **≤ 39 s** | 1.5 × max of the CB0-ON arms (26.0 s) |
| `GATE_TTC_MAX_TDA2` | **≤ 77 s** | 1.5 × max (51.5 s) |
| `GATE_CRC_FLOOR` | **93.4 %** | min of the CB0-ON arms (94.21) − 0.75 |
| `GATE_DROP_MAX` | **2.5 %** | observed max 1.71 % over 16 arms; it is a PDCCH scan-queue property (K45/K27), on main too |

Calibration set `[MEASURED, DGX rfsim 106 PRB 4 RX, 420 s, interleaved, flock -x per run, load < 4 at start]`: n = 2 main,
4 levers OFF, 6 CB0 ON (CPU backend) — **small; ≥ 5 idle runs per configuration are owed before the thresholds are final**
(task-CB0WIRE round 2 §4). Configuration-specific alternatives from the same report: levers OFF (K39 default) would need
164 / 216 s and a 68.4 % floor (too loose to catch a 1.5× slowdown, so the gate configuration is CB0 ON); main /
`ISAC_TD_K0_ORACLE_LEGACY=1` 21.7 / 130.4 s, floor 93.5. The calibration arms had fb2 OFF and the CPU backend; the
merged-main default adds fb2 and the GPU backend when available (re-check the margins in the full-combo idle gate, §25).
The 1-RX gate above (8.6 / 35.3 s, floor 94.5, drop 1 %, 150 s) is `HISTORICAL` for the merged main.

---

## 13. Current offline validation

### 13.1 DGX Spark (aarch64) — CURRENT (2026-09-30, commit `3b67eeee39`, code = `621bdc32a2`)

Host `spark-74c3` (§4.2), fresh Ninja build, `ENABLE_ISAC_SENSING=ON`. Evidence:
`tests/passive_rx/dgx_host_snapshot_2026-09-30/`.

| Validation | Command | Result on DGX | Status |
|---|---|---|---|
| Full ctest suite | `ctest -j4 --output-on-failure` (31 s) | **123/126**. Failures: `test_nr_pusch_ra0_qam256` (K22), `dft_test` (K21, upstream NEON), `test_nr_modulation` `NrLayerPrecoderTest.SIMD` (upstream, imag ±2 LSB). `test_vrtsim_cirdb` **passed** here. `test_nr_pusch_ra0_qam64` passed in this run but **fails intermittently** on re-runs (K22) | **PASS with 3 ARM failures, all triaged** |
| Blind-monitor shuffle | `./test_nr_pdcch_blind_monitor --gtest_shuffle --gtest_random_seed={1,3,5}` | 195 pass + 2 skips on each seed | PASS |
| Receiver suites by gate (§14) | covered by the full run | all receiver gtests (blind monitor, CORESET map/bank, AL1, RNTI bootstrap, GF(2)/joint, DCI length/layout/pin, hyp sweep, Technique D config sweep, Qm/xOverhead/PT-RS/PRB set, SSB rate match, CSI-RS 131+synth, DM-RS ID, HARQ init-TX, acq state, BWP, TDD, MAC TA, MIB hand-off) pass; of the 9 PUSCH RA0 tests, qpsk/qam16/multidmrs/short_segments/late_typeb/type2/fdra_collision pass, qam64/qam256 do not (K22) | PASS except K22 |
| Raw-baseline python | `python3 -m unittest test_ssb_reference test_ssb_normalization test_validate_raw` | 9/9 OK | PASS |
| idsweep selftest (CPU) | `gcc -O3 -march=native -fopenmp …idsweep_offline.c` then `--selftest` | css0 id=2/sym0 PASS, rest id=12345/sym3 PASS; 9.9 s wall | PASS |
| idsweep selftest (GPU, **sm_121**) | `nvcc -O3 -arch=sm_121 -c idsweep_gpu.cu` + `g++ … -DUSE_GPU … -lcudart` then `--selftest` | both PASS; **0.9 s** wall (11× CPU) | PASS |
| Offline sync contract | aarch64 port `tools/offline_sync_arm.sh` (K26) | OfflineSync.* **5/5** | PASS |
| NEON DFT/IDFT accuracy (receiver mode `scale_flag=1`) | `tools/dftcheck.c` vs double-precision DFT | 128…4096 incl. 1536/2048/3072/4096: SQNR 49–56 dB, scale 1/√N | PASS (K21 is test/convention-only for OFDM) |
| SSB checker on recorded IQ / decoder replay | datasets not on the DGX | — | NOT RUN (K18) |

**Fresh result 2026-09-30 on sens6** (HISTORICAL host; this consolidation): `ctest -j4 --output-on-failure` in
`sens6:/home/sens/NICOLA/agn-wt/cloud/cmake_targets/ran_build/build` (code `621bdc32a2` = HEAD code, built 2026-09-28,
ENABLE_ISAC_SENSING=ON): **125/126 passed in 42.3 s; only failure `test_vrtsim_cirdb`** (upstream vrtsim shared-memory
race, `shm_open() failed: errno 2`, unrelated to the receiver). Log:
`tests/passive_rx/sens6_host_snapshot_2026-09-30/evidence_2026-09-30/ctest_head_equivalent_621bdc32a2.txt`.

| Validation | Dataset / input | Command | Expected | Current result | Status |
|---|---|---|---|---|---|
| Full ctest suite | built-in fixtures | `ctest -j4 --output-on-failure` | 125/126 (`test_vrtsim_cirdb` known) | 125/126 (2026-09-30) | **PASS** |
| Blind PDCCH core (CSS0 autoconf, extraction, gates, layout, UL sweeps, DL adaptive, pin) | synthetic PDCCH | `ctest -R test_nr_pdcch_blind_monitor` | pass, 2 known skips | part of 125 | PASS |
| Blind PDCCH shuffle robustness | same | `./test_nr_pdcch_blind_monitor --gtest_shuffle --gtest_random_seed={1,3,5}` | 195 pass + 2 skips per seed | seeds 1/3/5 green on 2026-09-28 (`a13c2b9a06`/fix, cloud session) | STALE (not re-run today) |
| CORESET map / bank / AL1 map / USS / SS registry | synthetic | `ctest -R 'coreset_map|al1_map|ss_registry'` | pass | pass | PASS |
| RNTI bootstrap, GF(2) RNTI, joint solve/live | synthetic Gold/polar | `ctest -R 'rnti_bootstrap|gf2_rnti|joint'` | pass | pass | PASS |
| DCI length sweep, DCI 1_1 / 0_1 layout sweeps, DCI 1_1 pin | synthetic payloads | `ctest -R 'dci'` | pass | pass | PASS |
| Hypothesis engine + CRC evidence | synthetic | `ctest -R test_nr_hyp_sweep` | pass | pass | PASS |
| Technique D / PDSCH config sweep | synthetic contexts | `ctest -R test_nr_pdsch_config_sweep` | 44 pass + 1 skip (`PdschReset.Timing`) | pass | PASS |
| Qm oracle, xOverhead, PT-RS unav, PRB sets, adaptive DL | synthetic | `ctest -R 'qm_oracle|xoverhead|ptrs|prb_set|dl_adaptive'` | pass | pass | PASS |
| SSB rate matching (unit + production-linked Z1–Z15) | synthetic SSB/PDSCH grids | `ctest -R test_nr_ssb_rate_match` | pass | pass | PASS |
| CSI-RS blind search + synth chain | synthetic CSI-RS/ZP scenes | `ctest -R 'csirs_blind'` | 131 + synth PASS | pass | PASS |
| DM-RS ID estimate, scrambling-ID sweep, HARQ init-TX | synthetic | `ctest -R 'dmrs_id|scrambling_id|harq_init'` | pass | pass | PASS |
| Passive PUSCH RA type 0 (QPSK..256QAM, multi-DMRS, short segments, late type B, type 2, FDRA collision) | real encoded UL waveforms (ulsim encoder) | `ctest -R test_nr_pusch_ra0` | 9 pass | pass | PASS |
| MIB → blind monitor hand-off (NSA pos3, repeated MIB, no-CSS0) | real MAC MIB path | `ctest -R test_nr_ue_mib_blind_handoff` | 6 pass | pass | PASS |
| Acquisition state, passive BWP, TDD pattern, MAC TA, MRC weights, arm sweep | synthetic | `ctest -R 'acq_state|passive_bwp|tdd_pattern|mac_ta|mrc_weights|arm_sweep'` | pass | pass | PASS |
| UE RA procedures (link stubs) | unit | `ctest -R test_nr_ue_ra_procedures` | pass | pass (was a link failure until the misc lane) | PASS |
| Sensing tests (`test_isac_*`, trackers, ECA, AoA, …) | synthetic | `ctest -R 'isac|tracker|eca|matrix|det_quality|sparse|clean|occ'` | pass with ENABLE_ISAC_SENSING=ON; **do not link with OFF** | pass (ON) | PASS (out of receiver scope) |
| Raw-baseline SSB reference / normalization / validator | synthetic + no dataset | `cd tests/passive_rx/raw_baseline && OPENBLAS_NUM_THREADS=1 python3 -m unittest test_ssb_reference test_ssb_normalization test_validate_raw` | OK | **9/9 OK (2026-09-30)** | PASS |
| SSB checker on recorded raw IQ | `captures/raw_batch.*` (4 × 4 s × 122.88 MS/s) | `python3 check_ssb_reference.py <dir> --ssb-center-hz 3408960000 --channel 2 --output …` | PCI 2, 20 ms period | dataset **missing** on sens6 | NOT RUN |
| Same-build job replay (decoder oracle) | `captures/adaptive_ul_dl_mrc2.*/replay.sh` (55/55 DL TBs byte-identical, 2026-09-11) | `bash <dir>/replay.sh` | 55/55 | dataset **missing** | STALE / NOT RUN |
| Offline sync contract | synthetic | `tests/passive_rx/offline_sync_contract/build_and_run.sh` | OfflineSync.* pass | not run (x86-only build script) | NOT RUN |
| DCI regression wrapper | = blind_monitor gtest | `tests/passive_rx/agnostic/run_dci_regression.sh` | focused+regression exit 0 | covered by ctest | PASS (via ctest) |
| idsweep stage-1/2 self-test | synthetic | `./idsweep_offline --selftest` (snapshot `discovery_tool/`) | selftest pass | not run today | NOT RUN |
| Physim link (`nr_psbchsim`, `nr_ulsim`, `nr_srssim`, `nr_ulsim_mu_mimo`) | — | build | link | pre-existing link failures | FAIL (known, out of scope) |

### 13.2 Cloud x86 (Xeon 2.8 GHz, 4 cores) — 2026-10-01 — NOT the DGX, NOT merged with §13.1

Host: Intel Xeon @2.80 GHz (avx512f, no GFNI), 4 cores, 15 GB, container, Ubuntu 24.04, gcc 13.3, root; no IPv6, no SCTP,
`pthread_getaffinity_np` EINVAL; build `-DOAI_USRP=OFF`, `-j4` 11 min.

**Final gates at the end of the session (code = final-review fixes `6528bd0cfc`..`343d1f062a` + base merge `d6cb580ce5`):** ctest **126/129**, failures env-only (`test_thread-pool`, `nr_cuup_functional_test`, `time_management_tests` intermittent); blind-monitor shuffle seeds 1/3/5: **197 pass + 2 skips** each; sens6 frozen diff empty; `ENABLE_ISAC_SENSING=OFF` `nr-uesoftmodem` links. `[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, d6cb580ce5]` Evidence: `tests/passive_rx/cloud_run_2026-10-01/final_gates/`.

| Validation | Result | Label |
|---|---|---|
| ctest on HEAD `be2e7fa4b6` (before this session's code) | **123/126**; failures env-only: `test_thread-pool` (affinity EINVAL), `nr_cuup_functional_test` (SCTP unsupported), `time_management_tests` (intermittent timing, passes alone). `test_vrtsim_cirdb` passed | `[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, be2e7fa4b6]` |
| DGX ctest note 2026-10-02 | `nr_cuup_functional_test` failed on the DGX because GTP-U port 2152 was in use by an unrelated docker OAI SA bed (`/opt/oai-gnb`, `/opt/oai-nr-ue`, 192.168.71.140). Environment-only, like the cloud SCTP case; the docker bed also loads the host (quiet host needed for the gate) | `[MEASURED, DGX, 2026-10-02]` |
| ctest after A2/A3 | 124/127, same env-only set | `[OFFLINE VERIFIED, cloud x86 …, 385e02b9cf]` |
| ctest after A7 (last full run) | **126/128**; only `test_thread-pool`, `nr_cuup_functional_test` fail | `[OFFLINE VERIFIED, cloud x86 …, b6e5fb27ac]` |
| Blind-monitor gtest, shuffle seeds 1/3/5 | 197 pass + 2 skips each (195 + 2 `Phase2Concurrent*`) | `[OFFLINE VERIFIED, cloud x86 …, 1d6cbdf5c3]` |
| New gtests | `test_nr_passive_metrics` 5, `test_nr_passive_obs` 8 (also TSAN, 0 warnings), `test_nr_initial_sync_budget` 5, `Phase2Concurrent*` 2 (red/green shown; TSAN 13 races → 0) | `[OFFLINE VERIFIED, cloud x86 …, 1d6cbdf5c3 / 843e5cff49]` |
| New python tests | `test_score_rx.py` 4 (1 skip without fixture), `test_campaign.py` 15, `test_health.py` 16 (+`test_monitor.py`), `test_run_rx_dgx.py` 12 (dry-run, `ONLINE_CPUS_OVERRIDE=0-19` for the DGX map) | `[OFFLINE VERIFIED, cloud x86 …, cf8e2e79d7 / 876c8ce5b9]` |
| `ENABLE_ISAC_SENSING=OFF` | `nr-uesoftmodem` links after A2+A3 and after A7 (gtest 197+2) | `[OFFLINE VERIFIED, cloud x86 …, 2547ff90aa / b6e5fb27ac]` |
| Offline sync contract (A13) | x86 fallback path (no system libgtest; CPM gtest) **OfflineSync.* 5/5**; x86 old vs new command lines identical (echo-diff, system-gtest case); aarch64 branch == `offline_sync_arm.sh` by echo-diff only | `[OFFLINE VERIFIED, cloud x86 …, 3b6119853f]`; aarch64 run = DGX follow-up |
| Full `ninja` | `nr_psbchsim`, `nr_srssim`, `nr_ulsim`, `nr_ulsim_mu_mimo` fail to link (undefined `nr_isac_*`, `nr_ue_diag_*`, …) — pre-existing (K15), not in ctest | `[KNOWN ISSUE]` |

Frozen sens6 paths (`captures`, `*.conf`, `sens6_host_snapshot_2026-09-30`) unchanged against `sens6-frozen-2026-09-30` at every commit.
Evidence: `tests/passive_rx/cloud_run_2026-10-01/{a1_baseline,a2_metrics,a3_obs,a4_campaign_smoke,a5_dashboard,a7_concurrency,a11_scan_scratch,a13_sync_contract}/`.

---

### 13.3 DGX verification of the merged tree (2026-10-01, `7f2fb28acf`, aarch64) — CURRENT

| Check | Result |
|---|---|
| Build (Ninja, `ENABLE_ISAC_SENSING=ON`) | 0 errors |
| ctest | **126/129**; failures = known ARM set only: `dft_test` (K21), `test_nr_modulation`, `test_nr_pusch_ra0_qam256` (K22); the new tests (metrics, observations, …) pass |
| Blind-monitor shuffle seeds 1/3/5 | 198 pass + 2 skips each (A7 + follow-up tests included) |
| Python: `dgx/test_score_rx.py`, `dgx/test_run_rx_dgx.py`, `campaign/test_campaign.py`, `monitor/test_health.py`, `monitor/test_monitor.py` | 4 / 21 (2 skipped) / 15 / 16 OK, monitor self-test OK |
| A13 aarch64: `offline_sync_contract/build_and_run.sh` | OfflineSync.* **5/5** (aarch64 branch verified) |
| GPU (`build_gpu`, `-DLDPC_CUDA_ARCH=121`) | builds; `nr_pdsch_gpu_fep_test` OK, `nr_polar_sc_cuda_test` bit-exact, `nr_pdcch_gpu_fep_test` internal-consistency only (K17, K34–K36) |

Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/merge_2026-10-01/`. `[OFFLINE VERIFIED, DGX aarch64, 2026-10-01, 7f2fb28acf]`.

### 13.4 Branch `td/convergence-levers` offline checks (2026-10-01 → 10-04, DGX aarch64; local merge into `adaptive-rx-UL-DL` 2026-10-04)

| Check | Result | Label |
|---|---|---|
| Full `ctest -j4` (BC9d, BC6) | **133/137**; failures = the known ARM set only (`dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam256`, `test_nr_pusch_ra0_qam64`) | `[MEASURED, DGX unit test, @852b9bb86f]` |
| Focused ctest `-R "nr_td\|td_sim\|pdsch_config_sweep\|dci_history\|tdd_pattern"` | 8/8 (BC9d); 10/10 focused on the merged tree `ca1cbc5470` (with BC12a tests) | `[MEASURED, DGX unit test]` |
| New gtest binaries | `test_nr_td_gate`, `test_nr_td_order`, `test_nr_td_fieldbook`, `test_nr_td_legal`, `test_nr_pdsch_chest_key`, `test_nr_passive_sample_lifetime`, `test_nr_dci_history`, `nr_td_sim_test` (+ new cases in `test_nr_pdsch_config_sweep`, `test_nr_tdd_pattern`, `test_nr_passive_acq_state`, `test_nr_passive_metrics`). Engine fixes were shown RED first and mutation-checked per binding line (ledgers) | `[MEASURED, DGX unit test]` |
| Python | `tests/passive_rx/dgx/test_score_rx.py` (postconv scorer incl. fixture `fixtures/idle_on_r1_trim_rx.log`), `tests/passive_rx/td_sim/test_campaign.py` | `[MEASURED, DGX unit test]` |
| sens6 freeze | `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` clean at `83ca2b9d6c` (checked 2026-10-02 for this update) | `[MEASURED]` |
| Link note | `nr_ulsim`/`nr_psbchsim` fail to link in a fresh worktree build (undefined `nr_isac_enabled`, `nr_ue_diag_producer_absolute_slot`) — pre-existing (K15), not in the CLAUDE.md build set | `[MEASURED]` |
| **Final tree of the acceleration round `003b8c3f93`** (built with `-DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121`) | build OK; full ctest: **only the 4 known ARM failures**; sens6 freeze check clean (ledger "FINAL levers 003b8c3f93") | `[MEASURED, DGX unit test, @003b8c3f93]` |
| Merge step 1 (`334c0bfca8` = G1 + K38 + R2fb) | ctest 135/139, only the known ARM set | `[MEASURED, DGX, flock]` |
| New suites 2026-10-03/04 | `test_ldpc_cuda_pool` 20/20, `test_ldpc_cb0_cuda` 13/13, `test_nr_td_cb0_gpu_backend` 6/6 (GPU, `[MEASURED, DGX GB10]`, also with `LDPC_CB0_MEM=explicit`); `test_nr_llr_norm` 5/5; `test_nr_td_grantwork` 16/16; `test_nr_td_cb0_batch` 9/9; `test_nr_td_cb0_wire` 16/16; `test_nr_passive_metrics` 9/9; new cases `PdschFieldBook.*`, `PdschSweepCb0.*`, `PdschCb0Ctx.*` in `test_nr_pdsch_config_sweep`, `TdSimCb0.*` in `nr_td_sim_test` | `[OFFLINE VERIFIED, DGX unit test]` |
| Load-sensitive tests on a shared host | `time_management_tests`, `nr_cuup_functional_test` (GTP-U port 2152 held by another bed) and once `test_nr_pdcch_al1_map` failed only under load / port contention and pass alone — environment-only | `[MEASURED, DGX, shared host]` |
| x86 build of the new code (sens6 / cloud) | **not built** (no cross toolchain on the DGX); CPU path is plain C + simde, CUDA dlopen'd `[CODE-READ]` | — |

## 14. Exact offline validation procedure (run on the DGX after the build)

```text
build (fresh clone, fresh cmake)
  -> ctest full suite                          (software sanity for gates G2..G9 logic)
  -> blind-monitor shuffle x3 seeds            (test isolation; G6/G7)
  -> receiver suites by gate (below)
  -> raw-baseline python tests                 (G1/G2 tooling)
  -> idsweep selftest                          (G6 discovery tool)
  -> phy-test rfsim bed smoke (SIM)            (end-to-end G5A..G8 without radio; CPU budget on ARM)
```

```bash
cd <repo>/cmake_targets/ran_build/build
ctest -j4 --output-on-failure 2>&1 | tee ctest_dgx.log            # expect 125/126, test_vrtsim_cirdb failing
for s in 1 3 5; do ./test_nr_pdcch_blind_monitor --gtest_shuffle --gtest_random_seed=$s --gtest_brief=1; done
ctest -R 'test_nr_ue_mib_blind_handoff'                          # G4/G5B
ctest -R 'coreset_map|al1_map|ss_registry|pdcch_blind'           # G6
ctest -R 'dci|rnti_bootstrap|gf2_rnti|joint|hyp_sweep'           # G7
ctest -R 'pdsch_config_sweep|qm_oracle|xoverhead|ptrs|prb_set|ssb_rate_match|csirs|harq_init|pusch_ra0|dl_adaptive'  # G8
ctest -R 'dmrs_id|scrambling_id|acq_state|passive_bwp|tdd_pattern|mac_ta'                                           # G8/G10
cd ../../../tests/passive_rx/raw_baseline && OPENBLAS_NUM_THREADS=1 python3 -m unittest test_ssb_reference test_ssb_normalization test_validate_raw
cd ../sens6_host_snapshot_2026-09-30/discovery_tool && gcc -O3 -march=native -fopenmp -I<repo> idsweep_offline.c -o idsweep_offline -lm && ./idsweep_offline --selftest
```

Then the SIM smoke (proves the whole chain runs on the new CPU and measures its budget): phy-test bed §10.4 at 106 PRB
rank 1, 150 s, fully agnostic receiver config. Baseline on sens6 (2026-09-27, `sdd/integration` receiver, HISTORICAL):
**Technique D CONVERGED 5/5 runs, time-to-converge 1.31 ± 0.04 s, crc_ok ≈ 67 000/67 800 (98.9 %), scanq drop_full
0.33 %**, converged on the gNB's own TDRA entries (S1 L13, S1 L5).

### 14.1 DGX Spark phy-test results — CURRENT (2026-09-30, `[SIM VERIFIED]`, aarch64)

Receiver = HEAD (`3b67eeee39`, code `621bdc32a2`), gNB = **plain HEAD `nr-softmodem --phy-test`** (the test-gNB
branch is not on GitHub, K23; the baseline arm needs no knobs), both on the DGX, **run as the normal user (no sudo,
no RT priority, no pinning)**. Receiver conf = in-tree **fully agnostic** `tests/passive_rx/ue.passive.bwp.agn.conf`
(106 PRB; no manual CORESET/SS/BWP/TDA/DCI/DM-RS keys; sensing engine disabled) — *not* the sens6 scratch conf
`phyA2/ue.passive.q.agn.conf` (not in git), so the comparison with the sens6 baseline is close but not identical.
Launch: `tools/run.sh` / `tools/run2.sh` (gNB `-D 0xff -O gnb.sa.rfsim.conf --rfsim`; receiver `-C 3319680000 -r 106
--numerology 1 --band 78 --ssb 516`), scored by `tools/score.sh`; scores in `phytest/scores_*.txt`. ttc = time from
the first `rnti=0x1234` line to the first `Technique D CONVERGED`.

| Arm (150 s each) | Runs | CONVERGED | ttc (s) | TB CRC (LDPC ok / PDSCHQ decoded) | scanq drop_full | Receiver CPU | Notes |
|---|---|---|---|---|---|---|---|
| 106 PRB rank 1, MCS 9 table 0 (baseline) | 5 | **5/5**, both gNB TDRA entries (S1 L13, S1 L5) every run | 0.74 / 1.61 / 0.80 / 0.81 / 1.56 (mean 1.10) | **98.6–99.1 %** (56–58 k TBs/run) | **0.03–0.08 %** | ~3.0 cores, RSS 0.69 GB | CORESET banked `len=46`, dmrs_id CONFIRMED, 0 PDSCHQ drops |
| 106 PRB rank 1, **MCS 25 table 1 (256QAM DL)** | 2 | 2/2, **mcs_table=1 found blind** (Qm oracle qm=8) | 0.80 / 0.67 | **99.5 / 99.6 %** | 0.05 % | ~3.3 cores | DL 256QAM is fine on ARM (contrast K22 = UL) |
| **273 PRB** rank 1, MCS 9 table 0 (`gnb.sa.rfsim.100mhz.conf` + `ue.passive.auto.100mhz.conf`, `-C 3750000000 -r 273 --ssb 1478`) | 2 | 2/2 (S1 L13, S1 L5) | 3.71 / 3.92 | **95.3 / 95.9 %** (23.9 k / 27.6 k decoded) | 0.04–0.05 % | ~2.7 cores, RSS 1.08 GB | CORESET banked `len=49`; first C-RNTI accept at 24–29 s (vs 14 s at 106 PRB) |
| 273 PRB **rank 4, 4 RX** (`gnb.sa.rfsim.100mhz.rank4.conf -m 25 -n 1 -M 273 -l 4` + `ue.passive.autor4.100mhz.conf --ue-nb-ant-rx 4`) | 2 | **0/2 — never synced** | — | — | — | — | see §14.2 |

Comparison with the sens6 baseline (2026-09-27, HISTORICAL: CONVERGED 5/5, ttc 1.31 ± 0.04 s, 98.9 %, drop_full 0.33 %):
**the DGX matches or beats it** at 106 PRB (same convergence, ~same CRC, 4–10× lower scan-queue drops) with only
~3 of 20 cores busy. The ARM CPU is **not** the bottleneck for one 106/273-PRB 1-RX receiver in rfsim.

**Thread profile (per-thread CPU over 20 s, `tools/thrprof.sh`), 273 PRB 1 RX, tracking:** `UEthread_0` (PHY receive
thread) **100 %**, `pdcchUssHash` (USS hash/AL tracker, `nr_pdcch_uss_tracker.c`) **90 %**, `passivePdcch0` (blind
PDCCH scan consumer) **79 %**, `UL__actor` ×2 9 %, `passivePdsch0..2` 6 % each, `Tpool*` 2–3 % each; total 331 %. →
The receiver is limited by **three serial threads**, not by core count (§14.3).

### 14.2 Rank-4 / 4-RX bed on the DGX (2026-09-30) — FAILED to sync, cause not ARM-specific (probably)

Both 150 s runs: the first initial-sync attempt ran **~90 s** (t = 4 → 94 s) with `UEthread_0` at 100 % and only
`Tpool0..2` busy (30 %, 30 %, 14 %), then `Initial sync: pbch not decoded on any branch, ssb index 0` → `synch Failed`;
no second attempt completed. `[HYPOTHESIS]` the PBCH failure is the **known bed issue** recorded on sens6 (2026-09-27:
the agnostic `.q`-geometry scene in `sensing_channel.c` with nb_tx/nb_rx = 4 does not PBCH-sync; the working rank-4
bed used the *pinned* `ue.passive.pin49r4.100mhz.conf` → 100 % CRC) — **not re-tested here with `pin49r4`**. The
**90 s single-threaded initial sync at 273 PRB × 4 RX** is itself a finding: the GSCN/PSS scan is effectively serial
on this CPU (cf. §8 X20) — a parallelization target (§14.3) and a reason to keep pinning `--ssb` at 273 PRB × 4 RX.

### 14.4 Branch `port/multirx-rx-fixes` — validation of the ported receiver fixes (2026-10-01, DGX, `[SIM VERIFIED]` except the scan-confirm fix)

Ported from `feature/multirx-clean-adaptive` (triage in K2): scan-confirm CFO fix (3 commits), BRANCHFO
serialisation + CRC-gating + decode-entry reset (default **OFF**), P39 single-branch PDSCH chest, `_ss` help text.
Build clean; ctest = same 4 ARM failures as HEAD (K21, K22 incl. intermittent qam64), nothing new; sens6 files unchanged.

| Arm (150 s) | Result |
|---|---|
| 106 PRB rank 1 baseline ×2 | CONVERGED 2/2 (S1 L13, S1 L5), ttc 1.41 / 1.58 s, 98.6 % / 98.6 %, drop_full 0.03–0.05 % — same as HEAD (§14.1) |
| 273 PRB **rank 4, 4 RX, pinned `ue.passive.pin49r4.100mhz.conf`**, default env | **100.0 % (3106/3106)**, Nl=4 Qm=8 — matches the sens6 rank-4 bed (3266/3270). The pinned conf syncs where the agnostic `autor4` did not (§14.2) |
| same, `ISAC_RX_MRC_MODE=0 ISAC_RX_BRANCH_FO=1` | **15.3 %** (478/3129); BRANCHFO integrates a spurious +55.7 Hz on branch 3 in an ideal channel → **do not enable BRANCHFO at rank > 1** (it measures the per-branch slope on layer-0 estimates); default OFF kept |

Not validated (needs OTA): the scan-confirm CFO fix (rfsim has no LO/CFO) — pass = with `--ue-scan-carrier` the
confirm pass measures the full CFO (≈ scan pass), no LOG_W "confirm-pass offset < 0.1x", and SIB1 decodes. P39 engages
only at rank 1 / 4 RX with a planned branch (not exercised by these beds).
Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/phytest/scores_port_branch.txt`.

### 14.3 CPU core allocation on the DGX and parallelization plan (design, 2026-09-30)

**Topology** (`lscpu -e`): 2 clusters with separate L3 — cluster 0 = cpus 0–9, cluster 1 = cpus 10–19; each cluster =
5 × Cortex-A725 (little, 2.8 GHz: 0–4 / 10–14) + 5 × Cortex-X925 (big, 3.9 GHz: 5–9 / 15–19). No isolcpus, no RT
kernel (K25). Keep one receiver instance inside **one cluster** (shared L3) and put every thread that is on the
per-slot critical path on an **X925** core.

**Where the time goes** (§14.1 profile, 273 PRB 1 RX): 3 serial threads ≈ 1 core each — `UEthread_0` (RF read +
per-slot RT path incl. FEP, PBCH, blind-monitor RT tap), `pdcchUssHash`, `passivePdcch0`; everything else < 10 %.
At 4 RX the initial sync is serial for ~90 s (§14.2). So adding cores does nothing until those threads are split.

**Pinning knobs that exist today (no code change):**

| Thread | Knob | Proposed core (instance A = cluster 0) | Instance B (cluster 1) |
|---|---|---|---|
| `UEthread_0` (PHY receive / RF reader) | `ISAC_UE_RT_CORE=<cpu>` | **5** (X925), alone | 15 |
| `passivePdcch0..N-1` (blind PDCCH scan; N > 1 is thread-safe since A7 + follow-up, §14.5, but untested on the DGX) | `pdcch_blind_monitor_scan_thread = "1:8:<cpu>"` | **6** (X925) | 16 |
| `pdcchUssHash` (USS AL/hash tracker) | `ISAC_PDCCH_USS_CORE=<cpu>` | **7** (X925) | 17 |
| `Tpool*` (per-antenna FEP/chest, LDPC segments pushed by the RT path) | `--thread-pool 8,9,0,1` | **8, 9** (X925) + 0, 1 (A725) | 18,19,10,11 |
| `passivePdsch*` decode consumers (queue, latency-tolerant) | `pdcch_blind_monitor_pdsch = "…:<n>:<depth>:<first_cpu>"` (consecutive cores) | 2, 3, 4 (A725) | 12,13,14 |
| `passivePusch*` UL consumers | `pdcch_blind_monitor_ul_thread = "<n>:<depth>:<first_cpu>"` | 0–1 shared (A725) | 10–11 |
| SYNC/DL/UL actors (near idle in passive mode) | `--sync-actor-core`, `--dl-actor-core-start`, `--ul-actor-core-start` | A725 0–4 | A725 10–14 |
| GPU FEP worker (only with GPU FEP) | `ISAC_GPU_WORKER_CORE` | A725 | A725 |
| X410 NIC IRQs (CX-7 port) | `/proc/irq/*/smp_affinity_list` from `/sys/class/net/<nic>/device/msi_irqs` | **not** on 5–7; start with A725 3–4 and measure `rx_missed_errors` | — |

Warnings: the in-tree `run_arm.sh` defaults (`RTCORE=2`, `--thread-pool 0,1,4,5,6,7`, IRQs 8–13, `taskset 0-7`) and the
conf files' pinned cores (`scan_thread "1:8:5"`, `ul_thread "2:32:2"`, `pdsch …:2`) were chosen for sens6; on the DGX,
core 2 is a little core. Without RT priority (`ulimit -r 0`, no sudo) pinning is only affinity; for OTA run as root or
grant `rtprio`. **This map is a starting point to be measured** (RFSTALL, `scanq drop_full`, `over_slot`, NIC missed),
not a validated setting.

**Status 2026-10-01 (cloud session):** the core-map launcher exists — `tests/passive_rx/dgx/run_rx_dgx.sh` +
`coremap_dgx.env` + `ue.passive.auto.100mhz.dgx.cfg` (+ `COREMAP=1` in `rfsim_arm.sh`), commit `876c8ce5b9`;
`[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 876c8ce5b9]` as **dry-run only** (12 tests; on the 4-core host
`COREMAP=1` stops with rc=3 "core 5 is not online"). **No measured A/B of the map exists — still DGX-only (§25).** Note:
with `COREMAP=1` and the default frozen RXCONF the scan thread (core 5) collides with the RT core; use the `.dgx.cfg`.
Item 4 below (N scan consumers) is now implemented (K27, §14.5); the 273-PRB pinned A/B is still open.

**Parallelization opportunities, ordered by measured benefit** (all implementable and testable offline/rfsim):

1. **Split `UEthread_0`**: a thin RF reader (recv → IQ ring, nothing else) + per-slot work dispatched to the existing
   `DL__actor` threads (4 of them sit at < 1 % in passive mode). Removes the single-thread ceiling on the RT path and
   protects RF draining (RFSTALL risk, K3).
2. **Parallel initial sync**: GSCN candidates × PSS hypotheses × antennas × CFO hypotheses are independent → fan out on
   the thread pool (or GPU). Today ~90 s at 273 PRB × 4 RX (§14.2).
3. **`pdcchUssHash` → data-parallel**: the hash/AL search is independent per RNTI hypothesis and per occasion → shard
   the RNTI range over K threads or move it to the GPU (same pattern as idsweep, 11× on GB10).
4. **Blind-PDCCH scan with N consumers**: **DONE in code, 2026-10-01 (A7 + follow-up, §14.5)** — N consumers by
   occasion, TSAN-clean in the scan code on the cloud rfsim bed. Still open: measure on the DGX at 273 PRB whether N > 1
   actually relieves `passivePdcch0` (the 4-core cloud A/B showed the work splitting, not a speed-up); parallelizing
   inside an occasion (per-candidate/per-AL decode) is not started.
5. **GPU on unified memory**: the old "GPU LDPC 20× slower" result (K17) was PCIe-transfer-dominated on a discrete
   4060 Ti; GB10 shares memory with the CPU, so batched PDCCH candidate decode, USS hash, PDSCH FEP/LLR and LDPC
   across grants/antennas/cells must be **re-measured**, not assumed slow. **2026-10-04 (CB0 LDPC re-measured, §14.10):**
   a dedicated CB0 batch entry reaches 77–81 µs per wrong CB0 on GB10, only 1.05–1.4× an 8-thread CPU, because the
   flooding int8 decoder is memory-bandwidth bound; unified vs explicit copies differ by ≤ ~5 %. A bigger GPU gain
   needs a different (layered / on-chip) CB0 decoder and a new dominance proof (§25 deferred).
6. **Multi-cell/-carrier (§17–§20)**: today one process = one cell (global state). First step = one process per cell,
   each pinned to its own core set (≈ 3 hot X925 cores per instance → ~3 instances on 10 X925 cores as the code
   stands); then shared RF ring + per-cell `CellContext`; then shared GPU batches across cells.

### 14.5 Blind-PDCCH scan with N consumers + race fixes (Task A7 + follow-up, 2026-10-01, cloud x86, NOT DGX)

Host: cloud container, x86 Xeon 2.8 GHz, **4 cores**, no GPU, consumers unpinned (`scan_thread` core field -1).
Results are from that host only (§0.1 rule 5); nothing here has run on the DGX or OTA.

- **A7** (commit `b6e5fb27ac`, branch `cloud/dgx-next-steps`): Phase-2 lock (`nr_pdcch_blind_phase2.{c,h}`), atomic
  occasion counters, RNTI-persistence ring + energy floor made safe; the >1-consumer warning removed.
  `[OFFLINE VERIFIED]` gtest 197 + 2 skips; `[SIM VERIFIED]` partial-TSAN rfsim passivePdcch0↔1 races 26 → 0.
  Evidence: `tests/passive_rx/cloud_run_2026-10-01/a7_concurrency/README.md` (incl. the S/M A/B: CRC and acc/occ
  within run-to-run spread, total scan CPU +10 %, no speed-up claimed on 4 cores).
- **Follow-up** (commit `71dbfd582a`, branch `claude/elegant-davinci-jlrfol` = `cloud/dgx-next-steps` + this fix):
  the two groups A7 left, both present with ONE consumer too:
  1. `UEthread_0` ↔ `passivePdcchN` in `nr_pdcch_blind_monitor.c` (Technique A histogram vs
     `note_rnti_for_windows()`/`autodiscover_next()`; footprint commit rewriting `g_cfg` on the receive thread).
     Fix: leaf mutex `s_techA_mu` around the shared arrays only (receive thread snapshots, never takes the Phase-2
     lock); **deferred commit** — with the scan pool running, the receive thread posts its decision and
     `run_occasion()` applies it on a consumer under the Phase-2 lock (no pool = applied at once, as before);
     SS occasion gate read via `nr_pdcch_blind_monitor_occasion_gate()` (one atomic word).
  2. passive PDSCH pool: lazily resolved `getenv` statics in `nr_pdsch_passive_queue.c` / `nr_pdsch_passive_decode.c`,
     shared log budgets, `g_sfo_ppm_ema` → `_Atomic`; **`task_ans.c` join counter relaxed → acq_rel** (upstream file,
     §3.2: other LDPC workers' segment writes were unordered w.r.t. the joiner — benign on x86, a real hazard on aarch64).
  `[OFFLINE VERIFIED]` `test_nr_pdcch_blind_monitor` **198 pass + 2 skips** (A7's 197 + the new
  `DiscoveryGates.DeferredCommitIsAppliedByTheConsumerNotTheReceiveThread`), shuffle seeds 1/3/5 same.
  `[SIM VERIFIED]` partial TSAN (A7 method + `task_ans.c`), rfsim 106 PRB, `scan_thread "2:16:-1"`, 300 s, TRACKING
  reached: 24 reports (A7 round 2) → **5, none in the scan code** (left: rfsim teardown + SIGINT handler); the
  second footprint decision (~130 s) went through the deferred path with 2 consumers.
  `[SIM VERIFIED]` gate `GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 rfsim_regress.sh 1` (default cfg, auto `1:8:-1`): PASS,
  crc 96.85 %, drop_full 1.26 %, CONVERGED 2. sens6 frozen-tag diff empty.
  Evidence + exact TSAN build/run recipe: `tests/passive_rx/cloud_run_2026-10-01/a7_followup_races/README.md`.
- **Not done / open**: no DGX or 273-PRB run with N > 1 (A8); gtest not re-run under full TSAN for the new test;
  `ISAC_PDCCH_TIMING` diagnostic counters (`g_pdtim_*`, off by default) remain unsynchronised; other `g_cfg` writers
  on the MAC thread (CSS0 autoconf geometry fields) were not in scope beyond the SS gate.

**If the ctest suite passes (except the known failure), the shuffle seeds pass, and the phy-test smoke converges with
drop_full ≤ ~1 %, the software build is known-good enough to begin X410/OTA validation.** If drop_full or
`over_slot` is much worse than on sens6, the ARM CPU budget is the first problem to solve (§5.1).

### 14.6 Cloud x86 rfsim results (2026-10-01) — NOT the DGX

**Do not compare these numbers with §14.1.** Host: cloud container, Intel Xeon @2.80 GHz, 4 cores, no GPU, no IPv6 (rfsim
via `V4SHIM`), scan thread **unpinned** (`1:8:-1`, the frozen conf pins core 5 which does not exist), 106 PRB fully
agnostic bed (`rfsim_regress.sh`), 150 s per arm, binaries built with the code of the commit given. Every run has
CONVERGED 2/2. Cloud gate (re-baselined, §24 K31): `GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5` (DGX gate 98 / 1 — HEAD misses it
on 4 cores; the 4-core budget and the unpinned scan thread are confounded, `[HYPOTHESIS]`).

**Final 3-run gate (code `d6cb580ce5`):** PASS 3/3 — crc **95.33 / 96.18 / 95.68 %**, drop_full 1.50 / 0.87 / 1.46 %, ttc 3.78 / 4.10 / 4.18 s, CONVERGED 2/2 each, CPU ~200 %, RSS 0.89 GB; within the HEAD baseline spread (crc 94.93–96.89, drop 0.77–1.55). `[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, d6cb580ce5]` Evidence: `tests/passive_rx/cloud_run_2026-10-01/final_gates/regress_scores.jsonl`.

| Item | Commit | crc % | drop_full % | ttc s | Other | Label / evidence |
|---|---|---|---|---|---|---|
| **HEAD baseline** r1 / r2 / r3 (unmodified code) | `be2e7fa4b6` | 94.93 / 96.89 / 95.19 | 1.17 / 1.55 / 0.77 | 4.2 / 2.9 / 4.5 | sync 8.0–8.7 s, CPU ~200 %, RSS 0.89 GB, bank len 46 | `[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, be2e7fa4b6]` `a1_baseline/` |
| A2 ISAC_METRICS on | `4cdb890759` | 96.75 | 1.10 | — | 7 `ISAC_METRICS` lines = 7 JSONL lines; JSON `pdschq_crc_ok` == text `crc_ok` (15526) | `[SIM VERIFIED, cloud x86 …, 4cdb890759]` `a2_metrics/` |
| A3 obs ON (`ISAC_OBS_PATH`) | `10cc6f0058` | 94.81 | 1.79 | — | n_dl 14852 <= pdschq_decoded 14913 (layout-probe share 0.41 %); DL crc_ok 14139 == `pdschq_crc_ok`; obs_dropped 0 | `[SIM VERIFIED, cloud x86 …, 10cc6f0058]` `a3_obs/` |
| A3 obs OFF (control) | `10cc6f0058` | 94.87 | 1.29 | — | CRC with/without obs differs 0.06 pt | same |
| A3 **throttled writer** (`ISAC_OBS_TEST_WRITER_PAUSE_MS=60000`, ring 256) | `10cc6f0058` | 95.38 | 1.64 | — | obs_pushed 13129, **obs_dropped 3591**; decode CRC unaffected → writer never blocks decode | same |
| A3 fix round (obs on) | `385e02b9cf` | 95.94 | 1.17 | — | check_obs PASS | `a3_obs/score_fix.txt` |
| A4 campaign smoke (`campaign.py`, run 002_base) | `2547ff90aa` | 95.45 | 1.39 | 4.2 | verdict VALID; metrics 7 lines (DL_CONVERGED), obs 19119 lines, CPU 202 %, RSS 0.90 GB; run 001 killed externally → classified INTERRUPTED (stale `running`) | `[SIM VERIFIED, cloud x86 …, 2547ff90aa]` `a4_campaign_smoke/` |
| A5 `/health` on 002_base | `0d04761969` | window 100 | 0.75 | — | acq DL_CONVERGED, grants/s **161** (cloud rfsim rate; not the DGX figure), top RNTI 4660, screenshot | `[SIM VERIFIED, cloud x86 …, 0d04761969]` `a5_dashboard/` |
| A7 gate, default config | `b6e5fb27ac` | 96.65 | 0.75 | 2.2 | PASS | `[SIM VERIFIED, cloud x86 …, b6e5fb27ac]` |
| A11 `ISAC_SCAN_SCRATCH_MB` unset | `843e5cff49` | 95.4 | 0.71 | 3.8 | PASS; log "Scan scratch budget 512 MB … batches of 1" | `[SIM VERIFIED, cloud x86 …, 843e5cff49]` `a11_scan_scratch/` |
| A11 `ISAC_SCAN_SCRATCH_MB=2048` | `843e5cff49` | 96.33 | 1.10 | 4.6 | PASS; "budget 2048 MB"; rfsim scans 1 GSCN so batch = 1 in both arms (behaviour identical; timing effect DGX-only) | same |

**A7 A/B — blind-PDCCH scan consumers, 106 PRB, alternating S/M x3, unpinned** `[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, b6e5fb27ac]` (`a7_concurrency/`).
S = `scan_thread "1:8:-1"`, M = `"2:16:-1"`. acc/occ = `pdcch_accepts/pdcch_occasions` in tracking. CPU = `thrprof.sh`, 20 s at t+75 s, 100 % = 1 core.

| Arm | crc % | drop_full % | ttc s | acc/occ | scanq max_lag | passivePdcch CPU % | UEthread_0 % |
|---|---|---|---|---|---|---|---|
| S x3 | 95.04 / 95.38 / 96.37 (mean 95.60) | 1.63 / 1.17 / 1.57 (1.46) | 4.8 / 4.3 / 2.5 | .475 / .458 / .449 | 8 | 71.6 / 71.0 / 72.5 (one thread) | 103.8 |
| M x3 | 97.78 / 94.76 / 96.65 (96.40) | 0.64 / 0.56 / 0.47 (0.55) | 2.6 / 4.3 / 2.6 | .470 / .443 / .446 | 16 | 80.6 / 73.0 / 84.2 (~40 + 40) | 105.1 |
| control `2:8:-1` | 94.89 | 1.02 | 4.8 | .462 | 8 | 82.5 | — |
| control `1:16:-1` | 96.71 | 0.72 | 3.1 | .480 | 16 | 72.3 | — |

Reading: acc/occ and CRC within run-to-run spread, drop_full lower with M; total scan CPU +10 % (79 vs 72 %); `max_lag`
follows the queue **depth** (controls), not the consumer count. **No speed-up is claimed** (on 4 cores one consumer already
keeps up). TSAN oracle on the real occasion path (receiver instrumented, 300 s): consumer<->consumer races 37 -> 0 (full
build, discovery stage) and 26 -> 0 (blind-PDCCH sources, TRACKING stage); remaining reports are K30.

**Not measured here (DGX-only):** 273 PRB A/B `"1:8:6"` vs `"2:16:6"`, core-map effect, scan-scratch timing, aarch64 anything.
UL decode is not exercised by phy-test (K28).

---

### 14.7 DGX rfsim on the merged tree (2026-10-01, `7f2fb28acf`) — `[SIM VERIFIED, DGX aarch64]`

| Arm (150 s, 106 PRB, fully agnostic, DGX gate 98 % / 1 %) | CONVERGED | ttc (s) | TB CRC | scanq drop_full | CPU |
|---|---|---|---|---|---|
| base_r1 (`rfsim_regress.sh`, with `ISAC_METRICS_PATH` + `ISAC_OBS_PATH`) | 2 | 0.81 | 99.05 % | 0.054 % | 298 % |
| base_r2 | 2 | 1.61 | 98.59 % | 0.036 % | 300 % |
| scan2 (`SCANTHREAD="2:16:-1"`, two blind-PDCCH consumers, unpinned) | 2 | 0.93 | 99.09 % | 0.195 % | 288 % |

Gate PASS. Same regime as §14.1 (HEAD before the merge: 98.6–99.1 %) — no regression from Track A + follow-up on ARM.
Two consumers work on ARM (first DGX run of A7); 273-PRB pinned A/B still open (§25).
**Observation ≡ metrics:** cutting each run's obs records at the time of its last `ISAC_METRICS` snapshot, DL records =
`obs_written` (55 025 / 55 022) and CRC-OK records = `pdschq_crc_ok` (54 556 / 54 330) exactly; obs ≤ decoded (layout
probes excluded); `obs_dropped` 0. (`check_obs.py` reported FAIL only because both runs appended to one file and the
final records come after the last 20 s snapshot — not a defect.)

### 14.8 DGX rfsim on `td/convergence-levers` (2026-10-01/02, 1 RX unless stated) — phy-test bed carries NO SIB1

All rows: phy-test rfsim, DGX `spark-74c3`; "idle" = 1-min load < 2 and no `nr_td_sim` process. Each row names its
bed and commit; rows are never pooled. The 106-PRB bed (1 RX, RNTI 0x1234) has two contexts (tda0 S1 L13 k0 0, tda2
S1 L5) and no SIB1, so TDD / DCI-adjacency exclusions are **inert** (`tdd_known=0`, `adj_rows=0`) and BC12a reads
`sib1_row=none deftab=mismatch`.

| What | Result | Label |
|---|---|---|
| F1 chest cache (K32) | probe-equivalence arms 1125/1195 → 0/1160 (cache hit), chest rows 167/167 → 0/340 (4 RX rank 1, branch 2); legacy gate PASS 99.06/98.64 % | `[MEASURED, DGX rfsim 106 PRB]`, `f1_chest_cache/` (details K32) |
| F2 stale credit (K33) | legacy gate PASS ×2 (98.58/98.63 %); `pdschq_stale_after_decode` 2 in one later run | `[MEASURED, DGX rfsim 106 PRB 1 RX]`, `f2_stale_credit/` |
| BC7 K39 fix vs `ISAC_TD_K0_ORACLE_LEGACY=1` | legacy ttc 0.87/0.80 s, CRC 99.05/99.02 %; fix ttc 2.54/1.96 s, CRC 97.33/97.64 % (same winner; fails the **legacy** 98 % gate) | `[MEASURED, DGX rfsim 106 PRB 1 RX, @b35c293c55]`, `bc7_k39/` |
| BC9 idle A/B, 5 on / 5 off (`ISAC_TD_DCI_ADJ=0`) interleaved | CRC median 97.00 vs 97.27 % (p = 0.79); tda0 ttc 3.0 vs 2.7 s; tda2 22.0 vs 21.8 s; tda0 winner trials 141 vs 125 (p = 0.47); certified passes all on k0 = 0, wrong-k0 alarms 0 → BC9 neutral without SIB1; no run reaches 98 % in either arm | `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @0232f351c3 (code 1a6be6155b)]`, `bc9_dci_adjacency/idle_ab_summary.md` |
| BC9 loaded runs (contaminated, kept for the record) | gate 93.4–96.0 % on vs 96.2–96.7 % kill switch with 6–8 simulator processes running — **invalid as evidence** (scheduling error) | `[MEASURED, DGX rfsim 106 PRB 1 RX, host LOADED]` |
| BC7b K42 A/B, pre (`8af53c3a40`) vs fix (`7b5c51b115`), 4 + 4 interleaved | CRC median 97.14 vs 96.95 % (p = 0.49); tda0 ttc 2.95 vs 3.21 s (p = 0.20); F1 lastset inert on this bed (one last symbol per mask). Re-scored with the postconv gate: 13/13 BC7b runs PASS (tightest: crc_floor 94.63 vs 94.5) | `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]`, `bc7b_k42/` |
| **Regression gate (postconv) after the K42 follow-up** | **PASS ×2**: postconv CRC 100.00/100.00 (43 479 / 42 521 grants), ttc0 4.57/3.57 s, ttc2 20.73/22.54 s, floor 96.11/96.65, drop_full 0.059/0.043 %, reopens 0 | `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @4db43db6f5]`, `final_gate_2026-10-02/gate.txt` |
| BC9d gate | PASS ×2: postconv 100.00/100.00, ttc0 2.16/1.98 s, ttc2 18.8/22.8 s, floor 97.65/97.63; 56–58 k confirms, missed_lookup 0 | `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @852b9bb86f]`, `bc9d_confirmed/` |
| BC12a gate (worktree build of `072d11abfb`) | PASS ×2: postconv 100.00, ttc0 2.13/2.74 s, ttc2 22.3/21.2 s, floor 97.57 (r1) | `[MEASURED, DGX rfsim 106 PRB 1 RX, idle, @072d11abfb]` (task report; raw evidence was in `/tmp`, not kept) |
| **Regression gate on the merged tree (BC9d + BC12a)** | **PASS ×2**: postconv 100.00/100.00 (41 872 / 42 406), ttc0 2.98/2.01 s, ttc2 20.59/20.35 s, floor 97.05/97.67, drop_full 0.044/0.076 % | `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @ca1cbc5470]`, `final_gate_2026-10-02/` |
| Rank 4 (`pin49r4`, 273 PRB, 4 RX) after each runtime change | 100 % every time: BC7 3036/3036, BC9 idle 3005/3005, BC7b 3011/3011, BC9d 3007/3007. **`[STALE]` as a grant-count reference (2026-10-04):** the same bed now decodes only ~150–220 grants per 150 s and 831–931 per 420 s, **on main too** (CRC still 100 %); unexplained, possibly host-related (K49) | `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` |
| `ldpc_zero_tb` | ~260–465 per 150 s run since K39 (k0 = 1 hypotheses decoding UL slot 8); not excludable without SIB1 TDD | `[MEASURED, DGX rfsim 106 PRB 1 RX]` |

### 14.9 Technique D simulator results (`nr_td_sim`) — model results, never merged with §14.8

Labels `[SIMULATED, DGX host, nr_td_sim @<commit>]`, seed 1 unless noted; "cold" = RNTIs 1–2 of an acquisition,
"steady" = RNTIs 3–4; oracle 1 = today's runtime oracles, oracle 0 = blind (NSA-like / oracle failure). Monte-Carlo
resolution ~1.5e-3 per RNTI at 2000 RNTIs; the 1e-6 claims are analytical.

| Study | Result | Commit / evidence |
|---|---|---|
| Task 5 baseline (v1, levers off) | oracle 1: 4 RX median 1.00 s, 1 RX 1.59 s; oracle 0: 4 RX cold median 356.6 s / steady 10.6 s; 1 RX cold 716.6 / steady 25.6 s; wrong 0 | `@00dd79eed4`, `baseline_2026-10-01.txt` |
| Task 6 P1 ablation (gate, K = 3, ordering, field book as ordering) | **P1 inert by construction** (every alive hypothesis needs ~r trials regardless of order): oracle 0 cold median 4 RX 356.6 vs 358.6 s (base vs all P1), 1 RX 716.6 vs 512.3 s; only the gate helps at 1 RX (−20 % cold mean, skipping rank-2 trials); field book as ordering without pruning regresses steady 10.6 → 358.6 s. Chosen: GATE = 1, K = 1, W = 0, FIELDBOOK = 0, P2 = 0 (simulator/R2 defaults, not runtime) | `@00dd79eed4` binary, `results_2026-10-01_p1/` |
| Task 7 P2 (probe FAILs as evidence) | **FAILED → P2 off**: oracle 1 1.20 → 0.81 s but oracle 0 wrong 0 → 932 (probe slots go to the first uncleared survivors in score order, truth probed ~30× its share → deflated); sound redesign would be a separate one-sided CB0 elimination channel, still blocked at Nl > 1 by K38 | `@02524c44ff`, `results_2026-10-01_p2/`, levers spec §5.4 |
| BC0 realism | oracle-miss 0.3: 4 RX cold mean 128.7 s; oracle-wrong 0.05: 234 undecidable / 425 wrong-oracle RNTIs, 0 wrong; trap 0.01 + crc-false 2^-24: 0 wrong | `@270dd00726`, `baseline_bc0` |
| BC1 lever E | **slower blind** (te 0.9: cold mean 422 → 567 s; te 0.964: 326 → 376 s), 0 wrong; default off, not recommended (selection bias) | `@8eb5f9ff4f` |
| BC5 field book (fb2 = reversible pruning) | oracle 0, 4 RX: steady 0.3 s vs prior 15.6 s vs ordering-only 430 s; forced wrong TDRA: recovery 171 k grants / 2.0 RNTIs | `@9af3c6a5a9` |
| BC2b levers P/C | retx-only trap: cold mean 335.9 → P 4.7 / C 3.7 / P+C 3.4 s, but stress p_f 1e-3 gave 47 wrong (C) / 53 wrong pins (P) vs bounds 1.4/2.2 (exploit bias) and the any-grant k0 trap gives wrong winners → fix A (explore-only evidence) + B (k0-sibling guard): 0 wrong / 0 pins everywhere, fast path then never fires under the k0 trap | `@7e8402c037` → `@932532810d`, `@77775006f2` |
| BC8 v2 (slot model) | fixed oracle, default traffic: 0 wrong, 0 undecidable (decided 14–49 s); legacy (K39) oracle: k0 = 1 truths mostly undecidable (1058–1072), wrong ≤ 14 (339–438 at adjacency 1); other-UE DM-RS → K41; with union protections → thrash K42 | `@b761021bb1`, `@f59112743e`, `## BC8 v2` |
| BC7b K42 | k0 = 1 undecidable 1072/1072 → 56–133 (adjacency 0), 0 at default traffic; wrong 0; k0 = 1 decides slowly (mean 413–933 s) | `@44a9391975`, `## BC7b` |
| BC9 sim (certified evidence + TDD) | cold mean base → C+P certified: rho 0.9 683 → 656 s (4 RX), 1052 → 1029 s (1 RX), no lever events (sibling guard blocks); + TDD DDDSU 522 → 310 / 870 → 549 s (TDD exclusion alone −34..−37 %); rho 0 373 → 157 / 673 → 258 s; wrong 0, pins 0 | `@fcbaeb9478`, `## BC9 sim` |
| BC9d confirmed-DCI rule | dci-false 1e-2, DDDSU: old rule prunes the truth on 1096/1200 RNTIs (48 wrong, 1048 undecidable) → confirmed rule 0/0; cost +0.5..0.9 % mean | `@852b9bb86f`, `## BC9d` |
| **BC6 reduced gate** (§8 decision of the blind addendum) | oracle 0 cold median 4 RX: TDD none 301.3 / 301.3 / 301.1 / 301.1 s (prior / fb2 / cp / fb2_cp), DDDSU 179.4 / 179.4 / 176.2 / 176.2 s — **target ≤ 30 s (4 RX) / ≤ 90 s (1 RX) missed**; fb2 steady 0.33 vs 16 s; non-injected arms wrong 0 / pins 0 (32 cells); **levers C/P inert** under the default guard (crc_accepts = geom_pins = 0); **fb2_inject FAILS recovery** (wrong 840–1980/2000, recovery never in 404–497/500 acquisitions → K44); guard-off cp_nog DDDSU wrong_pins 1 = simulator bookkeeping artefact (investigation A3: the real pin was correct). Decision: fb2 / C+P / fb2+C+P **NOT RECOMMENDED** for runtime enablement. **INVALID, do not quote:** the oracle-1 + DDDSU row "1072/2000 undecidable" (investigation A1: simulator artefact, the sim re-adds excluded k0 ≥ 2 probe layers and wipes per DCI; the runtime merges per row — runtime-API replay 3 wipes / 3000 DCIs; scratch fix 0/200 undecidable) | `@c826d0e4ae`, `results_2026-10-02_bc/`, `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/task-BC6-investigation.md` |
| **BC6b** (sim fixes A1/A3, field-book k0 fix, re-run; **landed**, `2588a83cc9` code, `ee5a805423` evidence; review APPROVED, minor items only) | Field book never prunes k0 (TDRA prune matches (S, L, mapping) only; k0 stays in votes and hints; k0-part tracker relearns k0 in place after `withdraw_rntis` distinct RNTIs agree). Simulator fixes: per-row merged TDD exclusion via `nr_pdsch_config_sweep_add_k0_layer_excl` (simulator-only public helper) and pin counting. Re-run `[SIMULATED, DGX host, nr_td_sim @2588a83cc9]`: oracle-1 DDDSU undecidable 1072 → 0, cold median 63 s at 4 RX and 90 s at 1 RX; fb2_inject wrong 840–1980 → 0, recovery-never 404–497 → 0 of 500 at 2.00 RNTIs; cp_nog wrong_pins → 0; 0 wrong and 0 wrong_pins in all 52 cells; fb2 steady 0.33 s → 0.9 s (oracle 0) / 2.8 s (oracle 1) at 4 RX TDD none; **blind cold target still missed: 301 s / 435 s**. Decision: **fb2 CONDITIONALLY RECOMMENDED** for runtime wiring (plan R2, operator decision, k0 rule kept); the blind cold target is not met and fb2 does not help the first RNTIs; C/P and fb2+C/P NOT recommended (inert under the default guard; BC9c open). Supersedes the BC6 row above (its fb2 NOT RECOMMENDED and the INVALID cells) | `@2588a83cc9`/`ee5a805423`, `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/task-BC6b-report.md` |

**CB0 elimination channel, simulator (2026-10-03/04).** Model results; never pooled with §14.10. CB0 model = "passes
iff the computation is right and SNR ≥ requirement − margin" (no CB-size, chest or K38 effects).

| Study | Result | Commit / evidence |
|---|---|---|
| ELIM round 0 (superseded by fix round 1) | `[SIMULATED, DGX host (shared), nr_td_sim @5ffe81aab8]` first-RNTI median blind 301 → 3.15 s (no TDD), 179 → 3.75 s (DDDSU) at 4 RX; with oracles ~0.7 s; 0 wrong in 73 cells; ~550–640 CB0 per grant blind. Superseded: no premise check, per-context decoder pin, no trap-family exemption | `599fa8cad3`, `results_2026-10-03_elim/` |
| **ELIM fix round 1 (current rules), 4 RX only** | `[SIMULATED, DGX host (shared, load 6–9), nr_td_sim @2c51f9c4e4]`, seed 1, acq 500 × 4 RNTIs, oracle 0, field book = prior: **blind first RNTI median 301 → 5.9 s (p95 28 s) with no TDD; 179 → 5.6 s with DDDSU** (rho 0.5); rho 0.9: 545 → 8.7 s / 240 → 6.2 s; **later RNTIs ~2–2.6 s** (2.61 s no TDD, 1.99 s DDDSU at rho 0.5); oracle 1 first 2.2–7.3 s. **Hash subset** (`--cb0-subset B`, rho 0.5): **B = 128 → 8.3 s, B = 64 → 12.9 s** (GPU load ÷4.5 / ÷8). CB0 per admissible grant ~390–440 blind, 60–100 with oracles. **wrong 0, wrong_pins 0, undecidable 0, truth eliminated 0, premise alarms 0 in all 78 cells.** Trap-family exemption costs 3.5 → 5.9 s (rho 0.5). Discriminating premise arm (near-perfect twin, CB0 margin −6 dB, TB-only HARQ gain): **31/200 wrong without the premise check, 0 with it** (all 200 fall back to TB-only). CUDA CB0 + CPU fallback at grant 200 = no-fallback cell (per-batch dominance). Stress crc-false 1e-3: 0 wrong (bound 2e-3). The BC6 blind target (≤ 30 s at 4 RX) is met in the model | `24e090ac1c`, `results_2026-10-04_elim_r1/` |

### 14.10 4-RX rfsim and GPU measurements of the acceleration round (2026-10-03/04, DGX) — never pooled with §14.8/§14.9

Bed: phy-test rfsim **106 PRB, receiver `--ue-nb-ant-rx 4`**, gNB `-m 9 -l 1` (rank 1), 420 s arms, exclusive
measurement lock per run, interleaved arms; ttc = CONVERGED − first sighting of the RNTI. Two contexts (tda0 S1 L13
0x804 k0 0, tda2 S1 L5 0x4). **No SIB1 on this bed** (TDD/adjacency exclusions inert). All arms: same winners as
every earlier bed record (not re-checked against the gNB config file), 0 reopens.

| Arm | ttc tda0 (s) | ttc tda2 (s) | overall CRC | postconv CRC | drop_full % | Label |
|---|---|---|---|---|---|---|
| **main** `8dbc76c0b7`, all off (r1/r2) | **13.9 / 14.4** | **86.9 / 78.6** | 94.79 / 94.62 | 100.00 | 1.20 / 0.79 | `[MEASURED, DGX rfsim 106 PRB 4 RX, load < 4]` (CB0WIRE r2) |
| **levers, all flags OFF** (branch r1/r2 + round-1 r1/r2) | **106.6 / 55.9 / 104.2 / 80.4** | **140.0 / 143.1 / 138.7 / 141.8** | 69.5–82.3 | 100.00 | 0.49–1.62 | same |
| pre-acceleration levers `5cc9d518e9` (r1/r2) | 87.2 / 109.4 | 143.7 / 139.8 | 72.1 / 69.2 | 100.00 | 0.87 / 1.37 | same |
| levers OFF + `ISAC_TD_K0_ORACLE_LEGACY=1` | **15.8** | 81.3 | 94.33 | 100.00 | 0.99 | same |
| **CPU ON** (GW + ELIM, CPU backend, persistent pool `993f78aa71`; r1/r2/r3) | **19.9 / 15.1 / 23.9** | **42.4 / 43.0 / 42.2** | 95.4–96.4 | 100.00 | 1.08 / 0.85 / 1.62 | same |
| CPU ON round 1 (`312047638b` / `2c4be2ec6f`, no pool) | 24.0 / 26.0 / 16.4 | 51.5 / 46.0 / 48.2 | 94.2–96.1 | 100.00 | 1.20 / 0.92 / 1.71 | `[MEASURED, …, load 0.26–3.2]` (CB0WIRE r1) |
| **GPU ON** (GW + ELIM, `ISAC_TD_CB0_BACKEND=gpu`, `@003b8c3f93` tree, **n = 1**) | **14.4** | **47.4** | 96.98 | 100.00 (12 249) | 1.50 | `[MEASURED, DGX rfsim 106 PRB 4 RX, host LOADED: load 7.5 at start]` (CB0GPU integration) |

Summary for the operator's table: **main 14 / 79–87 s; levers OFF 56–109 / 139–143 s; CPU ON 15–24 / 42–43 s; GPU ON
(n = 1, loaded host) 14.4 / 47.4 s.** Postconv CRC 100 % in every arm; **0 premise alarms and 0 wrong winners** in every
ON arm. fb2 was OFF in all these arms.

Readings:
- **K39's 4-RX cost:** levers OFF is 4–7× slower on tda0 than main; the pre-acceleration state `5cc9d518e9` is equally
  slow, so the acceleration merges are innocent; `ISAC_TD_K0_ORACLE_LEGACY=1` alone restores main (tda0 15.8 s,
  `ldpc_zero_tb` 0, tda0 winner trials 94 vs 190–222). At 1 RX the same change cost only 0.8 → 2.5 s. K39 stays the
  default because it is a correctness change (K39).
- **CB0 ON** recovers it: tda0 ≈ main, tda2 ~2× faster than main, ≥ 2× faster than levers OFF on both.
- **CB0 metrics (CPU ON r1–r3, round 1):** 747–1114 grants, 711–987 admissible, 2384–2981 items, **40 eliminations
  per run (20 per context), 0 premise alarms**, budget skips 9–106 (0–16 with the pool), not-testable 2113–3052
  (other k0), backend cpu only. **GPU arm:** 663 grants / 660 batches / 659 admissible / 2108 items, 28 eliminations,
  0 alarms, **backend gpu 660 / cpu 0**, `td_cb0_gpu` errors/timeouts/bypassed/sticky/trips/failed 0, mode 1 (unified),
  `ldpc_tb_cpu` 14 944 / `ldpc_tb_cuda` 0 (CPU TB while acquiring ⇒ CUDA-CB0-over-CPU-TB dominance), G1 TB pool unused.
- **Mean CB0 items per grant ≈ 3** (2.6–3.2 CPU, 3.2 GPU) instead of B = 96: only hypotheses with the job's k0 are
  testable on one GrantWork, and ≤ 5 geometries fit its 16 signatures. **This k0 + 5-geometry limit is the next speed
  lever** (K47). Per-item wall: CPU 8.5–11.9 ms, GPU ~1.1 ms (both include lazy signature computes; batches are tiny,
  so both run latency-bound).
- **CPU cost:** ON uses the same total CPU as OFF (user+sys 801–878 s vs 804–864 s per 420 s): the CB0 work is offset
  by fewer failed search decodes (`ldpc_seg_fail` 383–623 ON vs 3149–3780 OFF).
- **drop_full** > 1 % in some ON arms is the PDCCH scan queue, not CB0: main OFF is 0.79–1.20 %, the drops keep growing
  after both contexts converge when CB0 is idle, and the PDSCH queue dropped nothing (K45).
- **First C-RNTI at ~95–117 s in every arm, main included** — a property of the 4-RX bed (sync/acquisition).

Other measurements of the round (each with its own bed; never pooled):

| What | Result | Label |
|---|---|---|
| K38 fix, rank-4 probe equivalence | `ISAC_PROBE_ALL=1`, old statistic: probe k = 2 vs full k = 4 on every sample (CB0 LLRs 4×, 1062–1114 of 9664 clipped), `PROBE_EQUIV` 5/5 mismatched; fixed statistic 0/5 (0/48 320 LLRs); committed code `ISAC_TD_PROBE_EQUIV_CHECK=2`: PROBE_EQUIV 0/3, _CHEST 0/3, _HIT 0/3; TB CRC 100 % (149/149, 151/151). Samples small (bed decoded ~150–220 grants per run on a loaded host) | `[MEASURED, DGX rfsim 273 PRB rank-4 pin49r4 4 RX, host shared]`, `k38_llr_norm/` |
| GrantWork ≡ today (round 0) | `ISAC_TD_GW_CHECK=5`, 654 sampled main decodes, Nl = 1: TB LLR mismatches 0 (READY and no-GW), TB CRC 0, CB0 input/LLR 0; 6 CB0-verdict differences = `sibling_abort` | `[MEASURED, DGX rfsim 106 PRB 1 RX, host shared]` |
| GrantWork after fix round 1 (C1: unnormalised shared LLRs) | `PROBE_EQUIV_GW` 0/28 (LLR, input, CPU CRC), `PROBE_EQUIV`/`_HIT`/`_CHEST` 0. **`GW_EQUIV` after C1: no sample** (arm stayed in the layout-probe phase); **Nl > 1 not run** | `[MEASURED, DGX rfsim 106 PRB 1 RX, quiet, @480fbdc070]` |
| GrantWork cost profile | shared per grant 1558 µs (FEP 806, chest 351, demod+norm 357); per hypothesis: READY ≈ 0, CB0 de-match 77 µs, CB0 CPU LDPC 990 µs (single thread, 8 it) | `[MEASURED, DGX rfsim 106 PRB 1 RX, host shared]` |
| CB0 batch, CPU vs G1 TB entry | CPU 8 threads 78–161 µs/CB0 (14–100 grants/s at N 64–600); CUDA via G1's TB entry 305–385 µs (max_it 8), latency/launch-bound (GPU busy 1–43 %), N = 600 hits the 200 ms timeout → fallback. K17's 6.8 µs/probe was FEP + chest + LLR, **no LDPC** | `[MEASURED, DGX GB10, @135b6658b7/150bab84b0]` |
| GPU CB0 entry, 4-RX rank-4 grant (C_true 21 / 55) | N = 600 wrong-heavy: **77–81 µs/CB0** vs CPU-8 83–112 µs (**1.05–1.4×**); truth-only 21–25 vs 43–65 µs (2–3×); ~4× faster than round 2; memory-bandwidth bound (GPU busy 66–95 %, floor ~35–40 µs/CB0 with G1's algorithm on GB10's 273 GB/s); unified vs forced explicit within ~5 %; cap 12: 58 µs (1.33×); verdict mismatch GPU vs CPU 0, failed batches 0. The 5× target is **not** reached | `[MEASURED, DGX GB10, @9366e52544, load 2.6–4.2]`, `task-CB0GPU-bench.csv` |
| GPU CB0 entry paired dominance | see §3.3.1 (0 violations at caps 16/14/12 in 104 000 codewords; 1 at 10; 836 at 8) | `[MEASURED, DGX GB10, @5a1a278452]`, `task-CB0GPU-paired.csv` |
| sm_89 build | `-DLDPC_CUDA_ARCH="89;121"` builds `ldpc_cuda`, `td_cb0_gpu`, `pdsch_gpu`, `pdcch_gpu`, `polar_sc_cuda`, `test_ldpc_cb0_cuda`; `cuobjdump` shows sm_89 + sm_121 cubins. **Not compiled with nvcc 12.4, not run on sens6** | `[MEASURED, DGX build]` |
| R2fb gates | flag OFF and fb2 arms FAILED on a shared host (load 3–5; other branches' baselines degraded the same way) — invalid as evidence; **no idle-host fb2 gate exists** | `[MEASURED, DGX rfsim 106 PRB 1 RX, host LOADED]` |


**§14.10 addendum: full default combination, 4-RX gate (2026-10-04).** `[MEASURED, DGX rfsim 106 PRB 4 RX, 420 s, flock -x, idle host]`. All defaults ON (GrantWork + CB0 elimination + fb2 + backend auto + CPU TB while acquiring):

| Binary | Arm | ttc tda0 / tda2 | overall CRC | postconv CRC (decodes) | drop_full | gate |
|---|---|---|---|---|---|---|
| `f3c9e585de` (pre-fix) | ON r1 | 22.0 / 42.1 s | 95.6 % | 100 % (11983) | 0.91 % | PASS |
| `f3c9e585de` (pre-fix) | ON r2 | 12.8 / 49.5 s | 96.6 % | 100 % (11762) | 0.59 % | PASS |
| `4f0158f046` (final) | ON r1 | 22.2 / 40.3 s | 95.6 % | 100 % (13020) | 0.44 % | PASS |
| `4f0158f046` (final) | OFF (`ISAC_TD_GRANTWORK=0 ISAC_TD_CB0_ELIM=0 ISAC_TD_FIELDBOOK=0`) | 94.5 / 137.0 s | 73.1 % | 100 % (8464) | 1.64 % | FAIL (ttc, floor: K39 cost, expected) |
| `4f0158f046` (final) | ON r2 | 24.5 / 39.4 s | 95.2 % | 100 % (12698) | 0.86 % | PASS |

Same winners in every arm (tda0 S1 L13 mask 0x804 k0=0; tda2 S1 L5 mask 0x4), 0 reopens, 0 premise alarms. The 4-RX gate is calibrated for the ON defaults: an all-OFF build fails it by design (K39 at 4 RX). This closes the "full default combination never measured" item of K50 for the rfsim bed; Nl > 1 runtime admissibility and sens6 remain open.


## 15. Current OTA status (latest campaign only)

### 15.0 Site change (2026-09-30) — read first

- **Update 2026-10-02:** an X410 OTA session is scheduled from **2026-10-03** (operator); it will be the first air
  run of the `td/convergence-levers` receiver changes (K39–K43, BC12a census) and the first SIB1 cell for the BC9
  exclusions. Checklist: §25.
- **Update 2026-10-04:** the 2026-10-03 OTA did **not** run (X410 troubles); postponed to **2026-10-05/06** (operator).
  It will also be the first air run of the acceleration round with the new defaults (CB0 elimination, GrantWork, fb2,
  GPU backend), 4 RX. Checklist additions: §25 (2026-10-04 block).
- **No OTA has run on the DGX yet** (no X410 connected on 2026-09-30). The newest OTA evidence is still the
  2026-09-25 sens6 campaign below (`aa872eba3b`, Swiss site) — `HISTORICAL / NOT CURRENT` for the new site.
- **The lab testbed is not available** (operator, 2026-09-30): the sens4 lab gNB (PCI 2, PLMN 001/06, 273 PRB @
  3450 MHz) cannot be used, so §15.2 cannot be reproduced and there is **no gNB ground truth** (no gNB log, no C-RNTI
  list, no scheduler census) for the next OTA work unless a controllable gNB is found at the new site.
- **All next OTA tests are on a new cell observed from the DEIB building, Politecnico di Milano campus (Milan).**
  Nothing about that cell is known: operator, PCI, band (n78 is the likely FR1 TDD band in Italy but is **not**
  established), carrier centre, bandwidth, SSB position, SA vs NSA, TDD pattern. Treat it like any new cell:
  discover everything from the signal (§10.5 still requires `-C`/`-r`/`--ssb` or `--ue-scan-carrier` at startup).
  PLMN/operator only from a decoded SIB1 (`SIB1 CELL mcc=… mnc=…`; MCC 222 = Italy — the MNC→operator mapping is an
  external registry, not a decode).
- The Salt (PCI 64) and Swisscom (PCI 382) results are from Switzerland and **must never be combined** with Milan
  results (§27).
- Consequence for validation: on the Milan cell the gates can be scored only on **self-consistency** evidence
  (PBCH CRC, SIB1 CRC, DCI CRC + persistence, TB CRC). G7's "C-RNTI confirmed against truth" and G8's "grant census
  vs gNB log" cannot be done there; use the rfsim/OCUDU beds for truth-matched checks.

### 15.1 Identification of the latest campaign

Recency determined from raw capture directories on `sens6:/home/sens/NICOLA/captures/` (mtimes, `CMDLINE:` and
`Abrev. Hash` lines inside each `run.log`), not from summaries. The latest X410 captures of **this receiver lineage**
(`adaptive-rx-UL-DL`) were taken on **2026-09-25** (sens6 clock, UTC) with binary **`aa872eba3b`**. Later captures
(2026-09-26 `sense_085822/090304/090729`) used the **other** branch `feature/multirx-clean-adaptive` @ `173db3bf68`
(sensing-focused) — listed in §15.5 as context, not as this branch's status. **The X410 has been physically
disconnected from sens6 since then (2026-09-30: both ConnectX-5 ports "No cable", `uhd_find_devices` finds nothing).**
All code merged after `aa872eba3b` (136 commits: plan Tasks 1–16/18, all lanes, SSB/CSI-RS/NSA-MIB work) has
**never run OTA**.

Common parameters of the 2026-09-25 campaign:

| Item | Value |
|---|---|
| Host | sens6 (x86_64, Core Ultra 5 235, 14 GiB, Ubuntu 26.04, kernel 7.0.0-34-realtime) |
| X410 | product x410, serial `327C1F2`, FPGA `UC_200`, MPM 6.1 / FPGA 11.0, MCR 245.76 MHz, internal clock/time |
| UHD | 4.10.0.HEAD-0-g2af4ddb9 (host) |
| Link | data `192.168.20.2` over QSFP28 → `enp2s0f1np1` (100 GbE, MTU 9000); mgmt `192.168.1.140` via `enp128s31f6` |
| Sample rate | 122.88 MS/s per channel (both 273 and 217 PRB) |
| Band / numerology | n78, 30 kHz (µ=1) |
| Launcher | **the stale host copy `/home/sens/NICOLA/captures/run_arm.sh`** (per the same-day X410 investigation: it pinned 11 of 14 IRQs to the idle NIC port). The in-tree copy was already fixed. `[HYPOTHESIS]` this contributed to the RFSTALLs below. |
| Location | not recorded in the artifacts; the operator's earlier notes describe a fixed "known-good spot" ~120 m LOS from the Salt site (2026-09-19) — **not verified for 2026-09-25** |

### 15.2 Part A — lab cell (morning, 11:49–12:55 UTC)

| Item | Value |
|---|---|
| Cell | Lab gNB on host sens4 (srsRAN/OCUDU-family), **PLMN 001/06** decoded from SIB1 (`SIB1 CELL mcc=001 mnc=06 cell_identity=0x00066c001 tac=1`) → test PLMN, **not a commercial operator** |
| Frequency | carrier centre 3450.000 MHz (`-C 3450000000`), SSB 3408.96 MHz (subcarrier 150), Point A ARFCN 626724 (3400.86 MHz) |
| Bandwidth | 273 PRB / 100 MHz, `offsetToPointA=24` |
| PCI | 2 (SSB-derived; SIB1 consistent) |
| RX | 1 channel (ch0), `--ue-rxgain 49`, `--initial-fo -15000` (measured CFO −13.1/−13.4 kHz) |
| Conf | `agnostic_ota_noprior.conf` (fully agnostic: autoconf + autodiscover + full_auto, PDSCH mcs_table unknown, UL by search) + `ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1` |
| Command | see §10.3 (exact `CMDLINE` in `latest_ota_2026-09-25/agnostic_lab_114917/milestones.txt`) |

| Run | Verdict | What happened |
|---|---|---|
| `agnostic_lab_114917` (200 s) | VOID_DL_RATE | PCI 2 → PBCH_LOCKED → SIB1_DECODED → CELL_CONFIGURED; CSS0 autoconf; `PDCCH_SCRAMBLING_ID CONFIRMED` n_id=2; C-RNTI contexts (0x4768); **dedicated CORESET banked (`bank add … span=270 … len=47`)**; DCI 0_1 accepts 207 in the last summary line captured; CSI-RS `IDSWEEP SOLVED row1 fd4 l4 scramb_id=2 z=5.1`; DL LDPC ok = 72 total, TB rate ~1 % |
| `agnostic_lab_115315` | VOID_NO_SIB1 | nic_miss 22314 — **host packet loss**; run invalid |
| `agnostic_lab2_124600` | VOID_DL_RATE | same chain incl. bank add len=47, two C-RNTI contexts (0x4768, 0x47f7), DCI 0_1 accepts 585, LDPC ok 6 |
| `agnostic_lab2_125110` | VOID_DL_RATE | LDPC ok 1 |

Gates on the lab cell (binary `aa872eba3b`): G0 **partial** (1 of 4 runs lost packets), G1–G5A **PASS**, G6 **PASS**
(bank add; geometry len 47 = DCI 1_1 known for this cell), G7 **PASS partial** (C-RNTI accepts; DL/UL lengths 47/45 were
confirmed against the gNB log on 2026-09-17, not re-checked for this run), G8 **FAIL** (DL TB rate 0–1 %; Technique D had
only ~72 CRC-OK TBs in 200 s — below its 300-trial round-robin floor), G9+ not reached.

### 15.3 Part B — Salt commercial macro (afternoon, 14:21–15:14 UTC) — **the most recent OTA of this branch**

| Item | Value |
|---|---|
| Operator | **PLMN 228-03 decoded from SIB1** (`SIB1 CELL mcc=228 mnc=03 cell_identity=0x25aa70dae tac=546003 n_plmn=1`, run `salt_1ant_b_150954`). MCC 228 = Switzerland; MNC 03 = Salt Mobile per the public MCC/MNC registry (external mapping, not decoded). The arm name "salt" in the launcher is operator-entered metadata consistent with that. |
| Network mode | SA-style broadcast present (SIB1 decoded, CORESET#0 present). SA vs NSA for UEs not established. |
| Frequency | 3540.000 MHz carrier centre (derived from SIB1 = started), SSB at subcarrier 1182 of the started grid, SSB index 5 |
| Bandwidth | **217 PRB (80 MHz)**, `offsetToPointA=196` (SIB1) |
| PCI | 64 (co-channel PCI 244 known from 2026-09-19, HISTORICAL) |
| TDD (from SIB1) | `p1 period=5 slots dl=3+10sym ul=1+2sym`; 11 common PDSCH TDAs (S=1, L=13…3) |
| CORESET#0 | `groups=8 dur=1 bundle=6 interleaver=2 shift=64 scramb=64`, `bwp=[86..134)`, SS#0 `period=40 offset=15 dur=2` |
| RX / gain | 4 RX and 1 RX arms; `--ue-rxgain 50` (scan arms) / 43 (pinned arms); `--initial-fo -15000` (measured CFO +35 Hz residual after seed in one run) |

| Run | Settings | Verdict | Where it stopped |
|---|---|---|---|
| `salt_full4ant_142130` | 4 RX, `-r 273`, scan, RXG 50 | VOID_NO_SIB1, nic_miss 41692 | host packet loss; no SIB1 |
| `salt_full4ant2_142637` | 4 RX, `-r 217`, scan | VOID_NO_SIB1, nic_miss 52473 | host packet loss |
| `salt_full4ant4_144212` | 4 RX, 217, scan | VOID_NO_SIB1 | PCI 64 found after an early `RFSTALL … Overflow`; no SIB1 |
| `salt_full4ant5_145115` | 4 RX, 217, `--ssb 1182`, RXG 43 | VOID_NO_SIB1, nic_miss 22838, cpis 12 | PCI 64, CSS0 autoconf, timing loop active; no SIB1 |
| `salt_1ant_145919` | 1 RX, 217, `--ssb 1182`, RXG 43 | VOID_NO_SIB1 | `RFSTALL USRP_RX_START … ERROR_CODE_OVERFLOW (Out of sequence)` — never streamed |
| `salt_1ant_b_150954` | same | (no verdict file; run ended by stall) | **PCI 64 → PBCH_LOCKED → SIB1_DECODED**; SIB1 PLMN 228-03; carrier confirmed 217 PRB @ 3540 MHz; TDD derived; SIB1 prior; ~1 sensing CPI; then `RFSTALL USRP_RX_READ … ERROR_CODE_TIMEOUT` → end |
| `salt_1ant_c_151134` | same | — | `device_init failed … Someone tried to claim this device again` (launched too soon after the previous run) |

Gates on Salt (binary `aa872eba3b`): **G0 FAIL** (packet loss at 4 RX, RFSTALL at 1 RX, claim collision), G1–G4 PASS
(PCI 64, PBCH, MIB), **G5A PASS once** (1 of 7 runs, 1 RX), G6+ **not reached** — no dedicated CORESET, no C-RNTI, no
PDSCH/PUSCH decode on the commercial cell in this campaign. **The pipeline stopped at RF streaming stability (G0) after
reaching SIB1.**

### 15.4 What the DGX must reproduce first

**sens6 / Swiss-site procedure — FROZEN, still valid whenever sens6 and the lab gNB are used again:**

1. G0 on the X410 (benchmark_rate + a 600 s receiver capture without RFSTALL).
2. The lab milestones of `agnostic_lab_114917` with the current HEAD: PCI → SIB1 (PLMN 001/06) → CELL_CONFIGURED →
   `PDCCH_SCRAMBLING_ID CONFIRMED` → `multi-CORESET bank add … len=47` → C-RNTI contexts → DCI 0_1 accepts, n ≥ 5 runs.
   (Only if the lab gNB still has the same configuration — re-read it from the running gNB process on its host.)
3. Then Salt: PCI 64 → SIB1 228-03 repeatedly at 1 RX, then 4 RX, before any dedicated-path claim.

**DGX / Milan procedure (added 2026-09-30)** — the lab testbed and Swiss cells are unavailable from the DGX (§15.0):

1. G0 on the new X410 + DGX CX-7 link (benchmark_rate 300 s all channels + a 600 s receiver capture without RFSTALL).
2. **Survey the Milan cell (G1–G4)** at 1 RX: `--ue-scan-carrier` over the candidate window(s) (start with n78),
   record GSCN/SSB offset, PCI(s), CFO, PBCH `RFCENSUS pbch_ok=50 pbch_fail=0`, MIB k_SSB (≥ 24 ⇒ no CORESET#0 ⇒
   NSA/SIB1-less path, G5B). Note every co-channel PCI seen.
3. **G5A on the Milan cell** (if k_SSB < 24): SIB1 decoded repeatedly, PLMN, carrier geometry `ACQ carrier CONFIRMED`
   (relaunch once on `ISAC_ACQ_RETUNE` if the started `-C`/`-r` were wrong), TDD. `rm -rf /tmp/passive_rx` first (K11).
4. Only then the dedicated path (G6 → G8) on self-consistency evidence, n ≥ 5 runs per arm, 1 RX then 4 RX.

### 15.5 Other recent OTA context (HISTORICAL / NOT CURRENT — do not merge with the above)

| Date | Branch/binary | Cell | Result |
|---|---|---|---|
| 2026-09-26 | `feature/multirx-clean-adaptive` `173db3bf68` | lab, 273 PRB, 4 RX, RXG 50 | `sense_085822` SIB1 OK, DL LDPC 0, ANTPOW `[-0.9 -17.6 0.0 -11.3]` dB (ch1/ch3 weak); `sense_090304` RFSTALL at start; `sense_090729` SIB1 OK, DCI 0_1 accepts ~2.4 k, DL LDPC ok 4 |
| 2026-09-25 20:07–20:35 | none (benchmark only) | — | X410/host health: 4 ch × 122.88 MS/s × 300 s with **0 drops/overruns/seq errors**, NIC 99–102 °C, dnsmasq not enabled, stale launcher IRQs (§8) |
| 2026-09-23 | adaptive-rx-UL-DL (pre-merge WIP) | lab, 2 UEs + iperf | run `s3live2`: live GPU stage-1/2 discovery hand-off decoded both C-RNTIs (0x461a, 0x4626), PDSCH CRC on dedicated grants 0 %; run `s3live8`: 94.0 % of gNB-scheduled DCIs caught (scored vs the gNB SCHED debug log) |
| 2026-09-19 | adaptive-rx-UL-DL `3bde80c360` | Salt PCI 64 (217 PRB) and Swisscom PCI 382 (273 PRB, NSA) | Salt SIB1 after late-window rebase (`ISAC_TSYNC_REBASE_RATIO=3`); Swisscom PBCH 50/50 + SIB1 for 600 s runs at RXG 43; second location unusable |
| 2026-09-17 | adaptive-rx-UL-DL | lab 273 PRB, 1 RX, RXG 85 | **best blind dedicated result**: C-RNTI 0x4621 found and confirmed in the gNB log, DCI 1_1 len 47 / 0_1 len 45 locked after 37/15 occasions, scanq drop 0.97 %, 0 overflow; LDPC segment rate ~10 % (rank ≥ 2 vs 1 RX suspected) |
| 2026-09-14 | adaptive-rx-UL-DL `1c7a29132e`/`7a829f8395` | lab 273 PRB, MCS 24–25 | `NANT=1 RXG=49 TINTERP=1`: 73 % grants decoded, wide MCS-25 80 % |
| 2026-09-13 | adaptive-rx-UL-DL | lab, PCI 2, 2–6 UEs | acquisition states up to TRACKING; Technique D converged S=1 L=13 mask 0x884 table 1; six UEs at 32–44 % each (185 690 TBs); DL CRC hostage to the serving precoder (beam nulls the receiver) |
| Aug 2026 | total-passive-ue / passive-rx-only, sens3, other X410 | OCUDU/srsRAN cell 3414.99 MHz PCI 2 | 100 MHz passive sensing captures (1551 CPIs); 76.9 % catch after MPM-claim-loss fix |

---

## 16. Current project status (one-minute view)

| Pipeline block | Implementation | Offline | Simulated bed | OTA (latest evidence) | Main blocker | Next action |
|---|---|---|---|---|---|---|
| RF / X410 streaming (G0) | Done (X410 support, stall detection) | n/a | n/a | 2026-09-25 (sens6, HISTORICAL): RFSTALL / NIC loss / claim collisions ended most runs. **DGX: no X410 connected yet; CX-7 ports power down when uncabled (K24); UHD 4.11 vs X410 MPM compat unchecked** | Host drain, core map on big.LITTLE without RT (K25), claim hygiene | Cable X410 to a CX-7 port, §7 characterization, benchmark 300 s, 600 s capture without RFSTALL |
| PSS/SSS/PCI (G1–G2) | Upstream + fixes | PASS (DGX too) | PASS on DGX at 106/273 PRB 1 RX; **4-RX 273-PRB bed never synced on DGX** (§14.2) | PASS on Swiss cells (HISTORICAL); **Milan cell unknown** | Scan at 273 PRB × 4 RX: ~90 s serial on DGX | Survey the Milan cell at 1 RX (§15.4); parallel initial sync (§14.3) |
| PBCH/MIB (G3–G4) | Upstream + passive fixes | PASS | PASS | PASS | PBCH tracking failures on loaded cells (worked around) | Re-verify on new X410 |
| Tracking (timing/CFO) | Done + rebase guards + CFO trim | partial | PASS | PASS on lab; window mis-landing on 4-RX/macro (fixed by gated rebase) | Timing runaway class | Soak test (G10) |
| SIB1 (G5A) | Done (+ per-PCI cache) | PASS | PASS | PASS (lab; Salt 1/7 runs) | RF stability | Repeat on DGX |
| SIB1 absent / not decoded (G5B; NSA cells may broadcast SIB1) | PARTIAL (MIB DM-RS hand-off, CSS0-free discovery, RAR anchor) | PASS (6 tests) | **no bed** | not tested with this code | No NSA bed; carrier/TDD without SIB1 not estimated | Build a synthetic no-SIB1 fixture; OTA SIB1-less arm |
| Dedicated CORESET discovery (G6) | Done for contiguous, duration-1, non-interleaved family + AL1 cover + offline/GPU nID sweep | PASS | PASS (OAI SA, OCUDU truth match) | PASS on lab (bank add len 47) | Duration 2/3, holed bitmaps, multi-CORESET, AL1-dominated cells | Validate on Salt; implement multi-symbol ranking |
| RNTI / DCI recovery (G7) | Done (bootstrap, length sweep, layout sweeps, joint solver opt-in) | PASS | PASS (DCI 42/38 = F1AP truth) | PASS on lab (C-RNTI confirmed 2026-09-17) | False-accept calibration; multi-UE fairness | Grant census vs gNB log on DGX |
| **Reconfiguration robustness (R1–R12, R17–R18)** | `[IMPLEMENTED, NOT VALIDATED on live reconfiguration, c6fc03be7c]` 140-bit DCI payload path; epoch authority and class-aware consumers; per-RNTI re-lock; CORESET VERIFIED/STALE/REMOVED bank; UeContext; CSI-RS map signal. Sources: identity/MIB/SIB1/SI/continuity/BWP/CSI-RS/trusted per-UE change; consumers: queues/GPU work, lengths/pins/bank, Technique D and FieldBook. | `[OFFLINE VERIFIED, DGX aarch64]` replay 8/8 incl. SIB1-less, zero stale-ticket credit; epoch/UeContext/CSI-RS tests (§25/RE) | `[MEASURED, DGX rfsim 4 RX, n=2, c6fc03be7c]` flag off 2/2 and `ISAC_RECONF=1` 2/2 PASS; SIB1-less 1/2 PASS only on `drop_full` (table below) | none | no live reconfiguration, OTA flag-on, R13, or soak | Live SA bed / SIB1-less arm; then soak |
| DL reconstruction + decode (G8) | Done: Technique D, rank ≤ 4, SSB/CSI-RS rate matching, HARQ reserved-MCS | PASS (DGX too) | **DGX phy-test: 5/5 CONVERGED, 98.6–99.1 %; 256QAM 99.5 %; 273 PRB 95–96 %** (§14.1); sens6: 98.9 %, OCUDU 76 % | **FAIL/low** (lab 2026-09-25: TB rate ~1 %; best historical 73 % on 2026-09-14 older binary) | Link margin/precoder nulling, rank vs 1 RX, CPU at 4 RX | Reproduce 1-RX lab baseline, then 4-RX |
| **Technique D blind convergence + acceleration** (branch `td/convergence-levers` @`003b8c3f93`, merging into main 2026-10-04) | Runtime: K39 (k0 never pinned by DM-RS), K40 TDD direction, K42 monotone observation, K43 confirmed-DCI exclusions, BC9 DCI history + TDD/adjacency exclusions, F1/F2, BC12a census; **2026-10-03/04:** K38 fix, G1 CUDA pool safety + `decoder_used`, fb2 field book, CB0 elimination (GrantWork + CB0 batch + CPU/GPU backends) — ON by default on the merged main (§10.2). Engine/sim only: gate, ordering, `next_k`, dormant/fail-open APIs, levers E/C/P, sibling guard | only the known ARM ctest failures at `003b8c3f93` | 1 RX: postconv gate PASS ×2 at `4db43db6f5`/`ca1cbc5470` (§14.8, HISTORICAL for the new defaults). **4 RX (§14.10): ttc tda0/tda2 main 14 / 79–87 s, levers OFF 56–109 / 139–143 s, CPU ON 15–24 / 42–43 s, GPU ON 14.4 / 47.4 s (n = 1, loaded); postconv 100 %, 0 premise alarms, 0 wrong winners.** Simulator (4 RX, fix round 1): blind first RNTI 301 → 5.9 s (p95 28 s), DDDSU 179 → 5.6 s, later RNTIs ~2–2.6 s, 0 wrong in 78 cells (§14.9) | none | No SIB1 on the rfsim bed (exclusions untested end-to-end); K41 open; ~3 CB0 items per grant (K47); 4-RX gate provisional; full default combination never measured; Nl > 1 CB0 admissibility never exercised at runtime; sens6 GPU unverified; ELIM fix round not re-reviewed (K50) | X410 OTA 2026-10-05/06 checklist (§25); full-combo 4-RX gate on an idle host; gate option (b) |
| UL decode (G8) | Done: one-layer CP-OFDM, RA0 segmented, UCI footprint, TP limited | PASS on sens6 (9 RA0); **DGX: 7/9, 64/256QAM bit errors (K22)** | PASS (OCUDU 50–54 % at times) | historical lab 78.6 % at 272–273 PRB (older binary) | Width search time, 64QAM ceiling, TP unvalidated live | Measure on DGX |
| CSI-RS / ZP-CSI-RS | Done (rows 1–5, opt-in wide), ZP probation/revocation | PASS (131 + synth) | FAIL then fixed (G4 pending) | IDSWEEP solved on lab, confirmation not reached | G4 of the fixes | OCUDU CSI-RS arm n=3 |
| RS / CFR extraction (G9) | Done (SSB, DM-RS, data-aided DL/UL, CSI-RS) | PASS | PASS | partial (sensing CPIs formed on lab) | depends on G8 | — |
| **Receiver metrics / observations / campaign / dashboard** (new 2026-10-01) | `ISAC_METRICS` JSON (A2), per-grant obs JSONL (A3, schema v1 §21), campaign runner (A4), Receiver-health tab (A5), regression gate (A1) | PASS: 5 + 8 + 5 gtests, 4 + 15 + 16 python tests (cloud x86, §13.2) | **PASS cloud x86 only** (§14.5); not run on the DGX | n/a | UL obs hook not exercised (K28); in-line DL decode not recorded (K29); DGX gate thresholds not yet reproduced | Run gate + campaign on the DGX (§25) |
| Acquisition state / reacquisition (G10–G11) | Evidence tracker + local DL relearning + stream-gap LOST; experimental unified epoch/change detector (`ISAC_RECONF=1`) | PASS | partial | observed LOST→reacquire (2026-09-13) | Live epoch/recovery and false-trigger targets unvalidated | R13 + soak; §25 robustness block |
| Multi-cell / carrier / operator (G12–G14) | **None** (one process = one cell). Parallelism: N<=4 blind-PDCCH scan consumers implemented (A7); core-map launcher + scan-scratch knob implemented (A6, A11) | PASS (gtest, TSAN consumer<->consumer 0, cloud x86) | cloud x86 A/B only (§14.5); core map and 273-PRB A/B **not measured** | — | Architecture; `UEthread_0` and `pdcchUssHash` still serial; K27, K30 | §14.3, §25 (A8–A10, A12), §17–§20 |
| Sensing consumer | Separate subsystem; newest work on unmerged branch | PASS (with sensing ON) | — | CPIs produced | Merge debt | Decide merge policy for `feature/multirx-clean-adaptive` |

---

## 17. The final intended pipeline (DESIGN TARGET)

```text
                         ┌─────────────────────┐
                         │   RF / X410 input   │  CURRENTLY IMPLEMENTED (1 carrier, <=4 ch, 122.88 MS/s)
                         └──────────┬──────────┘
                                    ▼
                         Wideband acquisition            PLANNED (400 MHz capture + channelizer, GPU)
                                    ▼
                       PSS / SSS / SSB discovery          PARTIAL (GSCN scan in one RF window; auto_acquire.py unvalidated)
                                    ▼
                          Cell identification            PARTIAL (PCI + SIB1 PLMN/cell id; no cell_track_id)
                                    ▼
                            PBCH / MIB decode            CURRENTLY IMPLEMENTED
                 ┌──────────────────┴──────────────────┐
                 ▼                                     ▼
            SIB1 available                    NSA / SIB1-less
       CURRENTLY IMPLEMENTED                       PARTIAL
                 └──────────────────┬──────────────────┘
                                    ▼
                    CORESET / SearchSpace discovery      PARTIAL (restricted family; SS/AL inference partial)
                                    ▼
                         Blind/adaptive PDCCH            CURRENTLY IMPLEMENTED (single cell)
                                    ▼
                              DCI recovery               CURRENTLY IMPLEMENTED (bounded layout families)
                                    ▼
                    Scheduled-resource reconstruction    CURRENTLY IMPLEMENTED (bounded catalogues) / PARTIAL
                                    ▼
                   PDSCH / DMRS / reference signals      CURRENTLY IMPLEMENTED (DL rank<=4, UL rank 1)
                                    ▼
                       PHY/channel observations          CURRENTLY IMPLEMENTED (CFR to sensing engine)
                                    ▼
                         Per-cell observation API        PLANNED
```

Multi-cell / multi-carrier extension (all **PLANNED**):

```text
                         DiscoveryManager                 (band scan, SSB inventory, scheduling of RF windows)
                                │
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                 ▼
           Carrier A         Carrier B         Carrier C       CarrierContext: LO/decimation/channel subset,
              │                 │                 │            shared IQ ring, per-carrier GPU budget
       ┌──────┴─────┐     ┌────┴──────┐         ...
       ▼            ▼     ▼           ▼
     Cell 1       Cell 2 Cell 3      Cell 4                    CellContext: sync, config, RNTIs, hypotheses, health
```

Future behaviour (design intent, from the project's 2026-09-27 multi-cell plan and wideband GPU spec, both
**not approved for implementation yet**):
- **Multi-cell (co-channel):** one capture ring, N read cursors; cell detection reports all PCIs above threshold;
  a supervisor spawns one receiver instance per selected PCI (top-K by SSB RSRP; an operator list may *select*,
  never *seed*). Each instance owns its timing/CFO/SSB loops; verify the strongest cell cannot capture a weaker
  instance's timing loop (Salt PCI 64 vs PCI 244 is the real-world case). Optional successive cancellation of the
  strongest cell's known RS only if measured necessary.
- **Multi-carrier:** carrier separation in the X410 FPGA (RFNoC DDC per channel: tune + decimate) or a GPU
  polyphase channelizer on a 400 MHz capture; run-time channel-allocation profiles (`4×opA` for AoA/MRC,
  `2×opA+2×opB`, `1×{A,B,C,D}`); intra-operator carriers are separate cells (no CA).
- **Multi-operator:** same machinery; operator identity is an attribute of a cell (from SIB1 PLMN when available),
  never a code path.
- **Persistent tracking + reacquisition:** §19.
- **Event-driven scheduling:** expensive search runs only on events (new SSB, health drop, config-change evidence,
  new RNTI), otherwise cheap health checks (§20).
- **RF retuning when carriers do not fit the instantaneous bandwidth:** time-share the RF front end between carriers,
  preserve `CellContext`s across retunes, timestamp every observation with the RF epoch, and re-validate with a cheap
  SSB/PBCH check instead of full rediscovery after each retune.

---

## 18. Final multi-operator architecture (DESIGN TARGET)

No operator-specific pipelines. The receiver manages generic objects:

```text
CarrierContext {
  carrier_id; rf_centre_hz; sample_rate; channel_subset (antennas); lo/decimation profile;
  capture_ring; rf_epoch (retune generation); gpu_budget; cells[]; health (drops, stalls, NIC loss)
}
CellContext {
  cell_track_id; carrier_id;
  frequency (SSB ARFCN/GSCN, Point A, centre); PCI; SSB index set; numerology; bandwidth (N_RB);
  timing state (rx_offset, max_pos_acc, drift); CFO state; SFN state (+ wrap epoch);
  MIB state; SIB1 state (PLMN list, cell identity, TAC, TDD, common TDRA, BWPs, PRACH) | NSA state (no SIB1, RAR anchors);
  CORESET state (bank); SearchSpace state (AL set, candidates, periodicity);
  known RNTIs (lifetime-keyed, reuse epoch); DCI hypotheses (lengths, layouts, pins);
  per-RNTI DL/UL waveform hypotheses (Technique D contexts, UL interp contexts);
  rate-matching resources (SSB, CSI-RS/ZP); scrambling IDs;
  health metrics (PBCH ok rate, accept rate, CRC rate, drops); confidence; acquisition state; last_seen;
  config_generation (unified epoch)
}
```

Where the fields live **today** (all as process-global singletons → one cell per process):

| Conceptual field | Exists today in | Notes |
|---|---|---|
| PCI, SSB index, numerology, N_RB, FFT geometry | `PHY_VARS_NR_UE`/`NR_DL_FRAME_PARMS` (`Nid_cell`, `ssb_index`, `numerology_index`, `N_RB_DL`) | upstream |
| Timing state | `nr_adjust_synch_ue.c` (`max_pos_acc`, shift), `nr-ue.c` rebase state | global |
| CFO state | `UE->freq_offset`/common offset, CFOTRK state in `nr-ue.c` | global |
| SFN state | UE proc frame/slot; unwrapped absolute slots in the blind monitor | partly |
| MIB state | `mac->mib`, `mac->dmrs_TypeA_Position` | upstream MAC |
| SIB1 state | `nr_pdcch_blind_common_config_t` common facts + per-PCI file cache; `nr_pdcch_sib1_prior.c` | global |
| CORESET state | `nr_pdcch_coreset_bank.c` bank, `g_cfg` in `nr_pdcch_blind_monitor.c` | bank life cycle is epoch-aware under `ISAC_RECONF=1`; old identity is archived/isolated on HARD_RESET (the prior global/not-cleared note is HISTORICAL) |
| SearchSpace state | `nr_pdcch_ss_registry.c`, `nr_pdcch_uss_tracker.c` | global |
| Known RNTIs | `nr_pdcch_blind_rnti_bootstrap.c` (16 slots) | numeric RNTI only, no reuse epoch |
| DCI hypotheses | `nr_pdcch_dci_length_sweep.c`, `nr_dci11_pin.c`, layout sweeps | global |
| Waveform hypotheses | `nr_pdsch_config_sweep.c` `g_rnti[64]` contexts; UL interp contexts | global LRU |
| Rate-matching resources | `nr_csirs_blind_*` banks, `nr_ssb_rate_match.c` | global |
| Acquisition state | `nr_passive_acq_state.c` | global, evidence labels |
| Health metrics | RFCENSUS counters, summary counters | log only |
| `cell_track_id`, `carrier_id`, `confidence`, `last_seen`, unified `config_generation`, operator attribute | **do not exist** | must be introduced |

Migration path (design intent): first wrap the existing globals in a single `CellContext` instance (pure refactor
with bit-identical tests), then allow N instances (one per process first, per the multi-cell plan's decision
"one receiver instance per (carrier, cell)"), then share front-end work.

---

## 19. Future persistent receiver state machine (DESIGN TARGET — not implemented)

The existing `nr_passive_acq_state.c` is an **evidence label reporter**, not a controller. The target is a
controller per `CellContext`:

| State | Purpose | Required information | Exit (success) | Failure → | Logs | Gate |
|---|---|---|---|---|---|---|
| SEARCH | Find SSBs in the RF window/band | search domain, RF window | PSS+SSS+PBCH CRC for a PCI | stay; widen/retune window | `Cell Detected…`, `Initial sync successful` | G1–G3 |
| SSB_LOCK | Hold timing/CFO on the SSB | PCI, SSB position | stable PBCH for N frames | SEARCH after K PBCH failures | RFCENSUS | G3 |
| MIB_LOCK | Consistent MIB, SFN | MIB | MIB consistent over frames | SSB_LOCK | CSS0 autoconf / MIB DM-RS line | G4 |
| COMMON_CONFIG | SIB1 (or NSA path) → carrier, TDD, common SS/TDRA | CORESET#0 or NSA anchors | SIB1 decoded (G5A) or NSA minimum set (G5B) | MIB_LOCK after timeout (SIB1-less → NSA path) | SIB1 lines / NSA lines | G5A/G5B |
| TRAFFIC_DISCOVERY | Dedicated CORESET/SS, RNTIs, DCI layouts, waveform hypotheses | traffic present | bank verified + ≥1 RNTI converged DL or UL | stays with bounded budget; reports "idle cell" | bank add, length locked, CONVERGED | G6–G8 |
| TRACKING | Cheap operation: decode, export observations | converged contexts | — | DEGRADED on health drop | periodic summaries | G9–G10 |
| DEGRADED | Health below threshold but sync holds | health metrics | recovery → TRACKING | LOCAL_REACQUIRE on persistent failure | health lines | G10 |
| LOCAL_REACQUIRE | Relearn only affected contexts (RNTI/TDA/CORESET), bump generation | evidence of change | TRACKING | GLOBAL_SEARCH | relearn lines (`ORACLE_RESTORE`, generation bumps) | G11 |
| GLOBAL_SEARCH | Full re-acquisition of the cell (keep prior as hint, revalidate) | — | SSB_LOCK | SEARCH | LOST lines | G11 |

Requirements: a unified sample/configuration epoch across queued jobs and CFR rows (stale jobs rejected after any
transition), explicit evidence expiry, hysteresis in sample time (not "updates"), and per-transition logging.

---

## 20. Future multi-operator computational strategy (DESIGN TARGET)

Avoid "operator 1 full blind search → operator 2 full blind search → repeat". Instead:

```text
initial discovery (expensive, once per cell)  ->  persistent CellContext state  ->  cheap health tracking
       -> expensive rediscovery ONLY on evidence (health drop, new PCI/RNTI, config-change signature, long gap)
```

Measured costs that motivate this (sens6, CPU, HISTORICAL but indicative): one 100 MHz 4-RX receiver uses most of a
12-core host; blind stage-1 nID sweep over 65536 IDs takes 3.3 s on the GPU + 0.6 s stage 2 (2026-09-23); Technique D
converges in ~1.3–5 s once traffic flows; the joint solver is CPU-faster below ~300 candidates; single-TB GPU LDPC was
20× slower wall-clock than CPU (transfer-dominated) — GPU only pays with large batched per-slot work
(many cells × antennas × grants) and data resident on the GPU from capture to bits. (2026-10-04, DGX GB10: batched CB0
LDPC on the GPU is 1.05–1.4× an 8-thread CPU on wrong hypotheses, 2–3× on true ones, §14.10.)

- **Cells inside the same instantaneous bandwidth:** one shared IQ ring; shared FFT/FEP of the common grid where the
  numerology and grid align; independent `CellContext`s; per-cell PDCCH search on shared frequency-domain data;
  shared GPU batches across cells and antennas.
- **Carriers outside the instantaneous bandwidth:** RF retuning or a second channel subset; preserve each cell's
  state across retunes; timestamp observations with RF epoch + sample index; after a retune run a cheap SSB/PBCH
  revalidation (tens of ms) instead of full rediscovery; schedule dwell by value (traffic, sensing need) and by
  staleness of each cell's evidence.
- Overload policy: bounded queues per cell, shed the lowest-priority cell first and report the gap; never block the
  RF reader.

---

## 21. Final output of the passive receiver (DESIGN TARGET)

Per observation (one record per decoded/observed grant or reference-signal occasion):

| Field | Available today? | Where |
|---|---|---|
| timestamp (sample index + host time) | partial | sample timestamps in the RF path; CFR rows carry slot times; no unified record |
| carrier_id, cell_track_id | no | — |
| PCI, SSB index | yes | frame params |
| SFN, slot | yes | UE proc / absolute slot counters |
| frequency, bandwidth | yes | config / SIB1 facts |
| signal type (SSB, PDCCH, PDSCH DM-RS, PDSCH data, PUSCH DM-RS, PUSCH data, CSI-RS) | yes | CFR source enums in `nr_isac.cc` |
| scheduled resource information (RNTI, PRBs, symbols, MCS, rank, TDRA, DM-RS) | yes (internal job structs) | PDSCH/PUSCH job structs; not exported as records |
| channel estimate (per antenna) | yes | CFR submissions (`nr_isac_submit_cfr_multi`) |
| timing measurement (per-grant delay) | partial | UL per-grant delay refinement; DL via sync |
| CFO / Doppler-related estimate | partial | CFOTRK, BRANCHFO, DMRSFO (residual) |
| signal quality (SNR, EVM, CRC) | partial | logs/counters |
| confidence / uncertainty | no (sensing has `p_real`/sigma, receiver not) | — |
| operator identity | optional (PLMN from SIB1 when present) | `SIB1 CELL` log only |
| gNB position | **not required** (never an input) | — |

**Implemented 2026-10-01 (Task A3): per-grant observation records, schema v1.** The authoritative schema is the header
comment of `openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.h` (JSON Lines, one record per decoded PDSCH/PUSCH grant, enabled by
`ISAC_OBS_PATH`, non-blocking ring, unknown = JSON `null`, consumers must ignore unknown keys; a meaning change needs schema 2).
`[IMPLEMENTED]` code; DL `[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 385e02b9cf]` (§14.5); UL **not exercised** (K28);
**no OTA**. Coverage v1: DL = deferred PDSCH queue only (K29); UL = every `nr_pusch_passive_decode()` with OK/CRC_FAIL/ZERO_TB.

| Field above → obs key | Available (v1)? | Source (DL / UL) |
|---|---|---|
| timestamp | yes: `abs_slot` (receiver slot of the samples), `t_mono_ns` (decode completion, not air time) | `job.absolute_slot` / arg; `clock_gettime` |
| PCI; SFN, slot | yes: `pci`, `frame`, `slot` | `frame_parms.Nid_cell`; `job.frame_rx/nr_slot_rx` / args (UL slot = DCI slot + k2) |
| frequency, bandwidth | yes: `carrier_hz`, `scs_khz`, `fs_hz`, `start_rb`, `nb_rb` (grant BW = nb_rb·12·scs) | `frame_parms` DL/UL carrier, SCS, sample rate |
| scheduled-resource info | yes: `rnti`, `rnti_class` (DL only), `start_sym`, `nb_sym`, `mcs`, `mcs_table`, `qm`, `nl`, `dmrs_symb_pos`, `dmrs_scrambling_id`, `tbs` (bits), `harq_pid`, `rv`, `ndi` | `job.grant`, `dec.cw`, `freq_alloc`, `dlsch_pdu` / `nr_passive_ul_grant` `g->…`, `out->…` |
| signal type | partial: `dir` DL (PDSCH data) / UL (PUSCH data) only; SSB, CSI-RS, PDCCH, DM-RS-only records **not** in v1 | — |
| signal quality | partial: `crc` (null for UL ZERO_TB), `nvar` (DL, receiver-internal scale), `snr_db` (UL; holds CFR mean power when noise est. = 0, known issue) | `dec.nvar`; `out->snr_db` |
| timing / CFO | partial: `delay_samples` (UL DM-RS CIR peak, 0 also = no clear peak), `fo_comp_hz` (FO the receiver removed, not a per-grant measurement) | `out->est_delay`; `job.fo_hz` / arg |
| channel estimate | no (CFR path unchanged, `nr_isac_submit_cfr_multi`) | — |
| carrier_id, cell_track_id, confidence, operator identity | no | — |

---

## 22. References and algorithm origins

| Reference | Pipeline part | What we use | What we do differently |
|---|---|---|---|
| **3GPP TS 38.211** (physical channels; V18.2.0 used in audits) | PSS/SSS (§7.4.2), PBCH + DM-RS (§7.3.3, §7.4.1.4), PDCCH mapping/REG bundles/interleaving + DM-RS + scrambling (§7.3.2, §7.4.1.3), PDSCH/PUSCH DM-RS type 1/2, scrambling IDs (§7.4.1.1, §6.4.1.1), CSI-RS rows (§7.4.1.5), Gold sequences (§5.2.1), resource grid | Sequence generation, RE mapping, identity domains | We *search* identities/configurations that a UE would receive by RRC |
| **3GPP TS 38.212** (V17.9.0) | Polar coding + CRC masking (§5.3.1, §7.3.2), DCI formats and field order (§7.3.1), LDPC (§5.3.2), rate matching (§5.4), UCI on PUSCH (§6.2.7, §6.3.2) | Decoders (OAI) and field definitions | DCI field widths unknown → enumerate layouts under the observed length; RNTI recovered from the CRC mask |
| **3GPP TS 38.213** (V18.2.0) | Cell search/initial access (§4.1), CORESET#0/Type0-PDCCH tables (§13), PDCCH monitoring and search-space hashing (§10.1), timing | CSS0 autoconf, candidate positions | Dedicated SS/AL inferred from occupancy and accepts |
| **3GPP TS 38.214** (V18.2.0) | PDSCH TDRA (§5.1.2.1), FDRA type 0/1 + VRB interleaving (§5.1.2.2–3), PRB bundling, MCS tables/TBS (§5.1.3), SSB/CSI-RS rate matching (§5.1.4), LBRM, PUSCH (§6.1) | TBS/TDRA/FDRA arithmetic | Technique D sweeps TDRA/DM-RS/MCS-table semantics per RNTI by TB CRC |
| **3GPP TS 38.331** | MIB, SIB1, PDCCH-Config, PDSCH-Config, CSI-RS configs (ASN.1 domains) | Field domains for enumeration; SIB1 decode | Dedicated IEs are not read (ciphered) — inferred |
| 3GPP TS 38.104 / 38.101-1 | GSCN raster, band n78, channel bandwidths | Raster tables (scan, auto_acquire) | — |
| **OpenAirInterface** (this repo's base, `integration_2026_w32`) | Everything upstream: UE PHY, PBCH/PDCCH/PDSCH decoders, gNB PUSCH receiver (reused for passive UL), UHD driver, rfsim | Code base | Passive mode, blind monitor, discovery, passive UL via a private gNB receive context |
| **NRSniffer** (OAI-based sniffer, copy at `sens6:/home/sens/NICOLA/NRSniffer`) | Blind PDCCH candidate handling | Mismatched-bits false-detection gate (migrated from its `dci_nr.c`), candidate class accounting | Adaptive thresholds; integrated into the RT scan thread |
| **5GSniffer** (NR PDCCH sniffer, published tool) | PDCCH DM-RS coherence gating | `correlate_DMRS()` + per-AL correlation thresholds idea | Adaptive per-occasion median+MAD floor instead of fixed {0.9,0.8,0.7,0.15,0.15}; default OFF (cut weak real grants) |
| **5GDescrambler** (source comments cite arXiv:2609.07367; not independently verified) | RNTI recovery | Linear GF(2) model of scrambling + CRC mask → joint RNTI/payload solve | Implemented as `nr_pdcch_joint_solve.c`/`gf2_rnti.c`, opt-in, validated in rfsim only |
| srsUE ISAC sensing pipeline (`srs-ue-isac-dmd`, D. M. Dumitriu) and deck "Multi-Static ISAC Using 5G NR Reference Signals & Data" | Consumer of receiver CFRs | Pipeline shape (CFR → range-Doppler → CFAR → tracking) | Ported into `openair1/PHY/NR_UE_ISAC/` |
| Wypich & Zieliński 2026 (MDPI Sensors), "Experimental Evaluation of 5G NR OFDM-Based Passive Radar Exploiting Reference, Control, and User Data" | Motivation for data-aided CFR | Data-aided full-band reconstruction concept | — |
| NVIDIA Aerial CUDA-Accelerated RAN (cuPHY, pyAerial; Apache-2.0) | Future GPU LDPC/polar/PUSCH chains | Candidate building blocks for §20 | Not integrated; GB10 support and licence compatibility to verify |
| OCUDU / srsRAN gNB, srsUE, open5gs | Simulated beds (ground truth by F1AP pcap / gNB logs) | Truth for validation only | — |

---

## 23. Pipeline block reference (uniform format)

### 23.1 RF acquisition / X410
Status: CURRENTLY IMPLEMENTED; stability `[KNOWN ISSUE]`.
Goal: continuous, loss-free multi-channel IQ with known channel/port/gain mapping.
Input: device args, rate (`-r`), centre (`-C`), gain, antenna count. Output: per-antenna sc16 buffers + timestamps.
Algorithm: UHD multi_usrp RX streamer, CHDR/UDP; short-read continuation; stall detection → abort.
3GPP: none. Implementation: `radio/USRP/usrp_lib.cpp` (`device_init`, `trx_usrp_read`, `usrp_set_rx_freq_all`), `executables/nr-ue.c`.
Runtime parameters: `--usrp-args`, `--ue-rxgain`, `--ue-nb-ant-rx`, `ISAC_RX_CHAN_MAP`, `ISAC_UE_RT_CORE`, `--thread-pool`.
Logs: §11.1. Offline: `benchmark_rate` (no OAI). OTA: 2026-09-25 (§15). Known issues: §8.
Pass gate: G0. Remaining work: re-characterize on DGX; restart-free stream recovery; passivity audit of TX setup.

### 23.2 SSB detection (PSS/SSS) and cell identification
Status: CURRENTLY IMPLEMENTED (single RF window). Goal: SSB position, PCI, timing, CFO.
Input: IQ in the RF window. Output: PCI, SSB subcarrier offset, rx_offset, CFO.
Algorithm: GSCN raster candidates in the window → PSS correlation → SSS ML over N_ID1 → PBCH CRC confirmation; CFO hypotheses.
3GPP: TS 38.211 §7.4.2, TS 38.213 §4.1, TS 38.104 raster. Implementation: `nr_initial_sync.c`, `pss_nr.c`, `sss_nr.c`.
Parameters: `--ssb`/`--ue-scan-carrier`, `--initial-fo`, `ISAC_SCAN_CONFIRM`, `ISAC_SCAN_ANT`, `ISAC_ACQ_CFO_MAX_HZ`.
Logs: §11.2. Offline: raw-baseline SSB checker. OTA: PASS 2026-09-25. Known issues: 273 PRB × 4 RX scan memory/CPU; co-channel capture.
Pass gates: G1, G2. Remaining: band-wide autonomous launcher validation (`auto_acquire.py`); multi-PCI reporting.

### 23.3 PBCH / MIB
Status: CURRENTLY IMPLEMENTED. Goal: frame timing and MIB. Input: SSB. Output: SFN, SSB index, MIB fields.
Algorithm: PBCH DM-RS chest, polar decode, CRC; per-branch decode options.
3GPP: 38.211 §7.3.3, 38.212 §7.1, 38.331 MIB. Implementation: `nr_pbch.c`, `nr_ue_procedures.c` (`nr_ue_decode_mib`).
Logs: §11.4. Offline: `test_nr_ue_mib_blind_handoff`. OTA: PASS. Known issues: PBCH tracking failures on loaded cells (RLM out-of-sync disabled in passive).
Pass gates: G3, G4. Remaining: soak.

### 23.4 SIB1 (SA)
Status: CURRENTLY IMPLEMENTED. Goal: cell-common configuration. Input: MIB (CORESET#0/SS#0). Output: PLMN, cell id, carrier geometry, TDD, common TDRA/SS/PRACH, initial BWPs.
Algorithm: Type0-PDCCH CSS on CORESET#0, SI-RNTI DCI 1_0, PDSCH decode, ASN.1 decode (upstream RRC), passive extraction to the blind monitor.
3GPP: 38.213 §13, 38.331 SIB1. Implementation: `nr_pdcch_blind_monitor.c` (CSS0 autoconf, common facts, cache), `config_ue.c`, `rrc_UE.c`, `nr_pdcch_sib1_prior.c`, `nr_tdd_pattern.c`.
Parameters: `pdcch_blind_monitor_autoconf=1`, `ISAC_SIB1_CACHE`, `ISAC_SIB1_CACHE_DIR`.
Logs: §11.5. OTA: PASS (lab, Salt once). Known issues: SIB1 cache persists by PCI across runs; OAI SI path decoded SIB1 on only ~half of X410 acquisitions at 4 RX (2026-09-15).
Pass gate: G5A. Remaining: cache keyed by cell identity + validity; SIB1 re-decode on change.

### 23.5 SIB1-less operation (SIB1 absent or not decoded; was titled "NSA / SIB1-less")
Status: PARTIAL. Goal: proceed without CORESET#0/SIB1. SIB1 presence is independent of SA/NSA (§11.5): an NSA cell that broadcasts SIB1 takes the §23.4 path.
Input: MIB (k_SSB ≥ 24), traffic. Output: dedicated CORESET, RNTIs, grants.
Algorithm: MIB DM-RS position → blind monitor; CSS0-free dedicated discovery; RAR-anchored trusted C-RNTI (CFRA) when RARs are visible; carrier/TDD from startup or cache.
Implementation: `nr_ue_procedures.c` (`nr_pdcch_blind_monitor_set_mib_dmrs_typeA_position`), `config_ue.c` (no-CORESET0 path), `nr_pdcch_blind_rnti_bootstrap.c` (`record_trusted`), `nr_pdcch_blind_monitor_rt.c`.
Logs: `MIB dmrs-TypeA-Position posX -> blind monitor`. Offline: 6 tests PASS. OTA: none with this code (Swisscom NSA observations 2026-09-21 are HISTORICAL, older binary). Known issues: no NSA bed; no signal-based carrier/TDD estimation; RNTI bootstrap without CSS0 unproven on air.
Pass gate: G5B. Remaining: synthetic no-SIB1 end-to-end fixture; TDD estimation from energy; OTA on a cell without decodable SIB1 (or a SIB1-ignoring arm). Note (2026-10-02): SIB1 presence is independent of SA/NSA (§11.5); whenever SIB1 is decoded the receiver uses it, also on NSA cells.

### 23.6 CORESET / SearchSpace discovery
Status: PARTIAL. Goal: dedicated CORESET geometry, PDCCH identity, AL set.
Input: FEP of the monitored symbols, occupancy. Output: verified CORESET bank entries, AL profile.
Algorithm: Technique A — correlate PDCCH DM-RS in 6-RB windows (PCI-derived identity), accumulate occupancy (≥ 1000 calls, ~30 hits/window, floor 3, cap 400 000), enumerate contiguous extents (≤ 45 windows, 1035 candidates), verify with fresh same-RNTI DCIs at later slots with different payloads (≤ 4000 occasions); AL1 cover (opt-in); offline/GPU stage-1 nID sweep (65536 IDs × 14 symbols on dumped snapshots) + stage-2 geometry, handed back via `/tmp/coresets_discovered.txt` (polled by `nr_pdcch_blind_monitor_discovered_poll()`); USS AL inference from accepts.
3GPP: 38.211 §7.3.2, 38.213 §10.1. Implementation: `nr_pdcch_coreset_map.c`, `nr_pdcch_coreset_bank.c`, `nr_pdcch_al1_map.c`, `nr_pdcch_uss_tracker.c`, `nr_pdcch_ss_registry.c`, snapshot `discovery_tool/idsweep_offline.c`.
Parameters: `pdcch_blind_monitor_autodiscover`, `ISAC_COREMAP_IDSWEEP=<batches>`, `ISAC_COREMAP_*`, `ISAC_AL1_COVER`, `ISAC_PDCCH_DISCOVERY_BUDGET_US`.
Logs: `autodiscover -- CORESET footprint`, `multi-CORESET bank add`, ladder/STAGE0. Offline: PASS. SIM: OCUDU CORESET RB 0..47 dur 2 = truth. OTA: lab PASS.
Known issues: duration 2/3 ranking needs multi-symbol snapshots (partly added 2026-09-25), AL1-dominated cells (Swisscom ~99.997 % AL1) give honest ambiguity, and first-symbol restriction in RT scan. The former bank-not-cleared-on-cell-change issue is fixed for the `ISAC_RECONF=1` epoch path (§24 K37).
Pass gate: G6. Remaining: holed bitmaps, multiple CORESETs, interleaved dedicated CORESETs, fair AL coverage.

### 23.7 Blind/adaptive PDCCH decoding
Status: CURRENTLY IMPLEMENTED. Goal: decode all candidates in known CORESETs and recover RNTIs.
Algorithm: per occasion, per candidate: DM-RS LS chest, equalization, REG/CCE demap/deinterleave, descramble, polar decode, CRC-mask RNTI recovery, re-encode mismatch gate, energy/persistence/SNR gates, queue to scan thread; three passes (CORESET#0-USS, bank, discovery walk) with evidence-gated pruning and cross-pass dedupe.
3GPP: 38.211 §7.3.2, 38.212 §7.3. Implementation: `nr_pdcch_blind_monitor_rt.c`, `nr_pdcch_blind_monitor.c`, `dci_nr.c`, `nr_pdcch_passive_queue.c`, optional `nr_pdcch_gpu_fep.cu`.
Parameters: `pdcch_blind_monitor_scan_thread`, `pdcch_blind_monitor_noise_gates`, `pdcch_blind_monitor_rnti_range`, `ISAC_PDCCH_DMRS_GATE`, `ISAC_PDCCH_JOINT`.
Logs: §11.6. Pass gate: G6/G7. Remaining: real-time budget at 4 RX; GPU batching.

### 23.8 DCI recovery and interpretation
Status: CURRENTLY IMPLEMENTED (bounded). Goal: exact field layout for 1_1/0_1; resolved grants.
Algorithm: Technique B (RNTI persistence), Technique C (length sweep 30–63 bits, gate `passes ≥ n·p0 + 6√(n·p0(1−p0)) + 1`, p0 = 1/256), layout enumeration under the length (BWP-ind 0–2, TDRA 0–4, FDRA type/dynamic, VRB-PRB, bundling, rate-matching/ZP, antenna ports), decode-free plausibility pruning, CRC-scored interpretation, pinning; UL 3-component discovery (length, widths, interpretation incl. RA0/TP), 1_0 exhaustive class barrier (REJECTED/AMBIGUOUS/UNRESOLVED).
3GPP: 38.212 §7.3.1. Implementation: `nr_pdcch_dci_length_sweep.c`, `nr_pdcch_dci11_layout_sweep.c`, `nr_pdcch_dci01_layout_sweep.c`, `nr_dci11_pin.c`, `nr_pdcch_ul_*`, `nr_pdcch_blind_rnti_bootstrap.c`, `nr_pdcch_joint_*`.
Logs: §11.7. OTA: lab DCI 47/45 = truth (2026-09-17). Known issues: false-lock calibration under repeated looks; unused fields' widths can stay ambiguous; data-scrambling-ID walk PARTIAL.
Pass gate: G7.

### 23.9 Scheduled-resource reconstruction and PDSCH/PUSCH decoding
Status: CURRENTLY IMPLEMENTED (bounded catalogues). Goal: decode TBs of any UE.
Algorithm: grant → PRB set (RIV/type 0/VRB interleaving) → TDRA semantics via **Technique D** (per RNTI/TDA context: (S,L) catalogue incl. type B and k0, DM-RS add-pos 0–3, max-len 1/2, type 1/2, MCS table 0–2(+3/4 UL), PRG, PTRS, LBRM n_L; DM-RS mask oracle, Qm oracle; KL/Bernoulli (Chernoff) anytime bounds `nr_crc_interval()` with a 1e-6 budget union-bounded over the live catalogue; selection = 3 of 4 grants to the hypothesis with most CRC passes until it has 64 trials, else shuffled round robin; early convergence (checked every 16 trials) = leader ≥ 64 trials & lower bound ≥ **0.02** & leader lower bound above EVERY other hypothesis's upper bound; fallback = all hypotheses ≥ 300 trials & best ≥ 2 % & ≥ 3× runner-up, else undecided. **Corrected 2026-10-01:** earlier versions of this file said LB ≥ 0.60 — that floor was removed in the code after the 2026-09-13 OTA winner decoded at only 40 % (`nr_pdsch_config_sweep.c` ~544). Cell-wide prior: promoted once two distinct RNTIs converge on the same fields, then seeds new RNTI contexts (sibling contexts converged 29× sooner). Convergence cost model (simulation of this exact logic, 2026-10-01, `[HYPOTHESIS]` until measured): grants ≈ linear in catalogue size and ∝ 1/p_true — n_hyp 6/30/233/800 at p_true 0.4 → ~0.9 k / 4.8 k / 37 k / 127 k grants; at p_true 0.95 → 0.25 k / 1 k / 7.3 k / 25 k; 0/60 wrong winners) → FEP/chest/MRC-MMSE (rank ≤ 4) → LLR → descramble (RNTI, data ID) → rate recovery (SSB/CSI-RS/ZP masks, xOverhead) → LDPC → TB CRC → feedback via generation-tagged ticket. UL: grant book (slot+k2), private gNB receive context, per-grant delay refinement, UCI footprint cache, RA0 segment gather, TP (PCI-default identity, no hopping).
3GPP: 38.214 §5.1, §6.1; 38.211 DM-RS; 38.212 LDPC/rate matching/UCI. Implementation: `nr_pdsch_passive_queue.c`, `nr_pdsch_passive_decode.c`, `nr_pdsch_config_sweep.c`, `nr_ssb_rate_match.c`, `nr_csirs_blind_*`, `nr_harq_init_tx.h`, `nr_pusch_passive_*`, `nr_pdcch_ul_interp_sweep.c`.
Parameters: `pdcch_blind_monitor_pdsch`, `pdcch_blind_monitor_ul_*`, `ISAC_RX_MRC_MODE`, `ISAC_CHEST_TINTERP`, `ISAC_SFO_CORRECT`, `ISAC_QM_ORACLE` (on), `ISAC_PDSCH_TYPEB` (on), `ISAC_PDSCH_K0_PROBE` (on), `ISAC_PRG_SWEEP` (on), `ISAC_UL_FDRA_REFUSE` (off), `ISAC_CSIRS_BLIND` (off).
Logs: §11.8–11.9. SIM: phy-test 98.9 %; OCUDU 75–76 % C-RNTI PDSCH (MCS-10 CSI-RS slots were 0 % before CSI-RS rate matching). OTA: low on 2026-09-25. Known issues: precoder nulling, rank vs RX count, int8 LLR saturation at rank 4 (float MMSE option), CPU at 4 RX, HARQ soft combining UL not built.
Pass gate: G8.
**Branch `td/convergence-levers` (2026-10-02, not merged) changes this block:** the "DM-RS mask oracle" above used to
prune k0 as well (it recorded the hypothesised k0 as observed) — `HISTORICAL`, unsafe (K39); now it prunes on DM-RS
mask / last symbol only (monotone last-symbol sets, K42) and records k0 as *plausible*. A k0 is removed only by a hard
exclusion from a CRC-confirmed DCI (SIB1 TDD direction per hypothesis, or DCI adjacency; K40/K43) or by `certify_k0`
(no runtime caller yet). The runtime still trials **one hypothesis per grant** with the selection/acceptance rule
above; `next_k`/`feed_k` (K > 1), the grant gate, ordering, field book, dormant masks and levers C/P/E exist in the
engine and simulator only (§3.3.1). The convergence-cost model above is superseded by the simulator (§14.9).
**2026-10-03/04 (merged-main defaults, §10.2):** the field book (fb2) prunes reversibly at context creation; and while a
context acquires, each new-data rv0 grant also gets a **CB0 batch**: code block 0 of a fixed, hash-chosen subset of the
active hypotheses (same k0 as the grant; the scheduled one always included) is decoded from shared GrantWork LLRs, and
a hypothesis whose CB0 upper bound falls below the full-TB leader's lower bound becomes dormant (ELIM). The full-TB KL
rule above still elects the winner; the TB decoder is forced to the CPU while acquiring, and CB0 runs on the GPU when
available (CPU otherwise). CRC evidence is stratified by `decoder_used` (K36).

### 23.10 Reference-signal / channel extraction
Status: CURRENTLY IMPLEMENTED. Goal: per-antenna CFR with provenance. Sources: SSB/PBCH DM-RS, PDSCH DM-RS, PDSCH data-aided (TB re-encode; one layer), PUSCH DM-RS, PUSCH data-aided (no UCI), CSI-RS (configured or blind).
Implementation: `nr_pdsch_data_aided.c`, `nr_pusch_data_aided.c`, `nr_isac.cc` (`nr_isac_submit_cfr*`). Invariant: enumerated data-RE count must equal `G/(Qm·Nl)` or nothing is submitted.
Pass gate: G9.

### 23.11 Acquisition state / reacquisition
Status: PARTIAL (evidence tracker). See §11.11, §19. Pass gates: G10, G11.

---

## 24. Known bugs and open questions

| ID | Component | Problem | Evidence | Severity | Workaround | Required investigation |
|---|---|---|---|---|---|---|
| K1 | Whole receiver | **HEAD has never run OTA**; 136 commits since the last OTA binary `aa872eba3b` | git + capture `CMDLINE`/hash lines | High | none | §25 steps 6–10 |
| K2 | Branches | Receiver fixes on unmerged `feature/multirx-clean-adaptive` (238 commits ahead of base `51f7d3deac`). **Triaged 2026-10-01 on the DGX:** 25 non-merge commits touch receiver paths, none patch-equivalent in HEAD. PORT: scan-confirm CFO fix `5c568b184d`+`43a2227e7c`+`890e6a999a` (scan-confirm is ON by default in HEAD → with `--ue-scan-carrier` the confirm pass re-applies only the residual CFO as the total, radio ends ~14.6 kHz off, all PDSCH incl. SIB1 NACK); BRANCHFO thread-safety/CRC-gating `092a0d3d78`(serialisation only, not default-on)+`0229944db9`+`2d0f65b1b5`; P39 single-branch PDSCH chest `46032db06f`; help-text fix `81964f1ea5`. SKIP: 11 LLR-confidence / decision-directed CFR commits (sensing-only), 3 multi-RX ISAC-ABI commits, `74c8d80852` (sensing CFR off scan thread), launchers `17001b8581` (edits the frozen sens6 `run_arm.sh`) and `0e62938991` | `git log --cherry-mark HEAD...origin/feature/multirx-clean-adaptive` | High | — | Ported on branch `port/multirx-rx-fixes` (§3.1) |
| K3 | RF/host | RFSTALL (overflow/timeout), NIC loss (nic_miss 22–52 k), claim collisions in 2026-09-25 runs | `latest_ota_2026-09-25/*/verdict.txt`, milestones | High | 60 s waits, SIGINT, IRQ pinning | Re-characterize on DGX (§7); [HYPOTHESIS] stale launcher IRQ list and NIC at 99–108 °C contributed |
| K4 | DL decode OTA | TB rate ~0–1 % on lab 2026-09-25 (fully agnostic conf, 1 RX) vs 73 % on 2026-09-14 (different binary/conf) | verdicts `VOID_DL_RATE`, LDPCDIAG | High | — | Re-run on DGX; separate Technique D trial starvation (72 CRC-OK TBs < 300 floor) from link/precoder effects; MCS/rank census vs gNB log |
| K5 | Multi-antenna | Branch imbalance (ch1/ch3 −8…−15 dB) on the old unit; MRC gains ≤ ~1 dB; 4-RX CPU overload | ANTPOW, investigation report | Medium | `ISAC_RX_MRC_MODE=0` (branch 0) | Measure new unit; retest MRC modes 1–3 once balanced |
| K6 | 4-RX acquisition | Window lands ±342 samples (N/12) early/late on 4-ch/macro; gated rebase added | TSYNC_OBS audits | Medium | `ISAC_TSYNC_GLOBAL_REBASE` (default on), `ISAC_TSYNC_REBASE_RATIO=3` on Salt | Root cause of why 4-antenna acquisition lands off is open |
| K7 | SIB1-less (was "NSA") | No SIB1-less/NSA bed; carrier/TDD not estimated without SIB1; RNTI bootstrap without CSS0 unproven. 2026-10-02: SIB1 presence is independent of SA/NSA (Swisscom NSA broadcast SIB1, §11.5) — the gap is "SIB1 absent or not decoded", not "NSA" | NSA lane report | High for cells without decodable SIB1 | Startup geometry + SIB1 cache | Build synthetic fixture; OTA SIB1-less arm (ignore SIB1 on a SIB1 cell) |
| K8 | CORESET discovery | Restricted family (contiguous, dur 1, non-interleaved); AL1-dominated cells ambiguous; first-symbol restriction. Bank identity isolation/life cycle is fixed under `ISAC_RECONF=1` (K37) | code + OTA notes | Medium | offline stage-1/2 tool | Multi-symbol ranking, AL1 joint consistency |
| K9 | CSI-RS | ZP fixes (probation, decoded-grant revocation) only offline; 8-RE ZP / two-hole symbols refused by design; UE-specific ZP not representable | CLOUD_REPORT (retired) | Medium | `ISAC_CSIRS_BLIND` default off | G4 on OCUDU CSI-RS bed |
| K10 | State | **Fixed in `1ecf98d162`..`c6fc03be7c`:** one `CellConfigEpoch` owner stamps/rejects queued work and makes learned state a hint by class-aware scope; UeContext adds RNTI incarnation/reuse tracking. [OFFLINE VERIFIED, DGX aarch64] epoch/replay tests; live recovery/false-trigger rate remains unmeasured | R7–R12, R17, RE reports | Medium | keep `ISAC_RECONF` unset for normal OTA baseline | R13 + soak validate recovery and false-trigger targets |
| K11 | SIB1 cache | **Fixed in `1ecf98d162`:** reconfiguration cache name keys PCI plus canonical SIB1 semantic hash; periodic re-decode compares semantics, not raw bytes. [OFFLINE VERIFIED, DGX aarch64] cache/epoch fixtures; old PCI-only cache remains historical when reconf is off | R7/R8b reports | Medium (agnosticity) | `ISAC_RECONF=1`; SIB1-less arm suppresses cache facts | Live SIB1 semantic-change validation |
| K12 | Launchers | Hard-coded sens6 paths/IPs/NICs/cores; `run_arm.sh` default `REPO` is another tree; `ssh sens4` gNB-log bracket; SIGKILL in watchdogs; verdicts tuned for sensing | script source | Medium | pass `REPO/BIN/DATA/MGMT/NIC` | Parameterize |
| K13 | Passivity | TX streamer is still created by the UHD backend (writes suppressed in software) | `usrp_lib.cpp`, `nr-ue.c` | Medium (legal/ethics) | tx gain 0, RU_write suppressed | Audit every TX entry point; verify no emission with a spectrum analyzer |
| K14 | UL | Transform-precoding live validation impossible on current beds (srsUE lacks TP); UL HARQ soft combining not built; UL TDRA type B only two curated rows (plus energy pin) | lane reports | Low–Medium | — | OAI SA bed with TP |
| K15 | Build | Physim targets fail to link; 12 sensing tests fail to link with `ENABLE_ISAC_SENSING=OFF` | ctest/cloud report | Low | build explicit targets | — |
| K16 | Tests | `test_vrtsim_cirdb` shm race | ctest 2026-09-30 (sens6); **passed on the DGX 2026-09-30** (race, not deterministic) | Low (upstream) | ignore | — |
| K17 | GPU | CUDA LDPC plugin 20× slower than CPU for single TBs (RTX 4060 Ti, PCIe, HISTORICAL); `CMAKE_CUDA_ARCHITECTURES=52` in cache. **DGX 2026-10-01:** GPU modules build for `sm_121` with `-DLDPC_CUDA_ARCH=121` (`build_gpu`); PDSCH GPU FEP test 9 cases OK, polar bit-exact, batched CB0-size probe work 177 → 9.9 → 6.8 µs/probe at batch 1 → 32 → 256. **2026-10-03 correction (task BATCH):** those 6.8 µs are GPU chest + equalisation + LLR for a CB0-sized span, **no LDPC**; not comparable with a decode. **2026-10-04:** batched CB0 LDPC re-measured on GB10: via G1's TB entry 305–385 µs per wrong CB0 (latency-bound, slower than 8 CPU threads); via the dedicated CB0 entry 77–81 µs (1.05–1.4× CPU-8, bandwidth-bound) `[MEASURED, DGX GB10]` (§14.10) | measurement (2026-09-2x), cache, task-BATCH, task-CB0GPU | Low | CPU LDPC for full TBs; GPU CB0 backend for the elimination channel | layered/on-chip CB0 decoder (§25 deferred) |
| K18 | Datasets | Raw OTA IQ recordings and the decoder-oracle replay directory referenced by earlier validations are missing from sens6's mounted disks | `ls`/`find` 2026-09-30 | Medium | — | Check unmounted `sda1`; re-record on the DGX |
| K19 | Timing loop | Timing runaway (max_pos_acc wind-up when no SSB decodes); two RFSTALL modes (deaf stream vs signal present) | RFSTALL census (Sept) | Medium | watchdog on PBCH count | [HYPOTHESIS] estimator issue under load |
| K20 | DL precoding | PDSCH decodability depends on the served UE's precoder (beam nulls the passive receiver) | 2026-09-13 lab | Inherent | multiple UEs / positions | Report per-UE PMI with results |
| K21 | ARM / DFT (upstream) | `dft_test` fails on aarch64: DFT-12 EVM ~190 %, size 16 "unsupported" → abort; with a size filter **every** size fails the test (EVM 29–50 %). Cause: on aarch64 `tools_defs.h` keeps the **legacy NEON DFT table** (`oai_dfts_neon.c`, "some newly generated DFT functions are not implemented for this architecture", upstream w31 `enhancedDFT` merge) and the upstream test assumes the new x86 conventions. **Independent check with `scale_flag=1` (the mode used by every receiver OFDM path: `slot_fep_nr.c`, `nr_pbch.c`, `pss_nr.c`, `ofdm_mod.c`, `nr_phy_common.c` freq2time): correct at 128…4096 incl. 1536/2048/3072/4096, SQNR 49–56 dB, scale = 1/√N.** Not a compiler issue (same at -O0 / armv8.2-a). | `dgx_host_snapshot_2026-09-30/ctest_dgx_3b67eeee39.txt`, `dft_neon_scale1_check.txt`, `tools/dftcheck.c` | Low for OFDM receive; **unknown for UL transform precoding** (aarch64 `nr_idft()` in `nr_ulsch_demodulation.c` uses DFT-12 with `scale_flag=0` and its own layout) | none needed for CP-OFDM | TP on ARM: validate with a TP fixture before trusting (see K14); report upstream |
| K22 | ARM / UL high-order QAM | `test_nr_pusch_ra0_qam256` fails on aarch64 (and `_qam64` **intermittently**: failed on a re-run, passed in the full ctest run): at 60 dB SNR `sign_errors` 2–15 of 17 664 LLRs per trial, channel BER ~4e-3, TB CRC still OK in most trials (BLER 0–50 % over 10 trials). QPSK/16QAM and all other RA0 tests pass. Passed on sens6/x86. The fixture's TX side (nr_ulsim encoder path) **and** RX side (gNB PUSCH chain reused by the passive UL) both run ARM code, so the fault is not yet localized. Related upstream failure: `test_nr_modulation` `NrLayerPrecoderTest.SIMD` (imag part off by 2 LSB on ARM). The 1-layer 64/256QAM LLR SIMD loops in `nr_phy_common.c` look correct; the scalar tail of `nr_64qam_llr` omits the abs() (both archs, ≤ 3 REs). | `pusch_ra0_highqam_arm.txt`, ctest log | **Medium** — passive UL at 64/256QAM on the DGX may be degraded. **DL is NOT affected:** phy-test DL 256QAM (`-m 25 -n 1`) on ARM decoded 99.5 % of TBs with MCS table 1 blind-found (§14.1) | none | Localize: run the fixture with TX-side and RX-side SIMD swapped for scalar references; check simde `mulhi/mulhrs/sign/packs` on NEON in the gNB-side PUSCH chain (`nr_ulsch_demodulation.c`, `nr_ulsch_llr_computation`) |
| K23 | Branches | **Resolved 2026-10-01:** `feature/multirx-clean-adaptive` pushed to `github` by the operator (tip `750338ed9e`). `sdd/rfsim-gnb-test` still only on sens6 | `git ls-remote` | Low | — | Push `sdd/rfsim-gnb-test` when sens6 is back |
| K24 | DGX NIC | Both ConnectX-7 devices enumerate at boot then are torn down ("Link down", E-Switch cleanup) and vanish from `lspci`/`ip link` when uncabled | kernel log 2026-09-30 15:19 | Medium (blocks G0 until checked) | — | After cabling the X410: confirm the CX-7 port stays up; set MTU 9000/rings; persistent NM profile; pin IRQs to X925 cores |
| K25 | DGX real-time | No PREEMPT_RT kernel, no isolated cores, `ulimit -r 0`, no passwordless sudo; big.LITTLE CPU (X925 = cpus 5–9,15–19; A725 = 0–4,10–14). `run_arm.sh` core map (reader core 2, `--thread-pool 0,1,4,5,6,7`, IRQs 8–13, `taskset 0-7`) would put the RF reader on a **little core** | §4.2 | Medium | rfsim beds run fine as the user | Define a DGX core map (reader + NIC IRQs on X925 cores) and measure drops without RT first (§5.3) |
| K26 | Scripts | **Resolved for the x86 path (A13, 2026-10-01).** `tests/passive_rx/offline_sync_contract/build_and_run.sh` is arch-aware: x86 command lines unchanged (echo-diff empty with a system libgtest; without one it links `lib/libgtest.a` + CPM include), aarch64 branch = `offline_sync_arm.sh` (echo-diff empty). x86 run OfflineSync.* 5/5 `[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 3b6119853f]`. **aarch64 run pending on the DGX.** Pre-existing: x86 flags include `-mgfni` (this cloud CPU lacks GFNI; no effect on the test) | `cloud_run_2026-10-01/a13_sync_contract/README.txt` | Low | — | Run the script on the DGX and compare with `offline_sync_arm.sh` (§25) |
| K27 | Parallelism | Three serial hot threads cap one instance (`UEthread_0` 100 %, `pdcchUssHash` 90 %, `passivePdcch0` 79 % at 273 PRB 1 RX); the blind-PDCCH scan ~~cannot use > 1 consumer~~ — **2026-10-01: N consumers thread-safe (A7 `b6e5fb27ac` + follow-up `71dbfd582a`, §14.5, cloud x86 rfsim only; benefit on the DGX not measured)**; initial sync at 273 PRB × 4 RX takes ~90 s mostly on one thread | §14.1–§14.3 profiles | Medium now, **High for multi-cell** | pin the three threads to X925 cores | §14.3 items 1–5 |
| K28 | Observations / UL | The UL hook of `nr_passive_obs` is compiled and reviewed but **never exercised at runtime**: phy-test rfsim has no PUSCH (`pusch_try = 0`); all 19 k observed records are DL | `a3_obs/README.txt`, `a4_campaign_smoke/` metrics `pusch_try 0` | Medium for UL observation consumers | — | Run an UL-bearing bed (OAI SA with UE traffic, OCUDU) and check `dir:"UL"` records |
| K29 | Observations / DL | In-line DL decode (used when the deferred PDSCH queue is not running, `nr_pdcch_blind_monitor_rt.c` ~6616) is **not recorded** by obs v1; configs without the deferred queue produce no DL records (documented in the header) | `nr_passive_obs.h` | Low–Medium | keep the PDSCH queue on | Hook the in-line path or require the queue |
| K30 | Thread safety (pre-existing) | **Mostly resolved 2026-10-01 by follow-up `71dbfd582a`** (session claude/elegant-davinci-jlrfol): Technique A receive-thread/consumer races fixed (leaf mutex `s_techA_mu`, deferred footprint commit applied by a consumer under `g_phase2_mu`, atomic SS-occasion gate), PDSCH-pool getenv statics and log budgets made atomic, and upstream `common/utils/threadPool/task_ans.c` join counter changed from `relaxed` to `acq_rel` (**a real hazard on aarch64/DGX**, benign on x86 TSO). Partial TSAN rfsim 300 s: 24 → 5 reports, 0 in blind_monitor / pdsch_passive_queue / pdsch_passive_decode / task_ans / Tpool; remaining 5 = rfsim teardown + SIGINT. Original description: Remaining TSAN races: `UEthread_0` <-> `passivePdcchN` in `nr_pdcch_blind_monitor.c` (13 reports: `autodiscover_step` on the receive thread vs `note_rnti_for_windows` / extent advance on a consumer) and inside the PDSCH decode pool (`passivePdsch1/2`, `nr_pdsch_passive_queue_thread`, 4 reports). All exist with **one** consumer too; none is consumer<->consumer. A7 changed diagnostics semantics: DMR | `tests/passive_rx/cloud_run_2026-10-01/a7_followup_races/` | Low (residual) | — | Re-run TSAN on the DGX (aarch64) |
| K31 | Cloud/container host quirks (host-specific) | The cloud container has **no IPv6** (rfsimulator server socket is `AF_INET6` -> `V4SHIM` LD_PRELOAD), **no SCTP** (`nr_cuup_functional_test` fails), **`pthread_getaffinity_np` EINVAL** (`test_thread-pool` fails), 4 cores (frozen conf scan core 5 -> EINVAL assert, so `SCANTHREAD=1:8:-1` auto), timing test `time_management_tests` intermittent. Gate thresholds therefore **re-baselined on cloud: crc >= 93.0 %, drop_full <= 2.5 %** (HEAD min 94.93 / max 1.55) vs **DGX 98 % / 1 %**; a < 2-point CRC regression would be invisible at the cloud gate (mitigated by comparing means vs baseline in each report). Do not apply these workarounds or thresholds on the DGX | `a1_baseline/README.txt` | Low | scripts auto-detect | none on the DGX |
| K32 | PDSCH chest cache | **Resolved 2026-10-01 by F1 + fix round 1 (branch td/convergence-levers)**. Fix: the cache is keyed on the full estimator input (`nr_pdsch_chest_key.h`: full-slot DM-RS mask, 12-bit ports, BWP/refPoint, fo, antenna count, single branch; a segmented estimate is never cached). The estimate always covers every DM-RS symbol of the slot, probes included (a probe still truncates data-symbol FEP/demod). Consequently `chest_time==1` averaging now also runs for probes (intended: probe ≡ full). The cache stores an immutable copy (only the filled rows: Nl×1 when single-branch) plus raw nvar. Each decode works on a working copy: interpolation / chest_time / branch zeroing / SFO rotation / nvar division run per call, and on a single-branch hit the non-planned rows are zeroed exactly as the estimator does, so hit ≡ miss bit for bit. `[MEASURED, DGX rfsim 106 PRB]` `ISAC_TD_PROBE_EQUIV_CHECK` harness: cache-hit arm 1125/1195 grants mismatched → 0/1160; TINTERP probe 1069/1128 → 0/1161; spec arm 0/1162 → 0/1165; 4-RX rank-1 forced branch 2, chest rows 167/167 → 0/340. `[MEASURED, DGX rfsim 106 PRB 1 RX]` regress PASS 99.06/98.64 %. `[MEASURED, DGX rfsim 273 PRB rank-4 pin49r4 4 RX]` crc 100 %. `[MEASURED, DGX rfsim]` probe cost: chest +35 µs (106 PRB, 1 RX); +963 µs chest and +181 µs FEP at rank 4 with 4 RX (~+16 %). Residual (not the chest) moved to K38. Original issue: the key omitted start/number of symbols and the probe horizon, kept 8 of 12 port bits, and the cached estimate was mutated in place | `tests/passive_rx/dgx_host_snapshot_2026-09-30/f1_chest_cache/` | resolved | — | — |
| K33 | Stale credit | No sample-lifetime re-check between decode end and Technique D feedback: a CRC_FAIL from IQ overwritten mid-decode (or on the GPU path) is credited to the hypothesis; only UNSUPPORTED counts as stale | review 2026-10-01, `nr_pdsch_passive_queue.c` ~984-1131 | Medium-high | — | Re-check lifetime before feedback → INCONCLUSIVE; epoch tag (reconfiguration spec §4.4) -- Note: nr_pdsch_passive_decode.c (~2193) already re-checks lifetime after FEP for deferred jobs (check_sample_lifetime); later stages use a private rxdataF/LLR copy, so the new post-decode check is conservative (may mark INCONCLUSIVE when only LDPC overran). **RESOLVED 2026-10-01 (task F2).** `nr_passive_credit_allowed` (reuses `nr_passive_samples_valid`) is re-checked after decode at every TD/layout/data-id/Qm feedback site (deferred queue, incl. GPU path, and in-line decode); stale -> INCONCLUSIVE, counted `pdschq_stale_after_decode`; CRC-OK TB still delivered. `[MEASURED, DGX rfsim 106 PRB 1 RX]` gate PASS x2 (CRC 98.58/98.63 %, drop_full 0.065/0.07 %), counter 0 in both initial runs; in a later re-run (fix round 1, base_r1) it fired naturally, cumulative 2 grants marked INCONCLUSIVE (`[MEASURED, DGX rfsim 106 PRB 1 RX]`, grants not identified, no per-grant log), 0 in the round-2 runs; staleness path not deliberately provoked (no existing knob) |
| K34 | GPU LDPC | CUDA LDPC pool: a silently skipped launch (> 512 CBs) or a CUDA error leaves old bits in a reused pinned slot that the CPU CRC can accept (**false CRC pass**); request queue has no bound check; partial slot reservations can deadlock; errors only printed, no CPU fallback, waits without timeout; discrete-GPU copy model on unified-memory GB10. **2026-10-02 (`td/convergence-levers`, BC9d review):** a GPU false CRC pass would now also CONFIRM a DCI and so feed the BC9d hard TDD/adjacency exclusions (K43) — one more reason to keep CPU LDPC | review 2026-10-01, `nrLDPC_cuda/ldpc_decoder.cu` ~991-1088, `nrLDPC_coding_cuda_decoder.c` ~141-262 | **RESOLVED 2026-10-03 by G1** (`2392d944f3`, `dd1fe1df40`, `312cac9fab`, `54bc0c49e4`; merged `15f2492c6c`; Opus reviews: stale-pass guarantee SOUND, round 2 APPROVED, round 3 diff-checked) | — | **Fix:** per-request status (GPU bits used only when rc == 0; poison 0xA5 as defence in depth); > 512 CBs split; bounded queue (256) → CPU fallback; all-or-nothing slot reservation with rollback; **200 ms** timed waits; a single timeout/error → that TB on the in-plugin CPU decoder (identical to `libldpc`); **N = 4 consecutive** timeouts/errors → 5 s bypass; sticky error / failed warm-up → permanent; capture-overlapping timeouts ignored; **warm-up** pre-captures BG1 Z=384 / BG2 Z=96, batch 1..512, 2×{8,5,10} it; OOB prep-pack write (BG1 Z=384) and queued-timeout slot leak fixed; **`decoder_used`** per TB; metrics `ldpc_cuda_{errors,fallbacks,poisoned,disabled,breaker_trips}`, `ldpc_tb_{cpu,cuda}`. Tests `test_ldpc_cuda_pool` 20/20 `[MEASURED, DGX GB10]`. **Bit-exact BLER table** `[MEASURED, DGX GB10 ldpc_cuda_pool_bler]` (BG1 Z=384 K'=8448 R1/3, QPSK AWGN, 300 random-payload blocks/pt, real CRC, **0 false passes everywhere**; failures/300 at 1.0/1.5/2.0/2.5/3.0 dB): libldpc CPU 8 it 300/300/158/0/0 (= libldpc_orig = in-plugin CPU fallback, block for block); CUDA default (8 → GPU 16 it) 85/0/0/0/0; CUDA at equal GPU iterations (4 → 8) 300/300/36/0/0; CPU 16 it 300/209/0/0/0. **CUDA is ~1 dB more sensitive** (normalised ×3/4 min-sum + 2× iterations vs OAI plain min-sum) ⇒ **CRC evidence is NOT exchangeable across decoders**: every consumer stratifies by `decoder_used` (CB0 elimination: per-batch dominance rule). The round-1 BLER table (no `crcTableInit` in the helper, false passes on aarch64) is **WITHDRAWN**. Residual: a GPU that is always > 200 ms but completes resets the breaker count by late successes (each TB then waits 200 ms → CPU; visible in `ldpc_cuda_fallbacks`); G1 pool is not copy-free on GB10 (pinned + device copies); untested on sm_89; ASan of the plugin not run; live `ISAC_METRICS` export with `_cuda` as the TB decoder not exercised in rfsim |
| K35 | GPU FEP | PDSCH GPU FEP worker copies the IQ ring after queueing with no lifetime re-check (stale-sample decode); self-check is diagnostics only (no tolerance gate) | review 2026-10-01, `nr_pdsch_passive_queue.c` ~654-962, `nr_pdsch_gpu_fep.cu` ~446-457 | High when `NR_GPU_FEP=1` | `NR_GPU_FEP` off (default) | Refcounted IQ or post-copy re-check |
| K36 | LDPC sensitivity | On the DGX (`ldpctest`, BG1 R1/3 K=8448, 8 it, 300 blocks) the CPU decoder (`libldpc`, same for `libldpc_orig`) needs ≈ 2–2.5 dB Eb/N0 (BLER 1.00/0.58/0.00 at 1.5/2.0/3.0 dB) while the CUDA decoder (`_cuda`, 2× iterations, flooding) reaches BLER 0 at 1.5 dB (0.33 at 1.0 dB); BG2 R1/5 same pattern | `cmake_targets/ran_build/build_gpu` runs 2026-10-01; G1 bit-exact harness 2026-10-03 (`task-G1-bler.md`) | Medium (link margin, K4) | — | **Explained 2026-10-03 (G1):** the gap is real and algorithmic, not an artefact: CUDA = normalised (×3/4) flooding min-sum run for 2× the configured iterations; CPU = OAI plain min-sum (not "layered": the `decoder_used` code was renamed `NRLDPC_DECODER_CPU`). At equal GPU/CPU iterations CUDA is still better (36 vs 158 failures/300 at 2.0 dB). Consequence: decoder-stratified CRC evidence (K34). Open: whether an x86 `ldpctest` shows the same CPU numbers (the ARM/SIMDE part of the original hypothesis is untested) and whether the CPU TB decoder should adopt normalisation (link margin) |
| K37 | DCI / CORESET adaptiveness | **Fixed in `1ecf98d162`..`c6fc03be7c`:** capacity is A=140 (K=164 polar), two length contexts/re-lock and bank life cycle/discovery exist; the joint solver remains explicitly <=64 while wider candidates use polar. [OFFLINE VERIFIED, DGX aarch64] unit/replay coverage. **Not live-validated** for commercial wide DCI/reconfiguration | R1–R6, R12, RE reports | High for commercial cells (Milan) | default 140 capacity; `ISAC_RECONF=1` arms adaptive consumers | R13/OTA validate wide/re-lock/moved-bank recovery |
| K38 | Probe ≡ full at Nl>1 | `[MEASURED, DGX rfsim 273 PRB rank-4 pin49r4 4 RX]` with `ISAC_PROBE_ALL=1 ISAC_TD_PROBE_EQUIV_CHECK=2`, the probe's CB0 LLRs differ from a whole-slot decode of the same hypothesis on 68/68 sampled grants (max abs diff 165), while the CB0 CRC verdict agrees (2000/2000); with `ISAC_PROBE_HORIZON=0` 0/23 differ; the chest-row and cache-hit arms are 0/68 | `tests/passive_rx/dgx_host_snapshot_2026-09-30/f1_chest_cache/`, `k38_llr_norm/`, `task-K38-report.md` | **FIXED 2026-10-03** (`c005d19675`, merged `f929120297`) | — | **Root cause `[MEASURED + CODE-READ]`:** not the demodulator but the LLR-norm shift (`ISAC_LLR_NORM`, default on) in `nr_pdsch_passive_decode.c`: the mean absolute LLR was taken over all G, and a probe's zeroed tail (~73–75 % of G) diluted it (probe mean 82–123 vs full 454–456) → probe shift k = 2, full k = 4 → probe CB0 LLRs exactly 4× and ~11 % clipped at the int8 rail (an information difference, not just scale); at rank 1 k = 0 in both (why Nl = 1 showed 0). **Fix:** `nr_llr_norm.h`, shift estimated over ceil(G/C) (E_0 ≤ ceil(G/C), the probe horizon covers it). **Evidence** `[MEASURED, DGX rfsim 273 PRB rank-4 pin49r4 4 RX, host shared]`: PROBE_EQUIV 5/5 → 0/5 (0/48 320 LLRs); committed code 0/3 on PROBE_EQUIV/_CHEST/_HIT; TB CRC 100 % (149/149, 151/151); `test_nr_llr_norm` 5/5 incl. a rank-4-shaped bit-identity check. Rank > 1 CB0 evidence is admissible since (`ISAC_TD_CB0_RANK_MAX=4`). **Residual caveats:** `ISAC_LLR_SCALE` (opt-in) still averages over all G (CB0 marks such grants inadmissible, bit `llr_scale`); GPU-LLR probe paths (`NR_GPU_FEP`) unverified (bit `gpu_llr`); the 106-PRB idle gate and the whole-TB "k changed" census were not run for this fix alone; **Nl > 1 CB0 admissibility has never been exercised at runtime** (the pinned rank-4 bed has no acquiring context) |
| K39 | DM-RS oracle k0 | `[CODE-READ, td/convergence-levers]` the DM-RS oracle records the decoded job's **hypothesised** k0 as observed (`nr_pdsch_passive_queue.c:805` `observe(..., job.sweep_ticket.k0)`), and `obs_admits` (`nr_pdsch_config_sweep.c` ~481-492) hard-prunes every other k0. DM-RS presence in the decoded slot proves only that *some* PDSCH is there: under full-buffer traffic with allocations in adjacent slots both k0 candidates see DM-RS, so the first k0 decoded is kept — the true k0 can be pruned (then no winner until reopen). `[HYPOTHESIS]` (never observed on air); the simulator assumes a perfect k0 oracle. Found by the k0 speed-recovery investigation (`docs/superpowers/specs/2026-10-01-technique-d-k0-speed-recovery-notes.md`) | — | resolved (BC7) | blind-convergence follow-up | record k0 as observed only when the adjacent-slot ambiguity is excluded (DCI-adjacency evidence), else observe mask/last symbol without k0 | **BC7 fix (commits b35c293c55, 68cdd4729d, + round 2):** `observe()` records k0 as a *plausible* mask and prunes on DM-RS mask / last symbol only; the only k0 prune is `nr_pdsch_config_sweep_certify_k0()` (uint64 mask, k0 0..32), scoped to the (configuration, RNTI, TDA row) context and persisted per key in a bounded per-RNTI LRU set (8 entries, so several layout configurations of one RNTI coexist); inherited when a context is recreated; re-applied after observe (type-B layer / restores), `add_k0` layers and the prior-probation catalog restore; cleared on reopen (catalog rebuilt). g_obs carries no k0; the plausible mask is logged (`plaus_k0=` on CONVERGED / ORACLE_RESTORE). `nr_pdsch_config_sweep_rebuild` now clears the PRIOR/FIELD dormant masks when the catalog changes. Covered by unit tests with mutation checks of each binding line (`[MEASURED, DGX unit test]`); `certify_k0` still has no runtime caller (2026-10-02); the runtime k0 removals are the BC9d `exclude_key` exclusions (K43). `ISAC_TD_K0_ORACLE_LEGACY=1` restores the pin (A/B). `[MEASURED, DGX rfsim 106 PRB 1 RX]` (numbers from commit b35c293c55, the default-behaviour commit; rounds 1-2 changed no default path): same winner (tda0 k0=0, 0x804); legacy ttc 0.865/0.797 s, CRC 99.05/99.02 %; default ttc 2.538/1.962 s, CRC 97.33/97.64 % (**the default FAILS rfsim_regress.sh 2 on the 98 % CRC gate; legacy PASSES**); seg fails 461/478 legacy vs 1151/1021 default. `[HYPOTHESIS]` (review analysis, not separately measured): about half of the extra failures is the tda0 slow-down (125 vs 80 trials on the winner) and about half the mixed-slot tda2 context, which converges 7-8 s later, plus `ldpc_zero_tb` 277/251 vs 0 from k0=1 hypotheses decoding UL slot 8. `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` crc 100 % (3036/3036). Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/bc7_k39/`. ~~`[HYPOTHESIS]` the gate failure is recovered by BC9~~ **REFUTED / resolved differently (2026-10-02):** BC9 is neutral on the phy-test bed because it broadcasts no SIB1 (`tdd_known=0`, `adj_rows=0`; idle A/B §14.8), so the K39-default search phase still fails the legacy 98 % criterion; the operator replaced the gate by the post-convergence gate (option a, §12), which passes ×2 at `4db43db6f5` and `ca1cbc5470` `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]`. The 98 % statements in this row are about `GATE_MODE=legacy`. **4-RX cost (2026-10-04, task CB0WIRE round 2)** `[MEASURED, DGX rfsim 106 PRB 4 RX, 420 s, load < 4, interleaved]`: levers with all flags OFF: ttc tda0 56–109 s / tda2 139–143 s vs main 14 / 79–87 s; the pre-acceleration state `5cc9d518e9` is equally slow; **`ISAC_TD_K0_ORACLE_LEGACY=1` restores main** (tda0 15.8 s, tda2 81.3 s, `ldpc_zero_tb` 0 vs 300–500, tda0 winner trials 94 vs 190–222). `[HYPOTHESIS]` the larger 4-RX factor comes from the slower 4-RX search rate per grant. **K39 stays the default (correctness change)**; the CB0 elimination channel (ON by default on the merged main) recovers the cost: tda0 15–24 s, tda2 42–43 s (CPU backend; §14.10).
| K40 | TDD slot direction (PDCCH gating, BC9 PDSCH exclusion) | `[CODE-READ, td/convergence-levers @0676c372b6]` `nr_tdd_pattern.c` placed the nrofUplinkSlots right after the mixed slot and called every remaining slot of the period DL; TS 38.213 11.1 puts the nrofDownlinkSlots FIRST, the nrofUplinkSlots LAST, nrofDownlinkSymbols at the start of the slot after the last DL slot, nrofUplinkSymbols at the end of the slot before the first UL slot, and everything between FLEXIBLE (flexible symbols can carry PDCCH and a DCI-scheduled PDSCH). Whenever dl_slots + mixed + ul_slots < period the old model marked flexible slots as UL. Impact: with `ISAC_TDD_SKIP=1` (`nr_pdcch_blind_monitor_rt.c` TDD skip) and in `nr_pdcch_coreset_map.c`, PDCCH in those flexible slots was NOT scanned (missed grants), and the BC9 TDD k0 exclusion (destructive prune) would have removed legal k0 values. `[HYPOTHESIS]` not observed: the rfsim cell's 7D1S2U fills its period (old and new models agree) and the phy-test beds carry no SIB1; earlier OTA cells' patterns were not re-checked | — | resolved (BC9 fix round 1) | TDD config / PDCCH gating | per-slot shape (leading DL / trailing UL symbols) from the spec placement; FLEXIBLE slot direction; `nr_tdd_slot_has_downlink` = not a full UL slot; `nr_tdd_pdsch_last_symbol` = 13 - trailing UL symbols; stricter `nr_tdd_config_init` validation (symbols must fit their slots); pattern2 concatenated as before | **Fixed (BC9 fix round 1):** `nr_tdd_pattern.c` slot_shape(); `nr_passive_acq_tdd_slot_has_downlink` also copies the pattern under the lock. Covered by `[MEASURED, DGX unit test]` TddSpec.* (10-slot DDDFFFFFUU, DDDSU, 7D1S2U, mixed symbols in different slots, symbol fit, two patterns with flexible slots) and AcqStateTdd.FlexibleSlotsAreMonitored. Not exercised on air or in rfsim (phy-test beds carry no SIB1). Open: the PDCCH gate does not check the SIB1 reference SCS against the receiver numerology (the BC9 PDSCH exclusion does) |
| K41 | DM-RS mask oracle attribution | `[CODE-READ, td/convergence-levers]` the DM-RS oracle measures the DCI's own slot on the grant's PRBs with this RNTI's nscid/dlDmrsScramblingId (`nr_pdsch_passive_queue.c` ~886-911 @b3c531b221+BC7b follow-up); DM-RS is cell-scrambled, so **another UE's PDSCH** on those PRBs is read as this RNTI's mask. `nr_pdsch_config_sweep_observe` records it and `prune_to_observed` (`nr_pdsch_config_sweep.c` `prune_to_observed_from` ~659) keeps only entries admitted by `r->obs ∪ g_obs` (with `prune_commit` wiping evidence). Runtime protections: union prune (a later true-mask observation widens the set), `restore_observed_typea` (`restore_observed_typea` ~2011, called ~2114, type A only, catalogue k0, evidence lost), `g_obs` promotion by a second RNTI (~2090-2097; since BC7b the union of every holder's last-symbol set). **The truth is lost persistently only if** the RNTI never measures its own mask (k0 ≥ 1 truth with no own same-PRB PDSCH in the DCI slot) **and** the true mask is never promoted to `g_obs`, or the truth is type B. `[SIMULATED, DGX host, nr_td_sim @b761021bb1]` without the union/restore protections: 82–88 / 2000 RNTIs undecidable at other-UE occupancy 0.1, 214 at 0.3 (upper bounds; BC8 round 2 models the protections). Not fixed; motivates accumulated soft oracles (blind addendum §5, lever S). **See K42:** with the round-2 protections the foreign mask's different last symbol drove a relax/re-refine thrash that wiped all evidence on every observation (1072/1072 k0 = 1 truths undecidable); BC7b made the last-symbol sets monotone, which removes the thrash but not the attribution error itself (a foreign mask still widens the admitted set and can prune a truth on a context's FIRST observation) | `tests/passive_rx/td_sim/baseline_bc0_2026-10-01.txt` (BC8 v2) | open | blind-convergence follow-up (lever S) | soft accumulated mask evidence instead of single-shot hard prune; at minimum never prune on a mask observed in a slot where another RNTI's grant overlaps (DCI history) |
| K42 | DM-RS oracle observation thrash | `[CODE-READ, td/convergence-levers @8af53c3a40]` `obs_record` relaxed a mask's last symbol to unknown on a contradicting observation and re-refined it on the next one; `restore_observed_typea` appended entries for the new (mask, last) that the following refine-prune removed again, and every size-changing `prune_commit` zeroed ALL CRC/probe/lever evidence while `context_reindexed` staled every in-flight ticket. Triggers: another UE's DM-RS on the grant's PRBs (K41) with a different last symbol, the RNTI's own PDSCH from other TDRA rows sharing a mask, energy-threshold last_sym jitter. Further wipe routes: a 9th mask (full `r->obs`) restored then pruned; BC9 `apply_cert` after a restore; and (found in BC7b) the type-B layer re-appended and re-pruned on every observation once `typeb_seen` | `[SIMULATED, DGX host, nr_td_sim @f59112743e]` (BC8 R2) 1072/1072 k0 = 1 truths undecidable with other-UE 0.1/0.3. After the fix `[SIMULATED, DGX host, nr_td_sim @44a9391975]` (fixed oracle, other-UE 0.1/0.3, --acq 500): adjacency 0: k0 = 1 undecidable 1072 -> 56/58 (rx4), 118/133 (rx1); default traffic: 1072 -> 0; wrong 0 everywhere; k0 = 0 truths unchanged to the digit. COST: k0 = 1 truths now decide but slowly (mean 413-682 s at default traffic, 540-933 s at adjacency 0; p95 up to ~2550 s), end catalogue ~256-343 hypotheses vs ~120-148 (a foreign mask now widens the admitted set for good). Legacy (k0-pin) arms dropped by controller decision (they only re-document K39). `[MEASURED, DGX unit test]` 9 PdschConfigSweepK42 tests (RED on 8af53c3a40, mutation-checked). `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]` the bed shows one last symbol per mask (F1 inert there): rfsim_regress.sh 2 FAIL under the then-current legacy 98 % gate (CRC 94.63 / 95.88; no K39-default run passed the legacy gate since BC7) — re-scored with the postconv gate (§12) all 13 BC7b runs PASS; interleaved A/B pre/fix 4+4: CRC median 97.14 / 96.95 (p = 0.49), tda0 ttc 2.95 / 3.21 s (p = 0.20); the same kind of restores ((0x4, last 5) only), matching reindex/stale counts, a 24-hypothesis tda0 search and the same outcome rate in both; no wipe of the tda0 context before convergence in either. `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` 100 % (3011/3011). Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/bc7b_k42/`, baseline file `## BC7b` | resolved (BC7b: engine 7b5c51b115, simulator 44a9391975) | F1 `obs_set_t.lastset` (uint16, a bit per S+L-1, 0 = unknown) in `r->obs` and `g_obs`: `obs_record` ORs and never re-refines; `obs_admits` passes iff the set is empty or holds S+L-1; the g_obs promotion copies the set; the same in legacy k0-pin mode (the k0 pin is unchanged). F2: no `restore_observed_typea` when the full own set dropped the mask and g_obs lacks it. F3: entries appended by the same observe call (restore, type-B layer) that the observe-path prune or `apply_cert` removes again are truncated (`prune_commit_tail`: no wipe, no reindex); restored entries are filtered by `cert_admits` (so `ORACLE_RESTORE added=` now counts only cert-admitted candidates, and a restore whose candidates are all filtered adds nothing and is not logged). No evidence remapping; a genuine narrowing (first observation, Qm/prior prunes, certification of existing entries) still wipes. **Follow-up (BC7b fix round, Opus review):** I1 `add_k0` binds a new k >= 2 layer with `apply_cert_from(c, n0)` (a certified / BC9-excluded context truncates a partly or wholly excluded layer instead of wiping, also on repeated probe hits); M1 an observe-path type-B layer truncated in full is latched per context against the observed-set signature (no append/truncate loop until a set changes, a reopen or a rebuild; `nr_pdsch_config_sweep_typeb_stats`); M3 g_obs promotion ORs every holder's set. Simulator `--obs-lastset 1` (default; 0 = round-2 model) | (a) K41 attribution itself is NOT fixed: a foreign mask still widens the set (slower convergence) and can prune a truth at a context's first observation; (b) `[MEASURED, DGX rfsim 106 PRB 1 RX]` the fix reached the earliest tda0 separation checkpoint in 0/9 runs vs 6/14 pre-fix (p = 0.048, post hoc, several looks); no engine mechanism found: re-measure with a larger interleaved A/B; (c) resolved by the follow-up (I1) |
| K43 | BC9 hard exclusions from unconfirmed DCIs | `[CODE-READ, td/convergence-levers @f800be8b22]` (BC9 simulator review) the BC9 accept hook (`nr_pdcch_blind_monitor_rt.c` `bc9_dci_accept`) applied the SIB1 TDD per-hypothesis exclusion and the DCI-adjacency exclusions for ANY accepted C-RNTI DCI 1_1, false accepts included, and adjacency used unconfirmed neighbours: **a spurious DCI could HARD-exclude the truth** of its (RNTI, configuration, TDRA row) context until reopen (UL slot or past a mixed slot's DL symbols at s + k0_true; or a spurious neighbour making k0_true "occupied"). Live on SIB1 cells (ISAC_TD_DCI_ADJ default on); inert on the phy-test rfsim beds (no SIB1). `[SIMULATED, DGX host, nr_td_sim @852b9bb86f]` hazard arm `--excl-unconfirmed 1`, DDDSU, 4 RX, dci-false 1e-2, 20 RNTIs: truth pruned on 19/20 RNTIs, 19 undecidable; confirmed rule: 0/20, undecidable 0 (= the dci-false 0 baseline at the 3600 s cap). `[MEASURED, DGX unit test]` DciBc9d.SpuriousDciNeverExcludesTruth RED before the fix (row-0 k0 = 1 truth removed by a spurious row-0 neighbour of a confirmed certified row) | resolved (BC9d, 852b9bb86f) | **Fix (confirmed-DCI rule):** a DCI is CONFIRMED when the grant it scheduled passes TB CRC-24 on any hypothesis (deferred consumer, not for first-code-block layout probes, and the in-line path); history entries carry `confirmed`, set by (rnti, abs slot, cfg, tda). The accept hook only records; `nr_pdsch_passive_bc9_confirm` -> `nr_dci_hist_on_confirm` applies the DCI's TDD exclusion (M5 cache kept, predicate unchanged) and the adjacency exclusions against confirmed neighbours only, after the KL feedback, once per DCI (TDD also on a missed lookup). Row certifications therefore come from confirmed DCIs only. The certified FLAG (levers C/P, off by default) keeps observed occupants (residual `certified_wrong` = missed real compatible occupant + spurious incompatible one); `ISAC_TD_CERT_CONFIRMED=1` (default 0) counts confirmed occupants only (sim: certified_wrong -> 0). `ISAC_TD_DCI_ADJ=0` still disables all. `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]` rfsim_regress.sh 2 (postconv) PASS x2 (postconv CRC 100.00/100.00, ttc0 2.16/1.98 s, ttc2 18.8/22.8 s, floor 97.65/97.63), 56-58k confirms, missed_lookup 0; `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` 100 % (3007/3007). Evidence `tests/passive_rx/dgx_host_snapshot_2026-09-30/bc9d_confirmed/`, baseline `## BC9d` | Cost: a DCI excludes only after one of its decodes passed (slower early pruning: `[SIMULATED, DGX host, nr_td_sim @852b9bb86f]` 1200 RNTIs, DDDSU 4 RX: mean +0.5 % at rho 0.5, +0.9 % at rho 0.9 vs the unconfirmed rule at dci-false 0, while at dci-false 1e-2 the unconfirmed rule prunes the truth on 1096/1200 RNTIs (48 wrong, 1048 undecidable) and the confirmed rule on 0; `## BC9d`). `ISAC_TD_CERT_CONFIRMED=1` costs every lever event in the simulator (keep 0); adjacency needs both DCIs of a pair confirmed. Not exercised on air yet (first SIB1 cell: X410 OTA 2026-10-03) Note (BC9d review): with `ISAC_PROBE_ALL=1` grants are decoded as first-code-block probes only, which never confirm a DCI, so BC9d exclusions become inert; the analytical spurious-confirmation residual (~1e-6 per spurious DCI) is `[HYPOTHESIS]` |
| K44 | Field book k0 hole (BC6 investigation A2) | `[SIMULATED, DGX host, nr_td_sim @c826d0e4ae]` + `[CODE-READ]`: the field book's TDRA value packs k0 (`nr_td_pack_tdra(S,L,mapping,k0)`), so a wrongly promoted TDRA that differs from the truth **only in k0** makes the truth DORMANT while its trap-passing k0 sibling stays active; KL acceptance has no sibling guard and accepts the sibling; the sibling's passes reset `since_pass`, so fail-open never fires; a pruned field casts no vote, so no contradiction is recorded. BC6 `fb2_inject`: 840–1980 / 2000 RNTIs wrong, recovery never in 404–497 / 500 acquisitions. Generic class: any wrong removal of the truth while a trap-passing k0 sibling lives — in the runtime only through k0-specific hard rules (K39 cert, BC9/BC9d exclusions), which are confirmed-DCI-gated (K43) | `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/task-BC6-investigation.md`, `tests/passive_rx/td_sim/results_2026-10-02_bc/` | **RESOLVED by BC6b** (`2588a83cc9`/`ee5a805423`) `[SIMULATED, DGX host, nr_td_sim @2588a83cc9]`: fb2_inject wrong 840–1980 → 0, recovery-never 404–497 → 0 of 500. **Simulator-validated only; the runtime wiring (R2) does not exist yet** (no runtime caller of the field book) | field book stays unwired until R2 (operator decision; keep the k0 rule) | **BC6b (landed `2588a83cc9`):** (a) TDRA dormancy on (S, L, mapping) only, k0 removed only by certified/confirmed evidence; (a2) a winner differing from the promoted TDRA only in k0 counts as a contradiction of the k0 part; (b) race-based alternative rejected; plus simulator fixes A1/A3 and re-run of the invalid BC6 cells and fb2 arms **2026-10-04:** the fb2 runtime wiring now exists (R2fb, `ISAC_TD_FIELDBOOK=2`, ON by default on the merged main) and keeps the k0 rule (`PdschFieldBook.TdraPruneNeverRemovesK0`); runtime promotion has not been exercised (single-UE bed) |
| K45 | 4-RX `drop_full` (PDCCH scan queue) | `[MEASURED, DGX rfsim 106 PRB 4 RX, 420 s]` `drop_full` (= PDCCH **scan-queue** drops / queued; the PDSCH queue reported `dropped[full=0]` in every arm) exceeds the 1-RX 1 % limit in many 4-RX arms: **main all-off 1.20 / 0.79 %**, levers OFF 0.49–1.62 %, CB0 ON 0.85–1.71 %, GPU ON 1.50 %. It keeps growing after both contexts converge (CB0 idle), and CB0 adds no extra drops in its active window → pre-existing scan-thread capacity limit (K27), not caused by the levers | `task-CB0WIRE-report.md` round 2 §2 | Medium (4-RX real-time budget; OTA at 4 × 100 MHz will be harder) | provisional 4-RX gate `GATE_DROP_MAX=2.5` (§12) | **Open.** K27 work: DGX core map (`COREMAP=1`), a second scan consumer (`SCANTHREAD="2:16:-1"`), split `UEthread_0` / `pdcchUssHash` (§14.3) |
| K46 | CB0 TBS twins | `[MEASURED, DGX GB10, test_nr_td_cb0_batch / CudaLdpcPoolPath]` a wrong TBS of **+48 bits at C = 11** keeps K, Z and E (K′ 4 bits longer): the decoded CB0 is the true codeword and its CRC24B over 4 more zero filler bits still checks (a CRC codeword followed by zeros is a codeword). CPU and CUDA both pass it (9/9) | `task-BATCH-report.md` round 2 | Low (correctness kept: such twins are never eliminated, the full-TB KL rule must separate them) | — | **Open (by design):** CB0 elimination can never remove these TBS twins; they cost full-TB trials. Consider a cheap second-CB or full-TB check for twin families if they dominate the search OTA |
| K47 | CB0 items per grant | `[MEASURED, DGX rfsim 106 PRB 4 RX]` mean **~3 CB0 items per grant** (2.6–3.2 CPU, 3.2 GPU) instead of the scheduler's B = 96 (simulator assumes 64–128): one GrantWork = one PDSCH slot, so only hypotheses with the job's k0 are testable (`td_cb0_not_testable` ≈ half the members), and ≤ 5 geometries fit its 16 signatures | `task-CB0WIRE-report.md` round 1 §6.1, round 2 §3 | Medium (the next speed lever; it also keeps the GPU latency-bound) | — | **Open (deferred):** multi-k0 GrantWork (a second GrantWork on the s + k0′ slot), lifting the 16-signature / 5-geometry cap (§25) |
| K48 | fb2 epoch bump on SOFT gaps | `[CODE-READ]` `nr_pdsch_config_sweep_fieldbook_bump_epoch()` is called only at the RX-stream discontinuity edge in `executables/nr-ue.c` (next to `nr_passive_acq_note_sync_loss`), so **every** sync-invalidating gap bumps the field-book epoch, including SOFT gaps < 10 ms that the operator rule (2026-10-02) says must not invalidate evidence | `task-R2fb-report.md` (review Important 2, fix round 1 comment in `nr-ue.c`) | Low–Medium (a bump only turns promoted values into hints needing 2 new supporters: slower, never wrong) | — | **Open:** robustness plan R7 must **replace** that call site with the SOFT/HARD (≥ 10 ms HARD_REVERIFY) classification, not add a second bump |
| K49 | rank-4 pin49r4 grant count | `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` the pinned rank-4 bed now decodes only ~150–220 grants per 150 s and **831–931 per 420 s on main and on the branch** (CRC 100 %, first C-RNTI 44–52 s), vs ~3000 per 150 s recorded until 2026-10-02 (§14.8) | `task-CB0WIRE-report.md` round 1 §4, round 2 §1 | Low (CRC unaffected; reduces the statistical power of rank-4 checks) | — | **Open, unexplained** (`[HYPOTHESIS]` host-related: other agents' load / docker OAI bed). Re-run on an idle host before quoting rank-4 counts |
| K50 | Review status of the acceleration round | Per the ledger: the **ELIM fix round 1** (`2c51f9c4e4`) was merged without its Opus re-review (usage limit; recorded as "TODO before runtime enable"); **CB0WIRE** and **CB0GPU** have no independent review recorded (controller diff checks and the agents' own tests only). The merged-main defaults turn all three ON | `progress.md` | Medium (process: unreviewed code on by default) | each ON default can be disabled with `=0` (§10.2) | **Open:** review ELIM fix round 1, the CB0 wiring (admissibility bits, adapter re-validation, `note_tb_decoder` via empty grants) and the GPU CB0 entry before the first OTA claims |

---

[OFFLINE VERIFIED, DGX aarch64, 2026-10-02, rr/reconfig-robustness] R6b's review of R3–R6 fixes is recorded in
[the R6b report](docs/superpowers/reports/rr/R6b.md): required build passed, ctest 122/131 (only documented ARM/sandbox
failures), touched binaries passed shuffle seeds 1/3/5. [IMPLEMENTED, NOT VALIDATED on live traffic] Bank health uses
one producer-slot clock; idle occasions do not accumulate length misses; only changed lengths reopen Technique D;
second-length fallback exclusion/deduplication, occupancy persistence and occasion-based scouts are corrected.
The report distinguishes source-reviewed integration changes and rejected subfindings from tested state transitions.

[OFFLINE VERIFIED, DGX aarch64, 2026-10-02, rr/reconfig-robustness] R8b corrects R7/R8 review findings:
post-boundary SIB1 comparison with a five-second fallback grace, strict short-only P-RNTI evidence, startup
cache hints, deferred listener dispatch, bounded SIB1 monitoring, identity frequency rounding, and in-flight
result epoch checks. Required targets built; full ctest 125/134 (documented ARM/sandbox failures only).
[IMPLEMENTED, NOT VALIDATED on live traffic] RRC skips full configuration for unchanged periodic SIB1;
GPU result guards and stable-cell false-trigger targets await orchestrated validation. Per-finding evidence,
shuffle results and exact deferred commands: [R8b report](docs/superpowers/reports/rr/R8b.md).

[OFFLINE VERIFIED, DGX aarch64, 2026-10-02, rr/reconfig-robustness] R2d bounds exhausted
DCI sweeps to 30..63 plus one low-duty wide probe (140-bit capacity remains default).
`ISAC_DCI_WIDE_PROBE_EVERY` defaults to 1 with a stride-aware <=5% work-count bound;
`dci_wide_probes` is added to the blind summary. Required builds passed; full CTest
127/135 (only documented ARM/sandbox failures), touched shuffle seeds 1/3/5 passed.
[IMPLEMENTED, NOT VALIDATED on live traffic/GPU] The orchestrator's measured Phase-1
regression and exact deferred CPU/GPU commands are in [R2d](docs/superpowers/reports/rr/R2d.md).

[OFFLINE VERIFIED, DGX aarch64, 2026-10-04, rr/integration] RI merges levers main
`4d98184411` with robustness `7703e9cf02`. Required CPU targets build; focused epoch/metrics
registrations pass, including stale CB0/GPU-backend completion, fieldbook catalogue allocation,
RELOCK reset and BC9 history. The union metrics schema needs an 8192-byte emitter/CSI-RS fixture
buffer. Full CTest completed 140/150; the CSI-RS fixture repair passed separately, leaving
only four known ARM and five sandbox socket failures. Shuffle seeds 1/3/5 passed 111/111
invocations (3255 cases passed, 45 documented skips). [IMPLEMENTED, NOT VALIDATED on live traffic/GPU]
GrantWork and CB0 plans retain the
admitting epoch; guarded sweep/fieldbook/BC9 consumers reject stale results and use the owner's
once-only dropped-epoch count. Main defaults are retained; R10/R11 and K48 remain deferred.
Full test/shuffle results, conflict decisions and exact flag-off/on 4-RX gates:
[RI report](docs/superpowers/reports/rr/RI.md).

[CODE-READ; OFFLINE VERIFIED, DGX aarch64, 2026-10-04, rr/w2-uectx] R17b corrects
UeContext's trusted-only reconfiguration inference, invalid-slot epoch notes,
DCI-SUSPECT epoch wiring, mixed 0_0/0_1 and CSS configuration flapping, and DCI-only
UE aging. The writer's shutdown handshake now uses sequential consistency; active
RNTIs use a direct index and aging scans run once per second. CORESET packing is
shared between accept and removal paths. A C-writer JSON fixture is parsed by the
offline report test, which labels CSI-RS evidence as coincident with the request
slot. Live false-trigger and drop-rate behavior remains unmeasured. Test results
and deferred 4-RX commands: [R17b report](docs/superpowers/reports/rr/R17b.md).

## 25. Next steps on the DGX Spark (strict order)

**Progress 2026-09-30:** steps 1–4 **done** (§4.2). Step 5: OAI build done (UHD 4.11 installed; X410 compat still to
check at step 8). Step 6: done — 123/126, the 3 failures triaged (K21, K22 open, upstream modulation test), shuffle /
python / idsweep CPU+GPU / sync contract all pass (§13.1). Step 7: done at 106 and 273 PRB 1 RX — matches or beats
sens6 (§14.1); 4-RX rank-4 bed did not sync (§14.2, re-test with `pin49r4`). Steps 8–10: **blocked, no X410
connected**; step 10 changes to the Milan-cell survey (§15.0, §15.4) because the lab testbed is gone. Step 12
**blocked by K23** (branch not on GitHub). New before step 8: define the DGX core map (§14.3) in the launcher.

1. Clone the repository (`adaptive-rx-UL-DL`), `git log -3`, confirm this file is present.
2. Read `PROJECT_MEMORY.md` completely.
3. Record the DGX hardware/software environment (§5.2) into a new §4-style table in this file.
4. Compare against sens6 (§4): list every difference that touches §5.3.
5. Install/verify UHD matching the new X410 (probe first); configure a fresh OAI build (§9); build
   `nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests`.
6. Run all offline validation gates (§14). Fix ARM portability failures before anything else.
7. Run the phy-test rfsim smoke (§10.4) and compare CPU/drop metrics with sens6 (§14 baseline).
8. Connect and characterize the new X410 (§7): addresses, serial, FPGA image, MPM/UHD versions, NIC, MTU, rings,
   temperatures, per-channel noise floor and antenna power; update §6.3 with the new channel map.
9. Validate RF streaming (G0): `benchmark_rate` 300 s all channels, then a 600 s receiver capture without RFSTALL.
10. Repeat the latest known OTA milestones (§15.4) on the lab cell (if available), then on Salt; n ≥ 5 runs/arm.
11. Confirm results reproduce on the new host (or explain the difference) **before** any development.
12. Triage and merge the receiver fixes from `feature/multirx-clean-adaptive` (K2), each with its test.
13. Continue unfinished work in gate order: G8 OTA decode rate (K4), NSA/SIB1-less (G5B, K7), CORESET generality (K8),
    CSI-RS G4 (K9), UL TP/HARQ (K14).
14. Implement persistent tracking and the state machine (§19; G10–G11), starting with the unified epoch (K10).
15. Implement multi-cell (G12), then multi-carrier (G13), then multi-operator (G14) per §17–§20 — only after the
    operator approves those designs.

**Progress 2026-10-01 (cloud Track-A session, x86 container, final fix `6528bd0cfc`..`343d1f062a` (merged with the base in `d6cb580ce5`)):** done in the cloud scope — **A0**
(CLAUDE.md, venv; `24f1a18b43`), **A1** (rfsim gate + scorer, HEAD baseline x3; `a5140ce43c`, `456be54aa7`, `2d1bb6c18e`),
**A2** (`ISAC_METRICS`; `4cdb890759`..`6e9a96c476`), **A3** (obs API; `10cc6f0058`..`385e02b9cf`), **A4** (campaign; `76e733251d`,
`dac7052c2e`, smoke `8be4711650`), **A5** (health tab; `0d04761969`, `cf8e2e79d7`), **A6 code + dry-run** (`876c8ce5b9`),
**A7 TSAN + rfsim A/B** (`b6e5fb27ac`, `1d6cbdf5c3`), **A11 code + default regression** (`843e5cff49`), **A13 x86 path**
(`a1a06e1f88`, `3b6119853f`, `2547ff90aa`). All cloud x86 evidence, §13.2/§14.5. **DGX follow-ups, in this order:**
1. **A6 Step 5** — run `run_rx_dgx.sh` / `COREMAP=1` for real on the DGX, measure the X925 core map (§14.3) vs unpinned.
2. **A8, A9, A10** — as specified in `docs/superpowers/plans/2026-10-01-dgx-next-steps.md` (skipped, DGX-only).
3. **A11 timing** — measure initial sync at 273 PRB x 4 RX with `ISAC_SCAN_SCRATCH_MB` 512 vs larger.
4. **A12** (DGX-only).
5. **A13 aarch64 check** — run `offline_sync_contract/build_and_run.sh` on the DGX (expect OfflineSync.* 5/5).
6. **A7 273-PRB pinned A/B** — `"1:8:6"` vs `"2:16:6"` (cloud had only 106 PRB, unpinned, 4 cores).
7. **Concurrency follow-ups (K30):** fix the remaining TSAN races UEthread_0 ↔ scan consumers in `nr_pdcch_blind_monitor.c` (discovery state written by `autodiscover_step` without the Phase-2 lock) and inside the PDSCH decode pool; one TSAN run that also instruments the PDSCH sweep/queue sources; launcher check for INSTANCE=A with cluster-1 pins.
8. Re-run the gate with the DGX thresholds (98 / 1; = `GATE_MODE=legacy` since 2026-10-02 on `td/convergence-levers`, §12) and a campaign on the DGX; then **Track B** (§25 steps 8 onward, X410/OTA).

**Progress 2026-10-01 (DGX, after merging the cloud work):** done on the DGX — follow-up 5 (**A13 aarch64** 5/5),
follow-up 7 (**K30 races**: merged `71dbfd582a`; re-run TSAN on aarch64 still advisable), follow-up 8 first half
(**DGX gate** PASS, §14.7), A7 two consumers at 106 PRB unpinned (§14.7), A9 Step 1–2 (GPU build + tests, K17).
**Next, in this order:**
1. ~~Levers plan (§0.6) **pure tasks 1–7, 4b, 4c** (any time) and **F1 (K32) + F2 (K33)** — correctness first.~~ **DONE 2026-10-02 on `td/convergence-levers`** (plus the blind-convergence plan), see the 2026-10-02 block below.
2. Operator review of the reconfiguration-robustness spec → write its plan (K37: the 63-bit DCI cap must be lifted
   before the Milan dedicated path, Track B4).
3. K36: run `ldpctest` identically on x86 to decide whether the CPU-vs-CUDA LDPC gap is ARM-specific.
4. Remaining DGX-only follow-ups: A6 Step 5 (core map measured), A7 273-PRB pinned A/B, A8 (USS GPU, after G1 K34),
   A10, A11 timing, A12 (K22), TSAN on aarch64.
5. X410 / Track B as soon as the radio is on site (§15.4 DGX/Milan procedure).

**Progress / next steps 2026-10-02 (DGX, branch `td/convergence-levers`, NOT pushed, NOT merged):**

1. **Merge `td/convergence-levers` into `adaptive-rx-UL-DL` — awaiting operator approval.** Do not push or merge
   without it. BC6b is **landed and reviewed** (`2588a83cc9`/`ee5a805423`, APPROVED). Still owed before merging: full ctest (known ARM set; `nr_cuup_functional_test` fails on the DGX while the unrelated docker OAI SA bed holds GTP-U port 2152, environment-only), a **final postconv gate ×2 on the merged HEAD `6e3d6c9a9a` or later, on a quiet host** (the docker bed is loading the host), rank-4 bed, sens6 freeze check. The merge awaits operator approval.
2. **X410 OTA from 2026-10-03** (first air run of K39–K43 and BC12a; first SIB1 cell for BC9). Run the §15.4 DGX/Milan
   procedure first (G0 → survey → G5A). Then, per run, with `ISAC_METRICS_PATH` set, read and record:
   - **BC12a census** (reading guide, task-BC12a report): on every `Technique D CONVERGED` line the tail
     `k0= map= dci= sib1_row= deftab=`; `SENSING: TDA-COMMON from SIB1` (list received); metrics `td_sib1_tdra_*`,
     `td_deftab_*` (last line = totals). `sib1_row=match` supports BC12b; `mismatch` = SIB1 pruning would remove the
     truth; `none` = no evidence. `deftab` only meaningful when `sib1_row=none` and the MIB was decoded. Only DCI 1_1
     contexts exist (ignore `_10` keys). Count matches **per k0**. Per cell, never merged.
   - **`BC9 DCICONF`**: `tdd_known` (must be 1 on a SIB1 cell), `tdd_dcis`, `tdd_lock_calls`, `missed_lookup` (≈ 0;
     read together with host load / `scanq drop_full`), `adj_rows/adj_removed/adj_refused`, `excl_removed[k0<2]`,
     `excl_removed[k0>=2]`, `excl_refused`; TDD exclusion counts per run.
   - **Wrong-k0 alarm**: any `BC9 DCIADJ_CERT ALARM` line (expected 0); `DCIADJ_CERT` f_S and certified passes per k0.
   - **Per-context ttc**: `score_rx.py` `ttc_by_tda`, `contexts`, `reopens`, `postconv_crc_pct` on the OTA log.
   - **Evidence restarts vs DCI phases**: read the CONVERGED suffix `excl_restarts=N excl_truncs=T dci_phases=M` (expected `excl_restarts <= dci_phases`, BC6 investigation A1; `[HYPOTHESIS]` until measured OTA), any `SENSING: TD_EXCL_RESTART_ALARM` line (rate-limited, once per context; a young context counts phases only from its creation, so a spurious alarm is possible) and metrics `td_excl_restarts`, `td_excl_truncs`, `td_excl_restart_alarms`; look for repeated `ORACLE_RESTORE` / `TYPEB_LATCH` on one context (K41/K42 symptoms).
   - `pdschq_stale_after_decode` and `ldpc_zero_tb`; rank / RX count per arm. n ≥ 5 runs per arm.
   Optional A/B arms: `ISAC_TD_DCI_ADJ=0` (BC9 off) and a SIB1-less arm (SIB1 ignored) — never
   `ISAC_TD_K0_ORACLE_LEGACY=1` as a default.
3. **BC6b: landed and reviewed** (`2588a83cc9` code, `ee5a805423` evidence; APPROVED, minor items only). K44 resolved, simulator-validated only (§14.9/K44). Decision: fb2 conditionally recommended for R2 (operator decision, k0 rule kept); blind cold target not met (301 s / 435 s `[SIMULATED, DGX host, nr_td_sim @2588a83cc9]`); C/P and fb2+C/P not recommended (BC9c open). No runtime wiring exists yet.
4. **Gate option (b)**: an SA bed that broadcasts SIB1 with several UEs (OAI SA rfsim + open5gs on sensnuc3, §10.4),
   so TDD/adjacency exclusions and the census are exercised end-to-end before the next change to them.
5. **Deferred (operator 2026-10-02):** BC9c (count k0-sibling-test trials only on certified grants — the open lever
   for C/P under persistent traffic), BC10 (dedicated fast-path stream), BC11 (adaptive anytime k0-neighbour test),
   BC12b (SIB1 TDRA as a reversible pruning cause, only after BC12a OTA evidence), lever S (accumulated soft oracles,
   K41), levers plan R2 (runtime wiring of gate/ordering/field book/K; bit-identity regression with explicit
   gate off), F3, G1–G4. Not recommended for enablement now: field book (recovery FAIL), C/P (inert), E (biased).
6. **Reconfiguration robustness** plan in its own worktree (`rr/reconfig-robustness`, `/home/nicola/NICOLA/wt/rr-robust`),
   Phase 1 first; K37 (63-bit DCI cap) must be lifted before the Milan dedicated path.
7. Earlier DGX follow-ups above (A6 Step 5, A7 273-PRB pinned A/B, A8, A10, A11 timing, A12, K36 on x86, TSAN on
   aarch64) remain open.

**Progress / next steps 2026-10-04 (DGX, acceleration round on `td/convergence-levers` @`003b8c3f93`; supersedes items
1–3 and 5 of the 2026-10-02 block where they overlap):**

1. **Merge.** Operator decision 2026-10-04: merge `td/convergence-levers` into `adaptive-rx-UL-DL` locally with the
   "fastest combination" defaults (§10.2), applied by the controller on `td/fast-defaults` together with the 4-RX gate
   thresholds in `rfsim_regress.sh` (§12). Push only with an explicit operator OK. State at `003b8c3f93`: build OK
   (CUDA arch 121), ctest only the 4 known ARM failures, sens6 freeze clean. Acceptance as the controller set it
   (ledger 2026-10-03 23:55): (a) flags OFF vs main fails only because of K39 (intentional correctness change);
   (b) ON: postconv 100 %, 0 alarms, 0 wrong — but `drop_full` > 1 % (pre-existing, K45); (c) ON ≥ 2× faster than
   levers OFF, ≈ main on tda0, ~2× faster than main on tda2; simulator blind first-RNTI median ≤ 30 s with the subset
   (8.3 s at B = 128). Hence the operator decision.
2. **Policies from this round** (also in §26): **4 RX for all tests** (operator 2026-10-04); measurement-lock policy
   (`LOCK_POLICY.md`: exclusive lock only for rfsim and benchmarks, shared for builds and tests, none for `nr_td_sim`);
   publication note: **do not depend on unified memory** — explicit copies are the baseline, unified memory is an
   optimisation (the GPU CB0 entry already runs both; GrantWork's `cudaMallocManaged` is GB10-friendly but migrates
   pages over PCIe on a discrete GPU).
3. **X410 OTA 2026-10-05/06 — checklist additions** (on top of the 2026-10-02 list; 4 RX; with `ISAC_METRICS_PATH` set;
   per run, per cell, never pooled):
   - **CB0 channel:** `td_cb0_items / td_cb0_grants` (expect ~3 on rfsim; record OTA), `td_cb0_admissible` and the
     `td_cb0_inadmissible{…}` split (which bits dominate on air: `not_new_rv0` = HARQ share, `iq_stale`, `lbrm`,
     `rank`, `decoder`, `budget`), **`td_cb0_premise_alarms` and any `TD_CB0_PREMISE_ALARM` line (expected 0; any
     occurrence is a finding)**, `td_cb0_eliminations`, CONVERGED suffix `cb0_*` per context, `td_cb0_not_testable`.
   - **Backend:** `td_cb0_backend{cpu,gpu}`, the `TD_CB0 GPU backend registered / not available` line, `td_cb0_gpu`
     `errors/timeouts/bypassed/sticky/trips/failed/skipped` (GPU failures) and `mode` (1 on the DGX), `td_cb0_us_per_item`,
     `TD_CB0_SCHED` (`us_per_iter`, budget skips).
   - **Decoders:** `ldpc_tb_cpu` vs `ldpc_tb_cuda` split (with `ISAC_TD_TB_CPU_WHILE_ACQ=1` acquiring TBs are CPU),
     `ldpc_cuda_breaker_trips`, `ldpc_cuda_fallbacks`, `ldpc_cuda_errors`, `ldpc_cuda_disabled`.
   - **fb2:** `td_fb_promotions`, `td_fb_withdrawals`, `td_fb_failopens` (+ `FIELDBOOK … FAIL-OPEN` lines),
     `td_fb_pruned_contexts`, `td_fb_untrusted_ctx`, `fb_pruned=` on CONVERGED. A real multi-UE cell is the **first
     runtime test of promotions**; a fail-open or withdrawal needs a look at the promoted value vs later winners.
   - **ttc:** `score_rx.py` `ttc_by_tda`, contexts, reopens, postconv CRC; compare with the 4-RX rfsim numbers only as
     an upper bound (never pooled).
   - **Rank > 1 admissibility:** Nl per grant and the `rank` inadmissible count; the first rank > 1 grants of an
     **acquiring** context feeding CB0 are the first runtime exercise of K38-fixed rank > 1 CB0 evidence (never
     exercised in rfsim). Any premise alarm at Nl > 1 → disable with `ISAC_TD_CB0_RANK_MAX=1` and investigate.
   - Optional A/B arms (alternated, n ≥ 5): `ISAC_TD_CB0_BACKEND=cpu` vs auto; `ISAC_TD_CB0_ELIM=0`; `ISAC_TD_FIELDBOOK=0`.
     Never `ISAC_RX_BRANCH_FO`, never `ISAC_TD_K0_ORACLE_LEGACY=1` as a default.
4. **Open items recorded this round:** K45 (4-RX `drop_full`, PDCCH scan queue, K27; main 0.79–1.20 % too), K46 (TBS
   twins), K47 (~3 items per grant), K48 (fb2 epoch bump on SOFT gaps → R7), K49 (pin49r4 grant count), K50 (review
   status); K38 residuals (`ISAC_LLR_SCALE`, GPU-LLR probe paths); G1 breaker edge case (K34).
5. **Deferred work (operator 2026-10-03/04), in no fixed order:**
   - **multi-k0 GrantWork** (CB0 for hypotheses whose k0 points at another slot) and **lifting the 16-signature /
     5-geometry cap** — the next speed lever (K47);
   - a **CB0-specialised layered (or on-chip-state) GPU LDPC decoder** — changes the algorithm, so it **needs a new
     paired dominance proof** against every TB decoder (§3.3.1);
   - the **full-combo 4-RX rfsim gate on an idle host** (all merged-main defaults at once incl. fb2 and `auto`, ≥ 5 runs,
     interleaved with main; also finalises the provisional 4-RX thresholds, §12) and ≥ 3 interleaved GPU-vs-CPU ON arms;
   - **Nl > 1 runtime admissibility** on an acquiring (non-pinned) rank-4 bed (`autor4`-type, which did not sync in
     §14.2 — needs a working bed), plus `PROBE_EQUIV_GW` at Nl > 1 and `GW_EQUIV` after GrantWork fix C1;
   - **sens6 GPU tests** (verify the model with `nvidia-smi`; build `LDPC_CUDA_ARCH=89` with nvcc 12.4; run
     `test_ldpc_cb0_cuda`, `test_ldpc_cuda_pool`, `test_nr_td_cb0_gpu_backend`, `test_nr_td_cb0_batch`,
     `test_nr_td_cb0_wire`; paired dominance; throughput in EXPLICIT mode; coexistence with G1 TB decodes; an rfsim arm
     with `ISAC_TD_CB0_BACKEND=gpu`, expected mode 2) — plan in `task-CB0GPU-report.md` §6; x86 build of the new code;
   - BC9c, BC10, BC11, BC12b (unchanged from 2026-10-02); **soft oracles** (lever S, K41);
   - **SA bed gate option (b)** (SIB1-broadcasting multi-UE bed: exercises exclusions, census and fb2 promotions);
   - cap-12 GPU CB0 iterations against the CPU TB decoder (`ISAC_TD_CB0_GPU_ITERS=12`, 1.33×): operator decision;
   - copy-free G1 TB pool on GB10; ASan of the CUDA plugin; CB0-vs-TB GPU contention measurement.
6. **Then** the reconfiguration-robustness plan (operator: "finish levers work then robustness"; plan
   `docs/superpowers/plans/2026-10-01-reconfiguration-robustness.md` on `adaptive-rx-UL-DL`, branch
   `rr/reconfig-robustness`), which owns R7 (K48) and K37.

**Robustness update 2026-10-04 (rr/integration `c6fc03be7c`).** [IMPLEMENTED, NOT VALIDATED on live
reconfiguration] R1–R12/R17/R18 now provide the cell epoch authority, sources (identity, MIB, SIB1 semantic hash,
SI boundary, continuity, BWP, CSI-RS and trusted per-UE changes), and consumers (queues/GPU work, DCI length/pins,
CORESET bank, Technique D and FieldBook). R10b's class/cause table is authoritative: narrow SOFT
continuity/BWP/CSI-RS only withdraws shared publication; dedicated SOFT and HARD_REVERIFY reopen affected dedicated
state as hints; HARD_RESET isolates identity state. Technique D's former global reset on a DCI RELOCK is replaced by
a targeted DL reopen. [OFFLINE VERIFIED, DGX aarch64, R10b/RE]

Enable only the experimental arm with `ISAC_RECONF=1`; default flags are the safe baseline. For an arm, grep
`CONFIG_EPOCH|DCI length (RELOCK|unresolved)|CORESET bank (STALE|REMOVED)|multi-CORESET bank add|dci_wide_probes|\
scanq_drop_epoch|pdcch_inline_drop_epoch|pdschq_drop_epoch|puschq_drop_epoch|UECTX_STATS|CSIRS_MAP_CHANGE` and
retain the corresponding `ISAC_METRICS`/JSONL. `ISAC_TD_IGNORE_SIB1=1` is the SIB1-less arm, not a claim that the
cell is NSA.

| Arm, 106 PRB / 4 RX / 420 s | Result | `drop_full` | Postconv CRC / ttc |
|---|---|---|---|
| flags off | [MEASURED, DGX rfsim 4 RX, n=2] 2/2 PASS | 0.94 / 1.13 % | 100 %; tda0 18–25 s, tda2 41–49 s |
| `ISAC_RECONF=1` | [MEASURED, DGX rfsim 4 RX, n=2] 2/2 PASS | 1.08 / 0.91 % | 100 %; same gate limits |
| `ISAC_RECONF=1 ISAC_TD_IGNORE_SIB1=1` | [MEASURED, DGX rfsim 4 RX, n=2] 1/2 PASS; failed arm was `drop_full` only | 2.06 / 3.42 % | 100 %; same gate limits |

The SIB1-less queue-drop increase interacts with pre-existing K45; it is not evidence of a stale winner. GPU polar
path `drop_full` is also pre-existing/unseparated here. The R2 150-s regression found the original all-30..140
default cost (1.7–3.7 % vs pre-R2 0.04 %) and R2d fixed it with the 30..63 plus <=5% low-duty wide probe; do not
replace that policy with an unbounded sweep. [MEASURED, DGX rfsim 106 PRB 1 RX, R2d]

**Live SA bed on sens6 (R13).** This is an OAI SA **rfsim** bed, not OTA: the operator runs an Open5GS core, one
attached active OAI UE, and a separate passive `nr-uesoftmodem --passive-rx` client on **sens6 x86_64**. Preserve each
result as `[SIM VERIFIED, sens6 SA rfsim, @<commit>, CPU|GPU]`; do not pool it with DGX, OCUDU, or OTA evidence.
`tests/passive_rx/sa_bed/RUNBOOK.md` is the detailed operator reference; the procedure below is the complete concise
runbook.

**Prerequisites and core.** Use an idle host and one bed only. Keep `ISAC_RX_BRANCH_FO` unset; stop receiver-owned
processes with SIGINT only. Before a run check the host's `nr-softmodem`/`nr-uesoftmodem` processes, containers,
ports and `/tmp` capacity; `sudo -n true` must work. The runner takes the exclusive `/tmp/td_measure.lock`, refuses a
held lock/BUSY file/occupied rfsim or telnet port, and owns cleanup. Before any commit retain the frozen-sens6 gate:

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures 'tests/passive_rx/*.conf' tests/passive_rx/sens6_host_snapshot_2026-09-30
```

Install MongoDB and Open5GS for the installed Ubuntu release using its official Quickstart; merge (do not replace)
`tests/passive_rx/sa_bed/open5gs-{amf,smf,upf}.cfg` into the installed service YAML. Configure AMF NGAP at
`127.0.0.1:38412`, gNB NGAP/NG-U at `127.0.0.100`, and bind UPF GTP-U **only** at `127.0.0.7:2152` (never wildcard or
another address). Provision `ogstun`, forwarding and the test subscriber in `open5gs-subscriber.cfg`: PLMN 001/06,
TAC 1, SST 1, SD 000000, DNN `internet`, IMSI `001060123456743` and the matching UE credentials. Restart the affected
services, then verify AMF/SMF/UPF status, SCTP `:38412`, exact UDP `127.0.0.7:2152`, `ogstun`, and AMF/SMF logs.

The runner automatically creates `r13-ue-<pid>` plus a veth (`192.0.2.1/30` host, `192.0.2.2/30` namespace), runs the
active UE and UDP sink in that namespace, and retains the UDP sender on the host. Keep that subnet unused; permit host
INPUT TCP/4043 from the veth if filtered. The assigned UE IP must route through `ogstun`, not `local` or the veth; the
runner records `ip route get` evidence. It removes the namespace/veth on cleanup. A fresh 10-second interval must show
at least 100 C-RNTI grants/s before the arm is ready.

**Builds (only while idle; shared lock; maximum `-j8`).** From the repository root, make the CPU control build with
telnet support, including both CI targets required for the BWP action:

```bash
R=$PWD; B=$R/cmake_targets/ran_build/build
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S "$R" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_TELNETSRV=ON -DENABLE_ISAC_SENSING=ON -DOAI_SIMU=ON -DOAI_USRP=OFF -DENABLE_LDPC_CUDA=OFF
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C "$B" -j8 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem telnetsrv telnetsrv_ci tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir "$B" -j4 --output-on-failure
```

For a GPU arm first inspect the actual discrete GPU and CUDA toolkit. The 2026-09-30 snapshot says RTX 4060 Ti, while
the operator expected RTX 4070; both are sm_89, but the observed capability is authoritative. Never use DGX sm_121 on
sens6 (nvcc 12.4 cannot target it). Build separately, substituting the observed capability if it differs:

```bash
nvidia-smi --query-gpu=name,memory.total,compute_cap,driver_version --format=csv,noheader; nvcc --version
BG=$R/cmake_targets/ran_build/build_gpu
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S "$R" -B "$BG" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_TELNETSRV=ON -DENABLE_ISAC_SENSING=ON -DOAI_SIMU=ON -DOAI_USRP=OFF -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=89
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C "$BG" -j8 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem telnetsrv telnetsrv_ci tests
```

Set `BUILD=$BG R13_GPU=1` only for a GPU campaign. Require `libtelnetsrv.so` and `libtelnetsrv_ci.so`, then verify
`TD_CB0 GPU backend registered` and `td_cb0_backend.gpu` in `metrics.jsonl`; a fallback is CPU evidence, not GPU
validation.

**Bring-up and arms.** `gnb_baseline.cfg` is a 106-PRB SA cell; it has a 40-PRB first dedicated BWP. The size variant
`gnb_dedicated.cfg` uses 24 PRB, so the operator must see a 40-to-24 BWP DCI-1_1 width change in
`DCI11_WIDTHS`/`Filling Format 1_1 DCI of size` gNB logs. `gnb_cell.cfg` changes PCI 0 to 1. The order is core/preflight
→ baseline gNB/NG → rfsim :4043 → active-UE RA/PDU session → continuous UDP → passive receiver acquisition, DCI-1_1
lock and Technique-D convergence → timed action → continued reception → SIGINT teardown. Restart arms retain the same
passive PID: reconnect has backoff, rebases timestamps and emits `RXDISCONT`; replacing a dead passive receiver makes
the run INCOMPLETE.

Use a fresh directory for each invocation. These are all scenario/arm combinations; `sa` leaves SIB1 evidence enabled,
while `sib1less` makes the runner set `ISAC_TD_IGNORE_SIB1=1` (the serving SA cell still broadcasts SIB1). `on` sets
`ISAC_RECONF=1`; `off` is the flag-off control. Both use receiver-only `ISAC_BWP_TRACK=1`.

| Scenario | Run each of these four arguments |
|---|---|
| stable | `stable sa on`, `stable sa off`, `stable sib1less on`, `stable sib1less off` |
| BWP soft change | `bwp_switch sa on`, `bwp_switch sa off`, `bwp_switch sib1less on`, `bwp_switch sib1less off` |
| same-PCI size restart | `same_cell_restart_size_change sa on`, `same_cell_restart_size_change sa off`, `same_cell_restart_size_change sib1less on`, `same_cell_restart_size_change sib1less off` |
| PCI-changing restart | `cell_restart sa on`, `cell_restart sa off`, `cell_restart sib1less on`, `cell_restart sib1less off` |

For each table cell: `bash "$R/tests/passive_rx/sa_bed/run_arm.sh" <arguments>`. Changed arms normally use
`R13_DURATION_S=180`; stable defaults to 900 seconds after `ready` (minimum 15 minutes), or selects a 3600-second soak
with `R13_SOAK=1` and no explicit duration. Run five fresh repetitions of all 16 arms without building concurrently:

```bash
export R=/path/to/repository BUILD=$R/cmake_targets/ran_build/build
export R13_APPLY_AT_S=45 R13_TRAFFIC_RATE=6M R13_RX_ANT=4 CAMPAIGN_GRACE_S=120 CAMPAIGN_TERM_GRACE_S=120
C=$(python3 "$R/tests/passive_rx/campaign/campaign.py" new --root /tmp/r13-sens6 --name r13-cpu --site sens6 --cell 'OAI SA rfsim PCI0/1 106PRB PLMN00106')
for scenario in stable bwp_switch same_cell_restart_size_change cell_restart; do
  if [ "$scenario" = stable ]; then unset R13_DURATION_S; secs=1800; [ "${R13_SOAK:-0}" = 1 ] && secs=4500; else export R13_DURATION_S=180; secs=900; fi
  for sib in sa sib1less; do for flag in on off; do for repeat in 1 2 3 4 5; do
    python3 "$R/tests/passive_rx/campaign/campaign.py" run "$C" --arm "${scenario}_${sib}_${flag}" --secs "$secs" -- bash "$R/tests/passive_rx/sa_bed/run_arm.sh" "$scenario" "$sib" "$flag"
  done; done; done
done
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" "$C"
```

Expected baseline logs are `Received NGSetupResponse`, `RA procedure succeeded`, `PDU Session Establishment Accept`,
`UE IPv4`, `SENSING: DCI 1_1 length locked`, and `Technique D CONVERGED`. A switch adds `triggered BWP switch` and
`BWP RESOLVED`; enabled changed arms add `CONFIG_EPOCH old -> new class=SOFT|HARD_REVERIFY|HARD_RESET cause=...` then
fresh lock/verification/convergence. Restart arms also show `rfsim passive client reconnect attempt=`, `reconnected
after`, and `RXDISCONT`; same-PCI needs `HARD_REVERIFY cause=CONTINUITY_LOSS`, PCI change needs `HARD_RESET
cause=CELL_IDENTITY_CHANGE`. No `CONFIG_EPOCH` is expected in controls.

**R13 VALIDATION RULES (explicit).** `score_r13.py` is authoritative; inspect each `score_r13.{json,txt}` and campaign
`r13_summary.json`.

1. For each enabled scenario × SIB1-available/SIB1-less arm, require at least five complete runs; also retain at least
   five complete matching flag-off controls. Controls are comparators, never enabled-R13 passes.
2. Require at least 100 C-RNTI grants/s from a recent pre-ready 10-second metrics interval. Sparse, missing or stale
   traffic/metrics is INCOMPLETE.
3. BWP soft recovery is at most 10 s from apply. Both hard restart recoveries are at most 30 s from the **first
   post-change gNB DCI** (`validation_start`), not operator/gNB/UE downtime; record the secondary from-apply value too.
4. Require fresh milestones: BWP RESOLVED plus convergence; new length lock/relock plus convergence; or post-restart
   acquisition lock/verification plus convergence. Enabled BWP/cell arms require their expected SOFT/HARD_RESET class.
5. Require zero stale DCI and TDRA/DM-RS winners. gNB logs/config are validation-only scorer input, **never receiver
   input**. The operator must add a run-local `td_truth_audit.json`; absent/insufficient gNB truth means
   `stale_winners: null` and INCOMPLETE, never a claimed zero.
6. In a stable run of at least 15 minutes, require no more than one false SOFT per hour (hour-normalized) and zero false
   HARD; one false SOFT in a 15-minute run fails. Report all four dropped-epoch deltas and ensure old work gains no
   credit.
7. Every evidence label names host `sens6`, bed, commit and CPU/GPU path. Freeze sens6 before every commit; keep CPU
   and GPU evidence separate. Run beds only on an idle host under the exclusive lock; builds/tests use the shared lock
   and never overlap a bed.

Known coverage limits are deliberate: one active UE cannot meet the two-distinct-RNTI/2-second condition for
`DEDICATED_CHANGE_SUSPECTED`, so this validates local DCI relock rather than that cell-wide trigger. The arm is the
plan deviation `same_cell_restart_size_change`, not a dedicated-config mutation under a continuously running gNB.
Passive rfsim reconnect is new and bed-validated only here; it is not OTA evidence.

**Teardown and fault handling.** Interrupt `run_arm.sh`/the campaign with SIGINT and wait for its process-group,
namespace/veth and lock cleanup; do not SIGKILL the passive receiver. Before another arm confirm no softmodem/traffic
process remains and that :4043/:9090 are free. For NGAP timeout check AMF address, PLMN/TAC, SCTP and logs; for PDU
failure check subscriber K/OPc/AMF, slice/DNN, SMF/UPF and `ogstun`; for traffic below 100 grants/s check TUN, route,
traffic logs and scheduler load. For no DCI-size difference inspect the MAC-debug widths (equal/missing is
unscorable). For restart failure preserve logs, check reconnect/RXDISCONT/epoch order, and never relaunch the receiver
as proof. For GPU fallback check the observed capability, `LDPC_CUDA_ARCH`, CUDA libraries/`LD_LIBRARY_PATH` and plugin
registration; label it CPU. See RUNBOOK.md §7–§8 for the exact manual reconnect checks and extended troubleshooting.

**R14 soak results.** `[MEASURED, DGX aarch64 rfsim 106 PRB 4 RX, rr/integration c6fc03be7c, 3600 s each,
ISAC_RECONF=1]` The SIB1 arm and SIB1-less arm each had 0 `CONFIG_EPOCH` bumps (0 SOFT, 0 HARD), 0 `RXDISCONT`, and
0 DCI-length RELOCK events. In the SIB1-arm run, two contexts converged with 0 reopens; overall CRC was 99.59%,
157435 grants decoded, and `drop_full` was 3.10%. The latter is K45 long-run accumulation; gate runs were 0.9–1.1%.

**R14 caveats.** The DGX `--phy-test` rfsim gNB broadcasts no SIB1 (0 SIB1 decodes in every run), so both nominal arms
were effectively SIB1-less. SIB1 hash, SI modification and SIB1 re-decode sources are testable only on the sens6 SA
bed. `[CODE-READ]` Script inspection verifies that the arm flag is passed through the environment, but no startup log
line proves that fact; add one as follow-up. These results are simulated soak evidence, not R13/sens6 or OTA
validation. R18 steps 2–4 and 7 (CSI-RS bed/value work) remain open.

**Rule: do not begin new receiver development on the DGX Spark until the current known-good offline baseline (§14)
has been reproduced and the OTA baseline (§15.4) has been reproduced or its failure understood.**

---

## 26. Working rules for any agent on this project (carried over from the old setup)

- Evidence labels (§0.1) in every status statement; retract wrong claims explicitly; never quote inherited numbers.
- **Never build or run tests that load the CPU heavily while a capture runs on the same host.** Check
  `pgrep -x nr-uesoftmodem` first.
- One radio harness per host at a time. Kill only processes you started, by PID or `pgrep -x`; **never `pkill -f`**
  (it matches your own ssh command line). A hung receiver may be stopped with `sudo kill -INT <pid>` (SIGINT = OAI's
  Ctrl-C path); avoid SIGKILL (leaves stale MPM claims).
- After a killed/stalled capture: wait ≥ 60 s and verify the X410 is unclaimed; after an MPM restart wait ~180 s.
  No routine MPM restarts.
- Don't wait blindly on a capture: poll with a background monitor and read the log as it grows.
- Never put `#` comments inside a `bash -c "…"` string that is `exec`'d (truncates argv).
- Never `git stash` in multi-worktree setups (the stash is shared); never stage hunks without context; `git add
  <explicit paths>`, never `-A`.
- Rebuild dlopen'd plugins explicitly (§9). Check a deployed binary contains your change (`strings bin | grep <new literal>`).
- Ground truth (gNB logs/configs, F1AP pcaps) is for validation only; re-read the live gNB config from the running
  process (`/proc/<pid>/cmdline`, the yaml's `log: filename`) — log paths move and old logs look like "no traffic".
- C-RNTIs change on every re-attach: re-read them from the gNB log for each validation run.
- Passive OTA results swing strongly run to run: ≥ 5 runs per arm, alternated, with a validity verdict per run;
  a "regression" must be bisected against the rig drifting on its own.
- rfsim beds and gates run on an **idle host** only: never concurrently with `nr_td_sim` campaigns or builds (a BC9
  A/B was invalidated by 6–8 simulator processes, 2026-10-02). One rfsim bed at a time.
- Simulator (`[SIMULATED …]`), rfsim (`[MEASURED, DGX rfsim …]`) and OTA numbers are never pooled; a simulator
  anomaly is root-caused against the runtime code before it is quoted (BC6 A1 was a simulator artefact).
- **4 RX for all tests** (operator, 2026-10-04): rfsim beds with `--ue-nb-ant-rx 4`, simulator arms at 4 RX; 1-RX
  numbers are HISTORICAL references, never pooled with 4-RX ones.
- **Measurement-lock policy** (operator 2026-10-03, `.superpowers/sdd/2026-10-01-technique-d-blind-convergence/LOCK_POLICY.md`),
  lock file `/tmp/td_measure.lock`: **exclusive** (`flock -x`) only for rfsim runs/gates and GPU/CPU throughput or
  latency benchmarks; **shared** (`flock -s`) for builds (ninja) and ctest/unit tests (shared holders never block each
  other, they only wait for and hold off an exclusive measurement); **no lock** for `nr_td_sim` arms, campaign scripts,
  git and code reading. Gate every build on `pgrep -x nr-uesoftmodem` being empty (abort/retry, never just print). Take
  the exclusive lock per measurement, never across a chain; use `flock -w <s>` and report a timeout.
- **Unified memory is an optimisation, not a requirement** (publication note, 2026-10-04): explicit host↔device copies
  are the baseline every GPU path must support and be validated with; unified memory (GB10) only removes copies. Report
  GPU results per memory mode and host.
- CRC evidence from different LDPC decoders (`decoder_used` CPU vs CUDA) is never pooled (K34/K36).
- Real-time budget is a requirement: per-slot work must not block the RF reader; sensing per-CPI processing < 75 ms.
- Before OTA: prove the RF path (`benchmark_rate`, `rx_samples_to_file` RMS) before debugging sync.
- Commit messages explain root cause + evidence. OAI code style (match the file, `LOG_I/LOG_W/LOG_E(PHY, …)`, no
  `printf` in library code, `nr_`/`NR_` prefixes).
- Keep this file updated: every finding, fix, validation, retraction goes into the relevant section with date,
  commit and evidence path.

---

## 27. No stale results (reminder)

§15 contains only the 2026-09-25 campaign (latest for this branch) with its binary, host, X410 unit and cells. Every
other OTA result is in §15.5 or §8, labelled HISTORICAL. Results from different X410 units (327B872 vs 327C1F2), hosts
(sens3 vs sens6 vs DGX), bandwidths (51/106/217/273 PRB), cells (lab PCI 2, Salt PCI 64, Swisscom PCI 382, older
3414.99 MHz cell), operators and commits must never be combined into one number.

---

## 28. Final self-check (answers located)

| # | Question | Where |
|---|---|---|
| 1 | What is the project building? | §1.1 |
| 2 | What does "passive agnostic receiver" mean? | §1.2, §1.3 |
| 3 | What currently works? | §16 (+ §13, §15) |
| 4 | What does not work? | §16, §24 |
| 5 | Which statements are offline-only? | labels throughout; §13, §16 "Offline" column |
| 6 | Latest OTA test? | §15.1–15.3 (2026-09-25, `aa872eba3b`, lab + Salt) |
| 7 | Operator and how we know | §15.3: PLMN 228-03 decoded from SIB1 (Salt via public MNC mapping); lab PLMN 001/06 test network |
| 8 | Files per stage | §2.2, §3.2, §3.3, §23 |
| 9 | Build | §9 |
| 10 | Run | §10 |
| 11 | Interpret logs | §11 |
| 12 | Run every validation test | §13, §14 |
| 13 | Tests ↔ gates | §12, §14 |
| 14 | X410 configuration | §6, §4 |
| 15 | Characterize a different X410 | §7 |
| 16 | Old X410 problems | §8 |
| 17 | Machine-specific assumptions | §4, §5.3 |
| 18 | What must change for the DGX | §5 |
| 19 | Full target architecture | §17 |
| 20 | What remains to implement | §17 labels, §24, §25 |
| 21 | Multi-cell processing | §17, §20 |
| 22 | Multi-operator processing | §18, §20 |
| 23 | Final receiver output | §21 |
| 24 | What to do first | §25 |
| 25 | Technique D blind convergence (branch `td/convergence-levers`): modules, env, logs, results, open issues | §3.3.1, §10.2, §11.8, §12, §14.8, §14.9, §24 K38–K44, §25 (2026-10-02 block) |
| 26 | Acceleration round 2026-10-03/04 (G1, K38, fb2, CB0 elimination, GrantWork, CB0 CPU/GPU backends): defaults, metrics, 4-RX results, gate, hosts, open items | header 2026-10-04, §0.3, §3.3.1 (second table), §10.2 (merged-main defaults), §11.8, §11.13, §12 (4-RX gate), §14.9–§14.10, §24 K34/K36/K38/K39, K45–K50, §25 (2026-10-04 block), §26 |
