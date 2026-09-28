# RFsim validation plan — full running agnosticity (no radio hardware)

Scouting only. All code lives on sens6 (`R=/home/sens/NICOLA/adaptive-rx-UL-DL`, branch
`adaptive-rx-UL-DL @ 25a8699a64`). No smoke run was executed for this scouting pass — the harness
below is already validated by its own README + multiple prior sessions (2026-07-27 through
2026-09-25, per memory and `tests/passive_rx/README*.md`), so a fresh smoke run would only re-confirm
what is already source- and log-verified. Run one before trusting a specific arm's numbers.

## 1. Harness

**Use `tests/passive_rx/`, NOT `tests/sensing_sim/`.** `tests/sensing_sim` is `--do-ra`-based, and
`--do-ra` is mutually exclusive with `--passive-rx` (SA-only cell-search/MIB/SIB1 path) — confirmed
in `tests/passive_rx/README.md`'s "Why NOT tests/sensing_sim" section.

### Processes (all rfsimulator, one cell)

| process | role |
|---|---|
| `nr-softmodem -O <gnb.conf> --rfsim` | SA gNB, rfsim **server**, NGAP to a local open5gs core |
| `nr-uesoftmodem -O ue.active*.conf` | ACTIVE UE(s) — full RA→RRC→NAS attach, carries iperf3/UDP traffic, own open5gs subscriber per UE (`ue.active.conf`/`ue.active2.conf`/`ue.active3.conf`, `ue.active.r4.conf` for rank-4) |
| `nr-uesoftmodem --passive-rx -O ue.passive*.conf` | PASSIVE receiver(s) — never transmits, never attaches (`UE_RECEIVING_SIB` forever; every UL trigger gates on `state >= UE_PERFORMING_RA`), decodes the active UE's PDCCH/PDSCH/CSI-RS blind |

Why it works: `radio/rfsimulator/simulator.cpp`'s `rfsimulator_write_internal()` writes the identical
downlink sample block to every connected client socket — a second/third client sees bit-for-bit what
the active UE sees, including PDCCH/PDSCH/CSI-RS addressed to someone else. This is the entire basis
for testing an agnostic receiver without radio hardware: the gNB emits real, spec-correct signalling
for the active UE, and the passive receiver has to blind-discover it exactly as it would OTA.

### Launch

```bash
cd /home/sens/NICOLA/adaptive-rx-UL-DL   # NOT the openairinterface5g checkout named in old docs
./tests/passive_rx/run_passive_rx.sh [duration_s] [out_dir]     # default 90s, /tmp/passive_rx
```

Key env vars (from the script header): `NUM_RX` (1-3 passive receivers), `NUM_UE` (1-3 active UEs,
each own RNTI/iperf3 stream — more UEs = more distinct RNTIs in flight, closer to a real multi-UE
cell), `IPERF_RATE` (per-UE DL rate, default 3M — raises grant density directly), `RX1/2/3_NANT`
(RX antennas per receiver, default 4, needed for AoA/rank tests), `BW100` (0=106 PRB
`gnb.sa.rfsim.conf`, 1=273 PRB `gnb.sa.rfsim.100mhz.conf`), `CONF_TAG` (selects a receiver-conf
variant: `.aoa`, `.upa`, `.blind`, `.auto`, `.autor4`, `.q`, `.pin49*`), `GNB_EXTRA` (extra gNB CLI
flags, e.g. `--telnetsrv --telnetsrv.shrmod ci` for the BWP-switch test), `PIN=0` (must stay 0 —
taskset pinning crashes netns'd UE startup, measured).

Confs already in `tests/passive_rx/` cover most of what this plan needs without new files:
`gnb.sa.rfsim{,.100mhz}.conf` (baseline, 106/273 PRB), `gnb.sa.rfsim{,.100mhz}.rank4.conf`
(`pdsch_AntennaPorts_XP=2,N1=2,maxMIMO_layers=4`), `gnb.sa.rfsim.bwp.conf` (dedicated BWP at CRB 30,
second BWP at CRB 70 for the telnet switch test), plus receiver confs for AoA (`*.aoa*.conf`, 4-elem
λ/2 ULA), blind-PDCCH-only (`*.blind*.conf`), and auto-discovery (`*.auto*.conf`,
`*.autor4*.conf`, `*.q*.conf`, `*.pin49*.conf` — believed to correspond to the "pin49r4" rank-4 phy-
test bed the memory files reference, re-purposed onto the SA+core harness).

### Ground truth

Read from the **gNB's own log** (`nr-softmodem`'s stdout/`gnb.log`), resolved from the running
process (`/proc/<pid>/fd`, per memory `live-gnb-config-is-sens4-home-sens-gnb-yaml`/
`gnb-rnti-recheck-every-prompt` conventions — RNTI changes every re-attach): C-RNTI assignment,
per-slot scheduling grants (`NR_MAC` debug: `grep "Scheduling CSI-RS in frame"`,
`gNB_scheduler_dlsch`/`ulsch` grant lines), MCS/TDA/DCI-format actually used. **Never seed the
receiver from this — validation only**, per the project's own agnosticity rule.

