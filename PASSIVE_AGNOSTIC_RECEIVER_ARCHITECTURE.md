# Passive Agnostic NR Receiver — Architecture Report

*2026-09-12 · branch `adaptive-rx-UL-DL` · host sens6 · radio USRP X410 · reference cell n78, 3450 MHz, 273 PRB, 30 kHz SCS, PCI 2.*
*Every number in this document comes from a logged run or a unit test. Sections marked **not built** describe design, not code.*

---

## 1. Purpose, and the rule it obeys

The receiver sits on the air interface of a 5G NR cell as a third party. It has no SIM, no RRC connection and no side channel to the base station. Its job is to recover the physical-layer traffic of the cell's own users — downlink PDSCH and uplink PUSCH — well enough to (a) measure the radio channel on every reference and data resource element, for sensing, and (b) prove, transport block by transport block, that its picture of the cell is correct.

"Agnostic" is a discipline. Every parameter the receiver needs falls into exactly one of four bins, and §9 keeps the ledger:

| Bin | Meaning |
|---|---|
| **Decoded** | Read from a broadcast channel (SSB, MIB, SIB1); true by construction. |
| **Searched** | Unknown a priori; a bounded hypothesis set is scored by the only oracle the receiver has, the transport-block CRC. |
| **Measured** | Estimated directly from the signal (a correlation, a coherence, an energy ratio) with a margin, and confirmed against the CRC where possible. |
| **Assumed** | Taken from the spec default. Every assumption must have a detector that fires when it is wrong. |

**The one rule.** A hypothesis is never *scored* by how plausible it looks; plausibility may only *reject* what the standard forbids. Ranking happens on CRC outcomes alone. Every estimator added this year is reject-only for the same reason: a search that prefers likely-looking answers silently biases the receiver toward the network it was developed on.

---

## 2. The pipeline at a glance

Radio front end (X410, or a raw IQ recording replayed identically) → **Acquisition** (SSB search, MIB, SIB1, carrier verification) → **Control-channel discovery** (CORESET, DCI length, RNTIs, field layout) → two data pipelines in parallel, **downlink** (DCI 1_x → PDSCH → CRC) and **uplink** (DCI 0_x → PUSCH → CRC) → outputs to the **sensing engine** (channel grid, range-Doppler) and to **verdicts and state** (acquisition tracker, confirmed/mismatch lines).

The transport-block CRC flows back from both data pipelines into every hypothesis search. Nothing downstream of acquisition is configured by hand.

---

## 3. Front end and acquisition

**Samples.** 122.88 Msample/s complex baseband from the X410 (up to four antennas) or, through the same code, from a validated raw recording replayed at real time. The file path additionally allows fault injection (a deliberate gap in the stream) so recovery can be tested without a radio. Continuity of the sample clock is checked on every read: a timestamp jump larger than a slot is a stream loss, not noise, and invalidates synchronisation immediately.

**SSB search — implemented, verified.** The receiver walks the GSCN raster inside its 100 MHz window, correlating each candidate against the PSS/SSS sequences and keeping the strongest peak; it is not told where the SSB is. Verified this session by starting the radio 360 kHz below the carrier: the search found the same cell (GSCN 7783) at grid subcarrier 162 instead of 150, with a 101 dB correlation peak.

**MIB / PBCH lock.** PBCH decode yields the frame number, the common subcarrier spacing, the SSB subcarrier offset, the DM-RS type-A position and the CORESET#0 / search-space-0 configuration. The tracker records the lock as an event; on this cell the receiver locks twice per short capture as it re-syncs on the second SSB burst.

**SIB1.** Decoded through CORESET#0 exactly as a UE would, but its contents are *published* to the passive side rather than used to attach: initial DL and UL BWP, common PDSCH and PUSCH time-domain allocation lists, k2, TDD pattern, offset to Point A. Each later feeds a search as a fact, never as a hypothesis.

**Carrier check — implemented, verified.** The receiver is started with a bandwidth, numerology and RF centre it cannot yet know are right. After SIB1 it checks them: bandwidth and spacing exactly, and — from the SSB's *measured* grid position, the MIB's subcarrier offset and SIB1's offset to Point A — whether Point A lands on the first subcarrier of the started grid and the carrier ends on its last. A match turns "started with" into "confirmed from the air". A mismatch names the cell's true centre as a retune target; the off-centre run reported *cell centre 3450.000000 MHz, receiver at 3449.640000 MHz*.

**Timing and CFO.** Once locked, a timing loop tracks the sample offset frame by frame and a frequency loop tracks residual CFO digitally; the local oscillator is never retuned during a run (retuning the X410 mid-stream has killed the radio before). A CFO estimate that settles far from zero is a mis-lock; the capture harness aborts and restarts such runs rather than measuring on them.

---

## 4. Acquisition state and recovery

