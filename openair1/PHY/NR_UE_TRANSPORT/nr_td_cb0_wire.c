/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Runtime wiring of the CB0 elimination channel: see nr_td_cb0_wire.h. */
#define _GNU_SOURCE
#include "nr_td_cb0_wire.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "common/utils/LOG/log.h"
#include "nr_td_cb0_adapter.h"

/* ---- configuration (read once) ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* scheduler, per-context table, configuration */
static bool g_cfg_done;
static int g_threads = 8, g_rank_max = 4;
static bool g_tb_cpu = true, g_llr_scale;
static nr_td_cb0_sched_t g_sched;
static bool g_dec_registered;
static bool g_freeze; /* test hook: the cost estimates stay at their initial values */
static bool g_unlimited; /* test hook: the token bucket admits every grant */

static double env_d(const char *n, double dflt)
{
  const char *e = getenv(n);
  return (e && *e) ? atof(e) : dflt;
}

static void cfg_locked(void)
{
  if (g_cfg_done)
    return;
  g_cfg_done = true;
  g_threads = (int)env_d("ISAC_TD_CB0_THREADS", 8);
  if (g_threads < 1)
    g_threads = 1;
  g_rank_max = (int)env_d("ISAC_TD_CB0_RANK_MAX", 4);
  g_tb_cpu = env_d("ISAC_TD_TB_CPU_WHILE_ACQ", 1) != 0;
  const char *ls = getenv("ISAC_LLR_SCALE");
  g_llr_scale = ls != NULL && *ls && atof(ls) != 0.0 && atof(ls) != 1.0;
  const long nc = sysconf(_SC_NPROCESSORS_ONLN);
  nr_td_cb0_sched_init(&g_sched, env_d("ISAC_TD_CB0_BUDGET_US", NR_TD_CB0_BUDGET_US_DEFAULT), env_d("ISAC_TD_CB0_CPU_PCT", 30), nc > 0 ? (int)nc : 1, 8);
  LOG_A(PHY,
        "SENSING: TD_CB0 wiring on: budget=%.0f us/grant cpu_pct=%.0f ncpu=%d threads=%d B0=%d rank_max=%d tb_cpu_while_acq=%d "
        "backend=%d engine_wired=%d\n",
        g_sched.budget_us, g_sched.cpu_pct, g_sched.ncpu, g_threads, nr_td_cb0_sched_B(&g_sched), g_rank_max, g_tb_cpu,
        nr_td_cb0_backend_mode(), nr_td_cb0a_engine_wired() ? 1 : 0);
}

/* ---- counters ---- */
static _Atomic uint64_t s_grants, s_batches, s_adm, s_items, s_inadm[NR_TD_CB0_R_COUNT], s_budget, s_not_testable, s_cpu_us,
    s_be_cpu, s_be_gpu;

/* ---- per-context table (CONVERGED suffix) ---- */
#define CTX_TAB 512
typedef struct {
  uint64_t seed;
  uint32_t grants, adm, items, elim, alarms;
} ctx_row_t;
static ctx_row_t g_ctx[CTX_TAB];
static ctx_row_t *ctx_row_locked(uint64_t seed)
{
  const uint32_t h = (uint32_t)(seed % CTX_TAB);
  for (int p = 0; p < 8; p++) {
    ctx_row_t *r = &g_ctx[(h + p) % CTX_TAB];
    if (r->seed == seed)
      return r;
    if (r->seed == 0) {
      memset(r, 0, sizeof(*r));
      r->seed = seed;
      return r;
    }
  }
  ctx_row_t *r = &g_ctx[h]; /* full probe window: overwrite (diagnostics only) */
  memset(r, 0, sizeof(*r));
  r->seed = seed;
  return r;
}

