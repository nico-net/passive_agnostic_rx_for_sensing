/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_gpu_memstat.h"
#include "nr_gpu_platform_internal.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* One slot per (module, 0 = SHARED | 1 + cell). Counters are relaxed atomics: no lock anywhere. */
typedef struct {
  uint64_t cur[NR_MS_KIND_N], peak[NR_MS_KIND_N], n[NR_MS_KIND_N], ngraph;
} slot_t;
static slot_t g_slot[NR_MS_MOD_N][1 + NR_GPU_MEMSTAT_CELLS];
static uint64_t g_tot_cur[NR_MS_KIND_N], g_tot_peak[NR_MS_KIND_N], g_tot_n;
static __thread uint32_t t_tag; /* 0 = NR_MS_UNTAGGED / SHARED */

static const char *const g_names[NR_MS_MOD_N] = {"untagged", "platform", "pass", "pass_graph", "pdsch_fep", "pdsch_demod", "pdsch_ldpc", "pdcch_fep", "pusch_fep",
                                                 "ldpc_dec", "ldpc_cb0", "td_cb0", "csirs", "pdcch_nid", "polar", "exact", "chan"};
const char *nr_gpu_memstat_name(int mod) { return mod >= 0 && mod < NR_MS_MOD_N ? g_names[mod] : "?"; }

