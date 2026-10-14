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
  uint16_t K;        /* bits per snapshot: K of the code block (multiple of 8) */
  uint8_t it_max;    /* numMaxIter */
  const int8_t *llr; /* Kc * Z decoder input (the int8 array LDPCdecoder gets); read in place by the GPU */
  /* outputs */
  int nsnap;         /* snapshots written (after iterations 3 .. nsnap + 2) */
  int syn0;          /* 1: stopped at a zero syndrome after the last snapshot (later iterations not run) */
  uint8_t snap[NR_GPU_LDPC_MAX_SNAP][NR_GPU_LDPC_KB_MAX];
} nr_gpu_ldpc_job_t;

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
