# DECODE-FIX — grant geometry, CSI-RS budgets, and paired CRC evidence

Branch `csd/decode-fix`, base `dc582e056d`, 2026-10-09. Capture: `raw_pci415_12s_b`.
All builds and replays use the orchestration `heavy.sh`, one job at a time. No RF, capture, hardware tools, sudo, merges, or changes to SOLVE2/GPU2-owned files.

**Result:** confirmed TDRA aliasing, incorrect CDM-group overhead, and incorrect CSI-RS exclusion budgets. Added per-grant diagnostics and independent default-off recovery flags. The recorded-input Nl1/MCS0–4/SNR>=6 cohort improves from **91/207 to 197/207 (95.17%)** using CRC-verified TDRA/CSI alternatives. **The requested runtime gate is not met.** The largest useful combined replay is **175/198 → 180/198 (90.91%)**; later population/extent validation runs acquired no solved dedicated grants. No claim is made that every high-SNR failure is fixed.

Required next integration: the exact SOLVE2 patches below, especially withholding original-hypothesis credit on a recovered TB. They are provided as patches, not edits to files owned by SOLVE2. The remaining small high-SNR failures also need further diagnosis; the proposed upstream patches are not proof that those failures disappear.

## Confirmed root cause: a solved layout aliases the TDRA bit

[MEASURED] `replays/replay_diag1_184403/run.log`: full dedicated Nl=1/MCS0–4 attempts with decision-directed EVM SNR >=6 dB pass **116/227** CRCs. Every grant has `G == produced`; `llr_have` is the last *nonzero* LLR index, not the number of produced LLRs. Failed CB0 probes must not enter the full-TB denominator.

[MEASURED] For RNTI 0x37f3, payload bit 32 (LSB numbering in the 49-bit `DCIRAW` payload) separates the dominant failure population. The four bits at 32..29 are 0 or 8. Every baseline pass in the low-MCS/high-SNR cohort reads 0; most failures read 8. All these full decodes were configured S=1/L=13, mask 0x804.

[MEASURED, identical received inputs] Recorded PDL1 LLRs are descrambled and decoded by the actual CPU `libldpc.so`. The baseline harness reproduces all 609 captured full dedicated CPU outcomes, including multiple-code-block outcomes. No synthetic waveform substitutes for capture data. Prepending one symbol of **erasures**, shifting the descrambling sequence, increasing G, and recomputing TBS for S=0/L=14 recovers **143** previously failing TBs among the 590 allocations without other RE exclusions. **13** recoveries have multiple code blocks. This test cannot exploit any new received samples: symbol 0 was not in the baseline dump. It is a conservative recovery experiment, not the final live-demodulation gate.

Example: 0x37f3 at 52.5, payload `0x1804100802018`, EVM SNR 20.05 dB, MCS0, 16 PRBs, QPSK/Nl1:

- Wrong S=1/L=13: G=4224, A=504, BG2, Zc=72, K=720, F=200, CB CRC fails.
- S=0/L=14 with symbol-0 erasures: G=4608, A=552, full TB CRC passes.
- Same identity 415, same RV0, same received symbols 1..13; no HARQ history, CSI-RS, PT-RS, or SSB exclusions in this example.

[CODE] `nr_pdsch_solver.c:choose_rep()` chooses the lowest candidate layout consistent with successful `(payload,S)` examples. Layout 1662 can assign zero bits to TDRA and absorb the real TDRA bit in `pre_mcs`. Two S=1 successes do not disambiguate it. Once solved, `plan_known()` emits the known S and returns; refinement tries other readings with that same S. Consequently S=0 successes are never sought and cannot disprove the alias. This is a solver bug, not an LDPC waterfall.

The owned decoder adds default-off `ISAC_PDSCH_TDRA_RETRY=1`: on a failed full dedicated decode ending at symbol 14, re-demodulate the same slot with the alternate S=0/L=14 or S=1/L=13, recompute all coding parameters, and accept only the normal full, nonzero TB CRC. Keep the successful PDU so reconstruction sees the actual geometry. Passing initial grants are untouched. Probes, GPU-provided LLRs, and HARQ-combining contexts are excluded. This is a bounded recovery workaround; the solver fix below is the preferred upstream correction.

## Confirmed second layout error: CDM groups without data

