# R13 live SA rfsim bed on sens6

**Status:** tooling prepared offline. The operator runs this on **sens6 x86_64** with an Open5GS core and one attached OAI UE. The passive receiver is a separate `nr-uesoftmodem --passive-rx` client of the gNB rfsimulator. Every result from this bed is `[SIM VERIFIED, sens6 SA rfsim, @<commit>]`, never OTA. Record the exact commit and whether the CPU or GPU build ran. Do not combine these figures with DGX, OCUDU, or OTA evidence.

## 1. Prerequisites and protected files

Use an idle sens6. No build or other bed may overlap this run. The runner takes the **exclusive** `/tmp/td_measure.lock` and rejects a held lock, a local orchestration `BUSY` file, occupied rfsim/telnet ports, or another softmodem. On sens6, check `pgrep -a -x nr-softmodem; pgrep -a -x nr-uesoftmodem; docker ps --format '{{.Names}} {{.Status}}'` (if Docker is installed) before starting; the development sandbox's `pgrep` does not see host processes. Keep `ISAC_RX_BRANCH_FO` unset. Stop receivers with SIGINT. Do not edit `tests/passive_rx/captures`, `tests/passive_rx/*.conf`, or `tests/passive_rx/sens6_host_snapshot_2026-09-30`; new configs here have `.cfg` suffix. Run before every commit:

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures 'tests/passive_rx/*.conf' tests/passive_rx/sens6_host_snapshot_2026-09-30
```

Host tools: CMake, Ninja, Python 3, `ss`, `flock`, `sudo -n`, SCTP support, MongoDB/Open5GS, `nvidia-smi` and CUDA only for the GPU arm. `sudo -n true` must pass for the active UE's TUN. `ogstun` must exist with the UPF route. Check `df -h /tmp` and the campaign destination: MAC debug logs over the 60-minute stable arms can be large. Keep the core running across arms; the runner manages gNB, UE, traffic and passive receiver. Prefer a quiet host and check `flock -n /tmp/td_measure.lock true` before building. The script itself acquires the bed lock.

## 2. Open5GS installation and provisioning (privileged operator steps)

Follow the [official Open5GS Quickstart](https://open5gs.org/open5gs/docs/guide/01-quickstart/) for the installed Ubuntu release, MongoDB, Open5GS packages, UPF routing/TUN and subscriber WebUI. The installed release's sample files are the authority for unrelated SBI/NF fields. **Privileged:** install packages, configure `/etc/open5gs/{amf,smf,upf,nrf}.yaml`, set up `ogstun`/IP forwarding, add a subscriber, then restart the affected services. If using Docker instead of packages, publish SCTP 38412 and UDP 2152 and ensure the gNB's `127.0.0.1` addresses really reach those endpoints; do not run an unrelated OAI container bed concurrently.

Merge [open5gs-amf.cfg](open5gs-amf.cfg) and [open5gs-smf.cfg](open5gs-smf.cfg) fragments into the installed YAML, preserving its other sections. Set the AMF NGAP listener to `127.0.0.1:38412`; gNB NGAP/NG-U addresses are `127.0.0.100`. The cell and subscriber use **MCC 001, two-digit MNC 06, TAC 1, SST 1, SD 000000, DNN internet**. The [subscriber template](open5gs-subscriber.cfg) gives IMSI `001060123456743`, K, OPc, AMF and the matching slice/DNN; provision through the installed WebUI or its documented subscriber API, not by assuming that JSON is a MongoDB import format. Credentials match [ue_active.cfg](ue_active.cfg). These are **test credentials**; use only this isolated core. If NRF has a serving-PLMN filter, include 001/06. Check `systemctl status open5gs-amfd open5gs-smfd open5gs-upfd`, `ss -ln -A sctp | grep 38412`, `ip addr show ogstun`, and the AMF/SMF logs.

The [Open5GS AMF sample](https://github.com/open5gs/open5gs/blob/main/configs/open5gs/amf.yaml.in) and [SMF sample](https://github.com/open5gs/open5gs/blob/main/configs/open5gs/smf.yaml.in) show the current key locations. Their syntax can change with the installed version; merge rather than replace complete service files.

## 3. Build on sens6 (no active bed)

The x86 build follows `CLAUDE.md`: **no `oai_usrpdevif`**, `-DOAI_USRP=OFF`, `-DOAI_SIMU=ON`, `-DENABLE_ISAC_SENSING=ON`. Run each configure/build/test command under a **shared** lock, after checking the idle host. `-j8` is the maximum. From the repository root:

```bash
R=$PWD
B=$R/cmake_targets/ran_build/build
mkdir -p "$B"
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S "$R" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DOAI_SIMU=ON -DOAI_USRP=OFF -DENABLE_LDPC_CUDA=OFF
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C "$B" -j8 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir "$B" -j4 --output-on-failure
```

Check the **actual** sens6 GPU first:

```bash
nvidia-smi --query-gpu=name,memory.total,compute_cap,driver_version --format=csv,noheader
nvcc --version
```

The old sens6 snapshot says RTX 4060 Ti 8 GB (compute capability 8.9); the operator expected an RTX 4070. Both imply `LDPC_CUDA_ARCH=89`, but use the observed compute capability. The DGX GB10 `121` setting is wrong for sens6 and nvcc 12.4 cannot target it. Make a separate GPU build directory so a CPU control remains available:

```bash
BG=$R/cmake_targets/ran_build/build_gpu
mkdir -p "$BG"
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S "$R" -B "$BG" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DOAI_SIMU=ON -DOAI_USRP=OFF -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=89
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C "$BG" -j8 nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests
```

For a GPU arm set `BUILD=$BG R13_GPU=1`. The runner puts `$BUILD` on `LD_LIBRARY_PATH` and loads `_cuda`; check `TD_CB0 GPU backend registered` in the receiver log and `td_cb0_backend.gpu` in `metrics.jsonl`. If it reports CPU fallback, label the run as CPU and investigate the plugin path/driver. A GPU build is not a GPU-validated result by itself.

## 4. Config variants and bring-up

`gnb_baseline.cfg` is the existing 106-PRB SA BWP config copied into a new `.cfg`, with MAC debug logging for ground truth. It has two added DL BWPs (40 PRB at CRB 30, 24 PRB at CRB 70), first active BWP 1. `gnb_dedicated.cfg` changes only the first active **dedicated BWP size from 40 to 24 PRB**. OAI `nr_mac_common.c:nr_dci_size()` derives the DCI 1_1 frequency-domain assignment width from `N_RB = DL_BWP->BWPSize`: type-1 RIV width is `ceil(log2(N_RB*(N_RB+1)/2))`, so 40 PRB needs 10 bits and 24 PRB needs 9. This is a dedicated config change that should reduce DCI 1_1 by one bit; **verify with gNB `DCI11_WIDTHS total=... fdra=...` and `Filling Format 1_1 DCI of size N` before/after logs**. Do not infer the width from the number of added BWPs: OAI's `configure_UE_BWP()` currently sets `n_dl_bwp=1` for any nonempty list. OAI `nr_radio_config.c:config_pdsch()` hardcodes DM-RS type and max length, so an antenna-ports-table size change is not exposed by a minimal `.cfg` knob in this tree. `gnb_cell.cfg` changes PCI 0→1 for a HARD_RESET identity change. The passive receiver config is [ue_passive.cfg](ue_passive.cfg), with unpinned scan thread. The active UE's RF arguments match 3319.68 MHz, 106 PRB, 30 kHz, band 78 and SSB subcarrier 516.

The runner order is: preflight/core check → baseline gNB and NG setup → rfsim `:4043` ready → active UE random access and PDU session → continuous downlink UDP traffic → passive receiver acquisition **and baseline DCI 1_1 lock plus Technique D convergence** → wait `R13_APPLY_AT_S` → apply one scenario → continue to `R13_DURATION_S` → SIGINT teardown. A dedicated change or cell restart stops traffic, UE and gNB, then launches the changed gNB and reattaches the UE **while the same passive receiver remains running**. If the receiver exits on rfsim disconnect, the run is incomplete; do not restart it and call that recovery.

## 5. Campaign commands

Use a fresh campaign root. `campaign.py` records host, commit, GPU, command, environment, output and a generic receiver verdict. `score_r13.py` is the R13 authority. `run_arm.sh` writes into the campaign run directory (its current directory). `--secs` covers attach time plus scheduled run; the campaign grace allows orderly SIGINT. Do not run a build during this loop.

```bash
export R=/path/to/checked-out/repository
export BUILD=$R/cmake_targets/ran_build/build
export R13_APPLY_AT_S=45 R13_TRAFFIC_RATE=6M R13_RX_ANT=4
export CAMPAIGN_GRACE_S=120 CAMPAIGN_TERM_GRACE_S=120
C=$(python3 "$R/tests/passive_rx/campaign/campaign.py" new --root /tmp/r13-sens6 --name r13-cpu --site sens6 --cell 'OAI SA rfsim PCI0/1 106PRB PLMN00106')
for scenario in stable bwp_switch dedicated_change cell_restart; do
  if [ "$scenario" = stable ]; then export R13_DURATION_S=3645; secs=4500
  else export R13_DURATION_S=180; secs=900; fi
  for sib in sa sib1less; do
    for flag in on off; do
      for repeat in 1 2 3 4 5; do
        python3 "$R/tests/passive_rx/campaign/campaign.py" run "$C" --arm "${scenario}_${sib}_${flag}" --secs "$secs" -- \
          bash "$R/tests/passive_rx/sa_bed/run_arm.sh" "$scenario" "$sib" "$flag"
      done
    done
  done
