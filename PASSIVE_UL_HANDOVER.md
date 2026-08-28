# Passive UPLINK reception — handover

Branch `total-passive-rx-UL-DL`, tree `/home/sens/NICOLA/openairinterface5g-total-passive-ue`
on **sens6**. Head at time of writing: `b9a4a262d8`.

Read `/home/sens/NICOLA/PASSIVE_RX_ONLY_HANDOVER.md` §§1–4 first for the rig rules (R1–R9), the
environment and the gNB-log bracketing method. This document covers only the uplink axis.

---

## 1. Headline: it works, and here is the number

**76.0 % transport-block CRC over 57 554 grants.** A receiver that never attached, never
transmitted and was never granted anything decodes another UE's uplink end to end: blind DCI 0_1
recovery → grant field extraction → FFT window placement → channel estimation → equalisation →
descrambling → rate recovery → LDPC.

Conditions, all of which matter:

| | |
|---|---|
| capture | `captures/mcs10_145223`, 150 s |
| traffic | **uplink-only** UDP iperf 50 Mbps |
| gNB | `pusch: min_ue_mcs = max_ue_mcs = 10` |
| receiver | `--ue-rxgain 25`, 4 RX, 273 PRB |
| result | `try=57554 crc_ok=43768 (76.0%) seg_fail=13786 ta_refined=54370` |

On a **bidirectional** cell the same configuration reads **0 %**. That is not a regression — it is
UCI-on-PUSCH, and §4.1 is the work to fix it.

---

## 2. What is implemented and validated

| capability | state | evidence |
|---|---|---|
| Blind DCI 0_1 recovery | validated | field-for-field against the gNB log over a byte-bracketed window: gNB `prb=[3,10) symb=[0,14) tbs=560` vs ours `prb=3+7 sym=0+14 tbs=560`, same shapes in the same proportions |
| Slot / k2 mapping | validated | gNB PDCCH slot 15 → PUSCH slot 19; ours land on 19 |
| Passive PUSCH decode | **76.0 %** | above |
| Per-grant timing refinement | validated | med 0, p90 3–6, 99 % within ±6 samples |
| UL DM-RS CFR → CPI grid | working | `ul_cfr[submits=57554 re=180996852]`, `occ[... pusch=N]` |
| Deferred UL decode queue | working | `PUSCHQ queued=57637 decoded=57554 dropped[full=0 stale=80]` |
| Deferred PDCCH scan queue | working | `scanq queued=45278 done=44999 drop_full=278` |
| DL decode scratch reuse | working | build-verified; microbenchmark 1469 µs → 75 µs |
| UCI-on-PUSCH reservation | **built but INERT and incomplete** | §4.1 |

### 2.1 The three fixes that took the uplink from 0 % to 76 %

Any one of them missing gives exactly 0 %, so all three are load-bearing.

1. **Use the gNB's UL FEP, not the UE's.** `nr_symbol_fep_ul()` (bare DFT) **followed by**
   `apply_nr_rotation_symbol_RX()` with `symbol_rotation[link_type_ul]` and **`N_RB_UL`** — i.e.
   `nr_ofdm_demod_and_rx_rotation()` (`slot_fep_nr.c:219`), which is what `ulsim` feeds
   `phy_procedures_gNB_uespec_RX()` and what decodes. **The RU's `nr_fep()` is NOT the reference** —
   it applies no rotation only because the RU rotates elsewhere.
   *Sign trap:* `nr_slot_fep()` **adds** `sample_offset` (`:85`); `nr_symbol_fep_ul()` **subtracts**
   it (`:158`). Same argument name, inverted meaning.
2. **Re-place the FFT window per grant from the measured `est_delay`.** A fixed advance cannot work:
   the residual walks a sawtooth −387 → +433 samples over ~2 s (~3.3 ppm free-running clock drift)
   while `nr_pusch_channel_estimation()` absorbs only ±`MAX_DELAY_COMP` (20). Correction is
   `ta_new = ta − est_delay`, one retry, no search.
3. **Operating point.** `--ue-rxgain 25`. See §3.2 — this is not a free parameter.

---

## 3. Facts about this rig that cost time to learn

### 3.1 Run validity — score every capture before reading it

Roughly **half of all captures are CFO mis-locks**, and a mis-lock is not partial: SIB1, DL PDSCH
and UL PUSCH all read 0.0 % simultaneously while PBCH stays perfect. It is indistinguishable from a
code regression, and one such run was nearly read as "the deferred queue broke the receiver".

Use `captures/run_arm.sh`:

