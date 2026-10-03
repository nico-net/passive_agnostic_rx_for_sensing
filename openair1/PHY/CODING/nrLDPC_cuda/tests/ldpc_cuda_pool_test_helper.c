/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "ldpc_cuda_pool_test_helper.h"
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "common/utils/threadPool/thread-pool.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"

static void *h;
static int32_t (*f_init)(void);
static int32_t (*f_dec)(nrLDPC_slot_decoding_parameters_t *);
static int (*f_pool)(uint32_t, uint32_t, uint32_t, int, const uint32_t *, const uint32_t *, const uint32_t *, int *);
static int8_t *(*f_llr)(void);
static uint8_t *(*f_bits)(void);
static void (*f_ctr)(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
static void (*f_hooks)(int, int, int, int, int, int, int);
static void (*f_reset)(void);
static int (*f_used)(void);
static tpool_t pool;
static int (*f_enc)(uint8_t **, uint8_t *, encoder_implemparams_t *);

int lcp_load(void) { return lcp_load_path("./libldpc_cuda.so"); }
int lcp_load_path(const char *path)
{
  if (h)
    return 0;
  h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!h)
    return -1;
  f_init = dlsym(h, "nrLDPC_coding_init");
  f_dec = dlsym(h, "nrLDPC_coding_decoder");
  f_pool = dlsym(h, "ldpc_pool_decode");
  f_llr = dlsym(h, "ldpc_pool_host_llr");
  f_bits = dlsym(h, "ldpc_pool_host_bits");
  f_ctr = dlsym(h, "ldpc_cuda_get_counters4");
  f_hooks = dlsym(h, "ldpc_cuda_test_hooks");
  f_reset = dlsym(h, "ldpc_cuda_test_reset");
  f_used = dlsym(h, "ldpc_cuda_test_slots_used");
  if (!(f_init && f_dec))
    return -2;
  /* REQUIRED: without the CRC tables check_crc()/crc24a() are wrong on aarch64 (CRC16/table paths compute 0, the CRC24A
   * PCLMUL constants are unset) and an all-zero or garbage block can "pass". Review 2026-10-03. */
  crcTableInit();
  f_enc = dlsym(h, "LDPCencoder"); /* the plugin's own OAI encoder (optim8segmulti), used to build real codewords */
  logInit();
  char p[] = "n";
  initTpool(p, &pool, false);
  return f_init();
}

static int run(int bg1, int Z, int C, const short *llr, uint8_t *ok, uint8_t *decoder_used, int iters, uint8_t *cout)
{
  const int K = (bg1 ? 22 : 10) * Z, E = (bg1 ? 66 : 50) * Z, Kc = bg1 ? 68 : 52;
  int16_t *d = calloc((size_t)C * Kc * Z, sizeof(int16_t));
  uint8_t *c = malloc((size_t)C * (K >> 3));
  memset(c, 0xEE, (size_t)C * (K >> 3)); /* the decoder must overwrite every block */
  uint32_t processed = 0;
  nrLDPC_TB_decoding_parameters_t *tb = calloc(1, sizeof(*tb));
  tb->processedSegments = &processed;
  tb->Qm = 2; tb->BG = bg1 ? 1 : 2; tb->max_ldpc_iterations = iters; tb->tbslbrm = 100000000;
  tb->K = K; tb->Z = Z; tb->F = 0; tb->C = C;
  tb->A = C == 1 ? K - (K - 24 > 3824 ? 24 : 16) : C * (K - 24) - 24;
  tb->E = E; tb->R = bg1 ? 13 : 15; tb->E2 = E; tb->R2 = tb->R; tb->first_rE2 = C;
  static decode_abort_t ab;
  static int ab_init;
  if (!ab_init) { init_abort(&ab); ab_init = 1; }
  set_abort(&ab, false);
  tb->abort_decode = &ab;
  tb->llr = (short *)llr; tb->c = c; tb->d = d; tb->d_to_be_cleared = true;
  nrLDPC_slot_decoding_parameters_t slot = {0};
  slot.nb_TBs = 1; slot.threadPool = &pool; slot.TBs = tb;
  const int rc = f_dec(&slot);
  for (int r = 0; r < C; r++)
    ok[r] = tb->decodeSuccess[r];
  *decoder_used = tb->decoder_used;
  if (cout)
    memcpy(cout, c, (size_t)C * (K >> 3));
  free(d); free(c); free(tb);
  return rc;
}

int lcp_run_tb_z(int bg1, int Z, int C, const uint8_t *noise, uint8_t *ok, uint8_t *decoder_used)
{
  const int E = (bg1 ? 66 : 50) * Z;
  short *llr = malloc((size_t)C * E * sizeof(short));
  for (int r = 0; r < C; r++)
    for (int i = 0; i < E; i++)
      llr[(size_t)r * E + i] = noise[r] ? (short)((rand() % 81) - 40) : 40;
  const int rc = run(bg1, Z, C, llr, ok, decoder_used, 10, NULL);
  free(llr);
  return rc;
}
int lcp_run_tb(int bg1, int C, const uint8_t *noise, uint8_t *ok, uint8_t *decoder_used)
{
  return lcp_run_tb_z(bg1, bg1 ? 384 : 96, C, noise, ok, decoder_used);
}

