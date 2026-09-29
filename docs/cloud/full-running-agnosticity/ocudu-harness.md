# OCUDU gNB + srsUE + agnostic passive OAI receiver over ZMQ (sensnuc3)

Status: **IN PROGRESS** — bring-up PASS (M1-M3); blind dedicated CORESET/C-RNTI/DCI lengths correct; Technique D
never converges on rfsim-val (root cause below, fixed on sdd/integration); integration-receiver runs next.

## Files (rfsim-val, uncommitted)
| file | what |
|---|---|
| `tests/passive_rx/run_ocudu_passive.sh` | start/stop everything in order; `run_ocudu_passive.sh [DUR] [RUN_DIR]`, `run_ocudu_passive.sh stop` |
| `tests/passive_rx/ocudu/ocudu_zmq_broker.py` | ZMQ sample broker (python3 + pyzmq 24 + numpy) |
| `tests/passive_rx/ocudu/gnb.ocudu.zmq.yaml` | OCUDU gNB: `gnb_zmq_n78_tdd_local.yaml` + rx_port → broker |
| `tests/passive_rx/ocudu/ue.srsue.ocudu.conf` | srsUE: `ue_zmq_n78_siso.conf` + rx_port → broker, log path |
| `tests/passive_rx/ocudu/ue.passive.ocudu.conf` | passive: `ue.passive.agn.conf` minus rfsim/sensing_channel, minus csirs_monitor (RRC-dedicated), 1 antenna |

Build change: `rfsim-val/cmake_targets/ran_build/build` reconfigured with `-DOAI_ZMQ=ON`, `ninja oai_zmqdevif`
→ `liboai_zmqdevif.so` (no other target rebuilt).

Core: local open5gs (AMF 127.0.0.1:38412, PLMN 001/06, TAC 1). Subscriber **IMSI 001060123456748** added to
mongodb (clone of ...743: same K/OPc). No systemctl used.

## Cell
n78, 20 MHz / 51 PRB, SCS 30 kHz, **23.04 Msps** (OCUDU srate 23.04 = OAI `-r 51 -E`, FFT 768), PCI 1,
dl_arfcn 627666 (3414.99 MHz), SSB ARFCN 627360, offsetToPointA 4, k_SSB 18 → OAI `--ssb 33`
(24 + 9 subcarriers @30 kHz), TDD 6D/3U + 8 DL symbols, pdsch qam256, pusch qam64.
Passive command line carries only cell-level/public inputs:
`nr-uesoftmodem -O ue.passive.ocudu.conf --passive-rx -E -r 51 --numerology 1 --band 78 -C 3414990000 --ssb 33
 --device.name oai_zmqdevif --zmq.[0].tx_channels tcp://127.0.0.1:2201 --zmq.[0].rx_channels tcp://127.0.0.1:2200`

## Protocol compatibility (verified in source)
OAI `radio/zmq` is a port of OCUDU `lib/radio/zmq`: cf32 on the wire, transmitter binds REP, receiver connects
REQ and sends a 1-byte request, reply = whatever samples are queued. srsRAN 4G rf_zmq (srsUE) is the same.
Per-message limits the broker respects: OAI rx 300000 samples, OCUDU rx 614400, srsUE 3072000.
OAI scales ±1.0 ↔ ±32767 int16. OAI keeps TX writes under `--passive-rx` because the zmq driver sets
`IS_SOFTMODEM_RFSIM` (nr-ue.c:338), so the passive's TX stream exists and must be drained.

## Broker design
```
gNB TX REP :2000  --REQ-->  broker --REP :2100--> srsUE RX     DL verbatim
srsUE TX REP :2001 --REQ--> broker --REP :2101--> gNB RX       UL verbatim
                            broker --REP :2200--> passive RX   DL[k] + UL[k]  (TDD: never both active)
passive TX REP :2201 --REQ--> broker (sink, zeros discarded)
```
- No timestamps on the wire: time = sample index, every lane forwarded 1:1. gNB-RX index k == srsUE-TX index k
  by construction, so DL[k]+UL[k] is an ideal co-located observer.
- gNB↔srsUE stays a closed lockstep loop (srsUE stalls ⇒ gNB stalls, as with a direct link).
- Passive pacing: OAI's RX poll thread requests continuously into a 1 s ring that overflows silently, so the
  broker uses the passive's TX sink as a consumption clock (OAI pushes exactly as many zeros as its RX consumed)
  and never lets sent−consumed exceed `--px-window` (0.1 s).
