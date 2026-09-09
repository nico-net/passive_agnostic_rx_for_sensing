# Adaptive CRC investigation — 2026-09-09

Worktree/branch: `adaptive-rx-UL-DL` on sens6. No merge. This extends
`ADAPTIVE_RX_STATUS_2026-09-09.md`; its old final CRC figures are superseded by
the measurements below. No gNB logs, scheduler hints, active UE actions,
transmission, radio reset or NIC writes were used in this investigation.

## Acceptance target and current verdict

Target: at least 60% CRC-valid TBs separately for DL and UL, per observed UE,
under independently learned operational configuration. Report search trials,
unsupported grants and queue drops separately. No cherry-picked grant shapes.

**NOT COMPLETE.** Both UL candidates exceed 60% on one small immutable recording
after the diagnostic fixes, but the UCI rescue is offline-only and UL discovery
has not converged. Live DL improvement is not yet established.

## Latest pre-fix valid live evidence

`adaptive_ul_dl_mrc2.NoxMgb`: 900 seconds, expected exit 124, no RFSTALL or
RXDISCONT, NIC missed counter unchanged at 27319338, zero stale/full UL drops.

| Receiver identity | DL operational TB CRC | UL leading-class TB CRC |
|---|---:|---:|
| 0x4858 | 102305/108000 = 94.7% | 826/1788 = 46.2% |
| 0x461e | 96747/226000 = 42.8% | 196/995 = 19.7% |

The UL numbers are unconfirmed leading hypotheses, not operational rates.
The aggregate UL 4001/88561 = 4.5% mixes competing interpretations and is NOT
a physical-link CRC figure. Do not reuse the earlier report's 2.9% DL and
0.32% best-UL as the current state.

## Proven defects and changes

### 1. Replay skipped the CFO sine-table initialization

The reader returns before `UE_thread()`, which initializes `InitSinLUT()`.
Before the fix, replay of XJn8TI reproduced 0/1 recorded live DL TB controls.
After explicit replay initialization, the same TB (RNTI 461e, source 16531)
reproduced byte-for-byte. The initial replay was VOID, not a decoder benchmark.

### 2. MRC2 was not applied to UL

`nr_ulsch_passive_keep_branch()` defaulted to branch 0 regardless of
`ISAC_RX_MRC_MODE=2`, zeroing the other channel estimates before combining.
Mode 2 now uses all RX branches in both combining and scale selection. Explicit
`ISAC_UL_RX_BRANCH` overrides and the old no-mode default are preserved.
The environment cache is thread-local. Active-gNB behavior is unchanged.

Matched recorded-IQ comparison refuted MRC wiring as the MAIN CRC cause:
43-bit candidate 8/16 (branch 0) versus 6/16 (all four); 45-bit candidate 3/16
either way. This wiring fix satisfies the requested mode, not a performance claim.

### 3. Passive timing refinement selected the greatest signed antenna delay

The estimator aggregated antennas by maximum `est_delay`, and passive UL used
that signed coordinate to move its FFT window. It was not a confidence score.
Passive UL now selects the strongest measured CIR peak among participating
branches, respecting a single-branch override. Active-gNB aggregation is unchanged.
On identical all-four IQ, the 45-bit candidate improved from 3/16 to 9/16.

### 4. Strong-channel timing IDFT overflow — analytically reproduced

The unitary fixed-point IDFT can amplify coherent channel estimates by sqrt(N).
Input values can fit in int16 while the intermediate/output timing transform
overflows. A real production-estimator test uses a known phase ramp, N=4096,
3276 active subcarriers, amplitudes 16/128/512/2048/8192, delays 0/-8/+13.

Before: at amplitude 8192, true delay 0 became +2047, true -8 became +2041,
and true +13 became -243. Three of fifteen cases failed. This reproduces the
live half-FFT jump without RF, propagation assumptions or DCI interpretation.

Passive DL/PUSCH now scale a temporary LS copy to reserve timing-IDFT headroom.
The original channel estimate/equalizer amplitude is untouched. Peak power is
restored to comparable input-domain units before combining evidence across
antennas. The same test now passes 15/15, with exact expected delays.
This is not a CFO/SFO feedback correction; those experimental apply flags remain off.

### 5. Actual rate-matched UCI removal rescues UL failures

