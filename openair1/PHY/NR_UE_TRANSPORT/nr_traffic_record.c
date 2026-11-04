/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode DCI record: JSON formatter (writer thread) and the emit functions. */
#include "nr_traffic_record.h"
#include "nr_sense_api.h"
#include "nr_traffic_decode.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { char *p; size_t n, w; bool bad; } tj_t;
static void tput(tj_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tput(tj_t *j, const char *fmt, ...)
{
  if (j->bad) return;
  va_list ap; va_start(ap, fmt);
  const int k = vsnprintf(j->p + j->w, j->n - j->w, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= j->n - j->w) j->bad = true; else j->w += (size_t)k;
}
static void tint(tj_t *j, const char *key, int64_t v)   /* -1 (any negative) = unknown = null */
{
  if (v < 0) tput(j, ",\"%s\":null", key); else tput(j, ",\"%s\":%" PRId64, key, v);
}

int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n)
{
  if (!r || !buf || !n) return -1;
  static const char *const lname[] = {"fixed", "solved", "candidate", "unsolved"};
  const bool nolayout = r->layout_state == NR_LAYOUT_CANDIDATE || r->layout_state == NR_LAYOUT_UNSOLVED;
  nr_passive_obs_t o = r->o;
  if (nolayout) o.start_rb = o.nb_rb = o.start_sym = o.nb_sym = o.mcs = o.mcs_table = o.qm = o.nl = o.tbs =
      o.harq_pid = o.ndi = o.rv = -1;
  char g[2048];
  if (nr_sense_grant_to_json(&o, &r->m, g, sizeof g) < 0) return -1;
  char *t = strstr(g, "\"type\":\"grant\"");
  const size_t gl = strlen(g);
  if (!t || gl < 2) return -1;
  tj_t j = {buf, n, 0, false};
  tput(&j, "%.*s\"type\":\"dci\"%.*s", (int)(t - g), g, (int)(gl - 1 - (size_t)(t - g) - 14), t + 14);
  tint(&j, "dci_format", r->dci_format); tint(&j, "ss_type", r->ss_type); tint(&j, "al", r->al);
  tint(&j, "cce", r->cce); tint(&j, "coreset_id", r->coreset_id); tint(&j, "dci_len", r->dci_len ? (int64_t)r->dci_len : -1);
  if (r->dci_len == 0) tput(&j, ",\"dci_bits\":null");   /* bits unavailable (never printed as zeros) */
  else {
    tput(&j, ",\"dci_bits\":\"");
    for (unsigned i = 0; i < (r->dci_len + 7u) / 8u && i < sizeof r->dci_bits; i++) tput(&j, "%02x", r->dci_bits[i]);
    tput(&j, "\"");
  }
  tput(&j, ",\"rnti_confirmed\":%s,\"layout_state\":\"%s\"", r->rnti_confirmed ? "true" : "false",
       lname[(unsigned)r->layout_state & 3u]);
  {
    static const char *const ev[] = {NULL, "bits", "phys", "crc"};
    if (r->layout_evidence >= 1 && r->layout_evidence <= 3) tput(&j, ",\"layout_evidence\":\"%s\"", ev[r->layout_evidence]);
    else tput(&j, ",\"layout_evidence\":null");
    if (r->fields_evidence >= 1 && r->fields_evidence <= 3) tput(&j, ",\"fields_evidence\":\"%s\"", ev[r->fields_evidence]);
    else tput(&j, ",\"fields_evidence\":null");
  }
  tput(&j, ",\"layout_checks_ok\":%u,\"layout_checks_fail\":%u", r->layout_checks_ok, r->layout_checks_fail);
  if (r->m.identity_gen < 0 || r->m.dedicated_epoch < 0) tput(&j, ",\"cfg_ref\":null");
  else tput(&j, ",\"cfg_ref\":\"%d.%d.%u\"", r->m.identity_gen, r->m.dedicated_epoch, r->cfg_ref_ver);
  tint(&j, "bwp_start", r->bwp_start); tint(&j, "bwp_size", r->bwp_size); tint(&j, "start_rb_bwp", r->start_rb_bwp);
#define L(k, f) tint(&j, k, nolayout ? -1 : r->f)
  L("vrb_prb", vrb_prb); L("tda_index", tda_index); L("k0", k0);
#undef L
  /* the HARQ-ACK fields of a split proven from bits are exact whatever the rest of the layout is */
  const bool hq = !nolayout || r->fields_evidence == NR_LAYOUT_EV_BITS;
  tint(&j, "k1", hq ? r->k1 : -1); tint(&j, "dai", hq ? r->dai : -1); tint(&j, "tpc", hq ? r->tpc : -1); tint(&j, "pri", hq ? r->pri : -1);
  tint(&j, "srs_req", r->srs_req); tint(&j, "csi_req", r->csi_req); tint(&j, "ulsch_ind", r->ulsch_ind);
  tint(&j, "cbgti", r->cbgti); tint(&j, "retx", nolayout ? -1 : r->retx);
  tint(&j, "si_ind", r->si_ind); tint(&j, "short_msg_ind", r->short_msg_ind); tint(&j, "short_msg", r->short_msg);
  tint(&j, "tb_scaling", r->tb_scaling);
  tint(&j, "pdsch_abs_slot", nolayout ? -1 : r->pdsch_abs_slot);
  tint(&j, "harq_fb_abs_slot", nolayout ? -1 : r->harq_fb_abs_slot);
  tput(&j, ",\"decode\":%d}", (int)r->decode);
  return j.bad ? -1 : (int)j.w;
}

