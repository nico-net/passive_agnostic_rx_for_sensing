/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * libpolar_gpu.so: the dlopen'd side of nr_polar_gpu.h. All it does is adapt an occasion's
 * candidate list to nr_polar_sc_cuda.h's batch API (register each (dci_length, AL) once, pack the
 * items the GPU can take, scatter the results back) and serialise submissions -- npc_register()
 * and npc_decode_batch() share device-side state, and two scan consumer threads can be inside an
 * occasion at the same time.
 */
#define NR_GPU_POLAR_NO_LOADER
#include <stdio.h>
#include "nr_polar_gpu.h"
#include "nr_polar_sc_cuda.h"

#include <pthread.h>

/* Sized for the LANE BATCH: every lookahead lane's (candidate x length) grid in ONE call, which is
 * the whole point -- at 96 lanes the per-CALL overhead (~90 us) was the entire cost, not the decodes.
 * MUST be >= LANE_BATCH_MAX_ITEMS in nr_pdcch_blind_monitor_rt.c. A smaller value here makes
 * decode_vec return -1, so every lane falls back to CPU AFTER the batch was already built and
 * unscrambled: strictly slower than not batching at all, and silent. Both lower layers
 * (npc_decode_batch_vec, npc_gpu_decode) size their host/device buffers off n, so only this
 * shim's static arrays needed raising (~590 kB BSS). */
#define NPG_MAX_ITEMS 131072 /* dlsweep alone needs 64 cand x 34 len = 2176; the lane batch needs all K */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int npg_decode(const int16_t *llr, int stride, const uint16_t *len, const uint8_t *al, int n,
                      uint32_t *crc, uint64_t *payload, uint8_t *ok)
{
  if (llr == NULL || len == NULL || al == NULL || crc == NULL || payload == NULL || ok == NULL)
    return -1;
  if (n <= 0 || n > NPG_MAX_ITEMS || stride <= 0)
    return -1;

  /* Static, not stack: ~70 KB at this cap. Only touched under g_lock. */
  static npc_item_t item[NPG_MAX_ITEMS];
  static int map[NPG_MAX_ITEMS];
  static uint32_t c[NPG_MAX_ITEMS];
  static uint64_t p[NPG_MAX_ITEMS];
  int m = 0;

  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < n; i++) {
    ok[i] = 0;
    /* AL16 is 1728 coded bits, past the kernel's NPC_MAX_E; and the params table is finite, so a
     * long length sweep can legitimately run out. Both cases fall back, they are not failures. */
    if ((int)al[i] * 108 > NPC_MAX_E)
      continue;
    const int pid = npc_register(len[i], al[i]);
    if (pid < 0)
      continue;
    item[m].pid = pid;
    item[m].llr = llr + (size_t)i * (size_t)stride;
    map[m] = i;
    m++;
  }
  const int rc = (m > 0) ? npc_decode_batch(item, m, c, p) : 0;
  if (rc == 0)
    for (int j = 0; j < m; j++) {
      crc[map[j]] = c[j];
      payload[map[j]] = p[j];
      ok[map[j]] = 1;
    }
  pthread_mutex_unlock(&g_lock);
  return (rc != 0) ? -1 : m;
}

static int npg_decode_vec(const int16_t *vec, int vstride, int n_vec, const uint16_t *vidx, const uint16_t *len,
                          const uint8_t *al, int n, uint32_t *crc, uint64_t *payload, uint8_t *ok)
{
  if (vec == NULL || vidx == NULL || len == NULL || al == NULL || crc == NULL || payload == NULL || ok == NULL)
    return -1;
  if (n <= 0 || n > NPG_MAX_ITEMS || n_vec <= 0 || vstride <= 0)
    return -1;
  static int pid[NPG_MAX_ITEMS], vi[NPG_MAX_ITEMS], map[NPG_MAX_ITEMS];
  static uint32_t c[NPG_MAX_ITEMS];
  static uint64_t p[NPG_MAX_ITEMS];
  int m = 0;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < n; i++) {
    ok[i] = 0;
    if ((int)al[i] * 108 > NPC_MAX_E || (int)al[i] * 108 > vstride || vidx[i] >= n_vec)
      continue;
    const int id = npc_register(len[i], al[i]);
    if (id < 0)
      continue;
    pid[m] = id;
    vi[m] = vidx[i];
    map[m] = i;
    m++;
  }
  const int rc = (m > 0) ? npc_decode_batch_vec(vec, vstride, n_vec, vi, pid, m, c, p) : 0;
  if (rc == 0 && m > 0) {
    static unsigned long s_n = 0;
    static double s_a = 0, s_b = 0, s_c = 0, s_d = 0;
    const npc_timing_t t = npc_last_timing();
    s_a += t.h2d; s_b += t.kernel; s_c += t.d2h; s_d += t.host;
    const unsigned long nn = ++s_n;
    if (nn == 1 || (nn % 500) == 0)
      fprintf(stderr, "SENSING: GPUTIME calls=%lu items=%d nvec=%d mean_us h2d=%.0f kern=%.0f d2h=%.0f finish=%.0f sum=%.0f\n",
              nn, m, n_vec, s_a / nn, s_b / nn, s_c / nn, s_d / nn, (s_a + s_b + s_c + s_d) / nn);
  }
  if (rc == 0)
    for (int j = 0; j < m; j++) {
      crc[map[j]] = c[j];
      payload[map[j]] = p[j];
      ok[map[j]] = 1;
    }
  pthread_mutex_unlock(&g_lock);
  return (rc != 0) ? -1 : m;
}

static const nr_gpu_polar_api_t g_api = {.decode = npg_decode, .decode_vec = npg_decode_vec};

const nr_gpu_polar_api_t *nr_gpu_polar_api(void);
const nr_gpu_polar_api_t *nr_gpu_polar_api(void)
{
  return &g_api;
}
