# Agnostic passive receiver: incremental architecture and source audit

Source: sens6 `/home/sens/NICOLA/adaptive-rx-UL-DL`, branch `adaptive-rx-UL-DL`,
base commit `3ca9a174e9`, with pre-existing uncommitted receiver changes.
This is an incomplete implementation, not a claim that all acceptance gates pass.
No X410 access is permitted. Tests must not load UHD or launch a live modem.

## Audited native paths

| Stage | Existing behavior | Remaining limitation |
| --- | --- | --- |
| PDCCH | Polar decode, recovered RNTI, re-encode mismatch evidence | RNTI plausibility alone does not establish scheduling truth |
| DCI 1_0 | C/TC, SI, RA and paging field parsers; context-dependent RIV/TDRA | Legacy first-passing RNTI class can misinterpret an ambiguous word |
| DCI 1_1 | Raw decoding is separate from `nr_pdcch_blind_extract_11` | Payload length does not determine field widths |
| DL automatic layout | BWP indicator widths 0..2, TDRA widths 0..4; exact size constraint | Only a bounded family, at most three matching layouts |
| DL waveform search | Per-configuration/RNTI/TDA hypotheses; DMRS-table legality; TB CRC feedback | Limited S/L catalog and CRC-led selection do not prove a general configuration |
| UL | 0_0 recovery and raw 0_1 with interpretation sweeps | General end-to-end UL correctness is not established by this change |
| CSI-RS | `nr_csirs_monitor` consumes configured resource PDUs | Enabling a source does not discover its resource mapping |

The initial automatic 1_1 family assumes Type-1 allocation, one codeword,
Type-1/single-symbol antenna-port table, HARQ width 4, DAI 2, feedback timing 3,
antenna ports 4 and SRS request 2. Carrier indicator and optional rate matching,
ZP-CSI-RS, TB2, TCI and CBG fields are not generally searched. BWP geometry is a
configuration hypothesis; a recovered polar CRC cannot establish its truth.
Manual mode supplies TDRA, DMRS and DCI-width configuration. It remains an oracle,
not a permissible source of runtime truth for automatic inference.

DCI 1_0's existing context chooses the frequency reference, PRB origin, search
space and default/common/dedicated TDRA interpretation. Missing common TDRA can
fall back to a supplied dedicated list or default table. These legacy fallbacks
remain to be replaced with provenance-aware alternatives in general auto mode.
C and TC share an existing parser hypothesis; the search-space label is not proof
of RNTI identity. More RNTI types, interleaved VRBs and dedicated cases need audit.

## Implemented first increment: class ambiguity barrier

`nr_pdcch_blind_decode_10_mode` performs one native polar decode and one re-encode
mismatch measurement. Manual mode keeps the original first-match behavior.
Automatic mode runs every enabled class against the same decoded word and stores
all accepted/rejected candidates in `nr_dci10_interpretation_report_t`.

- No survivor: `REJECTED`, with individual parser rejection reasons.
- Multiple survivors: `AMBIGUOUS`; no allocation is exported to the caller.
- One survivor: `UNRESOLVED`, a unique protocol-plausible *decode attempt*.
- `VALIDATED` is reserved for independent physical and temporal evidence. This
  increment never assigns it, even when the PDCCH codeword is noiseless.

The report is exhaustive over enabled classes for one supplied context, NOT over
all possible cell configurations. No confidence probability or made-up weighted
score is emitted. A NULL report never disables automatic ambiguity protection.
RNTI, raw payload and mismatch count survive ambiguity/rejection, so downstream
format-0_0 recovery retains its input. The runtime worker invokes the strict path
only when `dl_full_auto` is enabled. PHY debug logging emits `DCI_INTERPRET` and
`DCI_HYPOTHESIS`, including scope, class, allocation and rejection reason.

This barrier does not yet replace 1_1 CRC-based convergence, combine BWP/TDRA
contexts, validate unique grants physically, or withhold every existing sensing
publication until a generalized interpretation is validated.

## Next architecture increments (not implemented here)

Generate explicit DCI field-width/configuration hypotheses from broadcast facts,
CORESET/search-space evidence and bounded standards domains. Represent unsupported
and truncated search domains explicitly; a surviving supported candidate must not
exclude an unsearched domain. Score protocol checks, measured DMRS/RE mapping,
TBS/G/LDPC/rate-matching contracts, CRC and distinct temporal/HARQ observations.
Never reinterpret missing measurements as passing checks. Never count repeated
processing of the same observation as independent evidence.

