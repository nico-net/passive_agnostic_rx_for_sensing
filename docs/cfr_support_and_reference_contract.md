# CFR support / reconstruction and range–frequency reference contract

Task P11 + P12 (plan `adaptive_RX_pipeline.md` §4 Stage 3, gate G3). Audit + contract document.
No DSP change, no engine refactor, no radio. Every claim below was verified by reading the cited
line in this tree at `merge/adaptive-sensing` (parent `57ec7dccaa`); nothing is carried over from
a brief, a handover or an earlier document without being re-read here.

Scope note: G3's five tests are real Stage 3 implementation work. This document is the CONTRACT they
will later be written against, plus a statement, per clause, of whether the contract is already true
today (with evidence) or is currently violated / undocumented (flagged, not fixed).

---

## Part P11 — support and reconstruction

### P11.0 The five CFR producers

| # | Producer | Source enum | Entry point |
|---|---|---|---|
| 1 | `openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:1054` (`nr_isac_submit_csirs_ls()`) | `NR_ISAC_SRC_CSI_RS` | `nr_isac_submit_cfr_multi()` |
| 2 | `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2275` (blind PDSCH DM-RS) | `NR_ISAC_SRC_PDSCH_DMRS_BLIND` | `nr_isac_submit_cfr_multi()` |
| 3 | `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c:391,419,428,434` | `NR_ISAC_SRC_PDSCH_DATA` | `nr_isac_submit_cfr_multi_branch()` |
| 4 | `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:992` (PUSCH DM-RS) | `NR_ISAC_SRC_PUSCH_DMRS` | `nr_isac_submit_cfr_multi()` |
| 5 | `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_data_aided.c:241` | `NR_ISAC_SRC_PUSCH_DATA` | `nr_isac_submit_cfr_multi()` |
| 6 | `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1459` (PBCH/SSB) | `NR_ISAC_SRC_SSB` | `nr_isac_submit_cfr_multi()` |

(`openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` holds the `ENABLE_ISAC_SENSING=OFF` no-ops; not a producer.)

Only producer 3 has been migrated to the P10a branch-identity entry point
(`nr_isac.cc:355`); the other five reach it through the `NR_ISAC_BRANCH_NONE` wrapper at
`nr_isac.cc:352-354`.

---

### P11 (a) — is `k_abs` really the CRB/Point-A coordinate, not an FFT index?

The contract is stated at `openair1/PHY/NR_UE_ISAC/nr_isac.h:85-86`:

> `k_abs` is always the logical CRB/Point-A carrier-grid coordinate in `[0, carrier->nof_prb*12)`.
> It is never an FFT-buffer index and excludes `first_carrier_offset`.

**Verdict: satisfied in VALUE by all six producers. Not ENFORCED anywhere, and one producer
normalises against the wrong modulus.**

| Producer | Line computing `k_abs` | Verified |
|---|---|---|
| CSI-RS | `csi_rx.c:981` `kk = rb * NR_NB_SC_PER_RB`; the LS buffer it reads is itself written at the Point-A index `kinit_tx = rb*12` (`csi_rx.c:299,313,317`), deliberately separate from `kinit_rx = (first_carrier_offset + rb*12) % ofdm_symbol_size` (`csi_rx.c:297`) | YES — the two coordinates are computed side by side and only the FFT one carries the offset |
| Blind DM-RS | `nr_pdcch_blind_monitor_rt.c:2219` `base_sc = (rb_origin + out.start_rb) * 12`, `rb_origin = is_dci10 ? dci10_rb_base : cfg->bwp_start` (`:1924`), both CRB-referenced | YES |
| PDSCH data-aided | `nr_pdsch_data_aided.c:201` `base_sc = (BWPStart + first_rb) * 12` vs `:203` `start_re = (first_carrier_offset + ...)` — the cleanest separation in the tree; the FFT index is a distinct variable used only to address `rxdataF` | YES |
| PUSCH DM-RS | `nr_pusch_passive_decode.c:947,963` `k_grid = logical_start_sc + j`, with the comment at `:930-945` recording the measured failure (7-PRB grants read untouched memory and returned exactly zero) that produced this separation | YES |
| PUSCH data-aided | `nr_pusch_data_aided.c:204-206` `k_buf[...] = k_grid`, comment: "the FFT rotation is only used above to address `rxdataF` and must not leak into this coordinate" | YES |
| SSB / PBCH | `nr_isac_ssb_axis.c:9-14`, `base_sc = ((ssb_start_subcarrier - k_ssb)/12)*12`, then `(base_sc + i) % ofdm_symbol_size`. `ssb_start_subcarrier` IS Point-A-referenced (`nr_pbch.c:45` and `nr_dl_channel_estimation.c:644` both form the FFT address as `first_carrier_offset + ssb_start_subcarrier`) | VALUE yes; **modulus wrong** — see below |

