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
#include "nr_scrambling_id_sweep.h"
#include <string.h>
#include <stdbool.h>

void nr_scrambling_id_sweep_init(nr_scrambling_id_sweep_t *s, uint16_t pci, int dmrs_id)
{
  memset(s, 0, sizeof(*s));
  bool seen[1024] = {false};
  int n = 0;
  const uint16_t p = (uint16_t)(pci & 1023); // PCI is always < 1008; masked defensively, not trusted blindly
  s->order[n++] = p;
  seen[p] = true;
  if (dmrs_id >= 0 && dmrs_id < 1024 && !seen[(uint16_t)dmrs_id]) {
    s->order[n++] = (uint16_t)dmrs_id;
    seen[(uint16_t)dmrs_id] = true;
  }
  for (int id = 0; id < 1024; ++id)
    if (!seen[id])
      s->order[n++] = (uint16_t)id;
  s->n = n;
  s->pos = 0;
  s->latched = -1;
  s->tries = 0;
}

int nr_scrambling_id_sweep_current(const nr_scrambling_id_sweep_t *s)
{
  if (!s || s->n <= 0)
    return -1;
  if (s->latched >= 0)
    return s->latched;
  return s->order[s->pos % s->n];
}

void nr_scrambling_id_sweep_feed(nr_scrambling_id_sweep_t *s, int tb_crc_ok)
{
  if (!s || s->n <= 0 || s->latched >= 0)
    return;
  ++s->tries;
  if (tb_crc_ok)
    s->latched = s->order[s->pos % s->n];
  else
    s->pos = (s->pos + 1) % s->n;
}

void nr_scr_link_note(nr_scr_link_t *l, uint16_t rnti, bool dedicated, bool crc_ok)
{
  if (!l)
    return;
  const uint64_t n = __atomic_add_fetch(&l->n, 1, __ATOMIC_RELAXED);
  if (!crc_ok)
    return;
  if (!dedicated) {
    __atomic_store_n(&l->common_pass_n, n, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&l->ded_pass_rnti, (uint32_t)rnti, __ATOMIC_RELAXED);
    __atomic_store_n(&l->ded_pass_n, n, __ATOMIC_RELAXED);
  }
}

bool nr_scr_link_healthy(const nr_scr_link_t *l, uint16_t rnti)
{
  if (!l)
    return false;
  const uint64_t n = __atomic_load_n(&l->n, __ATOMIC_RELAXED);
  const uint64_t c = __atomic_load_n(&l->common_pass_n, __ATOMIC_RELAXED);
  if (c && n - c < NR_SCR_LINK_WINDOW)
    return true;
  const uint64_t d = __atomic_load_n(&l->ded_pass_n, __ATOMIC_RELAXED);
  return d && n - d < NR_SCR_LINK_WINDOW && __atomic_load_n(&l->ded_pass_rnti, __ATOMIC_RELAXED) != rnti;
}
