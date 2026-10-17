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
 * What a pass does NOT cover (the caller keeps the old path, counted in nr_gpu_pass_stats): grants of more than 16 segments (32 in the v2 split mode) (PRG / VRB-bundle grants run in the segmented classes 1, 2, 4, 8, 16 [, 32]: the chest estimates per segment into scratch rows and
 * k_seg_compose lays them out in data order, the demod geometry is composed with the old path's RE gather), the chest cache, DM-RS type 2, a single-branch
 * estimate, probes, PT-RS, more than NR_GPU_PASS_MAX_CB code blocks, Z not a multiple of 4, HARQ soft combining, TBs whose decode needs the CPU decoder
 * (zero syndrome with a failing CRC: nr_gpu_pass_result_t::need_cpu), and unseen classes. */
#ifndef NR_GPU_PASS_H
#define NR_GPU_PASS_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_gpu_pass_post.h"
#include "nr_pdsch_prb_set.h"

#define NR_GPU_PASS_MAX_CB 32  /* code blocks per pass = NR_GPU_LDPC_MAX_JOBS */
#define NR_GPU_PASS_MAX_DMRS 4 /* DM-RS symbols of a grant */
#define NR_GPU_PASS_MAX_NL 4
#define NR_GPU_PASS_FP32_DEMOD 2 /* nr_gpu_pass_shape_t.fp32 bit: FP32 demod instantiation */
#define NR_GPU_PASS_MAX_SEG 32 /* segments of a segmented grant (the largest segment-count class; 32 only in the v2 split mode, ISAC_PDSCH_GPU_GRAPH_SPLIT) */
#define NR_GPU_PASS_MAX_SEG_FULL 16 /* the largest segment-count class of a single (v1 / non-split) graph */

#ifdef __cplusplus
extern "C" {
#endif

/* v2 (ISAC_PDSCH_GPU_GRAPH): the LLR statistics of passive_decode_body_core (nr_pdsch_passive_decode.c, the llr_* loops of the demod_ok block), on the device.
 * The first five fields stay contiguous and first: k_llr_stats_pre indexes them. */
typedef struct {
  uint64_t llr_n, llr_absum, llr_zero, llr_sat, llr_clip8, llr_pos; /* before descrambling, stride 32 (llr_pos: after, stride 32) */
  int64_t llr_sgnsum;                                               /* after descrambling, stride 32 */
  uint64_t posbit[2], nbit[2];                                      /* after descrambling, 4 LLRs per 32 */
} nr_gpu_llr_stats_t;
/* host side (nr_gpu_pass_dl.c): 1 = ISAC_PDSCH_GPU_GRAPH=1 and the module is up in v2 mode; nr_gpu_graph_flag("COVER"|"SPLIT"|"RETRY"|"SLOT"|"LEAN"):
 * ISAC_PDSCH_GPU_GRAPH_<sub>=1 and the master on */
int nr_gpu_graph_on(void);
int nr_gpu_graph_flag(const char *sub);

/* nrb_class: 6,12,24,48,96,136,217,273 (the smallest class >= the grant's PRBs); ncb_class: 1,2,4,8,16,32 (smallest >= C); nsym, qm: informational (they do not change the
 * graph); ndmrs: DM-RS symbols of the grant (1..4); fp32: bit 0 = 1 the float EVM chain (the product build), 0 the FP64 EVM chain (the chest post is always the float build);
 * bit 1 (NR_GPU_PASS_FP32_DEMOD) = the float instantiation of the demod kernels (Task 5, RTX 4070; not bit-exact), else the bit-exact double one. */
typedef struct {
  uint16_t nrb_class, nsym, nl, qm, ndmrs, ncb_class;
  uint8_t fp32;
  uint16_t nseg_class; /* Task 4b: 0 = a contiguous grant. 1, 2, 4, 8, 16 (32: split mode) = a segmented grant (PRG / VRB bundles) of up to that many segments: the chest runs one row set per segment into
                        * scratch rows and a compose kernel lays them out in data order (the estimate layout the demod reads); the smallest class >= the segments of the grant. */
  uint8_t ul;          /* Task 7: 1 = a PUSCH pass class (nl = 1, CP-OFDM, DM-RS type 1; nrb_class, ndmrs and ncb_class are its whole structure) */
} nr_gpu_pass_shape_t;

static inline int nr_gpu_pass_nrb_class(int nrb)
{
  static const int c[8] = {6, 12, 24, 48, 96, 136, 217, 273};
  for (int i = 0; i < 8; i++)
    if (nrb >= 1 && nrb <= c[i])
      return c[i];
  return 0;
}
static inline int nr_gpu_pass_nseg_class(int n) /* segmented-grant class: 1, 2, 4, 8, 16, 32; 0 = none (more segments than the largest class) */
{
  for (int c = 1; c <= NR_GPU_PASS_MAX_SEG; c <<= 1)
    if (n >= 1 && n <= c)
      return c;
  return 0;
}
static inline int nr_gpu_pass_ncb_class(int ncb)
{
  for (int c = 1; c <= NR_GPU_PASS_MAX_CB; c <<= 1)
    if (ncb >= 1 && ncb <= c)
      return c;
  return 0;
}

/* Segmented grant, demod geometry: `geo_y` (c16 [14][ext], .r = RX subcarrier, .i != 0 = valid) was built for the VIRTUAL contiguous allocation at BWP PRB 0 the old path demodulates, whose
 * subcarrier (off0 + i) % N holds the gathered RE i, rxdataF[(off0 + gidx[i]) % N] (gidx = the REs of the segments in data order). The graph reads rxdataF itself, so every valid index is
 * composed with that gather. 0 = done, -1 = an index outside the segments. off0 = (first_carrier_offset + BWPStart * 12) % N. */
static inline int nr_gpu_pass_seg_remap_geo(int16_t *geo_y, int ext, uint16_t sym_mask, const uint16_t *nre, int N, int off0, const nr_prb_seg_t *seg, int nseg)
{
  for (int m = 0; m < 14; m++) {
    if (!(sym_mask >> m & 1))
      continue;
    for (int i = 0; i < (int)nre[m]; i++) {
      int16_t *e = geo_y + 2 * ((size_t)m * ext + i);
      if (e[1] == 0)
        continue;
      const int v = ((e[0] - off0) % N + N) % N; /* virtual RE */
      int s = 0;
      while (s < nseg && v >= 12 * (seg[s].data_index + seg[s].n_prb))
        s++;
      if (s == nseg)
        return -1;
      e[0] = (int16_t)((off0 + seg[s].prb_start * 12 + (v - 12 * seg[s].data_index)) % N);
    }
  }
  return 0;
}


/* ---- Task 7: the PUSCH (UL) pass. One captured graph per class (nrb_class, ndmrs, ncb_class) and instance, the same four instances as the DL pass:
 *   k_ul_fep (the PUSCH symbols of every antenna, from the UL snapshot, with the cached timing advance) -> chest (k_ul_ls, the GPU6 delay transform, the 16-tap FIR) ->
 *   k_ul_level + k_ul_decide (the fixed-point scale of nr_rx_pusch_group_internal) -> k_ul_eq (extraction + nr_channel_compensation) -> k_ul_llr (nr_compute_llr + the
 *   unscrambling) -> k_rdm -> LDPC -> k_crc -> k_pick -> k_crc. Every step is integer arithmetic, so the pass is bit-exact with nr_rx_pusch_group_tp + nr_ulsch_decoding
 *   whatever ISAC_GPU_PASS_FP32 says (the FP32 instantiation only exists for the DL demod's MMSE).
 * Scope (anything else returns 1 from nr_gpu_pass_ul_try, counted, and the old path decodes): one layer (the passive UL receiver refuses more upstream), CP-OFDM, DM-RS type 1,
 * contiguous PRBs, normal CP, no PTRS, no frequency hopping, no HARQ combining (rv 0 of a fresh process), a TB of at most NR_GPU_PASS_MAX_CB code blocks, N = 4096. */
#define NR_GPU_PASS_UL_NSYM 14
typedef struct {
  int on;                       /* 1 = an UL job */
  /* ---- front end: the FFT window of symbol m of antenna a starts at iq[a] + win[m] (c16); win[m] < 0 = the symbol is not transformed */
  const int16_t *iq[4];
  uint32_t iq_len;              /* c16 readable from iq[a] (DISCRETE copies the span of the windows) */
  int32_t win[NR_GPU_PASS_UL_NSYM];
  int16_t rot[NR_GPU_PASS_UL_NSYM][2]; /* fp->symbol_rotation[link_type_ul][symbol + (slot % slots_per_subframe) * symbols_per_slot] */
  int rot_nb_rb, rot_fco;       /* apply_nr_rotation_symbol_RX(fp->N_RB_UL, fp->first_carrier_offset) of the old FEP */
  int16_t *rx[4];               /* the caller's rxdataF row of symbol 0 of the slot per antenna (c16, OAI layout): DIRECT = written in place by the pass; DISCRETE = filled after it; NULL = pass-private */
  /* ---- grant */
  int nrb, k0;                  /* PRBs, first subcarrier ((first_carrier_offset + (bwp_start + rb_start) * 12) % N) */
  int ndmrs, dmrs_sym[NR_GPU_PASS_MAX_DMRS];
  const int16_t *pil[NR_GPU_PASS_MAX_DMRS]; /* c16 pilots 6 * nrb (nr_pusch_dmrs_rx, the conjugate of the transmitted DM-RS) */
  int delta;                    /* get_delta(port) */
  int keep;                     /* the receive branch the passive UL equaliser uses (nr_ulsch_passive_keep_branch), -1 = all */
  uint16_t nre[NR_GPU_PASS_UL_NSYM];   /* data REs per symbol (get_nb_re_pusch; 0 = the symbol is not in the grant or carries no data) */
  uint8_t is_dmrs[NR_GPU_PASS_UL_NSYM];
  int8_t est_sym[NR_GPU_PASS_UL_NSYM]; /* the DM-RS symbol whose estimate the symbol uses */
  uint32_t llr_off[NR_GPU_PASS_UL_NSYM];
  int meas_sym;                 /* first symbol with data REs (the channel level is measured on it) */
  int16_t *est_out;             /* optional caller buffer [ndmrs * nbRx][4096] c16 (row = dmrs_index * nbRx + antenna): the chest estimates (the CFR / diagnostic consumers of ul_ch_estimates) */
} nr_gpu_pass_ul_t;

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
  /* ---- segmented grant (PRG / VRB-bundle segments in data order; nr_pdsch_prb_set.h). nseg == 0: the grant above. nseg >= 1: pil[][] points at the pilot sequence of RB 0
   * (seg_pil_nrb RBs, 6 c16 each); segment s is n_prb PRBs at pilot RB pil_rb, first LS subcarrier k0, and its estimate lands at data_index * 12 of the row. nrb is the sum of
   * the n_prb; k0 above is unused. */
  int nseg, seg_pil_nrb;
  struct { uint16_t n_prb, data_index; int pil_rb, k0; } seg[NR_GPU_PASS_MAX_SEG];
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
  int16_t *llr_out; /* optional caller buffer [G]: receives the final (descrambled, normalised) LLRs when the TB fails its CRC, so that the caller's retry arms can re-decode them */
  int llr_out_all;  /* 1 = llr_out is filled on CRC success too (tests, FP32 sign checks) */
  uint64_t t_entry_ns; /* Task 8b: CLOCK_MONOTONIC at nr_gpu_pass_{dl,ul}_run entry (0 = not stamped); the module histograms prep / launch-to-done / post */
  nr_gpu_pass_ul_t ul; /* Task 7: the PUSCH inputs of nr_gpu_pass_ul_try (ul.on); the TB fields above (A, C, K, Z, F, E, E2, first_rE2, tbslbrm, BG, R, R2, rv, Qm, it_max, G, scr, shape, llr_out) are shared */
  /* ---- v2 Task 2 (layout probes). 0 = the defaults (the whole grant) */
  uint32_t n_decode; /* code blocks decoded (blocks >= n_decode are padding; shape.ncb_class is the class of n_decode), 0 = C; K' / CRC type / E stay those of C */
  uint32_t G_dem;    /* LLRs the demod writes (a probe past its horizon: fewer); llr[G_dem, G) are zeroed, as the body's cleared buffer. 0 = G */
  uint8_t dmrs_type; /* v2 ISAC_PDSCH_GPU_GRAPH_DMRS2: 2 = DM-RS type 2 (pil[][]: 4 c16 per RB; the chest rows carry the type), 0 / 1 = type 1 */
  uint8_t probe;     /* 1 = a layout probe: never a decoded TB (tb_ok = 0, llr_out filled; outcome seg_ok[0]); the low-priority twin stream (ISAC_PDSCH_GPU_PRIO) */
} nr_gpu_pass_job_t;

/* the per-segment inputs of a segmented job as nr_pdsch_gpu_chest_grant's rows take them: pilot RB (prb_start + the BWP start unless the reference point is CRB 0) and first LS
 * subcarrier ((first_carrier_offset + (BWPStart + prb_start) * 12) as an unsigned short, % N). Returns the PRBs in total. */
static inline int nr_gpu_pass_seg_fill(nr_gpu_pass_job_t *j, const nr_prb_seg_t *seg, int nseg, int bwp_start, int ref_point, int first_carrier_offset, int N)
{
  int tot = 0;
  j->nseg = nseg;
  for (int s = 0; s < nseg; s++) {
    j->seg[s].n_prb = seg[s].n_prb;
    j->seg[s].data_index = seg[s].data_index;
    j->seg[s].pil_rb = seg[s].prb_start + (ref_point ? 0 : bwp_start);
    j->seg[s].k0 = (int)((uint16_t)(first_carrier_offset + (bwp_start + seg[s].prb_start) * 12) % N);
    tot += seg[s].n_prb;
  }
  return tot;
}

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
  /* ---- Task 7 (UL pass): row = dmrs_index * nbRx + antenna */
  int32_t ul_log2_maxh;          /* the fixed-point scale of the equaliser */
  int32_t ul_avg[4];             /* channel level of the measurement symbol per antenna (kept branches only) */
  int32_t ul_delay[16][3];       /* nr_passive_est_delay candidate of every chest row: est_delay, delay_max_pos, delay_max_val */
  uint64_t ul_acc[16];           /* noise sum of every chest row: sum |LS - H|^2 over 12 * nrb */
  uint64_t ul_hsum[16];          /* sum |H|^2 over 12 * nrb of every chest row */
  uint32_t ul_nrows;
  uint8_t ul_tb_crc;             /* TB-level CRC24A of a multi-block TB (hp_crc_failed's second condition) */
  nr_gpu_llr_stats_t llr_stats;  /* v2: the DL LLR statistics (k_llr_stats_pre / _post) */
} nr_gpu_pass_result_t;

