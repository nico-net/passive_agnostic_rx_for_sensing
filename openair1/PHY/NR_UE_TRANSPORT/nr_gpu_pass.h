/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4: the DL decode pass of one PDSCH grant as ONE captured CUDA graph per shape class, one host wait per pass (ISAC_GPU_PASS=1, default off).
 *
 * Graph (per shape class and per instance), all on one stream:
 *   [H2D of the parameter blocks (DISCRETE only)] -> exact FEP (all 14 symbols) -> exact chest -> chest post (nvar + DM-RS FO/SFO sums) ->
 *   demod (gather, level, mask, [align], [mmse3 prep], tail; the EVM chain on a forked/joined branch when the class wants it; nvar read from the chest post's
 *   device output) -> descramble -> LLR norm shift -> rate dematching -> LDPC -> CRC of every snapshot -> first-passing-snapshot pick (OAI's stop rule) ->
 *   CB/TB CRC -> [D2H of the result blocks].
 * Per launch only the CONTENTS of device-accessible parameter blocks change (the kernels read them when they run): staged FEP/chest slots, the demod job block
 * (jref), the chest-post parameters, the rate-dematching / CRC tables, the LDPC DJob array. No cudaGraphExecKernelNodeSetParams is needed; the by-value
 * arguments of a captured kernel never change.
 *
 * Concurrency: NR_GPU_PASS_RING (4) pass instances, each with its own buffers, staging slot (= instance index), demod context and LDPC context. A pass takes
 * a free instance for its whole duration (compare-and-swap on a bit mask); none free -> nr_gpu_pass_dl_try returns 1 (old path, counted). So at most 4 passes are in
 * flight per process, whatever the number of consumer threads (the PDSCH queue has up to 5). Nothing is allocated after the RX fence: buffers, streams,
 * events and graphs exist from nr_gpu_pass_init(); an unseen shape class returns 1.
 *
 * What a pass does NOT cover (the caller keeps the old path, counted in nr_gpu_pass_stats): segmented / PRG grants, the chest cache, DM-RS type 2, a single-branch
 * estimate, probes, PT-RS, more than NR_GPU_PASS_MAX_CB code blocks, Z not a multiple of 4, HARQ soft combining, TBs whose decode needs the CPU decoder
 * (zero syndrome with a failing CRC: nr_gpu_pass_result_t::need_cpu), and unseen classes. */
#ifndef NR_GPU_PASS_H
#define NR_GPU_PASS_H
#include <stdint.h>
#include <stddef.h>
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_gpu_pass_post.h"

#define NR_GPU_PASS_MAX_CB 32  /* code blocks per pass = NR_GPU_LDPC_MAX_JOBS */
#define NR_GPU_PASS_MAX_DMRS 4 /* DM-RS symbols of a grant */
#define NR_GPU_PASS_MAX_NL 4

#ifdef __cplusplus
extern "C" {
#endif

/* nrb_class: 6,12,24,48,96,136,217,273 (the smallest class >= the grant's PRBs); ncb_class: 1,2,4,8,16,32 (smallest >= C); nsym, qm: informational (they do not change the
 * graph); ndmrs: DM-RS symbols of the grant (1..4); fp32: 1 = the float EVM chain (the product build), 0 = the FP64 EVM chain (the chest post is always the float build). */
typedef struct {
  uint16_t nrb_class, nsym, nl, qm, ndmrs, ncb_class;
  uint8_t fp32;
} nr_gpu_pass_shape_t;

static inline int nr_gpu_pass_nrb_class(int nrb)
{
  static const int c[8] = {6, 12, 24, 48, 96, 136, 217, 273};
  for (int i = 0; i < 8; i++)
    if (nrb >= 1 && nrb <= c[i])
      return c[i];
  return 0;
}
static inline int nr_gpu_pass_ncb_class(int ncb)
{
  for (int c = 1; c <= NR_GPU_PASS_MAX_CB; c <<= 1)
    if (ncb >= 1 && ncb <= c)
      return c;
  return 0;
}

/* Everything the body computes from the DCI before the first kernel. Host buffers are read during nr_gpu_pass_dl_try only. */
typedef struct {
  nr_gpu_pass_shape_t shape;
  /* ---- front end: the FEP of the slot (all 14 symbols) from the receiver's IQ ring */
  const int16_t *rxdata[4];
  uint32_t ring_len, ring_offset;
  const int16_t *symbol_rot;   /* symbols_per_slot OAI c16, fp->symbol_rotation[DL] + slot offset */
  int16_t *rxdataF;            /* the caller's rxdataF[nbRx][rx_stride] (c16, OAI layout) or NULL; DIRECT: filled in place by the GPU; DISCRETE: filled after the pass */
  uint32_t rx_stride;          /* c16 between two antennas of rxdataF */
  uint16_t copy_mask;          /* symbols of rxdataF the caller needs (DISCRETE copy-back) */
  /* ---- channel estimate, DM-RS type 1 (one row per DM-RS symbol x layer x antenna) */
  int nbRx, nl, ndmrs;
  int dmrs_sym[NR_GPU_PASS_MAX_DMRS];
  int delta[NR_GPU_PASS_MAX_NL];                                 /* get_delta(port) of each layer */
  const int16_t *pil[NR_GPU_PASS_MAX_DMRS][NR_GPU_PASS_MAX_NL];  /* c16 pilots of (DM-RS symbol, layer): 6 * nrb values from the grant's first RB */
  int nrb, k0;                                                   /* PRBs of the grant, first LS subcarrier (bwp_start_subcarrier % N) */
  /* ---- chest post: nvar normalisation + per-branch substitution, DM-RS FO/SFO sums */
  int number_symbols, nvfix, branch;                             /* branch: nr_dlsch_planned_branch() when ISAC_RX_NVAR_PERBRANCH, else -1 */
  int fo_first, fo_last;                                         /* DM-RS indices (into dmrs_sym) of the first and last DM-RS symbol, -1 = no FO measurement */
  double dt_d, df_d;
  /* ---- demod: everything nr_pdsch_gpu_demod_grant resolves; the pointer fields and nvar are set by the module */
  nr_gpu_demod_job_t dj;
  const int16_t *geo_y, *geo_h;                                  /* c16 [14][dj.ext]: nr_dlsch_extract_geometry's index maps */
  /* ---- LLR post */
  uint32_t G;
  const uint32_t *scr;                                           /* gold_cache(c_init, ceil(G/32)): descrambling words, bit set = flip */
  int norm;                                                      /* K38 LLR normalisation (ISAC_LLR_NORM on and G >= 64) */
  uint32_t norm_span;                                            /* nr_llr_norm_span(G, C) */
  /* ---- decoder (the TB parameters passive_ldpc_decode_core derives) */
  uint32_t A;                                                    /* TBS */
  uint32_t C, K, Z, F, E, E2, first_rE2, tbslbrm;
  uint8_t BG, R, R2, rv, Qm, it_max;
  /* ---- the symbol whose equalised layer-0 samples the host's modulation-order oracle reads (the one with the most data REs) and how many of them */
  int qm_sym;
  uint32_t qm_n;
} nr_gpu_pass_job_t;

typedef struct {
  uint8_t tb_ok;
  uint8_t seg_ok[NR_GPU_PASS_MAX_CB];
  uint32_t tbs;
  float evm_db, snr_db, nvar_db;
  uint8_t *tb;       /* caller's buffer, >= (lenWithCrc(1, A) + 7) / 8 + 8 bytes: receives the reassembled TB (the CRC bytes of a single-CB TB included) */
  uint32_t gpu_us;   /* launch to completion */
  /* ---- appended */
  int8_t instance;   /* the pass instance that ran it (diagnostics) */
  uint8_t need_cpu;  /* OAI's decoder would iterate on a block the GPU stopped on (zero syndrome, failing CRC): the TB must be redone on the CPU */
  uint32_t nvar;     /* what the demod used */
  int32_t log2_maxh;
  nr_gpu_chest_post_out_t post;
  nr_gpu_demod_job_t *demod_out; /* optional caller buffer: the decisions, kappa and EVM sums of the demod (for the host fold) */
  int16_t *brfo_rows;            /* optional caller buffer [nbRx][2][4096] c16: layer-0 estimates of the first and last DM-RS symbol */
  int16_t *qm_row;               /* optional caller buffer [4096] c16: job.qm_n equalised layer-0 samples of symbol job.qm_sym (DIRECT model only: qm_valid) */
  uint8_t qm_valid;
  uint32_t nvar_ant[4];          /* the chest's per-antenna noise of the last DM-RS symbol and layer (what nr_dl_chest_nvar_ant holds after the old chest) */
} nr_gpu_pass_result_t;

/* Process-wide configuration, before nr_gpu_pass_init: the bit-exact chest reads the receiver's delay table (a fixed pointer, baked into the graphs). */
typedef struct {
  int nbRx;                    /* receive antennas, <= 4 */
  int n_rb_dl;                 /* fp->N_RB_DL (the DM-RS FO measurement) */
  const int16_t *delay_table;  /* c16 [2 * max_delay_comp + 1][dt_stride]: fp->delay_table[0] */
  int dt_stride, max_delay_comp;
} nr_gpu_pass_cfg_t;
typedef int (*nr_gpu_pass_config_fn)(const nr_gpu_pass_cfg_t *cfg);
/* pre-fence: allocate the instances and record + instantiate one graph per warm class. max_classes bounds the default warm set
 * (nrb_class 24/48/96/217 x nl 1..4, ndmrs 2, ncb_class 1..32 by size; float chest post/EVM without EVM chain and the grant variants the job asks for). */
typedef int (*nr_gpu_pass_init_fn)(int max_classes);
/* warm one more class (pre-fence); evm = the graph carries the EVM chain; qmode/align: the demod variants (dj.qam_mode, dj.align != 0) */
typedef int (*nr_gpu_pass_warm_fn)(const nr_gpu_pass_shape_t *shape, int qmode, int align, int evm);
/* 0 = done (r filled); 1 = not handled (class not cached, no free instance, unsupported variant): the caller uses the old path; < 0 = error after the
 * launch started (the caller uses the old path as well). */
typedef int (*nr_gpu_pass_dl_try_fn)(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r);
typedef void (*nr_gpu_pass_stats_fn)(char *buf, int n);
/* counters: launches, graph hits, fallbacks[unseen, busy, unsupported, error], need_cpu, tb_fail, instances, classes */
typedef void (*nr_gpu_pass_counters_fn)(uint64_t o[16]);

int nr_gpu_pass_config(const nr_gpu_pass_cfg_t *cfg);
int nr_gpu_pass_init(int max_classes);
int nr_gpu_pass_warm(const nr_gpu_pass_shape_t *shape, int qmode, int align, int evm);
int nr_gpu_pass_dl_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r);
void nr_gpu_pass_stats(char *buf, int n);
void nr_gpu_pass_counters(uint64_t o[16]);

#ifdef __cplusplus
}
#endif
#endif