```
ARM=<name> CONF=<path> DUR=200 TRIES=5 [RXG=25] [TASWEEP=start:step:count] [ULPROBE=1] \
  /home/sens/NICOLA/captures/run_arm.sh
```

It restarts `usrp-hwd`, brackets the gNB log, scores each run and retries until VALID, writing
`$OUT/verdict.txt` and `captures/<ARM>_LAST_VALID`. Verdicts: `VOID_NO_SIB1`, **`VOID_DL_ZERO`**
(`LDPCDIAG ok=0` — the downlink is the canary, it is unrelated to any uplink change and dies with
the lock), `VOID_NO_CPI`, `VALID`. **Never read an uplink number from a run whose downlink is dead.**

`CFOAPPLY=1` arms the CFO retune and is **default off on purpose**: delivery is `nrue_ru_reinit()`,
a full device teardown, and on this X410 that can hit a stale MPM claim after which the process dies
with `rpc::timeout` on `rfdc_set_nco_freq` — measured 5/5 VOID with it armed against ~1-in-2
without. The retry loop is the safe way past a mis-lock.

### 3.2 RX gain is a narrow window, and the obvious reasoning about it is wrong

- **The uplink is 20–26 dB STRONGER than the downlink here** (`ULPROBE ul_minus_dl_dB =
  [25.8 21.1 21.6 19.5]`) — the UE is far closer to the X410 than the O-RU is. The natural
  assumption, that a downlink-set gain starves the uplink, is backwards. Measure it.
- **rxgain 40 compresses the uplink.** Free-MCS ladder at 40: MCS 1 93.8 %, MCS 2 44.4 %,
  MCS 3 6.5 %, MCS 4+ 0 %. Dropping to 25 moved the ceiling from ~MCS 3 to ~MCS 9.
- **Below ~21 the X410 AMP stage drops out** (it steps in 7 dB): rxgain 15 takes ANTPOW from ~126
  to ~5, i.e. the noise floor. There is no usable point below ~21.
- **25 is the sweet spot found.** It is an operating point, not a fix.

### 3.3 The gNB moves — resolve it every session

`pgrep -a gnb` on **sens4** and read its `-c` argument. Both the config path and the log path have
changed mid-session (`/home/sens/gnb.yaml` → `/home/sens/OCUDU/configs/gnb-4x4-r1-ulmcs4.yaml`,
logging to a different file). **A stale log stops growing and reads exactly like "no traffic"** —
`stat -c%s` twice a few seconds apart before concluding a cell is idle. The C-RNTI changes on every
re-attach. `log.all_level: info` suppresses the per-field PDCCH dump that DCI-size derivations need;
`debug` is required for those.

---

## 4. Next steps, in priority order

### 4.1 UCI-on-PUSCH de-interleaving — the one thing between 76 % and a loaded cell

**Status: a partial implementation exists and is both inert and insufficient. Do not trust it.**
`pdcch_blind_monitor_ul_uci = "max_trials[:beta_idx[:alpha_idx]]"`, currently `"4:6:0"`.

Two independent defects, both measured:

1. **It never takes effect.** `nr_rx_pusch_group_tp()` does not read the `unav_res` handed to it —
   it computes its own from PTRS alone (`nr_ulsch_demodulation.c:711-722`, zero on this cell) and
   **overwrites the caller's value at `:948`**. Measured consequence: `uci[trials=115338 rescued=0]`
   — every trial decoded identical bits to the untouched attempt.
2. **Shrinking `G` was never sufficient anyway.** The UE does not append the HARQ-ACK after the
   ULSCH, it **interleaves** it. `nr_ulsch_ue.c` builds a per-coded-bit template
   (`uci_on_pusch_bit_type_t`, `:47`) marking each bit position ULSCH / ACK / CSI and maps the ULSCH
   around the reserved positions. So the LLR stream has UCI scattered through it at spec-defined
   positions, and correcting only the length leaves every ULSCH bit after the first ACK position
   misaligned.

**What to build.** Mirror the transmitter's mapping and de-multiplex:

- Reference, in-tree and exact — read these rather than TS 38.212 directly, so the transmitter being
  inverted and the receiver cannot drift apart:
  `nr_ulsch_ue.c:689 initialize_mapping_resources()`, `:748 skip_mapping_current_uci()`,
  `:775` the mapping loop, `:860 map_overlapped_ack()`, and `:600 calc_rate_match_info_uci()`.