**Finding P11-A1 (flagged, not fixed).** `nr_isac_ssb_axis.c:11` wraps modulo `ofdm_symbol_size`,
not modulo `nof_prb*12`. The two differ (4096 vs 3276 at 273 PRB), so the function can return a
value outside the declared range `[0, nof_prb*12)` while looking normalised. It is inert for every
in-carrier SSB placement tested here, so this is a latent contract mismatch, not an observed defect.
Fixing it needs `nof_prb` at that call site, i.e. an ABI change — out of scope for an audit.

**Finding P11-A2 (flagged, not fixed).** The contract has no enforcement point. A submission whose
`k_abs` is out of grid is **silently discarded, one RE at a time**, at
`sensing_engine.cc:543` — `const uint32_t k = s.subcarrier[i]; if (k >= subcarriers) continue;` —
with no counter, no log, and no effect on the row's reported occupancy. A producer that gets the
coordinate wrong therefore loses support invisibly and the CPI simply reports a sparser row. This is
the class of failure the PUSCH comment at `nr_pusch_passive_decode.c:936-941` says took a measured
`pw=[0 0 0 0]` over 308628 REs to notice. **Recommended for a future task**: count the drops and
surface the count (see the G3 inventory, test 3).

**Finding P11-A3 (support, not coordinate).** Two producers report DENSE support they did not
measure:

- SSB/PBCH submits 240 contiguous `k_abs` values (`phy_procedures_nr_ue.c:1417-1421`), but
  `nr_pbch_channel_estimation()` builds `dl_ch_estimates` by INTERPOLATING (the `filt16a_*` kernels
  selected at `nr_dl_channel_estimation.c:659-687`) from PBCH DM-RS that occupy one subcarrier in
  four. ~60 REs are measured; 240 are claimed.
- PUSCH DM-RS walks every subcarrier of the allocation (`nr_pusch_passive_decode.c:946`, `j++`),
  reading `ul_ch_estimates`, which `nr_pusch_channel_estimation()` interpolates from comb-2 DM-RS.
  Half the REs are measured; all are claimed.

CSI-RS has a third variant: for `csi_type != 0` the estimator SUMS every CDM contribution in an RB
into the single index `kinit_tx` (`csi_rx.c:313-314`), so the value reported at `k = rb*12` is an
RB-aggregate, not the channel at that subcarrier. The ABI has no way to express "interpolated" or
"aggregated" support, so the observed-support mask cannot today distinguish measured from
reconstructed REs. **Flagged; it is exactly what P11's "measured support" clause is about.**

---

### P11 (b) — "actual OFDM symbol time": what `l_sym` and `slot_frac` mean per producer

**Verdict: `l_sym` is DEAD. It is copied into the engine and never read. The only time coordinate
the pipeline uses is `slot_idx + slot_frac`, and four of six producers pass `slot_frac = 0.0f`.**

Evidence that `l_sym` is unused: `sensing_engine.cc:339` assigns `value->symbol`, and the only
other occurrences of `symbol` in that file are the declaration (`:54`), the reserve (`:215`),
the parameter (`:306`) and the null-guard (`:310`). `consume()` (from `:482`) reads
`s.subcarrier[i]` and never `s.symbol[i]`. `PendingRow` (`:57-80`) has no symbol field.

| Producer | `l_sym` value | `slot_frac` | Measured or assumed? |
|---|---|---|---|
| CSI-RS | `csi_rx.c:989` `loverline0` — the true CSI-RS symbol in the slot | `0.0f` (`:1054`) | symbol is real; the TIME the engine uses is assumed = slot start |
| Blind DM-RS | `nr_pdcch_blind_monitor_rt.c:2255` `dmrs_sym` — the true DM-RS symbol | `0.0f` (`:2275`) | same |
| PDSCH data-aided | `nr_pdsch_data_aided.c:309` `l` — the true data symbol | **computed**: `:414-415` / `:432-433` `centre = 0.5*(sym_id[first]+sym_id[last]) + 0.5`, `frac = centre / NR_SYMBOLS_PER_SLOT` | the ONLY producer supplying real sub-slot time |
| PUSCH DM-RS | `nr_pusch_passive_decode.c:964` `dmrs_sym` | `0.0f` (`:992`) | assumed |
| PUSCH data-aided | `nr_pusch_data_aided.c:205` `l` | `0.0f` (`:241`) | assumed |
| SSB / PBCH | `phy_procedures_nr_ue.c:1419` `relPbchSymb` — **PBCH-RELATIVE (0..2), not the slot symbol index** | `0.0f` (`:1460`) | wrong domain, but harmless today only because nothing reads it |

