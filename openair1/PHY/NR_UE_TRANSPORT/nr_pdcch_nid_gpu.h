/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PDCCH_NID_GPU_H
#define NR_PDCCH_NID_GPU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PDCCH_NID_BATCH 336
#define NR_PDCCH_NID_MAX_RECORDS 16
#define NR_PDCCH_NID_MAX_WINDOW 20
#define NR_PDCCH_NID_ABI 2
#define NR_PDCCH_NID_KERNEL_KINDS 13
typedef struct { int16_t r, i; } nr_pdcch_nid_iq_t;
typedef struct { int nid, sym, dur, grid_rb, groups, L, R, shift, ties; } nr_pdcch_nid_record_t;
typedef struct {
 int count;
 nr_pdcch_nid_record_t records[NR_PDCCH_NID_MAX_RECORDS];
 float stage1_ms, stage2_ms;
 float kernel_max_ms;
 int budget_exceeded; /* soft deadline observed while polling; completed results still publish */
 /* CUDA event intervals: include contention when other streams are active. */
 float kernel_total_ms[NR_PDCCH_NID_KERNEL_KINDS], kernel_peak_ms[NR_PDCCH_NID_KERNEL_KINDS];
 unsigned kernel_calls[NR_PDCCH_NID_KERNEL_KINDS];
 /* Stage-1 diagnostic winners, css0 then rest. Small, never a bulk readback. */
 int top_id[2], top_sym[2], list_n, list_id[40], list_sym[40];
 uint64_t hit_hash[2];
 int al_win[NR_PDCCH_NID_MAX_RECORDS][5];
} nr_pdcch_nid_result_t;
typedef struct {
 int nsc, point_a_sc, css0_crb, css0_nrb, pci, sps, dur;
} nr_pdcch_nid_config_t;
typedef struct {
 nr_pdcch_nid_config_t cfg;
 int slot[NR_PDCCH_NID_BATCH], sym[NR_PDCCH_NID_BATCH];
 nr_pdcch_nid_iq_t *iq;
} nr_pdcch_nid_batch_t;
/* Minimal hand-off for T15. Snapshot is immutable during this call. Returns -1
 * for no NEW generation, otherwise count (including zero). Never reads a file.
 * A generation is published only after a complete, successful GPU pass. */
int nr_pdcch_nid_gpu_records(nr_pdcch_nid_record_t *out, int capacity, uint64_t *generation);
bool nr_pdcch_nid_gpu_enabled(void);
bool nr_pdcch_nid_gpu_running(void);
/* Call before radio start; all CUDA allocations, registration, module loading,
 * kernels and worker creation are warmed here. Stop after the producer stops. */
int nr_pdcch_nid_gpu_start(int max_rb);
void nr_pdcch_nid_gpu_stop(void);
/* Single producer; bounded try-lock, drops if staging is full. rx points to the
 * first requested FFT symbol, with fft complex samples between symbols. */
int nr_pdcch_nid_gpu_push(const void *rx, int fft, int fco, int slot, int sym,
                          const nr_pdcch_nid_config_t *cfg);
/* Module ABI: only create/destroy allocate. Upload only the NEW batch; the last
 * W batches and correlation/snapshot workspaces stay in cudaMalloc device
 * memory; pinned hit summaries support worker-side stable selection. */
typedef struct {
 unsigned abi;
 void *(*create)(int max_rb, int dur, int window, nr_pdcch_nid_iq_t **staging);
 int (*upload)(void *, const nr_pdcch_nid_batch_t *);
 int (*run)(void *, nr_pdcch_nid_result_t *);
 void (*destroy)(void *);
} nr_pdcch_nid_ops_t;
const nr_pdcch_nid_ops_t *nr_pdcch_nid_gpu_ops(void);
#ifdef __cplusplus
}
#endif
#endif