/* ---- per-thread plan (one grant at a time per consumer thread) ---- */
typedef struct {
  bool active, batch, ran, done;
  uint32_t pre_reasons;
  uint64_t seed;
  int64_t abs_slot;
  uint8_t job_k0;
  int n_hyp_snapshot, n_active;
  int tb_hyp, tb_forced; /* ticket hypothesis; forced >= 0 when it is active (must have a verdict) */
  int n_sel;
  int idx[NR_TD_CB0_PLAN_MAX];
  nr_pdsch_cfg_hypothesis_t hyp[NR_TD_CB0_PLAN_MAX];
  double planned_us;
  /* run */
  uint32_t gw_flags;
  bool member_error, backend_failed;
  nr_td_cb0_exec_t ex;
  int n_res, tb_pos;
  int ridx[NR_TD_CB0_PLAN_MAX];
  nr_pdsch_cfg_hypothesis_t rhyp[NR_TD_CB0_PLAN_MAX];
  bool rpass[NR_TD_CB0_PLAN_MAX];
  nr_td_cb0_item_t items[NR_TD_CB0_PLAN_MAX];
  nr_td_cb0_result_t res[NR_TD_CB0_PLAN_MAX];
  /* active-set scratch */
  int *aidx;
  nr_pdsch_cfg_hypothesis_t *ahyp;
  uint64_t *akey, *agkey, *asort;
  int *asel;
  int n_sigs_est;
} plan_t;
static __thread plan_t *t_plan;
static plan_t *plan(void)
{
  if (t_plan == NULL) {
    t_plan = calloc(1, sizeof(*t_plan));
    if (t_plan) {
      t_plan->aidx = malloc(sizeof(int) * NR_TD_CB0A_MAX_ACTIVE);
      t_plan->ahyp = malloc(sizeof(nr_pdsch_cfg_hypothesis_t) * NR_TD_CB0A_MAX_ACTIVE);
      t_plan->akey = malloc(sizeof(uint64_t) * NR_TD_CB0A_MAX_ACTIVE);
      t_plan->agkey = malloc(sizeof(uint64_t) * NR_TD_CB0A_MAX_ACTIVE);
      t_plan->asort = malloc(sizeof(uint64_t) * NR_TD_CB0A_MAX_ACTIVE);
      t_plan->asel = malloc(sizeof(int) * NR_TD_CB0A_MAX_ACTIVE);
      if (!t_plan->aidx || !t_plan->ahyp || !t_plan->akey || !t_plan->agkey || !t_plan->asort || !t_plan->asel) {
        free(t_plan->aidx);
        free(t_plan->ahyp);
        free(t_plan->akey);
        free(t_plan->agkey);
        free(t_plan->asort);
        free(t_plan->asel);
        free(t_plan);
        t_plan = NULL;
      }
    }
  }
  return t_plan;
}

