/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "ldpc_cuda_pool_test_helper.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "common/utils/threadPool/thread-pool.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"

#define BGV 2
#define ZV 96
#define KV 960
#define EV 4800
#define KC 52

static void *h;
static int32_t (*f_init)(void);
static int32_t (*f_dec)(nrLDPC_slot_decoding_parameters_t *);
static int (*f_pool)(uint32_t, uint32_t, uint32_t, int, const uint32_t *, const uint32_t *, const uint32_t *, int *);
static int8_t *(*f_llr)(void);
static uint8_t *(*f_bits)(void);
static void (*f_ctr)(uint64_t *, uint64_t *, uint64_t *);
static tpool_t pool;

int lcp_load(void)
{
  if (h)
    return 0;
  h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_GLOBAL);
  if (!h)
    return -1;
  f_init = dlsym(h, "nrLDPC_coding_init");
  f_dec = dlsym(h, "nrLDPC_coding_decoder");
  f_pool = dlsym(h, "ldpc_pool_decode");
  f_llr = dlsym(h, "ldpc_pool_host_llr");
  f_bits = dlsym(h, "ldpc_pool_host_bits");
  f_ctr = dlsym(h, "ldpc_cuda_get_counters");
  if (!(f_init && f_dec && f_pool && f_llr && f_bits && f_ctr))
    return -2;
  logInit();
  char p[] = "n";
  initTpool(p, &pool, false);
  return f_init();
}

int lcp_run_tb(int C, const uint8_t *noise, uint8_t *ok, uint8_t *decoder_used)
{
  short *llr = malloc((size_t)C * EV * sizeof(short));
  int16_t *d = calloc((size_t)C * KC * ZV, sizeof(int16_t));
  uint8_t *c = malloc((size_t)C * (KV >> 3));
  memset(c, 0xEE, (size_t)C * (KV >> 3)); /* the decoder must overwrite every block */
  for (int r = 0; r < C; r++)
    for (int i = 0; i < EV; i++)
      llr[(size_t)r * EV + i] = noise[r] ? (short)((rand() % 81) - 40) : 40;
  uint32_t processed = 0;
  nrLDPC_TB_decoding_parameters_t *tb = calloc(1, sizeof(*tb));
  tb->processedSegments = &processed;
  tb->Qm = 2; tb->BG = BGV; tb->max_ldpc_iterations = 10; tb->tbslbrm = 100000;
  tb->K = KV; tb->Z = ZV; tb->F = 0; tb->C = C;
  tb->A = C == 1 ? KV - 16 : C * (KV - 24) - 24;
  tb->E = EV; tb->R = 15; tb->E2 = EV; tb->R2 = 15; tb->first_rE2 = C;
  tb->llr = llr; tb->c = c; tb->d = d; tb->d_to_be_cleared = true;
  nrLDPC_slot_decoding_parameters_t slot = {0};
  slot.nb_TBs = 1; slot.threadPool = &pool; slot.TBs = tb;
  const int rc = f_dec(&slot);
  for (int r = 0; r < C; r++)
    ok[r] = tb->decodeSuccess[r];
  *decoder_used = tb->decoder_used;
  free(llr); free(d); free(c); free(tb);
  return rc;
}

int lcp_pool_decode(uint32_t BG, uint32_t Z, uint32_t iters, uint32_t first, uint32_t count, uint32_t K, int *req_rc)
{
  return f_pool(BG, Z, iters, 1, &first, &count, &K, req_rc);
}
int8_t *lcp_host_llr(void) { return f_llr(); }
uint8_t *lcp_host_bits(void) { return f_bits(); }
void lcp_counters(uint64_t *e, uint64_t *f, uint64_t *p) { f_ctr(e, f, p); }
