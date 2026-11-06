#define _GNU_SOURCE
/* Sensing-mode scheduler thread (plan Task S7). See nr_sense_sched_rt.h. */
#include "nr_sense_sched_rt.h"
#include "nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "common/utils/nr_cell.h"
#include "common/utils/nr_core_pool.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Other libraries: weak, so this file links (and its unit test runs) without them; a missing symbol means "no such resource / no such data". */
__attribute__((weak)) nr_pool_t *nr_pool_site(void);
__attribute__((weak)) void nr_pool_stats(nr_pool_t *, nr_pool_stats_t *out);
__attribute__((weak)) int nr_pool_size(const nr_pool_t *);
__attribute__((weak)) int nr_passive_metrics_pci_of(int cell);
__attribute__((weak)) int nr_gpu_pass_active(void);
__attribute__((weak)) int nr_isac_pdsch_data_aided_workers(void);

static const char *const k_res_name[NR_RES_N] = {"pool", "queue_dl", "queue_ul", "gpu", "sense_ring", "da"};
const char *nr_sense_res_name(int r) { return (unsigned)r < NR_RES_N ? k_res_name[r] : "none"; }

/* Process-wide: there is one scheduler thread (it serves every cell). The state is touched by that thread (or by a test calling step() alone). */
static nr_sense_sched_state_t g_st;
static nr_sense_unit_view_t g_views[NR_SENSE_CELLS][256];
static _Atomic uint64_t g_decisions, g_enables, g_disables, g_bad_cap, g_deferrals;
static _Atomic int g_binding;
static bool (*_Atomic g_sampler)(nr_sense_sched_sample_t *, uint64_t);
static pthread_t g_thread;
static _Atomic int g_running; /* 1 = the thread exists and has not been joined */
static _Atomic int g_stop;

/* Production sampler state (process-wide, scheduler thread only). */
static struct {
  uint64_t t_ns, rec_dropped[NR_SENSE_CELLS]; bool primed; /* decision to decision */
  uint64_t sub_t_ns, sub_hold, sub_unattr; double sub_max; bool sub_have; /* 500 ms sub-windows within the decision period */
  int no_sample;                                           /* consecutive periods without a pool sample (M3) */
} g_prev;
/* grant-level wideband path, per cell: cumulative counters at the previous decision and the recent mean cost per tap (scheduler thread only) */
static struct { uint64_t t_ns, offered, taps, cost_ns, records, ring_ns; float cost_per_tap, ring_per_rec, offered_hz; } g_wbgp[NR_SENSE_CELLS];
void nr_sense_sched_wbg_info(int cell, double *offered_hz, double *cost_ns)
{
  *offered_hz = *cost_ns = 0;
  if ((unsigned)cell < NR_SENSE_CELLS) { *offered_hz = g_wbgp[cell].offered_hz; *cost_ns = g_wbgp[cell].cost_per_tap; }
}

/* last decision per cell, for ISAC_METRICS (written by the scheduler thread, read by the metrics thread): prb(16) | in_gap(1) | gap_ms(47) in one atomic word, so a read is never torn */
static _Atomic uint64_t g_floor_log[NR_SENSE_CELLS];
int nr_sense_sched_floor_info(int cell, uint64_t *gap_p95_ms, bool *in_gap)
{
  if ((unsigned)cell >= NR_SENSE_CELLS) return 0;
  const uint64_t w = atomic_load_explicit(&g_floor_log[cell], memory_order_relaxed);
  *gap_p95_ms = w >> 17; *in_gap = (w >> 16) & 1;
  return (int)(w & 0xFFFF);
}


static void sched_log(const nr_sense_sched_in_t *in, const nr_sense_sched_sample_t *sm, const nr_sense_sched_out_t *out);