- Backpressure: while the passive is live and falls `--px-hwm` (0.3 s) behind, the broker stops pulling DL from
  the gNB ⇒ the whole cell runs at the passive's speed, no samples lost.
- Stall: no passive progress for `--px-stall` (30 s) with data waiting ⇒ lane dropped, gNB/UE loop continues.
  A (re)started passive re-opens the lane at the current DL index (logged as DISCONTINUITY). So the passive
  alone may be restarted; gNB and srsUE must still be restarted together.
- `--no-ue`: gNB UL synthesized as zeros up to the DL index (passive sync with no UE).
- Gains: `--px-gain-db`, `--px-ul-gain-db` (calibration knobs for the passive lane only).
- Offline self-test (fake gNB/UE/passive, scratchpad `broker_selftest.py`): UL alignment at gNB exact,
  passive DL[k]==UL[k] on every sample, late join + passive death (lane dropped, loop kept running) + restart:
  **PASS**.

## Run order (script)
guards (refuses if run_passive_rx/any softmodem/gnb/srsue running or ports busy) → broker → gNB (sudo) → wait
"Connection to AMF" → [PX_FIRST=1: passive] → srsUE (sudo, netns ue1) → wait "PDU Session Establishment
successful" → passive → wait "SIB1 decoded" → traffic (udp_dl.py host→UE at DL_RATE, ping UE→10.45.0.1) →
supervised dwell → stop (traffic, passive, gNB+srsUE together, broker) → summary.txt.

## Procedure (copy-paste)
```bash
cd /home/sens/NICOLA/rfsim-integ/tests/passive_rx        # or rfsim-val (baseline receiver)
./run_ocudu_passive.sh 240 /tmp/ocudu_passive/<name>     # blocks ~DUR+40 s, writes <name>/summary.txt
./run_ocudu_passive.sh stop                              # teardown if a run was interrupted
python3 ocudu/score_ocudu_run.py /tmp/ocudu_passive/<name>   # per-run score vs OCUDU truth
# variants: PX_FIRST=1 (passive before attach)  PX=0 (no passive)  NO_UE=1 (passive + gNB only)
#           GNB_EXTRA='cell_cfg pdcch dedicated --ss2_n_candidates 0 0 4 2 0'  SRSUE_EXTRA='--rf.tx_gain=10'
```
Prereqs (one-time, already done on sensnuc3): build dir configured with `-DOAI_ZMQ=ON` and `ninja oai_zmqdevif`;
IMSI 001060123456748 in open5gs mongodb; `sudo -n` works; netns `ue1` is created by the script.
Run-dir contents: `broker.log gnb.yaml gnb.log gnb_stdout.log f1ap.pcap srsue.log srsue_stdout.log passive.log
udp_*.log ping_ul.log summary.txt`. **Do not edit run_ocudu_passive.sh while a run is in progress** (bash reads
it incrementally). **Do not build during a run.**

## Ground truth decoded from the F1AP pcap (RRC Setup; RRC Reconfiguration only adds bearers)
- CORESET#1: freqDomainResources `ff0000000000` (RB 0-47), duration 2, nonInterleaved, precoder sameAsREG-bundle,
  no pdcch-DMRS-ScramblingID (⇒ n_ID = PCI 1).
