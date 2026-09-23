# Passive NR RX: implementation status, verified findings, and remaining work

Evidence snapshot: 2026-09-11, sens6, OAI repository
`/home/sens/NICOLA/adaptive-rx-UL-DL`, branch `adaptive-rx-UL-DL`.

**Overall status: INCOMPLETE. The receiver is not yet a fully agnostic passive RX.**
This document consolidates existing source audits and saved validation artifacts;
it is not a claim that a new test campaign was run while writing it. Some
implementation changes remain in the working tree. Artifact paths below are on
sens6. Earlier progress documents contain historical NOT RUN entries superseded
by the later evidence explicitly identified here.

## 1. Current implementation and work still required

| Work item | Current status | Remaining implementation / validation |
| --- | --- | --- |
| Raw-IQ input and replay | Implemented and exercised on real recordings; file-only backend, timestamps, EOF, digital tuning and production sample scaling | Broader sample-rate/numerology support; deterministic receiver scheduling and reproducible event-time measurements |
| Synchronization and broadcast acquisition | Repeated PBCH/MIB and SIB1 recovered automatically from all three short raw baselines and the long baseline | Explicit acquisition states and entry/exit confidence; broader unknown-cell coverage; no assumption that one capture proves general acquisition |
| DCI decode versus interpretation | Raw DCI recovery, RNTI recovery, manual 1_1 interpretation and a bounded automatic layout search exist | Generalized 1_0/1_1 interpretation across BWP, allocation types, optional fields and dedicated configurations |
| Hypothesis generation and validation | Protocol checks, bounded layout/configuration searches, effective-DMRS legality and CRC-led evidence exist | Independent RE/TBS/LDPC/rate-matching checks, temporal/HARQ evidence and configuration provenance in one inspectable interpretation contract |
| Ambiguity handling | Automatic 1_0 class ambiguity barrier implemented and tested; manual selection preserved | Extend explicit UNRESOLVED/AMBIGUOUS/VALIDATED/REJECTED states across 1_1 and configuration hypotheses; expose unsupported/unsearched domains |
| Dynamic configuration learning | Keyed PDSCH contexts separate configuration, RNTI and TDA; generations protect against stale queued results | Unified cell/BWP/CORESET/search-space/RNTI/DMRS/CSI-RS state, observation identity and confidence; prevent duplicate observations from manufacturing evidence |
| Acquisition state machine | Individual acquisition stages work in raw replay | Implement SEARCH_SSB, PBCH_LOCK, CELL_CONFIG, SEARCH_PDCCH, TRACK_PDCCH and TRACK_SCHEDULING with explicit transitions and hysteresis |
| Recovery | Local DL hypothesis relearning after sustained contradictory CRC evidence implemented; controller tests pass | Connect recovery to synchronization, PBCH/PDCCH loss, CFO/STO drift, RNTI loss, BWP changes and replay discontinuities; test actual IQ interruption without process restart |
| Health metrics | Queue, CRC, broadcast and local-relearning diagnostics exist | Unified confidence/health reporting, denominators, unresolved hypotheses, last validated scheduling time, and reliable sample-time windows |
| CSI-RS / dedicated layouts | Configured CSI-RS resource processing exists; enabling a source is not discovery | Passive resource hypotheses, observability limits, unsupported-layout reporting, and broader dedicated PDSCH/DMRS interpretation |
| Offline validation and regression | Native tests, manual decoder oracle, short raw acquisition checks and long real-IQ replay available | Multiple materially different cells/layouts, full manual-auto allocation equivalence, invalid-layout vectors, configuration-change and IQ-loss sequences |
| Operational robustness | A valid two-minute raw baseline exists; late-run CRC can be high | Investigate run-to-run UL variation and scheduling/drop effects without assuming a cause or tuning against one result |