[MEASURED] `recovery4_192424` solved 0x37f3 as layout 2090. On 152.5 it supplied type-1 DM-RS port 0, **one** CDM group, S=0/L=14, G=4992 and A=608. In the earlier correctly mapped capture, the same DCI/PRBs require **two** CDM groups, G=4608 and A=552. The one-group assumption includes odd-subcarrier data on the two DM-RS symbols that is not part of this codeword.

[MEASURED, identical recorded inputs] `decodefix_cdm_trials.py` reconstructs the RE layout, removes those falsely included REs (or inserts erasures when the original dump omitted REs), changes S when requested, and recomputes TBS. All original outcomes reproduce exactly. For 29 captured high-SNR Nl1/MCS0–4 grants in that run, TDRA alone remains 4/29, while the CDM alternative plus TDRA reaches **24/29**. These are twenty additional verified TBs; no new received samples are used.

Default-off `ISAC_PDSCH_DMRS_CDM_RETRY=1` tries one versus two CDM groups only for type-1 DM-RS ports 0/1, where both counts are physically legal. It composes with the TDRA and CSI-RS trials and retains the successful PDU. Higher port groups are not assigned an illegal one-group hypothesis.

## Other hypotheses

[MEASURED] Scale factors 1/2, 1/4, 1/8, 1/16, 2, 4, 8, 16 on the original 227 inputs, with 25 iterations, recover at most one failure (factor 4). The unmodified baseline at 25 iterations still passes exactly 116/227. RV1/2/3 recovers none of the original failures. Exhaustive nearby legal small TBS values and either base graph do not recover the original failures at unchanged G/mapping. Searching all 65536 data scrambling identities finds no substantial repetition correlation on the failed MCS0 inputs. The original normalization/scaling retry experiment was removed. After correcting TDRA, 8/25/64-iteration and scale trials on the remaining failures still recover none; global QPSK quarter-turn rotations and RV1/2/3 also recover none.

[MEASURED] Repeated coded-bit sign correlation: passing small MCS0 grants average 0.959; failures average 0.0015 (chance). In the dominant TDRA-failure population, fixed-point compensated QPSK signs agree with the independently computed float equalizer. Later replay `recovery5` also exposed isolated fixed-point sign disagreements (135/4224 on 0xdb3e 611.4); the wide-accumulator hypothesis is evaluated separately. High EVM SNR describes a clean constellation; it does not verify bit position, scrambling alignment, or codeword geometry.

[MEASURED] Multi-CB operation already works in the baseline: 0x37f3 at 17.2 passes all seven CBs, E=[16536,16540,16540,16540,16540,16540,16540], A=55304. There is no universal “CB0 works, later CBs fail” implementation failure. The observed full failures generally fail all CBs. LBRM is not binding for this cohort.

[MEASURED] Eleven low-MCS/high-SNR failures with TDRA bits=0 have CSI-RS exclusions (e.g. 0x37f3 224.15 removes 192 REs from 16 PRBs). The solver latched `rm=1` on 9.0, where the exclusion hypothesis really does pass and the no-exclusion hypothesis fails. That does not establish that the entire subsequently discovered union of CSI-RS resources should apply to every future grant. `ISAC_PDSCH_CSI_RETRY=1` independently retries the opposite exclusion arm (remove the configured union, or add the currently discovered overlapping union) and combines it with TDRA/CDM alternatives when enabled. This hypothesis must be judged by the paired CRC result, not by the count of configured resources.

## Replay validity

- `diag1_184403`: useful baseline and PDL1 dump; CPU decoder=1, GB10 PDCCH discovery enabled.
- `diag2_scale_rv_184700`: zero solved dedicated RNTIs; not a decoder A/B result.
- `tdra1_190430`: zero solved dedicated RNTIs; excluded from the recovery gate.
- `tdra2_audit_190909`: zero solves with the experimental leader setting; useful only for PDCCH prescreen audit.
- `recovery3_191352`: both identity populations solved with SCR_CLEAN=0 and more probe coverage; initial latch S=0 exposed the reverse TDRA error. Exact S=1 trimming raises the captured Nl1/MCS0–4/SNR>=6 cohort from 91/207 to 192/207. Offline CSI-RS exclusion trials recover four more (196/207); this is still below the requested 95% gate and is not presented as passing it.
- `recovery4_192424`: CDM ambiguity above; independent probe outcomes also selected a different layout for the same RNTI. The CDM recorded-input recovery is verified; the later runtime cohorts did not reproduce that wrong-CDM solved population.

## 415 population

