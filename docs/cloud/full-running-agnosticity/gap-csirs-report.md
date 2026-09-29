# Round 3 lane: CSI-RS coverage and simulator triage

Status: ASSESSMENT COMPLETE; no implementation, build, or radio run. Worktree `/home/sens/NICOLA/agn-wt/gap-csirs` stayed clean on `sdd/gap-csirs` at `239eb144acd398968d5869a75e083e540ddefa26`.

## Findings

3GPP TS 38.211 V17.6.0 Table 7.4.1.5.3-1 defines rows 1–18, spanning 1, 2, 4, 8, 12, 16, 24, and 32 ports. Rows 1–5 are the ordinary blind-enumerator path; rows 6–18 are a separate periodic-footprint path, enabled only by `ISAC_CSIRS_BLIND_WIDE=1` (default off). The estimator need not separate all CSI-RS ports to rate-match PDSCH: it passes a detected row/RE geometry to the existing mapper, which excludes the occupied REs.

| Rows | Ports; TS density; CDM | Blind discovery in this tree |
|---|---|---|
| 1 | 1; 3; noCDM | Enumerated |
| 2 | 1; 1 or 0.5 even/odd RB; noCDM | Enumerated |
| 3 | 2; 1 or 0.5 even/odd RB; fd-CDM2 | Density 1 only; density 0.5 is a blind-search gap |
| 4–5 | 4; 1; fd-CDM2 | Enumerated |
| 6–7 | 8; 1; fd-CDM2 | Wide path only |
| 8 | 8; 1; cdm4-FD2-TD2 | Wide path only |
| 9 | 12; 1; fd-CDM2 | Wide path only |
| 10 | 12; 1; cdm4-FD2-TD2 | Wide path only |
| 11, 13, 16 | 16, 24, 32; 1 or 0.5 even/odd RB; fd-CDM2 | Wide path only |
| 12, 14, 17 | 16, 24, 32; 1 or 0.5 even/odd RB; cdm4-FD2-TD2 | Wide path only |
| 15, 18 | 24, 32; 1 or 0.5 even/odd RB; cdm8-FD2-TD4 | Wide path only |

The wide matcher reconstructs the candidate geometry through OAI's `get_csi_mapping_parms()` / `nr_generate_csi_rs()` path. It requires periodic footprint evidence, groups jointly periodic RE cells, and keeps maximal footprints. Its own code notes that the union of two narrower row-4 resources can fit a row-6 footprint; therefore, a wide-row confirmation can over-mask PDSCH if that larger geometry is an alias. No higher-port live evidence was collected here. For density 0.5 rows, the mapper must preserve even/odd-PRB parity; the PDSCH path explicitly refuses segmented mappings whose virtual PRB ordering changes that parity.

Once a candidate is confirmed, `nr_csirs_blind_rt_rate_match[_zp]()` exports the row, bitmap, symbols, CDM, density, bandwidth and period/offset as a FAPI resource. `nr_pdcch_blind_monitor_rt.c` attaches NZP and ZP candidates to PDSCH grants on their observed occasions, refusing candidates that overlap that grant's DM-RS symbols. `nr_dlsch_demodulation.c::build_csi_overlap_bitmap()` has row-indexed time/frequency masks for rows 1–18 and maps density 0.5 by RB parity. This is source coverage; row-6–18 discovery remains opt-in and the composed high-port path has no live validation in this lane.

## Simulated gNB capability

- **OAI is the only available stack with an explicit configured emitter path above 4 ports.** `nr_radio_config.c::get_nzp_csi_rs_resource()` accepts `num_dl_antenna_ports` 8 and 12; its default layouts map these to row 6 and row 9, respectively. `gNB_scheduler_primitives.c` maps CSI-RS PDUs across rows 1–18. The same helper's default branch is fatal above 12, so the available OAI config path does not emit 16/24/32 ports. The active passive-RX fixture `rfsim-val/tests/passive_rx/gnb.sa.rfsim.conf` has no explicit high-port configuration; the rank-4 variant sets `pdsch_AntennaPorts_XP=2`, `N1=2` (four logical ports). OAI 8/12-port emission is configurable in source, but **not live-verified** by this assessment.
- **OCUDU cannot emit >4 ports through its available scheduler/config builder.** `csi_helper.cpp` rejects `params.nof_ports > 4` for both default ZP resource construction and periodic ZP resource lists. Its lower-level CSI-RS mapping table and PHY pattern code include rows up to 18, but that does not bypass the scheduler guard. The installed ZMQ config is `gnb_zmq_n78_tdd_local.yaml`; no configuration-only route above four ports was found.
- **Commercial-cell implication:** the blind receiver can represent and (when the opt-in footprint path is enabled) search 8–32-port RE patterns, but the local simulator bed can validate only the OAI 8/12-port subset by configuration. There is no available simulated-gNB route for 16/24/32-port live validation, and no wide-row live run is claimed.

