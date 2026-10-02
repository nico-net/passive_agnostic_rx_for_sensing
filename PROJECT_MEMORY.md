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
| `sens6` | Development + OTA host. Only host with the X410 (on a 100 GbE ConnectX-5). All code lives here: `/home/sens/NICOLA/adaptive-rx-UL-DL` + lane worktrees `/home/sens/NICOLA/agn-wt/*`. **No 5G core** here, so only OAI `--phy-test` rfsim beds run here. |
| `sensnuc3` | Operator's local machine. open5gs 5G core (AMF 127.0.0.1:38412, PLMN 001/06, MongoDB), OCUDU gNB + srsUE (`/home/sens/NICOLA/repos/`), OAI SA rfsim trees (`rfsim-local` = test gNB, `rfsim-val`, `rfsim-integ` = receiver under test). Holds the agent lock file `/home/sens/NICOLA/AGENT_OWNER` and non-repo design docs under `/home/sens/NICOLA/docs/` (their content is folded into §17–§20 here). **Its `/home/sens/NICOLA/adaptive-rx-UL-DL` is a stale copy — never use it.** |
| `sens4` | Lab gNB host (srsRAN/OCUDU-family `gnb -c /home/sens/gnb.yaml`, driving a radio at 3450 MHz). Live log `/home/sens/NICOLA/gnbLogs/gnb.log` (the old `/home/sens/gnb.log` is stale). **Ground truth for validation only; never an input to the receiver.** |
| `sens3` | Older OTA host (Aug 2026), different X410/NIC addresses. HISTORICAL. |
| **`spark-74c3`** | **NEW (2026-09-30): NVIDIA DGX Spark (GB10, aarch64), DEIB / Politecnico di Milano.** Development + future OTA host. §4.2. The Swiss hosts above (and the sens4 lab gNB) are **not reachable** from here. |

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
| Plan | `docs/superpowers/plans/2026-10-01-technique-d-convergence-levers.md` | revision 2 written, not started; branch to use: `td/convergence-levers` |
| Spec | `docs/superpowers/specs/2026-10-01-reconfiguration-robustness-design.md` | written, awaiting operator review; plan not yet written |
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
| Broadcast | PSS/SSS/PBCH/MIB/SIB1 (upstream OAI + passive fixes); SIB1-less (NSA) path partly handled | Same plus robust SIB1-less operation |
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

**B5. SIB1-less / NSA handling** — see §11 "SIB1/NSA" and §23. Status `[PARTIAL]`, `[OFFLINE VERIFIED]` only.

**B6. Blind PDCCH monitor** — the core of the agnostic receiver. Files and techniques in §23; status
`[OTA VERIFIED on the lab cell, 2026-09-17..25, older binaries]`; `[SIM VERIFIED]` on OAI phy-test/SA and OCUDU.

**B7. DCI recovery/interpretation** — §23. `[OTA VERIFIED]` DCI 1_1 length 47 / 0_1 length 45 on the lab cell
(C-RNTI confirmed against the gNB log); `[SIM VERIFIED]` 42/38 on OCUDU (checked against F1AP truth).

**B8. PDSCH/PUSCH decoding + Technique D** — §23. `[SIM VERIFIED]` (phy-test TD converged 5/5 runs at 1.3 s,
98.9 % CRC; OCUDU 76 % C-RNTI PDSCH CRC). OTA DL TB rate is low (`VOID_DL_RATE`) `[KNOWN ISSUE]`.

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
| `td/convergence-levers` | Planned, not created. | Execution branch for the levers plan (§0.6). |
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
| `nr_pdcch_ss_registry.c`, `nr_pdcch_sib1_prior.c`, `nr_tdd_pattern.c` | Search-space set registry, SIB1 prior, TDD slot direction | gtests |
| `nr_pdcch_passive_queue.c`, `nr_pdcch_gpu_fep.cu` | Candidate queue; GPU DM-RS chest+LLR (optional) | offline; GPU value not established |
| `nr_hyp_sweep.c/.h`, `nr_crc_evidence.h` | Shared hypothesis engine (8192 raw/class cap) and KL/Bernoulli anytime bounds | `test_nr_hyp_sweep` |
| `nr_pdsch_passive_queue.c` (1414), `nr_pdsch_passive_decode.c` (3873) | DL job queue (IQ lifetime, k0, tickets), passive PDSCH decode (FEP→chest→demod→LDPC→CRC), LDPCDIAG/HARQC/BRANCHFO, data-aided CFR submit, ZP grant evidence | SIM/OTA |
| `nr_pdsch_config_sweep.c/.h` (1376) | **Technique D**: per-RNTI/TDA context sweep over (S,L,mapping A/B), DM-RS add-pos/max-len/type, MCS table, k0, PRG, PTRS, LBRM; DM-RS mask oracle; `ORACLE_RESTORE`; LRU contexts | `test_nr_pdsch_config_sweep` (44+1 skip) |
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
| `openair1/PHY/CODING/nrLDPC_cuda/*`, `nrPolar_tools/cuda/*`, `nr_pdsch_gpu_fep.cu`, `nr_polar_gpu*` | CUDA LDPC decoder plugin (`--loader.ldpc.shlibversion _cuda`), CUDA polar SC, GPU FEP | Measured **20× slower wall-clock** than CPU for single TBs on the RTX 4060 Ti — not used by default |
| `openair1/SIMULATION/TOOLS/sensing_channel.c`, `SIMULATION/NR_PHY/pusch_ra0sim.c`, `hidden_waveform.h` | Sim channel for sensing scenes; RA0 regression fixture; hidden-DCI transmitter fixture | gtests |
| `nr_passive_metrics.c/.h`, `nr_passive_metrics_json.c` | `ISAC_METRICS` JSON snapshot (schema 1) emitted with the blind-monitor summary (§11.13); optional file `ISAC_METRICS_PATH`; pure serializer in `_json.c` (testable without the receiver). `pci` is `_Atomic`. (A2, `4cdb890759`/`6e9a96c476`) | `test_nr_passive_metrics` (5) `[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 6e9a96c476]` + rfsim `[SIM VERIFIED]` (§14.5) |
| `nr_passive_obs.c/.h` | **Per-grant observation API**: one JSONL record per decoded DL/UL grant to `ISAC_OBS_PATH`, non-blocking ring (16384 slots, drop-on-full), writer thread; header comment = schema v1 (§21). (A3, `10cc6f0058`, fixes `385e02b9cf`) | `test_nr_passive_obs` (8) `[OFFLINE VERIFIED, cloud x86 …, 385e02b9cf]`, also under TSAN 0 warnings (A7); rfsim `[SIM VERIFIED]` (§14.5). UL hook compiled, **not exercised** (K28) |
| `nr_pdcch_blind_phase2.c/.h` | **Phase-2 lock** (`g_phase2_mu`) for the blind-PDCCH occasion tail (accepts, dci_thres EMA, RNTI persistence ring, census, CFR/PDSCH submit); energy floor with its own leaf lock; production accept gate `nr_pdcch_blind_dl_accept_gate()`. Enables N scan consumers (K27). (A7, `b6e5fb27ac`, `1d6cbdf5c3`) | `Phase2Concurrent*` (2) in `test_nr_pdcch_blind_monitor` (197+2 skips) `[OFFLINE VERIFIED, cloud x86 …, 1d6cbdf5c3]`; TSAN 0 warnings; rfsim A/B §14.5 |
| `nr_initial_sync_budget.c/.h` | Pure parser/batcher for `ISAC_SCAN_SCRATCH_MB` (default 512, clamp 64..16384) used by `nr_initial_sync.c` (A11, `843e5cff49`) | `test_nr_initial_sync_budget` (5) `[OFFLINE VERIFIED, cloud x86 …, 843e5cff49]`; timing effect DGX-only |