/* Protected work of all cells up to now (token-time ns of SCAN, RETRY(solver), DISC, HOUSE per nr_pool_stats_t.hold_ns_cell_cls, plus the cost-only calls without a unit row). */
static bool protected_total(uint64_t *hold, uint64_t *unattr)
{
  nr_pool_t *pool = (nr_pool_site && nr_pool_stats && nr_pool_size) ? nr_pool_site() : NULL;
  if (!pool) return false;
  static nr_pool_stats_t ps; /* ~16 KB: static, scheduler thread only */
  nr_pool_stats(pool, &ps);
  int n = nr_cell_count();
  if (n > NR_SENSE_CELLS) n = NR_SENSE_CELLS;
  uint64_t h = 0, u = 0;
  for (int k = 0; k < n; k++) {
    for (int c = 0; c < NR_POOL_NPRIO; c++)
      if (c == NR_POOL_SCAN || c == NR_POOL_RETRY || c == NR_POOL_DISC || c == NR_POOL_HOUSE) h += ps.hold_ns_cell_cls[k][c];
    u += nr_sense_unit_unattributed_ns(k);
  }
  *hold = h; *unattr = u;
  return true;
}

/* One 500 ms sub-window of the pool baseline: the rate (token-time ns per 50 ms bucket) over the time since the previous tick; the period keeps the MAXIMUM, the closest
 * thing to the spec's p99 over 50 ms buckets that the cumulative pool counters allow. */
static void sub_tick(uint64_t now_ns)
{
  uint64_t h, u;
  if (!protected_total(&h, &u)) return;
  if (g_prev.sub_t_ns && now_ns > g_prev.sub_t_ns) {
    const double dt = (double)(now_ns - g_prev.sub_t_ns);
    const double d = (double)(h >= g_prev.sub_hold ? h - g_prev.sub_hold : 0) + (double)(u >= g_prev.sub_unattr ? u - g_prev.sub_unattr : 0);
    const double rate = d / (dt / (double)SENSE_BUCKET_NS);
    if (!g_prev.sub_have || rate > g_prev.sub_max) g_prev.sub_max = rate;
    g_prev.sub_have = true;
  }
  g_prev.sub_t_ns = now_ns; g_prev.sub_hold = h; g_prev.sub_unattr = u;
}

/* POOL: cap = pool tokens x one bucket (ns of token time per 50 ms); base = the largest 500 ms sub-window rate of the protected classes in the period (see sub_tick).
 * QUEUE_DL/UL: cap = the late threshold (NR_UNIT_LATE_NS), base 0 = UNMEASURED: no queue-wide wait statistic is exposed (the PDSCH/PUSCH queues keep per-job BTIM stages and
 * cumulative counters only; the daBg wait histogram is the units' own queue); the per-unit wait p99 is the demand. GPU: cap = one bucket when the GPU pass is up, else 0 (absent);
 * base 0 = UNMEASURED: no pacer refusal or GPU busy counter is exposed (nr_gpu_pass_dl_cell_counters gives calls and host refusals per cell, not time). SENSE_RING (per cell):
 * cap = one bucket (the writer), base = the whole cap when records were lost on that cell's ring in the period. */