/* Process-wide configuration, before nr_gpu_pass_init: the bit-exact chest reads the receiver's delay table (a fixed pointer, baked into the graphs). */
typedef struct {
  int nbRx;                    /* receive antennas, <= 4 */
  int n_rb_dl;                 /* fp->N_RB_DL (the DM-RS FO measurement) */
  const int16_t *delay_table;  /* c16 [2 * max_delay_comp + 1][dt_stride]: fp->delay_table[0] */
  int dt_stride, max_delay_comp;
  int v2;                      /* DL GPU data path v2 (ISAC_PDSCH_GPU_GRAPH): the graphs carry k_llr_stats; 0 = the v1 graph, unchanged */
  int split;                   /* v2 Task 3 (ISAC_PDSCH_GPU_GRAPH_SPLIT): a DL pass is three sub-graphs C (FEP + chest), D (chest post + demod + LLR post), L (decode), cached under
                                * separate keys (nrb, nl, ndmrs, nseg) / (nrb, nl, demod variant, EVM) / (ncb) and launched back to back; segment class 32 */
  int split_mb;                /* split: GPU memory cap of the pass (an estimate: the instance buffers' sizes + cudaMemGetInfo deltas around graph instantiation); warming stops at
                                * it; the instances alone at or above it turn split off (nr_gpu_pass_split_on). 0 = 1024 */
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
static inline uint64_t nr_gpu_pass_now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef void (*nr_gpu_pass_counters_fn)(uint64_t o[16]);
/* UL: launches, graph hits, fallbacks[unseen, busy, unsupported, error, ldpc key], tb_fail, need_cpu, host wait p50 / p99 <= (us, power-of-two bucket), classes */
void nr_gpu_pass_ul_counters(uint64_t o[16]);

int nr_gpu_pass_config(const nr_gpu_pass_cfg_t *cfg);
int nr_gpu_pass_init(int max_classes);
int nr_gpu_pass_warm(const nr_gpu_pass_shape_t *shape, int qmode, int align, int evm);
int nr_gpu_pass_dl_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r);
/* Task 7: the same for a PUSCH grant (j->ul.on); 0 = done (r filled: tb_ok, tb, seg_ok, the UL outputs), 1 = not handled (class not cached, no free instance, outside the scope), < 0 = error */
int nr_gpu_pass_ul_try(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r);
typedef int (*nr_gpu_pass_ul_try_fn)(const nr_gpu_pass_job_t *j, nr_gpu_pass_result_t *r);
void nr_gpu_pass_stats(char *buf, int n);
void nr_gpu_pass_counters(uint64_t o[16]);

#ifdef __cplusplus
}
#endif
#endif
