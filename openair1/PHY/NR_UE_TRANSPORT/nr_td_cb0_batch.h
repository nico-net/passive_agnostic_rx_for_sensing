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
  /* K38 LLR-norm shift k_h of THIS hypothesis (0..8): every LLR is read as (llr >> k_h), int16 arithmetic shift,
   * exactly what the full decode applies to its buffer (nr_llr_norm_shift over nr_llr_norm_span(G,
   * nr_llr_norm_num_cb(tbs, bg))). The batch applies exactly this value and nothing else; it never computes it. */
  uint8_t llr_shift;
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
  NR_TD_CB0_ERR_GPU = 6,     /* CUDA LDPC pool error / timeout / rejected request in this batch: no verdict */
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
  uint8_t dedup;     /* 1: identical computation key to an earlier item of the batch, verdict copied (exact) */
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
/* CUDA LDPC through the G1-safe libldpc_cuda.so, via its PUBLIC TB entry nrLDPC_coding_decoder (worker queue,
 * slot pool, bounded waits, circuit breaker, CPU fallback -- all G1's): each item is submitted as a TB with
 * nb_segments_to_decode = 1 (exactly the receiver's layout probe), up to 64 per call, calls from the worker threads
 * batched together by G1's worker. GPU iterations = 2 x max_iter (G1's TB rule). decoder_used comes from G1 per item:
 * 2 = CUDA flooding, 1 = G1 fell back to its CPU decoder for that TB (error, timeout, breaker, pool full) -- never
 * silently mixed. The plugin does the de-matching itself (on the CPU), so the GPU dematch is not used on this path.
 * 1 = use it, 0 = CPU layered (cb0_ldpc_decode_batch). Returns 1 if active. Default at the first batch: on iff
 * NR_TD_CB0_CUDA_LDPC=1. */
int nr_td_cb0_use_cuda_ldpc(int on);
/* Counters since start: items in, items decoded (after dedup), CUDA chunks, CUDA errors, CPU fallbacks. */
typedef struct {
  uint64_t items, decoded, cuda_chunks, cuda_errors, cuda_fallback_items;
} nr_td_cb0_stats_t;
void nr_td_cb0_get_stats(nr_td_cb0_stats_t *s);