[MEASURED] “415 dedicated UEs never solve” is not universal on the merged branch. `diag1` explicitly decodes dedicated grants with **both DM-RS and data scrambling identities 415**, including 0x37f3, 0xb3d3, 0x5d93, 0x101b and a two-layer 0x3383 grant. A solver `scr=-1` label alone would not establish that fact; DECODEDIAG records the actual PDU identities.

[MEASURED] Population acquisition is unstable. The same capture gives per-CORESET 10163 DM-RS decisions 415 in `diag1` and 442 in later runs; its later top-three list is **442,249,467**. `ISAC_PDSCH_SOLVER_SCR_CLEAN=1` drops the real 249 hypothesis after the unverified 442 decision. Serving CORESET 62922 does identify 415, but sparse probes often do not reach the correct layout/physical hypothesis combination before the capture ends. Zero-solve runs cannot be pooled with full decoded grants.

[CODE, additional identity hazard] The `scr=-1` PDU inherits the global per-nSCID DM-RS decision, whereas SCR_CLEAN treats it as guaranteed PCI. If the global decision belongs to another population, removing explicit PCI from the alternate list removes the serving-cell hypothesis. Keep explicit physical identity pairs, and retain candidate identities until CRC evidence—not merely the blind DM-RS ranking—distinguishes them.

## PDCCH misses (diagnose only)

[MEASURED] `tdra2_audit` enables the existing shared-screen audit at the baseline known-screen threshold 0.66. It finds and admits valid known-RNTI DCIs below that cutoff, including 0xce76 with scores 0.615882/0.627184 and 0xe513 at 0.659457. The cutoff is therefore not a safe empty-CCE test. SCANFAST independently measured admitted DCIs down to 0.462671. The known-RNTI path still defaults to 0.66 even though the newer shared screen defaults safely to zero.

[CODE] `ussh_allows()` admits only m=0 for an aggregation level not yet observed (`M=0`), although m>0 may be configured. Its fit retains only four hypotheses and assumes one M-vector shared by all RNTIs. These are additional possible false-negative mechanisms; they were not isolated by this audit. HARQ-PID jumps alone do not prove a precise DCI miss rate: scheduling need not increment PIDs consecutively, and the current layout handoff sets `harq_pid_bits=0` by folding HARQ into the skipped `tb2_bits` region. That metadata bug must be fixed before interpreting parsed PID continuity. No claim is made that this audit apportions the historical 6%/30% exactly.

## Required SOLVE2 patches (not applied here)

### TDRA refinement; opt-in, to be built/tested by SOLVE2

```diff
--- a/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c
+++ b/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c
@@ -475,6 +475,16 @@
   const int t_use = table >= 0 ? table : tl[0];
   if (phase == 1 && S >= 0 && t_known) {
     emit(out, n, max, slot, phase, reading, rd, l0, S, add_pos, t_use, false, nt > 1, false, false, tda, dci, payload);
+    /* Successful probes at one TDRA codepoint do not resolve an aliased TDA/pre-MCS split.
+     * Keep testing other starts so CRC evidence can reject that representative. */
+    static int refine_tdra = -1;
+    if (refine_tdra < 0)
+      refine_tdra = getenv("ISAC_PDSCH_SOLVER_TDRA_REFINE") && atoi(getenv("ISAC_PDSCH_SOLVER_TDRA_REFINE"));
+    for (int si = 0; refine_tdra && si < 4 && *n < max; si++) {
+      if (kS[si] == S) continue;
+      emit(out, n, max, slot, phase, reading, rd, l0, kS[si], add_pos, t_use,
+           true, nt > 1, true, false, tda, dci, payload);
+    }
     return;
   }
   for (int si = 0; si < 4; si++) {
@@ -764,6 +774,9 @@
         memcpy(r->S_by_tda, m, sizeof(m));
         return id;
       }
+      /* Two CRC-verified payloads assigned different S to the same TDA index.
+       * Do not resurrect this contradiction when the 16-entry evidence window rolls. */
+      r->cand_bm[b] &= (uint8_t)~(1u << k);
     }
   }
   return -1;
```

### Retain explicit PCI and blind-ID runners-up; proposed, not applied