**Finding P11-B1.** "Actual OFDM symbol time" is currently expressible only through `slot_frac`.
Every producer already knows its symbol index and passes it, but the value is discarded. Each
non-`pdsch_data` row is therefore timestamped at its slot boundary, an error of up to
13/14 of a slot (~464 us at 30 kHz SCS). For the slow-time axis this is a per-row jitter, not a
bias, and it is smaller than the inter-row spacing in every measured regime — but it is an
ASSUMPTION, not a measurement, and the plan clause requires the measurement.

**Finding P11-B2.** Even the one producer that computes `slot_frac` uses a UNIFORM symbol model
(`centre / NR_SYMBOLS_PER_SLOT`, `nr_pdsch_data_aided.c:415`). Symbol 0 of each half-subframe
carries a longer cyclic prefix, so a uniform 1/14-of-a-slot spacing is an approximation of order a
few microseconds. Small, but it belongs in the contract rather than in a reader's assumption.

**Contract (P11-B).** The physical measurement time of a CFR row is
`t = (slot_idx + slot_frac) * slot_duration_s(scs_hz)`, with `slot_idx` the producer's own
monotonic absolute slot and `slot_frac` in `[0,1)` the fraction of the slot at which the row's energy
is centred. `l_sym` is diagnostic only and carries no time. A producer that cannot compute
`slot_frac` declares the row at the slot boundary; it must not be read as a measurement of the
symbol time.

---

### P11 (c) — is `noise_var` per-branch, post-P10a?

**Verdict: NO. P10a's `branch_id` is an identity tag only; it does not reach the noise argument.
Worse, `noise_var` is not even on a COMMON SCALE across producers, yet the engine uses it as an
inverse-variance combining weight.**

The weight is formed at `sensing_engine.cc:541`:
`const float weight = s.noise_variance > 0.0f ? 1.0f / s.noise_variance : 1.0f;`
and applied per RE at `:559-563` as a running weighted mean. What each producer passes:

| Producer | `noise_var` argument | What it actually is |
|---|---|---|
| CSI-RS | `noise_power` (`csi_rx.c:1055`) | `abs(LS - interpolated)` powers summed over **every rx antenna and port**, then divided by `nb_antennas_rx * ports` (`csi_rx.c:426`). An ANTENNA-AVERAGED scalar. |
| Blind DM-RS | `nvar` (`nr_pdcch_blind_monitor_rt.c:2276`) | `nr_pdsch_channel_estimation()`'s return, which is the **mean over antennas** of the per-antenna values: `nvar_acc / nvar_ant_count` at `nr_dl_channel_estimation.c:1486`. |
| PDSCH data-aided | `nvar` (`nr_pdsch_data_aided.c:392` etc.) | same antenna-mean |
| PUSCH DM-RS | **`1.0f` literal** (`nr_pusch_passive_decode.c:995`) | no noise estimate at all |
| PUSCH data-aided | **`1.0f` literal** (`nr_pusch_data_aided.c:241`) | no noise estimate at all |
| SSB / PBCH | **`0.0f` literal** (`phy_procedures_nr_ue.c:1469`) | no estimate; falls into the `weight = 1.0f` branch |

**Finding P11-C1 — the per-branch value ALREADY EXISTS and is thrown away one function call before
the tap.** `nr_dl_chest_nvar_ant[NR_DL_CHEST_MAX_ANT]` (`nr_dl_channel_estimation.c:31`,
thread-local) is populated per antenna at `:1310` and `:1476`. `nr_pdsch_passive_decode.c`
already consumes it per branch (`:1366-1378`, `:2112-2113`). The CFR taps do not: they take the
mean `*nvar` instead. Making branch-specific noise real is therefore a plumbing change, not a new
measurement — but it needs a per-branch `noise_var` on the ABI, which is Stage 3 / P13 work.
This matters materially here: the four X410 receive branches are physically imbalanced
(a recorded 8-15 dB spread between channels), so one antenna-averaged number misweights every
branch.

**Finding P11-C2 — the inverse-variance merge is not physically meaningful across sources.**
`noise_power` (CSI-RS, from `calc_power_csirs` on int16 LS residuals), `nvar`
(`|dl_ls_est - dl_ch|^2` on int16 estimates) and the literals `1.0f` / `0.0f` are in four
different, unrelated scales. Because `weight = 1/noise_var`, a CSI-RS row reporting `noise_power`
in the thousands is weighted ~1e-3 to 1e-4 against a PUSCH row declaring `1.0`, purely from units.
Any CPI that merges sources into one `PendingRow` therefore combines them by an arbitrary ratio.
**Flagged, not fixed — normalising these is a DSP change and is explicitly out of this task's scope.**