The current automatic 1_1 family is not a general 3GPP interpreter. The existing
source audit identifies assumptions including Type-1 frequency allocation, one
codeword, fixed-width HARQ/DAI/feedback/antenna-port/SRS fields, and a restricted
BWP/TDRA-width family. The PDSCH search also has a bounded S/L/DMRS/MCS catalog.
A recovered polar CRC does not prove those assumptions correct. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the detailed field audit.

Runtime inference must remain passive and independent: no gNB configuration,
scheduler hints, dedicated coordination or external cell synchronization. Manual
parameters are permitted only in clearly labeled oracle/regression tests.

## 2. Measured validation results

### Native tests and manual reference

| Check | Latest confirmed result | Evidence |
| --- | --- | --- |
| PDSCH configuration/recovery controller | 18/18 PASS | E1 test XML |
| DCI length sweep | 8/8 PASS | E1 test XML |
| Blind PDCCH monitor suite | 114/117 PASS; three failures remain | E1 test XML |
| Recovery test against old controller | FAIL: still settled after 5,000 injected failures | E1 old-controller regression log |
| Same recovery behavior after change | PASS in the controller suite | E1 test XML |
| Saved manual DL decoder oracle | 55/55 identical controls, zero failed; exit 0 | E2 manual log |
| UL observations in that oracle | 17 raw UL records, NOT 17 successful UL decodes | E2 manual log |
| Three short raw captures | 3/3 PASS_RAW_BROADCAST_ACQUISITION; each has two PBCH observations and one SIB1 | E2 acquisition JSON files |
| Long raw replay after recovery change | Valid transport, broadcast acquisition, EOF; no local recovery events on this uninterrupted recording | E3 |

The pre-change controller suite had 13 passing tests. The five added recovery
cases cover sustained loss and relearning, isolation of other contexts, stale
feedback, intermittent losses, a marginal-link reference, queued exploration
feedback and invalid recovery-policy parameters. The old-controller comparison
used a compatibility shim for the new policy API; its feedback/recovery behavior
was unchanged. This is controller-level testing, not an RF interruption test.

The three unchanged blind-monitor failures are:

1. `BlindPdcchTest.UlControllerPoolsEvidenceAcrossUesAtTheSameDciLength`
2. `UlFieldSweep.NeverInterpretableClassCannotBlockConvergence`
3. `UlInterpSweep.FullCatalogueCannotSilentlyBypassClassCap`

Do not rewrite these tests merely to make the suite green. Their contracts and
implementations still require investigation.

### Long-capture CRC: retain both observations

Both runs used the same saved 120-second, single-channel raw recording in automatic
mode. Queue rates include interpretation/search attempts and use the last emitted
cumulative snapshot, not a proven fully drained census of every unique scheduled TB.

| Replay | Cumulative DL | Cumulative UL | Final 10 reporting intervals DL | Final 10 reporting intervals UL |
| --- | --- | --- | --- | --- |
| Memory-fix baseline, E4 | 41,909/154,418 = 27.14% | 2,190/7,290 = 30.04% | 6,174/8,029 = 76.90% | 286/370 = 77.30% |
| After local-recovery change, E3 | 55,405/155,584 = 35.61% | 4,284/9,620 = 44.53% | 6,391/8,316 = 76.85% | 208/459 = 45.32% |

Proven conclusions:

- Late-run DL decoding reached approximately 77% in both observations.
- Late-run UL was not repeatably 77%: the later observation was 45.32%.
- Neither run achieved a cumulative 60% pass rate in both directions.
- UL stale-job drops were 372 in E4 and 231 in E3; DL stale drops were 4 and 0.
- E3 logged no local DL relearning event. This does not establish the cause of
  the changed UL result or rule out other implementation/runtime effects.
- Reporting intervals are NOT necessarily equal-duration seconds. Earlier
  `TRACKLOCK`-derived elapsed-time labels were invalid because logging is throttled.
  Those time labels must not be used to claim a measured convergence time.

