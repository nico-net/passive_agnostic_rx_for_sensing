/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_host_membudget.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* scalable modules: weight of the free part of the budget (the GrantWork LLR pool takes most; the rest is for the fixed modules' estimates) */
static const int host_weight[NR_HOST_MEM_N] = {60, 0, 0, 0, 0, 0};

int nr_host_parse_meminfo(const char *text, size_t *out)
{
  const char *p = text ? strstr(text, "MemAvailable:") : NULL;
  if (!p) return -1;
  p += strlen("MemAvailable:");
  char *end;
  errno = 0;
  const unsigned long long kb = strtoull(p, &end, 10);
  if (end == p || errno) return -1;
  *out = (size_t)kb << 10;
  return 0;
}
int nr_host_parse_limit(const char *text, size_t *out)
{
  if (!text) return -1;
  while (isspace((unsigned char)*text)) text++;
  if (!strncmp(text, "max", 3)) { *out = SIZE_MAX; return 0; }
  if (!isdigit((unsigned char)*text)) return -1;
  char *end;
  errno = 0;
  const unsigned long long v = strtoull(text, &end, 10);
  if (end == text || errno) return -1;
  *out = v >= (1ull << 62) ? SIZE_MAX : (size_t)v; /* cgroup v1 "no limit" is PAGE_COUNTER_MAX (9223372036854771712) */
  return 0;
}
int nr_host_parse_cgroup_path(const char *text, int v1, char *path, size_t n)
{
  if (!text || !n) return -1;
  for (const char *l = text; *l;) {
    const char *eol = strchr(l, '\n');
    const size_t len = eol ? (size_t)(eol - l) : strlen(l);
    const char *c1 = memchr(l, ':', len);
    const char *c2 = c1 ? memchr(c1 + 1, ':', len - (size_t)(c1 + 1 - l)) : NULL;
    if (c1 && c2) {
      const size_t flen = (size_t)(c2 - c1 - 1);
      char fields[128];
      snprintf(fields, sizeof fields, "%.*s", (int)(flen < sizeof fields ? flen : sizeof fields - 1), c1 + 1);
      const int hit = v1 ? (strstr(fields, "memory") != NULL) : (flen == 0 && c1 == l + 1 && l[0] == '0');
      if (hit) {
        const size_t plen = len - (size_t)(c2 + 1 - l);
        snprintf(path, n, "%.*s", (int)(plen < n ? plen : n - 1), c2 + 1);
        return 0;
      }
    }
    if (!eol) break;
    l = eol + 1;
  }
  return -1;
}
static int read_text(const char *path, char *buf, size_t n)
{
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  const size_t k = fread(buf, 1, n - 1, f);
  fclose(f);
  buf[k] = 0;
  return 0;
}
static int read_bytes(const char *path, size_t *out)
{
  char b[256];
  return read_text(path, b, sizeof b) ? -1 : nr_host_parse_limit(b, out);
}
/* the tightest (limit - usage) over a cgroup and its ancestors; SIZE_MAX = no limit anywhere */
static size_t cgroup_headroom(const char *cg_root, const char *proc_root, int *used_v1)
{
  char buf[4096], path[1024], file[2300];
  snprintf(file, sizeof file, "%s/self/cgroup", proc_root);
  if (read_text(file, buf, sizeof buf)) return SIZE_MAX;
  size_t best = SIZE_MAX;
  for (int v1 = 0; v1 <= 1; v1++) {
    if (nr_host_parse_cgroup_path(buf, v1, path, sizeof path)) continue;
    if (v1 && !strcmp(path, "/")) { /* v1 root: the controller directory itself */ }
    *used_v1 = v1;
    for (;;) {
      char dir[2048];
      snprintf(dir, sizeof dir, "%s%s%s%s", cg_root, v1 ? "/memory" : "", path[0] == '/' && path[1] ? path : "", "");
      size_t limit = SIZE_MAX, high = SIZE_MAX, cur = 0;
      if (v1) {
        snprintf(file, sizeof file, "%s/memory.limit_in_bytes", dir);
        const int a = read_bytes(file, &limit);
        snprintf(file, sizeof file, "%s/memory.usage_in_bytes", dir);
        const int b = read_bytes(file, &cur);
        if (!a && !b && limit != SIZE_MAX) { const size_t h = limit > cur ? limit - cur : 0; if (h < best) best = h; }
      } else {
        snprintf(file, sizeof file, "%s/memory.max", dir);
        read_bytes(file, &limit);
        snprintf(file, sizeof file, "%s/memory.high", dir);
        read_bytes(file, &high);
        snprintf(file, sizeof file, "%s/memory.current", dir);
        const int b = read_bytes(file, &cur);
        if (high < limit) limit = high;
        if (!b && limit != SIZE_MAX) { const size_t h = limit > cur ? limit - cur : 0; if (h < best) best = h; }
      }
      char *slash = strrchr(path, '/');
      if (!slash || slash == path) break;
      *slash = 0;
    }
    return best;
  }
  return SIZE_MAX;
}
void nr_host_probe_at(const char *proc_root, const char *cg_root, nr_host_probe_t *o)
{
  memset(o, 0, sizeof *o);
  o->mem_available = o->cgroup_headroom = o->avail = SIZE_MAX;
  snprintf(o->limited_by, sizeof o->limited_by, "unknown");
  char buf[8192], file[1024];
  snprintf(file, sizeof file, "%s/meminfo", proc_root);
  size_t ma;
  if (!read_text(file, buf, sizeof buf) && !nr_host_parse_meminfo(buf, &ma)) {
    o->mem_available = ma;
    o->avail = ma;
    snprintf(o->limited_by, sizeof o->limited_by, "MemAvailable");
  }
  int v1 = 0;
  o->cgroup_headroom = cgroup_headroom(cg_root, proc_root, &v1);
  if (o->cgroup_headroom != SIZE_MAX && o->cgroup_headroom < o->avail) {
    o->avail = o->cgroup_headroom;
    snprintf(o->limited_by, sizeof o->limited_by, v1 ? "cgroup_v1" : "cgroup_v2");
  }
}

/* ---- process state ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int probed;
  nr_host_probe_t probe;
  size_t bytes[NR_HOST_MEM_N];
  double scale[NR_HOST_MEM_N];
  int committed[NR_HOST_MEM_N];
  uint64_t failures[NR_HOST_MEM_N];
  size_t (*gpu_committed)(void);
} st;
static void probe_once_locked(void)
{
  if (st.probed) return;
  st.probed = 1;
  nr_host_probe_at("/proc", "/sys/fs/cgroup", &st.probe);
  const char *e = getenv("NR_HOST_MEM_BUDGET_MB"); /* diagnostic: emulate a host with this much available memory */
  if (e && *e) {
    const unsigned long mbv = strtoul(e, NULL, 10);
    if (mbv) {
      st.probe.avail = (size_t)mbv << 20;
      snprintf(st.probe.limited_by, sizeof st.probe.limited_by, "NR_HOST_MEM_BUDGET_MB");
    }
  }
}
size_t nr_host_membudget_total(void)
{
  pthread_mutex_lock(&mu);
  probe_once_locked();
  size_t B = nr_host_membudget_calc(st.probe.avail);
  size_t (*gc)(void) = st.gpu_committed;
  pthread_mutex_unlock(&mu);
  if (gc && B != SIZE_MAX) { const size_t g = gc(); B = g >= B ? 0 : B - g; }
  return B;
}
size_t nr_host_membudget_committed(void)
{
  size_t s = 0;
  pthread_mutex_lock(&mu);
  for (int m = 0; m < NR_HOST_MEM_N; m++) s += st.bytes[m];
  pthread_mutex_unlock(&mu);
  return s;
}
size_t nr_host_membudget_share(nr_host_mem_module_t m)
{
  if (m >= NR_HOST_MEM_N) return 0;
  const size_t B = nr_host_membudget_total();
  if (B == SIZE_MAX) return SIZE_MAX;
  size_t fixed = 0;
  pthread_mutex_lock(&mu);
  for (int o = 0; o < NR_HOST_MEM_N; o++)
    if (o != (int)m && st.committed[o] && host_weight[o] == 0) fixed += st.bytes[o];
  pthread_mutex_unlock(&mu);
  const size_t freeb = fixed >= B ? 0 : B - fixed;
  return host_weight[m] ? freeb / 100 * (size_t)host_weight[m] : B;
}
void nr_host_membudget_commit(nr_host_mem_module_t m, size_t bytes, double scale)
{
  if (m >= NR_HOST_MEM_N) return;
  pthread_mutex_lock(&mu);
  st.bytes[m] = bytes;
  st.scale[m] = scale;
  st.committed[m] = 1;
  pthread_mutex_unlock(&mu);
}
void nr_host_membudget_set_unified(size_t (*gpu_committed)(void))
{
  pthread_mutex_lock(&mu);
  st.gpu_committed = gpu_committed;
  pthread_mutex_unlock(&mu);
}
void nr_host_membudget_note_failure(nr_host_mem_module_t m)
{
  if (m >= NR_HOST_MEM_N) return;
  pthread_mutex_lock(&mu);
  st.failures[m]++;
  pthread_mutex_unlock(&mu);
}
uint64_t nr_host_membudget_failures(nr_host_mem_module_t m)
{
  uint64_t v = 0;
  if (m >= NR_HOST_MEM_N) return 0;
  pthread_mutex_lock(&mu);
  v = st.failures[m];
  pthread_mutex_unlock(&mu);
  return v;
}
int nr_host_membudget_format(char *out, size_t n)
{
  const size_t B = nr_host_membudget_total();
  pthread_mutex_lock(&mu);
  probe_once_locked();
  int k = snprintf(out, n, "avail_mb=%lld limited_by=%s budget_mb=%lld unified=%d", st.probe.avail == SIZE_MAX ? -1LL : (long long)(st.probe.avail >> 20), st.probe.limited_by,
                   B == SIZE_MAX ? -1LL : (long long)(B >> 20), st.gpu_committed != NULL);
  for (int m = 0; m < NR_HOST_MEM_N && k > 0 && (size_t)k < n; m++) {
    if (!st.committed[m]) continue;
    k += snprintf(out + k, n - k, " %s=%lluMB/scale=%.3f%s", nr_host_mem_name[m], (unsigned long long)(st.bytes[m] >> 20), st.scale[m], st.bytes[m] ? "" : "(CPU)");
    if (st.failures[m] && (size_t)k < n) k += snprintf(out + k, n - k, "/failures=%llu", (unsigned long long)st.failures[m]);
  }
  pthread_mutex_unlock(&mu);
  return k;
}
void nr_host_membudget_test_reset(void)
{
  pthread_mutex_lock(&mu);
  memset(&st, 0, sizeof st);
  pthread_mutex_unlock(&mu);
}