An explicit state machine reports where the receiver is: SEARCHING → PBCH_LOCKED → SIB1_DECODED → PDCCH_LOCKED → CORESET_VERIFIED → CELL_CONFIGURED → DL/UL_CONVERGED → TRACKING, plus LOST. The state is the highest piece of evidence currently held, not a strict chain: on this cell SIB1 lands before any PDCCH discovery, so a run legitimately skips states. Forward moves apply immediately — a decoded winner is a fact — while demotion by weakening evidence needs eight consecutive regressed observations, so a transient dip during relearning is not read as loss.

Two kinds of input drive it: polled discovery evidence (DCI length found, CORESET verified, UL BWP known, search winners), read at the blind-PDCCH monitor's summary cadence; and event edges (PBCH applied, SIB1 published, stream discontinuity), raised where those things happen. The events exist because on a four-second capture the monitor runs zero occasions while MIB and SIB1 both decode; a poll-only tracker reported nothing until it was tested.

**Recovery, as built.**

- *Stream loss.* A timestamp discontinuity clears synchronisation, timing state and the frame mapping, drops the tracker straight to LOST with no hysteresis (a discontinuity is proof, not noise) and re-enters SSB acquisition. SIB1 facts survive; the PBCH lock does not. Tested by injecting 50 ms, 3 s and 10 s gaps into a saved capture at several positions: every run detected the gap, declared LOST, re-locked and re-entered TRACKING, with uplink decode continuing at 86 % afterwards. A no-gap control never declares LOST.
- *Stale evidence.* Every search context carries a generation. Feedback from a block decoded under an earlier generation, a different identity or a different DCI length is refused and counted, so a re-attached user or a relearned layout cannot pollute the new search with old outcomes.
- *Local relearning.* A settled downlink interpretation is reopened when a sustained run of CRC failures contradicts its own learned confidence bound; only that context is reset. This is a health trigger, not an assertion that the cell changed.
- *Mis-lock watchdog.* The harness treats a CFO that converges away from zero as a void run and retries; roughly half of cold starts on this rig are mis-locks, and a mis-lock reads as 0 % on every stream at once.

**Not built:** recovery from a *configuration change* (a BWP or CORESET change while tracking). Today it would surface as a sustained CRC collapse followed by a relearn, with no explicit detection.

---

## 5. Finding the control channel without being told

A normal UE is told its dedicated CORESET, search space, DCI sizes and field layout over an encrypted RRC channel. The passive receiver recovers each by search, ordered so that every search needs only what the previous one established.

| Stage | Method | Status on this cell |
|---|---|---|
| CORESET#0 | Derived from the MIB alone; configured automatically. | Every run. |
| Dedicated CORESET (Technique A) | Correlate received PDCCH DM-RS against the cell's known sequence across candidate resource groups and symbols; hit-driven termination. | Converges to the exact known CORESET. |
| DCI length (Technique C) | Polar-decode candidate payload lengths on every occasion; lock the length whose CRC recovers plausible, repeating RNTIs. Independent instance for the uplink. | DL 47 bits; UL 43 or 45 depending on the user. |
| RNTIs (Technique B) | Recover C-RNTIs from the CRC mask; admit by persistence into a 16-entry table with weakest-evidence eviction. | Cross-checked against the base-station log: receiver right, operator's remembered RNTI stale. |
| DL field layout | Enumerate the width families that fit the length (BWP-indicator width × TDA-index width, other widths fixed); prefer, per grant, the family whose interpretation context has already passed the CRC; rotate only while no family has evidence. | See below. |
| PDSCH interpretation (Technique D) | Rotate a bounded catalogue of legal (start, length, DM-RS pattern, MCS table) combinations across grants; score by TB CRC per family, identity and observed TDA index; declare a winner when every hypothesis has enough trials and the best clearly separates. | Settles on 13-symbol allocations, DM-RS at symbols 2/7/11, 256QAM table. |

**The downlink's problem on air, and its fix.** Several families fit one DCI length, and until this session the receiver rotated among them per grant. The family that decodes received about one grant in fourteen, so its interpretation search never reached its trial minimum; cumulative DL CRC read 0.3 %, 15 % or 26 % across runs depending only on that share. Rank 2, two CDM groups, limited-buffer rate matching and link margin were each tested and refuted first (§6). With the evidence-led preference the family was chosen after eight passes, Technique D converged, and DL CRC after convergence was 82 %.

---

## 6. The downlink data pipeline, one grant at a time

An accepted DCI becomes a job carrying the recovered allocation and the current hypothesis. The job is queued to a consumer thread; nothing heavier than the DCI scan runs on the receive thread, which has 500 µs per slot and loses PBCH lock above roughly 18 % duty.