void nr_sense_emit_dci(nr_sense_dci_rec_t *r)
{
  if (!nr_sense_enabled() || !r) return;
  nr_sense_grant_meta_finish(&r->o, &r->m);
  if (!r->o.t_mono_ns) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); r->o.t_mono_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
  nr_sense_push_dci(r);
}

void nr_sense_emit_decode(uint64_t abs_slot, int64_t dci_abs_slot, uint16_t rnti, int dir, bool probe, int crc, float snr_db, const char *learned)
{
  if (!nr_sense_enabled()) return;
  char line[512], lr[128] = "";
  const bool has_l = learned != NULL;
  if (has_l) { size_t k = 0; for (; *learned && k < sizeof lr - 2; learned++) if (*learned != '"' && *learned != '\\' && (unsigned char)*learned >= 32) lr[k++] = *learned; lr[k] = 0; }
  char dcis[32]; if (dci_abs_slot < 0) strcpy(dcis, "null"); else snprintf(dcis, sizeof dcis, "%" PRId64, dci_abs_slot);
  char snr[32]; if (isfinite(snr_db)) snprintf(snr, sizeof snr, "%.7g", (double)snr_db); else strcpy(snr, "null");
  const int k = snprintf(line, sizeof line,
      "{\"schema\":2,\"type\":\"decode\",\"abs_slot\":%" PRIu64 ",\"dci_abs_slot\":%s,\"rnti\":%u,\"dir\":\"%s\",\"probe\":%s,\"crc\":%d,\"snr_db\":%s,\"learned\":%s%s%s}",
      abs_slot, dcis, (unsigned)rnti, dir ? "UL" : "DL", probe ? "true" : "false", crc, snr,
      has_l ? "\"" : "null", lr, has_l ? "\"" : "");
  if (k > 0 && (size_t)k < sizeof line) nr_sense_push_record(line);
}

/* ---- counters (this cell) ---- */
#include "common/utils/nr_cell.h"
#include <stdatomic.h>
#include <stdlib.h>
#define TS_RING 512
typedef struct { _Atomic uint64_t accepted[4], state[4], cnt[NR_TRAFFIC_CNT_N]; _Atomic uint32_t emit_us[TS_RING]; _Atomic uint32_t w; } traffic_ctr_t;
NR_CELL_STATIC(traffic_ctr_t, g_traffic_ctr)
#define g_traffic_ctr NR_CELL_REF(g_traffic_ctr)

void nr_traffic_count_dci(int fmt, nr_layout_state_t ls, uint64_t emit_ns)
{
  traffic_ctr_t *c = &g_traffic_ctr;
  atomic_fetch_add_explicit(&c->accepted[fmt & 3], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&c->state[(unsigned)ls & 3u], 1, memory_order_relaxed);
  const uint32_t w = atomic_fetch_add_explicit(&c->w, 1, memory_order_relaxed);
  atomic_store_explicit(&c->emit_us[w % TS_RING], (uint32_t)(emit_ns / 1000u) + 1u, memory_order_relaxed); /* +1: 0 = empty */
}

void nr_traffic_count(int what)
{
  if (what >= 0 && what < NR_TRAFFIC_CNT_N) atomic_fetch_add_explicit(&g_traffic_ctr.cnt[what], 1, memory_order_relaxed);
}

static int cmp_u32(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }

