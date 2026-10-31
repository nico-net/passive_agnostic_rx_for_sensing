# fix1031/mlcfr -- S7: data-aided CFR for multi-layer PDSCH (Nl 2..4)

Base mpci-int-1031 @743b622ffd. Build: DGX aarch64, fresh dir, `nr-uesoftmodem pdsch_gpu ldpc_cuda` + tests, all through heavy.sh -j6.

## What changed
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_mimo_ls.{c,h}` (new, pure, no static state): windowed LS `Y = H X + N`. Per window: Gram G = X^H X,
  Gauss-Jordan inverse, h = G^-1 X^H y per RX; skipped when some layer's noise gain G_ll (G^-1)_ll > 10 (numerical bound, singular -> skipped).
  Window width = widest of 2Nl, 4Nl, ... 256 whose LS residual per dof stays within 3 sigma (chi-square spread) of the 2Nl window's residual
  (flat channel to within the noise), evaluated on <= ~64 sampled windows per width; derived from the received data, no cell constant.
  The brief asked for the chest's delay spread; the chest does not expose one in the passive path (shared-delay mode is gone), so flatness is measured from the data instead.
- `nr_pdsch_data_aided.c`: Nl 1 guard replaced (Nl 2..4 reconstructed, Nl > 4 skipped with a one-shot warning). RE enumeration for Nl>1 gathers Y
  and consumes Nl modulation symbols per RE (x_l(i) = d(Nl*i+l)); windows never cross a symbol or a k gap > 6; centre RE k_abs, window raw count kept for the
  sub-slot SNR/coverage gates. New `da_emit`: Nl=1 = the original single call; Nl>1 = one frame per layer (ports=1, port_mask = l-th lowest DM-RS port,
  `layer`, `n_layers`). Buffers sized for G = REs*Qm*Nl (coded_bits, mod_syms, scrambled now heap, segments MAX_NUM_NR_DLSCH_SEGMENTS). The decode_G == tap_G refusal
  (PT-RS/CSI-RS/SSB holes) is unchanged and applies to Nl>1 too.
- `nr_sense_records.{h,c}`, `nr_isac.cc`: rs meta `layer_idx1`/`n_layers`; JSON keys `layer` (0-based), `n_layers` only when set (single-layer records byte-identical).
- CMake (library + `test_nr_pdsch_mimo_ls`), `tools/mpci/mpci_audit_files.txt`, `mpci_statics.tsv` (3 new TLS rows, regenerated lines).

## Tests [OFFLINE VERIFIED, DGX aarch64 unit]
- `test_nr_pdsch_mimo_ls` 8/8: 4-RX frequency-selective synthetic, 15 dB, 5 seeds, NMSE vs truth: LS vs DM-RS-style comb LS+interp:
  Nl2 0.0022 vs 0.0447; Nl3 0.0035 vs 0.0802; Nl4 0.0072 vs 0.0954. Noiseless exactness; singular (identical layers) window skipped and nothing exported; partially singular grant exports only the good half;
  ladder window shrinks with delay spread; windows stay inside symbol/run.
- `test_nr_sense_records` (layer keys present only for layer frames), `tools/sense_api/test_read_sense_layers.py` (3 layer frames round-trip via read_sense.load), `static_audit_check` pass.
## NOT verified
- Nl=1 byte compare of the full submit: not unit-tested (the only harness, nr_ssb_rate_match_prod_check, is a known ARM failure and needs a full UE). Nl=1 is identical by construction (same enumeration branch, `da_emit` nl<=1 = original call, gate arithmetic unchanged since sym_raw == sym_count and g_out > 0); needs a replay diff of pdsch_data frames/records.
- No replay/OTA run. The test DM-RS reference is a synthetic comb estimator, not the receiver's chest.
## Expected on-air effect [HYPOTHESIS]
Nl>=2 CRC-ok grants (earlier: skipped, sense_unexported_pdsch_data 1442 / 60 s) now export Nl frames each; the one-shot "data-aided CFR skipped -- Nl=" warning disappears, replaced at most by "no estimable LS window". Rate of pdsch_data frames rises roughly by Nl x (multi-layer grants); records with `layer`/`n_layers`. CPU: ladder is sampled; final pass ~ nre*Nl*(Nl+nant) complex MACs, plus <=1.5 MB per-thread heap.
## Risks / overlap
Other fixes touching `nr_pdsch_data_aided.c` (tail / rs_meta) or `nr_sense_rs_meta_t` will conflict textually; consumers must tolerate more than one frame per slot with identical k/l (one per layer). `nr_isac_pdsch_data_aided_submit` is not fed to the sensing engine, only exported.