**Algorithm origins:** blind PDCCH mismatched-bit gate and parts of candidate handling migrated from **NRSniffer**
(`/home/sens/NICOLA/NRSniffer` on sens6, an OAI-based sniffer); DM-RS coherence gate modelled on **5GSniffer**
(`correlate_DMRS()` + AL thresholds, but adaptive); joint GF(2) RNTI solver after **5GDescrambler** (source cites
arXiv:2609.07367 — citation not independently verified). Everything else is project-original on top of OAI.

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
| `tests/passive_rx/dgx/rfsim_arm.sh`, `rfsim_regress.sh`, `score_rx.py` (+`test_score_rx.py`, `fixtures/`) | **In-tree rfsim regression gate** (A1): one agnostic 106-PRB arm, JSON score (sync/ttc/CONVERGED/crc %/drop_full %/CPU/RSS); gate `GATE_CRC_MIN`/`GATE_DROP_MAX` + CONVERGED>=1. Knobs `V4SHIM`, `SCANTHREAD`, `COREMAP` (§10.2). Cloud thresholds 93.0/2.5, DGX 98/1 |
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
| GPU | NVIDIA GeForce RTX 4060 Ti 8 GB, compute capability **8.9**, driver 580.178.04 | yes |
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
| GPU | NVIDIA **GB10**, compute capability **12.1** (`sm_121`), driver 580.178.04, CUDA runtime 13.0; memory reported N/A (unified) | CC 8.9 → 12.1 |
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
| CUDA arch | `LDPC_CUDA_ARCH=89` (default in `openair1/PHY/CODING/CMakeLists.txt:33`), `CMAKE_CUDA_ARCHITECTURES=52/90` | CMake cache | Set to the GB10 compute capability; CUDA 12.4 may be too old for Blackwell — use the DGX OS CUDA toolkit. `ENABLE_LDPC_CUDA` is optional (measured slower). |
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
| GPU LDPC (optional) | `-DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=<cc>` | `libldpc_cuda.so` plugin, `pdsch_gpu`, `pdcch_gpu`, `polar_sc_cuda` |
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
| `V4SHIM`, `SCANTHREAD`, `COREMAP`, `GATE_CRC_MIN`, `GATE_DROP_MAX` | `rfsim_arm.sh`/`rfsim_regress.sh` knobs: IPv4 shim (auto if no IPv6), scan-thread override (auto `1:8:-1` when `nproc<=5`), DGX core-map mode (needs the online cores of `coremap_dgx.env`; rc=3 otherwise), gate thresholds (script defaults = DGX 98/1; cloud uses 93.0/2.5) | host-specific, §14.5, K31 |

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
**Swisscom is NSA** (operator statement; consistent with 450+ RARs and no Msg4). The site ("known-good spot")
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

**SIB1 present:** MIB → CORESET#0 → SI-RNTI DCI 1_0 → SIB1 PDSCH → common facts (above). `[OTA VERIFIED]`.
**SIB1 absent (NSA / k_SSB ≥ 24):** there is no CORESET#0. What is implemented: (1) the MIB DM-RS type-A position
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

### 11.8 PDSCH / DM-RS / decoding

| Message | Meaning |
|---|---|
| `SENSING: Technique D ARMED: independent RNTI/TDA contexts, TB-CRC scoring` | DL waveform sweep active (must not appear repeatedly — it did 265 k times/150 s before the fix) |
| `SWEEP: rnti=0x… CONVERGED tda=… mapping=A k0=… mcs_table=… dmrs_add_pos=… dmrs_max_len=… (x/y trials)` / `SENSING: Technique D CONVERGED rnti=… tda=… S=… L=… mask=0x884 table=…` | converged PDSCH interpretation for that RNTI/TDA (mask 0x884 = DM-RS symbols 2,7,11) |
| `SWEEP: ORACLE_RESTORE …` | a measured DM-RS mask re-exposed a pruned short-TDA candidate |
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

