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
  logInit();
  char p[] = "n";
  initTpool(p, &pool, false);
  return f_init();
}

static int run(int bg1, int C, const short *llr, uint8_t *ok, uint8_t *decoder_used, int iters)
{
  const int Z = bg1 ? 384 : 96, K = bg1 ? 8448 : 960, E = bg1 ? 25344 : 4800, Kc = bg1 ? 68 : 52;
  int16_t *d = calloc((size_t)C * Kc * Z, sizeof(int16_t));
  uint8_t *c = malloc((size_t)C * (K >> 3));
  memset(c, 0xEE, (size_t)C * (K >> 3)); /* the decoder must overwrite every block */
  uint32_t processed = 0;
  nrLDPC_TB_decoding_parameters_t *tb = calloc(1, sizeof(*tb));
  tb->processedSegments = &processed;
  tb->Qm = 2; tb->BG = bg1 ? 1 : 2; tb->max_ldpc_iterations = iters; tb->tbslbrm = 100000000;
  tb->K = K; tb->Z = Z; tb->F = 0; tb->C = C;
  tb->A = C == 1 ? K - 24 : C * (K - 24) - 24;
  if (!bg1 && C == 1)
    tb->A = K - 16;
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
  free(d); free(c); free(tb);
  return rc;
}

int lcp_run_tb(int bg1, int C, const uint8_t *noise, uint8_t *ok, uint8_t *decoder_used)
{
  const int E = bg1 ? 25344 : 4800;
  short *llr = malloc((size_t)C * E * sizeof(short));
  for (int r = 0; r < C; r++)
    for (int i = 0; i < E; i++)
      llr[(size_t)r * E + i] = noise[r] ? (short)((rand() % 81) - 40) : 40;
  const int rc = run(bg1, C, llr, ok, decoder_used, 10);
  free(llr);
  return rc;
}

static double gauss(void)
{
  return sqrt(-2.0 * log(1.0 - drand48())) * cos(2 * M_PI * drand48());
}
int lcp_bler_trial(double ebn0_db, uint8_t *decoder_used)
{
  static short llr[25344];
  const double sigma = 1.0 / sqrt(2 * pow(10, ebn0_db / 10.0) / 3.0); /* ldpctest: SNR_lin = EbN0 * 1/3 */
  for (int i = 0; i < 25344; i++) {
    double q = 16.0 * (1.0 + sigma * gauss()) / sigma; /* int8-range scaling comparable to ldpctest's quantizer */
    llr[i] = (short)(q > 32000 ? 32000 : q < -32000 ? -32000 : lround(q));
  }
  uint8_t ok = 0;
  const int rc = run(1, 1, llr, &ok, decoder_used, 8);
  return rc != 0 || !ok;
}

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