**Contract (P11-C).** `noise_var` is, today, an OPTIONAL per-SOURCE scalar in producer-local units,
not a per-branch noise power, and `0.0f` means "not estimated". No consumer may treat it as
comparable across sources or as branch-specific until the ABI carries a per-branch value in a
declared unit.

---

### P11 (d) — is allocation membership tracked per CPI?

**Verdict: SOURCE-class and BRANCH membership are tracked; per-TRANSMISSION (allocation / RNTI /
grant) membership is NOT, and the ABI cannot express it.**

What `PendingRow` records (`sensing_engine.cc:57-80`):
`time_slots`, `raw_slot`, `slot_fraction`, `source_mask`, `dl_source_mask`,
`ul_source_mask`, `dl_branch_mask`, `ul_branch_mask` (P10a), `source_occurrences[]`
(+ DL/UL splits), `first_utc_ns`, `available_antennas`, and the DL/UL CFR + weight planes.

What it propagates (`pipeline_types.h:40-45`): `CfrWindow` carries `row_source_mask` per row,
one window-level `branch_mask`, and `source_occurrences[]`. The comment at
`pipeline_types.h:41-43` is explicit that branch identity is window-level, not per-row, on purpose.
The report emits `branch_mask` and, only when the CPI is unambiguously one branch, `branch_id`
(`report_writer.cc:128-134`).

What is missing: there is no RNTI, no `(start_rb, num_rb)`, no HARQ/TB identity and no grant key
anywhere in the accumulator — and none of the six producers could supply one, because
`nr_isac_submit_cfr_multi_branch()` (`nr_isac.h:102-113`) has no such parameter. The nearest
thing is `align_allocation_families()` (`sensing_engine.cc:789`), whose report block
(`report_writer.cc:178-182`: `families`, `repeated_families`, `aligned_rows`,
`singleton_rows`) counts allocation FAMILIES inferred downstream from the observed support pattern.
That is an inference from the data, not recorded membership from the producer.

**Finding P11-D1.** The plan clause "keep allocation membership for every CPI; one detection may
integrate many transmissions" is NOT satisfied. A detection can today be traced to which SOURCE
TYPES and which BRANCHES contributed, and to an inferred family count, but not to WHICH
transmissions. Adding it is an ABI + accumulator change (Stage 3 / P13), not an audit fix.

---

### P11 (e) — any claim of symbol-level Doppler from one OFDM symbol?

**Verdict: no such claim found.** Searched: every `doppler` occurrence under
`openair1/PHY/NR_UE_TRANSPORT/`, `openair1/SCHED_NR_UE/`, `executables/` (all hits are
`nr_ntn_l1.c`'s satellite-ephemeris Doppler pre-compensation and
`nr_pusch_passive_monitor_rt.c:196`'s reuse of `ue->dl_Doppler_shift` as a frequency offset —
neither is a sensing claim), and every `per-symbol` / `symbol-level` / `single symbol` /
`symbol...doppler` occurrence under `openair1/PHY/NR_UE_ISAC/` and `docs/`. Nothing asserts
Doppler evidence from one symbol.

**Wording risk found (the nearest thing, reported honestly as such, not as a violation):**
`nr_pdsch_data_aided.c:375` — sub-slot grouping "multiplying the effective PRF (and hence the
unambiguous velocity) by the number of groups". PRF multiplication is a UNIFORM-sampling statement.
The rows this code emits are separated by ~36 us within a slot and by a whole scheduling gap between
slots, i.e. strongly non-uniform. The velocity axis the engine actually reports is built from
`row_time_slots` (`sensing_engine.cc:791-796`, `:807-809`), not from an assumed PRF, so the code
is correct and only the comment over-claims. Recommend rewording to "denser, irregular slow-time
sampling" if that comment is ever touched; not worth a commit on its own.

**Contract (P11-E).** One OFDM symbol yields one slow-time sample. Doppler is a property of the
ROW SEQUENCE over a CPI and of nothing shorter. No report field, log line or comment may attribute
a velocity, range-rate or Doppler shift to a single symbol or to a single slot.

---

## Part P12 — range / frequency reference contract

### P12.0 What is already settled and must NOT be re-derived

