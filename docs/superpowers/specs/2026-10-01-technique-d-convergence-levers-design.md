# Technique D convergence levers — design

Date: 2026-10-01. Status: approved in conversation (operator), pending written-spec review.
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
| Cost | extra CPU for probes ≤ 4 cores at 273 PRB on the DGX; no increase of scanq / PDSCH-queue drops | DGX |

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
- Contradiction: a converged RNTI with a different value for that field, or ≥ M (default 64) consecutive full-TB
  failures of hypotheses carrying the promoted value while another value passes → `contradictions++`; above the
  per-field threshold (default 2) the field is **withdrawn** (promoted → unknown), logged, and its ordering bonus
  removed from live contexts.
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
synthetic RNTIs arriving and leaving (Poisson arrivals, exponential lifetimes), per-RNTI grant rate, rank mix,
per-hypothesis decode probability (true hypothesis p_true; wrong hypotheses 0, plus an adversarial arm with a
near-twin hypothesis at p_twin < p_true), probe outcome model (PASS/FAIL/INCONCLUSIVE rates), SIB1 on/off, catalogue
size. Outputs per acquisition: grants and simulated seconds to correct convergence, wrong winner (yes/no), full-TB
decodes, probes, field promotions/withdrawals, gated grants. Runs ≥ 20 000 acquisitions per configuration in minutes.

### 6.2 Live bed (acceptance for time targets)
OAI SA rfsim with a 5G core and N OAI UEs (rank 1/2, iperf traffic), receiver at 4 RX and 1 RX. Two arms: **SA**
(SIB1 used) and **NSA-like** (SIB1 ignored via `ISAC_TD_IGNORE_SIB1=1`). Ground truth (C-RNTIs, dedicated PDSCH-Config)
from the gNB config/log — validation only. Dependency: a usable 5G core on the DGX (docker bridges `oai-public` /
`oai-traffic` exist; not verified; docker access may need the operator). Fallback: OCUDU-over-ZMQ bed if available.

### 6.3 Ablation and metrics
Arms: baseline · +gate · +top-K(P1) · +ordering · +CellFieldBook · all(P1) · all(P2). Per arm, SA and NSA-like,
4 RX and 1 RX. Metrics: T_winner (s and grants), N_TB (full decodes), N_probe, CPU/GPU cost, P_wrong (wrong winners,
wrong promotions), N_reset (field withdrawals), gated grants, UNDECIDABLE reports. Every metric is emitted in the
receiver's metrics/observation outputs (Track-A A2/A3) so live arms are scored the same way as simulated ones.

### 6.4 Regression
K = 1 + all levers off: bit-identical hypothesis sequence and winner vs current code on recorded sweep traces;
existing `test_nr_pdsch_config_sweep` (44 + 1 skip) unchanged; rfsim regression gate (Track-A A1) unchanged.

## 7. Out of scope
Bayesian rewrite; per-field final decisions; GPU probe batching (A9 / multi-cell); UL (PUSCH) interpretation sweep
(same ideas apply later); HARQ soft combining in the search.

## 8. Open items (decide during planning)
- Default weights and K — from the simulator ablation.
- Exact rule for "M consecutive failures while another value passes" in contradiction counting — simulator.
- Whether the energy-based start/end symbol observable is reliable enough at 1 RX — measured on the live bed.