- SearchSpace#2 on CORESET#1: every slot, symbol 0, candidates AL1 0 / **AL2 6 / AL4 4** / AL8 0 / AL16 0,
  formats0-1-And-1-1. (CSS: SS#1 on CORESET#0 AL4 ×2.)
- PDSCH-Config: DM-RS mappingTypeA setup with no fields ⇒ **type 1, additional position pos2 (default), maxLength 1**;
  resourceAllocationType1; **no mcs-Table ⇒ qam64 table** (the yaml's `pdsch.mcs_table: qam256` does not reach
  RRC Setup); no dedicated TDA list ⇒ common list from SIB1 `{[0] k0=0 S2 L12 typeA, [1] k0=0 S2 L6 typeA}`;
  static PRB bundling wideband; no VRB interleaver; ZP-CSI-RS periodic only; dynamic HARQ-ACK codebook,
  dl-DataToUL-ACK 8 entries {4..9,11,12}.
- PUSCH: resourceAllocationType1, transformPrecoder disabled, codebook maxRank 1, 1 SRS port, 5-entry common TDA
  (k2 4,5,6,7,11), betaOffsets semiStatic, no CSI reportTriggerSize.
- **DCI sizes derived from that config (TS 38.212 7.3.1.1.2 / 7.3.1.2.2): DCI 1_1 = 42 bits, DCI 0_1 = 38 bits**
  (1_1: 1 id + 11 FDRA + 1 TDRA + 5 MCS + 1 NDI + 2 RV + 4 HARQ + 2 DAI + 2 TPC + 3 PRI + 3 K1 + 4 ports + 2 SRS
  + 1 DMRS-init; 0_1: 1 + 11 + 3 TDRA + 5+1+2+4 + 2 DAI + 2 TPC + 3 ports + 2 SRS + 1 DMRS-init + 1 UL-SCH).
  **Passive blind lengths 42 / 38: both correct.**
- Observed PDSCH for 0x4601 (gNB PHY log): S2 L12 16QAM (BG1 55710 / BG2 8467), S2 L6 16QAM ×5635.

## Results
### M1 — OCUDU + srsUE through the broker (PX=0, 60 s, `/tmp/ocudu_passive/m1_noPX`): PASS
- gNB N2 up in 2 s, srsUE attached + PDU session in 2 s (UE IP 10.45.0.20), C-RNTI **0x4601**
  (gNB `tc-rnti=0x4601`).
- Broker 0.90x real time sustained; UL ping 300/300, rtt avg 31 ms; DL UDP 2 Mbit/s.
- Ground-truth DCI census (60 s): C-RNTI 1_1 on ss_id=2 AL4 ×32658, 0_1 ss_id=2 AL2 ×4983 / AL4 ×117,
  fallback 1_0/0_0 on ss_id=1 ×21, SI-RNTI 1_0 ss_id=0 ×340, RA-RNTI 0x10b ×1.
- Finding: srsUE's ZMQ TX comes out at ~+47 dBFS peak block RMS (tx_gain 50 applied as linear scale), DL at
  −12 dBFS. Fed raw into OAI's float→int16 path the UL would rail, so the broker now auto-normalises UL to the
  DL level in the passive lane only, and applies −6 dB to the whole lane (`--px-gain-db`).
### M2 — + passive receiver (`/tmp/ocudu_passive/m2_px`)
- Passive (started after attach) synced SSB/MIB/SIB1 in **15 s**; broker 0.61-0.74x real time with the passive
  in the loop, passive lag ≤ 0.007 s, 0 lane drops. 240 s dwell (script died at the end on a mid-run edit of
  itself — bash reads scripts incrementally; processes were stopped cleanly with `run_ocudu_passive.sh stop`).
- Blind dedicated discovery vs OCUDU ground truth (all ✓ = matches):
  | item | passive (blind) | OCUDU truth |
  |---|---|---|
  | dedicated CORESET | `CORESET VERIFIED offset=0 span=48` (dur 2) | CORESET#1 = RB 0..47 (8×6-RB resources, 51 PRB), dur = CORESET#0 dur 2 ✓ |
  | C-RNTI | 0x4601 (length lock + CORESET verify + UL lock all on it) | tc-rnti=0x4601 ✓ |
  | n_RNTI (PDCCH DM-RS) | STAGE0 DECIDED 0 | pdcch-DMRS-ScramblingID absent ✓ |
  | DCI 1_1 length | 42 (`evidence=distinct_ota`) | not yet cross-checked (F1AP pcap added for next run) |
  | DCI 0_1 length | 38 | not yet cross-checked |
  | DCI 1_1 layouts | 455 armed → 356 plausible after 36k DCIs, "configured IS among survivors" | — |
  | TDA of C-RNTI PDSCH | PARMSET dominant sym=1+10, Qm=2, BG2 — **wrong** | symb=[2,14) (S2 L12), 16QAM, BG1 |
- Counters at end: occasions=443738, `dci10[accepts=720 C=613 TC=1 SI=106 RA=0 P=0]` (the C=613 on 1_0 are
  mostly false accepts of random RNTIs, 0xa839/0xe9c5/…, held by the persistence gate),
  `dci01[accepts=3042 rejects=1149930]`, **`pdsch_decode[try=34079 crc_ok=73 (0.2%)]`**,
  `scanq[drop_full=13642 of 225040 (6%)]`, **Technique D ARMED ×193523, CONVERGED ×0**.
- **UL PUSCH: `pusch_passive[try=3041 crc_ok=1516 (49.9%) seg_fail=1459]`** — works (partially). Context: OCUDU's own
  PUSCH is 16575 OK / 1110 KO with a pathological median SINR of 2.6 dB (p10 1.1, p90 5.7) on an ideal channel,
  so UL itself is poor on this bed (suspect srsUE tx_gain=50 ⇒ ×316 amplitude; A/B with `--rf.tx_gain=0` next).

#### Receiver bug found: Technique D is reset every occasion on SA (root cause of CONVERGED=0 here)
`nr_pdcch_blind_monitor_rt.c:4165` calls `pdsch_sweep_maybe_enable(cfg)` for every non-CSS0 occasion. The
CORESET#0-USS pass (`run_occasion`, rt.c:3002-3043, `PASS_C0USS`) runs occasions with the snapshot built by
`nr_pdcch_blind_monitor_coreset0_uss_cfg()` (monitor.c:136-168), which has `dci10_ss_type = UE_SPECIFIC`
(so `css0_occasion` is false) and **`autodiscover = 0`** (so `ready` = true) and CORESET#0 geometry (so the
configuration `identity` differs from the dedicated cfg). Interleaved with the dedicated-cfg occasions this
flips `g_pdsch_sweep_on` / `identity` on every alternation, and each flip calls
`nr_pdsch_config_sweep_reset_all()` (rt.c:1170) — the log shows `Technique D ARMED` from line 179 (before any
DCI length lock, i.e. before the dedicated cfg could ever be ready) and 193523 times in 240 s. Every
alternation before the pass is GATED, then every 64th occasion after — TD contexts cannot accumulate the
evidence needed to converge. **phy-test cannot see this** (no SIB1 ⇒ `coreset0_uss_cfg()` returns false),
which explains why TD converges there and not in SA.
**Already fixed on sens6 `sdd/gap-perf` @ ccf3b7a2c6** (`pdsch_sweep_maybe_enable` no longer resets on an
identity change / not-ready pass; contexts keyed by identity; ARMED capped at 20 prints) — this bed is an
independent SA reproduction of why that fix is needed; rfsim-val (c295fa18f1) does not have it.
Original proposed fix (not applied, per brief): arm/score Technique D only from the dedicated configuration — e.g.
`if (!css0_occasion && t_pass_kind != PASS_C0USS) pdsch_sweep_maybe_enable(cfg);` (and, if several banks
exist, key `identity` per bank or drop CORESET geometry from it, since a PDSCH config does not depend on which
CORESET carried the DCI). Validation: ARMED should print once per real (re)configuration, then CONVERGED.