- Reconstruct the same `uci_on_pusch_bit_type_t` template over the `G` coded-bit positions, then
  **compact the LLR array to the ULSCH-marked positions** before `nr_ulsch_decoding()`. The already-
  written `passive_ul_unav_res()` gives the reservation size (`Q'_ACK`, TS 38.212 6.3.2.4.1.1) and
  is believed correct — it mirrors `calc_rate_match_info_uci()` — but it has never been exercised
  against a real grant, so verify it before building on it.
- `unav_res` cannot be used as the hook. Either patch `nr_ulsch_demodulation.c` to honour an
  incoming value, or set `G` through a path the callee does not overwrite.

**Parameters, read off this gNB's own debug log (do not assume):**

```
oack=7  ocsi1=0  alpha=0.5  betas=[6.25, 1.125, 1.125]     <- large ACK
oack=1  ocsi1=0  alpha=0.5  betas=[20,   1.125, 1.125]     <- small ACK
```

- `ocsi1=0` — **no CSI on PUSCH on this cell.** HARQ-ACK only. That bounds the work considerably.
- `beta` **varies with O_ACK** (TS 38.213 has separate betaOffset indices for O_ACK ≤ 2, 3–11, > 11).
  6.25 is table index 6; 20 is index 11. The current single-index config is wrong for the
  O_ACK ≤ 2 case, though that case punctures rather than rate-matching so it does not reserve.
- `O_ACK ≤ 2` **punctures** the ULSCH instead of rate-matching around it (TS 38.212 6.2.7) — `G` is
  unchanged and only a handful of REs are corrupted, which LDPC should survive.

**`O_ACK` is not derivable, and that is structural.** The UL DAI carries `(V_T_DAI − 1) mod 4`, so
the DCI pins `O_ACK` only *modulo 4*. An attached receiver resolves the rest from its own downlink
assignment history; a passive one would have to reconstruct that UE's whole HARQ codebook state,
which fails silently the moment a DL assignment was missed. The existing design tries the candidates
consistent with the DAI (`dai+1+4t`) and lets the 24-bit TB CRC decide — sound, since the CRC is what
decides acceptance anyway and a false accept over a handful of trials is ~n/2²⁴. `out->dai` is
already populated (`nr_pdcch_blind_monitor.c:2165`).

**Cost to watch.** Each candidate re-runs the whole receive chain. In the inert run that was 3.5
extra passes per grant and the queue went from `stale=80` to `stale=7721` (19 %) with `max_lag`
63 → 208. Budget for it: raise consumers, or gate the search on grants likely to carry UCI.

**How to test it.** Bidirectional iperf, `min_ue_mcs = max_ue_mcs = 10` pinned so margin stays out
of the picture, rxgain 25. Success is `uci[... rescued=N]` non-zero and CRC recovering toward the
76 % measured uplink-only. UCI presence scales with **downlink** load: 98.3 % bidirectional,
32.7 % uplink-only, so the DL rate is the knob that controls the difficulty.

### 4.2 The ~20 dB to the gNB

MCS 25 (64QAM R≈0.8) is out of reach; the gNB decodes the same grants at 25–31 dB SINR with 1–2 LDPC
iterations. Unexplored, in rough order of expected value:

- **Are the four branches actually being combined?** Never verified. If the chain is effectively
  single-branch there is up to 6 dB sitting there.
- **Branch a1 is 15 dB down** and its connection is mechanically intermittent (it has moved between
  −24 and −15 dB across sessions without being touched). A dead branch in an MRC sum with a shared
  noise estimate actively hurts — the downlink already has an `ISAC_RX_BRANCH_MIN_DB` exclusion for
  exactly this; the uplink has none.
- Antenna placement: the UE is close, so this may simply be geometry.

### 4.3 Smaller, known open items

- **UL and DL rows share a CPI but not a geometry.** UL is UE→target→receiver, a different bistatic
  ellipse from the DL rows beside it, and they are currently fused as if they shared an illuminator.
  **Do not read a range or velocity off a mixed CPI as a target parameter until this is settled.**
  Design decision, not a bug.
- `data_submits=0` in the blind-monitor summary is a **counter gap**, not zero submissions — the
  deferred queue calls `nr_isac_pdsch_data_aided_submit()` without incrementing the counter owned by
  `nr_pdcch_blind_monitor_rt.c:1646`.
- `PUSCHQ max_lag_slots=N/M` prints the margin as the second number, unlike the DL queue which
  prints `slots_per_frame`. Confusing; unify.

---

## 5. Retracted — do NOT re-derive these

Each was stated with confidence during this work and then disproved by measurement. They are listed
so the next agent does not spend a capture rediscovering them. **§7 is the companion list**: causes
that were tested and RULED OUT, which is a different thing from a claim that was wrong.