```diff
--- a/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
+++ b/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
@@ -8633,13 +8633,11 @@
       if (cfg->discovered_coreset) { /* T26c: this CORESET's blind-decided PDSCH DM-RS id first, when it differs from the cell's */
         sv_did = nr_pdsch_passive_coreset_dmrs_id((int)cfg->coreset_pdcch_dmrs_scrambling_id);
         int top3[3];
-        static int s_clean = -1;
-        if (s_clean < 0) s_clean = getenv("ISAC_PDSCH_SOLVER_SCR_CLEAN") && atoi(getenv("ISAC_PDSCH_SOLVER_SCR_CLEAN")) ? 1 : 0;
-        if (s_clean && sv_did >= 0) {
-          /* A decided PCI already is the default. The lower-ranked IDs have no CRC evidence and, on the serving
-           * CORESET, are near-noise estimates. A non-PCI decision is the sole alternate until CRC proves it. */
-          if (sv_did != fp->Nid_cell) sv_alt[sv_nalt++] = (uint16_t)sv_did;
-        } else if (sv_did >= 0 && nr_pdsch_passive_coreset_dmrs_top((int)cfg->coreset_pdcch_dmrs_scrambling_id, top3) == 3) {
+        /* The implicit/default PDU can inherit another population's global DM-RS ID.
+         * PCI must remain an explicit (DM-RS,data) hypothesis. A blind ranking is
+         * not CRC evidence for deleting its runners-up (measured 442 ahead of 249). */
+        sv_alt[sv_nalt++] = fp->Nid_cell;
+        if (sv_did >= 0 && nr_pdsch_passive_coreset_dmrs_top((int)cfg->coreset_pdcch_dmrs_scrambling_id, top3) == 3) {
           for (int q = 0; q < 3; q++)
             if (top3[q] >= 0 && top3[q] != fp->Nid_cell) sv_alt[sv_nalt++] = (uint16_t)top3[q];
         } else if (sv_did >= 0 && sv_did != fp->Nid_cell)
```

### Disable the disproven known-CCE cutoff by default; measured root cause

```diff
--- a/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
+++ b/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
@@ -6564,11 +6564,11 @@
        * x 64 candidates every slot (~2 ms p50 on the PCI 415 replay). */
       {
         static int s_kps = -1;
-        static double s_kmin = 0.66;
+        static double s_kmin = 0.0;
         if (s_kps < 0) {
           s_kps = getenv("ISAC_PDCCH_KNOWN_PRESCREEN") && atoi(getenv("ISAC_PDCCH_KNOWN_PRESCREEN")) ? 1 : 0;
           const char *e = getenv("ISAC_PDCCH_KNOWN_PRESCREEN_MIN"); /* amplitude-consistency score; noise mean 2/pi = 0.637 */
-          if (e && atof(e) > 0) s_kmin = atof(e);
+          if (e && isfinite(atof(e)) && atof(e) >= 0) s_kmin = atof(e);
         }
         bool rejected = (shared_screen && !shared_screen_ok[c])
             || (s_kps && cfg->discovered_coreset && !t_discovered_token && !shared_screen
```

These patches are deliberately not applied in this worktree. The TDRA refinement must run with decoder fallback disabled while evaluating solver learning: the current queue feedback token describes the original S, so a recovered S=0 full decode must not be credited as proof of S=1. A production integration should pass actual successful geometry into feedback or withhold parameter-learning credit on a recovery.

For HARQ metadata, group sums cannot distinguish a second TB from 4/5-bit HARQ PID and DAI/K1 widths. Preserve those semantic widths in the layout enumeration and grouping key; do not merely replace `harq_pid_bits=0` with 4 for every hypothesis. The latter is only valid after establishing a one-codeword, four-bit-HARQ layout.

The requested 95% gate has **not** yet been met; see the paired measurements below.

### Withhold solver-parameter credit for an owned-decoder recovery

The new result field distinguishes a recovered TB from evidence for the queued hypothesis. Apply this queue patch with the decoder flags to prevent false S/CSI learning. It does not discard the recovered TB from the data-aided path.

