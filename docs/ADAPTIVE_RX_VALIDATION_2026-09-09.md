# Adaptive passive RX validation — 2026-09-09 (Europe/Zurich)

Branch/worktree: `adaptive-rx-UL-DL` on sens6. This report supersedes the earlier
offline-only status notes; those remain historical records. No merge into the original
worktree. Receiver-only OTA testing was explicitly authorized later in the conversation.

## Verdict

**Not an end-to-end working autonomous UL/DL system yet.** Two observed RNTIs have
independently reached DL Technique D convergence and produced CRC-verified payloads.
UL independently discovers 45- and 43-bit DCI lengths, but has no payload-CRC validation:
automatic BWP/TDA/field-width/interpretation initialization is still unresolved.
Do not count search-wide CRC percentages as post-convergence link performance.

## Fixed in this checkpoint

- Raw DL polar recovery is independent of assumed DCI interpretation. Dedicated-geometry
  verification uses distinct raw payload evidence. The supported initial DL front-layout
  candidates have separate per-RNTI/TDA Technique D contexts.
- UL length/controller contexts survive interleaved UEs; queued feedback remains generation-tagged.
- SIB1 publishes actual OTA common DL/UL BWP/TDA facts without overwriting dedicated hypotheses.
  Actual PCI, numerology and MIB DMRS Type-A position replace hardcoded UL cell inputs.
- Deferred PDCCH/PDSCH jobs retain their ORIGINAL RF timestamp and common CFO.
  Antenna worker dispatch explicitly carries/restores the CFO snapshot instead of losing TLS.
  Pre- and post-FEP lifetime checks exclude overwritten samples from CRC evidence.
- UL grant delivery retains multiple UEs per target slot, accepts late DCIs while IQ survives,
  handles mixed slots, and preserves the target slot's CFO. Full/expired jobs are counted,
  not negative CRC trials. Reacquisition clears pending grant/history state.
- RX timestamp continuity state resets across acquisition; discontinuous reads never dispatch
  decode jobs. Radio automatic reinitialization is disabled in validation runs.
- Polar cache shutdown refuses to free active decoder nodes, including nodes initializing
  outside its mutex. An actual ownership/cleanup/reinitialization regression covers this.
- Sensing compiled OFF (`ENABLE_ISAC_SENSING=OFF`), runtime enable=0.
  DM-RS CFO apply, SFO correction and branch-CFO apply remain OFF.

## Radio evidence and validity

All captures: `/home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.*`.
Only the adaptive passive receiver was launched, with four RX channels and MRC mode 2.
No gNB logs, scheduler hints, active UE actions, transmitter, radio reset or NIC configuration writes.

- `49VmrW`: **VOID**, missing config plugin, failed before RF.
- `nCHKqU`: continuous stream/SIB1, but DL parser mismatch; no working payload pipeline.
- `xNABbQ`, `mFYfn0`, `5MbqEE`: **VOID for performance**, RF discontinuities/reacquisition defects.
- `iog4ec`: 180 s continuous RF, no added NIC missed packets; decoding metrics **VOID**
  because delayed-job provenance/lifetime was not yet fixed.
- `Orknvj`: 180 s continuous RF after provenance fixes; no added NIC missed packets,
  sparse exploratory CRC successes, no complete adaptive convergence.
- `FMwicC`: 180 s continuous RF, no added NIC missed packets; DL convergence for RNTI
  0x486f, S=1/L=13, DMRS mask 0x884, MCS table 1. These values were learned, not supplied.
- `UfIPMK`: longer validation; both 0x486f and 0x4b83 converged independently.
  Final results are appended below after the bounded run completes.

Moving receiver workers away from NIC IRQ/softirq cores (8–13) enabled continuous runs.
Receiver CPUs are 0–7; this is host scheduling, not radio configuration or network side information.
The former sustained-CFO-error hypothesis was retracted: its cited -1.3 kHz observation was
an early acquisition sample, not evidence of a persistent residual.

## Verification

- Full passive modem build with sensing OFF.
- 102 focused GTests / 15 suites pass on sens6 and in an independent local source snapshot.
- Five offline real-FEP/FFT/equalizer contracts pass. Cross-thread CFO snapshot:
  corrected residual -1.394 Hz; lost-snapshot counterexample +813.170 Hz.
  Reproduction: `bash tests/passive_rx/offline_sync_contract/build_and_run.sh`.
  These tests do not claim live tracker or RF performance.
- Focused CTest aggregate and final bounded receiver run: results appended below.
- The unrelated full repository ISAC sync-test signature mismatch is not fixed; no full-suite claim.

## Remaining UL design dependency

The documented sequence scores field widths by PUSCH CRC BEFORE learning the TDA/DMRS
interpretation needed to decode that PUSCH. SIB1 common configuration is not proof of a
UE's dedicated configuration. Substituting common values as established dedicated facts is invalid.

Even with a candidate BWP of 273 PRBs, length alone leaves these width-vector counts
for TDA index widths 0/1/2/3/4:
- DCI length 43: 10345 / 4145 / 1420 / 400 / 87.
- DCI length 45: 44428 / 22666 / 10345 / 4145 / 1420.
These are independently counted from the documented axes, not recovered configuration.
The existing raw/class caps must not be raised or bypassed to manufacture convergence.
Joint initialization and per-TDA interpretation state still need implementation and real
PUSCH-oracle validation. Unsupported waveform/port-table and UCI interpretation remain limitations.

## Reproduction

`tests/passive_rx/adaptive_no_hints.conf` contains no dedicated BWP/TDA/DMRS/MCS/UCI hints.
`bash tests/passive_rx/run_adaptive_receive_test.sh` is a guarded 480 s sens6/X410 passive-only
test. It refuses occupied hardware, captures source/config/binary identities and NIC counters,
and terminates only its own receiver. It is not a production-ready-system declaration.
Band/carrier/PRB count/numerology remain acquisition CLI inputs, as excluded from the original
dedicated-configuration discovery plan. Initial CFO/drift seeds came from passive reception.
Automatic mode is `pdcch_blind_monitor_full_auto=1`; 0 preserves manual interpretation.

## Completed long-run checkpoint

`UfIPMK`: 600 s, expected timeout exit 124, zero RXDISCONT/RFSTALL, NIC missed counter
27319338 before and after. Last periodic census: 604758 exploratory+operational TB attempts,
44114 CRC successes, 271999 queue-full drops, 261556 stale drops. This is NOT a 7.3% steady-state
link rate: it mixes intentionally wrong search hypotheses with settled decoding. Both observed
RNTIs converged; autonomous UL remained at zero attempts. Final per-RNTI operational validation
uses the next run and a separate counter.