The existing reservation-only retry changes `unav_res`, then demodulation
overwrites it, and never removes interleaved UCI positions from the LLR stream.
The new explicitly OFFLINE probe preserves full descrambled LLRs, removes actual
ACK or CSI positions, resets HARQ state for each LDPC attempt and verifies the
final TB CRC. It does not re-demodulate or re-descramble an already descrambled copy.

Five additional XJn8TI/4858 TBs were rescued: ACK footprints 17 and 22 RE;
CSI footprints 38 and 33 RE. These are inferred per-TB footprints, NOT known
ACK bit counts, CSI report configuration, beta offsets or proof of DCI widths.
They are not written into live configuration. All five are large segmented TBs.

## Matched immutable-IQ results

Recording: `captures/adaptive_ul_dl_mrc2.XJn8TI/replay.bin`.
Receiver-derived SIB1 common BWP/TDA entries and leading width candidates from
that run are explicit OFFLINE hypotheses. Every attempted grant is counted;
both hypotheses have 16 trials with zero rejected grants. Every decode is
repeated; all statuses and successful payload hashes match. Live DL TB control
remains byte-identical through every valid arm.

| Arm, all four RX | UL 4858 | UL 461e |
|---|---:|---:|
| Old timing selection | 6/16 | 3/16 |
| Peak-based timing selection | 6/16 | 9/16 |
| Plus timing-IDFT headroom | 6/16 | 15/16 |
| Plus offline ACK/CSI compaction probe | 11/16 | not rerun |

These small samples do not establish a sustained 60% live rate or convergence.
Combined ACK/CSI layouts, small-ACK puncture reconstruction and PTRS are not
covered by the single-component UCI probe. Its up-to-1024-coded-bit search
budget is diagnostic scope, not a standards limit or an affordable live policy.

## Test artifacts (sens6)

- `/tmp/adaptive-crc-replay-before.log`: VOID, uninitialized CFO table.
- `/tmp/adaptive-crc-replay-after.log`: live DL byte control PASS.
- `/tmp/adaptive-crc-ul*-branch*.log`: VOID, diagnostic config rejected.
- `/tmp/adaptive-crc-ul{43,45}-ant{0,-1}.log`: matched antenna controls.
- `/tmp/adaptive-crc-ul45-mrc2-fixed.log`: default MRC2 matches explicit all-four.
- `/tmp/adaptive-crc-ul{43,45}-timing-fixed.log`: peak-selection comparison.
- `/tmp/adaptive-delay-contract-{before,after}.log`: analytic timing regression.
- `/tmp/adaptive-crc-ul{43,45}-headroom.log`: corrected timing replay.
- `/tmp/adaptive-crc-ul43-uci.log`: 11/16 with offline UCI rescue.
- `/tmp/adaptive-crc-regressions.log`: 113 tests / 15 suites PASS.
- Nine focused CTest entries PASS; no full-repository-suite claim.
- Branch policy (10 assertions), timing selection (6), and exact ACK/CSI
  compaction/invalid-input/in-place controls PASS.

## Run safety

Sensing remains compiled/runtime OFF. Four RX, MRC2. No dedicated online
configuration supplied. Radio occupancy and host NIC configuration are checked,
not changed. The acquisition watchdog now resolves its own modem using the
unique copied config path before signalling its exact PID; no broad pkill.
NIC misses or RF discontinuities now make the run verdict VOID. Source manifests
include new untracked headers, not just a commit ID and tracked diff.

`StA57C` stopped before RF when the source manifest encountered a gitlink
directory; VOID/no metrics. The manifest now hashes files only. Live validation
of the timing fixes is the next checkpoint; online UCI learning remains open.

## Corrections to earlier reasoning

- Large/confident LLRs and smooth interpolated channels cannot establish correct
  geometry, correct noise calibration or absence of a channel problem. The old
  ULSIG metric is also not bounded by 1; it is not a calibrated coherence proof.
- Equal DCI lengths do NOT prove equal RRC configurations across UEs. The current
  pooled UL search assumption remains a correctness concern.
- A leading width vector is a candidate, not proof. New distinguishing payloads
  cannot be dismissed as though finite-sample equivalence were universal.
- CSI configuration cannot in general be inferred just from `csi_request`.
  Periodic reports and unknown dedicated RRC configuration remain relevant.
- Successful large multi-code-block decodes after the timing fix rule out a
  universal multi-block decoder failure. No claim that all rate-matching cases
  or LBRM configurations have been verified.

Next implementation work: bounded measurement-driven online UCI learning and
honest per-UE operational UL accounting, followed by held-out/live validation.
