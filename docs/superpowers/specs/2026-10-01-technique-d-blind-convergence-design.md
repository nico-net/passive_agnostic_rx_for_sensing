# Technique D blind-case convergence — design addendum

Status: APPROVED by the operator 2026-10-01 (with the §3 and §4 SUSPECT amendments below). Amends `2026-10-01-technique-d-convergence-levers-design.md` (the
"levers spec"); where the two disagree, this addendum wins. §3 (CRC-pass acceptance) is approved for design and simulation
behind a default-off flag; runtime enablement is a **separate operator acceptance decision after BC6**.

## 1. Why this addendum

Evidence (all `[SIMULATED, DGX host, nr_td_sim]`, plan Tasks 5–7):
- With the DM-RS oracle (mask, last symbol, k0) and the Qm oracle (MCS table) the engine converges in ≈ 1 s at 4 RX
  (median 1.01 s, `@80a2c2d291`); rfsim measures 0.74–1.61 s (`[MEASURED, DGX rfsim 106 PRB]`).
- **Blind** (no oracle, `--oracle 0`): first RNTIs ≈ 357 s median at 4 RX, ≈ 717 s at 1 RX; target ≤ 30 s / ≤ 90 s.
- Ordering (side-information weights, top-K probes with P1) is **structurally inert**: every alive hypothesis gets one
  slot per round and acceptance needs every other hypothesis's upper bound below the leader's lower bound, so ordering
  moves trials inside a round but cannot reduce the number of rounds (≤ ~1 round, ≈ 1 %; Task 6 review).
- P2 (failure-only probe evidence) is biased and produced 932 wrong winners blind (Task 7, spec §5.4).
- The oracles are single-shot hard decisions and are assumed perfect by the simulator; on an unknown cell (low SNR,
  interference, unusual configuration) they can be missing **or wrong**. A wrong DM-RS/Qm decision prunes the truth.

The dominant cost is: **every wrong hypothesis must accumulate enough failures**. Levers that help must either remove
hypotheses (reversibly) or produce more symmetric (pass and fail) evidence per grant, or accept on evidence a wrong
hypothesis cannot produce.

## 2. Equivalence crediting (lever E) — exact, no extra decode

Two hypotheses are **grant-equivalent** when, on that grant, the receiver would run exactly the same computation:
same `tda_start`, `tda_length`, `k0`, `mapping_type`, `dmrs_mask` (14 bits), `dmrs_add_pos`, `dmrs_max_len`, layer
count, modulation order Qm and target code rate R of the grant's MCS index under the hypothesis's MCS table (so TBS
and rate matching are identical). The key is `nr_td_equiv_key(h, nl, qm, code_rate_x1024)`; it extends Task 4c's
`nr_td_signature` with R. Including `dmrs_add_pos`/`dmrs_max_len` may over-split (conservative) and never merges
distinct computations.

Rule: the outcome (PASS or FAIL) of one full-TB decode is credited to **every alive hypothesis in the decoded
hypothesis's equivalence class** on that grant (`feed_equiv`). Both outcomes are credited every time, independently
of engine state ⇒ no selection bias (contrast P2). Per hypothesis the trials are still one per grant, so the
per-hypothesis Bernoulli samples and `nr_crc_interval` remain valid. Main effect: MCS-table twins (differ only in
`mcs_table`) share every non-exercising grant, and duplicates that legality dedup missed share all grants.

## 3. CRC-pass acceptance (lever C) — experimental, default off (operator 2026-10-01)

A wrong hypothesis can pass a 24-bit TB CRC only (a) by a CRC accident (≈ 2⁻²⁴ per decode) or (b) when its
computation is identical to the truth's (an equivalent hypothesis, §2) or (c) when another transmission matches it
(HARQ retransmission of the same TB in the slot a wrong `k0` points to). Lever C uses (a) and excludes (b)/(c):

- A pass is **unique** when the decoded hypothesis was alone in its equivalence class among alive hypotheses on that
  grant **and** the grant carries **new data** (new transmission: NDI toggled; never a HARQ retransmission of a TB
  already counted). Two unique passes therefore come from two independent TBs on two distinct grants. Per hypothesis
  the engine counts `ok_unique[h]`.
