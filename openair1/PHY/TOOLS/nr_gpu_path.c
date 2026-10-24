/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-path GPU/CPU choice: see nr_gpu_path.h. */
#include "nr_gpu_path.h"
#include "nr_gpu_platform_internal.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const g_names[NR_GPU_PATH_N] = {"fep", "chest", "demod", "ldpc_dl", "ldpc_ul", "pass_dl", "pass_ul", "ul_fep"};
static const char *const g_env[NR_GPU_PATH_N] = {"NR_GPU_PATH_FEP",     "NR_GPU_PATH_CHEST",   "NR_GPU_PATH_DEMOD",   "NR_GPU_PATH_LDPC_DL",
                                                 "NR_GPU_PATH_LDPC_UL", "NR_GPU_PATH_PASS_DL", "NR_GPU_PATH_PASS_UL", "NR_GPU_PATH_UL_FEP"};
enum { MAX_SAMPLES = 64 };

const char *nr_gpu_path_name(nr_gpu_path_t p) { return (unsigned)p < NR_GPU_PATH_N ? g_names[p] : "?"; }

int nr_gpu_path_parse_override(const char *v)
{
  if (!v || !*v || !strcmp(v, "auto")) return NR_GPU_PATH_UNSET;
  if (!strcmp(v, "gpu")) return NR_GPU_PATH_GPU;
  if (!strcmp(v, "cpu")) return NR_GPU_PATH_CPU;
  return -2;
}

int nr_gpu_path_parse_select(const char *v)
{
  if (!v || !*v || !strcmp(v, "auto")) return NR_GPU_PATH_SEL_AUTO;
  if (!strcmp(v, "bench")) return NR_GPU_PATH_SEL_BENCH;
  if (!strcmp(v, "off")) return NR_GPU_PATH_SEL_OFF;
  return -1;
}

int nr_gpu_path_policy_begin(nr_gpu_platform_model_t tier, int select, int override, int gpu_ok, int measure, const char **reason)
{
  const char *r;
  int c;
  if (!gpu_ok) { r = "gpu_unavailable"; c = NR_GPU_PATH_CPU; }
  else if (override == NR_GPU_PATH_CPU || override == NR_GPU_PATH_GPU) { r = "env"; c = override; }
  else if (select == NR_GPU_PATH_SEL_OFF) { r = "select_off"; c = NR_GPU_PATH_GPU; }
  else if (tier != NR_GPU_SHARED && tier != NR_GPU_DISCRETE) { r = "cpu_tier"; c = NR_GPU_PATH_CPU; }
  else if (tier == NR_GPU_SHARED && select != NR_GPU_PATH_SEL_BENCH) { r = "shared"; c = NR_GPU_PATH_GPU; }
  else if (measure == NR_GPU_PATH_MEASURE_NONE) {
    /* nothing to measure: on SHARED (bench mode) the entry runs as before; elsewhere fail closed */
    r = tier == NR_GPU_SHARED ? "shared" : "direct_only";
    c = tier == NR_GPU_SHARED ? NR_GPU_PATH_GPU : NR_GPU_PATH_CPU;
  } else { r = "measure"; c = NR_GPU_PATH_TRIAL; }
  if (reason) *reason = r;
  return c;
}

int nr_gpu_path_policy_verdict(double gpu_us, double cpu_us, int n_gpu, int n_cpu, int n_min, double margin, const char **reason)
{
  const char *r;
  int c;
  if (!isfinite(gpu_us) || !isfinite(cpu_us) || gpu_us <= 0 || cpu_us <= 0 || n_gpu < n_min || n_cpu < n_min || n_min < 1 || !isfinite(margin)
      || margin <= 0) {
    r = "bench_fail";
    c = NR_GPU_PATH_CPU;
  } else if (gpu_us < margin * cpu_us) {
    r = "faster";
    c = NR_GPU_PATH_GPU;
  } else {
    r = "slower";
    c = NR_GPU_PATH_CPU;
  }
  if (reason) *reason = r;
  return c;
}

