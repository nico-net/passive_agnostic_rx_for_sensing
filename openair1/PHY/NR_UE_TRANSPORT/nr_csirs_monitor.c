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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_monitor.c
 * \brief Config parsing + per-slot occurrence for UE-agnostic CSI-RS sensing monitors.
 */

#include "nr_csirs_monitor.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"

/// One configured monitor resource: its CSI-RS PDU plus a (period, offset) occurrence rule.
typedef struct {
  fapi_nr_dl_config_csirs_pdu_rel15_t pdu;
  uint32_t                            period_slots; ///< slow-time period, in slots (>0)
  uint32_t                            offset_slots; ///< absolute-slot offset within the period
  int16_t                             slot_delta;
  bool                                dynamic;
} nr_csirs_monitor_res_t;

static nr_csirs_monitor_res_t g_monitors[NR_CSIRS_MONITOR_MAX];
static int                    g_n_monitors = 0;
static int                    g_parsed     = 0;
static pthread_mutex_t        g_monitor_lock = PTHREAD_MUTEX_INITIALIZER;

static bool same_geometry(const fapi_nr_dl_config_csirs_pdu_rel15_t *a,
                          const fapi_nr_dl_config_csirs_pdu_rel15_t *b)
{
  return a->row == b->row && a->freq_domain == b->freq_domain && a->symb_l0 == b->symb_l0
      && a->symb_l1 == b->symb_l1 && a->cdm_type == b->cdm_type && a->freq_density == b->freq_density
      && a->start_rb == b->start_rb && a->nr_of_rbs == b->nr_of_rbs && a->scramb_id == b->scramb_id;
}

/**
 * @brief Parse one "row:start_rb:nr_rbs:freq_domain:symb_l0:symb_l1:cdm_type:freq_density:scramb_id:period:offset"
 * entry into a monitor resource. Returns 1 on success, 0 on a malformed entry.
 */
static int parse_one(const char* item, nr_csirs_monitor_res_t* m)
{
  int row, start_rb, nr_rbs, freq_domain, symb_l0, symb_l1, cdm_type, freq_density, scramb_id, period, offset;
  const int n = sscanf(item, "%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d", &row, &start_rb, &nr_rbs, &freq_domain, &symb_l0,
                       &symb_l1, &cdm_type, &freq_density, &scramb_id, &period, &offset);
  if (n != 11) {
    LOG_E(PHY,
          "SENSING: malformed csirs_monitor entry '%s' (need 11 fields "
          "row:start_rb:nr_rbs:freq_domain:symb_l0:symb_l1:cdm_type:freq_density:scramb_id:period:offset)\n",
          item);
    return 0;
  }
  if (period <= 0) {
    LOG_E(PHY, "SENSING: csirs_monitor entry '%s' has non-positive period; ignored\n", item);
    return 0;
  }

  memset(m, 0, sizeof(*m));
  fapi_nr_dl_config_csirs_pdu_rel15_t* p = &m->pdu;
  p->csi_type          = 1; // NZP CSI-RS (the only type the PHY estimation path handles today)
  p->start_rb          = (uint16_t)start_rb;
  p->nr_of_rbs         = (uint16_t)nr_rbs;
  p->row               = (uint8_t)row;
  p->freq_domain       = (uint16_t)freq_domain;
  p->symb_l0           = (uint8_t)symb_l0;
  p->symb_l1           = (uint8_t)symb_l1;
  p->cdm_type          = (uint8_t)cdm_type;
  p->freq_density      = (uint8_t)freq_density;
  p->scramb_id         = (uint16_t)scramb_id;
  // Sensing-only markers: non-zero so any downstream ">0 measurement" guards stay satisfied. The lean
  // capture path never switches on these, and never reports CSI to MAC.
  p->measurement_bitmap     = 2;
  p->power_control_offset    = 0;
  p->power_control_offset_ss = 1; // 0 dB NZP-CSI-RS-EPRE-to-SSB ratio (amplitude only; cancels in Ĥ=Y/X)

  m->period_slots = (uint32_t)period;
  m->offset_slots = (uint32_t)offset % (uint32_t)period;
  return 1;
}

