/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode cfg_ref: per cell, per RNTI version table (FNV-1a of the snapshot) + the "config" record on change. */
#include "nr_traffic_cfg.h"
#include "nr_sense_api.h"
#include "common/utils/nr_cell.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CFG_RNTI_N 256
typedef struct { uint16_t rnti; uint8_t fmt; bool used; uint32_t ver; uint64_t hash, stamp; } cfg_ent_t;
/* next_ver: per-cell monotonic, so a (rnti, cfg_ref) never repeats with different content after an LRU eviction. */
typedef struct { cfg_ent_t e[CFG_RNTI_N]; uint64_t clock; uint32_t next_ver; } cfg_tab_t;
NR_CELL_STATIC(cfg_tab_t, g_cfg_tab);
#define g_cfg_tab NR_CELL_REF(g_cfg_tab)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_cfg_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_cfg_mu NR_CELL_REF(g_cfg_mu)

static uint64_t fnv1a(const void *p, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  for (const uint8_t *b = p; n--; b++) h = (h ^ *b) * 1099511628211ULL;
  return h;
}
typedef struct { char *p; size_t n, w; } cj_t;
static void cput(cj_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cput(cj_t *j, const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  const int k = vsnprintf(j->p + j->w, j->n - j->w, fmt, ap);
  va_end(ap);
  if (k > 0 && (size_t)k < j->n - j->w) j->w += (size_t)k;
}
static void cint(cj_t *j, const char *key, int v)   /* -1 = unknown = null */
{
  if (v < 0) cput(j, ",\"%s\":null", key); else cput(j, ",\"%s\":%d", key, v);
}
static void carr(cj_t *j, const char *key, const int8_t *a)
{
  cput(j, ",\"%s\":[", key);
  for (int i = 0; i < 16; i++) { if (i) cput(j, ","); if (a[i] < 0) cput(j, "null"); else cput(j, "%d", a[i]); }
  cput(j, "]");
}
static void emit_config(uint16_t rnti, uint8_t fmt, uint32_t ver, const nr_traffic_cfg_snap_t *s)
{
  char buf[1024]; cj_t j = {buf, sizeof buf, 0};
  const nr_dci11_layout_t *l = &s->layout;
  cput(&j, "{\"type\":\"config\",\"rnti\":%u,\"dci_format\":%u,\"cfg_ref\":\"%u.%u.%u\"", rnti, fmt, s->identity_gen, s->dedicated_epoch, ver);
  cput(&j, ",\"identity_gen\":%u,\"dedicated_epoch\":%u", s->identity_gen, s->dedicated_epoch);
  cint(&j, "coreset_id", s->coreset_id); cint(&j, "coreset_start_rb", s->coreset_start_rb);
  cint(&j, "coreset_n_rb", s->coreset_n_rb); cint(&j, "coreset_dur", s->coreset_dur);
  cint(&j, "bwp_start", s->bwp_start); cint(&j, "bwp_size", s->bwp_size); cint(&j, "layout_rep", s->layout_rep);
  cput(&j, ",\"layout\":{\"bwp_ind\":%u,\"pre_mcs\":%u,\"pre_ant\":%u,\"ant_ports\":%u,\"post_ant\":%u,\"dmrs_type\":%u,"
           "\"fdra_mode\":%u,\"n_rbg\":%u,\"tail\":%u}",
       l->bwp_ind, l->pre_mcs, l->pre_ant, l->ant_ports, l->post_ant, l->dmrs_type, l->fdra_mode, l->n_rbg, l->tail);
  carr(&j, "s_by_tda", s->s_by_tda); carr(&j, "l_by_tda", s->l_by_tda);
  cint(&j, "dmrs_add_pos", s->dmrs_add_pos); cint(&j, "mcs_table", s->mcs_table); cint(&j, "rm_set", s->rm_set);
  cput(&j, "}");
  nr_sense_push_record(buf);
}

uint32_t nr_traffic_cfg_ref(uint16_t rnti, uint8_t dci_format, const nr_traffic_cfg_snap_t *s)
{
  const uint64_t h = fnv1a(s, sizeof *s);
  pthread_mutex_lock(&g_cfg_mu);
  cfg_tab_t *t = &g_cfg_tab;
  cfg_ent_t *e = NULL, *victim = &t->e[0];
  for (int i = 0; i < CFG_RNTI_N; i++) {
    if (t->e[i].used && t->e[i].rnti == rnti && t->e[i].fmt == dci_format) { e = &t->e[i]; break; }
    if (!t->e[i].used) { if (victim->used) victim = &t->e[i]; }
    else if (victim->used && t->e[i].stamp < victim->stamp) victim = &t->e[i];
  }
  bool changed = true;
  if (e) changed = e->hash != h;
  else { e = victim; e->used = true; e->rnti = rnti; e->fmt = dci_format; }
  if (changed) e->ver = ++t->next_ver;
  e->hash = h; e->stamp = ++t->clock;
  const uint32_t ver = e->ver;
  /* emitted under the lock: a same-key DCI cannot reach the ring before its config record */
  if (changed) emit_config(rnti, dci_format, ver, s);
  pthread_mutex_unlock(&g_cfg_mu);
  return ver;
}
void nr_traffic_cfg_reset(void)
{
  pthread_mutex_lock(&g_cfg_mu);
  memset(&g_cfg_tab, 0, sizeof g_cfg_tab);
  pthread_mutex_unlock(&g_cfg_mu);
}
