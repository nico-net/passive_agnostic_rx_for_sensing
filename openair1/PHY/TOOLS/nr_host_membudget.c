/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_host_membudget.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* scalable modules: weight of the free part of the budget (the GrantWork LLR pool takes most; the rest is for the fixed modules' estimates) */
static const int host_weight[NR_HOST_MEM_N] = {60, 0, 0, 0, 0, 0, 0, 0};
_Static_assert(NR_HOST_MEM_N == 8, "host_weight and nr_host_mem_name follow the module list");

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
static int meminfo_kb(const char *text, const char *key, unsigned long long *out)
{
  const char *p = text ? strstr(text, key) : NULL;
  if (!p) return -1;
  p += strlen(key);
  char *end;
  errno = 0;
  *out = strtoull(p, &end, 10);
  return end == p || errno ? -1 : 0;
}
int nr_host_parse_hugetlb(const char *text, size_t *total, size_t *free_b)
{
  unsigned long long t, f, sz;
  if (meminfo_kb(text, "HugePages_Total:", &t) || meminfo_kb(text, "HugePages_Free:", &f) || meminfo_kb(text, "Hugepagesize:", &sz)) return -1;
  *total = (size_t)(t * sz) << 10;
  *free_b = (size_t)(f * sz) << 10;
  return 0;
}
/* one smaps entry: header "start-end perms offset dev inode [path]", then "Key: value kB" lines, the last one "VmFlags: ..." */
size_t nr_host_parse_smaps_unfaulted(const char *text)
{
  size_t total = 0, size_kb = 0, rss_kb = 0, kps_kb = 4;
  int counted = 0, in_vma = 0;
  const char *l = text;
  while (l && *l) {
    const char *eol = strchr(l, '\n');
    const size_t len = eol ? (size_t)(eol - l) : strlen(l);
    char line[512];
    snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), l);
    unsigned long a, b;
    char perms[8] = "", path[256] = "";
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &a, &b, perms, path) >= 3 && strchr(line, '-') && strlen(perms) == 4) {
      in_vma = 1;
      size_kb = rss_kb = 0;
      kps_kb = 4;
      /* accessible (not a PROT_NONE reservation), not a device mapping (VM_IO/VM_PFNMAP, e.g. /dev/nvidia*), not the kernel's special pages */
      counted = (perms[0] == 'r' || perms[1] == 'w' || perms[2] == 'x') && strncmp(path, "/dev/", 5) && strcmp(path, "[vvar]") && strcmp(path, "[vsyscall]") && strcmp(path, "[vvar_vclock]");
    } else if (in_vma) {
      unsigned long long v;
      if (!strncmp(line, "Size:", 5) && sscanf(line + 5, "%llu", &v) == 1) size_kb = v;
      else if (!strncmp(line, "Rss:", 4) && sscanf(line + 4, "%llu", &v) == 1) rss_kb = v;
      else if (!strncmp(line, "KernelPageSize:", 15) && sscanf(line + 15, "%llu", &v) == 1) kps_kb = v;
      else if (!strncmp(line, "VmFlags:", 8)) {
        const char *f = line + 8;
        const int special = strstr(f, " io") || strstr(f, " pf") || strstr(f, " ht") ? 1 : 0;
        if (counted && !special && kps_kb == 4 && size_kb > rss_kb) total += (size_t)(size_kb - rss_kb) << 10;
        in_vma = 0;
      }
    }
    if (!eol) break;
    l = eol + 1;
  }
  return total;
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
  if (nr_host_parse_hugetlb(buf, &o->hugetlb_total, &o->hugetlb_free)) o->hugetlb_total = o->hugetlb_free = 0;
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
  int pending[NR_HOST_MEM_N]; /* reserved, not allocated yet */
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
  st.pending[m] = 0;
  pthread_mutex_unlock(&mu);
}
void nr_host_membudget_reserve(nr_host_mem_module_t m, size_t bytes)
{
  if (m >= NR_HOST_MEM_N) return;
  pthread_mutex_lock(&mu);
  st.bytes[m] = bytes;
  st.scale[m] = 1.0;
  st.committed[m] = 1;
  st.pending[m] = 1;
  pthread_mutex_unlock(&mu);
}
size_t nr_host_membudget_headroom(void)
{
  const size_t B = nr_host_membudget_total();
  if (B == SIZE_MAX) return SIZE_MAX;
  const size_t c = nr_host_membudget_committed();
  return c >= B ? 0 : B - c;
}
size_t nr_host_membudget_hard_limit(void)
{
  pthread_mutex_lock(&mu);
  probe_once_locked();
  const size_t avail = st.probe.avail;
  size_t (*gc)(void) = st.gpu_committed;
  pthread_mutex_unlock(&mu);
  if (avail == SIZE_MAX) return SIZE_MAX;
  size_t h = avail > NR_HOST_MB_HARD_RESERVE ? avail - NR_HOST_MB_HARD_RESERVE : 0;
  if (gc) { const size_t g = gc(); h = g >= h ? 0 : h - g; }
  return h;
}
int nr_host_membudget_over_hard(char *why, size_t n)
{
  const size_t H = nr_host_membudget_hard_limit(), c = nr_host_membudget_committed();
  if (H == SIZE_MAX || c <= H) return 0;
  if (why && n) {
    int k = snprintf(why, n, "committed %zu MB > hard limit %zu MB (avail - %zu MB):", c >> 20, H >> 20, NR_HOST_MB_HARD_RESERVE >> 20);
    pthread_mutex_lock(&mu);
    for (int m = 0; m < NR_HOST_MEM_N && k > 0 && (size_t)k < n; m++)
      if (st.committed[m] && st.bytes[m]) k += snprintf(why + k, n - k, " %s=%zuMB", nr_host_mem_name[m], st.bytes[m] >> 20);
    pthread_mutex_unlock(&mu);
  }
  return 1;
}
size_t nr_host_membudget_live_avail(void)
{
  pthread_mutex_lock(&mu);
  probe_once_locked();
  const int emulated = !strcmp(st.probe.limited_by, "NR_HOST_MEM_BUDGET_MB");
  const size_t emu = st.probe.avail;
  pthread_mutex_unlock(&mu);
  if (emulated) { /* no live view of an emulated host: what it had minus what was allocated since (the pending reservations are not allocated) */
    size_t c = 0;
    pthread_mutex_lock(&mu);
    for (int m = 0; m < NR_HOST_MEM_N; m++) if (!st.pending[m]) c += st.bytes[m];
    pthread_mutex_unlock(&mu);
    return c >= emu ? 0 : emu - c;
  }
  nr_host_probe_t p;
  nr_host_probe_at("/proc", "/sys/fs/cgroup", &p);
  return p.avail;
}
size_t nr_host_membudget_live_share(nr_host_mem_module_t m)
{
  if (m >= NR_HOST_MEM_N) return 0;
  const size_t live = nr_host_membudget_live_avail();
  if (live == SIZE_MAX) return SIZE_MAX;
  size_t B = nr_host_membudget_live_calc(live), pend = 0;
  size_t (*gc)(void);
  pthread_mutex_lock(&mu);
  for (int o = 0; o < NR_HOST_MEM_N; o++) if (st.pending[o] && o != (int)m) pend += st.bytes[o];
  gc = st.gpu_committed;
  pthread_mutex_unlock(&mu);
  if (gc) { const size_t g = gc(); B = g >= B ? 0 : B - g; } /* unified: conservative (part of the GPU commitments is already in MemAvailable) */
  B = pend >= B ? 0 : B - pend;
  return host_weight[m] ? B / 100 * (size_t)host_weight[m] : B;
}
long nr_host_membudget_size(nr_host_mem_module_t m, int pct, size_t fixed, size_t per_unit, long lo, long hi)
{
  if (m >= NR_HOST_MEM_N || hi < 1) return 0;
  pthread_mutex_lock(&mu);
  const size_t own = st.committed[m] ? st.bytes[m] : 0;
  pthread_mutex_unlock(&mu);
  const size_t hr = nr_host_membudget_headroom();
  const long k = nr_host_membudget_fit_pct(hr, own, pct, fixed, per_unit, lo, hi);
  nr_host_membudget_commit(m, k ? fixed + per_unit * (size_t)k : 0, (double)k / (double)hi);
  return k;
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
  size_t c = 0;
  for (int m = 0; m < NR_HOST_MEM_N; m++) c += st.bytes[m];
  const size_t hard = st.probe.avail == SIZE_MAX ? SIZE_MAX : st.probe.avail > NR_HOST_MB_HARD_RESERVE ? st.probe.avail - NR_HOST_MB_HARD_RESERVE : 0;
  int k = snprintf(out, n, "avail_mb=%lld limited_by=%s budget_mb=%lld hard_mb=%lld committed_mb=%zu headroom_mb=%lld unified=%d hugetlb_mb=%zu/free=%zu(outside_MemAvailable)",
                   st.probe.avail == SIZE_MAX ? -1LL : (long long)(st.probe.avail >> 20), st.probe.limited_by, B == SIZE_MAX ? -1LL : (long long)(B >> 20),
                   hard == SIZE_MAX ? -1LL : (long long)(hard >> 20), c >> 20, B == SIZE_MAX ? -1LL : (long long)((c >= B ? 0 : B - c) >> 20), st.gpu_committed != NULL,
                   st.probe.hugetlb_total >> 20, st.probe.hugetlb_free >> 20);
  for (int m = 0; m < NR_HOST_MEM_N && k > 0 && (size_t)k < n; m++) {
    if (!st.committed[m]) continue;
    k += snprintf(out + k, n - k, " %s=%lluMB/scale=%.3f%s%s", nr_host_mem_name[m], (unsigned long long)(st.bytes[m] >> 20), st.scale[m], st.bytes[m] ? "" : "(off)",
                  st.pending[m] ? "(reserved)" : "");
    if (st.failures[m] && (size_t)k < n) k += snprintf(out + k, n - k, "/failures=%llu", (unsigned long long)st.failures[m]);
  }
  pthread_mutex_unlock(&mu);
  return k;
}
static _Atomic int g_usrp_ring_ms = -1;
void nr_host_membudget_set_usrp_ring_ms(int ms) { g_usrp_ring_ms = ms; }
int nr_host_membudget_usrp_ring_ms(void) { return g_usrp_ring_ms; }
void nr_host_membudget_test_reset(void)
{
  g_usrp_ring_ms = -1;
  pthread_mutex_lock(&mu);
  memset(&st, 0, sizeof st);
  pthread_mutex_unlock(&mu);
}
