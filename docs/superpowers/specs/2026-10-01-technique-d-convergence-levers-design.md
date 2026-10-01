# Technique D convergence levers — design

Date: 2026-10-01. Status: approved by the operator 2026-10-01 (written-spec review round 1 applied: shared-IQ correlation, pre-outcome gating, precise compute cap, NSA-like naming, starting values, live-bed dependency).
Scope: the per-RNTI PDSCH configuration search ("Technique D", `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.{c,h}`,
`nr_crc_evidence.h`, `nr_pdsch_passive_queue.c`, call sites in `nr_pdcch_blind_monitor_rt.c`). Context and current
behaviour: `PROJECT_MEMORY.md` §23.9 (corrected 2026-10-01), §11.8, §12 G8, §24 K4/K7/K20.

## 1. Goal and success criteria

Make Technique D converge fast on **any** cell — SA or NSA — without ever converging to a wrong configuration.
The design is cell-type agnostic: SIB1 is one optional input; NSA (no SIB1, RNTI churn, higher rank) is the hardest
case and the one the targets are set against.

| Metric | Target | Condition |
|---|---|---|
| Cold start (first two RNTIs on a never-seen cell): first C-RNTI accept → `Technique D CONVERGED` | ≤ 30 s median, ≤ 60 s worst of 5 | ≥ 100 grants/s per UE, 4 RX, mixed rank 1/2, NSA-like (no SIB1) |
| Steady state (every later RNTI, once cell-level fields exist) | ≤ 2 s median | same |
| 1 RX | 0 wrong winners; convergence whenever the UE carries rank-1 traffic; times ≤ 3× the 4-RX targets (≤ 90 s / ≤ 6 s); a UE with no decodable traffic at this RX config reports `UNDECIDABLE`, never hangs silently | same |
| Safety (hard rule) | **0 wrong winners** in every measured campaign; a wrong cell-level field is withdrawn and relearned, never sticks. **A speed-up is accepted only if the wrong-winner rate does not increase.** | always |
| Cost | At equal input traffic and without increased sample loss, the convergence machinery may consume at most the equivalent of **4 additional CPU cores** over the current baseline. Measured as **average and peak** CPU (per-thread, 1 s windows) **and** queue backlog (scanq / PDSCH-queue `max_lag`, drops) — a result where the RT producer falls behind fails this row even if average CPU is within budget | DGX, 273 PRB |

Calibration (simulation of the current logic, PROJECT_MEMORY §23.9): 233 hypotheses at p_true 0.4 ≈ 37 k grants
(≈ 6 min at 100 grants/s) → the cold-start target needs ≈ 12×. The dominant cost is accumulating failures on every
**wrong** hypothesis (≈ 55–100 each) until its upper bound drops below the leader's lower bound.

## 2. Principles

1. **Hard constraints remove hypotheses. Soft information only reorders them. KL evidence decides the winner.**
2. The KL anytime acceptance rule (`nr_crc_interval()`, 1e-6 budget, separation test, 300-trial fallback) is **not
   changed**. No Bayesian rewrite. The decision unit remains the **joint** hypothesis; nothing is decided per field.
3. Every lever is independently switchable; with all levers off (K = 1, no ordering score, no field book, no gate)
   behaviour is **bit-identical** to today (same hypothesis sequence for the same RNG seed, same winner).
4. Side information (SIB1, 38.214 defaults, observables, promoted cell fields) is called **ordering score / search
   priority**, never "prior": it affects exploration efficiency, not the acceptance criterion.

## 3. Architecture

```text
DCI accepted (RT thread)
  │
  ├─ 1a grant trial gate ── ELIGIBLE → continue
  │                         GATED_PHYSICAL / GATED_CHANNEL_QUALITY → if context SETTLED: normal decode;
  │                                                                   else: count gated grant, no trial
  ▼
select_k(ctx, grant, K)
  │  candidates = live joint catalogue minus 1b deterministic per-grant exclusions
  │  order: 1) exploit current leader (existing 3-of-4 rule until 64 trials)
  │         2) ordering_score(h) (descending)   3) shuffled fair round robin (existing)
  │  → ticket{ generation, main hyp, probe hyps[≤K-1] }
  ▼
passivePdsch consumer
  │  main  : full TB decode (existing path, unchanged)
  │  probes: CB0 decode under each probe hypothesis on the same IQ
  ▼
feedback_k(ticket, outcomes[])        outcome = {kind: FULL_TB | CB_PROBE, result: PASS | FAIL | INCONCLUSIVE}
  │  FULL_TB → KL statistics (unchanged)
  │  CB_PROBE → P1: probe statistics only (ordering + logs)
  │             P2 (gated): FAIL that satisfies §5.3 → one KL failure for that hypothesis; PASS → no KL success
  ▼
KL anytime test (unchanged) → CONVERGED → field_evidence(winner) → CellFieldBook → future ordering_score()
```