void nr_traffic_stats_get(nr_traffic_stats_t *s)
{
  memset(s, 0, sizeof *s);
  traffic_ctr_t *c = &g_traffic_ctr;
  for (int i = 0; i < 4; i++) { s->accepted[i] = atomic_load_explicit(&c->accepted[i], memory_order_relaxed); s->state[i] = atomic_load_explicit(&c->state[i], memory_order_relaxed); }
  s->dup = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_DUP], memory_order_relaxed);
  s->e1_changes = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_CHANGE], memory_order_relaxed);
  s->e1_skip_classes = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_SKIP], memory_order_relaxed);
  s->e1_unfed = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_UNFED], memory_order_relaxed);
  s->e1_deferred = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_DEFERRED], memory_order_relaxed);
  s->e1_deferred_scoped = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_DEFERRED_SCOPED], memory_order_relaxed);
  for (int i = 0; i < 5; i++) s->e1_cls[i] = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_E1_CLS_1 + i], memory_order_relaxed);
  s->traffic_skip = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_SKIP], memory_order_relaxed);
  s->check_dl = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_CHECK], memory_order_relaxed);
  s->check_ul = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_CHECK_UL], memory_order_relaxed);
  s->probe_dl = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_PROBE], memory_order_relaxed);
  s->probe_ul = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_PROBE_UL], memory_order_relaxed);
  s->probe_skip = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_PROBE_SKIP], memory_order_relaxed);
  s->ul_decode_skip = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_UL_SKIP], memory_order_relaxed);
  s->check_decodes = s->check_dl + s->check_ul;
  s->check_fail = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_CHECK_FAIL], memory_order_relaxed);
  s->probe_decodes = s->probe_dl + s->probe_ul;
  s->reopens = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_REOPEN], memory_order_relaxed);
  s->reopens_ul = atomic_load_explicit(&c->cnt[NR_TRAFFIC_CNT_REOPEN_UL], memory_order_relaxed);
  uint32_t v[TS_RING]; int n = 0;
  for (int i = 0; i < TS_RING; i++) { const uint32_t x = atomic_load_explicit(&c->emit_us[i], memory_order_relaxed); if (x) v[n++] = x - 1u; }
  if (n) { qsort(v, (size_t)n, sizeof v[0], cmp_u32); s->emit_us_p50 = v[n / 2]; s->emit_us_p99 = v[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100]; }
}

/* ---- retransmission flag: last NDI per (dir, rnti, pid), direct-mapped (a collision only loses the flag: -1) ---- */
#include <pthread.h>
#define RX_SLOTS 1024
typedef struct { uint32_t key; int8_t ndi; } rx_ent_t;   /* key 0 = empty; key = 1 + (dir<<20 | rnti<<4 | pid) */
typedef struct { rx_ent_t e[RX_SLOTS]; } rx_tab_t;
NR_CELL_STATIC(rx_tab_t, g_retx_tab)
#define g_retx_tab NR_CELL_REF(g_retx_tab)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_retx_mu, PTHREAD_MUTEX_INITIALIZER)
#define g_retx_mu NR_CELL_REF(g_retx_mu)

int nr_traffic_retx(int dir, uint16_t rnti, int pid, int ndi)
{
  if (ndi < 0 || pid < 0) return -1;
  const uint32_t key = 1u + (((uint32_t)(dir & 1) << 20) | ((uint32_t)rnti << 4) | ((uint32_t)pid & 15u));
  rx_ent_t *e = &g_retx_tab.e[(key * 2654435761u) >> 22 & (RX_SLOTS - 1)];
  pthread_mutex_lock(&g_retx_mu);
  const int r = e->key == key ? (e->ndi == ndi ? 1 : 0) : -1;
  e->key = key; e->ndi = (int8_t)ndi;
  pthread_mutex_unlock(&g_retx_mu);
  return r;
}

void nr_traffic_retx_reset(void)
{
  pthread_mutex_lock(&g_retx_mu);
  memset(&g_retx_tab, 0, sizeof g_retx_tab);
  pthread_mutex_unlock(&g_retx_mu);
}

bool nr_traffic_decode_outcome(uint64_t abs_slot, int64_t dci_abs_slot, uint16_t rnti, int dir, bool probe, int rv, int crc, float snr_db)
{
  nr_sense_emit_decode(abs_slot, dci_abs_slot, rnti, dir, probe, crc, snr_db, NULL);
  /* backoff of the UL decode gate (nr_traffic_decode.h): a UL trial/probe failure is not a failure of the gate, a pass is */
  if (crc >= 0 && dir == 1 && (!probe || crc == 1)) nr_traffic_probe_result(dir, rnti, crc == 1);
  if (probe) { nr_traffic_count(dir ? NR_TRAFFIC_CNT_PROBE_UL : NR_TRAFFIC_CNT_PROBE); return false; }
  nr_traffic_count(dir ? NR_TRAFFIC_CNT_CHECK_UL : NR_TRAFFIC_CNT_CHECK);
  if (crc == 0) nr_traffic_count(NR_TRAFFIC_CNT_CHECK_FAIL);
  const bool reopen = crc >= 0 && nr_traffic_decode_result(dir, rnti, rv, snr_db, crc == 1);
  if (reopen) nr_traffic_count(dir ? NR_TRAFFIC_CNT_REOPEN_UL : NR_TRAFFIC_CNT_REOPEN);
  return reopen;
}