| claim | why it was wrong |
|---|---|
| "Passive PUSCH decodes at 100 % CRC" | The accept test was `rc == 0` — the LDPC *interface* return, not a CRC. Real rate was 0 %. An exactly-100.000 % pass rate on a live link is an instrument fault, not a result. |
| "UCI is not the cause — 72 non-UCI grants, 0 decoded" | Measured while the FEP was still wrong, so nothing could decode. The test had no power. |
| "`snr=0.0` means the link is at 0 dB / ~25 dB deficit" | Instrument gap. `ulsch_noise_power` comes from `gNB->measurements.n0_subband_power`, which a real gNB fills from a separate noise procedure the minimal context never runs. **Do not trust `out->snr_db` from this path.** |
| "`rho = 0.96` proves the DM-RS is correct" | The estimator interpolates across subcarriers, so `rho` largely measures the interpolation filter; a pure delay is a phase ramp that preserves it. It read 0.96 throughout, including while completely broken. |
| "The gNB applies no rotation to the uplink, so remove it" | Reasoned from the RU's `nr_fep()`. `nr_ofdm_demod_and_rx_rotation()` is the reference, and rotation is **required**. Removing it kept CRC at 0. |
| "Timing is excluded — sweeps found nothing" | The sweeps were run while the rotation was wrong, so every offset failed. Timing was in fact a real defect (§2.1 item 2). |
| "The receiver cannot adapt to changing MCS" | Predicts the *rare* MCS values failing on state left by the dominant one; the data showed the opposite (MCS 25: 0/48046 while MCS 6: 2/2, 7: 3/5, 9: 1/5), and pinning to one MCS gives 76 %. |
| "RX gain is set for the DL and starves the UL" | Backwards. The uplink is 20–26 dB *stronger* here. |
| "ADC compression explains the bidirectional 0 %" | Refuted: received power triples with DL data, but the **downlink decoded 40 418 blocks in the same capture**. A compressed converter breaks both directions. |
| `CAPTURE_OPTIMIZATION_PLAN.md` "items 2–4 not implemented" | All three are implemented and live. That document is superseded by §2 here. |

---

## 6. Config and probes

Working config: `/home/sens/NICOLA/nrue.passive_rx.ul.q.conf`. Uplink-relevant keys:

```
pdcch_blind_monitor_dci01     = "1:43"          // scan:length_override (43 live-verified @273 PRB)
pdcch_blind_monitor_ul_bwp    = "0:273"
pdcch_blind_monitor_ul_tda    = "0:14:4,0:12:4,0:10:4,4:10:4"
pdcch_blind_monitor_ul_dmrs   = "0:2:1"         // config_type:add_pos:max_length
pdcch_blind_monitor_ul_misc   = "0:0:-1:-1:2"   // tp:mcs_table:data_scid:dmrs_scid:pci  (-1 = use PCI)
pdcch_blind_monitor_ul_dci_bits = "0:0:0:1:4:2:0:0:1:2:2:0:0:0:0:1"
pdcch_blind_monitor_ul_pusch  = "1:1:0"         // decode:max_per_slot:ta_offset (0 = derive N_TA_offset)
pdcch_blind_monitor_ul_uci    = "4:6:0"         // INERT, see 4.1
pdcch_blind_monitor_ul_thread = "2:32:2"        // consumers:depth:first_core
pdcch_blind_monitor_scan_thread = "1:8:8"
```

`pdcch_blind_monitor_ul_pusch` field 1 also accepts **`2` = CFR-only** (channel estimate + CFR, skip
LLR/LDPC): the DM-RS CFR is 29 µs of a 1065 µs grant and is all the sensing pipeline consumes.

Core map on this 12-core host: `--thread-pool 0,1,4,5,6,7`; DL PDSCH consumers 9–11, UL consumers
2–3, PDCCH scan 8. (The DL pool previously asked for 5 consumers from core 9, i.e. 9–13, two of
which do not exist.)

Probes, all default off:

| env | prints | use |
|---|---|---|
| `ISAC_PUSCH_DIAG=1` | `PUSCHDIAG` per grant: prb/sym/mcs/rv/ta/tbs/G/Qm/delay/seg/status | the workhorse; ~17 k lines per 200 s run |
| `ISAC_PUSCH_TIMING=1` | `UTIM` per-stage cost + over-slot histogram | RT budget |
| `ISAC_UL_TA_SWEEP=start:step:count` | `TASWEEP ta:crc_ok/try` | sweeps the timing advance **within one capture**, round-robin per grant — every value sees the same channel, traffic and lock, which a sequence of runs cannot guarantee on this rig |
| `ISAC_UL_PROBE=1` | `ULPROBE` slot map + per-branch DL vs UL power | the only way to compare the two directions' power |
| `ISAC_PDCCH_TIMING=1` | `BTIM` DL scan cost | RT budget |