## 4. Components

### 4.1 Grant trial gate (lever 1a) — pure function
`nr_td_gate_t nr_td_grant_gate(const nr_td_grant_view_t *g, const nr_td_rx_view_t *rx)` →
`ELIGIBLE | GATED_PHYSICAL | GATED_CHANNEL_QUALITY`.
- GATED_PHYSICAL (deterministic): number of layers signalled/implied by the grant > receive antennas in use
  (once the DCI layout is pinned; before that the gate never fires on rank).
- GATED_CHANNEL_QUALITY (heuristic, conservative): the grant's code rate (MCS index under the *most permissive* MCS
  table still alive) exceeds what the measured post-equaliser SNR of this RNTI supports by a margin
  (`ISAC_TD_GATE_SNR_MARGIN_DB`, default 6 dB). Disabled until ≥ 20 SNR samples exist for the RNTI.
- A settled context always decodes normally; the gate only decides whether a grant becomes a **trial**.
- **Pre-outcome rule:** the gate may use only information available before any decode of that grant is attempted
  (DCI fields, PRBs, MCS, RX count, SNR/channel estimates from *earlier* grants or from this grant's DM-RS). It must
  never use the outcome — or any by-product — of decoding this grant under any hypothesis; otherwise the trial
  population is selected on the outcome and the KL stream is biased.
- Counters: `gated_physical`, `gated_channel` per context; exported in metrics (Track-A A2 schema) and logs.
- 1-RX acceptance: a context that sees > N (default 2000) consecutive gated grants and no eligible one reports
  `UNDECIDABLE rnti=… reason=rank>rx` (state, not an error) and keeps waiting cheaply.

### 4.2 Per-grant deterministic exclusions (lever 1b) — inside `select_k`
Only mathematically guaranteed impossibilities; **reuse** the existing legality/rate-matching checks (legality
functions in `nr_pdsch_config_sweep.c`, TBS/G/LBRM geometry checks in `nr_pdsch_passive_decode.c`) — no duplicate
implementation. Examples: TBS does not fit G; impossible LBRM/rate-matching geometry; TDRA (S, L) outside the slot or
overlapping the observed PDCCH symbols illegally; DM-RS positions incompatible with L. Never SNR, operator habit,
defaults or any probabilistic argument.

### 4.3 `select_k` / `feedback_k` — extension of the existing API
- `int nr_pdsch_config_sweep_select_k(ctx, grant_view, K, ticket_k *out)`; `K` from `ISAC_TD_K` (default 1).
- K = 1 must reproduce today's `select()` exactly (same RNG consumption, same order). The main hypothesis is chosen by
  the existing rule; probes are the next K−1 distinct hypotheses by (ordering_score, round-robin position), skipping
  any already proven at its trial floor.
- `void nr_pdsch_config_sweep_feedback_k(const ticket_k *t, const outcome_t *outcomes, int n)`; the generation
  check of today's ticket is kept (stale tickets ignored).
- Per hypothesis the state gains `probe_trials`, `probe_pass`, `probe_fail`, `probe_inconclusive` beside the existing
  `trials`/`ok` (which remain the only inputs of the KL test in P1).

### 4.4 Probe decoder
- Decodes **code block 0 only** under the probe hypothesis (existing `nr_pdsch_passive_probe_mode()` /
  `layout_probe` path), with that hypothesis's own segmentation, rate matching and scrambling.
- Iteration policy: **the same maximum iterations and early-termination rule as the full decode** (otherwise a FAIL
  would not imply a full-decode FAIL). A probe stopped by the CPU budget or by sample-lifetime expiry before
  completing is `INCONCLUSIVE`.
- Budget: probes run only on the decode consumers, after the main decode, while the slot's IQ is still valid
  (`check_sample_lifetime`); a per-slot probe cap (`ISAC_TD_PROBE_BUDGET_US`, default 400 µs per grant) bounds cost.
- GPU batching of probes is out of scope here (it belongs to the A9 GPU work / multi-cell); the interface must allow it
  (probe requests are independent units).

### 4.5 `ordering_score()` (levers 3 and 5) — pure function
`float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si)`; higher = try earlier.
Additive terms, each 0 when its input is absent:
- **SIB1** (when decoded): hypothesis (S, L, mapping, k0) present in SIB1's common PDSCH TDRA list → +w_sib1.
- **38.214 default table A** for the cell's dmrs-TypeA-Position (from MIB) → +w_default (used with or without SIB1).
- **Decode-free observables** per field, each scored by agreement with the hypothesis: measured DM-RS symbol mask
  (existing DM-RS mask oracle), measured modulation order (existing Qm oracle), allocation start/end symbols from
  per-symbol energy (new), DM-RS type from the CDM comb pattern (new, optional), PT-RS presence by pilot correlation
  (new, optional).
- **CellFieldBook**: each promoted field that the hypothesis matches → +w_field × field confidence.
Constraints: the score never removes a hypothesis; a hypothesis with score 0 is still reached by the round robin.
Weights are configuration (`ISAC_TD_W_*`), defaults chosen by the simulator ablation.

### 4.6 CellFieldBook (lever 4) — replaces the all-or-nothing cell-wide prior
Per field (TDRA entry (S, L, mapping, k0), DM-RS additional position, DM-RS max length, DM-RS type, PRG, PT-RS
presence, LBRM; **MCS table is never promoted** — it is UE-capability specific):
`{field, value, confidence, supporting_rntis[], contradictions, last_confirmed_slot, config_epoch}`.
- Promotion: default rule = today's rule applied **per field**: two distinct RNTIs converged with the same value.
- Contradiction is counted in **independent RNTIs**, not raw events: an RNTI contradicts a field when it converges
  with a different value, or when ≥ M (default 64) of its consecutive full-TB trials on hypotheses carrying the
  promoted value fail while a hypothesis with another value passes for the same RNTI. The field records the set of
  contradicting RNTIs; when it reaches the per-field threshold (default **2 distinct RNTIs**) the field is
  **withdrawn** (promoted → unknown), logged, and its ordering bonus removed from live contexts.
- `config_epoch`: incremented on any detected cell-configuration change (new SIB1 content, PCI/carrier change,
  BWP change from the passive BWP tracker, stream gap → LOST); fields from an older epoch lose their ordering bonus
  and must be re-confirmed by one RNTI before regaining it.
- Seeding of new contexts uses CellFieldBook only through `ordering_score` (no pruning). The existing per-RNTI
  observations/prior seeding stays as it is.

## 5. Phases

### 5.1 P1 — deliverable
Levers 1a, 1b, 3/5 (ordering score), 4 (CellFieldBook), and top-K probes **for ordering and logging only**.
KL statistics fed exclusively by full-TB outcomes. All defaults keep today's behaviour (K = 1, weights 0, field book
off, gate off) until the ablation picks defaults.