static bool sample_prod(nr_sense_sched_sample_t *o, uint64_t now_ns)
{
  memset(o, 0, sizeof *o);
  int n = nr_cell_count();
  if (n < 1) return false;
  if (n > NR_SENSE_CELLS) n = NR_SENSE_CELLS;
  o->n_cells = n;
  sub_tick(now_ns);
  nr_pool_t *pool = (nr_pool_site && nr_pool_stats && nr_pool_size) ? nr_pool_site() : NULL;
  if (!pool) {
    if (++g_prev.no_sample == 3) LOG_E(PHY, "SENSING: SENSE_SCHED has no pool sample (nr_pool_site unavailable): no unit can be enabled\n");
    return false;
  }
  g_prev.no_sample = 0;
  const int prev_cell = nr_cell_idx();
  int da_workers[NR_SENSE_CELLS] = {0};
  uint64_t dec_wait[NR_SENSE_CELLS] = {0};
  const bool primed = g_prev.primed && now_ns > g_prev.t_ns;
  for (int k = 0; k < n; k++) {
    o->pci[k] = nr_passive_metrics_pci_of ? nr_passive_metrics_pci_of(k) : -1;
    nr_cell_bind(k);
    nr_sense_stats_t st;
    nr_sense_stats(&st);
    da_workers[k] = nr_isac_pdsch_data_aided_workers ? nr_isac_pdsch_data_aided_workers() : 0;
    dec_wait[k] = nr_sense_unit_take_decode_wait_max(k);
    nr_cell_bind(prev_cell);
    if (primed) o->ring_drops[k] = st.rec_dropped >= g_prev.rec_dropped[k] ? st.rec_dropped - g_prev.rec_dropped[k] : 0;
    g_prev.rec_dropped[k] = st.rec_dropped;
  }
  g_prev.t_ns = now_ns; g_prev.primed = true;
  if (!primed || !g_prev.sub_have) return false; /* the first period only primes the counters */
  const int tokens = nr_pool_size(pool);
  o->res[NR_RES_POOL] = (nr_sense_res_t){.cap_ns = (float)((double)tokens * (double)SENSE_BUCKET_NS), .base_p99_ns = (float)g_prev.sub_max};
  g_prev.sub_have = false; g_prev.sub_max = 0;
  o->res[NR_RES_QUEUE_DL] = (nr_sense_res_t){.cap_ns = (float)NR_UNIT_LATE_NS, .base_p99_ns = 0.f};
  o->res[NR_RES_QUEUE_UL] = (nr_sense_res_t){.cap_ns = (float)NR_UNIT_LATE_NS, .base_p99_ns = 0.f};
  o->res[NR_RES_GPU] = (nr_sense_res_t){.cap_ns = (nr_gpu_pass_active && nr_gpu_pass_active()) ? (float)SENSE_BUCKET_NS : 0.f, .base_p99_ns = 0.f};
  o->res[NR_RES_SENSE_RING] = (nr_sense_res_t){.cap_ns = (float)SENSE_BUCKET_NS, .base_p99_ns = 0.f};
  o->res[NR_RES_DA] = (nr_sense_res_t){.cap_ns = (float)SENSE_BUCKET_NS, .base_p99_ns = 0.f};
  for (int k = 0; k < n; k++) { /* the per-cell resources (queues, ring, DA) of each cell */
    o->cell_res_set[k] = true;
    memcpy(o->cell_res[k], o->res, sizeof o->res);
    o->cell_res[k][NR_RES_QUEUE_DL].base_p99_ns = (float)dec_wait[k]; /* measured: the longest decode-queue wait of the period (a max, not a p99: conservative) */
    o->cell_res[k][NR_RES_DA].cap_ns = (float)((da_workers[k] > 1 ? da_workers[k] : 1) * (double)SENSE_BUCKET_NS); /* workers x one bucket (the headroom is applied by the decision) */
    if (o->ring_drops[k]) o->cell_res[k][NR_RES_SENSE_RING].base_p99_ns = (float)SENSE_BUCKET_NS;
  }
  return true;
}

void nr_sense_sched_set_sampler(bool (*fn)(nr_sense_sched_sample_t *, uint64_t)) { atomic_store(&g_sampler, fn); }

void nr_sense_sched_reset(void)
{
  memset(&g_st, 0, sizeof g_st);
  memset(&g_prev, 0, sizeof g_prev);
  memset(g_wbgp, 0, sizeof g_wbgp);
  nr_sense_single_set(false);
  nr_sense_cell_floor_reset();
  for (int c = 0; c < NR_SENSE_CELLS; c++) atomic_store_explicit(&g_floor_log[c], 0, memory_order_relaxed);
  (void)nr_sense_sched_take_cell_resets();
  atomic_store(&g_decisions, 0); atomic_store(&g_enables, 0); atomic_store(&g_disables, 0); atomic_store(&g_bad_cap, 0); atomic_store(&g_deferrals, 0);
  atomic_store(&g_binding, 0);
  atomic_store(&g_sampler, NULL);
}

void nr_sense_sched_stats(nr_sense_sched_stats_t *out)
{
  out->decisions = atomic_load(&g_decisions); out->enables = atomic_load(&g_enables); out->disables = atomic_load(&g_disables);
  out->bad_cap = atomic_load(&g_bad_cap); out->deferrals = atomic_load(&g_deferrals); out->binding = atomic_load(&g_binding);
}