/* ---- per-item metadata (CPU), shared with the GPU kernel ---- */
typedef struct {
  uint8_t valid; /* 0: err says why */
  uint8_t err;
  uint8_t BG, Qm, rv, Kc, crc_type, R_dec;
  uint32_t A, C, K, Z, F, E, Kprime_crc; /* Kprime_crc = lenWithCrc(C, A) */
  uint32_t N, Ncb, k0, Foffset;          /* k0 = start index in the circular buffer (index_k0 * Ncb / N) * Z */
  uint32_t tbslbrm;
  uint8_t max_iter;
  uint8_t shift; /* item.llr_shift */
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
  uint32_t E, Qm, Kc, Z, K, F, Ncb, k0, Foffset, N, shift;
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
#define NR_TD_CB0_GPU_ABI 2 /* 2: nr_td_cb0_gpu_item_t.shift */

/* ---- Backend interface (td/cb0-cpu-wiring, 2026-10-03) -----------------------------------------------------------
 * The runtime CB0 scheduler (nr_td_cb0_sched.h, nr_td_cb0_wire.c) never calls a decoder directly: it calls
 * nr_td_cb0_exec(), which picks a backend per batch.
 *   ISAC_TD_CB0_BACKEND=auto (default) | cpu | gpu, read once.
 *     cpu  : always the CPU backend.
 *     auto : the registered GPU backend when it is registered, healthy() and not backing off; else the CPU backend.
 *     gpu  : as auto (the CPU backend stays the permanent fallback), but warns once when it has to fall back.
 *   CPU backend (always present, plain C + simde, portable to x86): nr_td_cb0_batch_cpu() = nr_td_cb0_batch() with the
 *     CUDA LDPC path and the GPU de-matching forced OFF, i.e. libldpc's CPU decoder (the receiver's TB decoder), on the
 *     batch's worker threads (nr_td_cb0_set_threads). Every item: decoder_used = NR_TD_CB0_DEC_CPU_LAYERED.
 *   GPU backend: registered by the GPU entry (td/cb0-gpu-entry) through nr_td_cb0_register_gpu_backend().
 * FAILURE RULE: a GPU batch that returns non-zero (error, timeout, breaker open), or reports any item with
 * err == NR_TD_CB0_ERR_GPU, or mixes decoders (an item not decoded by the GPU decoder, e.g. a pool CPU fallback), is
 * FAILED as a whole: nr_td_cb0_exec() marks every result pass = -1 / err = NR_TD_CB0_ERR_GPU and sets info.failed, and
 * the caller treats the grant as inadmissible (never partially credited, never re-decoded on the CPU for the same
 * grant: the decision to use the GPU was taken before any outcome). The GPU is then skipped for the next
 * ISAC_TD_CB0_GPU_BACKOFF batches (default 64): the NEXT grant runs on the CPU backend.
 * Every result carries decoder_used; info.decoder is the single decoder of a successful batch (the elimination
 * engine's dominance rule compares it with the context's full-TB decoders). */
enum { NR_TD_CB0_BE_AUTO = 0, NR_TD_CB0_BE_CPU = 1, NR_TD_CB0_BE_GPU = 2 };
typedef struct {
  const char *name; /* for logs */
  /* 1 = usable now (device present, warmed up, breaker closed). Called before every GPU batch; must be cheap. */
  int (*healthy)(void *ctx);
  /* Decode n items synchronously with a bounded wait. Fill out[i] (pass, iters, decoder_used, err, C, tb_result).
   * Return 0 when the batch completed; non-zero = error / timeout / breaker open (the whole batch is void). */
  int (*decode)(void *ctx, const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out);
  void *ctx;
} nr_td_cb0_backend_t;
/* Register (copied) the GPU backend; NULL unregisters. Thread-safe; takes effect at the next batch. */
void nr_td_cb0_register_gpu_backend(const nr_td_cb0_backend_t *be);
/* ISAC_TD_CB0_BACKEND as NR_TD_CB0_BE_* (read once; an unknown value = auto). */
int nr_td_cb0_backend_mode(void);
/* Test hook: force a mode (NR_TD_CB0_BE_*), -1 re-reads the environment. Also clears the GPU back-off. */
void nr_td_cb0_backend_mode_set(int mode);
typedef struct {
  uint8_t backend;    /* NR_TD_CB0_BE_CPU / NR_TD_CB0_BE_GPU: the backend that ran this batch */
  uint8_t decoder;    /* NR_TD_CB0_DEC_* common to every decoded item; 0 when none decoded */
  uint8_t failed;     /* 1: GPU failure rule above (whole batch void) */
  uint8_t mixed;      /* 1: decoded items disagree on decoder_used (also failed) */
  int decoded;        /* items with pass != -1 */
  uint32_t sum_iters; /* sum of iterations over decoded items (0 when the backend does not report them) */
  uint64_t wall_ns;   /* wall time of the batch (incl. waiting for the backend) */
  uint64_t compute_ns; /* CPU backend: decode wall without the lock wait (nr_td_cb0_last_compute); GPU: = wall_ns */
  int distinct;        /* CPU backend: items decoded after dedup */
  int threads;        /* worker threads the batch could use (CPU backend), 0 for the GPU */
} nr_td_cb0_exec_t;
/* Run one batch on the selected backend (see above). Returns info.decoded, or -1 on bad arguments. */
int nr_td_cb0_exec(const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out, nr_td_cb0_exec_t *info);
/* nr_td_cb0_batch() with the CUDA LDPC path and the GPU de-matching forced off: the CPU backend. */
int nr_td_cb0_batch_cpu(const nr_td_cb0_item_t *items, int n, nr_td_cb0_result_t *out);
/* This thread's last nr_td_cb0_batch / _cpu call: wall of the decode itself (after the internal lock was taken, so
 * without the wait for another thread's batch) and the number of distinct items decoded (after dedup). */
void nr_td_cb0_last_compute(uint64_t *ns, int *decoded);
/* Persistent CPU pool workers created so far (round 2: created once, grown to threads - 1, never per batch). */
int nr_td_cb0_pool_threads(void);
/* Worker threads configured for the CPU backend (nr_td_cb0_set_threads). */
int nr_td_cb0_get_threads(void);
/* Backend counters since start: batches per backend, GPU failures, GPU batches skipped (back-off / unhealthy). */
typedef struct {
  uint64_t batches_cpu, batches_gpu, gpu_failed, gpu_skipped;
} nr_td_cb0_backend_stats_t;
void nr_td_cb0_backend_get_stats(nr_td_cb0_backend_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif
