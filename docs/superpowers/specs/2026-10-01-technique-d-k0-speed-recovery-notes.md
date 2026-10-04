# Technique D: recovering blind-convergence speed under the k0 trap — design notes

Status: read-only investigation, 2026-10-01. Not committed; the controller decides. No code was written and nothing was built or run.
Scope: speed recovery AFTER fix round 1 (A: fp_trials from exploration only; B: k0-sibling guard N_sib). This note does not touch that fix.
Hard rules inherited: 0 wrong winners, 0 wrong geometry pins; the analytical argument must match the real selection process;
never credit one computation to several hypotheses (BC1); hard exclusions only if physically or specification deterministic, soft evidence = priority only.

Evidence labels: [CODE-READ] seen in the worktree at /home/nicola/NICOLA/wt/td-levers (cited file:line); [DERIVED] mathematical consequence of stated
premises; [ASSUMPTION] needs a measurement or an operator decision; [SIMULATED] only from earlier reports. No new measurements were made.
All speed estimates below are [DERIVED] from the BC2b report figures (sim default 200 grants/s, blind n_active ~ 375, truth pass rate ~0.85 at 4 RX) and
must be replaced by simulator results.

## 0. Findings that change the picture (read first)

F1. **Where the fix spends its 7-10 s.** [DERIVED] Fix A removes the exploit trials from the evidence: after the truth's first pass it is "hot"
(nr_pdsch_config_sweep.c:657, 3 of 4 picks), so only every 4th pick is an exploration pick and counts. The second fast-path pass of the truth then needs about one
round at 1/4 duty: 4 x n / (p x 200 grants/s) ~ 4 x 375 / (0.85 x 200) ~ 8.8 s. B's 277 deliberate trials cost ~1.4 s per sibling. So A's duty cycle, not B,
is the dominant cost.

F2. **B (zero-pass sibling test) cannot clear when the trap is not rare.** [DERIVED] If the truth is L and a sibling s traps with per-trial pass probability q
(q = P(adjacent allocation identical) x truth pass rate), the sibling test needs 0 passes in 277 trials: P = (1-q)^277.
q=0.002 -> 0.57; 0.005 -> 0.25; 0.01 -> 0.06; 0.05 -> 7e-7. So for any realistic persistence of allocations B almost always BLOCKS the fast path and the
RNTI falls back to the KL rule (336 s). The 7-10 s expectation holds only when the trap is below ~0.2 % per trial. The zero-pass criterion is the wrong test in
the regime it was added for; the sibling's passes on trap-possible grants are expected and carry no information.

F3. **Identifiability.** [DERIVED] If two consecutive grants have an identical decode computation (same PRB set, ports, MCS->TBS/Qm, rv, symbols), then
the leader's decode of grant g at slot t+k_L is bit-for-bit the same computation as the sibling's decode of the neighbouring grant g' at the same slot. A pass
pattern of a constant allocation stream shifted by one slot is indistinguishable from the unshifted one. Therefore **k0 is identifiable from CRC outcomes only
through grants whose neighbour allocation differs (or is absent)**. No test, fast or slow (KL included), can use the other grants. This is why the
DCI-adjacency stratum (item 1) is not just an accelerator: it is the exact set of k0-informative grants, and it is what makes the fast path usable at all under
the any-grant trap.

F4. **Existing oracle hazard (not part of the fix).** [CODE-READ] nr_pdsch_passive_queue.c:~802 records the DM-RS observation with `k0 = job.sweep_ticket.k0`
(the k0 of the hypothesis that happened to be decoded) and nr_pdsch_config_sweep.c:481-492 (`obs_admits`) prunes every hypothesis with a different k0. Under full-buffer
traffic both slots carry DM-RS, so whichever sibling is decoded first fixes the k0 for the cell record (a later conflicting job relaxes it to -1, but until then
the prune is destructive). In the trap regime this is a coin flip, soft evidence acting as a hard exclusion. The simulator's oracle arm assumes perfect k0. Recommend
the operator decide separately whether `obs_admits` may use k0 from a hypothesis job; this is flagged, not analysed further.

F5. **TDD slot direction is a deterministic k0 discriminator that is not used.** [CODE-READ] `nr_td_legal.{h,c}` contains no slot-direction rule (grep
"tdd" empty), while the SIB1 TDD common pattern is derived (nr_passive_acq_state.c:202, `nr_passive_acq_tdd_slot_has_downlink` :217, `nr_tdd_slot_direction`
nr_tdd_pattern.h:72). See item 3(c).

## 1. k0 discrimination from DCI adjacency