Runtime knowledge must be keyed by cell identity, configuration generation,
RNTI/BWP and observation identity. Keep broadcast facts separate from dedicated
hypotheses. Attach generation tickets to asynchronous decoder results; invalidate
stale results after configuration changes, sample discontinuities or reacquisition.
Learned winners need continuing validation and bounded failure hysteresis.

The acquisition controller still needs explicit SEARCH_SSB, PBCH_LOCK,
CELL_CONFIG, SEARCH_PDCCH, TRACK_PDCCH and TRACK_SCHEDULING states. Entry requires
repeated consistent observations, not one CRC. Recovery should progress from
predicted timing/CFO through local widening to fresh SSB/PBCH, rebuilding only
invalidated configuration. Expose confidence, success denominators, last validated
scheduling time, unresolved counts and recovery reason in native offline replay.
Event-only state-machine tests cannot satisfy raw-IQ acquisition gates.

CSI-RS periodicity and RE structure may be searched from repeated observations;
resource identity, density/row, scrambling identity, ports, QCL and dedicated
configuration are not all uniquely disclosed in MIB/SIB1. Current configured
resources are not passive inference. Record ambiguity/unsupported cases instead
of inventing a default resource or consulting gNB configuration/logs.

## Replay evidence boundary

Existing `captures/*/replay.bin` files use `nr_passive_replay_capture.c`: bounded
16-frame IQ plus same-build ABI frame parameters, saved slot CFO, DCI examples,
and PDSCH jobs with decoded-TB hashes. They are useful for oracle/regression work,
but the existing reader's injected configuration is not raw cold acquisition.
A new safe adapter must validate ABI/header bounds and separate receiver metadata
from forbidden acquisition hints. Preserve measurement geometry such as sample
rate/channel count, and exclude saved PCI/BWP/CFO/allocations from cold-start mode.

## Standards reference

Field-list audit is anchored to TS 38.212 V17.9.0, section 7.3.1.2.1:
https://www.etsi.org/deliver/etsi_ts/138200_138299/138212/17.09.00_60/ts_138212v170900p.pdf
Resource-allocation and TDRA work must also use TS 38.214 and TS 38.213, not
assumptions reverse-engineered from one gNB configuration.

## Offline command

```sh
bash tests/passive_rx/agnostic/run_dci_regression.sh
```

The runner builds only the CPU-only native DCI test target, executes real polar
encoder/decoder vectors, and retains logs and XML. It does not open a device.
Full regression failure is reported even if the focused tests pass.

## Deferred OTA checklist

Only after hardware permission and all offline gates pass: unknown-cell cold
start, automatic DCI/PDSCH interpretation, sustained tracking, forced loss and
in-process recovery, and configuration changes. Record the same health metrics
and ambiguity diagnostics as offline. Do not use gNB hints or external sync.

## Increment: local DL configuration relearning (2026-09-11)

The automatic keyed PDSCH controller no longer keeps a winner forever after
operational CRC evidence collapses. It freezes the conservative CRC lower bound
at convergence, counts only settled-winner outcomes, and compares a consecutive
failure run with that measured reference. A configurable minimum run length and
summable probability budgets provide hysteresis. CRC failures are a health signal,
not proof of a BWP/configuration change; correlated RF fading can also trigger it.

Recovery preserves the checked hypothesis catalog, clears old scores, increments
context generation, and returns that context to unresolved local search. Old
queued tickets cannot score the new generation. Other RNTI/TDA/configuration
contexts and the manual parser are unchanged. A successful settled outcome clears
the failure streak; queued exploration outcomes cannot manufacture operational
loss. Runtime emits PDSCH_RELEARN with reason, generation transition, streak,
reference bound and reacquisition count. The policy setter is currently a C API,
not a new configuration-file or command-line option.

This is controller-level recovery only: raw-IQ interruption recovery, full
acquisition-state-machine integration, duplicate observation identity, and a
non-CRC-only interpretation validator remain open. The interpretation catalog
still has its previously documented scope limitations. Validation evidence is
recorded in RECOVERY_VALIDATION.md; do not infer full Gate 5 from controller tests.
