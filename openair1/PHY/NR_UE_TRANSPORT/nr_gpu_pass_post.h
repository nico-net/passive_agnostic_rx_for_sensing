/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 3: the host steps between the GPU stages as launch-only kernels (implementation: nr_gpu_pass_post.cuh, in libpdsch_gpu.so).
 * Every nr_gpu_pass_launch_* only enqueues on `stream` (no host wait, no allocation, graph-capturable); pointers are device-accessible,
 * segment tables too (their contents are read when the kernel runs). Return 0, or -1 when the launch itself failed. */
#ifndef NR_GPU_PASS_POST_H
#define NR_GPU_PASS_POST_H
#include <stdint.h>

/* ---- chest post: the CPU math after the GPU chest in passive_decode_body_core (non-segmented path): nvar normalisation + per-branch
 * substitution (3697-3732) and the DM-RS phase-slope CFO/SFO measurement (3740-3990). The EMA, the s_apply hook and the logging stay on
 * the host: they read `out` after the pass. */
#define NR_GPU_POST_MAX_SEG 16
typedef struct {
  const uint32_t *nvar_rows; /* chest nvar u32[row], row = (k * nrx + a); k = dmrs_index * nl + layer, as the GPU chest job orders them */
  int nrx, nl, n_dmrs_sym;   /* k < n_dmrs_sym * nl */
  int number_symbols;        /* dlsch_config->number_symbols (nvar divisor unless nvfix) */
  int nvfix;                 /* ISAC_RX_NVAR_FIX */
  int branch;                /* nr_dlsch_planned_branch() when ISAC_RX_NVAR_PERBRANCH, else -1 */
  const int16_t *h0, *h1;    /* c16 rows of layer 0 / antenna 0 at the first and the last DM-RS symbol (N entries); NULL = no FO measurement */
  int N, n_rb_dl;            /* ofdm_symbol_size, fp->N_RB_DL */
  double dt_d, df_d;         /* seconds between the two DM-RS symbols, subcarrier spacing in Hz */
  float dt_f, df_f;          /* the same, rounded to float (the float kernel never touches FP64) */
  /* SEGMENTED estimate (Task 4b: PRG / VRB bundles, one chest row set per segment as the passive loop's segmented path combines them): nseg >= 2 = rows are
   * k = (dmrs_index * nl + layer), row (k * nseg + s) * nrx + a; segment s is seg_w[s] PRBs wide (0 = padding), nrb_total = the grant's PRBs. The per-(symbol,
   * layer) nvar is sum_s mean_a(q) * w_s / nrb_total (u64 math, u32 truncation), nvar_ant[a] = sum_s q[a] * w_s / nrb_total of the last (symbol, layer).
   * 0 (zero-initialised) = the contiguous grant, as before. */
  int nseg, nrb_total;
  uint16_t seg_w[NR_GPU_POST_MAX_SEG];
} nr_gpu_chest_post_par_t;
enum { NR_GPU_POST_FO_VALID = 1, NR_GPU_POST_SFO_MEASURED = 2, NR_GPU_POST_FO_REJECTED = 4 };
typedef struct { /* float build (the product one) */
  uint32_t nvar_valid; /* 0 when n_dmrs_sym * nl == 0: nvar is NOT written (the CPU keeps its previous value) and the caller must keep its own */
  uint32_t nvar;  /* what nr_rx_pdsch()/the demod job takes: exact (integer math) */
  uint32_t flags; /* NR_GPU_POST_* */
  float cfo_hz, sfo_ppm, sfo_dk, coh_gate, coherence;
  int32_t kn0, kn1;
  int64_t re[2], im[2], ksum[2]; /* the exact integer sums per half band (all the angles are functions of these) */
  float magsum[2];
  uint32_t nvar_ant[4]; /* the chest's per-antenna noise as nr_dl_chest_nvar_ant holds it after the estimate (last DM-RS symbol and layer; width-weighted when segmented) */
} nr_gpu_chest_post_out_t;
/* FP64 build: everything is bit-equal to the CPU except cfo_hz / sfo_ppm, which go through atan2(): CUDA's double atan2 (2 ulp) differs from glibc's in
 * the last bits on about a quarter of the inputs. The sums above are exact, so a host that needs the CPU's bits runs the last scalar lines on them. */
