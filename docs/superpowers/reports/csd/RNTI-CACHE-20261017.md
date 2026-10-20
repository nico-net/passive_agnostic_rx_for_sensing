# RNTI layout cache (csd/int16-rcache), 2026-10-17 — agent report (summary, orchestrator-saved)

## Summary
- Part 1 is delivered behind `ISAC_RNTI_LAYOUT_CACHE=1` (default off), commits 251c2b2156 and f9ac9ef783. Not merged.
- Part 2 (known-RNTI fast PDCCH) is a design only.
- The p99 target of 0.5 ms is not reached.

## Per-grant cost before the change
Every C-RNTI DCI 1_1 grant re-parses every live layout (about 1100-1200 distinct keys) under the Phase-2 lock. This includes already solved RNTIs.
DM-RS config, TDRA and scrambling id are not recomputed from scratch.

## Design
- The solver keeps a per-RNTI cache of the solved layout set: 4 contexts, up to 256 ids.
- Key: DCI length, CORESET nID, TDD slot class and the TDA flag.
- A generation counter invalidates entries when the solved set changes.
- The cache is bypassed (full pass) when:
  - the RNTI is new, unsolved or evicted;
  - there have been 8 consecutive CRC failures;
  - more than 5 readings overlap.
- Everything stays inside the Phase-2 lock; nothing was moved out.
- `ISAC_RNTI_LAYOUT_CACHE_CHECK=1` runs the full pass anyway and counts differences against the cache.

## Tests
- test_nr_pdsch_solver: 29 pass, including the new cache test (hit, slot-class separation, invalidation after CRC failures).
- monitor 273, prb_set 22, csirs 139, gpu_ldpc mismatches=0.

## Bit-exact verdict: not demonstrable
- Replay is real-time, so the decoded (rnti, slot, crc) set differs run to run even off vs off. [MEASURED, DGX replay, n=3/arm]
  - Jaccard, Vodafone: off-off 0.64-0.72, off-on 0.28-0.76.
  - Jaccard, TIM: off-off 0.86-0.90, off-on 0.86-0.96.
- CHECK mode (earlier binary, before the >5-readings fallback) [MEASURED, DGX replay, n=1/capture]: the cached pass equals the full pass except the order of refinement probes, in 6 of 3858 grants (Vodafone) and 0 of 5428 (TIM).
- The final binary was not re-run in CHECK mode.

## Latency, off vs on, t26gE [MEASURED, DGX replay, n=3 per arm, run means]
| capture | tot p50 (us) | tot p99 (us) | scan p99 (us) |
|---|---|---|---|
| Vodafone | 1191 -> 1073 (-10 %) | 3014 -> 2994 (inside noise: 2729-3221) | 1464 -> 1216 (-17 %) |
| TIM | 985 -> 904 (-8 %) | 2253 -> 2091 (-7 %) | 1019 -> 946 (-7 %) |

- Cache hits: Vodafone 3634 hit / 2469 CRC-failure bypass; TIM 5989 / 116.
- Decode-body stages are unchanged; the gain is in the scan and enqueue stages only.

## Risks
- p99 is dominated by heavy-grant decode bodies and queue wait (P99-BOTTLENECK-20261016). This lever saves at most about 0.1-0.25 ms.
- Bit-exactness is unproven; refinement probe order may diverge over long runs.
- `reopen_layouts` is ignored for solved RNTIs.

## Part 2 design (known-RNTI fast path)
Most pieces already exist: USS_HASH, KNOWN_PRESCREEN, USS_FAST. What is missing is a separate job:
1. Run a known-RNTI-only occasion (4-8 candidates per RNTI) first.
2. Then run the full blind occasion.
3. Dedupe on (rnti, slot, CCE).

This needs a scan-queue change in the 11k-line monitor file, and the Phase-2 lock is then taken twice per slot.

Replay dirs: rr-csd-int16rc/replays/replay_rc4_*.
