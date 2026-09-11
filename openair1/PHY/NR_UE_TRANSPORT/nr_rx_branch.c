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

#include "nr_rx_branch.h"
#include "common/utils/LOG/log.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NR_RX_BRANCH_PARSE_BUF 256

static int parse_uint_token(const char *tok, long *out)
{
  if (!tok || !*tok)
    return -1;
  char *end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0' || v < 0)
    return -1;
  *out = v;
  return 0;
}

int nr_rx_branch_set_parse(nr_rx_branch_set_t *set, const char *active_list, const char *phys_map,
                            const char *rx_id_prefix)
{
  if (!set)
    return -1;
  memset(set, 0, sizeof(*set));
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    set->b[i].branch_id = (uint8_t)i;
    set->b[i].physical_channel = -1;
    set->b[i].state = NR_RXB_DISABLED;
  }

  if (!active_list || !*active_list) {
    LOG_E(PHY, "rx_branches: empty active branch list\n");
    return -1;
  }
  if (strlen(active_list) >= NR_RX_BRANCH_PARSE_BUF) {
    LOG_E(PHY, "rx_branches: value too long\n");
    return -1;
  }
  char buf[NR_RX_BRANCH_PARSE_BUF];
  strncpy(buf, active_list, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  bool seen[NR_RX_BRANCH_MAX] = {false};
  int n_active = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    long v;
    if (parse_uint_token(tok, &v) || v >= NR_RX_BRANCH_MAX) {
      LOG_E(PHY, "rx_branches: malformed or out-of-range branch id '%s'\n", tok);
      return -1;
    }
    if (seen[v]) {
      LOG_E(PHY, "rx_branches: duplicate branch id %ld\n", v);
      return -1;
    }
    seen[v] = true;
    set->b[v].state = NR_RXB_ACQUIRING;
    n_active++;
  }
  if (n_active == 0) {
    LOG_E(PHY, "rx_branches: no valid branch ids parsed\n");
    return -1;
  }

  const char *prefix = (rx_id_prefix && *rx_id_prefix) ? rx_id_prefix : "rx";
  /* rx_id is "%s%d" of prefix + a single-digit branch id (0..NR_RX_BRANCH_MAX-1, all one digit),
   * into a NR_RX_BRANCH_ID_LEN(16)-byte buffer: prefix + 1 digit + NUL must fit, so prefix can be
   * at most 14 bytes. A longer prefix would have snprintf silently truncate the trailing digit,
   * making every branch's rx_id collide on the same truncated prefix -- reject it instead. */
  if (strlen(prefix) > NR_RX_BRANCH_ID_LEN - 2) {
    LOG_E(PHY, "rx_id_prefix: too long (max %d chars): '%s'\n", NR_RX_BRANCH_ID_LEN - 2, prefix);
    return -1;
  }
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    if (set->b[i].state != NR_RXB_DISABLED)
      snprintf(set->b[i].rx_id, sizeof(set->b[i].rx_id), "%s%d", prefix, i);
  }

  if (!phys_map || !*phys_map) {
    LOG_E(PHY, "rx_branch_phys_map: empty mapping\n");
    return -1;
  }
  if (strlen(phys_map) >= NR_RX_BRANCH_PARSE_BUF) {
    LOG_E(PHY, "rx_branch_phys_map: value too long\n");
    return -1;
  }
  char buf2[NR_RX_BRANCH_PARSE_BUF];
  strncpy(buf2, phys_map, sizeof(buf2) - 1);
  buf2[sizeof(buf2) - 1] = '\0';

  bool branch_mapped[NR_RX_BRANCH_MAX] = {false};
  bool phys_used[NR_RX_BRANCH_MAX] = {false};
  char *save2 = NULL;
  for (char *tok = strtok_r(buf2, ",", &save2); tok; tok = strtok_r(NULL, ",", &save2)) {
    char *colon = strchr(tok, ':');
    if (!colon) {
      LOG_E(PHY, "rx_branch_phys_map: malformed entry '%s' (expected branch:physical)\n", tok);
      return -1;
    }
    *colon = '\0';
    long branch, phys;
    if (parse_uint_token(tok, &branch) || branch >= NR_RX_BRANCH_MAX) {
      LOG_E(PHY, "rx_branch_phys_map: malformed or out-of-range branch '%s'\n", tok);
      return -1;
    }
    if (parse_uint_token(colon + 1, &phys) || phys >= NR_RX_BRANCH_MAX) {
      LOG_E(PHY, "rx_branch_phys_map: malformed or out-of-range physical channel '%s'\n", colon + 1);
      return -1;
    }
    if (branch_mapped[branch]) {
      LOG_E(PHY, "rx_branch_phys_map: duplicate branch key %ld\n", branch);
      return -1;
    }
    if (phys_used[phys]) {
      LOG_E(PHY, "rx_branch_phys_map: physical channel %ld used twice\n", phys);
      return -1;
    }
    branch_mapped[branch] = true;
    phys_used[phys] = true;
    set->b[branch].physical_channel = (int8_t)phys;
  }

  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    if (set->b[i].state != NR_RXB_DISABLED && set->b[i].physical_channel < 0) {
      LOG_E(PHY, "rx_branch_phys_map: active branch %d has no physical channel mapping\n", i);
      return -1;
    }
  }

  set->n_active = (uint8_t)n_active;
  return 0;
}

