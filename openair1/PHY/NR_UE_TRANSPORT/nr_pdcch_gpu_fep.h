/*
 * GPU front end for the blind PDCCH CORESET-extent search: batched DM-RS LS channel estimation +
 * per-CCE-candidate equalisation + QPSK LLR generation, for MANY (extent, mapping) CORESET
 * hypotheses against ONE resident slot's already-FFT'd rxdataF -- the fep_llr stage that
 * nr_pdcch_blind_monitor_rt.c currently pays per-candidate (measured mean 12.8 us, max 121 us over
 * 438000 occasions on a real commercial-cell OTA capture, 2026-09-16).
 *
 * WHY THIS EXISTS. nr_pdcch_blind_monitor.c's CORESET blind search tries one (extent, mapping) pair
 * at a time (s_ext_idx/s_map_idx), each held for NR_PDCCH_EXTENT_VERIFY_OCC=4000 REAL occasions
 * before advancing -- up to 1035 extents x 512 mappings, WALL-CLOCK bound by real PDCCH occasion
 * arrival rate, not compute. The same capture's BTIM report showed `decode` (Polar SCL + CRC) at
 * mean 130.2 us -- 10x fep_llr and the dominant per-candidate cost -- with 433078/437131 (99%) of
 * occasions ALREADY over the 500 us slot budget at a single active hypothesis. Both facts point the
 * same way: testing many candidates per occasion, not one, is what removes the N x 4000 multiplier.
 * This module speeds up the chest+LLR half of that; the decode+CRC half deliberately stays on the
 * CPU -- see the header comment in nr_pdcch_blind_monitor.c's integration note for why (short
 * version: [[gpu-ldpc-on-sens6]] measured a naive GPU port of an even more parallel channel code
 * losing to CPU 20x on wall time / 6x on CPU cost without an async batched redesign; Polar SCL
 * decoding has MORE serial dependency per bit than LDPC's per-check-node updates, so it is a worse
 * GPU candidate, not a better one, and this module was built with no GPU hardware available to
 * validate a port of it).
 *
 * Mirrors nr_pdsch_gpu_fep.{h,cu}'s architecture exactly (dedicated GPU worker thread behind a
 * dlopen'd libpdcch_gpu.so, loaded only under NR_GPU_FEP=1 -- reusing the SAME knob and loader as
 * the PDSCH module, since the two never run concurrently on the same resident-slot GPU state and a
 * second env var would just be one more thing to forget to set) -- NOT a second, independently
 * mutex-contended path: commit 002038bd70 already found and fixed the "shared per-consumer mutex"
 * failure mode once for the PDSCH GPU FEP; this module reuses that lesson rather than re-earning it.
 *
 * STATUS (2026-09-16): written with no CUDA toolkit and no NVIDIA GPU available in this environment
 * (checked: no nvcc, no /usr/local/cuda, no nvidia-smi) -- the math is mirrored faithfully from
 * openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c's nr_pdcch_channel_estimation()
 * (non-CH_INTERP branch) and dci_nr.c's nr_gold_pdcch()/nr_pdcch_dmrs_ref()/nr_rx_pdcch_symbol(),
 * and self-checked only by inspection, NOT by running it. UNTESTED ON REAL GPU HARDWARE. Before
 * trusting it live: build on sens6 (same libldpc_cuda.so toolchain, CUDA 12.4 + g++-13, sm_89),
 * run the self-check (ISAC_GPU_PDCCH_SELFCHECK, mirroring nr_pdsch_gpu_fep's ISAC_GPU_SELFCHECK) on
 * a live capture, and do NOT flip the caller over to trusting its LLRs until that agrees with the
 * CPU path on a real occasion.
 *
 * KNOWN SIMPLIFICATION (ponytail: flagged, not silently dropped): multi-antenna combining here is
 * plain power-weighted MRC, not the branch-roughness gate nr_rx_pdcch_symbol() grew on 2026-09-15
 * (zeroing branches whose adjacent-subcarrier estimate roughness exceeds 4x the smoothest -- see
 * dci_nr.c's "BRANCH QUALITY GATE" comment). That heuristic was tuned against one X410 4-RX capture
 * and porting it sight-unseen to CUDA without hardware to re-verify would be guessing, not mirroring.
 * Upgrade path: port it once this module is GPU-validated and a rank>1 capture is available to
 * confirm the port against the CPU gate's own output.
 */
#ifndef NR_PDCCH_GPU_FEP_H
#define NR_PDCCH_GPU_FEP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int nant;                 ///< receive antennas uploaded per slot (<= 4)
  int ofdm_symbol_size;     ///< N (4096 at 273 PRB / 30 kHz)
  int symbols_per_slot;     ///< 14
  int first_carrier_offset; ///< OAI rxdataF index of CRB0 subcarrier 0
  int n_rb_dl;               ///< carrier width in RB
} nr_gpu_pdcch_fep_cfg_t;