/* Apply st->enabled to the units where it differs from what the units say. The scheduler is the only writer of the switch. */
static void apply(const nr_sense_sched_in_t *in)
{
  for (int c = 0; c < in->n_cells; c++)
    for (int i = 0; i < in->n_units[c]; i++) {
      const nr_sense_unit_view_t *v = &in->units[c][i];
      int sl = v->rnti == 0 ? 0 : -1;
      for (int j = 1; sl < 0 && j < NR_SENSE_SLOTS; j++)
        if (g_st.rnti[c][j] == v->rnti) sl = j;
      if (sl < 0) continue;
      const bool want = g_st.enabled[c][sl][v->src];
      if (want != v->enabled) nr_sense_unit_set_enabled(c, v->rnti, v->src, want);
    }
}

void nr_sense_sched_step(uint64_t now_ns)
{
  const uint32_t rm = nr_sense_sched_take_cell_resets(); /* a cell that changed starts its T_cell over (a decision already in flight may publish the old level once) */
  for (int c = 0; c < NR_SENSE_CELLS; c++)
    if (rm & (1u << c)) { g_st.fl_idx[c] = g_st.fl_pend[c] = g_st.fl_cnt[c] = g_st.fl_warm[c] = 0; memset(g_st.pr_sum[c], 0, sizeof g_st.pr_sum[c]); memset(g_st.pr_w[c], 0, sizeof g_st.pr_w[c]); memset(&g_wbgp[c], 0, sizeof g_wbgp[c]); nr_sense_cell_floor_publish(c, 0); atomic_store_explicit(&g_floor_log[c], 0, memory_order_relaxed); }
  nr_sense_sched_sample_t sm;
  bool (*smp)(nr_sense_sched_sample_t *, uint64_t) = atomic_load(&g_sampler);
  if (!(smp ? smp(&sm, now_ns) : sample_prod(&sm, now_ns))) {
    atomic_fetch_add(&g_deferrals, 1);
    return;
  }
  static nr_sense_sched_in_t in; /* thread only */
  memset(&in, 0, sizeof in);
  in.n_cells = sm.n_cells > NR_SENSE_CELLS ? NR_SENSE_CELLS : sm.n_cells;
  for (int c = 0; c < in.n_cells; c++) {
    in.pci[c] = sm.pci[c];
    in.n_units[c] = nr_sense_unit_snapshot_at(c, now_ns, g_views[c], 256);
    in.units[c] = g_views[c];
  }
  memcpy(in.res, sm.res, sizeof in.res);
  memcpy(in.cell_res, sm.cell_res, sizeof in.cell_res);
  memcpy(in.cell_res_set, sm.cell_res_set, sizeof in.cell_res_set);
  in.min_prb = nr_isac_sense_min_prb();
  in.floor_prb = nr_isac_sense_min_prb_floor();
  in.cont_target_ns = (uint64_t)nr_isac_sense_cont_target_ms() * 1000000ull;
  {
    int steps[SENSE_FLOOR_MAX_STEPS + 1];
    const int ns = nr_sense_floor_steps(in.min_prb, in.floor_prb, steps);
    steps[ns] = 1; /* one more threshold: arrivals of any size = the evidence that the cell has grants at all */
    for (int c = 0; c < in.n_cells; c++) { /* the continuity feed (DCI arrivals) over the last decision period, per level */
      nr_sense_gapstat_t g[SENSE_FLOOR_MAX_STEPS + 1];
      nr_sense_unit_arrival_gaps(c, now_ns, SENSE_DECIDE_NS, steps, ns + 1, g);
      memcpy(in.gap[c], g, (size_t)ns * sizeof g[0]);
      in.arrivals[c] = g[ns].n;
      in.gap_valid[c] = true;
    }
  }
  in.now_ns = now_ns;
  for (int c = 0; c < in.n_cells; c++) { /* the wideband-grant resource: offered rate and recent costs, from the cell's cumulative counters (a counter that went down was reset under us: re-baseline) */
    nr_sense_wbg_stats_t ws;
    nr_sense_wbg_stats(c, &ws);
    const bool mono = ws.offered >= g_wbgp[c].offered && ws.taps >= g_wbgp[c].taps && ws.cost_ns >= g_wbgp[c].cost_ns && ws.records >= g_wbgp[c].records && ws.ring_ns >= g_wbgp[c].ring_ns;
    const bool first = !g_wbgp[c].t_ns; /* the first decision of the cell: the counters since start are one period's worth, so a cost measured from even one tap is used at once */
    if ((first || now_ns > g_wbgp[c].t_ns) && mono) {
      const double dt = first ? (double)SENSE_DECIDE_NS : (double)(now_ns - g_wbgp[c].t_ns);
      g_wbgp[c].offered_hz = (float)((double)(ws.offered - g_wbgp[c].offered) * 1e9 / dt);
      if (ws.taps > g_wbgp[c].taps) {
        const float m = (float)((double)(ws.cost_ns - g_wbgp[c].cost_ns) / (double)(ws.taps - g_wbgp[c].taps));
        g_wbgp[c].cost_per_tap = g_wbgp[c].cost_per_tap > 0.f ? 0.5f * g_wbgp[c].cost_per_tap + 0.5f * m : m;
      }
      if (ws.records > g_wbgp[c].records) {
        const float m = (float)((double)(ws.ring_ns - g_wbgp[c].ring_ns) / (double)(ws.records - g_wbgp[c].records));
        g_wbgp[c].ring_per_rec = g_wbgp[c].ring_per_rec > 0.f ? 0.5f * g_wbgp[c].ring_per_rec + 0.5f * m : m;
      }
    } else if (!mono) {
      g_wbgp[c].offered_hz = 0.f; /* counters restarted: this period has no rate */
    }
    in.wbg_offered_hz[c] = g_wbgp[c].offered_hz;
    in.wbg_cost_ns[c] = g_wbgp[c].cost_per_tap;
    in.wbg_ring_ns[c] = g_wbgp[c].ring_per_rec;
    g_wbgp[c].t_ns = now_ns; g_wbgp[c].offered = ws.offered; g_wbgp[c].taps = ws.taps; g_wbgp[c].cost_ns = ws.cost_ns; g_wbgp[c].records = ws.records; g_wbgp[c].ring_ns = ws.ring_ns;
  }
  if (!smp) { /* R3: cells the scheduler does not decide for (>= NR_SENSE_CELLS) get no wideband-grant tap; say so once per cell */
    static bool warned[NR_CELL_MAX]; /* scheduler thread only */
    for (int c = NR_SENSE_CELLS; c < NR_CELL_MAX; c++) {
      nr_sense_wbg_stats_t ws;
      nr_sense_wbg_stats(c, &ws);
      if (ws.overcap && !warned[c]) { warned[c] = true; LOG_W(PHY, "SENSING: SENSE_WBG cell %d is beyond NR_SENSE_CELLS=%d: no scheduler decision, no wideband-grant CFR for it (offered_wbg_overcap counts its wide grants)\n", c, NR_SENSE_CELLS); }
    }
  }
  static nr_sense_sched_out_t out;
  nr_sense_sched_decide(&in, &g_st, &out);
  apply(&in);
  { int active = 0; /* cells that are actually running: registered AND their PCI is known (set at sync); a registered cell that has not synced does not count, one that syncs counts from the next decision */
    for (int c = 0; c < in.n_cells; c++) active += in.pci[c] >= 0;
    nr_sense_single_set(active == 1); } /* from this decision on: exactly one active cell = no rationing; two or more = the scheduler's admission */
  for (int c = 0; c < in.n_cells; c++) nr_sense_wbg_set_rate(c, out.wbg_rate_hz[c]);
  for (int c = 0; c < in.n_cells; c++) {
    nr_sense_cell_floor_publish(c, out.floor_prb[c]);
    atomic_store_explicit(&g_floor_log[c], ((out.gap_p95_ns[c] / 1000000ull) << 17) | ((uint64_t)out.in_gap[c] << 16) | ((uint64_t)out.floor_prb[c] & 0xFFFF), memory_order_relaxed);
  }
  atomic_fetch_add(&g_decisions, 1);
  atomic_fetch_add(&g_enables, (uint64_t)out.enables);
  atomic_fetch_add(&g_disables, (uint64_t)out.disables);
  atomic_fetch_add(&g_bad_cap, (uint64_t)out.bad_cap);
  atomic_store(&g_binding, (int)out.binding);
  if (!smp) sched_log(&in, &sm, &out); /* a test sampler runs without the log subsystem */
}