### 5.2 P2 — gated extension (failure-only probe evidence)
A `CB_PROBE` outcome contributes **one KL failure** to its hypothesis iff all §5.3 conditions hold. A probe PASS never
contributes a KL success (CB0 passing does not imply the TB passes); optionally it promotes that hypothesis to the
next main decode. Statement for documentation and papers: *"The KL acceptance rule is unchanged. P2 admits an
additional negative observation only when the probe outcome deterministically implies failure of the corresponding
full-TB decode."*

### 5.3 Conditions for admitting a probe FAIL as KL evidence (P2)
1. Same computation as that hypothesis's full decode on CB0: same segmentation, rate matching, LLR scaling, scrambling,
   maximum iterations and early-termination rule (deterministic decoder).
2. No HARQ soft combining would apply: new transmission (NDI toggled / rv 0 and no stored soft buffer for this
   (RNTI, pid) under this hypothesis).
3. Same IQ, decoded within the sample lifetime; otherwise INCONCLUSIVE.
4. Gate state ELIGIBLE (a GATED grant produces no evidence of any kind).

### 5.4 P2 acceptance gate (all required before P2 is enabled by default)
- 0 wrong winners across the simulator Monte Carlo campaign (≥ 20 000 acquisitions over the matrix in §6) and all
  live runs.
- Same winner as P1/baseline for every replayable acquisition.
- No material slowdown of the true hypothesis (median trials-to-convergence of the true hypothesis not worse than P1).
- Measurable reduction of full-TB trials and of convergence time.
- Ablation over K, SNR / p_true, catalogue size, probe cost.
- Live multi-UE validation after simulation.