/// One CORESET-extent candidate ((extent, mapping) hypothesis) scored against the resident slot's
/// already-FFT'd CORESET symbols. CCE candidates of this hypothesis share the SAME channel estimate
/// (chest is over the whole CORESET RB range, CCE/REG bundling only affects demap/interleave, which
/// stays on the CPU exactly as today -- see the .cu file's top comment for why the split sits here).
typedef struct {
  /// The three RB quantities dci_nr.c's nr_rx_pdcch_symbol()/nr_pdcch_channel_estimation() keep
  /// separate -- deliberately NOT collapsed into one field, because they enter the RE position and
  /// the pilot/Gold sequence index differently (see nr_pdcch_gpu_fep.cu's chest kernel comment):
  uint16_t rb_offset;            ///< coreset_start_rb: CORESET first RB, CRB0-relative, EXCLUDING BWPStart
  uint16_t bwp_start_rb;         ///< BWPStart; added into the RE position only (0 for CORESET#0's own frame)
  uint16_t dmrs_ref_rb;          ///< dmrs_ref: 0 for a common CORESET, == bwp_start_rb for a UE-specific one;
                                  ///< added into the pilot/Gold sequence index only
  uint16_t n_rb;                 ///< CORESET width in RB (6 * num_freq_domain_groups)
  uint8_t  start_symbol, duration; ///< first CORESET symbol (slot-relative) and its duration (1..3)
  uint16_t dmrs_scrambling_id;   ///< coreset->pdcch_dmrs_scrambling_id
  uint8_t  slot;                 ///< n_s,f of the resident slot, for c_init
  uint32_t llr_offset;           ///< out: this job's first LLR (QPSK soft bit pair, int16) in the output buffer
  uint32_t n_llr;                ///< out: 2 * n_rb * RE_PER_RB_OUT_DMRS * duration int16 LLRs (0 = unsupported -> CPU fallback)
} nr_gpu_pdcch_job_t;

typedef struct {
  int (*init)(const nr_gpu_pdcch_fep_cfg_t *cfg);
  /// Upload the CORESET-duration symbols' per-antenna rxdataF for the resident slot (already FFT'd
  /// by the CPU's existing nr_slot_fep -- see the .cu top comment for why this module does NOT redo
  /// the FFT: it is already shared across every extent candidate regardless of RB range, and at
  /// 12.8 us mean it was never the measured bottleneck). rxdataF_symb[ant] is CORESET-RELATIVE:
  /// max_duration * ofdm_symbol_size c16 samples, row 0 == the CORESET's own first (slot-relative)
  /// symbol, NOT row `start_symbol` of a full-slot buffer -- the caller slices before calling this.
  /// Every job passed to the next pdcch_llr() call MUST share the same (start_symbol, duration) as
  /// this upload (only CORESET frequency range may vary across jobs of one call -- see
  /// nr_pdcch_blind_monitor.c's integration note for why that is already this search's structure).
  /// Blocks until resident.
  int (*upload_slot)(const int16_t *const *rxdataF_symb, int max_duration);
  /// Chest + equalise + QPSK-LLR for n jobs of the resident slot; LLRs land in llr_out (llr_cap
  /// int16s) at each job's llr_offset, as (r,i) PAIRS per RE -- llr_out cast to c16_t* is directly
  /// nr_pdcch_demapping_deinterleaving()'s own pdcch_llr_b input type, no repacking needed -- in
  /// symbol-major, then increasing-RB, then increasing-RE-within-RB order (DM-RS REs excluded),
  /// matching nr_pdcch_generate_llr()'s existing per-symbol-buffer convention. Returns total int16s
  /// written (== 2 * sum of n_llr's RE-pair count), < 0 on error.
  int64_t (*pdcch_llr)(nr_gpu_pdcch_job_t *jobs, int n, int16_t *llr_out, size_t llr_cap);
  void (*timing)(double *upload_us, double *llr_us);
} nr_gpu_pdcch_fep_api_t;

#ifdef __cplusplus
}
#endif

#ifndef NR_GPU_FEP_NO_LOADER
#include <dlfcn.h>
#include <stdlib.h>
/** dlopen libpdcch_gpu.so next to the executable when NR_GPU_FEP=1 (the SAME knob as the PDSCH GPU
 * FEP loader -- see this file's top comment for why one knob is deliberate, not an oversight). */
static inline const nr_gpu_pdcch_fep_api_t *nr_gpu_pdcch_fep_load(void)
{
  static const nr_gpu_pdcch_fep_api_t *api = NULL;
  static int tried = 0;
  if (tried)
    return api;
  tried = 1;
  const char *e = getenv("NR_GPU_FEP");
  if (e == NULL || atoi(e) == 0)
    return NULL;
  void *h = dlopen("libpdcch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    h = dlopen("./libpdcch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    return NULL;
  const nr_gpu_pdcch_fep_api_t *(*get)(void) = (const nr_gpu_pdcch_fep_api_t * (*)(void)) dlsym(h, "nr_gpu_pdcch_fep_api");
  api = get ? get() : NULL;
  return api;
}
#endif

#endif // NR_PDCCH_GPU_FEP_H