### Scoring — receiver log lines (all `LOG_A(PHY, ...)`, i.e. always-on, not debug-gated)

| Line (grep pattern) | File:line | Means |
|---|---|---|
| `Technique D CONVERGED rnti=... tda=... S=... L=... mask=... table=...` | `nr_pdcch_blind_monitor_rt.c:6452`, `nr_pdsch_passive_queue.c:1099` | PDSCH TDRA/DM-RS-mask/MCS-table hypothesis locked for that RNTI |
| `multi-CORESET bank add index=... offset=... span=... symbol=... mapping=.../.../... len=...` | `nr_pdcch_coreset_bank.c:175` | a new dedicated CORESET geometry discovered and banked |
| `CSIRS_BLIND CONFIRMED[...] after N slots -- ... z=... hits=...` / `CSIRS_BLIND CONFIRMED after N slots -- csirs_monitor = "..."` | `nr_csirs_blind_rt.c:160,165` | blind CSI-RS candidate confirmed (periodic hits above threshold) |
| `pdsch_decode[try=... crc_ok=... (X%) data_submits=...]` | (existing passive-decode summary) | TB-CRC pass rate for the data-aided path |
| `occ[csi=... dmrs=... data=... blind=...]` | (sensing occupancy line) | which RS sources are actually contributing rows/CPI |
| `accepts=N cfr_submits=N` | blind-PDCCH periodic status | blind candidates accepted / CFR taps fired |
| `JOINT_RNTI_HIT` | joint solver | RNTI recovered jointly with payload, cross-checked against the gNB's own log |

Also useful, per-feature, once a task's report names its own line (Technique D's `table=` field
distinguishes the MCS table hypothesis; AL1 cover work logs its own cover-size/lane lines — check
each `task-N-report.md` for the exact string before scoring, several are new since 2026-09-25).

### Duration / convergence — measure, don't assume

No task in this plan's audit has been run live on rfsim yet (`task-10-report.md`,
`task-15-report.md`, `task-16-report.md` all say "nothing has run live"). Rough anchors from prior
*similar* rfsim runs on this harness (CLAUDE.md / memory, not this plan's new features):
attach + first CSI-RS/blind-PDCCH activity: seconds. `Technique D CONVERGED` on one RNTI under
iperf3 traffic: historically tens of seconds to ~2 min once the right table/TDRA/mask survive enough
grants. `CSIRS_BLIND CONFIRMED`: needs 3 periodic hits at the resource's own period (160 slots = 80ms
stock, so ~3-15s once pinned — Tasks 1-3's pinning fix, already committed, exists specifically to
make this fast rather than "minutes" as the un-pinned round-robin measured). A cell-wide data-
scrambling-ID walk (`ISAC_*` gated) needs ≥20 consecutive CRC fails per RNTI before it even starts,
worst case up to 1024 further LDPC trials — could be minutes if the walk is exercised at all (only
fires if the injected scrambling ID actually differs from PCI). **Set each arm's duration by watching
for its own convergence line via `Monitor`, not a fixed clock** — this project's own house rule.

## 2. Feature → gNB config matrix

Evidence is file:line on `adaptive-rx-UL-DL @ 25a8699a64`, `openair2/LAYER2/NR_MAC_gNB/` unless noted.

