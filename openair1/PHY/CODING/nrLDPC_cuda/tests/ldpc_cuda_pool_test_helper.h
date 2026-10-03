/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* C side of ldpc_cuda_pool_test.cc (the OAI thread-pool header is not C++-clean). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int lcp_load_path(const char* path); /* any plugin with nrLDPC_coding_init/decoder; pool/hook API optional */
int lcp_load(void); /* dlopen ./libldpc_cuda.so, init pool + worker + thread pool; 0 = ok */
/* Decode one TB through nrLDPC_coding_decoder. bg1 = 0: BG2 Z=96 K=960 (C blocks); bg1 = 1: BG1 Z=384 K=8448 (C must be 1).
 * noise[r] != 0: random LLRs, else the all-zero codeword. ok[r] = decodeSuccess, *decoder_used = LCP_*. */
int lcp_run_tb(int bg1, int C, const uint8_t* noise, uint8_t* ok, uint8_t* decoder_used);
int lcp_run_tb_z(int bg1, int Z, int C, const uint8_t* noise, uint8_t* ok, uint8_t* decoder_used);
/* Random-payload TB through a real encoder and AWGN (see the .c). ok[r] = decodeSuccess, match[r] = decoded == source bytes. */
int lcp_random_tb(int bg1, int Z, int C, double ebn0_db, int iters, uint8_t* ok, uint8_t* match, uint8_t* decoder_used);
uint64_t lcp_trips(void); /* ldpc_cuda_breaker_trips */
int lcp_init_again(void); /* second nrLDPC_coding_init */
int lcp_pool_decode(uint32_t BG, uint32_t Z, uint32_t iters, uint32_t first, uint32_t count, uint32_t K, int* req_rc);
int8_t* lcp_host_llr(void);
uint8_t* lcp_host_bits(void);
void lcp_counters(uint64_t* errors, uint64_t* fallbacks, uint64_t* poisoned, uint64_t* disabled);
void lcp_hooks(int skip, int stall_ms, int inject, int queue_cap, int timeout_ms, int breaker_n, int breaker_ms);
void lcp_reset(void);
int lcp_slots_used(void);
enum { LCP_CPU = 1, LCP_CUDA = 2 };
#ifdef __cplusplus
}
#endif
