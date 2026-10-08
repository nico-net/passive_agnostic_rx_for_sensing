# ULB — banked DCI 0_1 and no-RRC UL configuration bootstrap

Branch `csd/ul-b`, based on `csd/ulbase`. ULSCOPE T26/T27 correspond to U2/U3 here. Evidence labels distinguish executable results from code inspection.

## Implementation

- **[CODE-READ + IMPLEMENTED]** `nr_pdcch_blind_monitor_rt.c:5437,5444,5505` keeps the existing banked candidate scheduler and GPU polar prefill. Under `ISAC_UL_DCI_BANK=1`, the UL length sweep uses the DL-locked C-RNTI's DCI 1_1 length as its first prior and puts neighboring lengths early in the later full sweep. On a non-PCI dedicated bank, GPU prefill and CPU fallback both descramble with the known C-RNTI and bank nID. The joint unknown-RNTI fallback is disabled for these UL tasks (`:3312`). The existing distinct-payload plus CRC significance lock remains the acceptance gate; no UL BWP is required for length discovery.
- **[IMPLEMENTED]** `nr_pdcch_ul_config_disc.c:87,166,245` records provisional first-0_1 and length-lock sample slots and per-bank/RNTI counts. The milestone path uses a try-lock and counts busy drops. `ISAC_UL_CONFIG_DISC=1` starts a bounded, allocation-free UL BWP/FDRA/TDRA bootstrap only when the UL BWP is unknown (`nr_pdcch_blind_monitor_rt.c:6644`). It ranks the DL solver's BWP first, then whole-carrier and nearby geometries. It tries type-1, type-0 and dynamic FDRA widths, default or small blind TDRA lists, and a bounded neighborhood of BWP/SRI/CSI-request field widths. Structural grant checks, HARQ/NDI recurrence and a weak periodic TDD prior from accepted DL DCI occupancy rank k2 landing slots; none is a hard proof of RRC configuration.
- **[IMPLEMENTED]** Each provisional grant carries a producer-time `hyp_generation` ticket keyed by bank and context incarnation (`nr_pdcch_ul_config_disc.c:294,313`). The existing PUSCH feedback entry routes that ticket to the new controller (`nr_pdcch_ul_discovery.c:551`). Only a full nonzero PUSCH TB CRC can lock a hypothesis; two distinct payloads are required. Stale/duplicate tickets cannot score. Once a BWP is validated, the existing complete UL width and interpretation sweeps take over for remaining fields and TDRA rows (`nr_pdcch_blind_monitor_rt.c:6659`). The controller uses nonblocking locks on the scan path and counts busy/capacity drops; its catalogue and ticket ring are fixed size.
- **[CODE-READ]** `nr_csirs_disc.h` was not changed; its existing contract and semantics are untouched. Both new flags default off.

## Verification