void nr_csirs_monitor_init(void)
{
  pthread_mutex_lock(&g_monitor_lock);
  if (g_parsed) {
    pthread_mutex_unlock(&g_monitor_lock);
    return;
  }
  g_parsed = 1;

  char*      p_monitor = NULL;
  paramdef_t params[]  = {
      {"csirs_monitor",
        "UE-agnostic CSI-RS sensing monitors; comma-separated resources, each "
        "row:start_rb:nr_rbs:freq_domain:symb_l0:symb_l1:cdm_type:freq_density:scramb_id:period:offset",
        0, .strptr = &p_monitor, .defstrval = "", TYPE_STRING, 0},
  };
  config_get(config_get_if(), params, (int)(sizeof(params) / sizeof(params[0])), "sensing");

  if (p_monitor == NULL || p_monitor[0] == '\0') {
    pthread_mutex_unlock(&g_monitor_lock);
    return; // monitor disabled
  }

  // Split on commas; parse each entry.
  const char* s   = p_monitor;
  size_t      pos = 0;
  const size_t len = strlen(s);
  char        buf[128];
  while (pos < len && g_n_monitors < NR_CSIRS_MONITOR_MAX) {
    const char* comma = strchr(s + pos, ',');
    size_t      end   = comma ? (size_t)(comma - s) : len;
    size_t      n     = end - pos;
    if (n == 0) {
      pos = end + 1;
      continue;
    }
    if (n >= sizeof(buf)) {
      n = sizeof(buf) - 1;
    }
    memcpy(buf, s + pos, n);
    buf[n] = '\0';
    // trim leading whitespace
    char* item = buf;
    while (*item == ' ' || *item == '\t') {
      item++;
    }
    if (*item != '\0' && parse_one(item, &g_monitors[g_n_monitors])) {
      g_n_monitors++;
    }
    pos = comma ? end + 1 : len;
  }

  LOG_I(PHY, "SENSING: %d UE-agnostic CSI-RS monitor resource(s) configured\n", g_n_monitors);
  for (int i = 0; i < g_n_monitors; i++) {
    const nr_csirs_monitor_res_t* m = &g_monitors[i];
    LOG_I(PHY,
          "SENSING:   monitor[%d] row=%u rb=[%u..%u) freq_domain=0x%x symb_l0=%u symb_l1=%u cdm=%u density=%u "
          "scramb=%u period=%u offset=%u\n",
          i, m->pdu.row, m->pdu.start_rb, m->pdu.start_rb + m->pdu.nr_of_rbs, m->pdu.freq_domain, m->pdu.symb_l0,
          m->pdu.symb_l1, m->pdu.cdm_type, m->pdu.freq_density, m->pdu.scramb_id, m->period_slots, m->offset_slots);
  }
  pthread_mutex_unlock(&g_monitor_lock);
}

int nr_csirs_monitor_enabled(void)
{
  pthread_mutex_lock(&g_monitor_lock);
  const int enabled = g_n_monitors > 0;
  pthread_mutex_unlock(&g_monitor_lock);
  return enabled;
}

int nr_csirs_monitor_due(int                                         frame,
                         int                                         slot,
                         int                                         slots_per_frame,
                         const fapi_nr_dl_config_csirs_pdu_rel15_t** out,
                         int                                         max)
{
  return nr_csirs_monitor_due_ex(frame, slot, slots_per_frame, out, NULL, max);
}