### M3 — PX_FIRST=1, srsUE `--rf.tx_gain=0`, 300 s (`/tmp/ocudu_passive/m3_pxfirst_txg0`)
- srsUE tx_gain A/B (gNB-measured PUSCH SINR, ideal channel): **tx_gain 50 → median 2.6 dB (p10 1.1, p90 5.7),
  16575 OK / 1110 KO; tx_gain 0 → 42.8 dB (p10 42.7, p90 42.9)**. rf_zmq multiplies TX by 10^(gain/20); ×316
  drives OCUDU's UL front end into clipping. `tx_gain = 0` is now the default in `ue.srsue.ocudu.conf`.
- Passive UL is unaffected by it (broker normalises UL in the passive lane): `pusch_passive[try=4572
  crc_ok=2025 (44.3%) seg_fail=1989 zero_tb=558]` vs 49.9 % in M2 ⇒ the ~50 % UL ceiling is a receiver-side
  issue, not SNR (seg_fail dominates; not diagnosed yet).
- Same DL picture as M2: CORESET/C-RNTI/1_1=42/0_1=38 all correct, `pdsch_decode[try=49540 crc_ok=78 (0.2%)]`,
  `scanq drop_full=19885/278050 (7 %)`, Technique D CONVERGED ×0. dci10 RA=0/TC=1: with PX_FIRST the passive's
  initial sync still finished after the (2 s) attach, so RAR/Msg4 were not observed.