The "~40 m systematic range-axis bias" is **CLOSED and measured gone** (2026-08-11). Signed
detection-level range error against ground truth on `validation_runs/trl4_final/positive`, rate-gated,
+/-80 m match window: rx1 +0.35 / +0.30 m, rx2 +0.52 / -0.11 m, sd ~2.4 m — sub-metre medians, under
one 3.05 m range bin. The cause was the SCENE, not the receiver: two object reflectivities of
1.0/0.8 at `los_gain_db = 0` made a target equal the direct path and capture the UE's own timing
loop; they are now 0.30/0.24. Do not cite 40 m as a live defect and do not carry the 40 m detection
tolerance it justified. The still-standing rule from that investigation is the one below, and it is
the reason this section exists: **the LOS bin must be evaluated dynamically, per CPI; never assume
`los_bin = 0`, never hardcode an offset.**

The residual note carried forward from that episode — "`range_m` subtracts no LOS reference, while
the downstream tracker expects a differential range; those agree only because the harness puts the
direct path at bin 0" — is the exact question P12 asks, and it is now answered below. It turns out to
be only half true.

---

### P12.1 Is `bistatic_range_m` excess path or absolute?

**Verdict: NEITHER, unconditionally. It is excess path relative to the admitted direct path WHEN
sync correction ran and was accepted, and relative to the receiver's own CIR-window origin
otherwise — and the same JSON field name is used for both.**

The arithmetic, `sensing_engine.cc:807`:

    d.range_m = object.range_bin * detector.axes.range_res_m;

No LOS reference is subtracted here — the formula alone reads as "delay from range-bin 0". Bin 0 is
not a physical origin; it is wherever the receiver's FFT window happens to sit, as established by
the UE's own time-tracking loop.

But the subtraction does happen — earlier, in the CFR domain. `sensing_engine.cc:784` calls

    apply_sync_correction(dl_corrected, report.sync, /*delay_reference_bin=*/0.0, los);

and `sync_correction.cc:378-379` computes

    const double constant = (estimate.sto_applied || estimate.sfo_applied)
                                ? estimate.los_bins - delay_reference_bin : 0.0;

With `delay_reference_bin = 0.0`, the whole window is shifted by `-los_bins`, i.e. **the measured
direct path is moved onto bin 0**. So when the correction is applied, `range_bin * range_res_m` IS
excess path relative to the admitted direct path. When it is not applied, `constant = 0.0`, nothing
moves, and the same expression is a raw window-origin delay.

The three regimes, all emitting the identical field:

| Regime | Condition | Physical meaning of `bistatic_range_m` |
|---|---|---|
| A | `config_.sync_enable` false, or `dl_window.rows < 3` (`sensing_engine.cc:775`) | raw CIR-window-origin delay; `sync.reject_reason` is the EMPTY STRING and `sto.applied` is false |
| B | sync ran, admission failed (`sync_correction.cc:283-286`, or the clock-tracker gate at `:487-489`) | raw CIR-window-origin delay; `sync.reject_reason` set, `sto.applied` false |
| C | sync ran, admitted | excess path relative to the admitted direct path, which is at bin 0 |

**Contract (P12-1).** `detections[].bistatic_range_m` is bistatic EXCESS path — path length via the
target minus the direct transmitter-to-receiver path — **if and only if `sync.sto.applied` or
`sync.sfo.applied` is true in the same report**. Otherwise it is an uncalibrated delay in the
receiver's own time frame, with no defined physical origin, and is comparable only with other
detections in the SAME CPI. The corrections that produced it are `sync.sto`, `sync.sfo` and
`sync.cfo`; their applied flags and `sync.los_bins` are the complete record
(`report_writer.cc:171-177`; UL mirror at `:234-243`).

---

### P12.2 What happens when direct-path admission fails? (G3 test 5)

**Verdict: the diagnostics needed to detect the failure ARE published; the range itself is emitted
with full apparent confidence and nothing on the detection marks it as unreferenced.**

The admission logic:
- Per-row admission, `sync_correction.cc:273-278`: a row is admitted if its CIR peak contrast
  clears a robust median/MAD threshold.
- CPI-level rejection, `:283-286`: if `admitted.size() < max(3, ceil(log2(rows)))`, set
  `reject_reason = "insufficient_statistically_admitted_rows"` and RETURN EARLY, leaving
  `los_bins = 0`, `sto_applied = false`.
- A second gate in the clock tracker, `:487-489`: `sto_applied` is set only if the admitted-row
  count, `los_bins` and `sto_standard_error_bins` are all finite and in range.