- **[OFFLINE VERIFIED, synthetic]** `test_nr_pdcch_blind_monitor` exercises two independent UEs with hidden UL BWPs, a DL BWP prior that is wrong, a hidden two-row TDRA width, banked exact-RNTI raw DCI 0_1 decoding at lengths different from 1_1, pure-noise false acceptance, no-truth refusal, and stale/duplicate CRC tickets. The nine focused tests passed after the final edit. Exact-known-RNTI noise produced **0/5,000 false 0_1 accepts**. The wrong-DL-BWP fixture made 34 proposal calls: **48.53 μs mean, 159.41 μs max** on the final DGX aarch64 run, including first catalogue construction (an earlier run measured 7.22/24.74 μs). This is a synthetic controller measurement, not live RT cost.
- **[BUILD VERIFIED]** `nr-uesoftmodem` and `raw_iq` build on the CPU configuration. GPU runtime was unavailable in the sandbox.
- **[BUILD VERIFIED]** Full `ninja tests` completed 354/354 build steps. The full `ctest` gate was stopped after 224/225 completions because the unrelated CSI-RS footprint suite was still processing a 20,000-slot × 3 no-pilot fixture. `test_nr_pdcch_blind_monitor` passed. The CSI-RS async stress test failed under parallel load, then passed alone (1/1); sandbox socket failures and known ARM failures also occurred. This is an incomplete full-suite gate, not a clean pass.
- **[OFFLINE REPLAY, raw_pci415_12s_20261008, CPU]** Replay `ulb` reached `RAW_IQ_EOF consumed=1474558460 remaining=1540` of 1,474,560,000 samples/ch. PCI 415 PBCH locked and NSA SIB1 decoded without `servingCellConfigCommon`. Final metrics: 1,011 PDCCH occasions, 3,033 candidates, 127 accepts (126 SI-RNTI, 1 TC-RNTI), **0 C-RNTI**, 0 banked DCI 0_1, 0 UL length locks, 0 UL configuration proposals, and scan queue full/stale drops 0/0. First-0_1 time and lock time are **not observed**; per-RNTI 0_1 counts are empty. CPU CSI-RS found 28 resources, but no dedicated nID bank was handed to the DL pipeline. Therefore this replay cannot validate UL acquisition on the capture. The replay binary predates the final known-RNTI GPU-prefill and pre-lock timestamp edits; those edits are compile/test checked below, not replay verified.

## Decisions and limits

The no-RRC candidate catalogue is intentionally bounded. Its provisional TDRA rows cover the default table and a small two/four-row bootstrap, with the existing full UL interpreter handling wider legal possibilities after TB CRC. If the true waveform lies outside the bootstrap catalogue, the ticket remains unresolved. UL-C/UL-D can use the existing feedback route without changing the grant ABI.

The CPU replay depends on DL nID/bank discovery before any UL result can exist. It does not validate GPU batching or live RT cost. A candidate first-0_1 timestamp before length lock is provisional until the length evidence converges. The CPU CSI-RS worker reported 78,715,549 μs of accumulated batch work in this replay; that is not UL CPU cost. UL proposal cost is measured by the synthetic fixture below.

## Deferred measurements

- On the GPU host, rerun the same 12-second raw capture with `ISAC_PDCCH_NID_GPU=1 ISAC_UL_DCI_BANK=1 ISAC_UL_CONFIG_DISC=1` plus the replay brief's DL flags, and report per-bank/RNTI first-0_1, lock, counts, scan-queue drops, GPU batch time and proposal cost.
- For OTA, add `ISAC_UL_DCI_BANK=1 ISAC_UL_CONFIG_DISC=1` to the orchestrator's unchanged four-RX PCI-415 DL-dedicated X410 run. Report sample-time and wall-time milestones and verify PUSCH TB CRC through UL-C/UL-D before calling any UL configuration solved.

## Commands

```sh
CCACHE_DIR=/tmp/rr-csd-e-ccache flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem raw_iq tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_blind_monitor --gtest_filter='BlindPdcchTest.UlConfigDisc*:BlindPdcchTest.BankedUl*:BlindPdcchTest.Dci01PureNoiseFalseAcceptRateIsBounded:BlindPdcchTest.UlRawOracleRejectsDlAndWrongRnti'
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -E '^nr_td_sim_test$' --output-on-failure
sed '/pdcch_blind_monitor_dci10 = "1";/a\  pdcch_blind_monitor_dci01 = "1";' /home/nicola/NICOLA/wt/rr-csd-a/tests/passive_rx/dgx/ue.passive.ota.dldedicated.dgx.cfg > /tmp/ulb-replay.cfg
REPLAY_PREFIX='' REPLAY_OUT=/tmp REPLAY_LAST=/tmp/ulb-last BASECFG=/tmp/ulb-replay.cfg CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_SAMPLES=1474560000 CAP_TICK=210036277 /home/nicola/NICOLA/wt/rr-orchestration/csd_replay_user.sh ulb cmake_targets/ran_build/build 0.25 ISAC_CORE_PLAN=0 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_DCI11_STAGE2=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_DCI_BANK=1 ISAC_UL_CONFIG_DISC=1
```
