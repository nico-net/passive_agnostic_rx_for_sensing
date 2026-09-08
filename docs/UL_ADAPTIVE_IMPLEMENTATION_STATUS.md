# UL adaptive pipeline: initial implementation (2026-09-08)

Worktree on sens6: /home/sens/NICOLA/adaptive-rx-UL-DL
Branch: adaptive-rx-UL-DL, based on e06edcb7db (total-passive-UL-DL-adaptive).
The original worktree's uncommitted DL/radio/capture changes are not included or modified.

Implements the architecture in:
- superpowers/specs/2026-09-08-passive-rx-ul-adaptive-discovery-design.md
- superpowers/plans/2026-09-08-passive-rx-ul-adaptive-discovery-plan.md

This is an initial, offline implementation, NOT completion of fully autonomous UL reception.
No OTA capture, transmitter, radio command, gNB log, or external synchronization was used.

## Switch

In the existing sensing configuration:

```conf
pdcch_blind_monitor_full_auto = 0;
```

0 (default): UL uses the existing manual DCI length override/formula, field widths, TDA,
DM-RS and waveform settings. No automatic UL grant interpretation is used.
1: enable experimental UL length -> width -> interpretation discovery, alongside the
existing DL Technique D switch. Unknown/oversized/ambiguous UL searches refuse grants.
It does not silently switch to manual UL interpretation when discovery fails.

The existing DL CORESET/DCI-length autodiscover flag remains a separate setting, as before.
To use fully manual DL geometry too, disable pdcch_blind_monitor_autodiscover.
UL DCI scanning must remain enabled with pdcch_blind_monitor_dci01. The automatic flag
does not bypass an explicit scan disable, malformed-config disable, or CSS0-only safeguard.
The underlying UL PUSCH decoder must be enabled in decode mode (not CFR-only) to supply
the TB-CRC oracle. This change does not modify running configuration files.

## Implemented

- Tasks 1-3: bounded shared engine with constraint filtering, sample-based equivalence
  classes, reject-only candidate gating, round-robin TB-CRC scoring and refusal on ties/caps.
  Members remain identifiable; empty/error states cannot expose a stale winner.
- Task 4: separate UL length evidence; raw polar CRC and UL identifier are checked before
  field extraction. A confirmed RNTI, multiple payloads and one supported length are required.
  DL sweep state is unchanged. UL state resets on identity/CORESET/PCI changes.
- Task 5: width/interp class IDs and a generation tag travel in the grant, including queued jobs.
- Tasks 6-9: all sixteen planned width dimensions, length-constrained generation, controller,
  real-extractor equivalence checks and offline tests. New payloads that distinguish merged
  classes invalidate the old scores. Failure to extract two hypotheses is not equivalence.
- Tasks 10-12 foundation: interpretation catalogue, apply at the observed TDA index,
  sequential controller, CMake registration and offline tests.
- Both inline and queued PUSCH decode sites return actual TB outcomes to a serialized controller.
  Stale/dropped/CFR-only/unsupported/setup-error outcomes are not negative CRC evidence.
  Feedback from a previous context is ignored using the generation tag.

The controller handles one bootstrapped UE at a time. It does not pool evidence across UEs.
Decoder signatures remain unchanged. DL Technique D has not been refactored.

## Plan corrections and remaining contracts

1. The proposed length scorer's placeholder UL opts had bwp_size=0, so the actual extractor
   rejected it before polar decoding. Even valid BWP placeholders still hit interpretation
   checks. Raw recovery and field extraction now have separate APIs, exercised with actual
   polar-encoded LLRs and deliberately invalid interpretation settings.
2. The plan's exact "47-bit" known-width fixture totals 40 bits. For BWP=273 and TDA count=2,
   independently counted admissible width vectors are 87 at length 40 and 4145 at length 43.
   Length 47 exceeds the 8192 raw cap. Tests assert these facts and the refusal behavior.
3. The plan initialized both sweeps without equivalence samples, which cannot fit the
   advertised class cap. The controller collects eight distinct CRC-verified payloads first.
   Caps remain 8192 raw / 64 classes; no silent truncation and no cap tuning.