## 6. Validation

### 6.1 Simulator (fast, deterministic) — new
A gtest-based harness linking the **real** `nr_pdsch_config_sweep.c` (and the new gate / score / field-book units):
synthetic RNTIs arriving and leaving (Poisson arrivals, exponential lifetimes), per-RNTI grant rate, rank mix, SIB1
on/off, catalogue size.

**Shared-IQ correlation (mandatory for the P2 gate):** outcomes are NOT independent Bernoulli draws per hypothesis.
Each grant draws one latent channel state (SNR with block fading + a per-RNTI mean, rank, interference) shared by
the main decode and all K−1 probes of that grant. Each hypothesis's outcome is a deterministic function of
(hypothesis correct or not, which fields the grant exercises, latent state, MCS): the true hypothesis passes iff the
latent state supports the MCS; a wrong hypothesis fails, except a **near-twin** (differs only in a field this grant
does not exercise) whose outcome equals the true hypothesis's on that grant — correlated, as on air. Probe
INCONCLUSIVE rates come from the probe-budget/lifetime model. The gate (§4.1) sees an **estimated** SNR = true
latent SNR + estimation error (configurable σ, default 2 dB) so channel-quality gating is evaluated with realistic
mistakes, including fading.

**Replay mode (preferred where data exist):** feed recorded per-grant observation vectors (from the Track-A A3
observation records of live beds/OTA: per grant the DCI fields, SNR estimate and the outcome under the winning
configuration) through the real sweep code, deriving each hypothesis's outcome from the recorded grant by the same
deterministic rules. Outputs per acquisition: grants and simulated seconds to correct convergence, wrong winner (yes/no), full-TB
decodes, probes, field promotions/withdrawals, gated grants. Runs ≥ 20 000 acquisitions per configuration in minutes.

### 6.2 Live bed (acceptance for time targets)
OAI SA rfsim with a 5G core and N OAI UEs (rank 1/2, iperf traffic), receiver at 4 RX and 1 RX. Two arms: **SA**
(SIB1 used) and **NSA-like** (SIB1 ignored via `ISAC_TD_IGNORE_SIB1=1`). Ground truth (C-RNTIs, dedicated PDSCH-Config)
from the gNB config/log — validation only.

The NSA-like arm validates the sweep **under the information constraints expected in NSA** (no SIB1). It does **not**
establish commercial-NSA compatibility; that requires the eventual OTA NSA campaign. Once a valid DCI/grant reaches
the sweep, the sweep itself is SA/NSA-independent.

- **Preferred live bed:** OAI gNB + 5GC on the DGX, conditional on confirming Docker/container access (docker bridges
  `oai-public` / `oai-traffic` exist; access not verified; may need the operator).
- **Fallback:** OCUDU/OAI-over-ZMQ controlled bed.
- Lack of Docker access must **not** block P1/P2 simulator development or offline acceptance.

### 6.3 Ablation and metrics
Arms: baseline · +gate · +top-K(P1) · +ordering · +CellFieldBook · all(P1) · all(P2). Per arm, SA and NSA-like,
4 RX and 1 RX. Metrics: T_winner (s and grants), N_TB (full decodes), N_probe, CPU/GPU cost, P_wrong (wrong winners,
wrong promotions), N_reset (field withdrawals), gated grants, UNDECIDABLE reports. Every metric is emitted in the
receiver's metrics/observation outputs (Track-A A2/A3) so live arms are scored the same way as simulated ones.

### 6.4 Regression
K = 1 + all levers off: bit-identical hypothesis sequence and winner vs current code on recorded sweep traces;
existing `test_nr_pdsch_config_sweep` (44 + 1 skip) unchanged; rfsim regression gate (Track-A A1) unchanged.

## 9. Compute acceleration (operator addition, 2026-10-01)

Objective: cold acquisition must become limited by the arrival of informative grants and statistical evidence, not by
compute. Order of levers: reject useless grants early (§4.1); remove impossible hypotheses cheaply (§4.2); reuse
common PHY work; collect evidence against several wrong hypotheses per grant (§5); batch the remaining expensive PHY
work on the GPU; avoid re-learning stable configuration (TRACKING / VERIFY modes, reconfiguration spec).
**Profile first:** before any CUDA change, measure where time goes per grant and per hypothesis (stage timers
`g_pdtim_*` with `ISAC_PDCCH_TIMING`, plus new per-hypothesis counters) and let that profile choose the kernels.

