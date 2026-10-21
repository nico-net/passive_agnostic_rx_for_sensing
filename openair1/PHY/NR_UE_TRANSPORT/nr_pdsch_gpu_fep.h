/*
 * GPU front end for the passive PDSCH decoder: per-slot FEP (CP removal, FO de-rotation, batched
 * cuFFT, OAI symbol/timeshift rotation) with rxdataF kept resident on the device, then for every
 * probe/job of that slot DM-RS channel estimation (LS + fd-OCC despreading, linear frequency
 * interpolation, optional linear time interpolation), per-antenna noise whitening, MMSE layer
 * separation and max-log LLRs -- batched into one launch set and downloaded as int16 LLRs in
 * nr_rx_pdsch()'s order (RE-major, layer, bit; positive = bit 0).
 *
 * Built as libpdsch_gpu.so (ENABLE_LDPC_CUDA). The whole-slot LLR front end of this API is not loaded (ISAC_PDSCH_GPU=0 in the
 * production config); the bit-exact path dlopens the module itself (nr_pdsch_gpu_exact.c). Measured numbers and the reason LDPC was
 * NOT the part to offload: memory gpu-ldpc-on-sens6.
 */
#ifndef NR_PDSCH_GPU_FEP_H
#define NR_PDSCH_GPU_FEP_H

#include <stdint.h>
#include <stddef.h>
#include "PHY/TOOLS/nr_gpu_mem.h"

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

/** Launch-only exact FEP of one slot (nr_pdsch_gpu_fep_dev, nr_pdsch_gpu_exact.h GPU6 bit-exact path). */
typedef struct {
  const int16_t *const *rxdata; ///< per-antenna host IQ ring (ring_len samples)
  uint32_t ring_len, ring_offset;
  const int16_t *symbol_rot;    ///< symbols_per_slot OAI c16 {r,i}
  uint16_t sym_mask;            ///< symbols to transform
  int16_t *out;                 ///< nr_pdsch_gpu_fep_stage_at, DIRECT only: write this rxdataF (OAI layout, c16) instead of the buffer given to _dev; NULL = no redirect
  uint32_t out_stride;          ///< c16 between two antennas of `out`
  uint16_t run_mask;            ///< graph pass: symbols this replay transforms (0 = sym_mask); must be a subset of the captured sym_mask (the grid is baked, the other blocks return at once)
} nr_gpu_fep_job_t;
/* GPUPASS contract (graph-capturable): call nr_pdsch_gpu_fep_stage on the HOST first (waits until the slot's previous pass has finished
 * reading it, then copies the IQ window -- DISCRETE only -- and the per-slot origin/rotation into pinned staging; returns the slot,
 * 0 <= slot < NR_GPU_PASS_RING), then launch or replay nr_pdsch_gpu_fep_dev(j, rxF, slot, stream), which only enqueues device work (H2D
 * from that slot's fixed pinned memory on DISCRETE, the kernel, an event marking the slot free). A graph is captured per slot index;
 * rxdata pointers, ring_len and sym_mask of the captured job are baked in, the window position (ring_offset) and rotation are not. */
/* Fix A+D: the pass instances are three disjoint pools, each with its own streams, graph executables, FEP staging slot, demod and LDPC context (all indexed by the
 * instance), so a submission of one pool can never wait for an instance or a stream of another: real DL grants [0, DL), real UL grants [DL, DL + UL), probes (DL layout
 * probes, UL probes) the last NR_GPU_PASS_PROBE. See nr_gpu_pass_graph.cu. */
#define NR_GPU_PASS_DL 4
#define NR_GPU_PASS_UL 2
#define NR_GPU_PASS_PROBE 1
#define NR_GPU_PASS_RING (NR_GPU_PASS_DL + NR_GPU_PASS_UL + NR_GPU_PASS_PROBE)
typedef int (*nr_gpu_fep_stage_fn)(const nr_gpu_fep_job_t *j, int *slot_out);
typedef int (*nr_gpu_fep_dev_fn)(const nr_gpu_fep_job_t *j, nr_gpu_buf_t *rxF_dev, int slot, void *stream);
/* stage into a slot chosen by the caller (the rx pointers, ring length, origin and rotation are read from the slot when the kernel runs) */
typedef int (*nr_gpu_fep_stage_at_fn)(const nr_gpu_fep_job_t *j, int slot);

typedef struct {
  int (*init)(const nr_gpu_fep_cfg_t *cfg);
  /** Upload + FEP one slot: rxdata[ant] is the per-antenna sample ring (ring_len samples); the slot's
   *  symbol 0 CP starts at ring_offset (wraps). abs_sample is that same position as the FO phase
   *  origin (nr_fo_compensation's sample_offset). symbol_rot = fp->symbol_rotation[link] + symb_offset
   *  (symbols_per_slot entries as OAI c16 {r,i}; NULL = no rotation). Blocks until resident by
   *  default; the call always waits for the stream. */
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
  /** Copy one resident OFDM symbol to `out` (N c16 samples). Used when all self-check jobs
   *  share a first DM-RS symbol, avoiding the full-slot diagnostic readback. */
  int (*read_rxdataF_symbol)(int ant, int sym, int16_t *out);
} nr_gpu_fep_api_t;

#ifdef __cplusplus
}
#endif

#endif