### Integration receiver (sdd/integration @ c087e5d987) — IN FLIGHT at handback
- Worktree `/home/sens/NICOLA/rfsim-integ` created from sens6 `sdd/integration`; configured
  `-GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_ISAC_SENSING=ON -DOAI_ZMQ=ON`; built nr-uesoftmodem,
  rfsimulator, oai_zmqdevif, params_libconfig (clean). Harness copied into its `tests/passive_rx/` (uncommitted).
- Batch running unattended: `scratchpad/batch1.sh` (log `scratchpad/batch1.log`) = i1, i2, i3 (integration),
  b1 (rfsim-val baseline), 240 s each, sequential, ~20 min from 10:26. Per-run score lands in
  `/tmp/ocudu_passive/<run>.score.txt` (`ocudu/score_ocudu_run.py`, computes ARMED/CONVERGED, time-to-converge
  from first C-RNTI accept via the new wall_clock log prefix, converged TDA vs truth, crc_ok/try, PUSCH, lengths).
- **i1 (integration, 240 s): Technique D CONVERGED at t+3.0 s after the first C-RNTI accept on tda=0 S2 L12
  DM-RS mask 0x884 (symbols 2/7/11 = type A pos2 + add-pos 2) table 0 (qam64) — all match the RRC truth.
  C-RNTI PDSCH `crc_ok=28384/37261 (76.2 %)`** (rfsim-val: 0.2 %). ARMED ×20 (print cap). drop_full 5462/220334
  (2.5 %). DCI 1_1=42 / 0_1=38 again correct. PUSCH 165/2855 (5.8 %): UL DCI-width exploration had not converged
  by the end (class0 86/146, needs 300 per class) — see PUSCH section.
- OCUDU test-knobs build loop (`scratchpad/isac_build.sh`) was STOPPED by me (pids 1923530/1923532) at 115/1367;
  it must be run to completion (`ninja -C /home/sens/NICOLA/repos/ocudu-test/build -j10 gnb isac_test_knobs_test`)
  once batch1 has ended — NOT yet done.

### Findings added after i1 (2026-09-27)
- **qam256 does not reach RRC Setup: UE capability, not config.** `du_pdsch_resource_manager.cpp:110-118`
  picks qam256 only if `ue_caps->pdsch_qam256_supported`, which comes from `phyParametersFR1.pdsch-256QAM-FR1`
  (`ue_capability_manager.cpp:234`). srsUE's SA capability builder (`srsue/src/stack/rrc_nr/rrc_nr.cc`
  `send_ue_capability_info`, ~l.752-847) never sets `phy_params_fr1` (only the NSA `get_nr_capabilities`
  does, l.1025), and the F1AP UE Capability Information confirms no 256QAM. For the qam256 arm: patch srsUE to
  set `ue_cap.phy_params.phy_params_fr1_present = true; ...pdsch_minus256_qam_fr1_present = true;` in
  `send_ue_capability_info` (srsUE already handles `mcs_table_256qam`, l.1260), or force it on the gNB
  (isac-test-knobs). Not done yet.
- **Passive PUSCH is not a flat ~50 % ceiling; it is two things** (incremental crc_ok per stats window):
  - M2 (UL mostly QPSK, tx_gain 50): 4 % for the first ~1200 grants, then 91-97 % once the UL DCI 0_1
    field-width search converged ("UL width and baseline CRC-validated: class=0 … lower=0.930").
  - M3 (UL 64QAM, tx_gain 0, SINR 42.8 dB): 4 % for ~1800 grants, then 74-81 % steady.
  - i1 (64QAM): still in exploration at 2855 grants (class0 86/146, needs 300 per class) → 5.8 %.
  So (1) the 78-class UL width exploration costs ~1500-3000 grants and is slower at 64QAM because per-class
  CRC evidence is sparser; (2) after convergence 64QAM tops out at ~75-80 % vs ~97 % for QPSK. (2) needs a
  per-grant comparison: next run uses `ISAC_PUSCH_DIAG=1` (per-grant PUSCHDIAG: mcs/tbl/rv/tbs/G/Qm/prb/sym/ta/
  seg) against the gNB PUSCH lines for the same slot.
- **Passive PDSCH 76 % breakdown (i1):** MCSHIST mcs=11 87 % (32394), **mcs=10 0 % (4683)**; NSYM short (≤9 sym,
  the S2 L6 special-slot grants) 1250 of 1387 end as zero-TB, long grants 28373 decoded / 7426 seg_fail.
  Leads: every MCS-10 grant fails, and special-slot grants don't decode. Needs a gNB phy-debug truth run
  (PDSCH mcs/R per slot, DCI size=) to separate "wrong MCS/TBS" from "wrong TDA".