### 11.11 Acquisition / reacquisition state

`SENSING: ACQ_STATE A -> B (updates=… time_in_prev=… regressions=…) evidence[len_found= coreset_ok= ul_bwp= dl_win= ul_width_win= ul_interp_win=]`
and `SENSING: ACQ_STATE X -> LOST (receive-stream discontinuity, no hysteresis; …)`. States (evidence labels, not a
strict chain): SEARCHING → PBCH_LOCKED → SIB1_DECODED → PDCCH_LOCKED → CORESET_VERIFIED → CELL_CONFIGURED →
DL_CONVERGED / UL_CONVERGED → TRACKING; LOST after 8 regressed updates or a stream discontinuity; LOST → SEARCHING.
Implemented recovery: stream-gap → clear sync, re-enter acquisition (SIB1 knowledge retained); local DL relearning
per context after ≥ 32 consecutive settled-ticket failures under a δ = 1e-6 bound. **There is no global loss
detector, no stale-evidence expiry, and no configuration-change detector** — a return to TRACKING can reuse latched
evidence from before the gap (`[KNOWN ISSUE]`).

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
| scan queue | `scanq_queued`, `scanq_processed`, `scanq_drop_full`, `scanq_drop_stale`, `scanq_max_lag` |
| PDSCH queue | `pdschq_queued`, `pdschq_decoded`, `pdschq_crc_ok`, `pdschq_drop_full`, `pdschq_drop_stale`, `pdschq_max_lag` |
| LDPC | `ldpc_ok`, `ldpc_seg_fail`, `ldpc_tb_fail`, `ldpc_zero_tb` |
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
| **G5B NSA / SIB1-less** | G4, k_SSB ≥ 24 (or SIB1 absent) | Receiver proceeds without CORESET#0: dedicated CORESET discovery converges, DM-RS pos from MIB used, RNTI anchored (RAR or persistence), no assert, no SIB1 fabricated | stall at SIB1_DECODED/PBCH_LOCKED, crash, cached SIB1 silently used as truth | `MIB dmrs-TypeA-Position posX -> blind monitor`, bank add, C-RNTI accepts, `ACQ_STATE` progression without SIB1 | `test_nr_ue_mib_blind_handoff` (done) + a synthetic no-SIB1 end-to-end fixture (**missing**) | NSA commercial cell (e.g. Swisscom) | G4 |
| **G6 PDCCH acquisition** | G5A or G5B | Dedicated CORESET verified (geometry equals truth where available); PDCCH scrambling ID confirmed; per-AL candidate budgets cover all ALs in use | no bank add in the run budget; AL starvation; wrong geometry banked | `multi-CORESET bank add …`, `PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)]`, ladder `accepts_per_al` | coreset_map, al1_map, blind_monitor gtests; idsweep `--selftest` | OTA lab (truth from gNB log), then commercial | G5 |
| **G7 DCI recovery** | G6 | C-RNTIs recovered and confirmed against truth (lab/sim); DCI lengths locked = truth; layout pinned; false-accept rate measured with a chance baseline | RNTI churn, length flapping, accepts at chance level | `DCI 1_1 length locked … len=`, `UL automatic DCI length locked`, per-RNTI contexts, `held[…]` rates | dci_length_sweep, layout sweeps, bootstrap, joint solver tests | OTA with ≥ 2 UEs; grant census vs gNB log (match key (sfn,slot,rnti,dir)) | G6 |
| **G8 Scheduled-resource reconstruction** | G7 | PRB/symbol allocation, TDRA, DM-RS mask, MCS table, rank, rate matching (SSB/CSI-RS) reconstructed; TB CRC > 0 and ≥ baseline | CRC 0 with healthy constellation; flood of rate-matching errors | `Technique D CONVERGED …`, `LDPCDIAG`, `pdsch_decode[crc_ok]`, `pusch_passive[crc_ok]`, `PDSCH SSB-OBS` | config_sweep, prb_set, ssb_rate_match(_prod), pusch_ra0_*, harq_init_tx, csirs tests | OTA lab: CRC vs 1-RX baseline; commercial cell | G7 |
| **G9 RS / channel extraction** | G8 | CFR produced per source with correct RE mapping; data-aided reconstruction passes its RE/bit-count invariant; per-antenna CFR coherent | invariant violations, misaligned X | `cfr_submits`, `data_submits`, `ul_cfr[submits]`, no reconstruction-mismatch lines | data_aided tests, dlsch_fixed_point, sensing CFR tests | OTA: CFR SNR/coherence vs pilot-only | G8 |
| **G10 Persistent tracking** | G9 | ≥ 30 min continuous: sync held, contexts converged, CRC stable, no unbounded queues/memory | drift to LOST, queue growth, decode collapse | `ACQ_STATE … TRACKING` held, periodic summaries stable | — (soak test in SIM) | 30–60 min OTA soak | G9 |
| **G11 Reacquisition** | G10 | After an injected gap / cell change / RNTI change / config change: stale evidence rejected, fresh broadcast+TB evidence re-establishes TRACKING, outage measured in sample time | stale winners restore TRACKING, cross-UE evidence mixing | `ACQ_STATE … LOST` → fresh chain | raw-IQ replay with injected gaps (`ISAC_RAW_IQ_GAP_*`) | pull antenna / restart gNB / UE re-attach | G10 |
| **G12 Multi-cell** | G10 | Two co-channel PCIs tracked simultaneously, each within tolerance of its single-cell baseline at a stated SIR, no timing capture | weaker cell steals/loses timing | per-cell tagged logs | 2–3 gNB rfsim/ZMQ bed | Salt PCI 64 + PCI 244 class case | G10 |
| **G13 Multi-carrier** | G12 | Two carriers decoded concurrently (separate channels/LOs or channelizer), no added sample loss | loss/drops increase | per-carrier summaries | multi-gNB beds | two carriers OTA | G12 |
| **G14 Multi-operator** | G13 | ≥ 2 operators' cells processed within compute budget, persistent states, bounded rediscovery | full rediscovery loops, starvation | per-cell health + scheduler logs | — | OTA 2 operators | G13 |