typedef struct {
  uint32_t nvar_valid;
  uint32_t nvar;
  uint32_t flags;
  double cfo_hz, sfo_ppm, sfo_dk, coh_gate, coherence;
  int32_t kn0, kn1;
  int64_t re[2], im[2], ksum[2];
  double magsum[2];
  uint32_t nvar_ant[4];
} nr_gpu_chest_post_out64_t;
typedef int (*nr_gpu_pass_chest_post_fn)(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out_t *out_dev, void *stream);
typedef int (*nr_gpu_pass_chest_post64_fn)(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out64_t *out_dev, void *stream);

/* ---- k_rdm: nr_deinterleaving_ldpc + nr_rate_matching_ldpc_rx (clear = 1) + the pack of prep_segment (nr_pdsch_gpu_ldpc_host.c) into the
 * int8 input of the GPU LDPC kernel: segment i writes Kc * Z bytes at ldpc_in + i * NR_GPU_LDPC_DEV_IN_STRIDE. A segment the CPU refuses
 * (C == 0, Foffset > Ncb, E % Qm != 0, Qm not 2/4/6/8, nothing selectable) is written as zeros; the host knows it from the same parameters. */
typedef struct {
  uint32_t llr_off; /* element offset of the segment's E LLRs in the llr array */
  uint32_t E, Qm, BG, Z, K, F, Kprime, Tbslbrm, C, rv;
} nr_gpu_rdm_seg_t;
typedef int (*nr_gpu_pass_rdm_fn)(const int16_t *llr, const nr_gpu_rdm_seg_t *segs, int nseg, int8_t *ldpc_in, void *stream);

/* ---- k_crc: check_crc() per code block and, for nseg > 1, over the reassembled TB (what passive_ldpc_decode_core checks). Block i of
 * `bits` is bytes [off, off + ceil(n_bits / 8)); n_bits includes the CRC; type 0 = CRC24A, 1 = CRC24B, 2 = CRC16 (coding_defs.h). The TB
 * is the concatenation of pay_bytes of each segment (K/8 - F/8 - 3), its CRC over lenWithCrc(1, tbs) bits. seg_ok[i] / tb_ok are 0/1;
 * for nseg == 1 tb_ok = seg_ok[0]. tb_ok = 0 when nseg < 1, segs[0].pay_bytes == 0 or the segments hold fewer bytes than the TB needs. The all-zero guards stay with the caller. */
typedef struct {
  uint32_t off, n_bits, pay_bytes;
  uint32_t type;
} nr_gpu_crc_seg_t;
typedef int (*nr_gpu_pass_crc_fn)(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, uint32_t tbs, uint8_t *seg_ok, uint8_t *tb_ok, void *stream);

#ifdef __cplusplus
extern "C" {
#endif
int nr_gpu_pass_launch_chest_post(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out_t *out_dev, void *stream);
int nr_gpu_pass_launch_chest_post_f64(const nr_gpu_chest_post_par_t *p, nr_gpu_chest_post_out64_t *out_dev, void *stream);
int nr_gpu_pass_launch_rdm(const int16_t *llr, const nr_gpu_rdm_seg_t *segs, int nseg, int8_t *ldpc_in, void *stream);
int nr_gpu_pass_launch_crc(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, uint32_t tbs, uint8_t *seg_ok, uint8_t *tb_ok, void *stream);
/* graph variants (Task 4): parameters / TB size read from device-accessible memory when the kernel runs */
int nr_gpu_pass_launch_chest_post_p(const nr_gpu_chest_post_par_t *par_dev, nr_gpu_chest_post_out_t *out_dev, void *stream);
int nr_gpu_pass_launch_crc_p(const uint8_t *bits, const nr_gpu_crc_seg_t *segs, int nseg, const uint32_t *tbs_dev, uint8_t *seg_ok, uint8_t *tb_ok, void *stream);
#ifdef __cplusplus
}
#endif
#endif