The rate increase supports learning/convergence behavior, but does not establish
an exact one-to-two-minute convergence threshold. The cause of the UL variation
has not been demonstrated. Do not label it a proven radio fault, thread-affinity
fault, CFO fault, or regression caused by the recovery change.

## 3. Problems solved, with demonstrated scope

| Problem | Proven correction / result | Boundary |
| --- | --- | --- |
| Long replay killed before acquisition | File mapping inherited receiver memory locking; disable locking when opening the file-only backend. Pre-fix memory test fails; post-fix 512 MiB mapping under a 64 KiB lock limit passes; full long replay reaches EOF | Host/replay bug, not an X410 hardware failure; live-radio locking policy unchanged |
| Wrong replay amplitude conversion | Replay matches the X400 production two-bit signed arithmetic shift, including negative values. Exhaustive test covers 65,536 int16 values at all 16 shifts | Correct scaling alone did not solve automatic interpretation/CRC performance |
| Stale MIB handoff during acquisition | Process queued RRC-to-MAC messages until the actual CONFIG_MIB message arrives, preserving FIFO order, instead of treating the next message as MIB | Real raw broadcast acquisition subsequently passes; this is not a full reacquisition state machine |
| Empty-window SSB checker numerical failure | Use double-precision energy/correlation and zero score for zero-energy windows, without changing detection thresholds; five retained checker tests pass | Reference-frequency checker results are not autonomous PBCH/DCI proof |
| Permanent DL hypothesis winner after operational loss | Reopen only the affected context after sustained failures contradict its learned conservative CRC bound; clear evidence, advance generation, reject stale tickets | Tested at controller level; correlated fading can also trigger local search, so this is not proof of a BWP change |
| Silent first-match ambiguity for tested DCI 1_0 classes | Enumerate enabled classes; withhold ambiguous allocation; expose candidate rejection reasons | Tested scope is 1_0 classes under supplied contexts, not generalized 1_1/dedicated interpretation |
| UL diagnostic replay missing required initialization | Earlier invalid attempt was marked VOID; corrected diagnostic run produced repeatable 6/17 UL CRC passes | A configured short diagnostic, not fully agnostic UL validation |

## 4. X410, capture and host integration: problems and remedies

This section covers issues established in the available experiment evidence. It
separates host/recorder failures from radio hardware failures. It is not a claim
that every historical X410 incident has a diagnosed root cause.

### 4.1 Disk throughput exhausted the recorder buffer

Two recordings stopped explicitly with
`disk backlog exceeded bounded ring; capture VOID`:

- Full-300-second attempt, approximately 148 seconds saved, 1 GiB buffer.
- Skip-90/save-210 attempt, approximately 186 seconds saved, 12 GiB buffer.

No RF timestamp gap was reported before these stops. The proven failure was disk
backpressure, not evidence of an X410 RF stall. The two failed IQ files, totaling
164,390,502,400 bytes, were removed with user approval; diagnostics remain (E6).

**Demonstrated remedy for the requested baseline:** receive continuously for five
minutes on one channel, discard the first 180 seconds, and save the final 120
seconds with a 12 GiB bounded buffer. The resulting 58,982,400,000-byte file passed
continuity, extent, NIC-loss and independently reread SHA-256 checks (E5).

This is a working shorter-save strategy, not proof that storage can sustain five
minutes of full-rate writing. A longer saved recording requires measured sustained
storage bandwidth or a separately validated lower-bandwidth/lossless recording
path. A larger buffer alone already failed and is not a demonstrated general fix.

### 4.2 Large recording caused replay OOM

Linux killed the replay before acquisition while about 20 GiB was memory-locked.
The raw mapping inherited `MCL_FUTURE`; the 59 GB recording exceeded available RAM.

**Demonstrated fix:** disable inherited memory locking inside the file-only replay
backend before mapping IQ. E7 contains the old failing test and passing memory,
transport and exhaustive scaling tests. Subsequent long replays reach EOF.
Do not disable live-radio real-time locking as an untested workaround.