int nr_rx_branch_set_check_antennas(const nr_rx_branch_set_t *set, int nb_antennas_rx)
{
  if (!set)
    return -1;
  if (nb_antennas_rx < 0 || set->n_active > (uint8_t)nb_antennas_rx) {
    LOG_E(PHY, "rx_branches: %u active branch(es) exceeds %d available antenna(s)\n",
          set->n_active, nb_antennas_rx);
    return -1;
  }
  return 0;
}

void nr_rx_branch_lock(nr_rx_branch_t *b, uint64_t absolute_slot)
{
  if (!b)
    return;
  if (b->state == NR_RXB_LOST)
    b->counters.relocks++;
  b->state = NR_RXB_LOCKED;
  b->lock_absolute_slot = absolute_slot;
}

void nr_rx_branch_lose_lock(nr_rx_branch_t *b)
{
  if (!b)
    return;
  b->lock_epoch++;
  b->state = NR_RXB_LOST;
}

void nr_rx_branch_set_rf_discontinuity(nr_rx_branch_set_t *set)
{
  if (!set)
    return;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    nr_rx_branch_t *b = &set->b[i];
    if (b->physical_channel < 0)
      continue; /* DISABLED slot: not participating in acquisition */
    b->acq_epoch++;
    b->counters.discontinuities++;
    b->state = NR_RXB_LOST;
  }
}

void nr_rx_branch_reset(nr_rx_branch_t *b)
{
  if (!b)
    return;
  memset(&b->counters, 0, sizeof(b->counters));
  b->lock_absolute_slot = 0;
  b->state = (b->physical_channel >= 0) ? NR_RXB_ACQUIRING : NR_RXB_DISABLED;
  /* branch_id, physical_channel, rx_id, acq_epoch, lock_epoch: untouched -- identity and epochs
   * are never reset, epochs must never go backwards. */
}

int nr_rx_branch_set_dispatch(const nr_rx_branch_set_t *set, nr_rx_branch_dispatch_t *out, int max)
{
  if (!set || !out || max < 1)
    return -1;
  int n = 0;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    const nr_rx_branch_t *b = &set->b[i];
    if (b->physical_channel < 0)
      continue;
    if (n >= max) {
      LOG_E(PHY, "nr_rx_branch_set_dispatch: %d active branches exceed caller capacity %d\n",
            (int)set->n_active, max);
      return -1;
    }
    out[n].branch_id = b->branch_id;
    out[n].physical_channel = b->physical_channel;
    out[n].lock_epoch = b->lock_epoch;
    out[n].acq_epoch = b->acq_epoch;
    n++;
  }
  return n;
}

int nr_rx_branch_dispatch_is_stale(const nr_rx_branch_set_t *set, const nr_rx_branch_dispatch_t *d)
{
  if (!set || !d)
    return 1;
  for (int i = 0; i < NR_RX_BRANCH_MAX; i++) {
    const nr_rx_branch_t *b = &set->b[i];
    if (b->physical_channel < 0 || b->branch_id != d->branch_id)
      continue;
    return (b->lock_epoch != d->lock_epoch || b->acq_epoch != d->acq_epoch) ? 1 : 0;
  }
  return 1; /* names no active branch: cannot be shown fresh, so it is stale */
}