### 9.1 CPU side (statistics and state stay on the CPU)
- **GrantWork** — one per grant, hypothesis-invariant, computed once: IQ reference (refcounted), FEP, **channel
  estimate over the full set of DM-RS symbols of the slot for each distinct (DM-RS mask, ports, Nl, type, nSCID,
  scrambling id) signature**, noise/SNR, PRB geometry, `G` per signature, legality bitsets, rate-matching metadata,
  `config_epoch`. Stored immutably: any post-processing (interpolation, branch zeroing, SFO rotation) works on a copy.
- **Legality bitsets (§4.2)** — live catalogue ∧ TDRA ∧ TBS-fits-G ∧ LBRM/rate-matching-feasible (E ≥ K−F per CB,
  Foffset ≤ Ncb) ∧ DM-RS-geometry masks, a few machine words per grant. Today impossible hypotheses are discovered
  only inside the decoder ("Problem in rate_matching"); this moves them before decode.
- **Computational signature** — hypotheses that share (TDRA (S,L,k0), DM-RS geometry, Nl, Qm, rate-matching
  geometry) share all expensive work; only the tail (MCS-table-dependent TBS / rate matching / LDPC) differs. Measure
  the number of distinct signatures in realistic catalogues (expected ≪ number of hypotheses).
- **Information-aware ordering** — optional discrimination term in `ordering_score` preferring hypotheses/grants that
  split many remaining candidates; ordering only.
- **Compute modes** — COLD (broad catalogue, K up to the cap, GPU batches), VERIFY (after an epoch change: previous
  configuration first, small neighbourhood, moderate K), TRACKING (K = 1, no sweep unless health degrades).

### 9.2 GPU side (regular PHY work only)
Never on the GPU: grant gate, legality decisions, `select_k`, ordering score, KL test, CellFieldBook, epochs, CORESET
life cycle, state machines. GPU stages by priority (subject to the profile): (1) LDPC — CB0 probes and full-TB code
blocks batched across hypotheses **and** grants; (2) rate de-matching; (3) equalisation + LLR (once per signature);
(4) DM-RS channel estimation (once per signature, only if the profile says so); later: PDCCH (CCE extraction, DM-RS
correlation, polar, re-encode checks). Execution: the RT path enqueues `GrantTrial{GrantWork ref, main, probes[],
generation, config_epoch}` without waiting; a GPU worker collects a batch across grants, runs persistent/batched
kernels (no launch per hypothesis), overlaps upload / pre-processing / LDPC with ≥ 2 CUDA streams, and returns compact
outcomes; old-epoch results are discarded; IQ buffers live until every job referencing them completes (refcount).
On GB10 use the unified memory directly (managed or mapped allocations) instead of the discrete-GPU copy flow.

### 9.3 P2 condition added by the compute design
"Same computation as the full decode" (§5.3 item 1) includes **the same LDPC decoder implementation and iteration
policy**: the CUDA decoder (flooding int8 min-sum, 3/4 damping, 2× iterations) is not the CPU decoder (layered).
A probe FAIL is admitted only if the probe used the same decoder as the hypothesis's full decode would.