4. Tied hypotheses do not "naturally collapse" under a win-ratio oracle. Actual extraction
   outputs are compared across every sample before scoring. Equal S/L or selected field widths
   alone are not an equivalence proof. New distinguishing payloads invalidate previous classes.
5. The supplied parity-based oracle tests assigned deterministic outcomes to alternating
   classes rather than the claimed identical rates. Tests now schedule outcomes per class.
6. Full interpretation catalogue: 960 raw hypotheses, with receiver-unsupported/illegal
   combinations still needing a complete standards-derived admission contract. The initial
   controller may refuse at the 64-class cap. It does not label that refusal convergence.
   Full-space PUSCH-oracle convergence has NOT been demonstrated.
7. Width scoring still requires known UL BWP/TDA field width and a working baseline
   interpretation. Total DCI length cannot uniquely infer TDA count or UL BWP. Default/unknown
   dedicated TDA count is explicitly unresolved. Joint width/interpretation initialization
   remains a design dependency; this is not a claim of no-manual-config UL completion.
8. The existing passive decoder rejects DFT-s-OFDM; antenna-port interpretation is limited.
   The automatic controller does not treat unsupported receiver modes as disproved network
   hypotheses. Missing modes may leave a search unresolved.
9. Multiple observed TDA indices need separate interpretation state; the initial controller
   refuses this case rather than overwrite index zero or mix its CRC evidence.
10. The workflow skills referenced by the old plan were unavailable in this environment.
    The implementation and review were performed directly, without delegated agents.

## Verification

Offline tests use synthetic LLRs and synthetic TB outcomes where explicitly stated. Synthetic
oracle convergence tests validate search decisions, not RF performance or real PUSCH decoding.
No range/Doppler sensing score or OTA metric is claimed.

Build and test commands (in an independent source snapshot when sens6 is capturing):
```sh
cmake -S . -B cmake_targets/ran_build/build -DENABLE_TESTS=ON \
  -DAUTO_DOWNLOAD_ASN1C=OFF -DASN1C_EXEC=/opt/asn1c/bin/asn1c -DOAI_SIMU=ON -DENABLE_ISAC_SENSING=ON
cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem tests -j8
ctest --test-dir cmake_targets/ran_build/build --output-on-failure
```

Never build on sens6 while nr-uesoftmodem captures are running.

## Verified results

- Full nr-uesoftmodem build passed with ENABLE_ISAC_SENSING=OFF and ON.
- Fixed an existing DL/SSB receiver-only link failure: nr_isac_ssb_axis.c must also be
  linked with nr_isac_stub.c because the PBCH receive code always references its helper.
- Eight focused CTest entries passed: shared engine, blind monitor, UL width,
  UL interpretation, DCI length, DL PDSCH sweep, CORESET map, RNTI bootstrap.
- The real UL extractor grouped 87 admissible 40-bit hypotheses into 41 classes across
  eight varied synthetic payloads. This is finite-sample evidence, not a proof that all
  future payloads remain equivalent. A new distinguishing payload invalidates old scores.
- Controller tests verified positive feedback attribution and rejection of feedback from
  a previous generation after reset.
- Full repository test build is BLOCKED by existing, unchanged isac_sync_test.cc and
  isac_sync_replay.cc calls to the old cpi_sto_tracker::process signature (7 versus 11
  arguments / shifted row metadata argument). The full suite is NOT reported green.
- Earlier filtered test probes against an unrecompiled binary matched zero tests: VOID;
  they provide no validation. The rebuilt equivalents each ran and passed.
- Verification ran in the isolated local source snapshot because sens6 had active captures.
  No OTA run was started, stopped, or scored by this change.

## DL follow-up

The subsequent DL-only fixes on this branch are documented in
[DL_ADAPTIVE_FIX_STATUS.md](DL_ADAPTIVE_FIX_STATUS.md). The initial UL implementation
above is unchanged; the earlier statement that Technique D was untouched describes
commit 770f19f6b6, not the current branch tip.
