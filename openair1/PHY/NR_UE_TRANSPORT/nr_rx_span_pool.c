/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "nr_rx_span_pool.h"
#include "common/utils/LOG/log.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int nr_rx_span_pool_init(nr_rx_span_pool_t *pool, int n_ch, int samples_per_buf, int n_buf,
                          int hold_budget, uint8_t active_branches, uint64_t sample_rate_hz)
{
  if (!pool) {
    LOG_E(PHY, "nr_rx_span_pool_init: NULL pool\n");
    return -1;
  }
  /* fix round 1 (review MINOR): reject double-init instead of leaking the previous bufs/storage/
   * free_list allocations and re-initializing a mutex that may still be in use. Deliberately does
   * NOT touch *pool in this one rejection case -- the whole point is to leave the live pool alone. */
  if (pool->magic == NR_RX_SPAN_POOL_MAGIC) {
    LOG_E(PHY, "nr_rx_span_pool_init: pool is already initialized (call nr_rx_span_pool_destroy() first)\n");
    return -1;
  }
  memset(pool, 0, sizeof(*pool));

  if (n_ch <= 0 || n_ch > NR_RX_BRANCH_MAX) {
    LOG_E(PHY, "nr_rx_span_pool_init: invalid n_ch %d\n", n_ch);
    return -1;
  }
  if (samples_per_buf <= 0) {
    LOG_E(PHY, "nr_rx_span_pool_init: invalid samples_per_buf %d\n", samples_per_buf);
    return -1;
  }
  if (n_buf <= 0) {
    LOG_E(PHY, "nr_rx_span_pool_init: invalid n_buf %d\n", n_buf);
    return -1;
  }
  if (hold_budget <= 0 || hold_budget > NR_RX_SPAN_POOL_MAX_HOLD) {
    LOG_E(PHY, "nr_rx_span_pool_init: invalid hold_budget %d (max %d)\n", hold_budget,
          NR_RX_SPAN_POOL_MAX_HOLD);
    return -1;
  }
  if (active_branches >> NR_RX_BRANCH_MAX) {
    LOG_E(PHY, "nr_rx_span_pool_init: active_branches 0x%x has a bit >= NR_RX_BRANCH_MAX\n",
          active_branches);
    return -1;
  }
  int n_active = 0;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++)
    if (active_branches & (1u << i))
      n_active++;
  if (n_active == 0) {
    LOG_E(PHY, "nr_rx_span_pool_init: active_branches has no bit set\n");
    return -1;
  }
  if (n_buf < 1 + n_active * hold_budget) {
    LOG_E(PHY,
          "nr_rx_span_pool_init: n_buf %d < 1 + n_active_branches(%d)*hold_budget(%d) -- producer "
          "starvation would not be impossible by construction\n",
          n_buf, n_active, hold_budget);
    return -1;
  }

  pool->bufs = calloc((size_t)n_buf, sizeof(nr_rx_span_t));
  pool->storage = calloc((size_t)n_buf * n_ch * samples_per_buf, sizeof(c16_t));
  pool->free_list = calloc((size_t)n_buf, sizeof(int));
  if (!pool->bufs || !pool->storage || !pool->free_list) {
    LOG_E(PHY, "nr_rx_span_pool_init: allocation failed\n");
    free(pool->bufs);
    free(pool->storage);
    free(pool->free_list);
    memset(pool, 0, sizeof(*pool));
    return -1;
  }

  pool->n_buf = n_buf;
  pool->n_ch = n_ch;
  pool->samples_per_buf = samples_per_buf;
  pool->hold_budget = hold_budget;
  pool->sample_rate_hz = sample_rate_hz;
  pool->n_active_branches = (uint8_t)n_active;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++)
    pool->branch_active[i] = (active_branches & (1u << i)) ? 1 : 0;

  for (int i = 0; i < n_buf; i++) {
    pool->bufs[i].data = pool->storage + (size_t)i * n_ch * samples_per_buf;
    pool->bufs[i].buf_id = i;
    pool->bufs[i].refcount = 0;
    pool->bufs[i].n_ch = n_ch;
    pool->bufs[i].samples_per_buf = samples_per_buf;
    pool->free_list[i] = i;
  }
  pool->n_free = n_buf;

  pthread_mutex_init(&pool->lock, NULL);
  pool->magic = NR_RX_SPAN_POOL_MAGIC;
  return 0;
}