static void peak_max(uint64_t *p, uint64_t v)
{
  uint64_t o = __atomic_load_n(p, __ATOMIC_RELAXED);
  while (v > o && !__atomic_compare_exchange_n(p, &o, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void sub_sat(uint64_t *p, uint64_t b)
{
  uint64_t o = __atomic_load_n(p, __ATOMIC_RELAXED);
  while (!__atomic_compare_exchange_n(p, &o, o > b ? o - b : 0, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/* tag -> slot; a bad module or a PER_CELL cell out of range falls back to UNTAGGED/SHARED so the miss is visible. */
static slot_t *slot_of(uint32_t tag)
{
  const unsigned mod = tag & 0xff, scope = (tag >> 8) & 0xff, cell = tag >> 16;
  if (mod >= NR_MS_MOD_N || scope > NR_MS_PER_CELL || (scope == NR_MS_PER_CELL && cell >= NR_GPU_MEMSTAT_CELLS)) return &g_slot[NR_MS_UNTAGGED][0];
  return &g_slot[mod][scope == NR_MS_PER_CELL ? 1 + cell : 0];
}
/* The cell of the calling thread when a PER_CELL scope gives none (cell < 0): the GPU modules are dlopen'd libraries without a link to common/utils/nr_cell.c,
 * so the TLS index nr_cell_tls_idx_ is looked up by name in the process (glibc returns the calling thread's address of a TLS symbol); absent = cell 0. */
static int (*g_cell_fn)(void);
void nr_gpu_memstat_set_cell_fn_for_test(int (*fn)(void)) { g_cell_fn = fn; }
static int host_cell(void)
{
  if (g_cell_fn) return g_cell_fn();
  const int *p = (const int *)dlsym(RTLD_DEFAULT, "nr_cell_tls_idx_");
  return p ? *p : 0;
}
int nr_gpu_memstat_host_cell(void) { return host_cell(); }
uint32_t nr_gpu_memstat_enter(int mod, int scope, int cell)
{
  const uint32_t prev = t_tag;
  if (scope == NR_MS_PER_CELL && cell < 0) cell = host_cell();
  t_tag = nr_ms_pack(mod, scope, scope == NR_MS_PER_CELL ? cell : 0);
  return prev;
}
void nr_gpu_memstat_leave(uint32_t prev) { t_tag = prev; }
uint32_t nr_gpu_memstat_current(void) { return t_tag; }

void nr_gpu_memstat_note_tag(uint32_t tag, int kind, size_t bytes)
{
  if (kind < 0 || kind >= NR_MS_KIND_N) return;
  slot_t *s = slot_of(tag);
  peak_max(&s->peak[kind], __atomic_add_fetch(&s->cur[kind], bytes, __ATOMIC_RELAXED));
  __atomic_add_fetch(&s->n[kind], 1, __ATOMIC_RELAXED);
  peak_max(&g_tot_peak[kind], __atomic_add_fetch(&g_tot_cur[kind], bytes, __ATOMIC_RELAXED));
  __atomic_add_fetch(&g_tot_n, 1, __ATOMIC_RELAXED);
}
void nr_gpu_memstat_release_tag(uint32_t tag, int kind, size_t bytes)
{
  if (kind < 0 || kind >= NR_MS_KIND_N) return;
  sub_sat(&slot_of(tag)->cur[kind], bytes);
  sub_sat(&g_tot_cur[kind], bytes);
}
void nr_gpu_memstat_note(int kind, size_t bytes) { nr_gpu_memstat_note_tag(t_tag, kind, bytes); }
void nr_gpu_memstat_note_to(int mod, int scope, int cell, int kind, size_t bytes)
{
  nr_gpu_memstat_note_tag(nr_ms_pack(mod, scope, scope == NR_MS_PER_CELL ? cell : 0), kind, bytes);
}
void nr_gpu_memstat_note_graph(void) { __atomic_add_fetch(&slot_of(t_tag)->ngraph, 1, __ATOMIC_RELAXED); }

int nr_gpu_memstat_get(int mod, int scope, int cell, nr_gpu_memstat_t *out)
{
  if (mod < 0 || mod >= NR_MS_MOD_N || (scope == NR_MS_PER_CELL && (cell < 0 || cell >= NR_GPU_MEMSTAT_CELLS))) return -1;
  const slot_t *s = &g_slot[mod][scope == NR_MS_PER_CELL ? 1 + cell : 0];
  for (int k = 0; k < NR_MS_KIND_N; k++) {
    out->cur[k] = __atomic_load_n(&s->cur[k], __ATOMIC_RELAXED);
    out->peak[k] = __atomic_load_n(&s->peak[k], __ATOMIC_RELAXED);
    out->n[k] = __atomic_load_n(&s->n[k], __ATOMIC_RELAXED);
  }
  out->ngraph = __atomic_load_n(&s->ngraph, __ATOMIC_RELAXED);
  return 0;
}
uint64_t nr_gpu_memstat_cell_bytes(int cell, int kind)
{
  uint64_t t = 0;
  if (cell < 0 || cell >= NR_GPU_MEMSTAT_CELLS || kind < 0 || kind >= NR_MS_KIND_N) return 0;
  for (int m = 0; m < NR_MS_MOD_N; m++) t += __atomic_load_n(&g_slot[m][1 + cell].cur[kind], __ATOMIC_RELAXED);
  return t;
}
uint64_t nr_gpu_memstat_total(int scope, int kind)
{
  uint64_t t = 0;
  if (kind < 0 || kind >= NR_MS_KIND_N) return 0;
  for (int m = 0; m < NR_MS_MOD_N; m++) {
    if (scope == NR_MS_SHARED) t += __atomic_load_n(&g_slot[m][0].cur[kind], __ATOMIC_RELAXED);
    else for (int c = 0; c < NR_GPU_MEMSTAT_CELLS; c++) t += __atomic_load_n(&g_slot[m][1 + c].cur[kind], __ATOMIC_RELAXED);
  }
  return t;
}
uint64_t nr_gpu_memstat_total_peak(int kind) { return kind >= 0 && kind < NR_MS_KIND_N ? __atomic_load_n(&g_tot_peak[kind], __ATOMIC_RELAXED) : 0; }
uint64_t nr_gpu_memstat_total_count(void) { return __atomic_load_n(&g_tot_n, __ATOMIC_RELAXED); }

long nr_gpu_memstat_rss_anon_kb(void)
{
  FILE *f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "RssAnon:", 8)) {
      if (sscanf(line + 8, "%ld", &kb) != 1) kb = -1;
      break;
    }
  fclose(f);
  return kb;
}

static int put(char *out, size_t n, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int put(char *out, size_t n, int pos, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int ok = out && (size_t)pos < n;
  const int w = vsnprintf(ok ? out + pos : NULL, ok ? n - (size_t)pos : 0, fmt, ap);
  va_end(ap);
  return pos + (w > 0 ? w : 0);
}
#define U(x) ((unsigned long long)(x))
static int group(char *out, size_t n, int pos, const char *label, int scope, int cell)
{
  pos = put(out, n, pos, " %s=[", label);
  int first = 1;
  for (int m = 0; m < NR_MS_MOD_N; m++) {
    nr_gpu_memstat_t s;
    nr_gpu_memstat_get(m, scope, cell, &s);
    if (!s.n[0] && !s.n[1] && !s.n[2] && !s.cur[0] && !s.cur[1] && !s.cur[2] && !s.ngraph) continue;
    pos = put(out, n, pos, "%s%s:%llu/%llu/%llu/%llu", first ? "" : " ", g_names[m], U(s.cur[0]), U(s.cur[1]), U(s.cur[2]), U(s.n[0] + s.n[1] + s.n[2]));
    if (s.ngraph) pos = put(out, n, pos, "+%llug", U(s.ngraph));
    first = 0;
  }
  return put(out, n, pos, "]");
}
static int cell_used(int c) { return nr_gpu_memstat_cell_bytes(c, 0) || nr_gpu_memstat_cell_bytes(c, 1) || nr_gpu_memstat_cell_bytes(c, 2); }
int nr_gpu_memstat_format(char *out, size_t n, const char *at)
{
  int pos = 0, ncell = 0;
  for (int c = 0; c < NR_GPU_MEMSTAT_CELLS; c++) ncell += cell_used(c);
  nr_gpu_memstat_t u;
  nr_gpu_memstat_get(NR_MS_UNTAGGED, NR_MS_SHARED, 0, &u);
  pos = put(out, n, pos, "GPUMEM at=%s dev=%llu pin=%llu mgd=%llu peak_dev=%llu peak_pin=%llu peak_mgd=%llu allocs=%llu untagged=%llu rss_anon_kb=%ld cells=%d", at ? at : "?",
            U(nr_gpu_memstat_total(0, 0) + nr_gpu_memstat_total(1, 0)), U(nr_gpu_memstat_total(0, 1) + nr_gpu_memstat_total(1, 1)),
            U(nr_gpu_memstat_total(0, 2) + nr_gpu_memstat_total(1, 2)), U(nr_gpu_memstat_total_peak(0)), U(nr_gpu_memstat_total_peak(1)), U(nr_gpu_memstat_total_peak(2)),
            U(nr_gpu_memstat_total_count()), U(u.cur[0] + u.cur[1] + u.cur[2]), nr_gpu_memstat_rss_anon_kb(), ncell);
  pos = group(out, n, pos, "shared", NR_MS_SHARED, 0);
  for (int c = 0; c < NR_GPU_MEMSTAT_CELLS; c++) {
    if (!cell_used(c)) continue;
    char label[16];
    snprintf(label, sizeof label, "cell%d", c);
    pos = group(out, n, pos, label, NR_MS_PER_CELL, c);
  }
  return pos;
}
void nr_gpu_memstat_log(const char *at)
{
  char line[4096], msg[4200];
  nr_gpu_memstat_format(line, sizeof line, at);
  if (at && !strcmp(at, "exit")) {
    fprintf(stderr, "SENSING: %s\n", line);
    return;
  }
  snprintf(msg, sizeof msg, "SENSING: %s", line);
  nr_gpu_platform_log(0, msg);
}
void nr_gpu_memstat_reset_for_test(void)
{
  memset(g_slot, 0, sizeof g_slot);
  memset(g_tot_cur, 0, sizeof g_tot_cur);
  memset(g_tot_peak, 0, sizeof g_tot_peak);
  g_tot_n = 0;
  t_tag = 0;
}
