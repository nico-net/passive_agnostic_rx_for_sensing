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
#include "nr_polar_gpu.h"
#include "nr_polar_sc_cuda.h"

#include <pthread.h>

#define NPG_MAX_ITEMS 2304 /* dlsweep: 64 candidates x 34 lengths = 2176 per occasion */

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
