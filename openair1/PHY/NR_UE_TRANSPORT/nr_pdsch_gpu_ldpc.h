/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DECGPU exact LDPC (ISAC_PDSCH_GPU_EXACT_LDPC=1, default off): OAI's CPU LDPC decoder (nrLDPC_decoder.c, the
 * aarch64 128-bit path: flooding min-sum, int8 messages, two-pass CN min/second-min, int16 BN sums saturated to int8,
 * subs_epi8 extrinsics, CRC early stop after every iteration from the third on) ported to the GPU so every code block
 * decodes to the same bits after the same number of iterations as libldpc.so.
 * Module side: nr_pdsch_gpu_ldpc.cu (libpdsch_gpu.so). Host side (rate dematching, CRC stop rule, TB assembly,
 * CHECK mode): nr_pdsch_gpu_ldpc_host.c. */
#ifndef NR_PDSCH_GPU_LDPC_H
#define NR_PDSCH_GPU_LDPC_H
#include <stdint.h>
#include <stddef.h>
#include "PHY/TOOLS/nr_gpu_mem.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_GPU_LDPC_MAX_JOBS 32 /* code blocks per launch (a TB with more is cut into launches) */
#define NR_GPU_LDPC_MAX_SNAP 16 /* hard-decision snapshots per code block: numMaxIter - 1 <= 16 */
#define NR_GPU_LDPC_KB_MAX 1056 /* K / 8 for BG1 Z = 384 */

/* One code block. The decoder runs iterations 1 .. it_max + 1 (OAI: one before its loop, it_max in the loop) and
 * stores the packed hard decision of the first K bits (MSB first, nrLDPC_llr2bitPacked) after every iteration
 * >= 3 -- the iterations after which OAI checks the CRC. It stops early only once every parity check of the used
 * rows holds (syndrome 0); the host then decides with check_crc on the snapshots, exactly as OAI. */
typedef struct {
  uint8_t BG, R;     /* R: 13, 23, 89 (BG1) or 15, 13, 23 (BG2): the rows nrLDPC_init() uses */
  uint16_t Z;
  uint16_t K;        /* bits per snapshot: K of the code block; bytes (K+7)>>3, tail bits of the last byte zero */
  uint8_t it_max;    /* numMaxIter */
  const int8_t *llr; /* Kc * Z decoder input (the int8 array LDPCdecoder gets); read in place by the GPU */
  /* outputs */
  int nsnap;         /* snapshots written (after iterations 3 .. nsnap + 2) */
  int syn0;          /* 1: stopped at a zero syndrome after the last snapshot (later iterations not run) */
  uint8_t snap[NR_GPU_LDPC_MAX_SNAP][NR_GPU_LDPC_KB_MAX];
  uint8_t pad;       /* graph pass only (nr_pdsch_gpu_ldpc_pass_set): 1 = a padding block of the shape class; its code block stops at entry (nsnap 0, syn0 1), nobody reads its output */
} nr_gpu_ldpc_job_t;

/* nr_pdsch_gpu_ldpc_dev (launch-only) buffer layouts. llr_dev: job k's Kc * Z int8 inputs at k * NR_GPU_LDPC_DEV_IN_STRIDE.
 * bits_dev: snapshots [NR_GPU_LDPC_MAX_JOBS][NR_GPU_LDPC_MAX_SNAP][NR_GPU_LDPC_KB_MAX] bytes, then int res[2 * MAX_JOBS] = {nsnap, syn0} per job. */
#define NR_GPU_LDPC_DEV_IN_STRIDE (68 * 384)
#define NR_GPU_LDPC_DEV_IN_BYTES ((size_t)NR_GPU_LDPC_MAX_JOBS * NR_GPU_LDPC_DEV_IN_STRIDE)
#define NR_GPU_LDPC_DEV_RES_OFF ((size_t)NR_GPU_LDPC_MAX_JOBS * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX)
#define NR_GPU_LDPC_DEV_BITS_BYTES (NR_GPU_LDPC_DEV_RES_OFF + 2 * NR_GPU_LDPC_MAX_JOBS * sizeof(int))