Analysis scripts written for this work live in the session scratchpad, not in-tree: the UCI join
(gNB `uci_t=` against `PUSCHDIAG` by `(SFN, slot)`, discarding keys ambiguous across SFN wraps) and
`allocbench.c` (in-tree at `tests/passive_rx/allocbench.c`).

---

## 7. Excluded hypotheses — what the failures were NOT

Distinct from §5, which lists claims that were *wrong*. These are causes that were **tested and
ruled out**, with the evidence. Two of them were ruled out *prematurely* and had to be reopened;
they are marked, because the reason they were false exclusions generalises.

### 7.1 For "UL PUSCH CRC = 0 %" (the state before §2.1's three fixes)

| hypothesis | verdict | evidence |
|---|---|---|
| Grant parameters wrong (PRB, symbols, MCS, TBS, G) | **excluded** | field-for-field against the gNB log over a byte-bracketed window: gNB `prb=[3,10) symb=[0,14) tbs=560`, ours `prb=3+7 sym=0+14 tbs=560`, same shapes in the same proportions (3585/38 vs 5169/64). `G = rb·12·11·Qm` to the bit. |
| Slot / k2 mapping wrong | **excluded** | gNB PDCCH at slot 15 → PUSCH at slot 19 (k2=4); ours land on slot 19. |
| DM-RS sequence, position or scrambling identity wrong | **excluded** | the channel estimate's per-antenna power tracks the *independently known* antenna imbalance (a1 ~15–24 dB down). Noise would be flat across branches. |
| Data/DM-RS scrambling IDs wrong | **excluded** | both resolve to the PCI, matching the gNB's `nid_pusch=2`, `pusch_dmrs_scrambling_id=2`. |
| LBRM (`tbSizeLbrmBytes` never set → 0) | **excluded** | `Tbslbrm == 0` gives `Ncb = N` (`nr_rate_matching.c:620`), and at `rv=0` the start index `k0 = 0` either way. Computing it properly gives `Nref = 239623 > N`, so `Ncb = N` regardless — no difference. |
| Demodulator producing no / empty LLRs | **excluded** | `llr_n=5544`, `llr_mean=672–1112`, `llr_active=0.93–0.99`. A full field of confident soft bits. |
| Multi-layer / precoding misinterpretation | **excluded** | `nrOfLayers=1` from the gNB; antenna-ports code point 2 → 2 CDM groups, port 0, matching the gNB's `num_dmrs_cdm_grps_no_data=2 dmrs_ports=1`. The DL antenna-port table is a *different* table — that trap was checked and not fallen into. |
| Thread-pool deadlock in the hand-built gNB | **excluded** | `pushTpool` with `len_thr==0` runs inline and the barrier is `num_workers+1`. The real cause of the original hang was `param_v4.numSpatialStreamIndices = 0` → `num_jobs = 0` → `join_task_ans` waiting forever. |
| **FFT window timing** | ~~excluded~~ → **REOPENED, and it was a real defect** | Swept −800…+2400 samples (~11 CPs), 366 grants/point, 0 CRC everywhere → looked conclusive. **It was not**: the sweep ran while the *rotation* was still wrong, so every offset failed for an unrelated reason. Timing turned out to be defect 2 of 3 (§2.1). **Lesson: a sweep over parameter A proves nothing while parameter B is broken.** |
| **UCI on PUSCH** | ~~excluded~~ → **REOPENED** | "72 non-UCI grants, 0 decoded" was measured on the broken FEP, where *nothing* could decode. The test had zero power. UCI is now the main open item (§4.1). |

### 7.2 For "bidirectional traffic reads 0 % where uplink-only reads 76 %"

