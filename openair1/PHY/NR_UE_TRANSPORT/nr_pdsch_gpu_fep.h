/*
 * GPU front end for the passive PDSCH decoder: per-slot FEP (CP removal, FO de-rotation, batched
 * cuFFT, OAI symbol/timeshift rotation) with rxdataF kept resident on the device, then for every
 * probe/job of that slot DM-RS channel estimation (LS + fd-OCC despreading, linear frequency
 * interpolation, optional linear time interpolation), per-antenna noise whitening, MMSE layer
 * separation and max-log LLRs -- batched into one launch set and downloaded as int16 LLRs in
 * nr_rx_pdsch()'s order (RE-major, layer, bit; positive = bit 0).
 *
 * Built as libpdsch_gpu.so (ENABLE_LDPC_CUDA); loaded at run time by nr_gpu_fep_load() when
 * NR_GPU_FEP=1, so a build without CUDA -- or a run without the knob -- is the CPU path exactly as
 * before. Measured numbers and the reason LDPC was NOT the part to offload: memory gpu-ldpc-on-sens6.
 */
#ifndef NR_PDSCH_GPU_FEP_H
#define NR_PDSCH_GPU_FEP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int nant;                 ///< receive antennas uploaded per slot (<= 4)
  int ofdm_symbol_size;     ///< N (4096 at 273 PRB / 30 kHz)
  int nb_prefix_samples;    ///< normal CP
  int nb_prefix_samples0;   ///< long CP of symbol 0
  int symbols_per_slot;     ///< 14
  int first_carrier_offset; ///< OAI rxdataF index of CRB0 subcarrier 0
  int n_rb_dl;              ///< carrier width in RB
  int samples_per_ms;       ///< sample rate / 1000 (FO de-rotation unit, as nr_fo_compensation)
  int ofdm_offset_divisor;  ///< the FFT window starts nb_prefix_samples/divisor early (OAI: 8)
} nr_gpu_fep_cfg_t;

/** One PDSCH hypothesis (a full TB decode or a first-code-block probe) on the resident slot. */
typedef struct {
  uint16_t start_rb;             ///< first RB, CRB0-relative (rxdataF index = first_carrier_offset + 12*rb)
  uint16_t nb_rb;
  uint8_t start_symbol, nb_symbols;
  uint16_t dmrs_mask;            ///< bit l set = symbol l carries DM-RS
  uint8_t dmrs_type;             ///< 1 or 2
  uint8_t n_cdm_groups_no_data;  ///< 1..3
  uint8_t Nl;                    ///< layers (<= nant, <= 4)
  uint8_t ports[4];              ///< DM-RS port per layer, 0..11 (1000 stripped)
  uint8_t Qm;                    ///< 2, 4, 6, 8
  uint16_t dmrs_scrambling_id;   ///< N_ID (cell id or the configured scrambling id)
  uint8_t nscid;
  uint16_t dmrs_ref_rb;          ///< RB where the DM-RS sequence index m starts at 0 (CRB0 or the BWP start)
  uint8_t slot;                  ///< n_s,f of the resident slot, for c_init
  uint8_t time_interp;           ///< 1 = linear between DM-RS symbols (ISAC_PDSCH_TINTERP), 0 = hold
  uint32_t max_llr;              ///< 0 = every LLR of the allocation; a probe asks for its first code block only
  uint32_t llr_offset;           ///< out: this job's first LLR in the output buffer
  uint32_t n_llr;                ///< out: LLRs written (0 = unsupported, caller falls back to the CPU)
} nr_gpu_pdsch_job_t;

typedef struct {
  int (*init)(const nr_gpu_fep_cfg_t *cfg);
  /** Upload + FEP one slot: rxdata[ant] is the per-antenna sample ring (ring_len samples); the slot's
   *  symbol 0 CP starts at ring_offset (wraps). abs_sample is that same position as the FO phase
   *  origin (nr_fo_compensation's sample_offset). symbol_rot = fp->symbol_rotation[link] + symb_offset
   *  (symbols_per_slot entries as OAI c16 {r,i}; NULL = no rotation). Blocks until rxdataF is resident. */
  int (*fep_slot)(const int16_t *const *rxdata, uint32_t ring_len, uint32_t ring_offset, uint32_t abs_sample,
                  double fo_hz, const int16_t *symbol_rot);
  /** Chest + MMSE + LLR for n jobs of the resident slot; LLRs land in llr_out (llr_cap int16s) at each
   *  job's llr_offset. Returns the total LLR count, < 0 on error. */
  int64_t (*pdsch_llr)(nr_gpu_pdsch_job_t *jobs, int n, int16_t *llr_out, size_t llr_cap);
  /** Copy of the resident rxdataF for antenna `ant` (symbols_per_slot * N c16 samples, OAI layout) --
   *  for the LLR/CRC self-check against the CPU path, not the fast path. */
  int (*read_rxdataF)(int ant, int16_t *out);
  /** Last timings (us): FEP of the last slot, and the last pdsch_llr launch set. */
  void (*timing)(double *fep_us, double *llr_us);
} nr_gpu_fep_api_t;

#ifdef __cplusplus
}
#endif

#ifndef NR_GPU_FEP_NO_LOADER
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
/** dlopen libpdsch_gpu.so next to the executable when NR_GPU_FEP=1; NULL = CPU path. */
static inline const nr_gpu_fep_api_t *nr_gpu_fep_load(void)
{
  static const nr_gpu_fep_api_t *api = NULL;
  static int tried = 0;
  if (tried)
    return api;
  tried = 1;
  const char *e = getenv("NR_GPU_FEP");
  if (e == NULL || atoi(e) == 0)
    return NULL;
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    return NULL;
  const nr_gpu_fep_api_t *(*get)(void) = (const nr_gpu_fep_api_t * (*)(void)) dlsym(h, "nr_gpu_fep_api");
  api = get ? get() : NULL;
  return api;
}
#endif

#endif