### srsUE 256QAM capability patch (local, NOT committed; source edited, NOT yet rebuilt)
`repos/srs-ue/srsue/src/stack/rrc_nr/rrc_nr.cc`, `send_ue_capability_info`: env `SRSUE_ADVERTISE_256QAM=1` sets
`phyParametersFR1.pdsch-256QAM-FR1` and per-band `pusch-256QAM`. Default off, so existing arms are unchanged.
Rebuild (idle window only): `make -C /home/sens/NICOLA/repos/srs-ue/build -j10 srsue`. Launch needs the env to
cross sudo: `sudo -n env SRSUE_ADVERTISE_256QAM=1 srsue …` (run_ocudu_passive.sh knob still to be added after
batch1 — the script must not be edited while the batch uses it).

### Batch1 partial + prepared-but-unapplied changes (state at handback)
- **i2 (integration):** CONVERGED ×1 at t+5.0 s, tda=0 S2 L12 mask 0x884 table 0 (matches truth), ARMED ×20,
  C-RNTI PDSCH **27821/36757 (75.7 %)**, drop_full 6077/216238, 1_1=42 / 0_1=38, PUSCH 271/2701 (10.0 %, UL width
  search still converging). i3 running, b1 queued.
- **Prepared, NOT yet applied (must not touch the scripts while the batch runs):**
  - `scratchpad/broker_new.py`: synthesizes gNB UL until srsUE's first DL request, then pads UL to the DL index
    and hands over (lets the cell and the passive run before any UE attaches → PX_FIRST can catch
    PRACH/RAR/Msg4); plus `--no-ul-norm`. Needs `scratchpad/broker_selftest.py` (late-UE case) to pass, then copy
    over both trees' `ocudu/ocudu_zmq_broker.py`.
  - `scratchpad/patch_script.py <run_ocudu_passive.sh>`: adds GNB_BIN override, GNB_ENV / SRSUE_ENV / PX_ENV,
    PX_FIRST waits for passive SIB1 before starting srsUE, attach timeout 600 s.
- Knob-arm env for the ocudu-test gNB (`GNB_BIN=/home/sens/NICOLA/repos/ocudu-test/build/apps/gnb/gnb`):
  RA0 DL `ISAC_OCUDU_TEST_DL_RA_TYPE0=1`; RA0 UL `ISAC_OCUDU_TEST_UL_RA_TYPE0=1`; VRB IL `ISAC_OCUDU_TEST_VRB_IL=2`;
  DM-RS type 2 `ISAC_OCUDU_TEST_DMRS_TYPE2=1`; DL scrambling `ISAC_OCUDU_TEST_DL_DMRS_ID0=700
  ISAC_OCUDU_TEST_DL_DATA_ID=500`; type-B TDA `ISAC_OCUDU_TEST_DL_TDA="0:typeA:2:12;0:typeB:2:4"` (type B is the
  only entry that fits the 8-DL-symbol special slot, so it is guaranteed to be used); AL1-only (stock gNB):
  `GNB_EXTRA='cell_cfg pdcch dedicated --ss2_n_candidates 4 0 0 0 0'`.
- Step-3 arms are pre-written as `scratchpad/batch2.sh [1]` (pdiag, pxfirst, q256, al1; with arg 1 also the six
  ocudu-test knob arms k_ra0dl, k_ra0ul, k_vrbil2, k_dmrst2, k_dlscr, k_tdab), each scored into
  `/tmp/ocudu_passive/<name>.score.txt`. Launch only after the builds, broker self-test and script patch.
- **i3 (integration):** see chain results table below (auto-appended). Unattended chain
  `scratchpad/chain.sh` (log `scratchpad/chain.log`) launched 10:41: waits for batch1 → ocudu-test build+test →
  srsUE rebuild → broker self-test/install + script patch → batch2 (knob arms iff BUILD_OK and test pass) →
  appends a results table here.

