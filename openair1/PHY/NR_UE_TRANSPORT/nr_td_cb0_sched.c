/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-grant CB0 scheduler: see nr_td_cb0_sched.h. Pure. */
#include "nr_td_cb0_sched.h"
#include <math.h>
#include <string.h>
#include "nr_td_grantwork.h"

const char *const nr_td_cb0_reason_name[NR_TD_CB0_R_COUNT] = {
    "not_new_rv0", "gated",   "iq_stale", "lbrm",     "rv_retry", "prg_ptrs",     "member_stale", "llr_scale",
    "gpu_llr",     "ldpc_error", "rank",  "decoder",  "contract", "budget",       "no_grantwork", "reindexed"};

uint32_t nr_td_cb0_admissibility(const nr_td_cb0_adm_in_t *in)
{
  uint32_t r = 0;
  if ((in->gw_flags & NR_TD_GW_F_HARQ) || in->rv != 0)
    r |= 1u << NR_TD_CB0_R_NOT_NEW_RV0;
  if (!in->tb_path_fed)
    r |= 1u << NR_TD_CB0_R_GATED;
  if (!in->iq_ok_after)
    r |= 1u << NR_TD_CB0_R_IQ_STALE;
  if (in->gw_flags & NR_TD_GW_F_LBRM)
    r |= 1u << NR_TD_CB0_R_LBRM;
  if (in->gw_flags & NR_TD_GW_F_RV_RETRY)
    r |= 1u << NR_TD_CB0_R_RV_RETRY;
  if (in->gw_flags & NR_TD_GW_F_ARM)
    r |= 1u << NR_TD_CB0_R_PRG_PTRS;
  if ((in->gw_flags & (NR_TD_GW_F_STALE | NR_TD_GW_F_FULL)) || in->member_error)
    r |= 1u << NR_TD_CB0_R_MEMBER_STALE;
  if (in->llr_scale)
    r |= 1u << NR_TD_CB0_R_LLR_SCALE;
  if (in->gpu_llr)
    r |= 1u << NR_TD_CB0_R_GPU_LLR;
  if (in->backend_failed)
    r |= 1u << NR_TD_CB0_R_LDPC_ERROR;
  if (in->nl < 1 || in->nl > in->rank_max)
    r |= 1u << NR_TD_CB0_R_RANK;
  /* dominance: sensitivity CPU (1) < CUDA (2); an unknown decoder never qualifies, on either side */
  if (in->cb0_decoder == 0 || in->tb_decoder == 0 || in->cb0_decoder < in->tb_decoder)
    r |= 1u << NR_TD_CB0_R_DECODER;
  if (in->contract)
    r |= 1u << NR_TD_CB0_R_CONTRACT;
  return r;
}