void nr_rx_span_pool_destroy(nr_rx_span_pool_t *pool)
{
  if (!pool)
    return;
  if (pool->bufs)
    pthread_mutex_destroy(&pool->lock);
  free(pool->bufs);
  free(pool->storage);
  free(pool->free_list);
  memset(pool, 0, sizeof(*pool));
}

nr_rx_span_t *nr_rx_span_pool_acquire(nr_rx_span_pool_t *pool)
{
  if (!pool) {
    LOG_E(PHY, "nr_rx_span_pool_acquire: NULL pool\n");
    return NULL;
  }
  pthread_mutex_lock(&pool->lock);
  if (pool->n_free == 0) {
    pool->producer_stalls++;
    pthread_mutex_unlock(&pool->lock);
    LOG_E(PHY, "nr_rx_span_pool_acquire: no free buffer (producer_stalls=%lu)\n",
          (unsigned long)pool->producer_stalls);
    return NULL;
  }
  int buf_id = pool->free_list[--pool->n_free];
  nr_rx_span_t *buf = &pool->bufs[buf_id];
  assert(buf->refcount == 0); /* free list must only ever hold released buffers */
  assert((int)__builtin_popcount(buf->holders) == buf->refcount); /* fix round 1: refcount/holders
                                                                       agreement, checked at every
                                                                       touch point */
  buf->n_samples = 0;
  buf->first_sample_ts = 0;
  buf->absolute_slot = 0;
  buf->acq_epoch = 0;
  pthread_mutex_unlock(&pool->lock);
  return buf;
}

/* Caller holds pool->lock. Releases branch_id's oldest ring entry (drop policy): clears
 * branch_id's own bit in the dropped buffer's holders mask before decrementing refcount, exactly
 * like an explicit release() (fix round 1) -- a drop is branch_id relinquishing its own share,
 * not some other branch's. */
static void drop_oldest(nr_rx_span_pool_t *pool, nr_rx_span_branch_state_t *bs, uint8_t branch_id)
{
  int old_id = bs->buf_id[0];
  nr_rx_span_t *old_buf = &pool->bufs[old_id];
  old_buf->holders &= (uint8_t)~(1u << branch_id);
  old_buf->refcount--;
  assert((int)__builtin_popcount(old_buf->holders) == old_buf->refcount);
  bs->samples_dropped += old_buf->n_samples;
  bs->dropped_spans++;
  if (old_buf->refcount == 0)
    pool->free_list[pool->n_free++] = old_id;
  memmove(&bs->buf_id[0], &bs->buf_id[1], (size_t)(bs->n_held - 1) * sizeof(int));
  bs->n_held--;
}

void nr_rx_span_pool_publish(nr_rx_span_pool_t *pool, nr_rx_span_t *buf)
{
  if (!pool || !buf) {
    LOG_E(PHY, "nr_rx_span_pool_publish: NULL pool/buf\n");
    return;
  }
  if (buf->buf_id < 0 || buf->buf_id >= pool->n_buf || &pool->bufs[buf->buf_id] != buf) {
    LOG_E(PHY, "nr_rx_span_pool_publish: buf does not belong to this pool\n");
    return;
  }
  pthread_mutex_lock(&pool->lock);
  buf->refcount = pool->n_active_branches;
  buf->holders = 0;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    if (!pool->branch_active[i])
      continue;
    nr_rx_span_branch_state_t *bs = &pool->branch[i];
    if (bs->n_held == pool->hold_budget)
      drop_oldest(pool, bs, (uint8_t)i);
    bs->buf_id[bs->n_held++] = buf->buf_id;
    buf->holders |= (uint8_t)(1u << i);
  }
  assert((int)__builtin_popcount(buf->holders) == buf->refcount);
  pthread_mutex_unlock(&pool->lock);
}