- Accept leader L (alongside, never instead of, the KL rule) when
  1. `ok_unique[L] >= m*`, and
  2. every other alive hypothesis has `ok_unique == 0` ("clean lead": any unique pass elsewhere — HARQ trap, near
     neighbour that really decodes — disables lever C for this state until the next prune/rebuild, and the KL rule
     decides as today), and
  3. `m* = max(2, smallest m with n_alive · C(T_max, m) · 2^(−24·m) <= 1e-6)`, where `T_max` = largest trial count
     of any alive hypothesis (same 1e-6 budget as the KL rule; log domain).
- The runtime probation after settlement (`nr_pdsch_config_sweep.c` invalidation when the winner's rate drops below
  `SWEEP_MIN_RATE`) stays as the safety net.
- Expected effect: convergence ≈ one round (the truth's first unique pass makes it "hot"; the exploit rule then
  delivers the second within a few grants).
- Flag `ISAC_TD_CRC_ACCEPT` / simulator `--crc-accept`, default 0. The KL rule stays untouched alongside it.
- **Evidence standard.** The 1e-6 wrong-winner argument is **analytical** (CRC-24 bound, new-data independence,
  exact equivalence collapsing, clean-lead condition). Monte Carlo only exposes implementation and correlation
  errors: 0 wrong winners in N acquisitions bounds the rate only to ≈ 3/N (95 %), e.g. ≈ 1.5e-4 for 20 000; a
  1e-6-scale empirical bound would need millions of acquisitions. BC6 therefore also runs a **stress arm** with an
  inflated false-pass probability (`--crc-false 1e-3`) where the analytical bound predicts a measurable wrong rate,
  and checks the measured rate against the formula.
- Enabling at runtime: separate operator decision after BC6 (and an amendment of the levers-plan Global Constraint).

## 4. Reversible pruning field book (lever F, replaces levers spec §4.6) — operator decision 2026-10-01

Catalogue construction:

```text
FULL JOINT CATALOGUE
  ├── deterministic per-grant legality          (Task 4b; per grant, not dormant)
  ├── existing cell-prior mask                  (dormant cause PRIOR)
  └── trusted field-book masks                  (dormant cause FIELD_f, one per field)
        ▼
  ACTIVE CATALOGUE  → Technique D / KL (and lever C)
  excluded hypotheses stay in the DORMANT CATALOGUE (never deleted)
```

- **Dormant, not deleted.** Each cause has its own mask (`PRIOR`, `FIELD_TDRA`, `FIELD_DMRS_ADD_POS`,
  `FIELD_DMRS_MAX_LEN`); a hypothesis is active iff no cause marks it. Withdrawing one field clears only that field's
  mask. A cause that would leave zero active hypotheses is refused.
- **Dormant hypotheses accumulate nothing.** `next`/`next_k` never select them, feeding a dormant index is ignored,
  the acceptance rules (KL, fallback, lever C) and the union-bound class count use the active set only. When reopened
  they resume with the evidence they had when they became dormant (zero for hypotheses dormant from context creation).
- **Fields.** TDRA (S, L, mapping, k0), DM-RS additional position, DM-RS max length; later other validated stable
  fields when the hypothesis carries them. **MCS table is never promoted.**
- **Field states.** `UNSEEN → CANDIDATE → PROMOTED (≥ 2 distinct independent RNTIs, same config_epoch) → SUSPECT
  (first independent contradiction) → PROMOTED (re-confirmed by a further independent RNTI) | WITHDRAWN (second
  independent contradiction; global restore of its dormant hypotheses)`. After WITHDRAWN the field returns to
  CANDIDATE with its candidate table (a new value promotes with 2 supporters).
