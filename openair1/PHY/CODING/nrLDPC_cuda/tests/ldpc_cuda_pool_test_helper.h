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
/* ---- CB0 entry tests / paired harness ---- */
void *lcp_sym(const char *name); /* dlsym in the loaded plugin */
/* A second plugin (the CPU TB decoder, e.g. ./libldpc.so), RTLD_LOCAL | RTLD_DEEPBIND; 0 = ok */
int lcp_load_ref(const char *path);
/* One code block, C = 1: random payload with a real CRC (crc_type 0 CRC24A, 1 CRC24B, 2 CRC16; corrupt_crc flips one
 * CRC bit before encoding: a valid LDPC codeword whose CRC fails), OAI encoder, rv0 E = N, Qm = 2, AWGN at ebn0_db
 * (ebn0_db >= 99: noiseless). K = (bg1 ? 22 : 10) * Z (Z % 4 == 0). Outputs (each may be NULL): src K/8 bytes,
 * l the CB0 decoder input (Kc*Z int8: 2Z zeros, saturated LLRs), llr the E = N int16 channel LLRs (TB path input).
 * Uses drand48 / lrand48 (caller seeds). Returns K, or < 0. */
int lcp_make_cw(int bg1, int Z, int crc_type, double ebn0_db, int corrupt_crc, uint8_t *src, int8_t *l, short *llr);
/* Decode llr (from lcp_make_cw, C = 1) as a TB through the main plugin (ref = 0) or the reference plugin (ref = 1)
 * with max_ldpc_iterations = iters. ok = decodeSuccess, bits K/8 bytes (may be NULL), du = decoder_used. */
int lcp_decode_cw(int ref, int bg1, int Z, const short *llr, int iters, uint8_t *ok, uint8_t *bits, uint8_t *du);
#ifdef __cplusplus
}
#endif