static int cmp_u64(const void *a, const void *b)
{
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}
static int count_distinct(uint64_t *v, int n)
{
  if (n <= 0)
    return 0;
  qsort(v, (size_t)n, sizeof(*v), cmp_u64);
  int d = 1;
  for (int i = 1; i < n; i++)
    d += v[i] != v[i - 1];
  return d;
}

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool nr_td_cb0_wire_pre(const nr_pdsch_sweep_ticket_t *t, nr_td_grantwork_t *gw, const nr_td_cb0_job_t *job, bool *tb_cpu)
{
  if (tb_cpu)
    *tb_cpu = false;
  if (!nr_pdsch_config_sweep_cb0_elim_env()) /* flag off: nothing at all */
    return false;
  plan_t *p = plan();
  if (p == NULL || t == NULL || job == NULL)
    return false;
  p->active = p->batch = p->ran = p->done = false;
  if (t->generation == 0 || t->settled || job->layout_probe)
    return false;
  pthread_mutex_lock(&g_lock);
  cfg_locked();
  const bool force_tb_cpu = g_tb_cpu;
  pthread_mutex_unlock(&g_lock);
  nr_td_cb0a_set_t set = {.idx = p->aidx, .hyp = p->ahyp};
  if (!nr_td_cb0a_active_set(t, &set) || set.winner >= 0)
    return false; /* stale ticket / settled since the selection: not an acquiring grant any more */
  p->active = true;
  p->pre_reasons = 0;
  p->seed = nr_td_cb0_ctx_seed(t->configuration, t->rnti, t->tda_index);
  p->abs_slot = job->abs_slot;
  p->job_k0 = job->job_k0;
  p->n_hyp_snapshot = set.n_hyp;
  p->n_active = set.n_active;
  p->tb_hyp = t->hypothesis;
  p->tb_forced = -1;
  p->n_sel = 0;
  p->planned_us = 0;
  atomic_fetch_add(&s_grants, 1);
  if (tb_cpu)
    *tb_cpu = force_tb_cpu; /* decoder dominance: TB of an acquiring context on the CPU decoder */
  if (job->gpu_job)
    p->pre_reasons |= 1u << NR_TD_CB0_R_GPU_LLR;
  if (!job->gw_on) {
    static _Atomic int s_warned;
    if (atomic_exchange(&s_warned, 1) == 0)
      LOG_W(PHY, "SENSING: TD_CB0 ISAC_TD_CB0_ELIM=1 needs ISAC_TD_GRANTWORK=1: CB0 elimination refused (TB-only)\n");
    p->pre_reasons |= 1u << NR_TD_CB0_R_NO_GRANTWORK;
  } else if (gw == NULL && !job->gpu_job) {
    p->pre_reasons |= 1u << NR_TD_CB0_R_MEMBER_STALE; /* the GrantWork could not be created */
  }
  if (g_llr_scale)
    p->pre_reasons |= 1u << NR_TD_CB0_R_LLR_SCALE;
  if (p->pre_reasons)
    return true; /* known before decoding: no batch (the CPU is not spent on an inadmissible grant) */
  /* Hypothesis set: fixed here, before any decode of this grant (nr_td_cb0_sched.h, two-level hash subset). */
  for (int k = 0; k < set.n_active; k++) {
    p->akey[k] = nr_td_cb0_hyp_key(&set.hyp[k]);
    p->agkey[k] = nr_td_cb0_geo_key(&set.hyp[k]);
    p->asort[k] = p->agkey[k];
    if (set.idx[k] == t->hypothesis)
      p->tb_forced = k;
  }
  const int n_geo = count_distinct(p->asort, set.n_active);
  nr_td_cb0_sizes_t z;
  pthread_mutex_lock(&g_lock);
  nr_td_cb0_sched_sizes(&g_sched, set.n_active, n_geo, &z);
  pthread_mutex_unlock(&g_lock);
  const int ns = nr_td_cb0_subset_select(p->seed, p->abs_slot, p->agkey, p->akey, set.n_active, z.m1, z.m2, p->tb_forced,
                                         p->asel, NR_TD_CB0A_MAX_ACTIVE);
  const uint64_t main_sig = p->tb_forced >= 0 ? nr_td_cb0_mix64(p->agkey[p->tb_forced] ^ set.hyp[p->tb_forced].mcs_table) : 0;
  int nsig = 0;
  for (int s = 0; s < ns && p->n_sel < NR_TD_CB0_PLAN_MAX; s++) {
    const int k = p->asel[s];
    if (set.hyp[k].k0 != job->job_k0) { /* another PDSCH slot: this grant's GrantWork cannot test it */
      atomic_fetch_add(&s_not_testable, 1);
      continue;
    }
    p->idx[p->n_sel] = set.idx[k];
    p->hyp[p->n_sel] = set.hyp[k];
    p->n_sel++;
    const uint64_t sig = nr_td_cb0_mix64(p->agkey[k] ^ set.hyp[k].mcs_table); /* upper bound of the GrantWork signatures */
    if (sig != main_sig)
      p->asort[nsig++] = sig;
  }
  if (p->n_sel == 0)
    return true;
  p->n_sigs_est = count_distinct(p->asort, nsig);
  if (p->n_sigs_est > NR_TD_GW_MAX_SIG - 1) { /* would not fit in one GrantWork: skip whole, before decoding */
    p->pre_reasons |= 1u << NR_TD_CB0_R_MEMBER_STALE;
    return true;
  }
  pthread_mutex_lock(&g_lock);
  if (g_unlimited)
    g_sched.tokens_us = 1e18;
  const bool ok = nr_td_cb0_sched_admit(&g_sched, now_ns(), p->n_sel, p->n_sigs_est, &p->planned_us);
  pthread_mutex_unlock(&g_lock);
  if (!ok) {
    p->pre_reasons |= 1u << NR_TD_CB0_R_BUDGET;
    atomic_fetch_add(&s_budget, 1);
    return true;
  }
  p->batch = true;
  return true;
}