- **Pruning status (operator 2026-10-01).** Only PROMOTED fields of the current epoch prune. On the transition to
  SUSPECT (first independent contradiction):
  - new contexts do **not** prune on this field (hint only, ordering);
  - existing **unsettled** contexts immediately restore the hypotheses dormant **only because of this field**
    (clear that field's dormant cause; masks of other fields, the prior and deterministic legality are untouched) —
    a suspect field must not keep narrowing a search that may be the one revealing the contradiction;
  - already **converged** contexts keep their winner but mark the field-derived assumption untrusted (flag on the
    context, logged; their probation continues as today).
  Re-confirmation by a further independent RNTI → PROMOTED (applies to contexts created afterwards); a second
  independent contradiction → WITHDRAWN.
- **Independence.** A converged RNTI supports or contradicts field f only if f was **not** pruned in that context
  (or the context was in fail-open when it converged). A context pruned to value v cannot vote for v (circular).
- **Epochs.** Support and contradiction sets are tagged with `config_epoch`. On an epoch bump PROMOTED/SUSPECT fields
  stop pruning and keep their value only as an ordering hint (`hint_value`) until 2 independent RNTIs of the new epoch
  confirm it. Old-epoch support never maintains pruning.
- **Fail-open (per RNTI).** Counted in eligible learning opportunities, not time: when a context has had
  `N_fo = ceil(n_active · ln(1/α) / p_min)` consecutive eligible trials (gate ELIGIBLE, samples valid) without any PASS
  on an active hypothesis (defaults α = 1e-3, p_min = 0.05), the context reopens **all** its dormant hypotheses (PRIOR
  and FIELD causes) for this RNTI only. If it then converges to a value that conflicts with a promoted field, that is
  an independent contradiction for the field. The prior is handled as today (its own invalidation path).
- **Today's behaviour preserved.** `ISAC_TD_FIELDBOOK=0` (default) keeps today's destructive prior pruning and
  catalogue exactly (bit-identical); dormant masks are used only with `ISAC_TD_FIELDBOOK=1`, where the prior becomes a
  dormant cause too.

## 5. Soft oracles (lever S) — separate spec after BC0

Replace single-shot DM-RS/Qm decisions by per-RNTI accumulated log-likelihood scores per candidate (pilot energy at
the hypothesis's DM-RS positions; constellation fit per MCS table) with an error-controlled sequential threshold;
prune (as a dormant cause `ORACLE`) only when the threshold is crossed. Needs receiver DSP changes and rfsim/OTA
validation at low SNR; the simulator task BC0 first quantifies how often missing/wrong oracles hurt today. Its own
spec and plan follow BC0.

## 6. GPU full-TB decodes per grant (lever G) — folded into plan Task G4

K complete transport-block decodes per grant from a state-independent round-robin schedule, PASS and FAIL both
credited (no P2 bias) — ≈ K× more evidence per grant. Requires G1/G2/R1 and K38 is irrelevant (full decodes, not
CB0 probes). Recorded in the levers plan's G4 task.

## 7. Simulator realism (prerequisite)

- `--oracle-miss P`: per RNTI, with probability P the oracles produce nothing (that RNTI is blind).
- `--oracle-wrong P`: per RNTI, with probability P the DM-RS oracle reports a legal neighbouring mask/last symbol and
  the Qm oracle a wrong table (destructive prune of the truth, as today's runtime would do).
- `--harq-trap P`: per grant, with probability P the `k0 ± 1` neighbour of the truth (same S, L, mask, table) passes.
- `--crc-false P` (default 2⁻²⁴): per wrong-hypothesis decode false-pass probability.
- Metrics added: active-catalogue size at RNTI start, fail-open count, recovery latency (grants from a wrong promotion
  to WITHDRAWN and to the first correct convergence), unique passes, lever-C acceptances.

## 8. Validation gate (BC6)

Arms × {oracle 1, oracle 0, oracle-miss 0.3, oracle-wrong 0.05} × rx {4, 1} × cell {SA, NSA-like}, twins 2,
harq-trap {0, 0.01}, ≥ 2000 acquisitions × 4 RNTIs per cell: current all-or-nothing prior (today); ordering-only
field book; +E; reversible-pruning field book (+E+F); +E+F with forced wrong/stale promotion; +E+F+C (experimental);
plus the lever-C stress arm (`--crc-false 1e-3`) compared with the analytical bound. Pass: wrong = 0 everywhere; undecidable not above baseline;
oracle-1 time not worse than baseline beyond seed noise; field-book recovery from a forced wrong promotion within
2 RNTIs; blind cold median ≤ 30 s at 4 RX and ≤ 90 s at 1 RX (target — report the gap if missed).

## 9. Amendments to the levers spec

- §4.6 replaced by §4 above. §8 starting value K = 3 → **K = 1** (Task 6 and Task 7 reviews: P1 probes inert, P2 off).
- Plan R2: `FieldBookOnOrdersInsteadOfPruning` → `FieldBookOnUsesDormantPruning`; the bit-identity regression runs with
  explicit `ISAC_TD_GATE=0` (GATE=1 is now the default); R2 Step 5 runs with K = 1, gate 1, weights 0, fieldbook per
  the BC6 decision.