| # | Feature | Status | Where / how |
|---|---|---|---|
| a | PDSCH **RA type 0** / dynamicSwitch | **NOT POSSIBLE** (without a large scheduler rewrite) | `nr_radio_config.c:1745` hardcodes `pdsch_Config->resourceAllocation = resourceAllocationType1`; `gNB_scheduler_primitives.c:1372` `AssertFatal(pusch_Config->resourceAllocation == ...Type1, ...)` — the **scheduler itself** only ever computes contiguous RIV allocations, never an RBG bitmap. Flipping the RRC field alone would tell the UE to expect type-0 grants the scheduler never produces. Confirmed by the plan itself: Task 10's report says "there is no gNB config in this lane to produce it" and the receiver-side PUSCH RA-type-0 decode is explicitly not implemented either. |
| a | PUSCH RA type 0 / dynamicSwitch | **NOT POSSIBLE** | Same root cause, `nr_radio_config.c:1656` + `gNB_scheduler_primitives.c:1357`. |
| b | Interleaved VRB-to-PRB (bundle 2/4) | **NEEDS SMALL GNB PATCH** | `pdsch_Config->vrb_ToPRB_Interleaver` is read (`gNB_scheduler_primitives.c:1772-1775`: NULL→non-interleaved, else→interleaved) but **never assigned anywhere in the tree** (grep for the setter finds nothing) — it is permanently NULL. Patch: `calloc` + set to `n2`/`n4` in `nr_radio_config.c` next to the `prb_BundlingType` block (~line 1747). |
| c | PRB bundling / PRG size | **NEEDS SMALL GNB PATCH** | `nr_radio_config.c:1747-1750` hardcodes `prb_BundlingType.present = staticBundling`, `bundleSize = wideband`. No `.conf` key exists for it (`gnb_paramdef*.h` has no `bundl*`/`PRG` string). Patch: make `bundleSize` a config value (n2/n4) or just hardcode the other choice for a test build; for `dynamicSwitch` also flip `.present` to `dynamicBundling`. |
| d | `dataScramblingIdentityPDSCH` / DM-RS `scramblingID0`/`scramblingID1` ≠ PCI (DL) | **NEEDS SMALL GNB PATCH** | All four hardcoded `= NULL` in `nr_radio_config.c` (lines 1738-1739 DM-RS ID0/1, 1744 data-scrambling). NULL means "use PCI" per spec. Patch: `calloc` + assign a value ≠ PCI at each site — four mechanical 2-line edits. |
| d | `dataScramblingIdentityPUSCH` / UL DM-RS `scramblingID0`/`scramblingID1` ≠ PCI | **NEEDS SMALL GNB PATCH** | Same pattern, UL side: `nr_radio_config.c:1592` (data), `:1616-1617` (DM-RS ID0/1), all NULL. |
| e | PDSCH mapping type B, k0 ≥ 2 | **NEEDS SMALL GNB PATCH** (pattern already exists in-file) | `nr_rrc_config_dl_tda()` (`nr_radio_config.c:1036-1074`) hardcodes `mappingType = typeA` on every DL TDRA entry it builds (up to 3: basic, CSI-RS-adjusted, TDD-mixed-slot) and never sets `k0` (commented out, "UE applies 0" by omission). The exact pattern needed already exists 40 lines below for PUSCH: `set_TimeDomainResourceAllocation(k2, sliv)` (`:1079-1086`) builds a `mappingType = typeB` entry with an explicit `k2`. Patch: add one more `asn1cSeqAdd`'d entry to the DL list with `mappingType=typeB` and `k0=calloc(...); *k0=2` (or higher), mirroring the UL helper. |
| e | PUSCH mapping type B | **ALREADY DEFAULT — no patch, no config change needed** | `set_TimeDomainResourceAllocation()` (`nr_radio_config.c:1079-1086`) sets `mappingType = typeB` **unconditionally, on every PUSCH TDRA entry it builds**. This is why CLAUDE.md's §12 already found "two curated rows (S,L)=(0,4) and (2,12)" on the live cell — the gNB was already emitting type B before any receiver work started. |
| e | PUSCH TDRA k2 values | **CONFIGURABLE (existing)** | Multiple `k2` values are already constructed and sorted (`tda_cmp`, `:1088+`) — this is already exercised by the existing UL interpolation sweep per the plan's own audit table. |
| f | UE-specific SS aggregation level candidates (incl. AL1-only, AL16) | **NEEDS SMALL GNB PATCH** (single value-table edit) | `gnb_config.c:1606-1611`: `uess_num_agg_level_candidates` (the dedicated/UE-specific search space, what DCI 1_1/0_1 actually use) is hardcoded `AL1=n0, AL2=n2, AL4=n0, AL8=n0, AL16=n0` — i.e. **this gNB only ever schedules AL2** on the dedicated SS. No `.conf` key found (`ss2_n_candidates`, mentioned in a memory file, belongs to a *different* gNB codebase — OCUDU — not this OAI tree). Patch: edit the 5 constants directly (e.g. `AL1=n1` for an AL1-only arm, `AL16=n1`/`n2` for an AL16 arm) and rebuild `nr-softmodem`. This is exactly the value table the `AL1_COVER`/AL16 work (Tasks 4/5/15) needs real air-interface exercise for. |
| f | Common SS aggregation levels | n/a for this plan | Separate hardcoded table (`nr_radio_config.c:1814-1818`, `gnb_config.c:932-937`) drives CORESET#0/SIB1 — not the dedicated-CORESET discovery this plan targets. |
| g | CSI-RS NZP, 1/2/4/8 ports | **CONFIGURABLE (8p likely untested here)** | `get_nzp_csi_rs_resource()` switch (`nr_radio_config.c:429-475`) implements `case 1` (row 2), `case 2` ("other"/fd-CDM2, ≈row 3), `case 4` (row 4), `case 8` (fd-CDM2 "other", p8) — driven by `num_dl_antenna_ports`, itself from `pdsch_AntennaPorts_{N1,N2,XP}` in the `.conf`. 8-port has a code path (`case 8`) but no run in this plan's history has exercised it — treat as CONFIGURABLE but unverified until run once. |
| g | CSI-RS NZP, 12 ports | **CONFIGURABLE (unverified)** | `case 12` exists in the same switch, same caveat. |
| g | CSI-RS NZP, 16/32 ports (rows 13-18-ish) | **NOT POSSIBLE without a GNB PATCH** | `default: AssertFatal(false, "Number of ports not yet supported")` — the gNB **crashes** rather than emitting anything above 12 ports. Patch would need new `case 16`/`case 32` branches with the correct row/`frequencyDomainAllocation`/`cdm_Type`/`density` per TS 38.211 Table 7.4.1.5.3-1 rows 13-18 — a real, non-trivial addition (more than the mechanical patches above; closer to "small-to-medium"). |
| g | CSI-RS rows 1 and 5 specifically | **NOT POSSIBLE without a GNB PATCH** (same function) | `case 1` (1 port) hard-routes to row 2 only, never row 1; `case 4` hard-routes to row 4 only, never row 5. `tests/passive_rx/README.md` already documents this exact gap for the *receiver's* blind enumerator — this table confirms it is the **gNB emitter**, not the receiver, that has no path to rows 1/5. |
| h | MCS table `qam256` | **CONFIGURABLE (existing)** | `nr_radio_config.c` sets `pdsch_Config->mcs_Table`/`pusch_Config->mcs_Table` to `qam256` conditionally (gated on UE capability unless `force_256qam_off` — `GNB_CONFIG_STRING_FORCE256QAMOFF`, `gnb_paramdef*.h:86` — is set). Already exercised per CLAUDE.md's 256QAM/rank-4 work. |
| h | MCS table `qam64LowSE` | **NOT POSSIBLE without a GNB PATCH** | Zero references to `qam64LowSE` anywhere in `openair2/LAYER2/NR_MAC_gNB/` — the gNB never constructs this table. Would need new code, not a config flip. |
| i | Rank up to 4 | **CONFIGURABLE (existing, validated)** | `maxMIMO_layers` (`GNB_CONFIG_STRING_MAXMIMOLAYERS`) + `pdsch_AntennaPorts_XP/N1` already used in `gnb.sa.rfsim{,.100mhz}.rank4.conf` (`maxMIMO_layers=4`). rfsim channel-model bug that dropped layers >1 (`rfsim-channel-nb-tx-from-own-tx-count` memory, fixed `fc53bf920f`) is resolved — the client channel now sizes to the peer's actual TX count. Passive receiver needs `--ue-nb-ant-rx 4` (`RX1_NANT=4` in `run_passive_rx.sh`, default already 4). rfsim has no hardware antenna-count ceiling (unlike the X410's 4-channel real limit) — validated to 4x4 per memory (`rank4-ota-converged`/`lbrm-n-l-is-the-rank4-mcs25-wall`), higher untried but not blocked by rfsim itself. |
| j | Non-zero dedicated BWP start (BWPStart ≠ 0) | **CONFIGURABLE (existing, validated)** | `gnb.sa.rfsim.bwp.conf`'s `bwp_list = ({bwpStart=30; bwpSize=40;}, {bwpStart=70; bwpSize=24;})` + `first_active_bwp=1`, consumed at `nr_radio_config.c` (the `configuration->bwp_config[...]` / `location_and_bw` branch, ~line 1780). This is the exact conf the "passive BWP tracking" work (memory `passive-bwp-tracking-and-phy-test-bed`) already built and validated offline against (17/17 gtests); `GNB_EXTRA="--telnetsrv --telnetsrv.shrmod ci"` + `ci trigger_bwp_switch <id> <rnti>` (port 9090) exercises a live BWP switch. |