static void sched_log(const nr_sense_sched_in_t *in, const nr_sense_sched_sample_t *sm, const nr_sense_sched_out_t *out)
{
  static const char *const sn[NR_UNIT_SRC_N] = {"pdsch_dmrs", "pdsch_data", "pusch_dmrs", "srs"};
  char set[512];
  int w = 0;
  set[0] = 0;
  for (int i = 0; i < out->n_admitted && i < NR_SENSE_ADMIT_LOG && w < (int)sizeof set - 64; i++)
    w += snprintf(set + w, sizeof set - (size_t)w, "%sc%d/%u/%s=%.3f", i ? "," : "", out->admitted[i].cell, (unsigned)out->admitted[i].rnti, sn[out->admitted[i].src], out->admitted[i].val);
  char tc[64];
  int tw = 0;
  tc[0] = 0;
  for (int c = 0; c < in->n_cells && tw < (int)sizeof tc - 12; c++) tw += snprintf(tc + tw, sizeof tc - (size_t)tw, "%s%d", c ? "," : "", out->floor_prb[c]);
  uint64_t viol = 0, skips = 0, drops = 0;
  uint32_t en = 0;
  for (int c = 0; c < sm->n_cells; c++) {
    nr_sense_unit_cell_stats_t cs;
    nr_sense_unit_cell_stats(c, &cs);
    viol += cs.violations; skips += cs.skips; en += cs.units_enabled; drops += sm->ring_drops[c];
  }
  LOG_A(PHY, "SENSING: SENSE_SCHED decision=%lu cells=%d enables=%d disables=%d bad_cap=%d binding=%s top=%s elig=%d cold_deferred=%d admitted=%d [%s] t_cell=[%s] enabled_now=%u violations=%lu disabled_skips=%lu ring_drops=%lu pool_cap_ms=%.1f pool_base_ms=%.1f queue_ul_base=unmeasured gpu_base=unmeasured\n",
        (unsigned long)atomic_load(&g_decisions), sm->n_cells, out->enables, out->disables, out->bad_cap, nr_sense_res_name((int)out->binding), nr_sense_res_name((int)out->top), out->n_elig, out->cold_deferred, out->n_admitted, set, tc, en,
        (unsigned long)viol, (unsigned long)skips, (unsigned long)drops, sm->res[NR_RES_POOL].cap_ns * 1e-6, sm->res[NR_RES_POOL].base_p99_ns * 1e-6); /* per 50 ms bucket */
  for (int c = 0; c < in->n_cells; c++) {
    nr_sense_wbg_stats_t ws;
    nr_sense_wbg_stats(c, &ws);
    LOG_A(PHY, "SENSING: SENSE_WBG cell=%d offered_hz=%.1f cost_us=%.1f frac=%.2f rate_hz=%.2f ring_ns=%.0f offered=%lu admitted=%lu taps=%lu records=%lu\n", c, in->wbg_offered_hz[c], in->wbg_cost_ns[c] * 1e-3,
          out->wbg_frac[c], out->wbg_rate_hz[c], (double)in->wbg_ring_ns[c], (unsigned long)ws.offered, (unsigned long)ws.admitted, (unsigned long)ws.taps, (unsigned long)ws.records);
  }
  {
    int steps[SENSE_FLOOR_MAX_STEPS];
    const int ns = nr_sense_floor_steps(in->min_prb, in->floor_prb, steps);
    for (int c = 0; c < in->n_cells; c++) {
      char lv[160];
      int w = 0;
      lv[0] = 0;
      for (int i = 0; i < ns; i++) w += snprintf(lv + w, sizeof lv - (size_t)w, "%s%d:%.1f/%u", i ? "," : "", steps[i], (double)in->gap[c][i].p95_gap_ns * 1e-6, (unsigned)in->gap[c][i].n);
      LOG_A(PHY, "SENSING: SENSE_FLOOR cell=%d pci=%d t_cell=%d gap_p95_ms=%.1f in_gap=%d target_ms=%.0f arrivals=%u levels(prb:p95_ms/n)=[%s]\n", c, sm->pci[c], out->floor_prb[c], (double)out->gap_p95_ns[c] * 1e-6, (int)out->in_gap[c],
            (double)in->cont_target_ns * 1e-6, (unsigned)in->arrivals[c], lv);
    }
  }
  /* the units behind the decision (the first 8 per cell): enabled flag, wideband rate, valid ratio, p99 CPU per bucket (ms), skips and violations since the row was created */
  for (int c = 0; c < in->n_cells; c++) {
    char u[640];
    int w = 0;
    u[0] = 0;
    for (int i = 0; i < in->n_units[c] && i < 8 && w < (int)sizeof u - 96; i++) {
      const nr_sense_unit_view_t *v = &in->units[c][i];
      w += snprintf(u + w, sizeof u - (size_t)w, "%s%u/%s:%s wb=%.1f ratio=%.2f cpu99=%.2f ring99=%.2f da99=%.2f wait99=%.2f skips=%u viol=%u", i ? " | " : "", (unsigned)v->rnti, sn[v->src], v->enabled ? "ON" : "off",
                    v->wb_rate_hz, v->valid_ratio, v->cpu_p99_ns * 1e-6, v->ring_p99_ns * 1e-6, v->da_p99_ns * 1e-6, v->wait_p99_ns * 1e-6, v->skips, v->violations);
    }
    LOG_A(PHY, "SENSING: SENSE_UNITS cell=%d pci=%d da_cap_ms=%.0f ring_cap_ms=%.0f dec_wait_max_ms=%.1f n=%d %s\n", c, sm->pci[c], sm->cell_res[c][NR_RES_DA].cap_ns * 1e-6, sm->cell_res[c][NR_RES_SENSE_RING].cap_ns * 1e-6, sm->cell_res[c][NR_RES_QUEUE_DL].base_p99_ns * 1e-6, in->n_units[c], u);
  }
}