| hypothesis | verdict | evidence |
|---|---|---|
| ADC compression from the higher total power | **excluded** | received power triples when the DL carries data, but the **downlink PDSCH decoded 40 418 blocks in the same capture**. A compressed converter breaks both directions, not one. |
| Link margin / MCS | **excluded** | MCS was pinned to 10 in both runs, and 10 gives 76 % uplink-only. |
| Timing regression | **excluded** | same capture: `delay` med 0, p90 3, 99 % within ±6. |
| Receiver cannot adapt to a changing MCS | **excluded** | predicts the *rare* MCS values failing on state left by the dominant one; the data show the opposite (MCS 25: 0/48046 while MCS 6: 2/2, 7: 3/5, 9: 1/5), and pinning to a single MCS gives 76 % rather than exposing stale state. |
| CSI multiplexed on PUSCH | **excluded** | the gNB reports `ocsi1=0` on every sampled grant. HARQ-ACK only — which usefully bounds §4.1. |
| Queue starvation (`stale=7721`, 19 %) | **contributing, not causal** | it degrades throughput, but 19 % dropped cannot turn 76 % into 0 %. Caused by the inert UCI search running 3.5 extra receive-chain passes per grant. |
| Consequence | **UCI on PUSCH is what is left** | 98.3 % of grants carry it bidirectional vs 32.7 % uplink-only. |

### 7.3 Also excluded, elsewhere in the session

| hypothesis | verdict | evidence |
|---|---|---|
| "The deferred UL queue broke the receiver" | **excluded** | the capture was a CFO mis-lock: `dl_ldpc_ok=0` too, and the queue does not touch the downlink. This is exactly why every run now carries a verdict (§3.1). |
| "My code broke SIB1 / the DL" (after a batch of VOID runs) | **excluded** | all five consumers of the changed config struct were confirmed rebuilt; the cause was first a stale X410 MPM claim, then a gNB reconfiguration, then a stale log path. |
| "RX gain set for the DL starves the UL" | **excluded, and inverted** | the uplink is 20–26 dB *stronger* (`ul_minus_dl_dB = [25.8 21.1 21.6 19.5]`). |
| "The `~40 m` range bias is still open" | **closed** | measured sub-metre in `validation_runs/trl4_final/positive`; do not carry the 40 m figure or the tolerance it justified. (Pre-existing, not from this work.) |

---

## 8. Findings — things learned that generalise beyond this branch

Measurements, each of which changed what was built next.

**F1. The gNB's uplink FEP and the UE's downlink FEP are not interchangeable, and the argument
names lie.** `nr_slot_fep()` *adds* `sample_offset`; `nr_symbol_fep_ul()` *subtracts* it. Same name,
inverted meaning. Separately, `nr_symbol_fep()` rotates with `N_RB_DL` while the uplink path uses
`N_RB_UL`. The authority for "what does `nr_rx_pusch_group_tp()` expect" is
`nr_ofdm_demod_and_rx_rotation()` (`slot_fep_nr.c:219`) — because that is what `ulsim` feeds it and
`ulsim` decodes. The RU's `nr_fep()` looks like a reference and is not: it omits rotation only
because the RU rotates elsewhere.

**F2. A free-running receiver's timing residual is a sawtooth, not a constant.** Pinned at the
derived `N_TA_offset`, the measured residual walks −387 → +433 samples over ~2 s and wraps: ~400
samples/s, ≈3.3 ppm. `nr_pusch_channel_estimation()` absorbs only ±20. So a *fixed* timing advance
decodes only when the ramp happens to cross zero — which is exactly what the first three successes
looked like (`est_delay` 8, 8, 13, against 58 failures scattered across the full ±430). This is the
project's own design premise (no shared clock reference) showing up as a decode failure.

**F3. Sweep inside one capture, not one value per run.** A run here costs ~4 minutes and mis-locks
~half the time, so a 17-point sweep as separate runs is two hours and a dozen VOID results. As a
round-robin (`ISAC_UL_TA_SWEEP`), it is hundreds of grants per value in a single capture — and every
value sees the *same* channel, traffic and lock, which a sequence of runs cannot guarantee on a rig
that swings this much. The same trick applies to any parameter the receiver can vary per grant.

**F4. Concentration is the signal, not magnitude.** The first non-zero result was 3 decodes out of
1424 — but all three at `ta=1600` and none at the eight other offsets. A false CRC accept scatters
uniformly; concentration at the one physically correct value is what made 0.2 % a result rather than
noise.

**F5. The uplink can be far stronger than the downlink at a passive receiver.** Measured
`ul_minus_dl_dB = [25.8 21.1 21.6 19.5]` — the UE is much closer than the O-RU. Every intuition
built on "the gNB transmits harder" is inverted, including the natural guess that a downlink-set
AGC starves the uplink. And there is a real tension: the receiver needs the *weak* downlink to sync
and the *strong* uplink to decode, on one gain.