```diff
--- a/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c
+++ b/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c
@@ -1630,7 +1630,7 @@
           atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed), job.absolute_slot, slots_per_frame);
       /* A CRC pass is evidence even on IQ that expired meanwhile (overwritten samples cannot pass a CRC but by the
        * same chance any wrong hypothesis can); a FAILURE on expired IQ says nothing and is not credited. */
-      const bool sv_ok = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
+      const bool sv_ok = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && !dec.config_recovered;
       static int s_sdiag = -1;
       static _Atomic int s_sdiag_left = 400;
       if (s_sdiag < 0) s_sdiag = getenv("ISAC_PDSCH_SOLVER_DIAG") ? 1 : 0;
@@ -1641,7 +1641,8 @@
               job.dlsch_pdu.number_symbols, job.dlsch_pdu.dlDmrsSymbPos, job.dlsch_pdu.numCsiRsForRateMatching,
               (long)(job.dci_abs_slot % 160), (int)st);
       nr_pdsch_solver_feedback(job.solver_token, sv_ok,
-                               (sv_ok || sv_credit) && nr_cfg_epoch_work_current() && st != NR_PDSCH_PASSIVE_DECODE_ERROR
+                               !dec.config_recovered && (sv_ok || sv_credit) && nr_cfg_epoch_work_current()
+                                   && st != NR_PDSCH_PASSIVE_DECODE_ERROR
                                    && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED);
     }
     if (!nr_cfg_epoch_work_current()) {
```

## Reproduction

Own build: `cmake_targets/ran_build/build`; own CPM cache: `cmake_targets/ran_build/cpm_cache`; replay output: `replays/`. `tests/passive_rx/decodefix_replay.sh` contains the exact complete baseline environment and invokes `heavy.sh`. The input is the existing capture, with CAP_SAMPLES=1474560000, CAP_TICK=208029025, speed 0.25, BASECFG=`tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg`, ISAC_MLOCK=0, scan cores 10,11 and PDSCH cores 8,9,12,13,14. There was no OTA transmission or acquisition.

Diagnostics: `ISAC_PDSCH_DECODE_DIAG=1`; `DECODEDIAG` records G, produced and last-nonzero LLR counts, all CB E/CRC outcomes, C/K/Zc/F/BG/Ncb/LBRM, RV/NDI/PID, S/L/DM-RS mask/add-pos/type/nSCID/ports/CDM, identities, exclusions, table, TBS, decoder, and attempt number. `attempt=0` is the original attempt; retries/restoration must not be counted as new grants. `DECODETDRA` supplies the paired final recovery status. Summary: `python3 tests/passive_rx/decodefix_summary.py RUN/run.log`.

Recorded-input harness: build `minimal_lib` through heavy.sh, then `heavy.sh env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp python3 tests/passive_rx/decodefix_build_diag.py`. Generate inputs with `decodefix_inputs.py LOG DUMP OUTPUT tdra` (or `scale`), and run `build/decodefix_ldpc build/libldpc.so OUTPUT` with LD_LIBRARY_PATH set to the build directory, also through heavy.sh. PDL1 dumps and generated binary inputs stay under ignored `replays/`.

## Paired recorded-input results by Nl/MCS/size

[MEASURED] `recovery3_offline.bin{,.json}` / `.log`, plus `csi_trials.bin{,.json}` / `.log`. Counts below are successful full TB CRCs, all measured SNRs, CPU decoder, MCS0–4. Size uses original G. Each original grant occurs once; the after columns preserve every original pass and add only CRC-verified alternatives on the same captured LLRs. Missing symbols/REs are zero erasures. The CSI column is an exhaustive **offline** exclusion search and must not be confused with the runtime discovered-union retry.

| Nl | MCS | Original G | TBs | Before | TDRA | TDRA + CSI search |
|---|---|---|---:|---:|---:|---:|
| 1 | 0 | 8–16k | 7 | 0 | 3 | 6 |
| 1 | 0 | <8k | 239 | 94 | 193 | 194 |
| 1 | 0 | >16k | 11 | 1 | 1 | 1 |
| 1 | 1 | 8–16k | 1 | 1 | 1 | 1 |
| 1 | 1 | <8k | 42 | 9 | 32 | 32 |
| 1 | 2 | 8–16k | 1 | 0 | 1 | 1 |
| 1 | 2 | <8k | 48 | 14 | 36 | 36 |
| 1 | 2 | >16k | 2 | 1 | 1 | 1 |
| 1 | 3 | <8k | 25 | 5 | 16 | 16 |
| 1 | 3 | >16k | 1 | 0 | 0 | 0 |
| 1 | 4 | 8–16k | 3 | 1 | 2 | 2 |
| 1 | 4 | <8k | 34 | 15 | 20 | 20 |
| 2 | 0 | 8–16k | 496 | 62 | 302 | 302 |
| 2 | 0 | <8k | 6 | 0 | 6 | 6 |
| 2 | 0 | >16k | 5 | 1 | 2 | 2 |
| 2 | 1 | 8–16k | 73 | 9 | 44 | 44 |
| 2 | 1 | <8k | 3 | 0 | 3 | 3 |
| 2 | 1 | >16k | 7 | 3 | 3 | 3 |
| 2 | 2 | 8–16k | 56 | 3 | 22 | 22 |
| 2 | 2 | <8k | 2 | 0 | 2 | 2 |
| 2 | 2 | >16k | 13 | 3 | 3 | 3 |
| 2 | 3 | 8–16k | 14 | 0 | 5 | 5 |
| 2 | 3 | <8k | 1 | 0 | 1 | 1 |
| 2 | 3 | >16k | 6 | 0 | 0 | 0 |
| 2 | 4 | 8–16k | 10 | 1 | 2 | 2 |
| 2 | 4 | <8k | 1 | 0 | 0 | 0 |
| 2 | 4 | >16k | 7 | 0 | 0 | 0 |