uint64_t nr_td_cb0_mix64(uint64_t x)
{ /* splitmix64 finaliser */
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

uint64_t nr_td_cb0_ctx_seed(uint64_t configuration, uint16_t rnti, uint8_t tda)
{
  return nr_td_cb0_mix64(nr_td_cb0_mix64(configuration) ^ ((uint64_t)rnti << 8) ^ (uint64_t)tda ^ 0xCB0CB0CB0ull);
}

uint64_t nr_td_cb0_hyp_key(const nr_pdsch_cfg_hypothesis_t *h)
{ /* field by field: struct padding never enters */
  uint64_t v = (uint64_t)h->tda_start | (uint64_t)h->tda_length << 8 | (uint64_t)h->k0 << 16 | (uint64_t)h->dmrs_add_pos << 24
               | (uint64_t)h->dmrs_max_len << 32 | (uint64_t)h->mcs_table << 40 | (uint64_t)h->mapping_type << 48;
  return nr_td_cb0_mix64(v ^ nr_td_cb0_mix64((uint64_t)h->dmrs_mask + 0x51ull));
}

uint64_t nr_td_cb0_geo_key(const nr_pdsch_cfg_hypothesis_t *h)
{
  uint64_t v = (uint64_t)h->tda_start | (uint64_t)h->tda_length << 8 | (uint64_t)h->k0 << 16 | (uint64_t)h->dmrs_mask << 24;
  if (h->dmrs_mask == 0) /* legacy mask-less entry: the IEs make the mask */
    v |= (uint64_t)h->mapping_type << 40 | (uint64_t)h->dmrs_add_pos << 48 | (uint64_t)h->dmrs_max_len << 56;
  return nr_td_cb0_mix64(v ^ 0x6E0ull);
}

uint32_t nr_td_cb0_subset_m(int n, int B)
{
  if (n <= 0 || B <= 0)
    return 1;
  return (uint32_t)((n + B - 1) / B);
}

bool nr_td_cb0_subset_hit(uint64_t seed, int64_t abs_slot, uint64_t key, uint32_t m)
{
  if (m <= 1)
    return true;
  const uint64_t x = nr_td_cb0_mix64(seed ^ nr_td_cb0_mix64((uint64_t)abs_slot ^ 0xA5A5ull) ^ nr_td_cb0_mix64(key));
  return (x % m) == 0;
}

bool nr_td_cb0_subset_hit2(uint64_t seed, int64_t abs_slot, uint64_t gkey, uint32_t m1, uint64_t key, uint32_t m2)
{
  return nr_td_cb0_subset_hit(seed, abs_slot, gkey, m1) && nr_td_cb0_subset_hit(nr_td_cb0_mix64(seed ^ 0x5A17ull), abs_slot, key, m2);
}

int nr_td_cb0_subset_select(uint64_t seed, int64_t abs_slot, const uint64_t *gkeys, const uint64_t *keys, int n, uint32_t m1,
                            uint32_t m2, int forced, int *sel, int max_sel)
{
  int c = 0;
  if (max_sel <= 0)
    return 0;
  if (forced >= 0 && forced < n)
    sel[c++] = forced; /* kept first, re-sorted below */
  for (int k = 0; k < n && c < max_sel; k++) {
    if (k == forced)
      continue;
    const bool hit = gkeys ? nr_td_cb0_subset_hit2(seed, abs_slot, gkeys[k], m1, keys[k], m2)
                           : nr_td_cb0_subset_hit(seed, abs_slot, keys[k], m2);
    if (hit)
      sel[c++] = k;
  }
  for (int i = 1; i < c; i++) { /* ascending positions (insertion sort; c is small) */
    const int v = sel[i];
    int j = i - 1;
    while (j >= 0 && sel[j] > v) {
      sel[j + 1] = sel[j];
      j--;
    }
    sel[j + 1] = v;
  }
  return c;
}

void nr_td_cb0_sched_init(nr_td_cb0_sched_t *s, double budget_us, double cpu_pct, int ncpu, int max_iter)
{
  memset(s, 0, sizeof(*s));
  s->budget_us = budget_us > 0 ? budget_us : NR_TD_CB0_BUDGET_US_DEFAULT;
  s->cpu_pct = cpu_pct > 0 ? (cpu_pct > 100 ? 100 : cpu_pct) : 30.0;
  s->ncpu = ncpu > 0 ? ncpu : 1;
  s->max_iter = max_iter > 0 ? max_iter : 8;
  s->us_per_iter = NR_TD_CB0_US_PER_ITER_INIT;
  s->sig_us = NR_TD_CB0_SIG_US_INIT;
  s->cap_us = 2.0 * s->budget_us;
  s->tokens_us = s->cap_us;
}

double nr_td_cb0_sched_item_us(const nr_td_cb0_sched_t *s)
{
  return s->us_per_iter * s->max_iter;
}

int nr_td_cb0_sched_B(const nr_td_cb0_sched_t *s)
{
  const double c = nr_td_cb0_sched_item_us(s);
  const double b = c > 0 ? floor(s->budget_us / c) : 1.0;
  return b < 1 ? 1 : (b > 1e6 ? 1000000 : (int)b);
}

static void refill(nr_td_cb0_sched_t *s, uint64_t now_ns)
{
  if (s->last_ns != 0 && now_ns > s->last_ns) {
    const double dt_us = (double)(now_ns - s->last_ns) / 1000.0;
    s->tokens_us += dt_us * s->cpu_pct / 100.0 * s->ncpu;
    if (s->tokens_us > s->cap_us)
      s->tokens_us = s->cap_us;
  }
  if (now_ns > s->last_ns || s->last_ns == 0)
    s->last_ns = now_ns;
}

void nr_td_cb0_sched_set_b(nr_td_cb0_sched_t *s, int b_target)
{
  s->b_target = b_target > 0 ? b_target : 0;
  const double full = s->b_target * nr_td_cb0_sched_item_us(s) + NR_TD_CB0_MAX_GEO * 3 * s->sig_us;
  s->cap_us = 2.0 * (s->b_target > 0 && full > s->budget_us ? full : s->budget_us);
  if (s->tokens_us > s->cap_us || s->updates == 0)
    s->tokens_us = s->cap_us;
}

void nr_td_cb0_sched_sizes(const nr_td_cb0_sched_t *s, int n_active, int n_geo, nr_td_cb0_sizes_t *z)
{
  if (s->b_target > 0) {
    const int ipg = n_geo > 0 ? (n_active + n_geo - 1) / n_geo : 1; /* hypotheses per geometry */
    int g = (s->b_target + (ipg > 0 ? ipg : 1) - 1) / (ipg > 0 ? ipg : 1);
    z->g_target = g < 1 ? 1 : (g > NR_TD_CB0_MAX_GEO ? NR_TD_CB0_MAX_GEO : g);
    z->b_items = s->b_target;
    z->m1 = nr_td_cb0_subset_m(n_geo, z->g_target);
    z->m2 = nr_td_cb0_subset_m((int)nr_td_cb0_subset_m(n_active, (int)z->m1), z->b_items);
    return;
  }
  const double per_geo = NR_TD_CB0_SIGS_PER_GEO * s->sig_us;
  double g = per_geo > 0 ? floor(0.4 * s->budget_us / per_geo) : NR_TD_CB0_MAX_GEO;
  z->g_target = g < 1 ? 1 : (g > NR_TD_CB0_MAX_GEO ? NR_TD_CB0_MAX_GEO : (int)g);
  const double c = nr_td_cb0_sched_item_us(s);
  const double rest = s->budget_us - z->g_target * per_geo;
  const double b = (c > 0 && rest > 0) ? floor(rest / c) : 1.0;
  z->b_items = b < 1 ? 1 : (b > 1e6 ? 1000000 : (int)b);
  z->m1 = nr_td_cb0_subset_m(n_geo, z->g_target);
  z->m2 = nr_td_cb0_subset_m((int)nr_td_cb0_subset_m(n_active, (int)z->m1), z->b_items);
}

bool nr_td_cb0_sched_admit(nr_td_cb0_sched_t *s, uint64_t now_ns, int n_items, int n_sigs, double *planned_us)
{
  refill(s, now_ns);
  const double need = (n_items > 0 ? n_items : 0) * nr_td_cb0_sched_item_us(s) + (n_sigs > 0 ? n_sigs : 0) * s->sig_us;
  if (planned_us)
    *planned_us = 0;
  if (need > s->tokens_us)
    return false;
  s->tokens_us -= need;
  if (planned_us)
    *planned_us = need;
  return true;
}

void nr_td_cb0_sched_account(nr_td_cb0_sched_t *s, double planned_us, uint64_t wall_ns, int threads, int n_items,
                             uint32_t sum_iters, uint64_t build_ns, int new_sigs)
{
  const double wall_us = (double)wall_ns / 1000.0, build_us = (double)build_ns / 1000.0;
  int t = threads < n_items ? threads : n_items;
  if (t < 1)
    t = 1;
  const double cpu_us = (wall_ns ? wall_us * t : 0.0) + build_us;
  s->tokens_us += planned_us - cpu_us;
  if (s->tokens_us > s->cap_us)
    s->tokens_us = s->cap_us;
  if (threads > 0 && sum_iters > 0 && wall_ns > 0) {
    double x = wall_us * t / (double)sum_iters;
    if (x < 1.0)
      x = 1.0;
    if (x > 5000.0)
      x = 5000.0;
    s->us_per_iter = s->updates == 0 ? x : 0.9 * s->us_per_iter + 0.1 * x;
    s->updates++;
  }
  if (new_sigs > 0 && build_ns > 0) {
    double y = build_us / new_sigs;
    if (y < 10.0)
      y = 10.0;
    if (y > 50000.0)
      y = 50000.0;
    s->sig_us = 0.9 * s->sig_us + 0.1 * y;
  }
}
