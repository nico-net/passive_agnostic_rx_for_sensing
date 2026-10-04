# R8b — independent R7/R8 review fixes

Host: DGX Spark (`spark-74c3`, aarch64). Branch: `rr/reconfig-robustness`. Scope: R8b, including the requested R9 in-flight correction. Authoritative inputs: orchestration `CODEX_BRIEF.md`, `SPEC_CURRENT.md` §4.4–4.6, plan Global Constraints and reports R7/R8/R9. No other worktree, frozen capture, spec or plan was changed.

## Findings and outcomes

1. **Noise P-RNTI — confirmed, fixed.** [OFFLINE VERIFIED] With `ISAC_RECONF=1`, `nr_pdcch_blind_monitor.c` requires FDRA, TDA, VRB, MCS and TB scaling to be zero for SMI=10, in addition to the six trailing reserved bits. These are the reserved scheduling fields in [TS 38.212 §7.3.1.2.1](https://www.etsi.org/deliver/etsi_ts/138200_138299/138212/17.10.00_60/ts_138212v171000p.pdf). The RT announcement path now requires re-encode mismatches ≤ the existing threshold + 30, including short-only rejects; the diagnostic mismatch bypass cannot authorize an epoch announcement. `NoisePrntiShortOnlyReservedFieldsNeverArm` conditions fixtures on recovered P-RNTI, SMI=10/MSB=1 and zero tail, then exercises every remaining reserved bit. A correctly encoded zero-reserved short message remains observable; `ShortMessageRequiresReencodeGate` tests the independent mismatch requirement. Short-only messages still never become PDSCH grants.

2. **Pre-boundary SIB1 comparison — confirmed, fixed.** [OFFLINE VERIFIED] `nr_cfg_epoch_note_sib1(hash, abs_slot)` ignores pre-boundary observations while an announcement is pending. Post-boundary equal hashes clear the pending event without a bump; different hashes bump once. Scheduling does not start the pending re-read before the boundary. Missing SIB1 causes fallback only after `ISAC_RECONF_SI_GRACE_MS` (default 5000), retaining `ISAC_RECONF_SI_BUMP_WITHOUT_SIB1=0` as the fallback override. This delay intentionally follows the R8b user instruction over the spec's older immediate-boundary fallback. Tests cover early observations, changed/unchanged post-boundary SIB1, deadline and disabled fallback. [IMPLEMENTED, NOT VALIDATED on live traffic] The RRC hook uses the received BCCH frame/slot, not the later MAC configuration time.

3. **Cache cannot load before live SIB1 — confirmed, fixed.** [OFFLINE VERIFIED] The common-facts cache now persists `sib1_common_pci<PCI>_last_hash.bin` alongside `(PCI, semantic hash)` entries. A new sync clears the applied live hash and permits a fresh cache lookup through that index. The loaded facts remain a cache hypothesis: their hash never feeds the epoch authority or becomes the applied live hash. The next live SIB1 replaces/rekeys the hint even when its common-facts subset is identical. `StartupHintRekeysOnLiveDecodeWithoutEpochBump` uses an isolated temporary directory and verifies load with hash zero, replacement, and zero startup bumps. Flag-off filenames and cache selection remain unchanged. Files: `nr_pdcch_blind_monitor.{c,h}`.

4. **Listener lock hazard — confirmed, fixed.** [OFFLINE VERIFIED] The epoch owner queues snapshots and subscription lists while holding its state mutex. `nr_cfg_epoch_drain()` takes one event at a time, releases that mutex, and invokes listeners with no receiver mutex held. A single nonblocking drainer preserves bump order, supports recursive drain attempts, and clears job provenance around callbacks. The production drain is at entry to `nr_pdcch_blind_monitor_run_occasion`, before Phase-2/bank/length locks; dequeue already released the queue mutex, and the inline call follows return from MAC indication. `note_*` and `tick` never dispatch callbacks. The regression holds a harness length mutex across two bumps, then drains a listener that acquires that mutex; events arrive exactly once in order. [IMPLEMENTED, NOT VALIDATED on live traffic] R10 can use this callback contract; no R10 consumer reset/relearning policy was implemented.

5. **Flag-off reject log — confirmed, fixed.** [OFFLINE VERIFIED, source inspection] The new P-class `DCIREJECT` branch is gated by `ISAC_RECONF`. The additional short-only reserved-field rejection is also gated, preserving the previous flag-off reject reason. This removes the identified log delta against `d1e3e79c3e`; a live byte-for-byte log comparison remains deferred. Other reconfiguration behavior retains its existing flag guard.

6. **Repeated full SIB1 configuration — confirmed, fixed.** [IMPLEMENTED, NOT VALIDATED on live traffic] Canonical hashing is exported from `config_ue.c` and invoked in the RRC BCCH decode path before `nr_rrc_process_sib1`. A repeated applied live hash feeds the epoch and releases the new ASN.1 message without rerunning RRC timers, `config_common_ue_sa`, or common BWP configuration. A first live decode after sync (applied hash zero), or a changed hash, follows the full path. The NAS PLMN allocation was reachable on every full decode; it now reuses the existing allocation. [OFFLINE VERIFIED] Periodic MAC acquisition is capped at eight monitoring occasions (`ISAC_RECONF_SIB1_MAX_OCCASIONS`) or one second, then retried on the five-second cadence; `PeriodicSib1WindowIsBounded` covers both limits. [OFFLINE VERIFIED, source inspection] During the periodic window, PDCCH configuration is refreshed once on request, rather than every slot. Under reconf, DCI 1_0 uses SIB1 refPoint/TDRA/PDSCH defaults only for SI-RNTI, so other RNTIs retain their normal interpretation. Initial acquisition behavior is retained. These RRC/MAC integration paths were compiled in the required UE build; the unit fixture does not emulate the full asynchronous RRC/MAC exchange.

7. **Identity rounding — confirmed, fixed.** [OFFLINE VERIFIED] `nr-ue.c` quantizes the derived SSB frequency to SCS/2 before passing it to the identity owner. The helper test equates a 1 Hz rounding change and distinguishes a 15 kHz change at 30 kHz SCS. Existing identity tests retain HARD_RESET for actual identity changes. The Point A evidence currently supplied by SIB1 is an integer offset, not a floating frequency estimate, so no rounding conversion was added to it.

8. **In-flight stale evidence — confirmed, fixed at result consumers.** [OFFLINE VERIFIED] `nr_passive_cfg_epoch.{c,h}` now carries thread-local job provenance through nested synchronous decoders, with cleanup on every scope exit. Queue admission inherits that original stamp instead of relabeling old work with the latest epoch. Queued scan, individual PDSCH slot-group members and PUSCH jobs bind their stored stamp and their existing dropped-epoch counter. Inline scans also bind an epoch. Checks now cover length scorer results (before trial/pass credit), length locks/touches, layout selection/seeding and feedback, CORESET addition/accept/reverification, discovery observations, Technique D CRC/DM-RS/Qm/k0 feedback, and decoder-local UL/DL learned outcomes. Checks are repeated after consumer mutex acquisition where feedback can wait on a lock. Each stale job increments its existing queue counter once. [OFFLINE VERIFIED] `InFlightJobAcrossBumpNotCredited` injects a bump inside the production length scorer callback, verifies zero trial/pass credit and refuses subsequent length/pin updates; the epoch-owner fixture checks once-only counting. [IMPLEMENTED, NOT VALIDATED on GPU/live traffic] The GPU FEP worker invalidates a batch that straddles a bump, and each owning PDSCH job checks again before consuming it. CPU/GPU LDPC results reach the same guarded feedback paths. This does not roll back work/evidence completed before the bump, nor implement R10's conversion of existing learned state to hints.

## Minor findings and continuity

- [OFFLINE VERIFIED] HARD_RESET clears `si_period_slots`, including Point A resets. Regression: `HardResetClearsSiPeriod`.
- [OFFLINE VERIFIED] Modification boundaries follow SFN, rather than an arbitrary unwrapped multiple for periods exceeding the 1024-frame SFN cycle. For those periods, the next representable SFN satisfying `SFN mod m = 0` is SFN zero ([TS 38.331 §5.2.2.2.2](https://www.etsi.org/deliver/etsi_ts/138300_138399/138331/16.05.00_60/ts_138331v160500p.pdf)). `ModificationPeriodBeyondSfnWrapUsesSfnZeroBoundary` covers a 2048-frame period.
- [OFFLINE VERIFIED, artifact inspection] `rg -n 'RXDISCONT' tests --glob '*.log' --glob '*.txt'` found no entries. These checked-in artifacts do not establish a live event rate. Continuity loss remains HARD_REVERIFY, unchanged.

## Validation

[OFFLINE VERIFIED] Every build/test command below was preceded by the HOLD/BUSY GATE, builds used `nice -n 19` and at most `-j8`, and ctest used at most `-j4`. No receiver, bed, simulator campaign, OTA or GPU benchmark ran.

```bash
while [ -e /home/nicola/NICOLA/wt/rr-orchestration/HOLD ] || [ -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]; do sleep 60; done
```

[OFFLINE VERIFIED] Initial regressions failed before their fixes: four epoch tests (boundary, early request, callback deferral, reset period), one reserved-field decoder fixture, and the cache startup fixture. Logs: `/tmp/rr-r8b-red-epoch.log`, `-red-prnti.log`, `-red-cache.log`. New identity/work-scope APIs first failed compilation (`-red-identity.log`, `-red-inflight.log`, `-red-gate.log`). Iteration commands:

```bash
CCACHE_DIR=/tmp/rr-r8b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_passive_cfg_epoch test_nr_pdcch_blind_monitor test_nr_pdcch_dci_length_sweep test_nr_pdsch_config_sweep test_nr_pdcch_coreset_bank
nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R 'test_nr_(passive_cfg_epoch|sib1_cache_hint|pdcch_blind_monitor|pdcch_dci_length_sweep|pdsch_config_sweep|pdcch_coreset_bank)$' --output-on-failure
```

[OFFLINE VERIFIED] Focused ctest: **6/6 passed** (`/tmp/rr-r8b-focused-tests.log`). Final required build: **passed**, log `/tmp/rr-r8b-full-build.log`:

```bash
CCACHE_DIR=/tmp/rr-r8b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
```

[OFFLINE VERIFIED] Full ctest: **125/134 passed**, nine documented baseline failures only: `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`, `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. Log: `/tmp/rr-r8b-ctest-final.log`. CMake adds the flag-on cache/feedback/P-RNTI fixtures as `test_nr_sib1_cache_hint` and links the epoch authority into affected standalone tests.

[OFFLINE VERIFIED] Shuffle commands (GATE before **each** invocation), seeds 1/3/5:

```bash
B=cmake_targets/ran_build/build
for seed in 1 3 5; do
  for binary in test_nr_passive_cfg_epoch test_nr_pdcch_blind_monitor test_nr_pdcch_dci_length_sweep test_nr_pdsch_config_sweep test_nr_pdcch_coreset_bank openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_ra_procedures; do
    # GATE above
    nice -n 19 "$B/$binary" --gtest_shuffle --gtest_random_seed="$seed" --gtest_brief=1
  done
  # GATE above
  nice -n 19 env ISAC_RECONF=1 "$B/test_nr_pdcch_blind_monitor" --gtest_filter='Sib1Cache.*:EpochFeedback.*:BlindPdcchTest.NoisePrntiShortOnly*:BlindPdcchTest.Dci10PRntiShortMessageOnly*' --gtest_shuffle --gtest_random_seed="$seed" --gtest_brief=1
done
```

[OFFLINE VERIFIED] All **24/24 shuffle invocations passed**. Per seed: epoch 34/34; blind monitor 208 passed + 5 skipped (two existing skips and three flag-on-only fixtures); length sweep 43/43; Technique D 44 passed + 1 existing skip; bank 12/12; MIB handoff 6/6; RA procedures 2/2; flag-on fixtures 4/4. Logs: `/tmp/rr-r8b-shuffle-<binary>-<seed>.log`; status ledger `/tmp/rr-r8b-shuffle-status.txt`.

[OFFLINE VERIFIED] `git diff --check` and the required sens6 frozen-file gate passed before commit.

## DEFERRED MEASUREMENTS

[PLANNED] In an orchestrator-approved quiet slot, flag-off/on regression and the stable-cell one-hour target (≤1 false SOFT/hour, zero false HARD):

```bash
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r8b-off bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r8b-on bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build bash tests/passive_rx/dgx/rfsim_arm.sh /tmp/rr-r8b-stable-3600 3600
rg 'CONFIG_EPOCH|RXDISCONT' /tmp/rr-r8b-stable-3600
```

[PLANNED] GPU build coverage requires an operator-provided preconfigured GPU directory; none was configured here. After GATE: `CCACHE_DIR=/tmp/rr-r8b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build_gpu -j8 nr-uesoftmodem tests`. GPU timing/real batch-straddle measurements, an injected live post-boundary SIB1 change, and a SIB1-less live arm remain orchestrated follow-ups. The current branch still lacks a working `ISAC_TD_IGNORE_SIB1` arm, so no such command is presented as runnable.

[IMPLEMENTED, NOT VALIDATED on live traffic] Offline results establish the tested transition/feedback contracts, not recovery-time or false-trigger-rate targets. Those require the deferred measurements; no stable-cell rate is claimed from these unit tests.