**F6. `crc_ok` is not decoder health, and `rc == 0` is not a CRC.** `nr_ulsch_decoding()` returns
the LDPC *interface* status; the verdict is `processedSegments == C` (`nr_ulsch_decoding.c:289`).
Report `health = ok/(ok+seg_fail)` separately from `crc_ok/try`, which also carries the empty-TB
rate. An exactly-100.000 % pass rate is an instrument fault.

**F7. Diagnostics can measure themselves.** Two probes written during this work reported their own
initialisation order rather than the receiver: an LLR probe that read `out->G` before it was
assigned (reported an empty LLR field on every grant), and a channel-estimate coherence metric that
largely measured the estimator's own interpolation filter (read 0.96 throughout, including while
completely broken). Before trusting a probe, ask what it reads when the thing it measures is absent.

**F8. UCI on PUSCH scales with *downlink* load.** 98.3 % of grants bidirectional, 32.7 % uplink-only,
0.7 % on the small BSR-sized grants. It is the HARQ-ACK for downlink assignments, so the DL rate is
the knob that sets uplink decode difficulty — a counter-intuitive coupling worth remembering when
designing a capture.

**F9. Allocation cost is page-fault cost.** `malloc16_clear()` is `memalign` + `memset`, and above
glibc's mmap threshold every allocation returns fresh pages the memset faults in one at a time.
Measured on the real shapes: 1469 µs to allocate+clear+free ~4 MB versus 75 µs to clear the same
buffers reused — 19.6×. `allocCast2D/3D` are *designed* to persist (`CheckArrAllocated` allocates
only when the handle is null); declaring the handle as a local defeats exactly that.

**F10. A capture with the wrong population cannot answer the question.** Two runs were spent on
captures containing no grant of the type under test — one with zero small grants, one whose entire
population was MCS 25. Check the population is present *before* reading a rate.

---

## 9. Commands

All paths on **sens6** unless stated. Nothing here needs to be run as a script; they are the exact
forms used to produce every number in this document.

### 9.1 Build

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make -j8 nr-uesoftmodem
```

**Never build while a capture is running** — 8 compile jobs starve the receive thread. Check first:

```bash
pgrep -c nr-uesoftmodem      # must be 0
```

`SIMULATION/TOOLS/sensing_channel.c` and `radio/USRP/usrp_lib.cpp` are dlopen'd plugins; editing
them needs `make rfsimulator` / `make oai_usrpdevif` explicitly or the old code keeps running.

### 9.2 Run a capture

```bash
ARM=<name> CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf DUR=200 TRIES=5 RXG=25 \
  setsid nohup /home/sens/NICOLA/captures/run_arm.sh > /tmp/arm_<name>.log 2>&1 < /dev/null &
```

Optional: `TASWEEP=1560:10:9` (timing sweep), `ULPROBE=1` (DL/UL branch power), `CFOAPPLY=1`
(arms the retune — see §3.1 before using it).

Wait for it, then find the valid run:

```bash
until grep -q "ARM <name> DONE" /tmp/arm_<name>.log; do sleep 30; done
D=$(cat /home/sens/NICOLA/captures/<name>_LAST_VALID)   # empty if every try was VOID
for d in /home/sens/NICOLA/captures/<name>_*/; do echo -n "$(basename $d) "; cat $d/verdict.txt; done
```

**Do not `pkill -f run_arm.sh`** — the pattern matches your own ssh command and kills the session.
Kill by PID: `ps -eo pid,args | grep ARM=<name>`.

### 9.3 Read the result

```bash
L=$D/run.log

# headline
grep -a 'pusch_passive\[' $L | tail -1

# decode rate by MCS -- the single most informative view
for m in $(seq 0 27); do
  T=$(grep -a PUSCHDIAG $L | grep -c "mcs=$m/")
  K=$(grep -a PUSCHDIAG $L | grep "mcs=$m/" | grep -c 'status=0')
  [ "$T" -gt 0 ] && echo "mcs=$m tried=$T ok=$K"
done

# timing residual after per-grant refinement (want med~0, p90<=6)
grep -a PUSCHDIAG $L | grep -oE 'delay=-?[0-9]+' | cut -d= -f2 | sort -n \
  | awk '{a[NR]=$1; v=($1<0?-$1:$1); if(v<=6)c++} END {print "n="NR," med="a[int(NR/2)]," p90="a[int(NR*0.9)]," within6="int(100*c/NR)"%"}'

# per-branch power: DL slots vs UL slots (needs ULPROBE=1)
grep -a ULPROBE $L | tail -1

# per-branch power, whole buffer / UL channel estimate
grep -a ANTPOW   $L | tail -1
grep -a ULBRANCH $L | tail -1

