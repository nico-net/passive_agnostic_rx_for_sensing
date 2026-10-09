# CSI-FULL: the complete per-population CSI-RS set for PDSCH rate matching (csd/csi-full, 2026-10-10)

Branch `csd/csi-full`, based on `csd/otatest` bc227efdd8. Host: DGX GB10 (aarch64), with its GPU. Every build, unit-test run, offline capture scan and replay went through `heavy.sh`, one at a time. No radio, no capture, no hardware tool and no sudo were used.

Evidence labels follow PROJECT_MEMORY §0.1. The two captures are always reported separately: `raw_pci415_12s_b` (CAP_TICK 208029025) is called **b** below, and `raw_pci415_12s_20261008` (CAP_TICK 210036277) is called **8**.

## TL;DR

**Root cause of class A.** The PDSCH of the TRS-occasion slots never had its rate-matching set. There were four reasons:

1. **Export capped at 4.** The export to the decoder was capped at `NFAPI_MAX_NUM_CSI_RATEMATCH = 4` resources per slot. One TRS occasion carries 24 resources.
2. **Wrong union.** The capped union was the first four in bank order, mixed across cells.
3. **Never applied after solving.** The solver latched `rm=0`, so post-solve grants never applied any CSI-RS at all. [MEASURED: every OTA ab_G2/ab_B2 DECODETDRA line at abs%160 ∈ {15,16,20,21} has `csi=0`, 720 grants.]
4. **Second burst refused.** Discovery refused the second TRS burst of a resource (`discovery_union_seed`: at most 2 adjacent phases, "never clones"). So 415's 20/21 burst never existed, even though WINNER lines for it were produced live.

**The rule that decodes.** [MEASURED, paired CSIFULLAB on identical samples]
- A cell's PDSCH rate-matches around **its own TRS and the co-channel neighbour's TRS in the same 52-RB block**.
- It does **not** rate-match around the other blocks' TRS.
- For population 415, class-A grants (any SNR) decode as follows:
  - no CSI-RS: 0/34;
  - own scrambling ID only: 0/34;
  - every resource in the slot (the old union): 8/34;
  - **own + co-block: 26/34**.
- No grant decodes under "none" and fails under the rule.

**Gate (MCS0-4 at ≥ 10 dB ≥ 98 % for Nl = 1 and 2): NOT demonstrated on the replays.**
- Class A on capture b, rule applied and the block discovered: 8/9 TBs. Without the fix the same grants give 2/9.
- Class A in whole flag runs (Nl=1): 4/5, 3/4 and 0/5. In the 0/5 run discovery had not yet confirmed the block when those grants arrived; this is a 12 s replay limit.
- The class B residual is not CSI-RS (section 6).

## 1. What the gNB transmits (capture ground truth)

[MEASURED, offline raw IQ, `tools/csirs_capture_map.py` + `tools/csirs_capture_map_report.py`, both captures, 4 RX, 60 periods of 160 slots] The tool works without any gNB log or configuration:
- It finds the slot grid from the cyclic prefix.
- It aligns slot numbering and `abs_slot % 160` with the TS 38.211 7.4.1.5.2 sequence of the serving TRS. The ratio is 2.98 on b and 2.99 on 8, where noise = 1 and present = 3. On capture 8 the CP fold had slipped two symbols; the TRS search over ±13 symbols fixed it.
- For every slot phase, symbol, comb offset k0 (0..3) and all 1024 scrambling IDs, it computes the per-RB row-1 (density 3) matched-filter ratio.
- The `map` mode adds the per-RE mean power per `slot % 160`, which is used for the ZP and other-row search.

