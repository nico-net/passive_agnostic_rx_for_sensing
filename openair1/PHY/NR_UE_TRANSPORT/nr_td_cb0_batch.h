/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/*! \file nr_td_cb0_batch.h
 * \brief Batched first-code-block (CB0) decode of many Technique D hypotheses per grant.
 *
 * The compute engine of the CB0 elimination channel (levers spec 2026-10-01 section 5.4 decision block and
 * section 9): on one grant every alive hypothesis gets a CB0 PASS / FAIL. Per item the processing is EXACTLY the
 * receiver's full decode restricted to code block 0 (passive_ldpc_decode_core with nb_segments_to_decode = 1,
 * nrLDPC_coding_segment_decoder.c nr_process_decode_segment):
 *   BG = get_BG(tbs, R) (or forced), nr_segmentation(lenWithCrc(1, tbs)) -> C, K, Z, F;
 *   E = nr_get_E(G, C, Qm, Nl, 0); R_dec = nr_get_R_ldpc_decoder(rv, E, BG, Z, .., 0);
 *   nr_deinterleaving_ldpc(E, Qm) -> nr_rate_matching_ldpc_rx(tbslbrm, BG, Z, d, e, C, rv, clear = 1, E, F, K-F-2Z)
 *   -> 2Z punctured zeros, F filler LLRs at 127, int8 saturation -> LDPCdecoder(numMaxIter = max_iter,
 *   crc_type = crcType(C, tbs), Kprime = lenWithCrc(C, tbs)); PASS iff iterations < max_iter and the decoded
 *   payload is not all zero (the probe / full-TB all-zero guard).
 * CRC: CB CRC24B when C > 1; when C == 1 the decoder checks the TB CRC (CRC24A, or CRC16 for tbs <= 3824) and
 * the CB0 result IS the TB result (result.tb_result = 1).
 *
 * Only new transmissions (no HARQ soft combining): the circular buffer is always cleared, as for a first round.
 *
 * Unified memory: item.llr is read IN PLACE. On GB10 (pageable memory access through the host page tables)
 * any host pointer works; elsewhere it must be managed (cudaMallocManaged) memory for the GPU dematch, else
 * that item falls back to the CPU dematch. Without the CUDA module the whole batch runs on the CPU.
 *
 * GrantWork contract (td/grantwork-lite): item.llr is consumed AS-IS. This module never descrambles, scales or
 * re-normalises (the GrantWork LLRs are already descrambled and ISAC_LLR_NORM-shifted, exactly what the full
 * decode receives). nr_td_cb0_batch is synchronous and keeps no reference to any item buffer after it returns, so
 * the caller retains each GrantWork the batch references for the duration of the call and releases it afterwards.
 *
 * Decoder provenance: every result carries decoder_used. Today every item of a batch uses the CPU layered decoder
 * (the GPU only does the de-matching). A future CUDA LDPC path must set decoder_used per item and must mark any
 * item that falls back to the CPU accordingly (CUDA and CPU CRC verdicts are not exchangeable, ~1 dB apart).
 */
#ifndef NR_TD_CB0_BATCH_H
#define NR_TD_CB0_BATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const int16_t *llr; /* descrambled demodulated LLRs of the hypothesis's geometry signature, length >= G */
  uint32_t G;         /* total coded bits of the allocation for this signature */
  uint8_t Qm, Nl, rv; /* modulation order (2/4/6/8), layers (1..4), redundancy version (0..3) */
  uint32_t tbs;       /* transport block size in bits for this hypothesis (its MCS table) */
  uint8_t mcs_table;  /* informational: the caller derives tbslbrm from it (nr_compute_tbslbrm) */
  uint32_t tbslbrm;   /* precomputed by the caller, 0 = no LBRM */
  uint8_t max_iter;   /* same iteration policy as the full decode (8 in passive_ldpc_decode_core) */
  /* --- additions to the 2026-10-03 contract: BG selection (38.212 7.2.2) needs the code rate --- */
  uint16_t R; /* target code rate x 10240, exactly cw->targetCodeRate (get_BG input) */
  uint8_t bg; /* 0 = get_BG(tbs, R); 1 / 2 = forced (a reserved-MCS retransmission keeps the initial BG) */
} nr_td_cb0_item_t;

enum {
  NR_TD_CB0_DEC_CPU_LAYERED = 1,
  NR_TD_CB0_DEC_CUDA_FLOODING = 2,
};

/* Reason codes for pass == -1 (result.err). */
enum {
  NR_TD_CB0_OK = 0,
  NR_TD_CB0_ERR_ARG = 1,     /* NULL llr, G == 0, Qm/Nl/rv/tbs/max_iter out of range, R == 0 with bg == 0 */
  NR_TD_CB0_ERR_SEG = 2,     /* nr_segmentation refused the TBS, or C > 255 (nr_get_E takes uint8 C) */
  NR_TD_CB0_ERR_E = 3,       /* E <= 0 */
  NR_TD_CB0_ERR_RM = 4,      /* nr_rate_matching_ldpc_rx would return -1 (Foffset > Ncb, empty buffer) */
  NR_TD_CB0_ERR_DECODER = 5, /* no LDPC decoder registered */
};