### 9.4 Verified state of existing code (2026-10-01, DGX, read-only reviews + GPU tests) — prerequisites
| # | Finding | Severity | Required before |
|---|---|---|---|
| V1 | Chest cache (`t_chest_cache`, `nr_pdsch_passive_decode.c` ~2240-2536) key omits start/number of symbols and the probe horizon; a probe or another (S,L) hypothesis can cache a chest built on a truncated DM-RS set and a later hit reuses it; key truncates 12-bit `dmrs_ports` to 8 bits (type-2 ports 8–11 alias 0–3); cached estimate is mutated in place afterwards (interpolation, branch zeroing, SFO rotation) | **high** | P1 top-K probes, GrantWork |
| V2 | Probe ≠ full decode: probes bypass HARQ combining; the probe horizon truncates FEP/chest/demod after CB0's symbols, changing the chest (interpolation, slope) | **high** for P2 | P2 (fixed by GrantWork full-slot chest + §5.3 conditions) |
| V3 | No sample-lifetime re-check between decode end and Technique D feedback; a CRC_FAIL produced from overwritten IQ is credited (only UNSUPPORTED counts as stale) | medium-high | P1 |
| V4 | CUDA LDPC pool: a silently skipped launch (> 512 CBs) or a CUDA error leaves old bits in a reused pinned slot that the CPU CRC can accept as the current TB (false pass) | **critical** | any GPU LDPC use |
| V5 | PDSCH GPU FEP reads the IQ ring after waiting in its queue with no lifetime re-check (stale-sample decode) | high | GPU FEP use |
| V6 | CUDA LDPC request queue has no bound check; partial slot reservations can deadlock; no error propagation/CPU fallback; workers wait without timeout | high | GPU LDPC use |
| V7 | GPU modules built with discrete-GPU copies (pinned + cudaMalloc + memcpy) on unified-memory GB10; default `LDPC_CUDA_ARCH=89` (must pass 121) | medium (perf) | GPU work |
| V8 | PDCCH GPU FEP module is dead code (never loaded) and its sign convention unverified | low | PDCCH GPU work |
| V9 | CPU vs CUDA LDPC on GB10 (`ldpctest`, BG1 R1/3 K=8448, 8 iterations, 300 blocks): CPU BLER 1.00/0.58/0.00 at Eb/N0 1.5/2.0/3.0 dB, CUDA 0.00/0.00/0.00 (CUDA 0.33 at 1.0 dB); `libldpc_orig` same as CPU; BG2 R1/5 same pattern. CUDA uses 2× iterations; whether the remaining gap is ARM/SIMDE-specific is `[HYPOTHESIS]` — compare on x86 | medium (sensitivity) | decoder choice, P2 §9.3 |
| — | Verified OK: probe and full share max 8 LDPC iterations, segmentation (real C, CRC24B on CB0; C = 1 runs a full CRC24A decode), LLR scaling; generation-tagged tickets ignore stale feedback; GPU tests on sm_121: PDSCH GPU FEP 9 cases OK, polar bit-exact (0 mismatches), batched CB0-size probe work 177 → 9.9 → 6.8 µs/probe at batch 1 → 32 → 256 | — | — |

### 9.5 Additional metrics and ablation
Add to §6.3: CPU utilisation (avg/peak), GPU utilisation and memory, CPU-core equivalents, queue depth and latency,
dropped jobs, sample-buffer lifetime margin, processing latency. Compute ablation: baseline sequential · shared
GrantWork · + signature grouping · + top-K CPU · + batched GPU LDPC · + GPU rate de-matching · full pipeline. GPU use
is reported separately from the 4-extra-CPU-core cap. All §1 hard requirements are unchanged.

## 7. Out of scope
Bayesian rewrite; per-field final decisions; GPU probe batching (A9 / multi-cell); UL (PUSCH) interpretation sweep
(same ideas apply later); HARQ soft combining in the search.

## 8. Open items and starting values (final values from the simulator ablation)
- **P1 defaults from the simulator ablation** `[SIMULATED, DGX host, nr_td_sim @00dd79eed4]` (evidence: `tests/passive_rx/td_sim/results_2026-10-01_p1/summary.md`):
  `ISAC_TD_GATE=1` (margin 6 dB), `ISAC_TD_K=1`, `ISAC_TD_W_SIB1/DEFAULT/OBS/FIELD/PROBE=0`, `ISAC_TD_FIELDBOOK=0` (today's `g_prior` pruning stays), `ISAC_TD_P2=0`.
  Only the gate helped (1 RX blind: cold mean 757 -> 603 s, -20 %, reproduced on seed 2; 4 RX neutral); K=3 and the ordering weights are within seed noise (~1 %) and K=3 costs ~700 M probes; the field book as modelled regresses steady RNTIs (steady median 356.6 s vs 10.6 s at 4 RX blind) because it replaces the pruning prior. 0 wrong, 0 undecidable everywhere (probation withdrawal not modelled). Targets: oracle 1 PASS (cold/steady 1.0 s at 4 RX, 1.4 s at 1 RX); oracle 0 FAIL (cold 358.6 s / 512.6 s, steady 10.6 s / 15.4 s). The K=3 line below is superseded by this result for P1.
- **K:** start at **3** (main + 2 probes); larger K only if the ablation shows gain within the compute cap.
- **Ordering weights:** start **neutral** (all `ISAC_TD_W_*` = 0, i.e. today's order); enable term by term in the
  ablation.
- **Contradiction rule:** counted in independent RNTIs (§4.6); M and the RNTI threshold tuned in the simulator.
- **Energy-based start/end symbol observable:** optional, ordering-only, off by default until measured reliable at
  1 RX on the live bed; it must never become a hard exclusion.