void nr_td_cb0_wire_run(nr_td_grantwork_t *gw)
{
  plan_t *p = t_plan;
  if (p == NULL || !p->active || !p->batch || p->ran)
    return;
  p->ran = true;
  p->gw_flags = 0;
  p->member_error = p->backend_failed = false;
  p->n_res = 0;
  p->tb_pos = -1;
  memset(&p->ex, 0, sizeof(p->ex));
  pthread_mutex_lock(&g_lock);
  if (!g_dec_registered) {
    g_dec_registered = true;
    void *dec = nr_pdsch_passive_cb0_cpu_ldpc();
    if (dec != NULL)
      nr_td_cb0_set_ldpc_decoder(dec);
    nr_td_cb0_set_threads(g_threads);
  }
  pthread_mutex_unlock(&g_lock);
  if (gw == NULL) {
    p->member_error = true;
    return;
  }
  nr_td_grantwork_retain(gw); /* GrantWork rule: held until the synchronous batch returned */
  const int ent0 = nr_td_grantwork_n_entries(gw);
  const uint64_t tb0 = now_ns();
  int n = 0;
  int pos[NR_TD_CB0_PLAN_MAX];
  for (int s = 0; s < p->n_sel; s++) {
    uint32_t fl = 0;
    const int rc = nr_pdsch_passive_gw_cb0_item(gw, &p->hyp[s], &p->items[n], &fl);
    p->gw_flags |= fl;
    if (rc == NR_TD_GW_OK) {
      pos[n++] = s;
    } else if (rc == NR_TD_GW_E_K0 || rc == NR_TD_GW_E_ARG || rc == NR_TD_GW_E_FAILED || rc == NR_TD_GW_E_RM) {
      atomic_fetch_add(&s_not_testable, 1); /* parameter-determined: the hypothesis cannot be computed on this grant */
    } else {
      p->member_error = true; /* STALE / FULL / NOTOWNER / NOMEM: the grant's shared work is incomplete */
    }
  }
  const uint64_t build_ns = now_ns() - tb0;
  const int new_sigs = nr_td_grantwork_n_entries(gw) - ent0;
  atomic_fetch_add(&s_cpu_us, build_ns / 1000);
  if (n > 0 && !p->member_error) {
    nr_td_cb0_exec(p->items, n, p->res, &p->ex);
    atomic_fetch_add(&s_batches, 1);
    atomic_fetch_add(p->ex.backend == NR_TD_CB0_BE_GPU ? &s_be_gpu : &s_be_cpu, 1);
    if (p->ex.failed || p->ex.mixed)
      p->backend_failed = true;
    for (int i = 0; i < n; i++) {
      const nr_td_cb0_result_t *r = &p->res[i];
      if (r->pass == -1) {
        if (r->err == NR_TD_CB0_ERR_DECODER || r->err == NR_TD_CB0_ERR_GPU)
          p->backend_failed = true;
        else
          atomic_fetch_add(&s_not_testable, 1); /* ARG / SEG / E / RM: parameter-determined */
        continue;
      }
      const int s = pos[i];
      if (p->idx[s] == p->tb_hyp)
        p->tb_pos = p->n_res;
      p->ridx[p->n_res] = p->idx[s];
      p->rhyp[p->n_res] = p->hyp[s];
      p->rpass[p->n_res] = r->pass == 1;
      p->n_res++;
    }
    const int thr = p->ex.backend == NR_TD_CB0_BE_CPU ? p->ex.threads : 0;
    int tu = thr < n ? thr : n;
    if (tu < 1)
      tu = 1;
    atomic_fetch_add(&s_cpu_us, (uint64_t)((double)p->ex.wall_ns / 1000.0 * tu));
    pthread_mutex_lock(&g_lock);
    nr_td_cb0_sched_account(&g_sched, p->planned_us, p->ex.wall_ns, thr, n, g_freeze ? 0 : p->ex.sum_iters, build_ns,
                            g_freeze ? 0 : new_sigs);
    const nr_td_cb0_sched_t sc = g_sched;
    pthread_mutex_unlock(&g_lock);
    const uint64_t nb = atomic_load(&s_batches);
    if (nb == 1 || nb % 500 == 0) {
      nr_td_cb0_sizes_t z;
      nr_td_cb0_sched_sizes(&sc, p->n_active, 1, &z);
      LOG_A(PHY,
            "SENSING: TD_CB0_SCHED batches=%lu us_per_iter=%.1f item_us=%.0f sig_us=%.0f tokens_us=%.0f b_items=%d g_target=%d "
            "last: n=%d new_sigs=%d build_us=%.0f batch_wall_us=%.0f threads=%d backend=%u\n",
            (unsigned long)nb, sc.us_per_iter, nr_td_cb0_sched_item_us(&sc), sc.sig_us, sc.tokens_us, z.b_items, z.g_target, n,
            new_sigs, build_ns / 1000.0, p->ex.wall_ns / 1000.0, thr, p->ex.backend);
    }
  } else {
    pthread_mutex_lock(&g_lock);
    nr_td_cb0_sched_account(&g_sched, p->planned_us, 0, 0, 0, 0, build_ns, g_freeze ? 0 : new_sigs); /* refund, charge the build */
    pthread_mutex_unlock(&g_lock);
  }
  nr_td_grantwork_release(gw);
}