What the report carries (`report_writer.cc:171-177`): `sync.rows`, `sync.admitted_rows`,
`sync.los_bins`, `sync.sto.{mean_frac_bin,applied}`, `sync.sfo.{sfo_ppm,applied}`,
`sync.cfo.{cfo_hz,applied}`, `sync.reject_reason`. That is a genuinely complete validity record
at CPI level, and it is more than the brief anticipated — P12's "export correction values and
validity" is largely already satisfied.

What is missing, and it is what G3 test 5 asks for:

**Finding P12-2a.** There is no "invalid reference" STATUS. A consumer must know to read
`sync.sto.applied` and reason about what it implies for `bistatic_range_m`; that implication is
written down nowhere in the code, the schema or any doc before this one. Detections in regimes A and
B are byte-indistinguishable from regime C apart from a boolean four fields away.

**Finding P12-2b.** `reject_reason` is the EMPTY STRING in regime A (sync disabled or fewer than 3
rows — `sensing_engine.cc:775`, the block is skipped entirely and `report.sync` keeps its
default-constructed value with `rows` set at `:774`). An empty `reject_reason` therefore means
either "no rejection occurred" or "no attempt was made", and `los_bins = 0.0` in the second case is
a default, not a measurement. Distinguishing them requires cross-reading `sync.rows` against
`sync.admitted_rows`.

**Finding P12-2c.** Detection emission is unconditional on the reference. `detect_clean()`
(`sensing_engine.cc:796`) runs on `dl_detector_input` whatever `report.sync` says, and the
accept lambda at `:801-810` writes `range_m` for every component with no reference check.

**Recommended (future task, NOT done here):** add one explicit, self-describing field — e.g.
`"range_reference": {"mode": "excess_path_direct_admitted" | "receiver_window_origin", "valid":
bool}` — derived entirely from state the report already computes. Deliberately not done in this
task: it is a schema addition, P16 owns schema versioning, and the alternative reading of the brief
(OMIT `bistatic_range_m` when the reference is invalid) would break every existing consumer and
would silently delete all output from the legitimate `sync_enable = false` single-receiver mode.
See "What was not fixed and why", below.

---

### P12.3 "A reflected dominant path is not automatically LOS"

**Verdict: the code currently assumes it is.**

`sync_correction.cc:236-238` selects the reference as the arg-max of the row-mean CIR power
profile:

    anchor_unsigned = static_cast<uint32_t>(
        std::max_element(coarse.begin(), coarse.end()) - coarse.begin());

and every per-row peak search is then confined to `+/-halfwidth` around that anchor (`:246-249`).
`los_bins` is the intercept of the weighted line fit through those peaks (`:305`). There is no
test that the dominant tap is geometrically consistent with a surveyed transmitter position, no
earliest-arrival preference, and no multipath check. The strongest tap is taken to be the direct
path by construction.

This is the exact failure the 2026-08-11 range-bias investigation hit from the other side: a target
whose reflectivity equalled the direct path captured the UE's timing loop. The same mechanism
applies here one layer up — a strong reflected path would become `los_bins`, and every reported
range would then be excess path relative to a REFLECTOR, silently.

**Finding P12-3.** `estimate_sync()` identifies the direct path as the strongest CIR tap and has
no mechanism to reject a dominant reflection. Two cheap discriminators exist and are unused: the
surveyed `config_.tx_position` / `config_.rx_position` are already available in the same function's
caller (`sensing_engine.cc:781-783` uses them for array steering), and earliest-significant-arrival
is a one-pass alternative to arg-max. **Flagged for Stage 3 / UG3; changing the anchor selection is a
DSP algorithm change and is out of this task's scope.**

**Contract (P12-3).** `sync.los_bins` names the CPI's DOMINANT CIR tap, which the pipeline treats as
the direct path. It is not verified against surveyed geometry. Any claim that a reported range is
referenced to the true LOS is, today, an assumption about the propagation environment, not a
property the receiver established.

---

### P12.4 "Branches need consistent physical definitions, not identical numerical offsets"

**Verdict: NOT YET, and it is structurally blocked upstream of this task — but the failure is a
different one from the one the plan clause anticipates.**