The specific Nl1/MCS0–4/original SNR>=6 cohort is 91/207 before, 192/207 with TDRA, and 196/207 (94.69%) after the offline CSI search. These denominators exclude failed probes and retain the original SNR even if the corrected geometry changes its estimate. The requested >=95% gate is not established by this table.

Exact full-replay override command (baseline environment is versioned in the invoked script):

```bash
bash tests/passive_rx/decodefix_replay.sh recovery6 \
  ISAC_DCI_RAW_LOG=1 ISAC_PDSCH_TDRA_RETRY=1 ISAC_PDSCH_CSI_RETRY=1 \
  ISAC_PDSCH_DMRS_CDM_RETRY=1 ISAC_PDSCH_WIDE_MRC_RETRY=1 \
  ISAC_PDSCH_SOLVER_SCR_CLEAN=0 ISAC_PDSCH_SOLVER_BACKOFF=0 \
  ISAC_PDSCH_SOLVER_PROBE_RATE=4 ISAC_PDSCH_SOLVER_PROBE_BURST=96 \
  ISAC_PDSCH_SOLVER_PROBE_FILL=50 \
  ISAC_PDSCH_LLR_DUMP=/home/nicola/NICOLA/wt/rr-csd-v/replays/recovery6.llr \
  ISAC_PDSCH_LLR_DUMP_MAX=30000 ISAC_PDSCH_LLR_DUMP_FULL=1
```

For `recovery3`, omit the CDM and wide-MRC flags; for `recovery4`, omit those same flags (the binary at that point has symmetric TDRA and CSI add/remove); for `recovery5`, omit only wide-MRC. Use matching run/dump names. Earlier `diag1` used the baseline script environment plus DCIRAW and its full-only dump, without the increased probe coverage or recovery flags.

## Additional measured results and limits

[MEASURED] Band-limited CSI-RS trials on the same `recovery3` LLRs add 0x37f3 **465.1**, a two-CB TB. Correct S=1, TBS=4488, and an exclusion pattern over the first 64 allocated RBs gives G=36480 and passes both CBs. The original full-slot hypothesis had G=41472/TBS=4872; correct S without CSI removal has G=38016 and still fails. This adds one unique success to the preceding 196/207 cohort, reaching **197/207 (95.17%) offline**. A passing mask is CRC evidence for that bit mapping; it does not uniquely identify the network's complete RRC CSI-RS configuration. The helper's physical-frequency interpretation assumes contiguous RBs, so successful recovery, not its label, is decisive.

Reproduce with `decodefix_csi_population_trials.py LOG DUMP recovery3_offline.bin.json recovery3_offline.log OUTPUT`, then the same recorded-input LDPC harness. It uses row-1 positions actually found in CSI-RS discovery logs and band edges 0/64/128; it is an offline diagnostic, not the runtime policy.

[IMPLEMENTED, default off] `ISAC_PDSCH_CSI_POPULATION_RETRY=1` obtains the currently occurring resources, tries the subset whose scrambling identity matches the grant's DM-RS identity, and separately tries rounding those resources' frequency extents outward to 16-RB boundaries. Each result must pass a full TB CRC. The subset identity is a hypothesis: CSI-RS and DM-RS identities need not generally match. This avoids committing all detected populations to the UE's exclusion budget. Neither variant affects an already passing grant.

[MEASURED] Runtime paired results; population-extent runs without solved grants are excluded:

| Replay | All full dedicated, before → after / total | Nl1/MCS0–4/original SNR>=6, before → after / total |
|---|---:|---:|
| recovery3 | 242 → 251 / 1867 | 91 → 96 / 207 |
| recovery4 | 17 → 18 / 208 | 6 → 6 / 31 |
| recovery5 | 54 → 54 / 223 | 16 → 16 / 18 |
| recovery6 | 341 → 382 / 521 | 175 → 180 / 198 |
| recovery7 | 89 → 112 / 164 | 27 → 38 / 44 |