**Regression gate (rfsim 106 PRB), post-convergence mode, 2026-10-02.** `tests/passive_rx/dgx/rfsim_regress.sh` now defaults to `GATE_MODE=postconv` (operator option a): n_contexts >= 2 with 0 reopens, post-convergence CRC >= 99.8 % over >= 5000 grants, per-context ttc tda0 <= 8.6 s / tda2 <= 35.3 s (1.5 x idle max), overall-CRC search floor 94.5 %, drop_full <= 1 %; `GATE_MODE=legacy` keeps the old overall-CRC >= 98 % criterion (which the K39 search phase fails at ~95-97.6 %). `score_rx.py` gained `contexts`, `ttc_by_tda`, `postconv_crc_pct`, `search_crc_pct`, `ldpc_zero_tb`. [MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @0232f351c3 (code 1a6be6155b)]: 10/10 BC9 idle runs have post-convergence CRC 100.00, ttc0 2.15-5.71 s, ttc2 19.70-23.48 s, overall CRC 95.25-97.57; derivation in `tests/passive_rx/dgx/README.txt`. Option (b), an SA bed broadcasting SIB1, is a later task.

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
   across grants/antennas/cells must be **re-measured**, not assumed slow.
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

## 15. Current OTA status (latest campaign only)

### 15.0 Site change (2026-09-30) — read first

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
| SIB1-less / NSA (G5B) | PARTIAL (MIB DM-RS hand-off, CSS0-free discovery, RAR anchor) | PASS (6 tests) | **no bed** | not tested with this code | No NSA bed; carrier/TDD without SIB1 not estimated | Build a synthetic no-SIB1 fixture; OTA on Swisscom |
| Dedicated CORESET discovery (G6) | Done for contiguous, duration-1, non-interleaved family + AL1 cover + offline/GPU nID sweep | PASS | PASS (OAI SA, OCUDU truth match) | PASS on lab (bank add len 47) | Duration 2/3, holed bitmaps, multi-CORESET, AL1-dominated cells | Validate on Salt; implement multi-symbol ranking |
| RNTI / DCI recovery (G7) | Done (bootstrap, length sweep, layout sweeps, joint solver opt-in) | PASS | PASS (DCI 42/38 = F1AP truth) | PASS on lab (C-RNTI confirmed 2026-09-17) | False-accept calibration; multi-UE fairness | Grant census vs gNB log on DGX |
| DL reconstruction + decode (G8) | Done: Technique D, rank ≤ 4, SSB/CSI-RS rate matching, HARQ reserved-MCS | PASS (DGX too) | **DGX phy-test: 5/5 CONVERGED, 98.6–99.1 %; 256QAM 99.5 %; 273 PRB 95–96 %** (§14.1); sens6: 98.9 %, OCUDU 76 % | **FAIL/low** (lab 2026-09-25: TB rate ~1 %; best historical 73 % on 2026-09-14 older binary) | Link margin/precoder nulling, rank vs 1 RX, CPU at 4 RX | Reproduce 1-RX lab baseline, then 4-RX |
| UL decode (G8) | Done: one-layer CP-OFDM, RA0 segmented, UCI footprint, TP limited | PASS on sens6 (9 RA0); **DGX: 7/9, 64/256QAM bit errors (K22)** | PASS (OCUDU 50–54 % at times) | historical lab 78.6 % at 272–273 PRB (older binary) | Width search time, 64QAM ceiling, TP unvalidated live | Measure on DGX |
| CSI-RS / ZP-CSI-RS | Done (rows 1–5, opt-in wide), ZP probation/revocation | PASS (131 + synth) | FAIL then fixed (G4 pending) | IDSWEEP solved on lab, confirmation not reached | G4 of the fixes | OCUDU CSI-RS arm n=3 |
| RS / CFR extraction (G9) | Done (SSB, DM-RS, data-aided DL/UL, CSI-RS) | PASS | PASS | partial (sensing CPIs formed on lab) | depends on G8 | — |
| **Receiver metrics / observations / campaign / dashboard** (new 2026-10-01) | `ISAC_METRICS` JSON (A2), per-grant obs JSONL (A3, schema v1 §21), campaign runner (A4), Receiver-health tab (A5), regression gate (A1) | PASS: 5 + 8 + 5 gtests, 4 + 15 + 16 python tests (cloud x86, §13.2) | **PASS cloud x86 only** (§14.5); not run on the DGX | n/a | UL obs hook not exercised (K28); in-line DL decode not recorded (K29); DGX gate thresholds not yet reproduced | Run gate + campaign on the DGX (§25) |
| Acquisition state / reacquisition (G10–G11) | Evidence tracker + local DL relearning + stream-gap LOST | PASS | partial | observed LOST→reacquire (2026-09-13) | No unified epoch, no change detector | Implement §19 on top of existing tracker |
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
| CORESET state | `nr_pdcch_coreset_bank.c` bank, `g_cfg` in `nr_pdcch_blind_monitor.c` | global; bank not cleared on cell change (known) |
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
(many cells × antennas × grants) and data resident on the GPU from capture to bits.

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

