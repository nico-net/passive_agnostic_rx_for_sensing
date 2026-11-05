#define _GNU_SOURCE
/* Sensing-mode scheduler thread (plan Task S7). See nr_sense_sched_rt.h. */
#include "nr_sense_sched_rt.h"
#include "nr_sense_api.h"
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

static const char *const k_res_name[NR_RES_N] = {"pool", "queue_dl", "queue_ul", "gpu", "sense_ring"};
const char *nr_sense_res_name(int r) { return (unsigned)r < NR_RES_N ? k_res_name[r] : "?"; }

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
  const bool primed = g_prev.primed && now_ns > g_prev.t_ns;
  for (int k = 0; k < n; k++) {
    o->pci[k] = nr_passive_metrics_pci_of ? nr_passive_metrics_pci_of(k) : -1;
    nr_cell_bind(k);
    nr_sense_stats_t st;
    nr_sense_stats(&st);
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
  for (int k = 0; k < n; k++) { /* the per-cell resources (queues, ring) of each cell */
    o->cell_res_set[k] = true;
    memcpy(o->cell_res[k], o->res, sizeof o->res);
    if (o->ring_drops[k]) o->cell_res[k][NR_RES_SENSE_RING].base_p99_ns = (float)SENSE_BUCKET_NS;
  }
  return true;
}

void nr_sense_sched_set_sampler(bool (*fn)(nr_sense_sched_sample_t *, uint64_t)) { atomic_store(&g_sampler, fn); }

void nr_sense_sched_reset(void)
{
  memset(&g_st, 0, sizeof g_st);
  memset(&g_prev, 0, sizeof g_prev);
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
  in.now_ns = now_ns;
  static nr_sense_sched_out_t out;
  nr_sense_sched_decide(&in, &g_st, &out);
  apply(&in);
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
  uint64_t viol = 0, skips = 0, drops = 0;
  uint32_t en = 0;
  for (int c = 0; c < sm->n_cells; c++) {
    nr_sense_unit_cell_stats_t cs;
    nr_sense_unit_cell_stats(c, &cs);
    viol += cs.violations; skips += cs.skips; en += cs.units_enabled; drops += sm->ring_drops[c];
  }
  LOG_A(PHY, "SENSING: SENSE_SCHED decision=%lu cells=%d enables=%d disables=%d bad_cap=%d binding=%s admitted=%d [%s] enabled_now=%u violations=%lu disabled_skips=%lu ring_drops=%lu pool_cap_ms=%.1f pool_base_ms=%.1f queue_base=unmeasured gpu_base=unmeasured\n",
        (unsigned long)atomic_load(&g_decisions), sm->n_cells, out->enables, out->disables, out->bad_cap, nr_sense_res_name((int)out->binding), out->n_admitted, set, en,
        (unsigned long)viol, (unsigned long)skips, (unsigned long)drops, sm->res[NR_RES_POOL].cap_ns * 1e-6, sm->res[NR_RES_POOL].base_p99_ns * 1e-6); /* per 50 ms bucket */
  /* the units behind the decision (the first 8 per cell): enabled flag, wideband rate, valid ratio, p99 CPU per bucket (ms), skips and violations since the row was created */
  for (int c = 0; c < in->n_cells; c++) {
    char u[640];
    int w = 0;
    u[0] = 0;
    for (int i = 0; i < in->n_units[c] && i < 8 && w < (int)sizeof u - 96; i++) {
      const nr_sense_unit_view_t *v = &in->units[c][i];
      w += snprintf(u + w, sizeof u - (size_t)w, "%s%u/%s:%s wb=%.1f ratio=%.2f cpu99=%.2f skips=%u viol=%u", i ? " | " : "", (unsigned)v->rnti, sn[v->src], v->enabled ? "ON" : "off",
                    v->wb_rate_hz, v->valid_ratio, v->cpu_p99_ns * 1e-6, v->skips, v->violations);
    }
    LOG_A(PHY, "SENSING: SENSE_UNITS cell=%d pci=%d n=%d %s\n", c, sm->pci[c], in->n_units[c], u);
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