These are within-run paired counts; changing acquisition outcomes make across-run totals unsuitable for an A/B comparison. `recovery7` tested nSCID and fixed the frequency-compensation value during retries. The population-extension code was not in that binary (verified by its embedded flag strings), so it is **not** population-extension validation. nSCID flips recovered no additional TBs and were removed from the final implementation.

[MEASURED] `recovery6` has 36 TDRA recoveries, four CSI recoveries, and one wide-MRC-arm recovery. The latter is 0x5d93 388.10, Nl1/MCS4, G=4224/TBS=2472: original sign disagreements 7/4224 and CRC fail; wide accumulation gives 0/4224 and CRC pass. The independent EVM changes from 5.39 to 5.50 dB across these attempts, so this is evidence of a recovery with the arithmetic arm, not a perfectly isolated equalizer A/B. The final wrapper fixes the original FEP frequency compensation across alternatives; the diagnostic now records it explicitly.

[CODE] `nr_channel_compensation()` clamps per-antenna products and intermediate antenna sums to int16. With cancelling branches, clamping an intermediate sum can reverse the sign of the final result. `ISAC_PDSCH_WIDE_MRC_RETRY=1` recomputes QPSK/Nl1 matched filtering with int64 products/sums, shifts once, then clamps once. It is tried only after original failure. No QAM thresholds or multi-layer arithmetic are changed.

[MEASURED negative experiments] 1,755 combinations of shorter allocation ends, starts 0..3, and TB scaling 1/2/4 on the residual recorded inputs recovered none. Band-limited CSI trials tried 5,670 alternatives; only the CRC successes above are credited. Unknown-Gold-seed recovery from repeated-bit parity did not find a consistent alternate identity on the small residual failures. These experiments do not establish that all possible scheduling configurations are exhausted.

Validation so far: receiver/modules built with the prescribed CUDA-121 configuration and GB10 PDCCH discovery exercised; `test_dlsch_fixed_point`, `test_nr_pdsch_xoverhead`, and `test_nr_pdsch_prb_set` all pass (3/3). Recorded-input baseline reproduction, including multi-CB CRCs, is the principal decoder regression check. Final runtime gate is **not met**, as stated at the top. The final build/check result is recorded below.

`recovery8_195542` contains no solved dedicated RNTIs and contributes no gate denominator. Its CORESET-10163 shortlist is 442/291/241, excluding the measured real 249 population; CORESET-62922 still ranks 415 first. This is an acquisition failure, not a measured decoder failure. The binary was verified to contain the population-retry flag and DECODECSI diagnostics before launch.

A further 6,696 legal small-TBS/base-graph trials on `recovery6` failures, including both S=0 and S=1, found only the already-known TDRA recovery of 0xb3d3 332.14 (S=0, TBS=552, BG2). They add no new residual recovery.

The conditional decoder retry preserves every initial CRC pass. This is not a proof of unchanged end-to-end throughput: extra attempts consume worker time, and acquisition varies between replays. The bounded retries are a diagnostic/recovery option; the SOLVE2 layout and feedback patches are needed to remove their recurring cost and prevent false learning.

`recovery9_200020` also acquired zero solved dedicated RNTIs, using the original SOLVE baseline probe budget/SCR_CLEAN=1 plus the recovery flags. A temporary port-0/1 retry therefore obtained no useful evaluation and was removed. This is not a negative result for the port hypothesis. CSI population/extent recovery has recorded-input evidence but its runtime implementation still needs a replay with solved dedicated grants.

### Additional SOLVE2 identity fallback patch (not applied)

`recovery8` shows that the true 249 identity can be absent even from the blind top three. The already-collected CSI-RS identities must remain candidates after an unverified blind DM-RS decision. This supplements the explicit-PCI/top-three patch above; it still requires solver-budget validation.

```diff
--- a/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
+++ b/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c
@@ -8669,7 +8669,7 @@
           if (!dup && s_n < 8) s_ids[s_n++] = sv_rm[k].scramb_id;
         }
-        for (int q = 0; s_cs && !sv_decided && q < s_n && sv_nalt < 16; q++) {
+        for (int q = 0; s_cs && q < s_n && sv_nalt < 16; q++) {
           bool dup = false;
           for (int z = 0; z < sv_nalt && !dup; z++) dup = sv_alt[z] == s_ids[q];
           if (!dup) sv_alt[sv_nalt++] = s_ids[q];
```