done
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" "$C"
```

This is 16 arms × 5 runs; the stable arms have at least 60 minutes after the receiver is ready. Short stable runs can only report an observed rate and score INCOMPLETE for the full-hour gate. Run GPU arms as a separate campaign with `BUILD=$BG R13_GPU=1`, after confirming the model/arch and CPU baseline. To run one scenario manually, create an empty directory, `cd` into it, then run `bash "$R/tests/passive_rx/sa_bed/run_arm.sh" bwp_switch sa on`; score the directory afterward.

The single-arm syntax is `bash "$R/tests/passive_rx/sa_bed/run_arm.sh" <stable|bwp_switch|dedicated_change|cell_restart> <sa|sib1less> <on|off>`. For example, from four different empty run directories use `stable sa on`, `bwp_switch sa on`, `dedicated_change sa on`, and `cell_restart sa on`; repeat each with `sib1less` and `off` for the other arms. Set `R13_DURATION_S=3645` for each stable run and `180` for the changed arms.

`sa` leaves SIB1 evidence available and waits for a pre-apply SIB1 decode. `sib1less` sets `ISAC_TD_IGNORE_SIB1=1`, which suppresses SIB1-derived receiver evidence while the **actual serving cell remains SA and broadcasts SIB1**. The scorer checks UeContext snapshots for present SIB1 evidence in the enabled SA arm and null SIB1 evidence in the enabled SIB1-less arm. This models a SIB1-absent observation; it is not a live NSA cell. `on` sets `ISAC_RECONF=1`; `off` is the flag-off control. Do not infer NSA OTA performance from this bed.

Both flag states set receiver-only `ISAC_BWP_TRACK=1`, matching `run_bwp_switch.sh`; OAI otherwise leaves BWP tracking off unless `ISAC_AGNOSTIC_V2=1`. This keeps the switch observable in the control while isolating `ISAC_RECONF`.

## 6. Logs, scoring and validation rules

Each run has `events.jsonl` (wall-clock apply/end), `gnb/{before,after}.log`, `ue/{before,after}.log`, `rx/rx.log`, `metrics.jsonl`, `uectx.jsonl`, traffic logs and campaign metadata. The passive config disables sensing output for this receiver gate; its relative output paths remain inside `rx/` if sensing is enabled later. Logs are Unix-time-prefixed by `stamp.py` to put apply, gNB truth and receiver evidence on one sens6 clock. Expect `Received NGSetupResponse`, `RA procedure succeeded`, `PDU Session Establishment Accept`, `UE IPv4`, `SENSING: DCI 1_1 length locked`, `Technique D CONVERGED`; on switch expect telnet `triggered BWP switch` and `BWP RESOLVED`; on enabled reconfiguration expect `CONFIG_EPOCH old -> new class=SOFT|HARD_REVERIFY|HARD_RESET cause=...`, followed by fresh lock/verification/convergence. A gNB restart can create an in-window `HARD_REVERIFY` with cause `CONTINUITY_LOSS`; the PCI-changing arm also needs `HARD_RESET` with cause `CELL_IDENTITY_CHANGE`. `CONFIG_EPOCH` absence in the flag-off control is expected.

The scorer requires fresh post-apply milestones: BWP RESOLVED plus convergence; new DCI length lock/relock plus convergence; or post-restart ACQ_STATE lock/verification plus convergence. It reports elapsed time **from scenario apply**, epoch class/cause/count, false SOFT per hour outside the scenario window (from apply through action completion plus 30 s soft or 60 s hard), false HARD count, four `dropped_epoch` deltas, C-RNTI grant rate, and CPU/GPU CB0 counters. It compares post-change receiver DCI 1_1 length winners with timestamped gNB MAC-debug DCI sizes. The gNB log is read only by the scorer; it is **never passed to the receiver**. For each run, inspect `score_r13.json` and `score_r13.txt` and then the campaign `r13_summary.json`. The one-UE dedicated change tests **per-UE DCI relock**: `nr_cfg_epoch_note_rnti_reopened()` needs two distinct converged RNTIs inside 2 seconds before `DEDICATED_CHANGE_SUSPECTED` can bump the cell epoch. A missing SOFT epoch in this particular arm is therefore expected; do not claim a cell-wide dedicated-change trigger from it.

For TDRA/DM-RS winners and any trusted DCI/layout use that does not create a fresh lock line, the gNB debug log needs a manual ground-truth review. Record a run-local `td_truth_audit.json` only after comparing each post-apply receiver `Technique D CONVERGED` winner with the contemporaneous gNB scheduler/RRC config, reviewing trusted DCI/layout evidence, and checking no old-epoch winner was used:

```json
{"source":"gnb-log","checked":true,"stale_winners":0,"note":"operator, date, gNB log lines/config and receiver lines inspected"}
```

The scorer leaves `stale_winners` null and status INCOMPLETE without this audit. A number entered in the file is an operator assertion, not an automated TDRA/DM-RS parser. If gNB debug does not provide enough fields to establish the comparison, keep the run INCOMPLETE and preserve logs. `uectx/uectx_report.py` can produce per-UE timelines from `uectx.jsonl`; cross-check `ue_reconfig` timestamps with the event boundary.

**R13 PASS:** ≥5 complete runs for each enabled scenario × SIB1-available/SIB1-less arm, plus ≥5 flag-off control runs per matching arm; measured C-RNTI traffic ≥100 grants/s; soft recovery ≤10 s, hard recovery ≤30 s from apply; expected BWP SOFT/cell HARD_RESET epoch and fresh lock/VERIFIED/converged state; 0 stale DCI and audited TD winners; ≤1 false SOFT/h and 0 false HARD on a 60-minute stable cell; dropped-epoch counters reported and old work never credited; CPU/GPU path explicitly labelled. The one-UE dedicated arm cannot satisfy the separate two-UE cell-wide trigger coverage. Flag-off runs are controls, not R13 PASS runs. The scorer's `PASS` is per enabled run; campaign `r13_summary.json.r13_pass` requires five passes in all eight enabled arms and at least five runs in each control arm. Inspect each control result for completeness. A SIB1-less SA test is not an NSA OTA validation. Preserve evidence with `[SIM VERIFIED, sens6 SA rfsim, @<commit>]` and the config/compute path. Any run with no post-change gNB DCI size, failed traffic, a dead receiver, missing metrics, a false hard bump or unaudited TD truth is INCOMPLETE.

## 7. Troubleshooting and teardown

- **AMF SCTP absent / NG setup timeout:** check `open5gs-amfd`, AMF NGAP address `127.0.0.1`, PLMN/TAC match, SCTP module, and AMF logs. Check UPF UDP 2152, SMF DNN/slice and `ogstun` if a PDU session fails.
- **UE RA succeeds but no PDU session:** verify the subscriber's exact IMSI/K/OPc/AMF, SST/SD/DNN, MongoDB, SMF and UPF logs. Avoid changing the active UE `.cfg` without changing the subscriber.
- **No DCI-size difference:** compare `Filling Format 1_1 DCI of size` in `gnb/before.log` and `gnb/after.log`. If absent, confirm MAC debug logging; if equal, inspect `n_dl_bwp` and another field's size alignment before claiming dedicated-change coverage.
- **No UDP traffic or <100 C-RNTI grants/s:** inspect `traffic/*.log`, UE IPv4/TUN, `ogstun`, host routing, CPU load and gNB scheduler debug. Do not score sparse ping traffic as a recovery gate.
- **Receiver exits on gNB restart:** keep the failed run as evidence. The within-stream hard-change test cannot be claimed from a new receiver process. Investigate rfsim reconnect behavior in a separate implementation task.
- **GPU CPU fallback:** compare `nvidia-smi` compute capability with `LDPC_CUDA_ARCH`, check `libldpc_cuda.so`/`libtd_cb0_gpu.so` in `$BUILD`, `LD_LIBRARY_PATH` and receiver registration log. Label fallback as CPU.
- **Hung process:** the runner sends SIGINT and reports any group still alive after 15 s. Inspect it manually; preserve logs. Do not start another arm while a softmodem remains. Core services stay running; stop them only if this test's isolated core was started for the campaign, using the operator's normal service procedure.

For an interrupted run, send SIGINT to `run_arm.sh` or the campaign runner and wait for its cleanup; check `pgrep -a -x nr-softmodem`, `pgrep -a -x nr-uesoftmodem`, `ss -ltn '( sport = :4043 or sport = :9090 )'`, and traffic processes before the next arm. Do not use `SIGKILL` for the passive receiver. The runner holds the exclusive lock until cleanup ends.