# queues and RT budget
grep -a PUSCHQ $L | tail -1
grep -a 'BTIM occ_total' $L | tail -1
grep -a 'SENSING: UTIM ' $L | tail -1

# timing sweep census (needs TASWEEP)
grep -a TASWEEP $L | tail -1

# grant shapes actually present -- check the population BEFORE reading a rate (F10)
grep -a PUSCHDIAG $L | grep -oE 'prb=[0-9]+\+[0-9]+' | sort | uniq -c | sort -rn | head
```

### 9.4 gNB ground truth (sens4)

Resolve the live config and log every session — both move (§3.3):

```bash
ssh sens4 'pgrep -a gnb'                       # read the -c argument
ssh sens4 'C=$(pgrep -a gnb | head -1 | sed "s/.*-c //"); grep -vE "^\s*#" $C \
  | grep -iE "dl_arfcn|band:|channel_bandwidth|common_scs|pci:|nof_antennas|max_rank|mcs_table|max_ue_mcs|dl_ul_tx_period|nof_dl_slots|nof_ul_slots|all_level|filename"'
```

Confirm it is live, not a stale file:

```bash
ssh sens4 'L=<logpath>; S=$(stat -c%s $L); sleep 5; echo growth=$(( $(stat -c%s $L) - S ))'
```

Current C-RNTI, and per-grant uplink truth:

```bash
ssh sens4 'tail -c 400000 <logpath> | grep -aoE "PUSCH: rnti=0x[0-9a-f]+" | sort | uniq -c | sort -rn | head -3'

# per-grant records WITH the slot, which lives in the log prefix, not the PUSCH: text
ssh sens4 'tail -c 3000000 <logpath> \
  | grep -aoE "\[ *[0-9]+\.[0-9]+\] PUSCH: rnti=0x4603 [^|]{0,200}" > /tmp/gnb_w.txt; wc -l < /tmp/gnb_w.txt'

# UCI fraction, overall and by grant shape
ssh sens4 "awk '{u=(/uci_t=0\.0us/)?0:1; n++; k+=u} END {printf \"n=%d uci=%.1f%%\n\", n, 100*k/n}' /tmp/gnb_w.txt"
ssh sens4 "grep -a 'prb=\[0, 7)' /tmp/gnb_w.txt | awk '{u=(/uci_t=0\.0us/)?0:1; n++; k+=u} END {printf \"n=%d uci=%.1f%%\n\", n, 100*k/n}'"
```

The UCI *parameters* (`oack`, `ocsi1`, `alpha`, `betas`) are in the indented block after each
`PUSCH:` line and need `log.all_level: debug`:

```bash
ssh sens4 'tail -c 200000 <logpath> | grep -a -m2 -A32 "PUSCH: rnti=0x" \
  | grep -aiE "uci|ack|csi|beta|alpha"'
```

To bracket a capture, `run_arm.sh` already writes `$OUT/bracket.txt` (gNB log byte offsets before
and after). Read that window with `dd if=<logpath> bs=1M skip=$((START/1048576)) count=$(((END-START)/1048576 + 10))`.

### 9.5 X410

```bash
# stale MPM claim -- "Thwarted attempt ... invalid token" or rpc::timeout in a run log
ssh sens6 'ssh root@128.178.122.3 "systemctl restart usrp-hwd"'   # run_arm.sh does this per try

# full reboot (recovers a claim that survives a service restart)
ssh sens6 'ssh root@128.178.122.3 "nohup reboot &"'
# then wait for usrp-hwd to go active again, and give it a few minutes to settle --
# ANTPOW reads ~5 (noise floor) for a while after boot and recovers on its own
ssh sens6 'timeout 90 uhd_usrp_probe --args "type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3" 2>&1 | grep -iE "X400-Series|invalid token|timeout"'

# data NIC must survive: MTU 9000 and rings 8192 (NOT reboot-persistent on the host)
ssh sens6 'IF=$(ip -o addr show | grep -E "192\.168\.20\." | awk "{print \$2}" | head -1); \
  ip -o link show $IF | grep -oE "mtu [0-9]+"; ethtool -g $IF | sed -n "/Current/,\$p" | grep -E "RX:|TX:"'
```

The system Python UHD module is version-mismatched against this device's MPM (expects 5.3, device
has 6.1) — `uhd_usrp_probe` works, `python3 -c "import uhd"` does not.

### 9.6 Offline unit tests

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make -j8 test_nr_pdcch_blind_monitor && ./test_nr_pdcch_blind_monitor
```