Primary standard source: [ETSI TS 138 211 V17.6.0, §7.4.1.5.3, Table 7.4.1.5.3-1](https://www.etsi.org/deliver/etsi_ts/138200_138299/138211/17.06.00_60/ts_138211v170600p.pdf), pp. 109–112. The table's density and CDM combinations are standards facts; the implementation support and alias risks above are source-derived assessment.

## `test_vrtsim_cirdb` base triage

The registered test executable was re-run under sens6's `/home/sens/NICOLA/radio_bed.lock`, after the approved OCUDU gate released it. It exited `rc=1`: `CIRDBDelayDL/0` and `/1` passed, then `/2` (2x2) failed when the client `shm_open()` returned `errno 2`, followed by the server timeout. Log `/tmp/csirs-vrtsim-r3-recheck/test.log`, SHA256 `7144be52eebd5a1490fd944864d76e2c461f483b96be988fc1204ce974779ad8`; executable SHA256 `6a07925960c2c963a7c1daf2f7d0b35a59addce320b375c9c39ecb6d945b3b65`.

This is strongly supported as pre-existing at `222f98d072`, but an exact base-built rerun was not performed. The exact test source is identical at base and head (SHA256 `0ed4ab645b48cd15e58be898515702fd917eb608422cb2ac98bcaa00a9c1cce9`), and the test CMake file is identical (SHA256 `886a17ad9e2a6f948049c926aef4164be9500aaeae1fa99a6ad1e1f9543f9709`). `git diff 222f98d072..239eb144ac` is empty for `radio/vrtsim/` and `common/utils/shm_iq_channel/`; source inspection also found no change to `common/config/` or `openair1/SIMULATION/TOOLS/`. The target's link metadata lists `libvrtsim_static.a`, `libshm_td_iq_channel.a`, `libUTIL.a`, `libCONFIG_LIB.a`, simulation libraries, and utilities. The only source-tree change in broad `common/utils/` is `bits.h`; the test object's own dependency file does not include it, while `common/utils/libutils.a` includes the compiled `bits.c.o`, whose dependency file does. That header adds fields to `freq_alloc_bitmap_t`; I did not establish whether this indirect linked object affects any target code. The unrelated top-level CMake changes do not edit the `radio/vrtsim/tests` target definition.

The failure ordinal is not stable: the retained earlier CTest log on this build failed at `CIRDBDelayDL/1` with the same `shm_open` error; this rerun passed `/1` and failed `/2`. That supports a process/shared-memory setup race, not a deterministic antenna configuration failure. Since the binary was built at integration HEAD, this is not represented as a fresh binary built from `222f98d072`.

## Evidence inventory

- Lane source hashes: `nr_csirs_blind_search.c` `55610732e8557f97480c17da955ff686c6555cf5af34704f0fa85ab81d4eceac`; `nr_csirs_blind_rt.c` `356b316302906601344dbf19c92159a017bf66fe535e5e5cc2f6ac77619504bd`; `nr_dlsch_demodulation.c` `cf81a0c7bb51b7e2ffebf23e96a8adcbddf8a576a3da4a634c3b1a9793ee08b1`; `nr_pdsch_passive_decode.c` `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`; `nr_pdcch_blind_monitor_rt.c` `91cbc770ce051d671d455ab38b986d024567f132b8765afa250253d64131e9ed`.
- OAI source tree commit: `67bb0eaa117923c510beee03150c2b54dc8b3c92`. OCUDU test tree commit: `153246e00d60a9333fbb1b8073e1fae9bd9d695f`; `csi_helper.cpp` SHA256 `c5160cc4d1b1f9d31687bc26376a68ea4a7433bafe881d57f4f87b21f8f6283b`.
- Existing OAI receiver/gNB test config hash: `rfsim-val/tests/passive_rx/gnb.sa.rfsim.conf` SHA256 `64867daf01b087eaae8da7180622e6323bfeb0a0c642d6c8b8a3e461c7405b32`; rank-4 config `gnb.sa.rfsim.rank4.conf` SHA256 `42ce8671277334b77b2def37dfd35aaacb06fd17799cbf81a7cbf8671f8e40ac`.

No receiver files, test sources, configs, or shared ledgers were edited by this lane. No commit or push.

## ROUND 3b G4 — 8-port OAI phy-test live, 2026-09-28 (Claude, in progress)

Bed: sens6 phy-test, gNB `agn-wt/gnbtest` @67bb0eaa11 with `rfsim_validation/csirs8/gnb.csirs8.conf`
(= `gnbtest/tests/passive_rx/gnb.sa.rfsim.conf` + `pdsch_AntennaPorts_XP=2, N1=4, maxMIMO_layers=1`,
RU `nb_tx=8`; SHA256 7ec7b1f5…). gNB log confirms `pdsch_AntennaPorts N1 4 N2 1 XP 2`, `nb_tx_streams 8`.
Truth (gNB source `get_nzp_csi_rs_resource` case 8, validation only): NZP row 6, bitmap 60 (freq_domain
60>>2 = 15 -> k=0..7), symbol 13, density 1, fd-CDM2, period 160 / offset 0; CSI-IM pattern1 s4 (k=4..7)
symbol 13, same period. Receiver: unchanged phy-test agnostic conf `phyA2/ue.passive.q.agn.conf`, env
`ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_WIDE=1` in BOTH arms. Arms: base = `agn-wt/integ` (sdd/integration
239eb144ac, bin SHA 4ecf1e3e…), fix = `agn-wt/gap-csirs` (621a91dabe, bin SHA 4af9b9eb…, no source newer
than binary). 180 s each, alternated B/F x3, each run under `flock radio_bed.lock` + quiet_run (load/compiler
checked, verdict file). Logs `sens6:/home/sens/NICOLA/rfsim_validation/csirs8/<arm>/`.

First results (AWGN receiver channel, as every earlier phy-test run):
- base_r1 (load 1.00, VALID): crc 48667/49428; NZP CONFIRMED `3:0:106:1:13:0:1:2:0:160:0` (row 3 = CDM
  group 0 only) at slot 73682; ZP CONFIRMED `2:0:106:256:13:0:0:2:0:320:0`; no FOOTPRINT line.
- fix_r1 (load 4.77, VALID): crc 48688/49453; NZP CONFIRMED `4:0:106:1:13:0:1:2:0:160:0` (row 4, k=0..3);
  ZP CONFIRMED `3:0:106:4:13:0:1:1:0:320:160` (a NEW row-3 density-0.5 odd-RB hypothesis); no FOOTPRINT.

Root cause (source, not guessed): the rfsim AWGN model is the IDENTITY matrix
(`random_channel.c` ~1762: `ch = (aarx % nb_tx == aatx)`), so a 1-RX receiver receives gNB TX antenna 0
ONLY — CSI-RS port 0 = CDM group 0 (k=0,1). Ports 1..7 never reach it. And the sequence stage correlates
port 0 only (`ref = t_refbuf[0]`), so row 3 fd1, row 4 fd1 and row 6 fd15 score IDENTICALLY; only the wide
footprint path can separate them, and on this channel it sees just the port-0 REs. On the AWGN bed the
8-port resource is physically unobservable beyond port 0; every NZP confirmation is a strict subset
(= false resource for the "discover 8-port" claim). The ZP confirmations match no configured geometry.
Next: finish AWGN x3, then a second x3 with a static iid Rayleigh 8x1 receiver channel
(`Rayleigh1_orthogonal`, forgetfact 1; `rfsim_validation/csirs8r/`) where all 8 ports are observable.

### AWGN x3 alternated — complete (all 6 VALID, no build overlap)

| run | load start/max | crc_ok/try | accepts (all) | dci10 C-RNTI | TD ARMED/CONVERGED | drop_full | NZP confirmed | ZP confirmed | FOOTPRINT |
|---|---|---|---|---|---|---|---|---|---|
| base_r1 | 1.00/6.40 | 48667/49428 | 198420 | 568 | 1/2 | 251 | row3 fd1 k0-1 p160 | row2 k8 p320 | 0 |
| fix_r1 | 4.77/6.51 | 48688/49453 | 198568 | 624 | 1/2 | 160 | row4 fd1 k0-3 p160 | row3 d0.5-odd k4 p320/o160 | 0 |
| base_r2 | 5.85/5.91 | 48749/49514 | 198716 | 568 | 1/2 | 218 | row4 fd1 k0-3 p160 | row4 k8-11 p160 | 0 |
| fix_r2 | 5.29/7.39 | 48454/49204 | 197548 | 648 | 1/2 | 92 | row3 fd1 k0-1 p320 | row2 k5 p160 | 0 |
| base_r3 | 5.35/6.87 | 48389/49154 | 197256 | 525 | 1/2 | 136 | row3 fd1 k0-1 p320/o160 | row2 k9 p160 | 0 |
| fix_r3 | 5.73/5.89 | 48368/49138 | 197175 | 571 | 1/2 | 153 | row3 fd1 k0-1 p160 | row3 d0.5-even k4 p160 | 0 |

(accepts are nearly all rnti 0x1234 = the phy-test C-RNTI: ~211k `rnti=0x1234` lines per run; TD CONVERGED
rnti=0x1234 tda0 and tda2 every run.) CRC: base mean 48602 (98.5 %), fix mean 48503 (98.5 %) — equal
within run-to-run spread; no regression. 8-port discovery: **0/6** — every NZP confirmation is a strict
subset of the configured row-6 resource (the only sequence-visible part: port 0 / CDM group 0), the wide
FOOTPRINT path fired 0 times, and every run exported one ZP resource matching no configured ZP-CSI-RS.
Those ZP exports all sit in symbol 13 at k=2..11 of the CSI-RS slots, which on this bed really are dark at
the receiver (ports 1-7 go to TX antennas 1-7, never received; PDSCH rate-matched there), so CRC does not
suffer — but they are false w.r.t. configuration. In 2/3 lane runs the ZP winner was one of the NEW row-3
density-0.5 hypotheses (odd/even-RB k=4): the added hypotheses do get exported as ZP rate-matching on
dark REs. G4 bar for 8-port NOT met on the AWGN bed, identically for base and lane.