1. **Front-end processing.** The consumer takes the slot's samples, applies the current timing and frequency correction, and transforms the allocated symbols to the frequency domain.
2. **Channel estimation.** The PDSCH DM-RS is regenerated from the cell identity and slot/symbol, least-squares estimated on the comb pilots, and interpolated across the allocation. Two measurements are taken on the raw pilots first: the even/odd pair coherence, ~1 for a single-layer transmission and collapsed when a second port shares the comb; and the energy on the other comb, full when it carries data and empty when reserved for a second CDM group. On air both read what the decoder assumes: coherence 1.00 on 56,000 grants, other comb carrying data.
3. **Equalisation, demodulation, descrambling.** Soft bits are formed under the hypothesis's modulation order and descrambled with the identity's RNTI and the data-scrambling identity.
4. **Transport block size and LDPC.** The TB size follows from the allocation, code rate, DM-RS overhead and xOverhead; code blocks are LDPC-decoded and the CRC checked. The verdict is fed back to Technique D and to the layout-family preference.
5. **Verdicts from a passing block.** A CRC-OK block is evidence, not just data: it confirms the data-scrambling identity (any other would have failed); it refutes every xOverhead value whose TB size for that allocation differs from the one used (all three alternatives, after a single decode, on this cell); and its DM-RS enters the blind identity estimate below.
6. **Sensing hand-off.** A verified block is re-encoded and remodulated, and the channel is measured at every data resource element as received divided by reconstructed — the data-aided channel frequency response that gives the sensing engine its densest rows.

**Blind DM-RS scrambling identity — measured.** This dedicated parameter had been assumed equal to the physical cell identity. For each of the 1024 candidates the pilot sequence is regenerated, a least-squares estimate formed on the comb, and the score is the coherence between adjacent pilots — two subcarriers apart, over which any real channel is highly correlated. The true identity sums coherently; a wrong one rotates each term by a pseudo-random phase. Numerator and denominator accumulate across grants as complex and real sums so wrong candidates random-walk down as evidence grows (a first version accumulated per-grant magnitudes and its margin could never improve; a unit test caught it). A decision needs sixteen CRC-verified grants and ten decibels over the median candidate. On this cell: identity 2, twenty decibels over the median and fifteen over the runner-up, on replay and on air. A mismatch is logged naming both identities — a detector that did not previously exist.

---

## 7. The uplink data pipeline

The uplink is harder in one specific way: a user's DCI 0_1 field widths depend on dedicated configuration (HARQ process count, DAI, antenna-port table, SRS, CBG, PTRS, beta offsets, DM-RS sequence-initialisation flag) that the receiver cannot read, and there is no compact family to enumerate. The uplink therefore has a three-component discovery chain on a shared search engine.

**Component 1 — length.** The format 0_1 length by the same CRC-oracle sweep as the downlink, on independent state.

**Component 2 — field widths.** All width vectors that sum to the locked length are generated, then reduced by the standard's own constraints (each reduction is a TS 38.212 field definition, never a heuristic — at length 45 the set falls from 4145 to 1480). Vectors the CRC oracle could never distinguish are merged into classes on a *frozen* set of observed payloads — frozen because live traffic supplies endless novel payloads, and an unfrozen set kept splitting classes and restarting the search indefinitely. Each grant is decoded under one class and the PUSCH CRC is fed back to it. A class that fails a necessary condition — it cannot even interpret the observed payloads — is retired after enough opportunities, since the true layout must interpret every real DCI; without that rule one such class blocked convergence forever. Contexts are per identity: two users sharing a DCI length are not assumed to share a configuration.

**Component 3 — interpretation (designed, never exercised).** The uplink analogue of Technique D: dedicated TDA table contents, DM-RS configuration, MCS table, transform precoding, scored the same way. It arms only when the width winner's CRC confidence bound falls below 60 %; on this cell the winner decodes at 70–90 %, so Component 3 has never armed on any capture.

**PUSCH decode — implemented, 71–91 % on air.** The uplink uses the base-station receive chain, not the UE's: a bare DFT followed by the uplink phase rotation, which is what actually decodes. The FFT window is re-placed per grant from the measured delay, because the free-running clock walks the residual over hundreds of samples in seconds. Channel estimation uses the PUSCH DM-RS, whose scrambling identity is estimated blind exactly as on the downlink (identity 2, seventeen decibels). UCI carried on PUSCH punctures data; its footprint is recovered online from CRC evidence rather than configured. LDPC and CRC close the loop; the verdict returns to Component 2 as feedback to the class that produced the grant, tagged with identity, DCI length and generation.

**The shared search engine.** Both uplink components run on one hypothesis-search engine with four stages: admissible generation; equivalence collapsing; a plausibility gate that may only reject; and a class-scored oracle. The oracle allocates trials with an anytime confidence bound — it explores every class, spends more on the current leader, skips classes whose upper bound has fallen below the leader's lower bound, and declares a winner either when one class clears a 60 % lower bound and separates from every rival, or when every surviving class has its minimum trials and the best beats the runner-up by a fixed ratio. Selection order is reshuffled once per pass; a reshuffle checked mid-pass silently skipped the true class and returned "nothing selectable" — the first of this session's bugs found only by running the tests.