### 4.3 Recorder sample format versus production receiver input

The raw file stores unshifted little-endian signed int16 I/Q. The X400 production
receive path supplies the PHY with a two-bit signed arithmetic shift.

**Demonstrated fix:** use `--sample-shift 2` with this recorder/production path.
Do not substitute four bits, floating division with truncation toward zero, or an
unlabeled amplitude adjustment. This is a format contract, not RF gain tuning.

### 4.4 Radio ownership and lock-file access

The X410 has been shared with other experiments. Capture preflight required the
device to report unclaimed and acquired the shared radio lock. A root open of the
existing `/tmp` lock using creation flags encountered protected-file permissions.

**Working lock acquisition used by the successful capture:**

```sh
# In the same shell that owns the capture; do not run against an occupied radio.
exec 9</tmp/adaptive-rx-UL-DL.radio.lock
flock -n 9
# Run the authorized passive recorder here, keeping descriptor 9 open.
```

The read-only descriptor avoids recreating/truncating the existing lock file.
Hold the lock through radio use, stop/drain RX, release UHD handles, then unlock.
A lock only coordinates clients that honor it; device ownership must also be
checked. Never force-reset an occupied device to bypass another experiment.

### 4.5 Connectivity and earlier RFSTALL/CFO/antenna reports

The successful capture used these last-observed connection values:

| Item | Value |
| --- | --- |
| sens6 LAN SSH | `128.178.122.140` |
| X410 data address | `192.168.20.2` |
| X410 management address | `128.178.122.174` |
| X410 serial | `327C1F2` |
| Host RF interface | `enp129s0f0np0` |

These are capture provenance, not a promise of permanent addresses or an agnostic
cell configuration. No validated persistent static-IP repair is established by
the artifacts summarized here.

Earlier conversation reports mentioned SSH/IP reachability, RFSTALL, CFO-related
stops, antenna placement and possible thread changes. **Their radio-specific root
causes and permanent remedies are not proven in this evidence set.** Do not record
a reboot, antenna move, GPU migration or affinity change as a demonstrated fix.

For a recurrence, the required diagnosis is to distinguish host SSH/VPN routing,
management connectivity, the data interface/route, device ownership, UHD metadata,
NIC loss, disk backlog and receiver timing/CFO logs. Preserve those observations
before changing settings. Assign a remedy only to the demonstrated failing layer.
A missing decoder update by itself is not evidence that the X410 stopped streaming.

Valid replays did estimate CFO around -15 kHz and acquire broadcasts; that does
not establish why any earlier live run stopped. This document prescribes no
unverified RF parameter tuning, network reconfiguration or hardware reset.

### 4.6 What the successful baseline actually contains

- Physical X410 RX channel 2, logical file channel 0; RX1, gain 40 dB.
- 3450 MHz center, 122.88 MSamples/s, internal clock/time sources.
- 14,745,600,000 complex samples, interleaved int16 I/Q, 120 saved seconds.
- No receiver CFO/STO correction applied during raw recording.
- Three preceding receive minutes discarded; no decoder state saved or warmed
  for replay. Replay acquisition and learning start from the saved samples.
- Single antenna: no equivalent four-branch spatial information is preserved.
- Raw integrity does not prove CSI-RS coverage, all NR messages, or all layouts.
- Radio handles were released after capture; offline replay opens no radio.

## 5. Acceptance gates

| Gate | Current outcome | Required evidence to close |
| --- | --- | --- |
| 1 Manual equivalence | OPEN | General automatic 1_1 allocation/configuration equality for every successful manual capture; no cell-specific manual inputs |
| 2 Multiple layouts | PARTIAL | Several materially different automatic 1_1/1_0 configurations beyond the current bounded family |
| 3 Ambiguity | PASS only for tested 1_0 case | Extend proof to 1_1 and configuration-domain ambiguity; never silently choose an unvalidated layout |
| 4 Full replay acquisition | PARTIAL | Raw SSB/PBCH/SIB1 and scheduling attempts exist; generalized, self-validating interpretation chain remains incomplete |
| 5 Reacquisition | OPEN; local controller tests pass | Forced IQ loss/configuration change, explicit transitions and in-process end-to-end recovery |
| 6 Invalid rejection | PASS only for tested scope | Broader malformed/unsupported dedicated layout combinations with explicit reasons |
| 7 Regression | OPEN / SUITE FAIL | Resolve three existing failures and complete field-level, multi-capture, reproducible automatic regression |