### Chain results (auto-generated by scratchpad/chain.sh)
| run | ARMED | CONVERGED | t_conv s | converged cfg | truth PDSCH (top) | C-RNTI PDSCH try/crc_ok | PUSCH try/crc_ok | DCI 1_1/0_1 len | dci10 TC/RA |
|---|---|---|---|---|---|---|---|---|---|
| i1 | 20 | 1 | 3.0 | tda=0 S=2 L=12 mask=0x884 table=0 | S2 L12 16QAM BG1 | 37261 crc_ok=28384 (76.2%) | 2855 crc_ok=165 (5.8%) | 42/38 | 1/0 |
| i2 | 20 | 1 | 5.0 | tda=0 S=2 L=12 mask=0x884 table=0 | S2 L12 16QAM BG1 | 36757 crc_ok=27821 (75.7%) | 2701 crc_ok=271 (10.0%) | 42/38 | 3/0 |
| i3 | 20 | 1 | 2.9 | tda=0 S=2 L=12 mask=0x884 table=0 | S2 L12 16QAM BG1 | 36453 crc_ok=27441 (75.3%) | 2595 crc_ok=282 (10.9%) | 42/38 | 1/0 |
| b1 | 166176 | 0 | - | - | S2 L12 QPSK BG1 | 30289 crc_ok=58 (0.2%) | 2844 crc_ok=570 (20.0%) | 42/38 | 4/0 |
| pdiag | 20 | 1 | 4.0 | tda=0 S=2 L=12 mask=0x884 table=0 | S2 L12 16QAM BG1 | 36375 crc_ok=27326 (75.1%) | 2630 crc_ok=76 (2.9%) | 42/38 | 2/0 |
| pxfirst | 1 | 0 | - | - | - | 1526 crc_ok=1526 (100.0%) | 0 crc_ok=0 (0.0%) | -/- | 0/7 |
| q256 | 20 | 0 | - | - | S2 L12 QPSK BG2 | 32081 crc_ok=4306 (13.4%) | 1097 crc_ok=23 (2.1%) | 42/38 | 0/0 |
| al1 | 20 | 5 | 4.7 | tda=0 S=2 L=12 mask=0x884 table=0 | S2 L12 16QAM BG1 | 40064 crc_ok=12668 (31.6%) | 7916 crc_ok=4262 (53.8%) | 42/38 | 4/0 |

### Current strict runner reconciliation (2026-09-27 18:25+02:00)

The preceding batches are historical and do not define the current launch procedure. The canonical,
independently approved strict stack is:

- `/home/sens/NICOLA/ocudu_dl_g4_strict_chain.sh`, SHA256
  `a245fb0134e1c31078ec912b08c12f638639888926e6249fa2d31d5717168424`;
- `/home/sens/NICOLA/ocudu_dl_g4_arm.sh`, SHA256
  `219dbe745b96514f0e7c21b4f67c92d6bac709bae69d5379b2d7828e01a204ea`;
- invoked harness `/home/sens/NICOLA/rfsim-integ/tests/passive_rx/run_ocudu_passive.sh`, SHA256
  `c4e0df1f27d70db110f34922031447e4ea0e6e17488657ad09b5e610c1681395`;
- ownership helper `/home/sens/NICOLA/rfsim-integ/tests/passive_rx/ocudu_owned_process.py`, SHA256
  `2a7983e9ae413b6568a1af421d5f8ca039b991091fe4263b69c14ad5a16cfc7e`;
- strict scorer `/home/sens/NICOLA/ocudu_dl_g4_score.py`, SHA256
  `84a69042562dcd1c4c672bdfd4e472db0d25698c2d599b2f8ef3fbad1267c3bf`.

Do not invoke the older `rfsim-val` harness (SHA256 `30763dbb...`) as the current procedure; it lacks
the ownership helper. Run strict arms only through the root wrapper/chain after the handover's two-host
quiescence checks. Strict arms require exactly 300 seconds. The current stack was re-reviewed Approved at
17:02 after test-first duration validation and exact PID/start-time cleanup fixes.

The strict OCUDU-DL chain exercised this stack in six alternated 300-second arms
`bb1,rr1,bb2,rr2,bb3,rr3`; all provenance files pin the hashes above and cleanup completed exactly.
This is valid runner smoke but not a passing harness-lane G4. It is the OCUDU-DL lane's failed G4:
target C-RNTI baseline `16/105751=0.01512988%`, candidate `12/137207=0.00874591%` (candidate lost all
three paired rates), with candidate unsupported ZP confirmations `8/8/8`. Earlier M1-M3/i1-i3/b1 runs
used older procedure generations; `harness_smoke_b0` is VOID because a sens6 build overlapped.
