# UL-INT — integrated first-PUSCH CPU path (U1–U9)

2026-10-08, `csd/ul-int`, DGX aarch64 sandbox. The first-PUSCH milestone requires a nonzero full PUSCH TB CRC for a C-RNTI whose DCI 1_1 length was trusted by the DL receiver, measured from its first accepted DCI 0_1. No gNB configuration or log is a receiver input.

## Implementation

- **[CODE-READ + IMPLEMENTED]** Merged `csd/ul-b`, `csd/ul-c`, and the evolving `csd/t26b` onto UL-A. The only UL-C conflict was its per-symbol FEP macro: the resolution retains UL-A's bounded IQ snapshot and UL-C's per-symbol timing sweep. The existing CSI-RS contract in `nr_csirs_disc.h` was not changed; existing semantics are untouched.
- **[IMPLEMENTED]** `ISAC_UL_PUSCH_CHAIN=1` is default off and activates the bounded UL worker, DL-locked per-UE state, banked DCI 0_1, U3 ticket catalogue, timing and CP DM-RS discovery. The DL-only full-auto OTA config gets DCI 0_1 scanning and PUSCH decode under this flag. Admission requires a current trusted DL DCI 1_1 ticket and a producer-time U3 hypothesis ticket. The accepted 0_1 is parked for k2, copied into the bounded worker, and fed through the existing gNB PUSCH FEP/chest/one-layer demod/rate-recovery/LDPC/TB-CRC path. The no-UCI decode is tried first; a CSI-request or nonzero DAI is logged as a tentative UCI hint. Existing optional UCI recovery remains separately configured.
- **[IMPLEMENTED]** DM-RS discovery's actual nSCID/DM-RS ID and the data ID used by the decoder now reach U4's TB-confirmed state instead of confirming the original grant's ID. Per-UE data-ID hypotheses rank PCI and DL PDSCH/PDCCH IDs, then walk the 10-bit domain at one candidate per failed grant, revisiting a DL prior every 16 failures. Full nonzero TB CRC remains the only ID/config confirmation. Identity generation, incarnation and epoch guard delayed results.
- **[IMPLEMENTED]** `UL_FIRST_0_1`, `UL_PUSCH_CRC`, and `UL_FIRST_TB_OK` lines report source/target absolute slots, MCS, PRBs, applied TA, tentative UCI hint, IDs, decode status and CPU microseconds. The first accepted DCI 0_1 starts each per-identity clock; a nonzero CRC-valid TB stops it. `UL_CHAIN_DROP`, `PUSCHQ` and `pusch_book` expose admission/queue losses.

## Verification

- **[OFFLINE VERIFIED, build/unit]** CPU receiver and focused UL tests built. Ten banked DCI/U3/first-PUSCH tests passed, including two new identity/timer and data-ID sweep tests; `test_nr_pusch_ul_disc` passed. The exact first-PUSCH tests reject duplicate/stale identity credit and preserve every data-ID sweep position around a prior retry. Banked known-RNTI noise controls reported 0/5,000 false accepts. These fixtures are not an OTA TB CRC result.
- **[OFFLINE VERIFIED, replay `raw_pci415_12s_b`, baseline flags]** 1,474,498,624 of 1,474,560,000 samples/channel reached raw-IQ EOF. PBCH and NSA SIB1 decoded. Final PDCCH census: 1,041 occasions, 3,123 candidates, 135 accepts (130 SI, five TC, **zero C-RNTI and zero DCI 0_1**). `pusch_book` parked/claimed 0/0; `PUSCHQ` queued/decoded/CRC-OK 0/0/0 and every UL queue drop counter 0. No `UL_FIRST_0_1`, `UL_PUSCH_CRC`, arrival or DM-RS observation exists. The idle UL worker used 1,491.309 ms CPU over 411,492.314 ms wall time (0.0036 core); per-grant CPU and first-CRC latency are unmeasured. The run used the brief's CPU flags and the UL-A config, before the later T26B tip and its dedicated DL solver flags. Log: `/tmp/ulint_replays/replay_ulint_b_163645/run.log`.

## Current gate and limits

- **[CODE-READ + OFFLINE VERIFIED]** Without a GPU, `nr_pdcch_coreset_map_idsweep_want()` enables only an optional CPU snapshot dump (`ISAC_COREMAP_IDSWEEP`), whose full-ID sweep is offline; it does not publish live dedicated CORESET banks. The baseline replay's zero C-RNTI/0_1 census therefore stops before U2, U3, and any PUSCH grant. U5–U9 cannot be judged on that run. `csd/t26b` independently found DL-locked C-RNTIs and PDSCH CRCs on the older capture with GPU nID discovery; that result is receiver-derived, not gNB truth.
- **[CODE-READ]** The reused one-layer gNB demod combines four RX channel estimates by matched filtering. For rank one under equal white branch noise this is decision-equivalent to the MMSE direction, but explicit noise-regularized four-RX MMSE and numerical parity remain unverified. Transform-precoded, multilayer, PTRS/complex UCI and unsupported RV cases remain outside this first no-UCI CP-OFDM path.
- **[ANALYTICAL + OFFLINE VERIFIED controls]** A reported milestone requires a DL-trusted C-RNTI ticket, CRC-accepted DCI 0_1, current epoch/incarnation, U3 ticket, and a nonzero full TB CRC. All-zero TBs are excluded. The synthetic 0/5,000 known-RNTI noise control constrains the DCI stage only; a quantitative real-waveform false-accept rate still requires wrong-RNTI/wrong-ID replay controls and an observed TB pass.

## Replay and orchestrator commands

Baseline main-capture command actually run (no sudo, no GPU):

```sh
REPLAY_PREFIX='' REPLAY_OUT=/tmp/ulint_replays REPLAY_LAST=/tmp/ulint_replays/last \
BASECFG=/home/nicola/NICOLA/wt/rr-csd-d/tests/passive_rx/dgx/ue.passive.ota.ula.cfg \
CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 \
nice -n 19 /home/nicola/NICOLA/wt/rr-orchestration/csd_replay_user.sh ulint_b \
/home/nicola/NICOLA/wt/rr-csd-d/cmake_targets/ran_build/build 0.25 \
ISAC_CORE_PLAN=0 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_PDCCH_NID_BANK=1 \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_DCI11_HARD_REFUTE=1 \
ISAC_DCI11_HARQ_SEQ=1 ISAC_DCI11_STAGE2=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_PUSCH_CHAIN=1
```

**[DEFERRED GPU replay/OTA]** Use the orchestrator's current validated X410/DPDK launcher and DL dedicated config with this exact feature environment (the replay additionally sets `ISAC_CORE_PLAN=0`; no radio command was run here):

```sh
ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 \
ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1 \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 \
ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 ISAC_PDCCH_KNOWN_PRESCREEN=1 \
ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_DCI11_HARD_REFUTE=1 \
ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 \
ISAC_PDSCH_SOLVER_REUSE=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_PUSCH_CHAIN=1
```

If the orchestrator's GPU replay finds a C-RNTI but cannot yet run the integrated binary, please provide its receiver-derived `JOINT_ANON_HIT`, DCI 1_1 length lock, dedicated CORESET record, each accepted raw DCI 0_1 payload/length/bank/absolute source slot, target slot, and CFO. The four-channel raw-IQ capture is already present in this worktree's host paths; those records identify the exact IQ windows for a CPU downstream fixture without gNB inputs.