const nr_rx_span_t *nr_rx_span_pool_take(nr_rx_span_pool_t *pool, uint8_t branch_id)
{
  if (!pool || branch_id >= NR_RX_BRANCH_MAX || !pool->branch_active[branch_id]) {
    LOG_E(PHY, "nr_rx_span_pool_take: invalid or inactive branch_id %u\n", branch_id);
    return NULL;
  }
  pthread_mutex_lock(&pool->lock);
  nr_rx_span_branch_state_t *bs = &pool->branch[branch_id];
  if (bs->n_held == 0) {
    pthread_mutex_unlock(&pool->lock);
    return NULL; /* nothing waiting -- normal poll outcome, not an error */
  }
  int buf_id = bs->buf_id[0];
  memmove(&bs->buf_id[0], &bs->buf_id[1], (size_t)(bs->n_held - 1) * sizeof(int));
  bs->n_held--;
  pthread_mutex_unlock(&pool->lock);
  return &pool->bufs[buf_id];
}

int nr_rx_span_pool_release(nr_rx_span_pool_t *pool, uint8_t branch_id, const nr_rx_span_t *span)
{
  if (!pool || !span || branch_id >= NR_RX_BRANCH_MAX) {
    LOG_E(PHY, "nr_rx_span_pool_release: NULL pool/span or invalid branch_id %u\n", branch_id);
    return -1;
  }
  if (span->buf_id < 0 || span->buf_id >= pool->n_buf || &pool->bufs[span->buf_id] != span) {
    LOG_E(PHY, "nr_rx_span_pool_release: span does not belong to this pool\n");
    return -1;
  }
  pthread_mutex_lock(&pool->lock);
  nr_rx_span_t *buf = &pool->bufs[span->buf_id];
  /* fix round 1 (review IMPORTANT finding): gate the decrement on branch_id's OWN bit in
   * buf->holders, not just buf->refcount > 0. refcount alone is one shared integer and cannot
   * tell which branches hold a share -- without this check, releasing through any other ACTIVE
   * branch_id would silently decrement a reference a different branch still needs (the exact
   * stale-read hazard this module exists to prevent). This also covers the plain double-release
   * case: once a branch's bit is cleared, releasing again through the same branch_id is rejected
   * the same way. */
  if (!(buf->holders & (1u << branch_id))) {
    pthread_mutex_unlock(&pool->lock);
    LOG_E(PHY, "nr_rx_span_pool_release: branch %u does not hold a ref on buf_id %d (double "
          "release, or release through the wrong branch_id)\n", branch_id, buf->buf_id);
    return -1;
  }
  buf->holders &= (uint8_t)~(1u << branch_id);
  buf->refcount--;
  assert((int)__builtin_popcount(buf->holders) == buf->refcount);
  if (buf->refcount == 0)
    pool->free_list[pool->n_free++] = buf->buf_id;
  pthread_mutex_unlock(&pool->lock);
  return 0;
}

const c16_t *nr_rx_span_channel(const nr_rx_span_t *span, int physical_channel)
{
  if (!span || physical_channel < 0 || physical_channel >= span->n_ch)
    return NULL;
  return span->data + (size_t)physical_channel * span->samples_per_buf;
}

nr_rx_span_view_t nr_rx_span_for_branch(const nr_rx_span_t *span, const nr_rx_branch_t *branch)
{
  nr_rx_span_view_t v = {0};
  v.samples = NULL;
  v.branch_id = branch ? branch->branch_id : 0;
  v.physical_channel = branch ? branch->physical_channel : -1;
  if (!span || !branch || branch->physical_channel < 0)
    return v;
  v.samples = nr_rx_span_channel(span, branch->physical_channel);
  return v;
}