---

## 8. What the sensing engine receives

Every stage that measures a channel submits it to one per-subcarrier grid: SSB DM-RS, blind PDSCH DM-RS, data-aided PDSCH, PUSCH DM-RS, data-aided PUSCH. Rows are stamped with their absolute slot and merged by source. A coherent processing interval closes on row count; clutter is removed by mean subtraction; a range transform runs across subcarriers and a Doppler transform across rows; a two-dimensional constant-false-alarm detector yields detections with range, velocity and SNR, published as reports. The receiver's role in that system is to be a sensing node that needs no cooperation from the network — which is why every parameter above has to be recovered rather than configured.

---

## 9. Ledger — where each parameter comes from

| Bin | Parameters | Count |
|---|---|---|
| Decoded from broadcast | PCI · SSB position · CFO/timing · SFN · common SCS · DM-RS type-A position · CORESET#0/SS#0 · DL and UL initial BWP · common PDSCH and PUSCH TDA lists · k2 · TDD · Point A · carrier bandwidth and numerology (verified against the started grid) | 14 |
| Searched by CRC oracle | dedicated CORESET · DL DCI length · C-RNTIs · DL layout family · PDSCH TDA/DM-RS/table · UL DCI length · UL field widths · UCI-on-PUSCH footprint · xOverhead (reject-only) · DL and UL data-scrambling identities (confirmed by CRC) | 11 |
| Measured from the signal | PDSCH DM-RS scrambling identity · PUSCH DM-RS scrambling identity · transmission rank · DM-RS CDM-group count | 4 |
| Designed, never exercised | UL TDA semantics · UL DM-RS configuration · UL MCS table · transform precoding (Component 3) | 4 |
| Still assumed | initial RF tune (the scan works inside the window; the band walk does not exist) · CSI-RS resources | 2 |

Twenty-nine of thirty-five parameters are decoded, searched or measured with a verdict on this cell (≈ 83 %). DM-RS type 2 / double-symbol DM-RS and DFT-s-OFDM are observable but *refused* rather than estimated; rank above one is measured but not decodable. Those are capability limits, not information limits.

---

## 10. Evidence

| What | Where | Result |
|---|---|---|
| Native unit tests, whole suite | offline | 90 / 91 (one upstream simulator test, untouched code) |
| Receiver-specific test targets | offline | 13 / 13 green |
| Manual decoder oracle (bit-identical controls) | saved IQ | 45 / 45, 0 failed |
| Broadcast acquisition on 4 s captures | saved IQ | pass on all three; 2 s is below the floor |
| Reacquisition after injected gaps (50 ms, 3 s, 10 s) | saved IQ | all pass; control never declares LOST |
| Six off-air verdicts | saved IQ and air | all confirmed, margins reproduced |
| Agnostic OTA, two users, automatic SSB search | X410 | VALID; UL 91 %, DL 82 % after convergence |
| Off-centre start (−360 kHz) | X410 | SSB found at subcarrier 162; carrier MISMATCH names 3450.000000 MHz |

Three defects in this session's own work were found only because the corresponding test was run: a mid-pass reshuffle, a tracker that could not see a stream loss because all its inputs were latched, and two commits whose content landed in the wrong place because the tested tree and the committed tree were not the same. Each is now a test, a rule, or both.

---

## 11. What is not built

- **Cold-start band walk.** The SSB search works within the current 100 MHz window; stepping the radio across a band and retuning onto the carrier the SIB1 check derives is a control loop that does not exist, and a replay cannot exercise it.
- **Downlink width sweep.** The downlink chooses among enumerated families; it does not generate them from constraints as the uplink does. A cell whose 1_1 widths fall outside the enumerated family would fail with the same signature as this session's DL problem.
- **Component 3 on real data.** Needs a cell where the width winner decodes poorly, or an explicit run-anyway mode for validation.
- **Configuration-change recovery.** Explicit detection of a BWP or CORESET change, rather than relearn-after-collapse.
- **Capability limits.** Multi-layer decode, DM-RS type 2, double-symbol DM-RS, DFT-s-OFDM uplink.
- **CSI-RS discovery.** Blind detection of periodic CSI-RS resources by correlation, as the SSB is found today.
- **Repeatability and the second user.** One valid air run per configuration so far; the rig's own rule is five. The second user has not been scheduled by the base station during this session's runs, so its downlink convergence is unproven.
- **Derived thresholds.** The loss hysteresis, the DM-RS decision gate, the layout-preference count and the early-separation bound are hand-set; none has a derivation.
