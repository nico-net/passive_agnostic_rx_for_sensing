# DL adaptive fixes — branch adaptive-rx-UL-DL

Scope: DL defects only, on top of 770f19f6b6. No merge into the original branch,
no radio runs, no OTA validation, and no gNB-derived runtime information.

## Fixed contracts

- Ported the retry/subset descrambling and mask-aware nvar/shift fixes from
  original-branch commit 362ce76042. Unrelated radio changes and capture scripts
  from that commit are not included.
- Subset diagnostics also preserve the CRC-verified TB bytes: saving only the
  success flag while diagnostic LDPC calls overwrite the shared buffer was unsafe.
- Technique D enumerates its complete 192-entry raw catalog rather than silently
  truncating at 64. The real DMRS table filters undefined combinations before
  selection; identical effective symbol/mask/MCS hypotheses are merged.
- State is isolated by configuration, RNTI and observed TDA index. All shared
  accesses are mutex-protected. Value-only tickets carry a generation; old
  feedback is discarded after reset or context eviction. Up to 256 contexts
  are retained; an evicted context must relearn, never inherit another UE's score.
- Both inline and deferred CRC outcomes feed the same keyed controller.
  Unsupported decodes, internal errors and dropped jobs are not CRC trials.
  RV/cap-ineligible grants do not advance the hypothesis rotation.
- One application helper updates PDU allocation/MCS, the decoder's grant MCS,
  and rate-matching MCS together. Hypothesis-dependent SNR gating cannot censor
  exploration before its CRC oracle. Spec/common format-1_0 semantics are not swept.
- Unverified geometry does not emit PDSCH/CFR; unresolved Technique D does not
  silently decode using manual interpretation. Exploratory DMRS rows are not
  published into sensing; CRC-verified data-aided rows remain usable.
- CORESET evidence is epoch-local: two distinct dedicated-DL grant fingerprints
  in different slots for the same RNTI. A historical/bootstrap RNTI alone cannot
  verify a geometry.
- Geometry candidates advance even without DCI-length convergence. All contiguous
  intervals containing the observed occupancy are eligible, within the carrier's
  45-window bound; the previous eight-entry cap is removed.
- Failed length budgets do not blacklist offsets or other widths at that offset.
  Exhaustion is explicitly inconclusive and restarts occupancy discovery, never a
  verified/manual fallback. Transitions clear length/PDSCH evidence and return
  before old LLRs can be interpreted using new geometry.
- New-UE sightings are recorded before confirmed-set membership filtering, so one
  already-confirmed UE cannot prevent every later UE from acquiring a context.

## Manual and automatic operation

`pdcch_blind_monitor_full_auto = 0` disables Technique D and preserves manual
PDSCH interpretation. The legacy `ISAC_PDSCH_CFG_SWEEP` environment override no
longer bypasses this flag. `full_auto = 1` enables experimental dedicated-DL
interpretation learning once upstream geometry/length are ready; its existing UL
meaning remains unchanged. Set `pdcch_blind_monitor_autodiscover = 0` as well
when manually configuring DL geometry.

## Offline verification

- Full `nr-uesoftmodem` builds with `ENABLE_ISAC_SENSING=ON` and `OFF`,
  using the local `OAI_SIMU=ON` build. Neither modem was launched.
- Nine relevant CTest entries: DL adaptive, PDSCH sweep, blind monitor, DCI length,
  CORESET map, RNTI bootstrap, shared hypothesis engine, UL fields and UL interpretation.
- Thirteen sweep tests under AddressSanitizer/UBSan and under ThreadSanitizer.
  The concurrent-consumer regression checks exactly 60,000 trials / 30,000 successes,
  not a timing-dependent approximate count.
- The initial PIE ThreadSanitizer attempt is VOID: runtime startup failed with
  "unexpected memory mapping", before tests. Rebuilding non-PIE allowed all thirteen
  tests to run successfully without reported races.
- The new CORESET regressions link the real DMRS generator, not the old monitor
  fixture's stub. They prove historical RNTIs do not verify a new geometry,
  repeated same-slot/fixed-payload observations do not verify it, old geometry
  evidence is discarded, same-offset widths remain searchable, and exhaustion
  does not invent verification.
- These checks cover software contracts, not real-air branch-rescue rates or
  end-to-end LDPC performance. No capture metrics are inferred. The previously
  documented unrelated full-suite ISAC sync signature blocker is not addressed.

## Remaining scope limits

This is not a claim of complete DL autonomy. CORESET discovery still assumes
a contiguous, PCI-scrambled, one-symbol/non-interleaved candidate at symbol zero
and a full-carrier BWP. Technique D covers the eight mapping-A shapes in its
catalog; DCI field-width recovery and configurations outside that scope remain
incomplete. Equal or insufficient CRC evidence stays undecided. Configuration
changes not reflected in observable/context inputs are not fully recovered yet.