**Tally: 4 CONFIGURABLE-already (i, j, PUSCH-type-B, qam256) + 2 CONFIGURABLE-but-unverified (8p, 12p
CSI-RS) / 7 NEEDS SMALL GNB PATCH (b, c, d×2, e-DL-typeB, f) / 1 NEEDS PATCH non-trivial (g 16/32-port,
rows 1&5 combined as one item since same function) / 3 NOT POSSIBLE without scheduler-level rework
(a×2, h-qam64LowSE).**

## 3. Constraints

- **CPU**: sens6 has 12 cores. A single arm already runs 4-6 processes concurrently (gNB + open5gs +
  1-3 active UEs + 1-3 passive UEs). 273 PRB/100 MHz rfsim is documented running at **~5-6% of real
  time with 4 concurrent processes** (CLAUDE.md §10, 100 MHz batch note) — budget ~20x wall-clock vs
  desired simulated time at that bandwidth. 106 PRB is materially lighter (the default
  `tests/passive_rx` harness uses it and reports normal-feeling 90s runs) but was not itself
  benchmarked against real time in anything read for this scout — measure per arm.
- **Parallel arms**: technically possible (`rfsimulator` server port and every `.conf`'s
  `report_path`/`out_path` are per-instance, so two independent gNB+UE+passive-UE triples on distinct
  ports don't collide on disk or network). **Not recommended by default**: a single arm already
  saturates a meaningful fraction of 12 cores, and CLAUDE.md explicitly records CPU contention
  degrading cross-receiver CPI alignment and causing ping/iperf3 stalls under "4-5 concurrent softmodem
  processes" in a single-arm run — doubling that with a second arm is very likely to push both into
  the pathological regime rather than just running slower. Run arms **sequentially**; only pair two
  arms if both are single-UE/single-receiver "does it even come up" checks (BWP start, rank, MCS table)
  rather than a Technique-D-convergence arm.
- **Convergence time**: see §1 — no fixed number exists yet for any of this plan's *new* features,
  because none has been run live. Budget short (2-5 min) smoke checks first for every CONFIGURABLE
  item, watch for the item's own log line via `Monitor`, and only commit to a long (10-20 min) capture
  once the line is known to fire at all on that arm.
- **Rebuild cost**: every "NEEDS SMALL GNB PATCH" item requires rebuilding `nr-softmodem` (not just the
  receiver), and per the branch's own stale-plugin trap, `sensing_channel.c`/`usrp_lib.cpp`-style
  dlopen'd modules aside, a plain `make -j12 nr-softmodem` after editing `nr_radio_config.c`/
  `gnb_config.c` is a normal relink — cheap, but must happen with **no receiver process running**
  (`pgrep -x nr-uesoftmodem` check first, per the project rule).

## 4. Proposed arm list

Baseline first, then one feature per arm. `R=/home/sens/NICOLA/adaptive-rx-UL-DL`; all launched via
`cd $R && ./tests/passive_rx/run_passive_rx.sh <duration> <out_dir>` with the env vars shown, unless a
gNB patch is required first (marked).

| Arm | Knobs | Conf | Pass criterion | Notes |
|---|---|---|---|---|
| 0. Baseline | defaults | `gnb.sa.rfsim.conf` + `ue.passive.conf` | `occ[...]` non-zero, `Technique D CONVERGED` on the one active RNTI, CSI-RS blind confirms | Re-establishes the harness is healthy before attributing any later arm's failure to its feature. |
| 1. Rank 4 | `BW100=0 or 1`, rank-4 confs | `gnb.sa.rfsim{,.100mhz}.rank4.conf` + `ue.passive.autor4.100mhz.conf`, `RX1_NANT=4` | Per-RNTI Technique D converges at `Nl` up to 4 in the CONVERGED log; PDSCH CRC rate vs gNB's own MCS/rank log | Already-configurable, already partly validated per memory — re-confirm on THIS branch/commit, not inherited from an older session. |
| 2. Dedicated BWP, non-zero start | `GNB_EXTRA="--telnetsrv --telnetsrv.shrmod ci"` | `gnb.sa.rfsim.bwp.conf` + base passive conf | passive BWP-tracking module reports the correct `(start,size)` on the CRB-30 and CRB-70 BWPs; `ci trigger_bwp_switch` exercises the live switch | Already-configurable; existing gtests (`nr_passive_bwp_test.cc`) cover the logic offline — this is the first LIVE rfsim exercise per the plan's own audit. |
| 3. AL1-only dedicated SS | **gNB patch**: `gnb_config.c:1606-1611` `uess_num_agg_level_candidates[AL1]=n1` (others 0) | patched `nr-softmodem` + base conf, `ISAC_AL1_COVER=1` | AL1 cover finds the CORESET (`multi-CORESET bank add` at AL1) and Technique D converges via AL1-only candidates | First live exercise of Tasks 4/5/15's AL1 cover — currently offline-only. |
| 4. AL16 dedicated SS | **gNB patch**: same table, `AL16=n1` or `n2` | patched `nr-softmodem` + base conf | AL16 candidates decoded, CRC-verified against gNB log | Tests the AL16 extension from Task 15; also offline-only today. |
| 5. Interleaved VRB-to-PRB | **gNB patch**: `nr_radio_config.c` set `vrb_ToPRB_Interleaver = n2` (then `n4`) | patched `nr-softmodem` + base conf, receiver's interleaved-VRB logic (existing) | Passive PDSCH CRC stays high with interleaving active vs a same-length run with it off | Two sub-arms (n2, n4). |
| 6. PRB bundling / PRG | **gNB patch**: `prb_BundlingType` → `dynamicBundling` with n2/n4, or static n2/n4 | patched `nr-softmodem` + base conf, `ISAC_PRG_SWEEP=1` (default on) | Task 8/9/12's PRG-boundary segmentation converges to the right bundle size instead of falling back to wideband | Also exercises the `SegmentsFollowDataOrderAndPrgBoundaries` review-focus item live for the first time. |
| 7. DM-RS/data scrambling ID ≠ PCI (DL) | **gNB patch**: 4 sites in `nr_radio_config.c` set non-NULL non-PCI values | patched `nr-softmodem` + base conf | Task 13's DM-RS coherence estimator and data-ID walk both converge to the injected (non-PCI) value, confirmed against the patched constant | High-value arm: this is the ONE feature CLAUDE.md flags as "a wrong scrambling decision is silently fatal" — worth a dedicated long run. |
| 8. Same, PUSCH/UL | **gNB patch**: 3 sites, UL side | patched `nr-softmodem` + base conf, `pdcch_blind_monitor_ul_thread` set | Same convergence, UL data path | |
| 9. PDSCH mapping type B, k0≥2 | **gNB patch**: extra `NR_PDSCH_TimeDomainResourceAllocation` entry in `nr_rrc_config_dl_tda()`, `mappingType=typeB`, `k0=2` | patched `nr-softmodem` + base conf, `ISAC_PDSCH_TYPEB=1`, `ISAC_PDSCH_K0_PROBE=1` | Task 14's type-B/k0≥2 path converges; DM-RS mask oracle correctly infers the type-B symbol layout | PUSCH type B needs **no patch** — already default; can be tested against arm 0 as a control that the existing k2 sweep already handles it. |
| 10. CSI-RS 8-port | `pdsch_AntennaPorts_XP*N1=8`, `nb_tx=8` in `.conf` | modified `gnb.sa.rfsim.conf` (no code patch — `case 8` exists) | `CSIRS_BLIND CONFIRMED` on an 8-port resource; blind port-count inference matches | First live use of an existing-but-unexercised code path — cheap, do this early. |
| 11. CSI-RS 12-port | Same, `=12` | modified conf | Same | |
| 12. MCS table qam256 (DL+UL) | `force_256qam_off=0`, capability negotiated | base rank confs already do this | Qm oracle (Task 6) correctly infers 256QAM without waiting on hundreds of TB-CRC trials | Already partly exercised per CLAUDE.md 256QAM notes — re-run to specifically score the Qm-oracle convergence speed, not just end CRC. |
| 13. Multi-UE / RNTI churn | `NUM_UE=3`, `IPERF_RATE` raised | base conf | Joint solver / RNTI-cache eviction fix (Task 1) holds all 3 RNTIs' state without evicting the evidence-bearing one | Exercises the `RNTI_CTX_MAX 16→64` fix under real multi-RNTI load for the first time on this branch. |

## 5. Features that cannot be exercised on rfsim at all, and why

- **RA type 0 / dynamicSwitch (PDSCH and PUSCH)** — not a rfsim limitation; the OAI gNB scheduler
  itself never computes an RBG-bitmap allocation, on any transport (rfsim or a real radio). This would
  need to be fixed on a **real commercial gNB** (which does use type 0 routinely) — i.e. the OTA target
  for this feature is a live macro/lab cell from a different vendor, never OAI-on-anything. rfsim
  cannot help either way; this is purely a receiver-side capability to build against captured OTA IQ
  or a future non-OAI test gNB, not something to validate against this branch's own gNB.
- **MCS table `qam64LowSE`** — same shape: OAI's gNB never emits it on rfsim or real hardware. Needs a
  non-OAI illuminator (an actual `qam64LowSE`-configured commercial cell) to validate against; rfsim
  is a non-starter regardless of effort spent.
- **CSI-RS rows 6-18 / 16-32 ports** — technically fixable with a gNB patch (§2, "not trivial"), so not
  permanently blocked, but as shipped today the gNB `AssertFatal`s before emitting anything above 12
  ports or outside rows {2,3,4} — no rfsim run can exercise it until that patch lands.
- **Everything AoA/multi-antenna beyond 4 receive channels** — not applicable to *this* plan's scope,
  but worth noting since rank/CSI-RS-port arms above interact with it: rfsim itself has no antenna-
  count ceiling (unlike the X410's real 4-channel limit), so this is not an rfsim constraint at all —
  if a future feature needs >4 RX, rfsim can still provide it; only the X410 hardware cannot.

## Answer to the assignment's three questions (recap)

1. Harness found: **yes** — `tests/passive_rx/` (SA gNB + open5gs core + active UE(s) + `--passive-rx`
   receiver(s), all rfsimulator). `tests/sensing_sim` is confirmed unusable for this purpose
   (`--do-ra` vs `--passive-rx` are mutually exclusive).
2. Feature counts: **4 already CONFIGURABLE** (rank≤4, non-zero BWP start, PUSCH mapping type B,
   MCS qam256) **+ 2 CONFIGURABLE-but-never-run** (CSI-RS 8/12-port) **/ 7 NEED A SMALL GNB PATCH**
   (interleaved VRB-to-PRB, PRB bundling/PRG, DL+UL data/DM-RS scrambling IDs ≠ PCI, PDSCH type-B/k0≥2)
   **+ 1 NEEDS A LARGER (non-trivial) PATCH** (CSI-RS 16/32-port + rows 1/5) **/ 3 NOT POSSIBLE on OAI
   at all** (PDSCH RA type 0, PUSCH RA type 0/dynamicSwitch, MCS qam64LowSE).
3. Report path: `/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity/rfsim-validation-plan.md`