typedef struct {
  int8_t pass;          /* 1 pass, 0 fail, -1 inconclusive / error */
  uint8_t iters;        /* LDPC iterations returned by the decoder */
  uint8_t decoder_used; /* NR_TD_CB0_DEC_* (0 when not decoded) */
  /* --- additions --- */
  uint8_t tb_result; /* 1: C == 1, pass IS the transport-block CRC result (not only CB0) */
  uint8_t err;       /* NR_TD_CB0_ERR_* when pass == -1 */
  uint8_t dematch_gpu; /* 1: the rate de-matching of this item ran on the GPU */
  uint16_t C;        /* code blocks of the hypothesis's TB */
} nr_td_cb0_result_t;

/* Decode CB0 of n items; out[i] for items[i]. Returns the number of items with pass != -1 (decoded), or -1 if
 * items/out is NULL with n > 0. Thread-safe (calls are serialised internally). */
int nr_td_cb0_batch(const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out);

/* ---- configuration ---- */
/* Register the CPU layered decoder: an LDPC_decoderfunc_t * (LDPCdecoder of libldpc.so, e.g. from
 * load_LDPClib("", &itf), or the symbol itself when nrLDPC_decoder.c is linked in). Without one
 * every item is inconclusive (ERR_DECODER). */
void nr_td_cb0_set_ldpc_decoder(void *ldpc_decoder_fn);
/* CPU LDPC worker threads for a batch (default 1; capped at 64). */
void nr_td_cb0_set_threads(int n);
/* GPU rate de-matching: 1 = load libtd_cb0_gpu.so (next to the executable, or ./), 0 = CPU. Returns 1 if the GPU
 * path is active after the call. Default at the first batch: on iff NR_GPU_CB0=1. */
int nr_td_cb0_use_gpu(int on);

/* ---- per-item metadata (CPU), shared with the GPU kernel ---- */
typedef struct {
  uint8_t valid; /* 0: err says why */
  uint8_t err;
  uint8_t BG, Qm, rv, Kc, crc_type, R_dec;
  uint32_t A, C, K, Z, F, E, Kprime_crc; /* Kprime_crc = lenWithCrc(C, A) */
  uint32_t N, Ncb, k0, Foffset;          /* k0 = start index in the circular buffer (index_k0 * Ncb / N) * Z */
  uint32_t tbslbrm;
  uint8_t max_iter;
} nr_td_cb0_meta_t;

/* Compute the metadata of one item. Returns 0 if valid, else the NR_TD_CB0_ERR_* code (meta->valid = 0). */
int nr_td_cb0_meta(const nr_td_cb0_item_t *item, nr_td_cb0_meta_t *meta);

/* Decoder input (int8, Kc*Z values: 2Z zeros, coded bits, F fillers at 127) and the int16 circular buffer d
 * (N values) of one item, per stride. */
#define NR_TD_CB0_L_STRIDE (68 * 384 + 64)
#define NR_TD_CB0_D_STRIDE (66 * 384)

/* Rate de-matching only (test / benchmark hook): fills l (n * NR_TD_CB0_L_STRIDE) and optionally d
 * (n * NR_TD_CB0_D_STRIDE, may be NULL) for every valid item; status[i] = 1 done on GPU, 0 done on CPU,
 * -1 invalid item. use_gpu = 0 forces the CPU reference (nr_deinterleaving_ldpc + nr_rate_matching_ldpc_rx).
 * Returns 0, or -1 if use_gpu was requested and the GPU path is not available. */
int nr_td_cb0_dematch(const nr_td_cb0_item_t *items, int n, int use_gpu, int8_t *l, int16_t *d, int8_t *status);

/* LDPC adapter: decode n dematched CB0 buffers (l at NR_TD_CB0_L_STRIDE). Today: CPU layered decoder.
 * HOOK (td/g1-ldpc-safety): route to the G1-safe CUDA pool (ldpc_pool_decode / nrLDPC_coding_decoder) once merged;
 * see the comment at its definition. */
void cb0_ldpc_decode_batch(const nr_td_cb0_meta_t *meta, const int8_t *l, int n, nr_td_cb0_result_t *out);

/* ---- GPU module ABI (libtd_cb0_gpu.so, nr_td_cb0_batch.cu) ---- */
typedef struct {
  const int16_t *llr;
  uint32_t E, Qm, Kc, Z, K, F, Ncb, k0, Foffset, N;
} nr_td_cb0_gpu_item_t;
typedef struct {
  int abi; /* NR_TD_CB0_GPU_ABI */
  /* 1 if the device can read pageable host memory (GB10: yes), 0 if only managed / device pointers. */
  int (*pageable_ok)(void);
  /* 1 if p is readable by the device (pageable_ok, or managed / device memory). */
  int (*ptr_ok)(const void *p);
  /* Dematch n items into l (and d if non-NULL), both host-visible memory of n * stride. Synchronous.
   * Returns 0 on success; non-zero = CUDA error, the caller must not use l / d. */
  int (*dematch)(const nr_td_cb0_gpu_item_t *items, int n, int8_t *l, int16_t *d);
  /* Managed scratch of at least bytes, owned by the module (reused across calls); NULL on failure. */
  void *(*scratch)(int which, size_t bytes);
} nr_td_cb0_gpu_api_t;
#define NR_TD_CB0_GPU_ABI 1

#ifdef __cplusplus
}
#endif

#endif