int nr_csirs_monitor_due_ex(int frame,
                            int slot,
                            int slots_per_frame,
                            const fapi_nr_dl_config_csirs_pdu_rel15_t **out,
                            int16_t *slot_delta,
                            int max)
{
  if (out == NULL || max <= 0 || slots_per_frame <= 0) {
    return 0;
  }
  static __thread fapi_nr_dl_config_csirs_pdu_rel15_t snapshot[NR_CSIRS_MONITOR_MAX];
  pthread_mutex_lock(&g_monitor_lock);
  const uint32_t abs_slot = (uint32_t)frame * (uint32_t)slots_per_frame + (uint32_t)slot;
  int            count    = 0;
  for (int i = 0; i < g_n_monitors && count < max; i++) {
    const nr_csirs_monitor_res_t* m = &g_monitors[i];
    if ((abs_slot % m->period_slots) == m->offset_slots) {
      snapshot[count] = m->pdu;
      out[count] = &snapshot[count];
      if (slot_delta)
        slot_delta[count] = m->slot_delta;
      count++;
    }
  }
  pthread_mutex_unlock(&g_monitor_lock);
  return count;
}

int nr_csirs_monitor_add_dynamic(const fapi_nr_dl_config_csirs_pdu_rel15_t *pdu,
                                 uint16_t period,
                                 uint16_t offset,
                                 int16_t slot_delta)
{
  if (!pdu || period == 0)
    return -1;
  pthread_mutex_lock(&g_monitor_lock);
  for (int i = 0; i < g_n_monitors; ++i) {
    if (same_geometry(&g_monitors[i].pdu, pdu)) {
      /* A manually configured resource owns its lifetime.  Discovery may
       * rediscover it, but must not turn it into an epoch-scoped entry that a
       * later revoke/remove-all operation can silently delete. */
      if (!g_monitors[i].dynamic) {
        if (g_monitors[i].period_slots == period && g_monitors[i].offset_slots == offset % period) {
          pthread_mutex_unlock(&g_monitor_lock);
          return i;
        }
      } else if (g_monitors[i].period_slots == period && g_monitors[i].offset_slots == offset % period) {
        g_monitors[i].slot_delta = slot_delta;
        pthread_mutex_unlock(&g_monitor_lock);
        return i;
      }
    }
  }
  if (g_n_monitors >= NR_CSIRS_MONITOR_MAX) {
    pthread_mutex_unlock(&g_monitor_lock);
    return -1;
  }
  const int index = g_n_monitors++;
  g_monitors[index] = (nr_csirs_monitor_res_t){.pdu = *pdu,
                                               .period_slots = period,
                                               .offset_slots = offset % period,
                                               .slot_delta = slot_delta,
                                               .dynamic = true};
  pthread_mutex_unlock(&g_monitor_lock);
  LOG_A(PHY,
        "SENSING: CSIRS_DISC MONITOR_ADD row=%u fd=%u rb=%u:%u id=%u P/O=%u/%u delta=%d\n",
        pdu->row,
        pdu->freq_domain,
        pdu->start_rb,
        pdu->nr_of_rbs,
        pdu->scramb_id,
        period,
        offset % period,
        slot_delta);
  return index;
}

void nr_csirs_monitor_remove_dynamic(const fapi_nr_dl_config_csirs_pdu_rel15_t *pdu)
{
  if (!pdu)
    return;
  pthread_mutex_lock(&g_monitor_lock);
  for (int i = 0; i < g_n_monitors;) {
    if (g_monitors[i].dynamic && same_geometry(&g_monitors[i].pdu, pdu)) {
      memmove(&g_monitors[i], &g_monitors[i + 1], (size_t)(g_n_monitors - i - 1) * sizeof(g_monitors[0]));
      g_n_monitors--;
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&g_monitor_lock);
}

void nr_csirs_monitor_remove_dynamic_all(void)
{
  pthread_mutex_lock(&g_monitor_lock);
  for (int i = 0; i < g_n_monitors;) {
    if (g_monitors[i].dynamic) {
      memmove(&g_monitors[i], &g_monitors[i + 1], (size_t)(g_n_monitors - i - 1) * sizeof(g_monitors[0]));
      g_n_monitors--;
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&g_monitor_lock);
}