/* The graph-pass instances (nr_gpu_pass.h) decode up to NR_GPU_PASS_LDPC_JOBS code blocks per launch; their llr_dev / bits_dev buffers use the same layouts at that size (the standalone contexts and the PUSCH pool keep 32). */
#define NR_GPU_PASS_LDPC_JOBS 160
#define NR_GPU_PASS_LDPC_IN_BYTES ((size_t)NR_GPU_PASS_LDPC_JOBS * NR_GPU_LDPC_DEV_IN_STRIDE)
#define NR_GPU_PASS_LDPC_RES_OFF ((size_t)NR_GPU_PASS_LDPC_JOBS * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX)
/* measurement only: after res, uint64 ts[2 * JOBS] = %globaltimer (ns) at the entry of each code block's first block and at its stop (k_ldpc*_t, written for every block of the launch) */
#define NR_GPU_PASS_LDPC_TS_OFF (NR_GPU_PASS_LDPC_RES_OFF + 2 * NR_GPU_PASS_LDPC_JOBS * sizeof(int))
#define NR_GPU_PASS_LDPC_BITS_BYTES (NR_GPU_PASS_LDPC_TS_OFF + 2 * NR_GPU_PASS_LDPC_JOBS * sizeof(uint64_t))

/* GPUPASS launch-only decode of n code blocks from device LLRs into device snapshots; 0 or -1. Below CC 9 (or ISAC_GPU_LDPC_NOCLUSTER=1) the non-cluster kernels run. Enqueue only. */
/* o[0] kernel family (0 undecided, 1 cluster, 2 non-cluster, 3 refused), o[1] refusals, o[2] dev graph misses */
typedef void (*nr_gpu_ldpc_stats_fn)(uint64_t *o);
typedef int (*nr_gpu_ldpc_dev_fn)(const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream);

/* graph-pass variants: instance inst (0 <= inst < NR_GPU_PASS_RING) has its own context and DJob array. _pass_set (host) writes the jobs of the next replay, _pass_dev
 * validates the same jobs, enqueues the sentinel memset and the kernel (n and the Z-multiple-of-4 kernel choice are baked into a captured graph). */
typedef int (*nr_gpu_ldpc_pass_set_fn)(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev);
typedef int (*nr_gpu_ldpc_pass_dev_fn)(int inst, const nr_gpu_ldpc_job_t *jobs, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream);

/* Why nr_pdsch_gpu_ldpc_run refuses or fails (always non-zero, so every caller's != 0 check holds). */
enum nr_gpu_ldpc_err { NR_GPU_LDPC_OK = 0, NR_GPU_LDPC_E_INIT = -1, NR_GPU_LDPC_E_RANGE = -2, NR_GPU_LDPC_E_PARAM = -3, NR_GPU_LDPC_E_GRAPH_MISS = -4,
                       NR_GPU_LDPC_E_STREAM = -5, NR_GPU_LDPC_E_SENTINEL = -6, NR_GPU_LDPC_E_CAP = -7, NR_GPU_LDPC_E_RT = -8, NR_GPU_LDPC_E_BUSY = -9,
                       NR_GPU_LDPC_E_DEADLINE = -10 };
#define NR_GPU_LDPC_NERR 11 /* counter slots: index -rc, 0 unused for the DL */

typedef int (*nr_gpu_ldpc_init_fn)(void);
/* Decodes n <= NR_GPU_LDPC_MAX_JOBS code blocks (any mix of BG / Z / R) in one launch (one thread-block cluster each), synchronously.
 * jobs must stay valid until return (the module copies inputs and outputs through pinned memory). 0 = done. */
typedef int (*nr_gpu_ldpc_run_fn)(nr_gpu_ldpc_job_t *jobs, int n);

/* ULLDPC (ISAC_UL_GPU_LDPC): the same kernel in a pool of its own for the passive PUSCH decode (nr_pdsch_gpu_ldpc.cu).
 * init(pct, deadline_us): duty cap percent of each 100 ms and per-call deadline. run: 0 decoded; < 0 nothing valid, the caller
 * runs the CPU decoder (-1 busy, -2 over cap / RT in flight, -3 off or error, -4 deadline). */
typedef int (*nr_gpu_ldpc_ul_init_fn)(int pct, int deadline_us);
typedef void (*nr_gpu_ldpc_ul_stats_fn)(uint64_t *o); /* calls cbs gpu_us max_us busy over_cap rt_defer late deadline err reclaimed part */

#ifdef __cplusplus
}
#endif
#endif
