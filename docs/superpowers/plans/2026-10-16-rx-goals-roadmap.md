# Passive RX roadmap: DL + UL at 0.5 ms, fewer cores, then single-process multi-PCI (2026-10-16)

Agreed with the user on 2026-10-16. This roadmap fixes the goals, their exact pass criteria, the order and the gates. Each phase gets its own implementation plan; only Phase 0 is detailed now (`2026-10-16-phase0-gpu-inventory-and-metrics.md`), because the later plans depend on its output.

## Goals, in order

| # | Goal | Pass criterion (per cell, daytime load, n >= 3 interleaved runs, every cell on site) |
|---|---|---|
| G1 | DL decoding | (a) p99 of `tot` (slot fully read -> decode done) <= 500 us over ALL real grants (probe=0, every size incl. 273 PRB / 4 layers); (b) SNR-qualified CRC-OK >= 80 %; (c) queue drops (full + stale + narrow) < 1 % of queued |
| G2 | UL decoding | same three criteria on real UL grants of ID-locked UEs (the first decode of a new UE waits for the ID search and is reported separately) |
| G3 | Fewer cores | move to the GPU everything that can go there without accuracy loss (CPU reference check per port) and without GPU stalls; report "cores of CPU work per cell" and "dedicated cores"; G1 and G2 still pass |
| G4 | Multi-PCI | one process for all cells, one CUDA context, shared worker pools; G1-G3 criteria hold per cell with >= 2 cells; then 400 MHz |

- "SNR-qualified": a grant counts toward the CRC criterion only if its measured per-layer SNR >= required SNR of its MCS + 2 dB (definition and code: Phase 0 Task 1). Other grants are reported, not hidden.
- G4 starts ONLY when G1, G2 and G3 pass. Exception: every new GPU interface written for G1-G3 is multi-cell ready from day one (takes a cell index, batches several cells, runs inside one CUDA context with priority streams), so G4 does not rewrite it.
- GPU targets: exploit the DGX GB10 unified memory (zero-copy where it is faster) AND keep sens6 (RTX 4070, discrete, sm_89) working: every GPU path has a discrete-memory variant chosen at runtime by `nr_gpu_model()`; sens6 is validated at each phase gate (sens6 Task-9 style: build, tests, replay).

## Phases

| Phase | Content | Where | Plan |
|---|---|---|---|
| 0 | Metrics + scorecard; full CPU/GPU inventory; on-air GPU timeline and the 0.1-1.9 s GPU stall root cause; DL CRC root cause; target pipeline + per-stage budget + GPU API design | DGX (traces, OTA by orchestrator) + agents (code reading) | `2026-10-16-phase0-gpu-inventory-and-metrics.md` |
| 1 (G1) | Port order from Phase 0: PDCCH fully on GPU (B3) + short Phase-2 lock (B5); DL CRC fixes; then large-grant lane (A2), GPU/parallel LDPC prep (A3), GPU stall fix (A4), housekeeping (A5) | DGX | written after Phase 0 |
| UL track (G2) | C1 UL BWP look-alike lock fix; C4 probe/real split in UL logs; C2 UL IQ retention + background 1024 data-ID search; C3 HARQ soft combining. Runs IN PARALLEL with Phase 1 on cloud sessions (x86, rfsim), merged and validated on air on the DGX | cloud + DGX | written next (needs csd/int16 pushed) |
| 2 (G3) | Remaining CPU work to GPU (joint-anon PDCCH search, DM-RS/CSI-RS searches, UL decode, helpers); helper threads pooled; second DPDK link off | DGX | after Phase 1 |
| 3 (G4) | Single-process multi-cell refactor, shared pools, in-process channelizer, one CUDA context; 2 cells at 200 MHz, then 400 MHz (2nd QSFP) | cloud (CPU refactor) + DGX | only after G1-G3 pass |

## Rules for every phase
- CLAUDE.md hard rules; OTA only by the orchestrator, daytime, one at a time, through heavy.sh; no per-cell tuning; never pool cells/hosts/bandwidths; evidence labels on every number.
- Every GPU port: CPU path kept as reference and fallback; CPU-vs-GPU check (bit-exact or a stated tolerance) on recorded slots; replay A/B; then OTA A/B interleaved with the current default.
- Positive validation of a goal -> Haiku updates PROJECT_MEMORY.md; merge into csd/otatest only then.

## Baseline at plan time [MEASURED, DGX OTA 2026-10-16, n=3]
DL all-on t26aXE: tot p99 V 11.5 ms / T 8.1 ms; real-grant CRC-OK V ~41 % / T 36-49 % (not SNR-qualified); drops V 1-2.7 % / T 0.2-0.4 %. UL: CRC ~0; UL real full drops ~9 % (PUSCHQ2 real full=5389 / queued=60423, xVaXE2). PDCCH slot-read -> enqueue p50 ~0.6 ms. Receiver CPU work ~7 cores (radio ~3.3, cell ~3.7) over ~17 pinned cores.