static void count_reasons(uint32_t r)
{
  for (int b = 0; b < NR_TD_CB0_R_COUNT; b++)
    if (r & (1u << b))
      atomic_fetch_add(&s_inadm[b], 1);
}

void nr_td_cb0_wire_feed(const nr_pdsch_sweep_ticket_t *t, const nr_td_cb0_tb_t *tb)
{
  plan_t *p = t_plan;
  if (p == NULL || !p->active || p->done || t == NULL || tb == NULL)
    return;
  p->done = true;
  uint32_t r = p->pre_reasons;
  ctx_row_t row_add = {0};
  row_add.grants = 1;
  if (!p->batch) { /* no batch: budget skip, pre-decode reason, or nothing testable on this grant */
    count_reasons(r);
    if (tb->tb_fed)
      nr_td_cb0a_note_tb_decoder(t, tb->tb_decoder);
  } else {
    nr_td_cb0_adm_in_t in = {.tb_path_fed = tb->tb_fed,
                             .iq_ok_after = tb->iq_ok_after,
                             .gw_flags = p->gw_flags,
                             .rv = tb->rv,
                             .llr_scale = g_llr_scale,
                             .gpu_llr = false,
                             .member_error = p->member_error,
                             .backend_failed = p->backend_failed || !p->ran,
                             .nl = tb->nl,
                             .rank_max = g_rank_max,
                             .cb0_decoder = p->ex.decoder,
                             .tb_decoder = tb->tb_decoder,
                             .contract = p->tb_forced >= 0 && p->tb_pos < 0};
    r |= nr_td_cb0_admissibility(&in);
    if (p->n_res == 0 && r == 0)
      r |= 1u << NR_TD_CB0_R_CONTRACT; /* nothing decoded: nothing to credit (scheduled hypothesis not testable) */
    nr_td_cb0a_grant_t g = {.idx = p->ridx,
                            .hyp = p->rhyp,
                            .pass = p->rpass,
                            .n = p->n_res,
                            .n_hyp_snapshot = p->n_hyp_snapshot,
                            .cb0_decoder = p->ex.decoder,
                            .tb_hyp = tb->tb_fed ? p->tb_hyp : -1,
                            .tb_pass = tb->tb_pass,
                            .tb_decoder = tb->tb_decoder,
                            .tb_pos = p->tb_pos,
                            .inadmissible = r};
    nr_td_cb0a_feed_out_t o;
    nr_td_cb0a_feed(t, &g, &o);
    if (o.reindexed)
      r |= 1u << NR_TD_CB0_R_REINDEXED;
    count_reasons(r);
    if (r == 0) {
      atomic_fetch_add(&s_adm, 1);
      row_add.adm = 1;
    }
    atomic_fetch_add(&s_items, (uint64_t)p->n_res);
    row_add.items = (uint32_t)p->n_res;
    row_add.elim = (uint32_t)o.elim_delta;
    if (o.premise_alarm) {
      row_add.alarms = 1;
      const nr_pdsch_cfg_hypothesis_t *h = &p->rhyp[p->tb_pos];
      LOG_A(PHY,
            "SENSING: TD_CB0_PREMISE_ALARM rnti=0x%x tda=%u slot=%ld hyp=%d S=%u L=%u k0=%u mask=0x%x table=%u tb=PASS cb0=FAIL "
            "cb0_dec=%u tb_dec=%u nl=%d\n",
            t->rnti, t->tda_index, (long)p->abs_slot, p->tb_hyp, h->tda_start, h->tda_length, h->k0, h->dmrs_mask, h->mcs_table,
            p->ex.decoder, tb->tb_decoder, tb->nl);
    }
  }
  pthread_mutex_lock(&g_lock);
  ctx_row_t *row = ctx_row_locked(p->seed);
  row->grants += row_add.grants;
  row->adm += row_add.adm;
  row->items += row_add.items;
  row->elim += row_add.elim;
  row->alarms += row_add.alarms;
  pthread_mutex_unlock(&g_lock);
}

