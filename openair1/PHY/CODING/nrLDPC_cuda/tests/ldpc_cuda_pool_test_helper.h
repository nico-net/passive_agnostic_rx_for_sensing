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
/* One BG1 R1/3 K'=8448 block, all-zero codeword over AWGN at Eb/N0 ebn0_db (ldpctest convention), 8 iterations,
 * through nrLDPC_coding_decoder. Returns 1 if the block failed CRC. */
int lcp_bler_trial(double ebn0_db, uint8_t* decoder_used);
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
