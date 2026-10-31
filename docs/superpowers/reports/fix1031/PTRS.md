# fix1031/ptrs — D3: blind PT-RS detection, RE exclusion, CPE mode (2026-10-31)

Branch `fix1031/ptrs` off `mpci-int-1031`. Audit item D3 (docs/WEAKNESS_AUDIT_20261031.md): `pduBitmap = 0` on both passive PDU builders made the
PT-RS guards, G exclusion and CPE dead; PT-RS grants were refused or decoded with G too large.

## What changed
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_ptrs_detect.{c,h}` (new, registered in CMakeLists.txt and tools/mpci/mpci_audit_files.txt):
  - `nr_ptrs_detect()`: reference-free, channel-free detector on the transformed slot. A PT-RS RE carries the same QPSK value in every PT-RS
    symbol, so r_a(k)·conj(r_b(k)) = |H|²|p|²·e^{j(φa-φb)} on a PT-RS subcarrier (channel and sequence cancel), zero-mean on data. Hypotheses:
    K∈{2,4} x k_RE_ref 0..11 x L∈{1,2,4} (72), RNTI-dependent k_RB_ref (TS 38.211 7.4.1.2.2), symbol set per TS 38.214 5.1.6.3. Statistic: per pair
    |ΣP(k)|²/Σ|P(k)|² (antenna-summed P), summed over the hypothesis' consecutive PT-RS pairs = Gamma(pairs,1) under no PT-RS, multiplicity-corrected
    (alpha `NR_PTRS_DET_ALPHA` = 1e-4). Independent of DM-RS port, sequence convention, scrambling id, channel estimator.
  - `nr_ptrs_class_t`: evidence per (RNTI, MCS band = mcs/6, N_RB band log-spaced) = 40 classes: detector votes per hypothesis, and CRC arbitration over
    three modes (0 none, 1 exclusion + PT-RS CPE, 2 exclusion only) with Wilson bounds; latch present/absent; detector-silence latches absent after 64
    runs (soft, re-probed 1 in 16); unlatches present if the PT-RS decodes stop working.
- `nr_pdsch_passive_decode.c`:
  - G now subtracts the PT-RS REs (`nr_pdsch_ptrs_unav_res`, exact vs the mask) when the PDU carries a valid (K, L, k_RE_ref); the unconditional PT-RS
    refusal remains only for an invalid triple, the segmented (PRG/VRB) path, and PT-RS together with SSB/CSI-RS rate matching (fail closed; the
    compaction in nr_pdsch_ptrs_processing indexes REs before those are removed).
  - `rnti_dec_t` gets `ptrs[40]` (+~3.6 KB/slot, per cell, g_ptrs_lock); helpers `rnti_ptrs_plan/should_detect/det_feed/crc_feed`, `ptrs_apply_hyp`.
  - `passive_decode_retry_impl`: (1) a class latched present applies its hypothesis from the first body (CPU path: the GPU pass/demod gates already refuse
    pduBitmap&1, unchanged); (2) the without-PT-RS first-body CRC feeds the class (first transmissions only); (3) new PT-RS arm after a failed plain body
    (before the TDRA/CSI/CDM chain, ry arm index RY_ARMS-1, same yield/budget rules): detect-only body (stops after FEP + lifetime check, `t_ptrs_detect_only`),
    feed class, decode again with the class' modal hypothesis and the picked mode, CRC feed. Logs `PTRS_DET`, `PTRS_ARM`, `PTRS_CLASS`.
- `nr_dl_channel_estimation.c` (~1796): pduBitmap bit 2 (0x4, "exclusion only") makes the per-symbol PT-RS phase identity after the compaction. PDU contract for
  the `phase` agent: bit0 = PT-RS present (REs excluded, G reduced); bit2 = additionally the PT-RS-referenced CPE must not be applied (use the other estimators).
  The existing PT-RS CPE correlates against `nr_gold_pdsch` per symbol (OAI gNB convention), which a commercial gNB need not follow (TS 38.211: the DM-RS value
  of the associated port); mode 1 vs mode 2 is CRC-arbitrated per class for exactly this reason.

## Tests (`test_nr_pdsch_ptrs_detect`, 13 tests; `test_nr_pdsch_ptrs_unav` 11 unchanged) [OFFLINE VERIFIED, DGX aarch64 unit]
Synthetic Rayleigh 4-RX, 16-QAM data, per-symbol CPE random walk, QPSK PT-RS fixed across symbols:
- best hypothesis right in 144/144 trials over every K, L, 4 offsets, 24 and 100 PRB, 3 RNTIs (12 dB); fired 72/72 at 100 PRB, 56/72 at 24 PRB (L=4 small grants stay below alpha).
- false alarms on PT-RS-free grants: 0/165 (8..273 PRB, 6..15 dB). (An early version that normalised per antenna gave 67/165; fixed by summing antennas before the normalisation.)
- recall K=2 L=1 100 PRB: 20/20 at -3, 0, 5, 12 dB; 1 RX 273 PRB K=4 L=2 detected.
- G test: PT-RS RE count equals a brute-force count from the spec RE rule for 300 random grids; without the exclusion G is too large by exactly that count.
- class logic: latches present in 6 grant pairs, absent when only the plain arm decodes, mode 2 wins when only it decodes, back-off re-probe, plurality votes.
Build: nr-uesoftmodem, pdsch_gpu, ldpc_cuda, the two tests build clean (fresh build dir, -j6 via heavy.sh).

## NOT verified
- No decode of a real or simulated PT-RS grant end to end (the encoder side with PT-RS in rfsim was not run: replays/rfsim forbidden here). The "decode passes only
  with the right exclusion" property is tested at the G / RE-count level, not through LDPC.
- The mpci static audit was not run separately beyond the build; `ptrs[]` lives inside the existing per-cell heap array `g_rnti_dec`; the two new `__thread`
  variables follow the file's t_* pattern.
- Real-cell PT-RS sequence convention, whether PT-RS exists on the cells at all, mode 1 vs 2 outcome.
- Non-contiguous / PRG>0 grants and grants overlapping SSB or CSI-RS rate matching are not handled (fail closed).
- Detection at tiny SNR / 1 RX / <24 PRB is weak (see above); detection needs a failed first transmission of an undecided class.

## Expected on-air effect [HYPOTHESIS]
On a cell with PT-RS, classes of big/high-MCS grants latch present after ~4-10 failed grants; those grants then decode (G right) instead of failing; Qm6/Qm8
CRC on wide grants rises. On a cell without PT-RS: one detector FEP per failed grant of an undecided class for the first 64, then 1 in 16, no CRC change.
Log fields: `PTRS_DET fired=.. K= L= k= log10p=`, `PTRS_ARM ... mode= ok=`, `PTRS_CLASS ... latched a -> b (ok/tr without cpe excl-only, detector hits/runs)`.

## Risks for the other fixes
- `nr_pdsch_passive_decode.c` is hot: `phase` (CPE) and `irc`/`rm` touch the same body. My edits are confined to: includes, `rnti_dec_t`, the helper block before
  `prg_arm_unsupported`, the PT-RS refusal/G lines, the `t_ptrs_detect_only` return after the lifetime check, and `passive_decode_retry_impl` (start, after first body,
  new arm before `try_start`). `phase`: honour pduBitmap bit2. `rm`: G argument is now `csi_unav + ssb_unav + ptrs_unav`.
- ry gate index RY_ARMS-1 (127) is reserved for the PT-RS arm.