### 23.5 NSA / SIB1-less operation
Status: PARTIAL. Goal: proceed without CORESET#0/SIB1.
Input: MIB (k_SSB ≥ 24), traffic. Output: dedicated CORESET, RNTIs, grants.
Algorithm: MIB DM-RS position → blind monitor; CSS0-free dedicated discovery; RAR-anchored trusted C-RNTI (CFRA) when RARs are visible; carrier/TDD from startup or cache.
Implementation: `nr_ue_procedures.c` (`nr_pdcch_blind_monitor_set_mib_dmrs_typeA_position`), `config_ue.c` (no-CORESET0 path), `nr_pdcch_blind_rnti_bootstrap.c` (`record_trusted`), `nr_pdcch_blind_monitor_rt.c`.
Logs: `MIB dmrs-TypeA-Position posX -> blind monitor`. Offline: 6 tests PASS. OTA: none with this code (Swisscom NSA observations 2026-09-21 are HISTORICAL, older binary). Known issues: no NSA bed; no signal-based carrier/TDD estimation; RNTI bootstrap without CSS0 unproven on air.
Pass gate: G5B. Remaining: synthetic no-SIB1 end-to-end fixture; TDD estimation from energy; OTA on an NSA cell.

### 23.6 CORESET / SearchSpace discovery
Status: PARTIAL. Goal: dedicated CORESET geometry, PDCCH identity, AL set.
Input: FEP of the monitored symbols, occupancy. Output: verified CORESET bank entries, AL profile.
Algorithm: Technique A — correlate PDCCH DM-RS in 6-RB windows (PCI-derived identity), accumulate occupancy (≥ 1000 calls, ~30 hits/window, floor 3, cap 400 000), enumerate contiguous extents (≤ 45 windows, 1035 candidates), verify with fresh same-RNTI DCIs at later slots with different payloads (≤ 4000 occasions); AL1 cover (opt-in); offline/GPU stage-1 nID sweep (65536 IDs × 14 symbols on dumped snapshots) + stage-2 geometry, handed back via `/tmp/coresets_discovered.txt` (polled by `nr_pdcch_blind_monitor_discovered_poll()`); USS AL inference from accepts.
3GPP: 38.211 §7.3.2, 38.213 §10.1. Implementation: `nr_pdcch_coreset_map.c`, `nr_pdcch_coreset_bank.c`, `nr_pdcch_al1_map.c`, `nr_pdcch_uss_tracker.c`, `nr_pdcch_ss_registry.c`, snapshot `discovery_tool/idsweep_offline.c`.
Parameters: `pdcch_blind_monitor_autodiscover`, `ISAC_COREMAP_IDSWEEP=<batches>`, `ISAC_COREMAP_*`, `ISAC_AL1_COVER`, `ISAC_PDCCH_DISCOVERY_BUDGET_US`.
Logs: `autodiscover -- CORESET footprint`, `multi-CORESET bank add`, ladder/STAGE0. Offline: PASS. SIM: OCUDU CORESET RB 0..47 dur 2 = truth. OTA: lab PASS.
Known issues: duration 2/3 ranking needs multi-symbol snapshots (partly added 2026-09-25), AL1-dominated cells (Swisscom ~99.997 % AL1) give honest ambiguity, bank not cleared on cell change, first-symbol restriction in RT scan.
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
| K7 | NSA | No NSA bed; carrier/TDD not estimated without SIB1; RNTI bootstrap without CSS0 unproven | NSA lane report | High for commercial NSA cells | Startup geometry + SIB1 cache | Build synthetic fixture; OTA on Swisscom |
| K8 | CORESET discovery | Restricted family (contiguous, dur 1, non-interleaved); AL1-dominated cells ambiguous; bank not cleared on cell change; first-symbol restriction | code + OTA notes | Medium | offline stage-1/2 tool | Multi-symbol ranking, AL1 joint consistency |
| K9 | CSI-RS | ZP fixes (probation, decoded-grant revocation) only offline; 8-RE ZP / two-hole symbols refused by design; UE-specific ZP not representable | CLOUD_REPORT (retired) | Medium | `ISAC_CSIRS_BLIND` default off | G4 on OCUDU CSI-RS bed |
| K10 | State | No unified epoch; stale winners can restore TRACKING after a gap; per-RNTI state lacks reuse epoch | architecture audit | Medium | local relearning | §19 |
| K11 | SIB1 cache | Persists across runs keyed by PCI (`/tmp/passive_rx`) → can leak a previous cell's facts | source | Medium (agnosticity) | `rm -rf /tmp/passive_rx` before acceptance runs; `ISAC_SIB1_CACHE=0` | Key by cell identity + validate |
| K12 | Launchers | Hard-coded sens6 paths/IPs/NICs/cores; `run_arm.sh` default `REPO` is another tree; `ssh sens4` gNB-log bracket; SIGKILL in watchdogs; verdicts tuned for sensing | script source | Medium | pass `REPO/BIN/DATA/MGMT/NIC` | Parameterize |
| K13 | Passivity | TX streamer is still created by the UHD backend (writes suppressed in software) | `usrp_lib.cpp`, `nr-ue.c` | Medium (legal/ethics) | tx gain 0, RU_write suppressed | Audit every TX entry point; verify no emission with a spectrum analyzer |
| K14 | UL | Transform-precoding live validation impossible on current beds (srsUE lacks TP); UL HARQ soft combining not built; UL TDRA type B only two curated rows (plus energy pin) | lane reports | Low–Medium | — | OAI SA bed with TP |
| K15 | Build | Physim targets fail to link; 12 sensing tests fail to link with `ENABLE_ISAC_SENSING=OFF` | ctest/cloud report | Low | build explicit targets | — |
| K16 | Tests | `test_vrtsim_cirdb` shm race | ctest 2026-09-30 (sens6); **passed on the DGX 2026-09-30** (race, not deterministic) | Low (upstream) | ignore | — |
| K17 | GPU | CUDA LDPC plugin 20× slower than CPU for single TBs (RTX 4060 Ti, PCIe, HISTORICAL); `CMAKE_CUDA_ARCHITECTURES=52` in cache. **DGX 2026-10-01:** GPU modules build for `sm_121` with `-DLDPC_CUDA_ARCH=121` (`build_gpu`); PDSCH GPU FEP test 9 cases OK, polar bit-exact, batched CB0-size probe work 177 → 9.9 → 6.8 µs/probe at batch 1 → 32 → 256; see K34–K36 before enabling | measurement (2026-09-2x), cache | Low | CPU LDPC | Batched GPU design (§20) |
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
| K34 | GPU LDPC | CUDA LDPC pool: a silently skipped launch (> 512 CBs) or a CUDA error leaves old bits in a reused pinned slot that the CPU CRC can accept (**false CRC pass**); request queue has no bound check; partial slot reservations can deadlock; errors only printed, no CPU fallback, waits without timeout; discrete-GPU copy model on unified-memory GB10 | review 2026-10-01, `nrLDPC_cuda/ldpc_decoder.cu` ~991-1088, `nrLDPC_coding_cuda_decoder.c` ~141-262 | **Critical** before any GPU LDPC use | keep CPU LDPC (default) | Fix before levers-spec §9.2 GPU work |
| K35 | GPU FEP | PDSCH GPU FEP worker copies the IQ ring after queueing with no lifetime re-check (stale-sample decode); self-check is diagnostics only (no tolerance gate) | review 2026-10-01, `nr_pdsch_passive_queue.c` ~654-962, `nr_pdsch_gpu_fep.cu` ~446-457 | High when `NR_GPU_FEP=1` | `NR_GPU_FEP` off (default) | Refcounted IQ or post-copy re-check |
| K36 | LDPC sensitivity | On the DGX (`ldpctest`, BG1 R1/3 K=8448, 8 it, 300 blocks) the CPU decoder (`libldpc`, same for `libldpc_orig`) needs ≈ 2–2.5 dB Eb/N0 (BLER 1.00/0.58/0.00 at 1.5/2.0/3.0 dB) while the CUDA decoder (`_cuda`, 2× iterations, flooding) reaches BLER 0 at 1.5 dB (0.33 at 1.0 dB); BG2 R1/5 same pattern | `cmake_targets/ran_build/build_gpu` runs 2026-10-01 | Medium (link margin, K4) | — | `[HYPOTHESIS]` part ARM/SIMDE-specific — run the identical `ldpctest` on x86; compare CPU at 16 iterations |
| K37 | DCI / CORESET adaptiveness | DCI length capped at 63 bits (commercial 1_1 can be longer); a locked (geometry, RNTI) length is never re-learned after a reconfiguration; one length per (geometry, RNTI); CORESET bank has no demote/remove | code review 2026-10-01 | **High** for commercial cells (Milan) | — | `docs/superpowers/specs/2026-10-01-reconfiguration-robustness-design.md` |
| K38 | Probe ≡ full at Nl>1 | `[MEASURED, DGX rfsim 273 PRB rank-4 pin49r4 4 RX]` with `ISAC_PROBE_ALL=1 ISAC_TD_PROBE_EQUIV_CHECK=2`, the probe's CB0 LLRs differ from a whole-slot decode of the same hypothesis on 68/68 sampled grants (max abs diff 165), while the CB0 CRC verdict agrees (2000/2000); with `ISAC_PROBE_HORIZON=0` 0/23 differ; the chest-row and cache-hit arms are 0/68. `[HYPOTHESIS]` a per-slot statistic in the multi-layer demod/LLR path (e.g. channel-magnitude/shift) is computed over the truncated symbol range; location not found. Consequence: P2 (levers Task 7) must not count a rank>1 probe FAIL as evidence until probe ≡ full at Nl>1 or the probe horizon is forced to 0 when Nl>1 | `tests/passive_rx/dgx_host_snapshot_2026-09-30/f1_chest_cache/` | open | levers: separate fix task before Task 7 | localize in `nr_rx_pdsch` multi-layer path, or disable horizon at Nl>1 |
| K39 | DM-RS oracle k0 | `[CODE-READ, td/convergence-levers]` the DM-RS oracle records the decoded job's **hypothesised** k0 as observed (`nr_pdsch_passive_queue.c:805` `observe(..., job.sweep_ticket.k0)`), and `obs_admits` (`nr_pdsch_config_sweep.c` ~481-492) hard-prunes every other k0. DM-RS presence in the decoded slot proves only that *some* PDSCH is there: under full-buffer traffic with allocations in adjacent slots both k0 candidates see DM-RS, so the first k0 decoded is kept — the true k0 can be pruned (then no winner until reopen). `[HYPOTHESIS]` (never observed on air); the simulator assumes a perfect k0 oracle. Found by the k0 speed-recovery investigation (`docs/superpowers/specs/2026-10-01-technique-d-k0-speed-recovery-notes.md`) | — | resolved (BC7) | blind-convergence follow-up | record k0 as observed only when the adjacent-slot ambiguity is excluded (DCI-adjacency evidence), else observe mask/last symbol without k0 | **BC7 fix (commits b35c293c55, 68cdd4729d, + round 2):** `observe()` records k0 as a *plausible* mask and prunes on DM-RS mask / last symbol only; the only k0 prune is `nr_pdsch_config_sweep_certify_k0()` (uint64 mask, k0 0..32), scoped to the (configuration, RNTI, TDA row) context and persisted per key in a bounded per-RNTI LRU set (8 entries, so several layout configurations of one RNTI coexist); inherited when a context is recreated; re-applied after observe (type-B layer / restores), `add_k0` layers and the prior-probation catalog restore; cleared on reopen (catalog rebuilt). g_obs carries no k0; the plausible mask is logged (`plaus_k0=` on CONVERGED / ORACLE_RESTORE). `nr_pdsch_config_sweep_rebuild` now clears the PRIOR/FIELD dormant masks when the catalog changes. Covered by unit tests with mutation checks of each binding line (`[MEASURED, DGX unit test]`); BC9 has not yet exercised certify_k0 in the receiver. `ISAC_TD_K0_ORACLE_LEGACY=1` restores the pin (A/B). `[MEASURED, DGX rfsim 106 PRB 1 RX]` (numbers from commit b35c293c55, the default-behaviour commit; rounds 1-2 changed no default path): same winner (tda0 k0=0, 0x804); legacy ttc 0.865/0.797 s, CRC 99.05/99.02 %; default ttc 2.538/1.962 s, CRC 97.33/97.64 % (**the default FAILS rfsim_regress.sh 2 on the 98 % CRC gate; legacy PASSES**); seg fails 461/478 legacy vs 1151/1021 default. `[HYPOTHESIS]` (review analysis, not separately measured): about half of the extra failures is the tda0 slow-down (125 vs 80 trials on the winner) and about half the mixed-slot tda2 context, which converges 7-8 s later, plus `ldpc_zero_tb` 277/251 vs 0 from k0=1 hypotheses decoding UL slot 8. `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` crc 100 % (3036/3036). Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/bc7_k39/`. `[HYPOTHESIS]` the gate failure is recovered by BC9 (TDD per-hypothesis exclusion plus DCI adjacency), not by lowering the threshold.
| K40 | TDD slot direction (PDCCH gating, BC9 PDSCH exclusion) | `[CODE-READ, td/convergence-levers @0676c372b6]` `nr_tdd_pattern.c` placed the nrofUplinkSlots right after the mixed slot and called every remaining slot of the period DL; TS 38.213 11.1 puts the nrofDownlinkSlots FIRST, the nrofUplinkSlots LAST, nrofDownlinkSymbols at the start of the slot after the last DL slot, nrofUplinkSymbols at the end of the slot before the first UL slot, and everything between FLEXIBLE (flexible symbols can carry PDCCH and a DCI-scheduled PDSCH). Whenever dl_slots + mixed + ul_slots < period the old model marked flexible slots as UL. Impact: with `ISAC_TDD_SKIP=1` (`nr_pdcch_blind_monitor_rt.c` TDD skip) and in `nr_pdcch_coreset_map.c`, PDCCH in those flexible slots was NOT scanned (missed grants), and the BC9 TDD k0 exclusion (destructive prune) would have removed legal k0 values. `[HYPOTHESIS]` not observed: the rfsim cell's 7D1S2U fills its period (old and new models agree) and the phy-test beds carry no SIB1; earlier OTA cells' patterns were not re-checked | — | resolved (BC9 fix round 1) | TDD config / PDCCH gating | per-slot shape (leading DL / trailing UL symbols) from the spec placement; FLEXIBLE slot direction; `nr_tdd_slot_has_downlink` = not a full UL slot; `nr_tdd_pdsch_last_symbol` = 13 - trailing UL symbols; stricter `nr_tdd_config_init` validation (symbols must fit their slots); pattern2 concatenated as before | **Fixed (BC9 fix round 1):** `nr_tdd_pattern.c` slot_shape(); `nr_passive_acq_tdd_slot_has_downlink` also copies the pattern under the lock. Covered by `[MEASURED, DGX unit test]` TddSpec.* (10-slot DDDFFFFFUU, DDDSU, 7D1S2U, mixed symbols in different slots, symbol fit, two patterns with flexible slots) and AcqStateTdd.FlexibleSlotsAreMonitored. Not exercised on air or in rfsim (phy-test beds carry no SIB1). Open: the PDCCH gate does not check the SIB1 reference SCS against the receiver numerology (the BC9 PDSCH exclusion does) |
| K41 | DM-RS mask oracle attribution | `[CODE-READ, td/convergence-levers]` the DM-RS oracle measures the DCI's own slot on the grant's PRBs with this RNTI's nscid/dlDmrsScramblingId (`nr_pdsch_passive_queue.c` ~846-861); DM-RS is cell-scrambled, so **another UE's PDSCH** on those PRBs is read as this RNTI's mask. `nr_pdsch_config_sweep_observe` records it and `prune_to_observed` (`nr_pdsch_config_sweep.c` ~604-615) keeps only entries admitted by `r->obs ∪ g_obs` (with `prune_commit` wiping evidence). Runtime protections: union prune (a later true-mask observation widens the set), `restore_observed_typea` (~1905/1998, type A only, catalogue k0, evidence lost), `g_obs` promotion by a second RNTI (~1979-1988). **The truth is lost persistently only if** the RNTI never measures its own mask (k0 ≥ 1 truth with no own same-PRB PDSCH in the DCI slot) **and** the true mask is never promoted to `g_obs`, or the truth is type B. `[SIMULATED, DGX host, nr_td_sim @b761021bb1]` without the union/restore protections: 82–88 / 2000 RNTIs undecidable at other-UE occupancy 0.1, 214 at 0.3 (upper bounds; BC8 round 2 models the protections). Not fixed; motivates accumulated soft oracles (blind addendum §5, lever S). **See K42:** with the round-2 protections the foreign mask's different last symbol drove a relax/re-refine thrash that wiped all evidence on every observation (1072/1072 k0 = 1 truths undecidable); BC7b made the last-symbol sets monotone, which removes the thrash but not the attribution error itself (a foreign mask still widens the admitted set and can prune a truth on a context's FIRST observation) | `tests/passive_rx/td_sim/baseline_bc0_2026-10-01.txt` (BC8 v2) | open | blind-convergence follow-up (lever S) | soft accumulated mask evidence instead of single-shot hard prune; at minimum never prune on a mask observed in a slot where another RNTI's grant overlaps (DCI history) |
| K42 | DM-RS oracle observation thrash | `[CODE-READ, td/convergence-levers @8af53c3a40]` `obs_record` relaxed a mask's last symbol to unknown on a contradicting observation and re-refined it on the next one; `restore_observed_typea` appended entries for the new (mask, last) that the following refine-prune removed again, and every size-changing `prune_commit` zeroed ALL CRC/probe/lever evidence while `context_reindexed` staled every in-flight ticket. Triggers: another UE's DM-RS on the grant's PRBs (K41) with a different last symbol, the RNTI's own PDSCH from other TDRA rows sharing a mask, energy-threshold last_sym jitter. Further wipe routes: a 9th mask (full `r->obs`) restored then pruned; BC9 `apply_cert` after a restore; and (found in BC7b) the type-B layer re-appended and re-pruned on every observation once `typeb_seen` | `[SIMULATED, DGX host, nr_td_sim @f59112743e]` (BC8 R2) 1072/1072 k0 = 1 truths undecidable with other-UE 0.1/0.3. After the fix `[SIMULATED, DGX host, nr_td_sim @44a9391975]` (fixed oracle, other-UE 0.1/0.3, --acq 500): adjacency 0: k0 = 1 undecidable 1072 -> 56/58 (rx4), 118/133 (rx1); default traffic: 1072 -> 0; wrong 0 everywhere; k0 = 0 truths unchanged to the digit. COST: k0 = 1 truths now decide but slowly (mean 413-682 s at default traffic, 540-933 s at adjacency 0; p95 up to ~2550 s), end catalogue ~256-343 hypotheses vs ~120-148 (a foreign mask now widens the admitted set for good). Legacy (k0-pin) arms dropped by controller decision (they only re-document K39). `[MEASURED, DGX unit test]` 9 PdschConfigSweepK42 tests (RED on 8af53c3a40, mutation-checked). `[MEASURED, DGX rfsim 106 PRB 1 RX, host idle]` the bed shows one last symbol per mask (F1 inert there): rfsim_regress.sh 2 FAIL (CRC 94.63 / 95.88; no K39-default run has passed since BC7); interleaved A/B pre/fix 4+4: CRC median 97.14 / 96.95 (p = 0.49), tda0 ttc 2.95 / 3.21 s (p = 0.20); identical restores, reindex/stale counts, 24-hypothesis tda0 search, outcome rate, no wipe in either. `[MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX]` 100 % (3011/3011). Evidence: `tests/passive_rx/dgx_host_snapshot_2026-09-30/bc7b_k42/`, baseline file `## BC7b` | resolved (BC7b: engine 7b5c51b115, simulator 44a9391975) | F1 `obs_set_t.lastset` (uint16, a bit per S+L-1, 0 = unknown) in `r->obs` and `g_obs`: `obs_record` ORs and never re-refines; `obs_admits` passes iff the set is empty or holds S+L-1; the g_obs promotion copies the set; the same in legacy k0-pin mode (the k0 pin is unchanged). F2: no `restore_observed_typea` when the full own set dropped the mask and g_obs lacks it. F3: entries appended by the same observe call (restore, type-B layer) that the observe-path prune or `apply_cert` removes again are truncated (`prune_commit_tail`: no wipe, no reindex); restored entries are filtered by `cert_admits`. No evidence remapping; a genuine narrowing (first observation, Qm/prior prunes, certification of existing entries) still wipes. Simulator `--obs-lastset 1` (default; 0 = round-2 model) | (a) K41 attribution itself is NOT fixed: a foreign mask still widens the set (slower convergence) and can prune a truth at a context's first observation; (b) `[MEASURED, DGX rfsim 106 PRB 1 RX]` the fix reached the earliest tda0 separation checkpoint in 0/9 runs vs 6/14 pre-fix (p = 0.048, post hoc, several looks); no engine mechanism found: re-measure with a larger interleaved A/B; (c) `add_k0` -> `apply_cert` keeps the wipe-on-append pattern (not in the BC7b scope) |

---

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
8. Re-run the gate with the DGX thresholds (98 / 1) and a campaign on the DGX; then **Track B** (§25 steps 8 onward, X410/OTA).

**Progress 2026-10-01 (DGX, after merging the cloud work):** done on the DGX — follow-up 5 (**A13 aarch64** 5/5),
follow-up 7 (**K30 races**: merged `71dbfd582a`; re-run TSAN on aarch64 still advisable), follow-up 8 first half
(**DGX gate** PASS, §14.7), A7 two consumers at 106 PRB unpinned (§14.7), A9 Step 1–2 (GPU build + tests, K17).
**Next, in this order:**
1. Levers plan (§0.6) **pure tasks 1–7, 4b, 4c** (any time) and **F1 (K32) + F2 (K33)** — correctness first.
2. Operator review of the reconfiguration-robustness spec → write its plan (K37: the 63-bit DCI cap must be lifted
   before the Milan dedicated path, Track B4).
3. K36: run `ldpctest` identically on x86 to decide whether the CPU-vs-CUDA LDPC gap is ARM-specific.
4. Remaining DGX-only follow-ups: A6 Step 5 (core map measured), A7 273-PRB pinned A/B, A8 (USS GPU, after G1 K34),
   A10, A11 timing, A12 (K22), TSAN on aarch64.
5. X410 / Track B as soon as the radio is on site (§15.4 DGX/Milan procedure).

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