TRS block means (k0 at its best, mean ratio over the block's 52 RB; noise 1.0, full presence 3.0):

| scrambling ID (cell) | RB block | k0 @ l=4,8 | k0 @ l=5,9 | abs%160 15/16 (b / 8) | abs%160 20/21 (b / 8) |
|---|---|---|---|---|---|
| 415 (serving, PCI 415) | 0-51 | 0 | 1 | 2.94 / 2.99 | 2.10-2.47 / 2.65-2.79 |
| 748 | 0-51 | 2 | 3 | 2.53-2.92 / 2.92-2.98 | 2.23-2.51 / 2.72-2.88 |
| 249 | 64-115 | 0 | 1 | 1.88-2.31 / 2.11-2.77 | 1.78-1.93 / 2.22-2.35 |
| 582 | 64-115 | 2 | 3 | 2.32-2.43 / 2.75-2.76 | 1.84-1.95 / 2.39-2.54 |
| 380 | 128-179 | 0 | 1 | 1.48-1.67 / 2.57-2.82 | 1.80-2.01 / 2.81-2.91 |
| 713 | 128-179 | 2 | 3 | ≤1.31 / 2.10-2.28 | 2.33-2.42 / 2.38-2.97 |

What the table shows:
- Every TRS is row 1, density 3, 1 port, noCDM, nrofRBs 52, period 160, at offsets {15,16} and {20,21}.
- Each ID uses the symbol pairs (4,8) and (5,9) with k0 and k0+1, in **both** bursts.
- So the carrier holds 6 IDs × 4 resources × 2 bursts = **48 resource-bursts (24 geometries)**.
- The IDs pair up into two triples: {249, 380, 415} and {582, 713, 748}. 333 = 748−415 = 582−249 = 713−380. PCI mod 3 picks the RB third: 1 → RB 0, 0 → RB 64, 2 → RB 128.
- Reading: [HYPOTHESIS] two sites of three sectors, each sector's TRS in its own RB third, co-channel with the other site's sector.

Other periodic CSI-RS [MEASURED, power map + live `CSIRS_DISC FOOTPRINT` lines]. These are not TRS, and discovery has no hypothesis for them (`HYPOTHESES n=0`):
- abs%160 = 63: l 10-13, subcarriers 4-11 full band. Classes 0-3 drop to 0.25 of median, which looks like ZP or CSI-IM; classes 8-11 are boosted.
- abs%160 = 24: l 10-13, subcarriers {0-3, 8-11} full band.
- abs%160 = 135: l 7, subcarriers 4-7 full band.
- abs%160 = 105: l 10/12/13, c0-3 at 0.8.

Together these are 4 of 160 slots. They are not handled here (section 6).

Per-cell discovered counts (resource-bursts out of the 8 each ID transmits):

| run | 415 | 748 | 249 | 582 | 380 | 713 | total / 48 |
|---|---|---|---|---|---|---|---|
| OTA LIVE ab_G1 (t26g, before) | 4 (15/16 only) | 6 | 4 | 4 | 4 | 6 | 28 |
| OTA LIVE ab_G2 (t26g, before) | 4 (15/16 only) | 6 | 4 | 4 | 5 | 5 | 28 |
| replay b, flag off (`base2_b`) | 4 | 4 | 0 | 4 | 3 | 2 | 17 |
| replay b, `ISAC_CSIRS_FULL_BURSTS=1` (`vab_b2`) | 4 | 6 | 1 | 4 | 4 | 2 | 21 |
| replay 8, flag off (`vbase_81`) | 4 | 5 | 4 | 4 | 5 | 4 | 26 |
| replay 8, `ISAC_CSIRS_FULL_BURSTS=1` (`vab_81`) | 6 | 8 | 4 | 6 | 8 | 8 | 40 |

## 2. Why the exported set was incomplete [CODE-READ + MEASURED]

1. **Export cap.** `nr_csirs_blind_rt_rate_match_all()` is called with `NFAPI_MAX_NUM_CSI_RATEMATCH` (4) by the DCI monitor (`nr_pdcch_blind_monitor_rt.c:8683, 8882`) and the DECODE-FIX arms. A TRS slot carries 24 resources, so the 4 exported were the first 4 in bank order, from any cell.
2. **Solver `rm=0`.** Post-solve grants copy the export only when the solver's per-RNTI `rm` hypothesis is 1. With a wrong union, `rm=1` loses, so the solver latches `rm=0`. [MEASURED] DECODETDRA at TRS slots in OTA ab_G2 and ab_B2 shows `csi=0` on all 720 lines.
3. **Second burst refused.** `discovery_union_seed()` keeps at most two adjacent phases per resource, and the incompatible-phase branch "never clones". 415's 20/21 burst is the same geometry as its 15/16 burst, so it was refused. [MEASURED, ab_G1] WINNERs `id=415 P/O=160/20,21` existed live: 2+2 for fd1 l4, 2+3 for fd1 l8. No 20/21 CONFIRMED line for 415 exists in any OTA or replay log before this change.
4. **Bank cap.** The bank held 32 confirmed resources against 48 resource-bursts.
5. **Wrong population rule.** "Same scrambling ID as the grant", as in `ISAC_PDSCH_CSI_POPULATION_RETRY`, is incomplete: the co-block neighbour's TRS must be included too (section 4).
6. **RB extent.** Legacy `nr_dlsch_csi_overlap_bitmap()` applies every resource to every allocated RB, ignoring the 52-RB extent. QAM-FIX's `ISAC_PDSCH_CSI_RB_EXTENT` was right but had nothing correct to apply. Its segmented-grant case tested the virtual RB, not the physical PRB.

## 3. Fix (each behaviour behind its own flag, all default off)

| flag | what it does | where |
|---|---|---|
| `ISAC_CSIRS_FULL=1` | Decode-time per-population set: `nr_csirs_full_apply()` replaces the grant's CSI-RS list with every confirmed resource in its PDSCH slot that has the grant's DM-RS scrambling ID, or that lies in the RB block (≥ half overlap) of one of that population's confirmed resources. Deduplicated by geometry; resources on DM-RS symbols are dropped. It also forces the per-RB extent (TS 38.211 7.4.1.5.3 / TS 38.214 5.1.4.1). Segmented grants get a virtual→physical PRB map for extent and parity, which replaces the old `UNSUP@seg-csirm-parity` refusal. | `nr_csirs_blind_rt.c` (`full_select`, `nr_csirs_full_rate_match`, `nr_csirs_full_apply`), `nr_dlsch_demodulation.c` (`nr_dlsch_csi_prb_map`, extent switch) |
| `ISAC_CSIRS_FULL_BURSTS=1` | A row-1 resource seen in a second burst becomes its own bank entry and is confirmed independently by the unchanged per-slot confirm path. The discovery bank cap goes from 32 to 64. | `nr_csirs_blind_rt.c` (`add_discovered`), `nr_csirs_blind_search.{c,h}` (`nr_csirs_blind_disc_cap`) |
| `ISAC_CSIRS_FULL_TRS_PAIR=1` | TS 38.214 5.1.6.1.1: TRS resources come in l / l+4 pairs in one slot with one frequency allocation. If either member of a confirmed pair occurs in the slot, both are applied. | `nr_csirs_blind_rt.c` (`full_add_trs_pairs`) |
| `ISAC_CSIRS_FULL_BLOCK_INFER=1` | This is a measured network property, not a TS rule. When any row-1 TRS of a 52-RB block occurs in the slot, every confirmed TRS geometry of that block is applied. Measured: every block carries all its TRS in both bursts. This covers bursts that discovery has not yet confirmed. | `nr_csirs_blind_rt.c` (`full_add_block`) |
| `ISAC_CSIRS_FULL_ERASE=1` | The resources not selected (other blocks' TRS) become LLR erasures: received RE zeroed, excluded from DD-EVM. GPU-LLR and GrantWork are bypassed when erasures are present. | `nr_dlsch_demodulation.c` (`nr_dlsch_csi_erase*`), `nr_csirs_blind_rt.c` |
| `ISAC_CSIRS_FULL_AB=1` | Diagnostic: CSIFULLAB paired hypotheses (none / own / rule / all / own16 / rule-no-erase), each with S and S-toggled, on the same samples. Never fed back. | `nr_csirs_blind_rt.c` (`nr_csirs_full_ab`) |

Capacity change:
- `NFAPI_MAX_NUM_CSI_RATEMATCH` 4 → 32. Every existing passive caller now passes the new `NR_CSI_RM_LEGACY_MAX` (4), so the DCI monitor, the solver's scrambling-ID harvest and the DECODE-FIX arms see exactly the old view.
- The stock MAC `AssertFatal`s in `nr_ue_procedures.c` now allow up to 32.
- With every flag off, receiver behaviour is unchanged.
- One bug fix sits inside QAM-FIX's own extent path (`ISAC_PDSCH_CSI_RB_EXTENT=1`): the RE-count loop in `nr_rx_pdsch` mixed the BWP-relative bitmap and CRB indices. It is masked when the SSB plan's count is used, as it always is on the passive path.

Hooks in files owned by others:
- `nr_pdsch_passive_decode.c` (QAM-FIX), about 20 lines:
  - the apply and AB hook at the top of `nr_pdsch_passive_decode()`;
  - the PRB-map set and reset around the demodulation loop, plus the `t_csi_prb_map` TLS;
  - parity-refusal skip under the flag;
  - erasure gates on GrantWork and GPU-LLR;
  - `NR_CSI_RM_LEGACY_MAX` in the two DECODE-FIX calls.
- `nr_dlsch_demodulation.c` (QAM-FIX), about 70 lines, all in the CSI overlap/extent helpers I own: PRB map, erasure list and mask, extractor erasure, DD-EVM skip of erased REs, extent RE count, flag switch.
- `nr_pdcch_blind_monitor_rt.c`: 2 tokens (`NR_CSI_RM_LEGACY_MAX`).
- No `*.cu`, `*_gpu*` or `nr_dl_channel_estimation.c` file was touched.

## 4. Paired evidence: which REs the gNB actually skips [MEASURED, CSIFULLAB, replay capture b]

Runs `bi_b1/2/3` use flags FULL + BURSTS + TRS_PAIR + BLOCK_INFER + ERASE + AB. The data are MCS0-4 dedicated decodes, and a hypothesis passes when its own decode is CRC-OK (`tools/csifull_ab_summary.py`).

| population, Nl, class | grants | none | own ID | **rule (own + co-block)** | all in slot | own, extent ↑16 | rule, no erasure |
|---|---:|---:|---:|---:|---:|---:|---:|
| 415, Nl1, A (first RB < 52, TRS slot) | 19 | 0 | 0 | **16** | 4 | 0 | 16 |
| 415, Nl2, A | 15 | 0 | 0 | **10** | 4 | 0 | 10 |
| 415, Nl1, C | 8 | 7 | 7 | 7 | 1 | 7 | 7 |
| 415, Nl2, C | 14 | 8 | 8 | 8 | 5 | 8 | 8 |
| 249, Nl1, A | 6 | 2 | 2 | 2 | 0 | 2 | 2 |
| 249, Nl2, A | 59 | 8 | 8 | 8 | 0 | 8 | 6 |
| 249, Nl2, C | 20 | 8 | 8 | 8 | 6 | 8 | 8 |

What the table shows:
- The rule never loses a grant that another hypothesis decodes.
- "All in slot", the old union intent, loses population-249 grants in RB 0-51.
- The other hypotheses lose population-415 grants.
- Most population-249 Nl2 grants are below 10 dB.
- Erasure adds 2 population-249 grants and changes nothing else.
- Earlier runs without BLOCK_INFER (`abb2`, `vab_b2`) show the same picture on the 15/16 burst: e.g. 0x37f3 512.16, 368.16, 544.16 and 640.16 decode only with the rule. They show no decode at 20/21, where the 415 burst was not yet discovered.

At ≥ 10 dB, class A on capture b with the rule applied and the block discovered (`bi_b*`, joined with EVMGRANT):

| | TBs |
|---|---|
| none | 2/9 |
| **rule** | **8/9** |

The remaining failure is 0x37f3 512.15. It fails every hypothesis and both S values in every run, while its neighbour 512.16 (same geometry) decodes with the rule. It stays unexplained in the denominator.

## 5. Replays: class tables (capture-separated, MCS0-4 QPSK, ≥ 10 dB, dedicated, first-RB classes as QAMFIX.md)

[MEASURED] Class definitions, as in `tools/csifull_classes.py`:
- A = first RB < 52 at abs%160 ∈ {15,16,20,21};
- B = first RB < 52, other slots;
- C = first RB ≥ 52.

The classes reproduce QAMFIX's ab_B counts exactly (69 / 986 / 1139).

The 12 s replays are bimodal in solver acquisition: about 5000 or about 600-1600 EVMGRANT per run, as DECODE-FIX documented. Across-run totals are therefore not A/B evidence; section 4 holds the paired evidence.

Capture b:

| run | flags | Nl1 A | Nl1 B | Nl1 C | Nl2 B | Nl2 C |
|---|---|---|---|---|---|---|
| base_b | off | 2/9 | 78/85 | 146/150 | 19/20 | 21/21 |
| base2_b | off | 1/10 | 66/75 | 138/146 | 16/18 | 22/22 |
| base3_b | off | 5/13 | 83/90 | 149/152 | 15/17 | 25/25 |
| bi_b1 | rule + all levers + AB | **4/5** | 82/89 | 134/139 | 19/22 | 20/21 |
| bi_b2 | same | **3/4** | 19/23 | 41/42 | 3/4 | 5/5 |
| bi_b3 | same | 0/5 (block not yet discovered) | 15/21 | 40/42 | 5/6 | 4/5 |

Capture 8 has almost no class-A traffic, at most 1 grant per run:

| run | flags | Nl1 B | Nl1 C | Nl2 B | Nl2 C |
|---|---|---|---|---|---|
| vbase_81 | off | 43/49 | 66/74 | 5/5 | 20/20 |
| vbase_82 | off | 47/54 | 80/90 | – | – |
| vfin_81 | rule + levers (no AB) | 47/54 | 81/91 | – | – |
| vab_82 | rule + levers + AB | 43/49 | 67/76 | 4/4 | 19/19 |

Capture 8 shows no regression: B and C are identical within one grant.

Sanity runs with the recommended set except erasure and AB (FULL + BURSTS + TRS_PAIR + BLOCK_INFER), no crashes or asserts:

| run | Nl1 A | Nl1 B | Nl1 C | Nl2 B | Nl2 C |
|---|---|---|---|---|---|
| `final_b` (capture b, 2557 EVMGRANT) | 2/3 | 37/43 | 65/66 | 11/12 | 12/13 |
| `final_8` (capture 8) | – | 41/47 | 61/68 | – | – |

## 6. Class B is not CSI-RS [MEASURED]

**Capture map.** In RB 0-51, in slots 1-5 and 11-15, there is no periodic NZP or ZP structure at period ≤ 160 outside the DM-RS symbols (2, 11). The per-class power ratio stays between 0.8 and 1.25.

**Replay failures.** In replay `base_b` / `base2_b`, attempt 0, ≥ 10 dB:
- 23/31 and 24/35 class-B failures are recovered by the DECODE-FIX TDRA arm (S=1/L=13 → S=0/L=14). TDRA_RETRY is already on in t26g.env.
- The unrecovered ones are 8+3 per run. They have clean per-symbol DD-SNR (spread ≤ 3 dB) and no k mod 12 signature. Examples: 0x007f 621.4 (S=2, DM-RS 2/7/11, 21 dB) and 0x37f3 178.14 (20 dB).
- Some show a monotonic SNR drop from symbol 3 to symbol 10 that recovers after DM-RS 11, e.g. 857.5: 26.5 → 17.0 dB. This is a time-variation signature, not CSI-RS. `ISAC_CHEST_TINTERP` (existing) is the lever; it was tried once and was not measurable on a run that lost acquisition.
- A few sit on the undiscovered full-band non-TRS CSI-RS of section 1 (abs%160 24/135/63), e.g. 0x101b 625.4 with dips at symbols 10/12/13 (ph 24) and 0x101b 198.15 / 0xe169 558.15 with dips at symbol 7 (ph 135).

**OTA logs (ab_G1/G2/B1/B2).** Class B failures are concentrated in **wideband** grants starting at RB 0 in slots 1-5 and 11-15:

| grant type | slots | ok |
|---|---|---|
| wide, RB 0 start | 1-5 / 11-15 | 28/90, 61/138, 9/37, 272/447 |
| wide, RB 0 start | 0/6/10/16 | 48/49, 85/86, 17/18, 184/195 |
| narrow | any | ≥ 91 % |

Without DECODEDIAG in those logs the cause cannot be classified further. Next step: one OTA run with `ISAC_PDSCH_DECODE_DIAG=1 ISAC_CSIRS_FULL_AB=1`.

## 7. Tests [MEASURED, DGX, heavy.sh]

17 related tests pass:
- test_dlsch_fixed_point;
- test_nr_csi_rb_extent, with a new virtual→physical PRB map check;
- test_nr_csirs_blind_search and blind_synth;
- test_nr_csirs_disc_dump, e2e, feed, fp, hyp, sweep and td;
- test_nr_csirs_observer;
- test_nr_llr_norm;
- test_nr_pdsch_prb_set and xoverhead;
- test_nr_ssb_rate_match and _prod.

`test_nr_csirs_disc_e2e` first failed (AsyncConfirmationFourActors expects `rate_match_all(…,64)` to return 30). That was caused by an intermediate clamp; it was fixed by moving the legacy 4 to the callers, and the test now passes. The sens6 frozen gate is clean. The full `ninja tests` + ctest was not run (the orchestrator does that after merging).

## 8. Flags for OTA (exact)

t26g.env plus:

```
ISAC_CSIRS_DISC=1 ISAC_CSIRS_FULL=1 ISAC_CSIRS_FULL_BURSTS=1 ISAC_CSIRS_FULL_TRS_PAIR=1 ISAC_CSIRS_FULL_BLOCK_INFER=1 ISAC_CSIRS_FULL_ERASE=1
```

For one diagnostic run, also add `ISAC_PDSCH_DECODE_DIAG=1 ISAC_CSIRS_FULL_AB=1`. AB costs up to 12 extra decodes per grant in TRS slots (4 of 160 slots).

Leave `ISAC_PDSCH_CSI_RB_EXTENT` unset: `ISAC_CSIRS_FULL` forces the extent itself.

Read the result in two places:
- CSIFULLAB lines: `range=1x` with `none=00`.
- Class A in `tools/csifull_classes.py run.log`.

Expected OTA behaviour: after about 1-2 min, discovery holds both bursts of every block. The live logs show 20/21 WINNERs for 415 within the first minute; BURSTS now keeps them.

## 9. Open items

- 0x37f3 512.15 and the clean class-B failures (section 6).
- Non-TRS full-band CSI-RS at abs%160 24/63/105/135: discovery has footprints but builds no hypotheses for them (rows with 4-8 subcarriers × 4 symbols). They would need a row-agnostic footprint export; this is about 2.5 % of slots.
- `BLOCK_INFER` is network-specific. On another network it must be re-checked with CSIFULLAB (`range` vs `own`).
- Replay acquisition is bimodal, so whole-run CRC tables are noisy. Use the paired CSIFULLAB numbers.

## Tools added

- `tools/csirs_capture_map.py` and `tools/csirs_capture_map_report.py`: offline TRS/CSI-RS ground truth from raw IQ. Heavy: run them through heavy.sh.
- `tools/csifull_classes.py`: class A/B/C tables.
- `tools/csifull_diag.py`: per-grant DECODEDIAG/DECODEEVM view.
- `tools/csifull_csi_grants.py`: TRS-slot grant outcomes.
- `tools/csifull_ab_summary.py`: pooled paired CSIFULLAB.
- `tests/passive_rx/csifull_replay.sh`: replay wrapper. It uses t26g.env without the radio-only flags, plus `ISAC_PDSCH_CORES=8,9,12,13,14`, `ISAC_CSIRS_DISC=1` and the discovered-CORESET file.