static int cmp_d(const void *a, const void *b)
{
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
double nr_gpu_path_median(const double *v, int n)
{
  if (!v || n < 1) return NAN;
  if (n > MAX_SAMPLES) n = MAX_SAMPLES;
  double t[MAX_SAMPLES];
  memcpy(t, v, sizeof(double) * (size_t)n);
  qsort(t, (size_t)n, sizeof(double), cmp_d);
  return n & 1 ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
}

/* ---- registry ---- */
typedef struct {
  _Atomic int state; /* NR_GPU_PATH_UNSET until begun */
  int tier;
  double s[2][MAX_SAMPLES]; /* [0] CPU, [1] GPU */
  int ns[2];
  unsigned picks;
} path_t;
static path_t g_p[NR_GPU_PATH_N] = {[0 ... NR_GPU_PATH_N - 1] = {.state = NR_GPU_PATH_UNSET}};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_env_read, g_select, g_trial_n = 8;
static double g_margin = 1.0;
static char g_last[512];

static const char *tier_name(int t) { return t == NR_GPU_SHARED ? "shared" : t == NR_GPU_DISCRETE ? "discrete" : "cpu"; }

/* emit: one GPU_PATH line through the platform logger (the receiver forwards it to LOG_I / LOG_W); mu held */
static void emit(int warning, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(int warning, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last, sizeof(g_last), fmt, ap);
  va_end(ap);
  nr_gpu_platform_log(warning, g_last);
}

static void read_env_locked(void)
{
  if (g_env_read) return;
  g_env_read = 1;
  const char *sel = getenv("NR_GPU_PATH_SELECT");
  g_select = nr_gpu_path_parse_select(sel);
  if (g_select < 0) {
    emit(1, "SENSING: GPU_PATH NR_GPU_PATH_SELECT=\"%s\" is not auto|bench|off; ignored (auto)", sel);
    g_select = NR_GPU_PATH_SEL_AUTO;
  }
  const char *tn = getenv("NR_GPU_PATH_TRIAL");
  if (tn && *tn) {
    const int n = atoi(tn);
    if (n >= 2 && n <= MAX_SAMPLES) g_trial_n = n;
    else emit(1, "SENSING: GPU_PATH NR_GPU_PATH_TRIAL=\"%s\" outside 2..%d; ignored (%d)", tn, MAX_SAMPLES, g_trial_n);
  }
  const char *mg = getenv("NR_GPU_PATH_MARGIN");
  if (mg && *mg) {
    const double m = atof(mg);
    if (m >= 0.1 && m <= 10) g_margin = m;
    else emit(1, "SENSING: GPU_PATH NR_GPU_PATH_MARGIN=\"%s\" outside 0.1..10; ignored (%.2f)", mg, g_margin);
  }
}

int nr_gpu_path_begin_tier(nr_gpu_path_t p, nr_gpu_platform_model_t tier, int gpu_ok, int measure)
{
  if ((unsigned)p >= NR_GPU_PATH_N) return NR_GPU_PATH_CPU;
  pthread_mutex_lock(&g_mu);
  path_t *P = &g_p[p];
  int st = atomic_load(&P->state);
  if (st != NR_GPU_PATH_UNSET) {
    pthread_mutex_unlock(&g_mu);
    return st;
  }
  read_env_locked();
  const char *ov = getenv(g_env[p]);
  int o = nr_gpu_path_parse_override(ov);
  if (o == -2) {
    emit(1, "SENSING: GPU_PATH %s=\"%s\" is not gpu|cpu|auto; ignored (auto)", g_env[p], ov);
    o = NR_GPU_PATH_UNSET;
  }
  const char *reason;
  st = nr_gpu_path_policy_begin(tier, g_select, o, gpu_ok, measure, &reason);
  P->tier = tier;
  P->ns[0] = P->ns[1] = 0;
  P->picks = 0;
  if (st == NR_GPU_PATH_TRIAL)
    emit(0, "SENSING: GPU_PATH path=%s tier=%s choice=trial reason=%s mode=%s n=%d margin=%.2f", g_names[p], tier_name(tier), reason,
         measure == NR_GPU_PATH_MEASURE_BENCH ? "bench" : "online", g_trial_n, g_margin);
  else {
    const int ignored = o != NR_GPU_PATH_UNSET && strcmp(reason, "env") != 0; /* e.g. gpu forced but the module is not ready */
    emit(0, "SENSING: GPU_PATH path=%s tier=%s choice=%s reason=%s%s%s", g_names[p], tier_name(tier), st == NR_GPU_PATH_GPU ? "gpu" : "cpu", reason,
         ignored ? " env_ignored=" : "", ignored ? (o ? "gpu" : "cpu") : "");
  }
  atomic_store(&P->state, st);
  pthread_mutex_unlock(&g_mu);
  return st;
}

int nr_gpu_path_begin(nr_gpu_path_t p, int gpu_ok, int measure)
{
  const nr_gpu_caps_t *c = nr_gpu_platform_probe();
  return nr_gpu_path_begin_tier(p, c ? c->model : NR_GPU_NONE, gpu_ok, measure);
}

/* decide_locked: the verdict of a TRIAL from medians; logs and stores */
static int decide_locked(nr_gpu_path_t p, double g, double c, int ng, int nc, const char *mode, const char *detail)
{
  path_t *P = &g_p[p];
  const char *reason;
  const int st = nr_gpu_path_policy_verdict(g, c, ng, nc, mode[0] == 'b' ? 1 : g_trial_n, g_margin, &reason);
  emit(0, "SENSING: GPU_PATH path=%s tier=%s choice=%s reason=%s mode=%s gpu_us=%.2f cpu_us=%.2f n_gpu=%d n_cpu=%d margin=%.2f%s%s", g_names[p],
       tier_name(P->tier), st == NR_GPU_PATH_GPU ? "gpu" : "cpu", reason, mode, g, c, ng, nc, g_margin, detail ? " " : "", detail ? detail : "");
  atomic_store(&P->state, st);
  return st;
}

int nr_gpu_path_finish(nr_gpu_path_t p, double gpu_us, double cpu_us, int n, const char *detail)
{
  if ((unsigned)p >= NR_GPU_PATH_N) return NR_GPU_PATH_CPU;
  pthread_mutex_lock(&g_mu);
  int st = atomic_load(&g_p[p].state);
  if (st == NR_GPU_PATH_TRIAL) st = decide_locked(p, gpu_us, cpu_us, n, n, "bench", detail);
  pthread_mutex_unlock(&g_mu);
  return st;
}

int nr_gpu_path_state(nr_gpu_path_t p) { return (unsigned)p < NR_GPU_PATH_N ? atomic_load_explicit(&g_p[p].state, memory_order_acquire) : NR_GPU_PATH_CPU; }
int nr_gpu_path_use_gpu(nr_gpu_path_t p) { return nr_gpu_path_state(p) != NR_GPU_PATH_CPU; }

int nr_gpu_path_pick(nr_gpu_path_t p)
{
  const int st = nr_gpu_path_state(p);
  if (st != NR_GPU_PATH_TRIAL) return st != NR_GPU_PATH_CPU;
  pthread_mutex_lock(&g_mu);
  path_t *P = &g_p[p];
  int r;
  if (atomic_load(&P->state) != NR_GPU_PATH_TRIAL) r = atomic_load(&P->state) != NR_GPU_PATH_CPU;
  else if (P->picks >= 32u * (unsigned)g_trial_n) {
    emit(1, "SENSING: GPU_PATH path=%s tier=%s choice=cpu reason=trial_incomplete picks=%u n_gpu=%d n_cpu=%d", g_names[p], tier_name(P->tier), P->picks,
         P->ns[1], P->ns[0]);
    atomic_store(&P->state, NR_GPU_PATH_CPU);
    r = 0;
  } else {
    /* alternate, but give the side with fewer samples the next call (a side whose calls are not all measured catches up) */
    r = P->ns[1] < P->ns[0] ? 1 : P->ns[0] < P->ns[1] ? 0 : (int)(P->picks & 1u) ^ 1;
    P->picks++;
  }
  pthread_mutex_unlock(&g_mu);
  return r;
}

void nr_gpu_path_sample(nr_gpu_path_t p, int on_gpu, double t)
{
  if (nr_gpu_path_state(p) != NR_GPU_PATH_TRIAL || !isfinite(t) || t < 0) return;
  pthread_mutex_lock(&g_mu);
  path_t *P = &g_p[p];
  if (atomic_load(&P->state) == NR_GPU_PATH_TRIAL) {
    const int k = on_gpu ? 1 : 0;
    if (P->ns[k] < MAX_SAMPLES) P->s[k][P->ns[k]++] = t;
    if (P->ns[0] >= g_trial_n && P->ns[1] >= g_trial_n)
      decide_locked(p, nr_gpu_path_median(P->s[1], P->ns[1]), nr_gpu_path_median(P->s[0], P->ns[0]), P->ns[1], P->ns[0], "online", NULL);
  }
  pthread_mutex_unlock(&g_mu);
}

void nr_gpu_path_fail(nr_gpu_path_t p, const char *why)
{
  if (nr_gpu_path_state(p) != NR_GPU_PATH_TRIAL) return;
  pthread_mutex_lock(&g_mu);
  path_t *P = &g_p[p];
  if (atomic_load(&P->state) == NR_GPU_PATH_TRIAL) {
    emit(1, "SENSING: GPU_PATH path=%s tier=%s choice=cpu reason=gpu_error (%s)", g_names[p], tier_name(P->tier), why ? why : "?");
    atomic_store(&P->state, NR_GPU_PATH_CPU);
  }
  pthread_mutex_unlock(&g_mu);
}

int nr_gpu_path_trial_n(void)
{
  pthread_mutex_lock(&g_mu);
  read_env_locked();
  const int n = g_trial_n;
  pthread_mutex_unlock(&g_mu);
  return n;
}
double nr_gpu_path_margin(void)
{
  pthread_mutex_lock(&g_mu);
  read_env_locked();
  const double m = g_margin;
  pthread_mutex_unlock(&g_mu);
  return m;
}

void nr_gpu_path_test_reset(void)
{
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < NR_GPU_PATH_N; i++) {
    atomic_store(&g_p[i].state, NR_GPU_PATH_UNSET);
    g_p[i].ns[0] = g_p[i].ns[1] = 0;
    g_p[i].picks = 0;
  }
  g_env_read = 0;
  g_select = NR_GPU_PATH_SEL_AUTO;
  g_trial_n = 8;
  g_margin = 1.0;
  g_last[0] = 0;
  pthread_mutex_unlock(&g_mu);
}
const char *nr_gpu_path_test_last_line(void) { return g_last; }