static void *sched_main(void *arg)
{
  (void)arg;
  {
    cpu_set_t all; /* ... and no fixed core: the creator's affinity is replaced by every CPU the process may use */
    CPU_ZERO(&all);
    for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &all);
    if (sched_setaffinity(0, sizeof all, &all) != 0) { /* the kernel refuses CPUs outside the cpuset: fall back to the process's own mask */
      cpu_set_t pm;
      if (sched_getaffinity(getpid(), sizeof pm, &pm) == 0) sched_setaffinity(0, sizeof pm, &pm);
    }
  }
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  uint64_t next = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec, sub_next = next + 500000000ull; /* the first step primes the counters; the first decision is one period later */
  while (!atomic_load(&g_stop)) {
    clock_gettime(CLOCK_MONOTONIC, &t);
    const uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    if (now >= next) {
      nr_sense_sched_step(now);
      next = now + SENSE_DECIDE_NS;
    } else if (now >= sub_next && !atomic_load(&g_sampler)) {
      sub_tick(now);
      sub_next = now + 500000000ull;
    }
    usleep(50000);
  }
  return NULL;
}

void nr_sense_sched_start(void)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING || atomic_exchange(&g_running, 1))
    return;
  atomic_store(&g_stop, 0);
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 2u << 20);
  struct sched_param sp = {.sched_priority = 0};
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED); /* SCHED_OTHER whatever the creator runs as ... */
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  pthread_attr_setschedparam(&at, &sp); /* >= 1 MB: the decision's candidate table is ~250 KB on the stack */
  if (pthread_create(&g_thread, &at, sched_main, NULL) != 0) {
    atomic_store(&g_running, 0);
    LOG_E(PHY, "SENSING: SENSE_SCHED thread not started\n");
  } else if (!atomic_load(&g_sampler)) { /* a test sampler runs without the log subsystem */
    LOG_A(PHY, "SENSING: SENSE_SCHED started period_s=%d headroom=%.2f cells=%d\n", (int)(SENSE_DECIDE_NS / 1000000000ull), SENSE_HEADROOM, nr_cell_count());
  }
  pthread_attr_destroy(&at);
}

void nr_sense_sched_stop(void)
{
  if (!atomic_exchange(&g_running, 0))
    return;
  atomic_store(&g_stop, 1);
  pthread_join(g_thread, NULL);
}