static double gauss(void)
{
  return sqrt(-2.0 * log(1.0 - drand48())) * cos(2 * M_PI * drand48());
}
/* One TB of C code blocks with RANDOM payloads: per-CB CRC (CRC24B for C>1, CRC24A/CRC16 for C=1), real OAI LDPC encoding,
 * TS 38.212 rv0 bit selection with E = N and Qm=2 interleaving, AWGN at ebn0_db (ldpctest convention, rate K/N), LLR = 16*y/sigma.
 * Z must make K = Kb*Z a multiple of 8. Uses drand48/lrand48 (caller seeds). ok[r] = decodeSuccess, match[r] = decoded bytes == source
 * bytes (bit-exact, independent of ok). Returns the decoder rc, or <0 if the harness itself failed (-3 CRC self-check). */
int lcp_random_tb(int bg1, int Z, int C, double ebn0_db, int iters, uint8_t *ok, uint8_t *match, uint8_t *decoder_used)
{
  const int Kb = bg1 ? 22 : 10, K = Kb * Z, N = (bg1 ? 66 : 50) * Z, E = N, KB = K >> 3;
  if (!f_enc || (K & 7) || C < 1 || C > 8)
    return -1;
  const int crc24 = C > 1 || K - 24 > 3824;
  const double sigma = 1.0 / sqrt(2 * pow(10, ebn0_db / 10.0) * ((double)K / N));
  uint8_t *src = calloc((size_t)C, KB + 64);
  short *llr = malloc((size_t)C * E * sizeof(short));
  uint8_t *out = malloc(68 * 384 + 64);
  for (int r = 0; r < C; r++) {
    uint8_t *in = src + (size_t)r * KB;
    for (int i = 0; i < KB - (crc24 ? 3 : 2); i++)
      in[i] = lrand48() & 0xff;
    if (crc24) {
      const uint32_t crc = (C > 1 ? crc24b(in, K - 24) : crc24a(in, K - 24)) >> 8;
      in[KB - 3] = crc >> 16; in[KB - 2] = crc >> 8; in[KB - 1] = crc;
    } else {
      const uint32_t crc = crc16(in, K - 16) >> 16;
      in[KB - 2] = crc >> 8; in[KB - 1] = crc;
    }
    if (!check_crc(in, K, C > 1 ? CRC24_B : crc24 ? CRC24_A : CRC16)) {
      free(src); free(llr); free(out);
      return -3;
    }
    uint8_t *ip = in;
    uint8_t inbuf[KB + 64];
    memcpy(inbuf, in, KB); /* the encoder may read a few bytes past the block */
    memset(inbuf + KB, 0, 64);
    ip = inbuf;
    memset(out, 0, 68 * 384);
    encoder_implemparams_t impp = {.Zc = Z, .Kb = Kb, .BG = bg1 ? 1 : 2, .K = K, .gen_code = 0, .n_segments = 1};
    f_enc(&ip, out, &impp);
    for (int j = 0; j < E / 2; j++)
      for (int i = 0; i < 2; i++) {
        const int b = out[i * (E / 2) + j] & 1;
        const double q = 16.0 * ((1.0 - 2 * b) + sigma * gauss()) / sigma;
        llr[(size_t)r * E + i + 2 * j] = (short)(q > 32000 ? 32000 : q < -32000 ? -32000 : lround(q));
      }
  }
  uint8_t *cout = malloc((size_t)C * KB);
  const int rc = run(bg1, Z, C, llr, ok, decoder_used, iters, cout);
  for (int r = 0; r < C; r++)
    match[r] = !memcmp(cout + (size_t)r * KB, src + (size_t)r * KB, KB);
  free(src); free(llr); free(out); free(cout);
  return rc;
}

uint64_t lcp_trips(void) { uint64_t (*f)(void) = dlsym(h, "ldpc_cuda_breaker_trips"); return f ? f() : 0; }
int lcp_init_again(void) { return f_init(); }
int lcp_pool_decode(uint32_t BG, uint32_t Z, uint32_t iters, uint32_t first, uint32_t count, uint32_t K, int *req_rc)
{
  return f_pool(BG, Z, iters, 1, &first, &count, &K, req_rc);
}
int8_t *lcp_host_llr(void) { return f_llr(); }
uint8_t *lcp_host_bits(void) { return f_bits(); }
void lcp_counters(uint64_t *e, uint64_t *f, uint64_t *p, uint64_t *d) { f_ctr(e, f, p, d); }
void lcp_hooks(int a, int b, int c, int d, int e, int f, int g) { if (f_hooks) f_hooks(a, b, c, d, e, f, g); }
void lcp_reset(void) { f_reset(); }
int lcp_slots_used(void) { return f_used(); }