## 6. Next implementation priorities

1. Establish repeatable UL measurements and a reliable sample-time axis; explain
   the two long-replay UL outcomes without assuming a cause.
2. Generalize the DCI interpretation/evidence contract and preserve manual mode.
3. Add observation identity and a generation-aware cell/configuration state.
4. Integrate the acquisition/recovery state machine with raw-IQ discontinuity and
   configuration-change tests, not only synthetic controller feedback.
5. Add CSI-RS/dedicated-resource inference or explicit unsupported/ambiguous status.
6. Close the acceptance gates using independent vectors and existing captures.

No additional capture is required to start these steps. Future OTA validation,
only with explicit availability/permission, must check unknown-cell cold start,
automatic interpretation, sustained DL/UL decoding, forced loss/reacquisition and
configuration changes. Hardware availability must not block offline development.

## 7. Evidence index

All absolute paths are on sens6; IQ files are not added to Git by this document.

| ID | Evidence |
| --- | --- |
| E1 | `/home/sens/NICOLA/captures/agnostic_recovery_change.AY0slj/`: `build.log`, `test_nr_pdsch_config_sweep.xml`, `test_nr_pdcch_dci_length_sweep.xml`, `test_nr_pdcch_blind_monitor.xml`, `old_controller_regression.log` |
| E2 | E1 `followup/`: `manual_status.txt`, `manual_oracle.log`, `raw_fullband_4s.p67kVf.acquisition.json`, `window_1.acquisition.json`, `window_2.acquisition.json` |
| E3 | E1 `replay_last120/`: `result.json`, `receiver.log`, `crc_regression.json` |
| E4 | `/home/sens/NICOLA/captures/crc_last120_mlockfix_20260911T125339Z/`: `crc_performance.json`, `CRC_PERFORMANCE.md`, `result.json` |
| E5 | `/home/sens/NICOLA/captures/raw_5min_last120.v51DBe/capture_120s/`: `validation.json`, `capture_info.json`, `timestamps.tsv`, `IQ_SHA256SUMS`, `checksum_reread.log`, `rx0.sc16` |
| E6 | `/home/sens/NICOLA/captures/raw_long_5min.mOLDO7/capture_300s/` and `/home/sens/NICOLA/captures/raw_5min_skip90.ObDnPd/capture_210s/`: failure/validation/removal records; failed IQ removed |
| E7 | `/home/sens/NICOLA/captures/crc_replay_mlock_fix.fB1Lxg/`: pre-fix failure and post-fix memory, transport and scaling test logs |
| E8 | `/home/sens/NICOLA/captures/raw_replay_mib_gate.0aPDbq/mib_wait.patch`; later raw acquisition checks in E2 |
| E9 | [SSB checker validation](../raw_baseline/SSB_VALIDATION.md); its later raw-acquisition NOT RUN text is historical and superseded by E2/E3 |
| E10 | [Architecture/source audit](ARCHITECTURE.md), [progress and initial DCI test evidence](PROGRESS.md) |
| E11 | `/home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.xRBLS0/`: saved native manual oracle and `ul_validation/` diagnostics |

An absent, incompatible, truncated, timed-out or otherwise invalid test is VOID.
Do not infer CRC performance or close a gate from a VOID run. Source inspection,
unit tests, raw integrity, acquisition, scheduling interpretation and successful
transport-block decoding are distinct evidence levels and must remain separate.