The plan clause guards against giving all branches one shared numerical offset. That specific error
is not present, because **there is no per-branch reference at all**: there is ONE
`SensingEngine` consuming every branch (`nr_isac.h:99-101` states this explicitly for P10a —
"one engine still consumes every branch... Routing a branch to its own engine/detector instance is
P13"), ONE `dl_clock_tracker_` (`sensing_engine.cc:777`), and ONE `report.sync` per CPI. Rows
from different branches are merged into the same `PendingRow` (`sensing_engine.cc:536-564`, which
records a branch MASK precisely because a row may carry several), so a single `los_bins` is fitted
across a mixture of branches whose true direct-path delays and clock states differ.

So the current state is worse than "identical offsets": it is one offset estimated from pooled
multi-branch data and applied to all of them. This cannot be fixed at the reference layer. It needs
per-branch windows and per-branch clock trackers, which is P13 (engine-per-branch), gated behind the
rest of Stage 3.

**Contract (P12-4), to be met at P13.** Each branch establishes its OWN direct-path reference from
its OWN admitted rows, and its own `sto/sfo/cfo` state. The requirement across branches is that
`bistatic_range_m` means the same PHYSICAL quantity — bistatic excess path for that branch's own
Tx-Rx geometry — not that the numbers agree. Two branches at different positions SHOULD report
different excess ranges for the same target; equality would be the bug.

---

## Summary of findings

| ID | Finding | Severity | Disposition |
|---|---|---|---|
| P11-A1 | `nr_isac_ssb_axis.c:11` normalises `k_abs` modulo `ofdm_symbol_size`, not `nof_prb*12` | latent, inert today | flagged; needs an ABI change |
| P11-A2 | out-of-grid `k_abs` silently dropped per-RE at `sensing_engine.cc:543`, no counter | observability | flagged; G3 test 3 |
| P11-A3 | SSB (240 of ~60 measured) and PUSCH DM-RS (comb-1 of comb-2) report interpolated REs as measured support; CSI-RS `csi_type!=0` reports an RB-aggregate at one `k_abs` | real | flagged; ABI cannot express it |
| P11-B1 | `l_sym` carried to the engine and never read; 4 of 6 producers pass `slot_frac = 0.0f` | contract gap | flagged; contract stated |
| P11-B2 | `slot_frac` uses a uniform 1/14-slot symbol model (long CP ignored) | microsecond-scale | documented |
| P11-C1 | per-antenna `nr_dl_chest_nvar_ant[]` exists and is discarded; taps pass the antenna MEAN | real, branch-relevant | flagged; needs per-branch ABI (P13) |
| P11-C2 | `noise_var` units differ per producer yet drive a `1/sigma^2` merge weight | real, DSP | flagged; out of scope |
| P11-D1 | no allocation/RNTI/grant membership anywhere; ABI cannot express it | contract gap | flagged; P13 |
| P11-E | no symbol-level Doppler claim found; one PRF wording over-claim at `nr_pdsch_data_aided.c:375` | wording | noted |
| P12-2a | no explicit "invalid reference" status; meaning of `bistatic_range_m` hinges on `sync.sto.applied` | real | contract stated; field recommended |
| P12-2b | empty `reject_reason` conflates "not rejected" with "not attempted" | real | flagged |
| P12-2c | detections emitted with a confident range regardless of reference validity | real (G3 test 5) | flagged; fix is a schema change |
| P12-3 | direct path = arg-max CIR tap; a dominant reflection is accepted as LOS | real | flagged; DSP change, out of scope |
| P12-4 | one engine, one clock tracker, one `los_bins` pooled across branches | structural | blocked on P13 |

---

## G3 test inventory

Not execution — a checklist for a future task. "Covered" means an existing offline test asserts the
property; "partial" means a test exercises the machinery but not the property.

| # | G3 test | Covered today? | Evidence / gap | Offline-testable? |
|---|---|---|---|---|
| 1 | Same CFR at different worker delays yields identical physical measurement times and detections | **PARTIAL** | The invariant holds STRUCTURALLY: every physical time in the report derives from `row_time_slots` = producer `slot_idx + slot_frac` (`sensing_engine.cc:752-753`, `:769-772`, `:791-793`), never from wall clock; the deferred PDSCH path publishes the producer's monotonic slot via `nr_isac_abs_slot_override` (`nr_pdsch_data_aided.c:369-371`). `test_causal_cpi_pipeline()` (`python_parity_test.cc:436`) exercises close/drain/restart ordering and asserts zero drops, but never varies submission delay and never compares two runs. **Gap: no A/B.** One residual violation: `start_utc_ns` (`sensing_engine.cc:335`, `:695`, `:750`) is stamped at `submit()` — on the CONSUMER thread for deferred paths — so it is a PROCESSING time reported as if it were acquisition. P16 requires acquisition-derived times. | YES — drive `SensingEngine::submit()` twice with identical inputs, once with injected jitter, and diff the JSONL with `start_utc_ns` masked |
| 2 | SFN wrap, reorder, duplicate allocation, RNTI reassignment, epoch reset | **PARTIAL** | Wrap and reorder ARE implemented: `unwrap_submission_slot()` (`sensing_engine.cc:399-412`) uses a signed delta folded into `+/-cycle/2` with `cycle = slots_per_frame*1024`, which handles both. Duplicate/co-timed submissions merge by design into one `PendingRow` keyed on `row_key(absolute_slot, fraction)` (`:498`). Late rows are counted as `stale_` (`:500-502`). **Not covered: no test crosses a wrap, no test reorders, and RNTI reassignment / epoch reset are invisible to this layer entirely** — the ABI carries no RNTI (finding P11-D1) and no acquisition epoch. | Wrap/reorder/duplicate: YES, via `submit()`. RNTI/epoch: NO — nothing to assert against until the ABI carries them |
| 3 | Sweep known allocation offsets/sizes and pilot patterns; verify frequency axis, support masks, symbol timestamps | **NO** | Nothing sweeps producer-side support. `test_causal_cpi_pipeline()` uses a single fixed `k[i]=i` pattern. Two properties are untestable as written: the support mask silently loses out-of-grid REs (P11-A2) and cannot distinguish measured from interpolated REs (P11-A3); and there is no symbol timestamp to verify (P11-B1 — `l_sym` is dead and `slot_frac` is 0 for 4 of 6 producers). | YES for the frequency axis and support mask (submit known `k_abs` sets and read back `observed` / `observed_re_count`). Symbol timestamps: blocked on P11-B1 |
| 4 | Known common timing/CFO/SFO perturbations yield the expected corrected excess range/rate and covariance | **NO** | `cuda_sync_test.cc:76-101` calls `estimate_sync()` and `apply_sync_correction()`, but it is a CPU-vs-CUDA PARITY test — it asserts the two implementations agree, never that either recovers a KNOWN injected impairment. `sync_correction_cuda_benchmark.cc` and `detector_cuda_benchmark.cc` are timing harnesses. **No estimator-accuracy test exists.** Note: the `selftest` / `selftest_los` synthetic-injection mechanism described in older project history **does not exist in this tree** (grepped `openair1/` for `selftest`: zero hits) — the sensing pipeline was rewritten since. | YES, and the vehicle already exists: `python_parity_test.cc` constructs synthetic `CfrWindow`s and drives the public API directly. Build an analytic window with a known delay/CFO/SFO and assert `los_bins`/`sfo_ppm`/`cfo_hz` recovery plus the corrected range. This is the single highest-value missing test |
| 5 | Fail direct-path admission deliberately; require invalid reference status rather than a confident range | **NO — and the property does not hold today** | Findings P12-2a/b/c. Admission failure sets `reject_reason` and leaves `sto_applied = false` (`sync_correction.cc:283-286`, `:487-489`), but `detect_clean()` runs unconditionally (`sensing_engine.cc:796`) and `range_m` is written for every component (`:807`) with no reference flag on the detection. There is no "invalid reference" status to assert. | YES — starving admission is easy (submit a noise-only window, or fewer than `max(3, ceil(log2(rows)))` admissible rows) and the report is already JSONL. But the test cannot PASS until the status field of P12-2a exists |

**G3 coverage: 0 of 5 covered, 2 of 5 partial (tests 1 and 2), 3 of 5 not covered (tests 3, 4, 5).**
Test 5 additionally requires a schema addition before it can pass at all. **G3 remains NOT PASSED.**

---

## What was not fixed, and why

Nothing in this tree was changed. Three candidates were considered and each was rejected as not
trivially safe:

1. **Omit `bistatic_range_m` when the reference is invalid** (the brief's own suggested example).
   Rejected: regime A (`sync_enable = false`) is a legitimate, widely-used configuration — every
   `tests/sensing_sim` scene conf runs it — and omitting the field there would delete all detection
   output from those runs. It is also a behaviour change to the report schema, which P16 owns. The
   right shape is an ADDITIVE status field, not an omission, and that still belongs to P16.

2. **Add a `range_reference` status field now.** Rejected: additive and cheap, but it is a schema
   change without the version bump P16 specifies, and its effect on the downstream consumer cannot
   be verified from this task (no radio, and the replay path cannot start the sensing engine).
   Asserting compatibility without running the deserializer would be exactly the kind of inherited,
   unmeasured claim this project has been burned by.

3. **Count the silently dropped out-of-grid REs** (P11-A2). Rejected as not trivial: it touches
   `sensing_engine.{h,cc}` and the report, needs a rebuild plus a manifest regeneration under the
   one-pair rule, and needs its own test. Genuinely worth doing — as part of G3 test 3, where the
   assertion that consumes the counter also gets written.

Delivering the audit and the G3 inventory alone is the correct outcome for this task.