### 1.1 What the receiver can observe
- [CODE-READ] Every accepted DCI 1_x yields `nr_pdcch_blind_result_t` (nr_pdcch_blind_monitor.h:166-258): rnti, start_rb/num_rb (or `rbg_bitmap` for RA type 0), start_symbol,
  num_symbols, dl_dmrs_symb_pos, n_dmrs_cdm_groups, dmrs_ports, nscid, mcs (:188), rv (:189), ndi (:190), harq_pid (:191), tda_index (:192), mapping_type, dci_format, mcs_table.
  All are known BEFORE any PDSCH decode. Under a hypothesis the TBS is a function of (mcs index, table, PRBs, symbols); Qm and the code rate come from the table.
- [CODE-READ] The monitor keeps NO per-slot DCI history: accepts are logged (nr_pdcch_blind_monitor_rt.c:6152) and turned into jobs (:6431-6434, :6656-6672). The
  UL side has a bounded grant book (nr_passive_ul_grant_book.h) but there is no DL analogue. Grants can also be dropped before decode (rv0_only :6278, per-slot cap
  `decodes_this_occasion` :6279/:6401, gate), so a history must be written at ACCEPT time, before every drop, not at decode time.
- [CODE-READ] The Technique D contexts are per (RNTI, tda_index) (nr_pdsch_config_sweep.c:~1470-1520); k0 is a field of the hypothesis (nr_pdsch_config_sweep.h:54).
  NDI/HARQ pid already travel with the job (`nr_pdsch_passive_grant_t.ndi`, nr_pdsch_passive_decode.h:75).

### 1.2 Mechanism: per-world occupant check
Take a grant g (RNTI R, row i, DCI slot t) and a candidate leader L with offset k_L. For each alive sibling s with k_s != k_L (same S, L, mapping, mask; table free)
consider the world "the truth is s":
- In that world every DCI of row i schedules its PDSCH k_s slots later. The slot L decodes, u_L = t + k_L, is the PDSCH slot of the row-i DCI at slot
  **t + d', d' = k_L - k_s**. The circularity disappears because each sibling is enumerated as its own scenario; the unknown true k0 never has to be named.
- If a row-i DCI X of R was positively observed at t + d', then in that world u_L carries exactly X's transport block (one PDSCH per RNTI per slot, [ASSUMPTION] A1).
  L's decode of g can then pass only if X's computation equals g's computation (the compatibility key below), or by CRC accident.
- Compatibility key (conservative): X and g are *incompatible* only if a provable difference exists in the decode computation: different PRB set (start_rb/num_rb or RBG bitmap),
  different layers/ports/nscid/cdm groups, different symbols, or (TBS, Qm, G, rv) different under EVERY pair of alive MCS tables (`nr_td_equiv_key`, nr_td_legal.h, already encodes
  geometry, DM-RS, layers, Qm, code rate and LBRM class). Fields that only change soft quantities (HARQ pid, NDI) are ignored; unknown fields never make a pair incompatible.
- **Definition.** g is *k0-unambiguous for leader L* iff for every alive sibling offset k_s != k_L: a row-i DCI of R was observed at t + (k_L - k_s) (the monitor having
  scanned that slot) and every DCI of R observed in that slot is incompatible with g. Absence, a missed slot scan, a gap in the history, or a compatible DCI make g
  *ambiguous* for that sibling, hence for L. Missed DCIs are never "trap-free".