const char *nr_td_cb0_wire_converged_suffix(const nr_pdsch_sweep_ticket_t *t)
{
  static __thread char buf[160];
  buf[0] = 0;
  if (!nr_pdsch_config_sweep_cb0_elim_env() || t == NULL)
    return buf;
  const uint64_t seed = nr_td_cb0_ctx_seed(t->configuration, t->rnti, t->tda_index);
  pthread_mutex_lock(&g_lock);
  const ctx_row_t r = *ctx_row_locked(seed);
  pthread_mutex_unlock(&g_lock);
  snprintf(buf, sizeof(buf), " cb0_grants=%u cb0_adm=%u cb0_items=%u cb0_elim=%u cb0_alarms=%u", r.grants, r.adm, r.items, r.elim,
           r.alarms);
  return buf;
}

void nr_td_cb0_wire_stats(nr_td_cb0_wire_stats_t *s)
{
  if (s == NULL)
    return;
  memset(s, 0, sizeof(*s));
  s->grants = atomic_load(&s_grants);
  s->batches = atomic_load(&s_batches);
  s->admissible = atomic_load(&s_adm);
  s->items = atomic_load(&s_items);
  for (int b = 0; b < NR_TD_CB0_R_COUNT; b++)
    s->inadmissible[b] = atomic_load(&s_inadm[b]);
  s->budget_skips = atomic_load(&s_budget);
  s->not_testable = atomic_load(&s_not_testable);
  s->cpu_us = atomic_load(&s_cpu_us);
  s->backend_cpu = atomic_load(&s_be_cpu);
  s->backend_gpu = atomic_load(&s_be_gpu);
  nr_td_cb0a_stats(&s->premise_alarms, &s->eliminations);
}

void nr_td_cb0_wire_reset(void)
{
  pthread_mutex_lock(&g_lock);
  g_cfg_done = false;
  g_dec_registered = false;
  memset(g_ctx, 0, sizeof(g_ctx));
  pthread_mutex_unlock(&g_lock);
  atomic_store(&s_grants, 0);
  atomic_store(&s_batches, 0);
  atomic_store(&s_adm, 0);
  atomic_store(&s_items, 0);
  for (int b = 0; b < NR_TD_CB0_R_COUNT; b++)
    atomic_store(&s_inadm[b], 0);
  atomic_store(&s_budget, 0);
  atomic_store(&s_not_testable, 0);
  atomic_store(&s_cpu_us, 0);
  atomic_store(&s_be_cpu, 0);
  atomic_store(&s_be_gpu, 0);
}

void nr_td_cb0_wire_test_freeze(bool on)
{
  pthread_mutex_lock(&g_lock);
  g_freeze = on;
  pthread_mutex_unlock(&g_lock);
}

int nr_td_cb0_wire_test_last_set(int *idx, int max)
{
  const plan_t *p = t_plan;
  if (p == NULL || !p->active)
    return -1;
  int n = 0;
  for (; n < p->n_sel && n < max; n++)
    idx[n] = p->idx[n];
  return n;
}

uint32_t nr_td_cb0_wire_test_last_reasons(void)
{
  const plan_t *p = t_plan;
  return p ? p->pre_reasons : 0;
}

void nr_td_cb0_wire_test_unlimited(bool on)
{
  pthread_mutex_lock(&g_lock);
  g_unlimited = on;
  pthread_mutex_unlock(&g_lock);
}

void nr_td_cb0_wire_test_set_tokens(double us)
{
  pthread_mutex_lock(&g_lock);
  cfg_locked();
  g_sched.tokens_us = us > g_sched.cap_us ? g_sched.cap_us : us;
  pthread_mutex_unlock(&g_lock);
}
