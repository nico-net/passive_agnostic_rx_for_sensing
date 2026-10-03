/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* C side of ldpc_cuda_pool_test.cc (the OAI thread-pool header is not C++-clean). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int lcp_load(void); /* dlopen ./libldpc_cuda.so, init pool + worker + thread pool; 0 = ok */
/* Decode one TB of C code blocks (noise[r] != 0: random LLRs; else the all-zero codeword). Returns the
 * nrLDPC_coding_decoder return value; ok[r] = decodeSuccess, *decoder_used = NRLDPC_DECODER_*. */
int lcp_run_tb(int C, const uint8_t* noise, uint8_t* ok, uint8_t* decoder_used);
int lcp_pool_decode(uint32_t BG, uint32_t Z, uint32_t iters, uint32_t first, uint32_t count, uint32_t K, int* req_rc);
int8_t* lcp_host_llr(void);
uint8_t* lcp_host_bits(void);
void lcp_counters(uint64_t* errors, uint64_t* fallbacks, uint64_t* poisoned);
enum { LCP_CPU = 1, LCP_CUDA = 2 };
#ifdef __cplusplus
}
#endif