## Final implementation and exact validation environment

All five behavioral flags default off:

- `ISAC_PDSCH_TDRA_RETRY=1`: S=0/L=14 versus S=1/L=13.
- `ISAC_PDSCH_DMRS_CDM_RETRY=1`: legal one/two CDM-group alternatives for type-1 ports 0/1.
- `ISAC_PDSCH_CSI_RETRY=1`: opposite discovered-union exclusion arm.
- `ISAC_PDSCH_CSI_POPULATION_RETRY=1`: same-identity CSI-RS subset and its 16-RB extent hypothesis (runtime coverage outstanding).
- `ISAC_PDSCH_WIDE_MRC_RETRY=1`: QPSK/Nl1 int64 matched-filter accumulation before one clamp.

The wrapper excludes probes, HARQ-combining contexts, externally supplied GPU LLRs, and decodes served from GrantWork. Original passes return immediately. Failed recovery restores original geometry, result, and thread-local LLR/decoder state. Accepted recovery retains its successful PDU and sets `config_recovered`. **Apply the queue feedback patch before integrating enabled recovery into solver learning.** No caller/solver/GPU files were changed here.

`ISAC_PDSCH_DECODE_DIAG=1` is diagnostic only. `DECODECSI` additionally records every configured exclusion resource's identity, RB interval, row, symbols, density, frequency bitmap, and CDM type. Retry attempt bits 0..3 are CSI/TDRA/CDM/wide-MRC; adding 64 or 128 selects the population or extended-population CSI variant. Attempt 32 restores the original. Initial attempt 0 is the only denominator entry. FEP compensation is held fixed across a grant's retries and is logged as `fep_fo`.

Exact command for the final shipped flags, using the fully specified baseline environment in `decodefix_replay.sh` (this is replay, not RF/OTA):

```bash
bash tests/passive_rx/decodefix_replay.sh decodefix_final \
  ISAC_DCI_RAW_LOG=1 ISAC_PDSCH_TDRA_RETRY=1 ISAC_PDSCH_CSI_RETRY=1 \
  ISAC_PDSCH_DMRS_CDM_RETRY=1 ISAC_PDSCH_CSI_POPULATION_RETRY=1 \
  ISAC_PDSCH_WIDE_MRC_RETRY=1 \
  ISAC_PDSCH_SOLVER_SCR_CLEAN=0 ISAC_PDSCH_SOLVER_BACKOFF=0 \
  ISAC_PDSCH_SOLVER_PROBE_RATE=4 ISAC_PDSCH_SOLVER_PROBE_BURST=96 \
  ISAC_PDSCH_SOLVER_PROBE_FILL=50 \
  ISAC_PDSCH_LLR_DUMP=/home/nicola/NICOLA/wt/rr-csd-v/replays/decodefix_final.llr \
  ISAC_PDSCH_LLR_DUMP_MAX=30000 ISAC_PDSCH_LLR_DUMP_FULL=1
```

This matches `recovery8`'s enabled behavior; the final code additionally refuses recovery on GrantWork-served decodes. GPU discovery was available and exercised (`PDCCH_NID_GPU ready` / ACQUIRE logs); there are no GPU measurements deferred because of device unavailability. What remains outstanding is useful runtime population/extent coverage and the overall CRC gate, not GPU access.

Residual examples in the useful `recovery6` cohort include 0x37f3 178.14 (MCS0, 16 PRBs, SNR16.97 dB), 192.5 (MCS0, 16 PRBs, 10.67 dB), 478.5 (MCS0, 16 PRBs, 14.59 dB), and large CSI-occasion grants at 465.1/512.16/513.1. The same initial grants pass neither TDRA/CDM alternatives nor the original full CSI union. No nearby-TBS, alternate-BG, scaling/iteration, RV, quarter-turn, or tested shorter-allocation hypothesis explained the small residuals. They must remain failures in the denominator.

Final verification: `replays/final_checks.log` records a successful receiver rebuild and **3/3** focused CTest passes. Diagnostic Python files parse successfully; `git diff --check` passes. The `sens6-frozen-2026-09-30` comparison for capture fixtures, passive configuration files, and the host snapshot is clean. All binaries/tests/replays were run under `heavy.sh`, with no overlapping heavy job launched by this task.