- Different-row DCIs never prove anything (their k0 is another context's unknown). Only same-row DCIs count. This keeps the cold-start case (nothing promoted) sound.
- Range > +-1: the offset set is D(L) = { k_L - k_s : k_s alive, != k_L }. A catalogue {0,1} gives one offset per leader (L=0 -> t-1, L=1 -> t+1). Each extra observed k0 layer (k0_seen,
  nr_pdsch_config_sweep.c:1287, `add_k0` :1632) adds an offset, so the stratum probability decays geometrically (f_S ~ f^|D|). Past ~3 siblings this path is
  useless and the receiver falls back to the sibling test (item 2). Offsets beyond the catalogue are NOT covered (A2: truth is in the catalogue).
- Latency: positive offsets need DCIs from slots after t. The consumer already waits for slot t+k_L; classification is deferred until the monitor's scan watermark
  passes t + max(d'), at most a few slots. The IQ ring keeps spf-2 slots, so this costs no samples.

### 1.3 Fast path on the stratum; can the sibling test be dropped?
Rule (stratified lever C/P): a new-data main-decode pass counts as a *certified pass* of h only if the grant is k0-unambiguous for h. fp_trials[h] counts the exploration
trials of h on grants unambiguous for h (T_h^S). m* (or the e-process of 2.3) uses these counts. Clean lead: any OTHER hypothesis with a certified pass blocks; uncertified
passes (traps) of siblings are ignored. Lever P: ok_geom counts certified passes only; two groups with certified passes block.
- **Yes, within the stratum the sibling test is unnecessary**: in every sibling world the trap probability on a stratum grant is the CRC-accident level, so the
  wrong-winner bound has the same form as for non-k0 wrong hypotheses. B remains only as a fallback for the grants outside the stratum (and for k0 layers not covered).
- Not covered by this argument (unchanged): twins/equivalent hypotheses (class attribution), retransmissions (new_data), catalogue incompleteness (A2).

### 1.4 Is the stratum unbiased? (BC1 lesson)
- BC1's bias: a twin was credited only on grants where its outcome equals the truth's, so its estimated rate was pulled up toward the truth's. The crediting condition
  was correlated with the outcome difference between hypotheses.
- Here the condition depends only on DCI observables (neighbour allocations and detection) and the alive k0 set. It is applied to the credited statistic of each
  hypothesis, never used to equalise two hypotheses' outcomes. For a WRONG sibling its pass probability on the stratum is the accident level by construction
  (the trap needs a compatible occupant, which the stratum excludes). Conditioning on S can only lower a wrong hypothesis's pass probability. For the truth, p_T|S may differ
  from p_T (link adaptation and PDCCH detection both correlate with SNR); this changes speed, not soundness, because acceptance is a count of certified passes, not a rate comparison.
- Two requirements keep it clean: (i) the stratum evidence is used ONLY for the fast-path certified counts, never mixed into the KL rates (KL stays on all grants);
  (ii) stratum membership must not depend on which hypothesis was picked beyond the leader's own k0: the per-hypothesis stratum S_h is fine for the union bound
  (each wrong h only needs p_h|S_h <= accident level and its own T_h^S fixed independently of outcomes). The hypothesis-independent version S_all (all pairwise
  offsets observed) is conservative and simpler to explain; compare both in the simulator.
- Selection must not look at outcomes: with decode deferred until the stratum is known, the choice "which hypothesis gets this grant" can be taken after S is known and still
  depend only on S and the fixed round order (see item 5).

### 1.5 Soundness budget (what "positive DCI" really assumes)
[DERIVED] The exclusion is wrong only if (a) X was a false accept carrying RNTI R and (b) the real occupant was compatible (its DCI missed). False accept of one noise
candidate with a specific RNTI: ~2^-24 x plausibility pass-rate (the monitor additionally uses the re-encode mismatch gate, nr_pdcch_blind_monitor.h:~195); with ~300 candidates per
slot ~1e-5 per slot; times a PDCCH miss probability (say 1e-2, [ASSUMPTION] A3, to be measured) ~1e-7 per grant and sibling; two certified passes make it ~1e-14. Same class of
argument as CRC-24 and charged to the same 1e-6 budget. The assumptions that must hold: A1 one unicast PDSCH per RNTI per slot per carrier (Rel-15 FR1, no multi-TRP
multi-DCI; SPS is CS-RNTI, so a C-RNTI context is not affected), A2 truth in the catalogue (k0 >= 2 layers appear through the existing k0 oracle before acceptance), A3 PDCCH
false-accept/miss rates measured and small.
- Sound? **Conditional yes**: yes given A1-A3; without A1 the exclusion is not deterministic.
- Evidence label: [DERIVED] + [CODE-READ] for the observables; the miss/false-accept rates are [ASSUMPTION].

### 1.6 Expected gain and cost
- Gain depends on f_S (fraction of eligible grants that are unambiguous). [DERIVED] Time ~ m* x n / (K x f_S x p x rate) with K hypotheses per stratum grant (item 4/5).
  n=375, p=0.85, 200 grants/s, m*=2: K=1: f_S=0.5 -> 8.8 s, 0.1 -> 44 s, 0.02 -> 220 s; K=8: 1.1 s, 5.5 s, 27 s.
  Constant-allocation lab traffic (identical adjacent allocations) gives f_S ~ 0: then nothing can accelerate the CRC path (F3) and the correct outcome is fallback to KL.
  Loaded multi-UE cells or link-adapting schedulers give f_S of several tens of percent [ASSUMPTION], to be measured on the DGX captures (frozen sens6 data may be read-only used to
  estimate f_S offline).
- Cost: a DCI book (ring of the last ~40 slots per RNTI, 24 bytes per DCI; mutex or per-RNTI lock), a monitor scan-watermark, a deferred-classification step before feedback
  (nr_pdsch_passive_queue.c:~1148), one flag on feed_attr/feed_shared (`certified`). Negligible CPU (a few ns per lookup; add a PDTIM-style counter).
- Simulator: needs a time-ordered grant stream (item 6 below). The current simulator draws i.i.d. grants (nr_td_sim.cc:~440: mcs i.i.d., no neighbours), so it cannot represent adjacency at all.

## 2. Reducing N_sib safely

### 2.1 p_min from the leader's own statistics
- The relation. [DERIVED] In the trap world (truth s, L passes by trap) L's trap pass on g is the truth's decode of the neighbour grant g': P(L passes g) = 1[compat] x p_T(g') <= p_T(g').
  The sibling, if it is the truth, passes grant g with probability p_T(g). So E[sibling passes over a set A of grants] >= E[L trap passes over the grant set B] provided A covers the shifted set B + d'.
- Rigorous? **Only under a shift-stationarity assumption** [ASSUMPTION] A4: the truth's pass probability sequence p_T(.) is exchangeable over the window of a few consecutive slots (or A is
  chosen to include B + d'). Fading across slots is not adversarial but SNR drift correlated with allocation changes is possible, so this is "conditional", not deterministic.
- Selection bias. p_min must come from FRESH trials after L is nominated (a confirmation phase of n_c = 20-40 new trials of L). The passes that made L the leader are selected for being lucky
  and would bias the estimate up (the P2 lesson). With x passes in n_c, p_min = Clopper-Pearson lower bound at level delta_p: 17/20 -> 0.58, 34/40 -> 0.67 (99 %); 2/3 -> 0.06 (useless: too few trials).
  Then N_sib = ceil(ln(n_sib/eps)/p_min): p_min 0.58 -> 24 (n_sib = 1, eps = 1e-6) vs 277 at p_min = 0.05. Budget: eps_sib + delta_p share the 1e-6.
- Deterministic version? **No.** The strongest deterministic statement is the identity of F3: if g and its neighbour g' are compatible, the leader's pass on g and the sibling's pass on g' are the same computation
  (same slot, same key, same decoder), so a leader pass on a compatible grant NECESSARILY coincides with a sibling pass. It shows the compatible grants are uninformative; it provides no lower bound
  on the sibling's rate. Everything deterministic about k0 comes from incompatible neighbours = the stratum of item 1.
- Sound? **Conditional** (A4, fresh confirmation trials). Evidence label: [DERIVED] + [ASSUMPTION] A4.

### 2.2 Anytime sequential sibling test (SPRT / Ville), budget split
- Per sibling: H0 "s is the truth with per-trial pass probability >= p_min" vs H1 "s is wrong (accident level eps0 = 2^-24)". LR for rejecting H0: product of
  (1-eps0)/(1-p_min) per failure and eps0/p_min per pass. Under H0 this is a supermartingale, so P(sup LR >= 1/delta_s) <= delta_s (Ville), valid for ANY predictable schedule
  (any stopping time, adaptive trial counts). Failures only: LR = (1-p_min)^-N, i.e. the same N as the fixed formula but now sequential and stoppable.
- A pass multiplies LR by eps0/p_min (about 1e-6): the sibling is plausible as truth, so it blocks (the present behaviour) and the accumulated evidence is gone.
- Budget: delta_s = eps/n_sib (union over siblings), or a share of the global 1e-6 split over fast-path restarts (delta_r = delta/(r(r+1))).
- Early stop: (i) reject s as soon as LR >= 1/delta_s; (ii) accept L as soon as it has m certified passes on the stratum (then no sibling test at all, item 1); (iii) if a sibling is hard-excluded
  by the TDD rule (3c), remove it from the set; (iv) if some sibling has a certified pass, block immediately.
- Bundling: siblings do not have to be tested one after another. All n_sib siblings can be run on the SAME grant (K = 1 + n_sib decodes of different slots, item 4). N_sib grants then cost N_sib grants, not
  n_sib x N_sib. Per-sibling bounds are marginal, so dependence between siblings on one grant does not matter for the union bound.
- Sound? **Yes** for the null "sibling is the truth with p >= p_min"; the choice of p_min inherits 2.1's conditionality. Evidence label: [DERIVED].
- Gain: B cost from 277 x n_sib grants (1.4 s x n_sib at 200/s) to ~24-40 grants (0.12-0.2 s) when the leader's rate is high. Zero help in the F2 regime (compat grants make the sibling pass); there item 1 is needed.

### 2.3 The same anytime machinery also repairs the 4b failure of the m* formula (candidate replacement for A's duty cycle)
- [DERIVED] The 4b failure (measured 47 vs bound 1.43) comes from evaluating C(T,m) eps^m at the observed T_h at the stopping time, with T_h grown by exploit scheduling. The sound statement is
  an e-process per hypothesis: E_h = prod over its trials of (p1/eps0)^X ((1-p1)/(1-eps0))^(1-X) (or a mixture over p1 in {0.05,0.2,0.5,0.9}, still a martingale). For a wrong h (p <= eps0, accident only) it is a
  supermartingale under any predictable schedule, so P(sup E_h >= 1/delta_h) <= delta_h, sum over hypotheses and over fast-path restarts <= 1e-6 (delta_h = 1e-6 / (n_max x r(r+1))).
  Numbers (delta_h = 1e-9, accident eps0 = 2^-24): each pass is worth 13.6 nats at p1 = 0.05 (16 at p1 = 0.5); two passes accept if fewer than ~128 (p1 0.05) / 16 (p1 0.5) interleaved failures of the
  same hypothesis, 3 passes ~394 / 39. Fits the truth's ~0.85 pass rate easily.
- Consequence: **exploit trials may count again** (the hot hypothesis may be tried 3 of 4 times), because validity no longer depends on the schedule. This would remove F1 (A's 1/4 duty).
  It does NOT cover non-accident passes (twins, k0 traps, retransmissions): those stay handled by attribution, new_data, and items 1-2.
- The existing simulator stress arm (--crc-false 1e-3 / 1e-2) checks the bound directly: measured wrong must be <= sum delta_h. If it still exceeds the bound, the e-process premise
  (independent Bernoulli(eps) per distinct (hypothesis, grant) decode) is violated somewhere, and the plan has found a real implementation or modelling error before the fast path goes live.
- Sound? **Yes, conditional on the independence premise and on counting each (hypothesis, grant) decode once**. Evidence label: [DERIVED]. This is an alternative to A's exploration-only rule; evaluate it
  as a separate arm, do not replace A until the stress arm agrees.

## 3. Cheap sibling rejection

### 3a. CB0 probes instead of full-TB decodes for the zero-pass sibling test
- Inclusion: pointwise, TB pass => CB0 pass for the same IQ, same chest/demod, same decoder. So 0 CB0 passes in N trials implies 0 TB passes and
  P(0 CB0 passes | s truth) <= (1-p_min)^N, the same bound. One-sided, no rate estimate, scheduled before outcomes are known, so no selection (this is NOT P2: P2 failed because
  it fed probe FAILs into the KL rate under an outcome-dependent allocation; Task 7 report, levers spec §5.4).
- Conditions (levers spec §9.3, K38): [CODE-READ] the probe horizon truncates demod to the symbols CB0 needs (nr_pdsch_passive_decode.c:2099, `t_probe_first_seg` :1086; segment-0 only
  :1299-1306, :1339-1352). F1 shows CB0 LLRs equal the full decode's at 106 PRB 1 RX (0/1165 mismatches; F1 report) but K38 stays open at rank > 1. So: allowed at Nl = 1 with the same decoder, or with the horizon forced to 0 for Nl > 1.
  A CB0 pass only blocks (it does not prove a TB pass), so the false-clearance direction is excluded; the cost of a spurious CB0 pass is an unneeded block, and the all-zero guard (:1346-1352) must stay.
- **Exact variant, no probe statistics at all:** TB fails as soon as any CB fails. A full-demod decode that aborts after the first failed CB (and continues only if CB0 passes) has EXACTLY the full TB outcome.
  [CODE-READ] today the coding interface decodes all C segments in one call (:1306-1332) and checks seg_ok afterwards (:1356). Saves (C-1) x ~278 us of LDPC per failing trial at 106 PRB 1 RX (F1 PDTIM: ldpc 278 us of ~457 us/probe),
  nothing at rank 4 (demod 4.4 ms dominates). Needs a decoder flag; HARQ soft-buffer side effects for aborted attempts must be confirmed irrelevant to sweep trials.
- Sound? **Yes (conditional on same decoder/IQ; exact variant needs no condition).** Evidence label: [CODE-READ] + [DERIVED]. Gain is compute, not evidence: it matters only when K > 1 decodes per grant are compute-limited.

### 3b. DM-RS correlation or energy in the sibling's slot
- [CODE-READ] `dmrs_oracle_measure` exists and already probes slots +1..K (nr_pdsch_passive_queue.c:~835-880) but (i) is a threshold on any DM-RS with the cell's scrambling, not RNTI-specific (MU-MIMO or another UE on the same PRBs fires it),
  (ii) runs on 1 job in 8, (iii) has a non-zero miss probability that is not modelled.
- "No DM-RS energy at all on the grant's PRBs in the sibling slot => no transmission => no trap" is true physically, but the observation is a statistical detection: absence is deterministic only up to the miss probability.
  Under the rule "hard exclusion only if deterministic" it is **priority only** (order siblings/grants, never certify).
- More fundamentally it does not help in the problem regime: the trap needs a compatible TB in the sibling slot, and a TB carries DM-RS, so the trap grants are exactly those where the energy is present. DM-RS only removes grants where the sibling pass was impossible anyway.
- Sound? **No as a hard exclusion; yes as priority.** Evidence label: [CODE-READ] + [DERIVED]. Gain ~0 for the trap regime.

### 3c. Resource-geometry / slot-direction incompatibility
- TDD slot direction. A hypothesis (row i, k_s) is impossible if a REAL grant of row i has t + k_s in an UL slot (or its S..S+L-1 symbols reach UL symbols of a mixed slot) according to the SIB1 common TDD pattern: a PDSCH cannot be
  scheduled on UL symbols (TS 38.213 11.1, 38.214 5.1.2). [CODE-READ] pattern and direction function exist; `nr_td_legal` has no rule for it. This is **deterministic** given (i) a CRC-verified SIB1 TDD pattern, (ii) a real DCI, (iii) the same
  absolute-slot alignment the PDCCH path already uses (nr_pdcch_blind_monitor_rt.c:3151). It eliminates the sibling (not just the trap on one grant), is free, and also shortens the catalogue before any decode. Gain exists on TDD cells only;
  for the DDDSU-like lab cell the k0=1 hypothesis dies at the first grant sent in the last DL slot before the UL slots. Dedicated TDD configuration cannot turn common UL symbols into DL, so the exclusion holds.
- Not deterministic: "no DCI at t + d' because that slot is UL" does NOT prove slot u_L is empty, since another row could reach u_L with a different k0; the single-row occupant argument of 1.2 only holds with an observed row-i DCI.
- PRB footprint: k0-independent (no help). SSB/CSI-RS overlaps are rate-matched, not forbidden (not an exclusion).
- Sound? **Yes (TDD legality, conditional on a verified SIB1 pattern)**. Evidence label: [CODE-READ] + [DERIVED] (spec).

## 4. Shared computation and batching

- Leader and sibling decode DIFFERENT slots, so there is no shared IQ or FEP between them for one grant. Sharing exists ACROSS grants and trials of one slot: [CODE-READ] the slot-level FEP share (t_share/`share_slot`) and F1's exact chest key
  (`nr_pdsch_chest_key_t`, key includes slot, DM-RS positions, ports, scrambling, rb range, BWP, ref point, branch). GrantWork (plan Task R1) should therefore be keyed by (slot, signature), not by grant: the sibling trial of grant g
  (slot t+1) and the leader trial of grant g' (slot t+1) reuse one FEP and, when the PRBs/ports agree (exactly the trap-prone compatible case), one chest and one LLR set.
- Do not turn computation sharing into evidence sharing: in the compatible case the two trials are one computation attributed to two hypotheses (BC1). Share the computation, keep the credit per (hypothesis, grant), and
  classify such pairs as ambiguous (item 1) so their correlated outcomes never reach the fast path.
- G4 batching: the k0 siblings of one grant are a natural bundle. GrantTrial{main, probes[]} (plan G4) with K = 1 + n_sib entries on different slots; the batch builder groups by (slot, signature) so each slot's LLRs are computed once, then
  one batched LDPC launch. The bundle schedule is state-independent (every sibling on every bundle grant), so it satisfies the fairness rule and is the right shape for 2.2.
- Cost per sibling trial [DERIVED from the F1 PDTIM table, 106 PRB, 1 RX, rank 1, CB0 probe, "after" column]: fep 72 + chest 77 + alloc 2 + demod 28 + ldpc 278 = ~457 us; with the slot share and chest cache hitting (compatible neighbour already
  decoded) ~310 us. A full TB costs ~ (fep+chest+demod ~180 us) + C x 278 us for a failing hypothesis (all segments decoded at max iterations; C ~ 3 at mid MCS, ~9 at the top MCS: 1-2.7 ms) unless the early-exit variant of 3a is used (~457 us).
  Rank-4 273 PRB 4 RX: ~7.7 ms per probe (fep 0.49 + chest 2.23 + demod 4.43 + ldpc 0.56), dominated by demod/chest: the bundle at rank 4 is a GPU or duty-limited job (n_sib = 2-3 at 200 grants/s is ~3-5 cores of CPU).
- Profile hooks: PDTIM timers exist (nr_pdsch_passive_decode.c:87-96: fep/chest/alloc/demod/ldpc, `ISAC_PDCCH_TIMING=1`). Add before relying on the estimates: per-trial-kind counters (explore/exploit/sibling/bundle), cache-hit rate of the (slot, signature) share, and the DCI-book lookup/classification time.
- Sound? **Yes** (pure compute, no statistical effect, provided credit stays per hypothesis). Evidence label: [CODE-READ] + [DERIVED].

## 5. Fast-path schedule that is fair without forcing round-robin on the whole sweep

Requirements: a wrong hypothesis's trials counted by the fast path must be allocated by a rule that cannot depend on any decode outcome of that hypothesis (A, or the e-process which tolerates predictable rules);
the truth must still accumulate certified passes quickly.

S1. **Stratum-keyed streams.** [DERIVED] Route by an observable: grants in the stratum S (known before the decode, item 1) feed the fast-path stream (round-robin over a fixed permutation of the active set, K hypotheses per grant); all other
grants run the existing exploit/KL schedule. The routing key is a function of DCI observables only, never of outcomes, so both streams are well defined and no stream has to share the exploration slots. Replaces the "every 4th pick" coupling that costs F1's factor 4.
S2. **State-independent duty.** If no stratum is available (observation poor), use the grant counter: counter mod 2 (or any fixed pattern) selects the fast-path stream, with round order fixed per evidence epoch (restart only when the active set changes, as lever C already does). Duty phi = 1/2 gives 2x over the current implicit 1/4 after the first pass.
S3. **K hypotheses per grant in the fast-path stream** (G4 batch / CPU consumer pool): K consecutive entries of the round, each credited only to itself. Time falls ~1/K; per-hypothesis counts are unchanged in structure; compute K x ~0.46 ms.
S4. **Ordering inside the round (soft evidence, priority only).** The levers spec showed ordering is inert for the KL rule because acceptance needs every other hypothesis's bound. For lever C/P acceptance needs only the truth's m* certified passes, so where the truth sits in the round matters (expected 0.5 round
without side information). Soft side information (DM-RS mask plausibility, TDRA prior, TDD legality, cell field book hints) can place likely hypotheses first without changing the fairness of the counts (each hypothesis still exactly one slot per round, order independent of CRC outcomes). Realistic gain is up to ~2x on m* = 2.
S5. **e-process counting (2.3)** as the alternative that lets exploit trials count.
Fairness/soundness: S1-S4 are sound whenever the order and routing are outcome-independent and each hypothesis's counted trials are distinct (hypothesis, grant) decodes.
- Sound? S1/S2/S3/S4 **yes**; S5 **conditional** (stress-arm check). Evidence label: [DERIVED].
- Expected gain over fix A (4 RX, n=375, [DERIVED]): S2 8.8 s -> 4.4 s; S3 with K=4: 1.1 s; S1 only helps if f_S is high (item 1.6); S4 up to 2x on top. S5 ~ 3.7 s (the P/C figure) at K=1.

## 6. Simulator modelling (required before any of this is trusted)

The current engine-linked simulator draws grants i.i.d. and applies a per-grant trap switch (nr_td_sim.cc:~395-460, 449, 457). To represent the proposals it needs:
1. A **slot-indexed grant stream**: per slot the RNTI is scheduled with prob lambda; allocation key (mcs index, PRB count, rv, ports) repeats the previous slot's with prob rho (persistence), else is redrawn; new_tx prob; SNR as an AR(1) process (parameter snr_rho) so shift-stationarity (A4) is a testable knob.
2. **Physical trap**: the sibling decoding offset delta passes grant n iff a grant exists at n + delta, its key equals grant n's key, and the truth passes THAT slot (plus accident eps); this replaces `harq_trap` as the default (any-grant k0 trap) and makes the sibling's pass correlated with the truth's pass on a shifted grant (retx-only trap kept as a negative control).
3. **DCI observation**: each DCI detected with probability 1 - p_miss (optionally lower at low SNR) and a false-accept process for the RNTI with rate p_fa per slot (contents random, i.e. incompatible). Classification uses only observed DCIs; absence = ambiguous.
4. **Deferred feed**: the engine feed for a trial is delayed until the offsets are classified (max offset in grants), with a `certified` flag passed to `feed_attr`/`feed_shared`; fp_trials and ok_unique/ok_geom increment only when certified.
5. Metrics: f_S, certified passes, fast-path acceptances, sibling blocks (fraction of RNTIs that fall back to KL), wrong winners, wrong pins, per-RNTI time, hypotheses per grant K, compute estimate (trials x per-trial cost table of item 4), unidentifiable runs (rho = 1).
6. TDD option: pattern with UL slots to exercise 3c; PDCCH only in DL slots.
7. Oracle realism: keep `--oracle 0` for the speed table and add an arm where the k0 oracle is wrong (F4).

## 7. Ranked recommendation

Ranking criterion: speed gained in the realistic any-grant trap regime x soundness x implementation cost.

1. **Item 1: DCI-adjacency certified-pass stratum (with TDD legality as a free hard add-on, 3c).**
   It is the only mechanism that makes the fast path usable when sibling passes are common (F2: B blocks for any q above ~0.5 %), it is the exact set of k0-informative grants (F3), it needs no p_min or stationarity, and where f_S is small the correct outcome
   (fall back to KL) is explicit. Moderate cost (DCI book, scan watermark, `certified` flag). Do the simulator first (item 6) to measure the f_S needed; label every speed claim with the allocation-persistence parameter.
2. **Item 5 (S2/S3/S1) + 2.3 (e-process) evaluated as an arm: separate fast-path stream with fixed duty and K hypotheses per grant.**
   Recovers the dominant cost of fix A (F1: ~8.8 s -> ~1-4 s) in every traffic regime and is independent of f_S; the e-process arm also tests whether the 4b bound can be made to match the real selection process, which is the hard rule. Low statistical risk (S2/S3), moderate implementation
   (G4-style bundle, CPU consumers) risk for compute at rank 4.
3. **Item 2.2 + 3a exact early exit: adaptive anytime sibling test with confirmation-phase p_min, bundled siblings, plus abort-on-first-failed-CB.**
   Cuts B from 277 x n_sib grants to ~25-40 grants where the leader's rate is high, and cuts the failing-trial LDPC cost at rank 1. Conditional on shift-stationarity (A4), so it ships as a labelled conditional improvement and never replaces the stratum certification; the exact early exit has no statistical condition.
   (Items 3b DM-RS energy and the CB0-pass-rate p_min inversion are priority-only and add nothing in the trap regime; do not schedule.)

### 7.1 Simulator experiments (operator's required comparison)
Fixed settings: oracle 0, table-exercise 0.964, realistic tables (twins 2, crc-false 5.96e-8), cold ranks 0-1 and steady ranks 2-3, >= 2000 acquisitions x 4 RNTIs per cell, 4 RX and 1 RX (separate cells, never merged), any-grant physical trap.
Allocation persistence rho in {0, 0.5, 0.9, 0.99} (0.99 ~ constant allocation), p_miss in {0, 0.01, 0.1}, k0 catalogue {0,1} and {0..3}, TDD pattern {FDD, DDDSU}.

| arm | what it is |
|---|---|
| baseline | KL only (flags off) |
| P/C pre-fix | levers C+P at 399d441112/7e8402c037, any-grant trap (expected to show the wrong winners) |
| A | fp_trials from exploration only |
| A+B | A + sibling guard N_sib = 277 (expected: blocks when q > 0.5 %) |
| A+B+adaptive | A + anytime sibling test (2.2) + confirmation p_min + bundled siblings |
| A+B+S | A + stratum certification (item 1), B only as fallback |
| A+B+S+K | A+B+S + fast-path stream with K = 4/8 hypotheses per grant (S2/S3) |
| A-eproc | e-process counting with exploit trials counted (2.3), stress arm only for bound check |
| best safe | the best combination that meets the hard rule in all cells |

Reported per arm and cell: cold/steady mean and median time, wrong winners, wrong pins, undecidable, f_S, fraction of RNTIs blocked into KL, certified passes, compute per decoded grant (K x trial cost), plus the analytical bound vs measured wrong under the stress arm (`--crc-false 1e-3` and 1e-2). Pass rule: wrong = 0 and pins wrong = 0 everywhere
at base crc-false, measured wrong <= analytical bound under the stress arm, speed gains stated against baseline and A+B at the same (rho, p_miss).

### 7.2 Assumption register
A1 one unicast PDSCH per RNTI per slot; A2 truth in catalogue (k0 >= 2 via the oracle); A3 PDCCH miss/false-accept rates small and measured; A4 shift-stationary truth pass probability over a few slots (only for 2.1/2.2 p_min);
A5 independence of accident passes across distinct (hypothesis, grant) decodes (e-process premise); A6 verified SIB1 TDD pattern for 3c.
