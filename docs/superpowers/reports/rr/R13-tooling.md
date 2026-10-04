# R13 tooling — live SA bed on sens6 (no live run)

Host of this work: DGX Spark aarch64, branch `rr/w4-r13` from `rr/integration` `c6fc03be7c`. Target operator host: **sens6 x86_64**. `[IMPLEMENTED, NOT VALIDATED on live traffic]` No OAI gNB, UE, rfsim, Docker/Open5GS or GPU bed was run in this task.

## Ready

- [CODE-READ] `tests/passive_rx/sa_bed/gnb_{baseline,dedicated,cell}.cfg` use PLMN 001/06, TAC 1 and the existing 106-PRB SA cell. Baseline has a 40-PRB first active dedicated BWP; dedicated variant changes it to 24 PRB, leaving other BWP entries/PCI alone. `nr_mac_common.c:nr_dci_size()` type-1 RIV width is `ceil(log2(N_RB*(N_RB+1)/2))`: 40→24 PRB changes 10→9 FDRA bits. `nr_radio_config.c:config_pdsch()` hardcodes DM-RS type and max length, so the requested antenna-port-table switch is not a config-only knob. A tempting change in number of BWP entries was rejected: `configure_UE_BWP()` sets `n_dl_bwp=1` for any nonempty BWP list, so it does **not** alter the indicator width in this implementation. Cell variant changes PCI 0→1. gNB MAC debug emits `DCI11_WIDTHS` and `Filling Format 1_1 DCI of size` for operator verification; no measured DCI size is claimed yet.
- [IMPLEMENTED, NOT VALIDATED on live traffic] `ue_active.cfg` holds the OAI UE test IMSI/K/OPc, and `ue_passive.cfg` uses the agnostic BWP receiver config with an unpinned scan consumer. `open5gs-{amf,smf,subscriber}.cfg` are merge templates, not replacement service files. All new configs use `.cfg`; frozen sens6 files are untouched.
- [IMPLEMENTED, NOT VALIDATED on live traffic] `run_arm.sh` checks the operator's running core, starts gNB, UE/PDU session, downlink UDP and passive receiver, waits for baseline DCI 1_1 lock and Technique D convergence, then applies stable/BWP switch/dedicated change/cell restart at a timed boundary. It captures timestamped gNB, UE and receiver logs, metrics and UeContext, enforces the exclusive bed lock and preflight checks, and tears down with SIGINT. SA and `ISAC_TD_IGNORE_SIB1=1` arms, `ISAC_RECONF=1` and flag-off controls are supported. Both controls set receiver-only `ISAC_BWP_TRACK=1`; that feature is otherwise off by default. `campaign.py` launches it in each run directory; the runbook gives five repetitions per arm.
- [IMPLEMENTED, NOT VALIDATED on live traffic] `score_r13.py` writes per-run JSON/text and campaign JSON. It scores apply-to-recovery, epoch class/cause/count, outside-window false bumps, four dropped-epoch deltas, C-RNTI grants/s and CPU/GPU CB0 path. It compares receiver DCI 1_1 winners with gNB-log DCI widths **only in the scorer** and checks hard-arm PCI 1. TDRA/DM-RS and other trusted winner use require an explicit operator `td_truth_audit.json`; without it, the scorer reports INCOMPLETE rather than 0 stale winners. The scorer's PASS requires at least a 60-minute stable-cell window and ≥100 C-RNTI grants/s; campaign PASS needs ≥5 passing runs per arm.
- [IMPLEMENTED, NOT VALIDATED on live traffic] `RUNBOOK.md` gives sens6 x86 CPU/GPU builds, model check via `nvidia-smi`, `LDPC_CUDA_ARCH=89` only if the observed GPU is sm_89, Open5GS privileged steps, commands, evidence/validation rules, teardown and troubleshooting. Current sens6 GPU model is unresolved by the 2026-09-30 snapshot versus operator belief.
- [CODE-READ] Reused the `run_bwp_switch.sh` CI telnet command, `run_passive_rx.sh` attach/UDP log patterns, `udp_dl.py`, `campaign.py`, and UeContext JSON/report tool. The old wrapper launches `run_passive_rx.sh`, whose cleanup uses `pkill -9`; R13 therefore uses a new SIGINT-only process owner. `rfsim_arm.sh` is a `--phy-test` bed without an attached UE or core; `gt_score.py` scores sensing targets, not reconfiguration. The campaign's generic `score_rx.py` verdict remains supplementary to `score_r13.py`.

## Offline checks

- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 bash -n tests/passive_rx/sa_bed/run_arm.sh` passed.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 -m py_compile tests/passive_rx/sa_bed/score_r13.py tests/passive_rx/sa_bed/test_score_r13.py tests/passive_rx/sa_bed/stamp.py` passed.
- [OFFLINE VERIFIED, DGX aarch64] `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures 'tests/passive_rx/*.conf' tests/passive_rx/sens6_host_snapshot_2026-09-30` passed.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 python3 -m unittest discover -s tests/passive_rx/sa_bed -p test_score_r13.py -v`: 14 passed. An initial syntax error was fixed before this green run.
- [OFFLINE VERIFIED, DGX aarch64] A temporary libconfig parser (`flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cc -O2 -o /tmp/rr-r13-libconfig-parse /tmp/rr-r13-libconfig-parse.c -lconfig`, then `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 /tmp/rr-r13-libconfig-parse tests/passive_rx/sa_bed/{gnb_baseline,gnb_dedicated,gnb_cell,ue_active,ue_passive}.cfg`) parsed all five gNB/UE `.cfg` files (5/5). The Open5GS AMF/SMF YAML fragments parsed with `yaml.safe_load`; the subscriber template parsed with `python3 -m json.tool` (3/3). No dedicated OAI config parser test exists in this tree.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DIR=/tmp/rr-w1-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` passed (104 Ninja steps after CMake regeneration; warnings in existing C/C++ sources).
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure`: **148/157 passed** in 1598.21 s, including `nr_td_sim_test`, reconfiguration replay and UeContext. The nine failures are exactly the brief's documented ARM/intermittent set (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`) and sandbox socket EPERM set (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`). Exit code 8 reflects those known failures.

## DEFERRED MEASUREMENTS

Operator on sens6, after Open5GS is provisioned: follow [RUNBOOK.md](../../../../tests/passive_rx/sa_bed/RUNBOOK.md) §3–§6. Exact one-arm smoke, from a fresh empty output directory (after setting `R` and `BUILD` as in the runbook):

```bash
mkdir -p /tmp/r13-sens6-smoke && cd /tmp/r13-sens6-smoke
R13_APPLY_AT_S=45 R13_DURATION_S=180 bash "$R/tests/passive_rx/sa_bed/run_arm.sh" bwp_switch sa on
python3 "$R/tests/passive_rx/sa_bed/score_r13.py" /tmp/r13-sens6-smoke
```

Run the full `campaign.py` loop in the runbook for all four scenarios × SIB1 available/SIB1-less × `ISAC_RECONF` on/off, five runs each, then `python3 "$R/tests/passive_rx/sa_bed/score_r13.py" "$C"`. The operator must also build the sens6 GPU directory, check the actual GPU with `nvidia-smi`, validate plugin registration, and keep CPU/GPU evidence separate. The standard DGX 4-RX regression gate remains for the orchestrator and was **not** run here:

```bash
cd /home/nicola/NICOLA/wt/rr-w1
flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 BUILD=/home/nicola/NICOLA/wt/rr-w1/cmake_targets/ran_build/build CELL='-C 3319680000 -r 106 --ssb 516' RXEXTRA='--ue-nb-ant-rx 4' GNBARGS='-m 9 -n 0 -M 106 -l 1' GATE_SECS=420 GATE_MODE=postconv GATE_NCTX_MIN=2 GATE_REOPENS_MAX=0 GATE_POSTCONV_CRC_MIN=99.8 GATE_POSTCONV_MIN_DEC=5000 GATE_TTC_MAX_TDA0=39 GATE_TTC_MAX_TDA2=77 GATE_CRC_FLOOR=93.4 GATE_DROP_MAX=2.5 OUT=/tmp/rr-r13-reconf bash tests/passive_rx/dgx/rfsim_regress.sh 1
```

## Open questions / limits

- [PLANNED] sens6 must show whether the BWP1 40→24 PRB change actually makes different DCI 1_1 sizes and whether the UE supports reattachment under it. The scorer rejects equal/missing sizes.
- [PLANNED] The same passive rfsim client may exit when gNB restarts. Such a run is INCOMPLETE; a new receiver process would not prove within-stream recovery.
- [CODE-READ] One active UE cannot trigger the cell-wide `DEDICATED_CHANGE_SUSPECTED` epoch: `nr_cfg_epoch_note_rnti_reopened()` requires two distinct reopened RNTIs within 2 s. This arm validates local length relock only. A two-UE bed would need a second subscriber/netns/traffic stream to test that specific epoch cause.
- [PLANNED] The TDRA/DM-RS winner ground-truth audit may need more gNB debug information than the existing logs provide. If unavailable, keep stale-winner status INCOMPLETE rather than asserting 0.
- [KNOWN ISSUE, inherited] sens6 discrete GPU runtime, rank >1 CB0 admissibility, ~3 CB0 items per grant and scan-queue drops remain unverified/unresolved; BC9c/BC10/BC11/BC12b remain deferred.
