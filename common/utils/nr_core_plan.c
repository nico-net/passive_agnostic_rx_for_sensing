/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_core_plan.h"
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

_Static_assert(NR_CP_MAX_CPUS == CPU_SETSIZE, "CPU mask size mismatch");
static nr_core_plan_t current;
static bool active;
/* fail: formats an error message into out (at most n bytes) and returns -1; used by the plan builder and the machine reader to report why a core plan cannot be built. */
static int fail(char *out, size_t n, const char *fmt, ...)
{
  va_list ap; va_start(ap, fmt); vsnprintf(out, n, fmt, ap); va_end(ap); return -1;
}
/* rt: true for the real-time roles (role <= NR_CP_SYNC): the roles that need an exclusive core and must not share with lower-priority work. */
static bool rt(nr_core_role_t r) { return r <= NR_CP_SYNC; }
/* nr_core_role_name: returns the short name of a core role (for example "ring" or "pdsch") for logs and environment messages, or "invalid". */
const char *nr_core_role_name(nr_core_role_t r)
{
  static const char *names[] = {"ue", "ring", "uhd_ctrl", "dl", "ul", "sync", "scan", "uss", "pdsch", "pusch",
                                "pool", "gpu_fep", "csirs", "nid", "stats"};
  return r >= 0 && r < NR_CP_ROLE_COUNT ? names[r] : "invalid";
}
/* nr_core_parse_list: parses a CPU list such as "6,7,8-9" into a bit mask (up to NR_CP_MAX_CPUS), returns -1 on a syntax error, an out-of-range CPU or a reversed range. Leading and trailing spaces are allowed.
 *   Callers: nr_core_machine_read() (online and isolated lists), nr_core_parse_uhd(), executables/nr-uesoftmodem.c and radio/USRP/usrp_lib.cpp (ISAC_UHD_CTRL_CORES).
 */
int nr_core_parse_list(const char *s, unsigned char mask[NR_CP_MAX_CPUS])
{
  unsigned char tmp[NR_CP_MAX_CPUS] = {0};
  if (!s) return -1;
  while (isspace((unsigned char)*s)) s++;
  while (*s) {
    if (!isdigit((unsigned char)*s)) return -1;
    char *end; errno = 0; long first = strtol(s, &end, 10), last = first;
    if (errno || first >= NR_CP_MAX_CPUS) return -1;
    s = end;
    if (*s == '-') {
      s++; if (!isdigit((unsigned char)*s)) return -1;
      errno = 0; last = strtol(s, &end, 10);
      if (errno || last >= NR_CP_MAX_CPUS || last < first) return -1;
      s = end;
    }
    for (int i = first; i <= last; i++) tmp[i] = 1;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) break;
    if (*s++ != ',' || !*s) return -1;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return -1;
  }
  memcpy(mask, tmp, sizeof(tmp)); return 0;
}
/* nr_core_parse_hex: parses a hexadecimal CPU mask (the kernel's IRQ affinity format, comma separated groups allowed) into a bit mask. Returns -1 on an invalid digit or when the mask is too wide. Caller: nr_core_machine_read(). */
int nr_core_parse_hex(const char *s, unsigned char mask[NR_CP_MAX_CPUS])
{
  unsigned char tmp[NR_CP_MAX_CPUS] = {0};
  size_t n = strlen(s); int bit = 0;
  while (n) {
    int c = tolower((unsigned char)s[--n]);
    if (isspace(c) || c == ',') continue;
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0 || bit + 4 > NR_CP_MAX_CPUS) return -1;
    for (int k = 0; k < 4; k++) tmp[bit++] = (v >> k) & 1;
  }
  memcpy(mask, tmp, sizeof(tmp)); return 0;
}
/* INI section selection is deliberate: never interpret another device's core list. */
/* nr_core_parse_uhd: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/utils/nr_core_plan.c:33, common/utils/nr_core_plan.c:249, common/utils/nr_core_plan.h:34, common/utils/tests/test_nr_core_plan.cpp:173.
 */
int nr_core_parse_uhd(const char *text, unsigned char mask[NR_CP_MAX_CPUS])
{
  char *copy = strdup(text), *save = NULL; if (!copy) return -1;
  int enabled = 0, found = 0, result = 0;
  unsigned char tmp[NR_CP_MAX_CPUS] = {0};
  for (char *s = strtok_r(copy, "\n", &save); s; s = strtok_r(NULL, "\n", &save)) {
    while (isspace((unsigned char)*s)) s++;
    char *comment = strpbrk(s, "#;"); if (comment) *comment = 0;
    if (*s == '[') {
      char section[128]; int k = 0;
      for (s++; *s && *s != ']' && k < 127; s++) if (!isspace((unsigned char)*s)) section[k++] = *s;
      section[k] = 0; enabled = !strcmp(section, "use_dpdk=1");
    } else if (enabled && !strncmp(s, "dpdk_corelist", 13)) {
      s += 13; while (isspace((unsigned char)*s)) s++;
      if (*s++ != '=' || nr_core_parse_list(s, tmp)) { result = -1; break; }
      for (int i = 0; i < NR_CP_MAX_CPUS; i++) mask[i] |= tmp[i];
      found = 1;
    }
  }
  free(copy); return result < 0 ? result : found;
}
/* nr_core_plan_add: appends `count` assignments of one role to the plan. With first >= 0 the assignments take consecutive CPUs starting at `first` (explicit, user-pinned); with first = -1 they are chosen later by nr_core_plan_build().
 *   Each assignment gets the next index of its role. Returns -1 for an invalid role, a count beyond the plan capacity, or a bad CPU. Callers: executables/nr-uesoftmodem.c and the tests.
 */
int nr_core_plan_add(nr_core_plan_t *p, nr_core_role_t role, int count, int first)
{
  if (role < 0 || role >= NR_CP_ROLE_COUNT || count < 0 || count > NR_CP_MAX_ROLES - p->count || first < -1
      || (first >= 0 && (first >= NR_CP_MAX_CPUS || count > NR_CP_MAX_CPUS - first))) return -1;
  int index = 0;
  for (int j = 0; j < p->count; j++) if (p->a[j].role == role) index++;
  for (int j = 0; j < count; j++) p->a[p->count++] = (nr_core_assignment_t){role, index++, first < 0 ? -1 : first + j, first >= 0, false};
  return 0;
}
/* same_core: true when two CPUs are SMT siblings of the same physical core (same package and same core id). */
static bool same_core(const nr_core_machine_t *m, int a, int b)
{
  return m->package[a] == m->package[b] && m->sibling[a] == m->sibling[b];
}
/* nr_core_plan_build: chooses a CPU for every role assignment that has none and records which CPUs are real-time and which are background, excluding the DPDK CPUs and their SMT siblings.
 *   Order: validate the DPDK CPUs; reserve every explicit (user) pin; then place the automatic roles in role order, preferring an exclusive core and, for real-time roles, a large capacity without an IRQ and an isolated or nohz_full CPU; then mark the background CPUs.
 *   The result sets shared on each assignment when two roles land on the same physical core. Returns 0, or -1 with a message in error. Callers: executables/nr-uesoftmodem.c and the tests.
 */
int nr_core_plan_build(const nr_core_machine_t *m, nr_core_plan_t *p, char *error, size_t n)
{
  int used[NR_CP_MAX_CPUS] = {0};
  int max_capacity = 1;
  for (int c=0;c<NR_CP_MAX_CPUS;c++) if (m->online[c] && m->capacity[c]>max_capacity) max_capacity=m->capacity[c];
  memset(p->realtime, 0, sizeof(p->realtime)); memset(p->background, 0, sizeof(p->background));
  if (p->count < 0 || p->count > NR_CP_MAX_ROLES) return fail(error,n,"too many roles");
  for (int c=0;c<NR_CP_MAX_CPUS;c++) if (m->dpdk[c] && !m->online[c])
    return fail(error,n,"DPDK CPU %d is offline",c);
  /* DPDK reserves the entire physical core, including SMT siblings. */
  unsigned char reserved[NR_CP_MAX_CPUS] = {0};
  for (int i = 0; i < NR_CP_MAX_CPUS; i++) if (m->online[i])
    for (int j = 0; j < NR_CP_MAX_CPUS; j++) if (m->dpdk[j] && same_core(m,i,j)) reserved[i] = 1;
  /* Reserve ALL overrides first, including lower priority roles. */
  for (int i = 0; i < p->count; i++) {
    nr_core_assignment_t *a = &p->a[i];
    if (a->role < 0 || a->role >= NR_CP_ROLE_COUNT) return fail(error,n,"invalid role");
    if (!a->explicit_core) { a->core = -1; continue; }
    int c = a->core;
    if (c < 0 || c >= NR_CP_MAX_CPUS || !m->online[c] || !m->allowed[c] || reserved[c])
      return fail(error,n,"explicit %s[%d]:%d is offline, outside the cpuset, or reserved for DPDK",nr_core_role_name(a->role),a->index,c);
    used[c]++;
    if (rt(a->role)) p->realtime[c] = 1;
  }
  for (int role = 0; role < NR_CP_ROLE_COUNT; role++) for (int i = 0; i < p->count; i++) {
    nr_core_assignment_t *a = &p->a[i]; if (a->role != role || a->explicit_core) continue;
    long best = LONG_MIN; int chosen = -1;
    /* Keep automatic RT roles exclusive when possible. Numbered user pins can exhaust
     * the free cores; in that case share a non-DPDK core rather than reject a working
     * legacy layout. Prefer sharing another actor over a busy-polling reader/control. */
    for (int pass = 0; pass < 2 && chosen < 0; pass++) for (int c = 0; c < NR_CP_MAX_CPUS; c++) {
      if (!m->online[c] || !m->allowed[c] || reserved[c]) continue;
      bool busy = false, realtime = false, nonrealtime = false; int physical_load = 0;
      for (int j = 0; j < NR_CP_MAX_CPUS; j++) if (used[j] && same_core(m,c,j)) {
        physical_load += used[j];
        if (p->realtime[j]) realtime = true;
      }
      for (int j = 0; j < p->count; j++) if (p->a[j].core >= 0 && (p->a[j].explicit_core || p->a[j].role < role)
          && same_core(m,c,p->a[j].core)) {
        if (p->a[j].role <= NR_CP_UHD) busy = true;
        if (!rt(p->a[j].role)) nonrealtime = true;
      }
      if (pass == 0 && (rt(role) ? physical_load != 0 : realtime)) continue;
      long score = -(long)used[c] * 1000000000L - (long)physical_load * 100000000L;
      if (pass && busy) score -= 10000000000L;
      if (pass && rt(role) && nonrealtime) score -= 5000000000L;
      if (pass && realtime && !rt(role)) score -= 1000000000L;
      /* Linux capacity is frequency-invariant but can differ a few percent even within the
       * same big/little class (997 vs 1017 on GB10). Bucket at 10% to avoid chasing that noise. */
      long capacity = ((long)m->capacity[c] * 10 + max_capacity / 2) / max_capacity;
      if (rt(role) || role == NR_CP_SCAN) {
        score += capacity * 1000000 + (!m->irq[c] ? 10000000 : 0)
                 + (m->isolated[c] || m->nohz[c] ? 1000000 : 0);
        /* Keep the ring near DPDK, separate from the RT reader's cluster when possible. */
        if (role == NR_CP_RING || role == NR_CP_UHD)
          for (int j = 0; j < NR_CP_MAX_CPUS; j++) if (m->dpdk[j] && m->package[c] == m->package[j]
              && m->cluster[c] == m->cluster[j]) { score += 500000; break; }
      } else {
        score += (m->irq[c] ? 10000000 : 0) - capacity * 100000;
      }
      if (score > best) { best = score; chosen = c; }
    }
    if (chosen < 0) return fail(error,n,"no allowed non-DPDK CPU for %s[%d]",nr_core_role_name(role),a->index);
    a->core = chosen; used[chosen]++;
    if (rt(role)) p->realtime[chosen] = 1;
  }
  int bg = 0;
  for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (m->online[c] && m->allowed[c] && !reserved[c]) {
    bool blocked = false;
    for (int j = 0; j < NR_CP_MAX_CPUS; j++) if (p->realtime[j] && same_core(m,c,j)) blocked = true;
    if (!blocked) { p->background[c] = 1; bg++; }
  }
  if (!bg) {
    /* An all-RT layout still needs a legal inherited mask for helper threads. */
    for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (m->online[c] && m->allowed[c] && !reserved[c]) {
      p->background[c] = 1; bg++;
    }
  }
  if (!bg) return fail(error,n,"no allowed non-DPDK CPU for helper threads");
  for (int i = 0; i < p->count; i++) {
    p->a[i].shared = false;
    for (int j = 0; j < p->count; j++) if (i != j && same_core(m,p->a[i].core,p->a[j].core)) p->a[i].shared = true;
  }
  return 0;
}
/* read_text: reads a small text file into buf (at most n-1 bytes); returns 0 only when the whole file fit, else -1. */
static int read_text(const char *path, char *buf, size_t n)
{
  FILE *f = fopen(path,"r"); if (!f) return -1;
  size_t got = fread(buf,1,n-1,f); int extra = fgetc(f); fclose(f); buf[got] = 0;
  return extra == EOF ? 0 : -1;
}
/* cpu_int: reads an integer sysfs attribute of one CPU (for example topology/core_id), or returns fallback when it cannot be read. */
static int cpu_int(int c, const char *leaf, int fallback)
{
  char path[256], value[128]; snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/%s",c,leaf);
  return read_text(path,value,sizeof(value)) ? fallback : atoi(value);
}
/* nr_core_machine_read: reads the host topology into *m: online, allowed (process cpuset), package, cluster, SMT sibling, capacity, isolated and nohz_full CPUs, IRQ affinity and the DPDK lcores.
 *   With use_dpdk=1 in args it reads the UHD configuration files (system, home, root and UHD_CONFIG_FILE) and fails when no dpdk_corelist is readable, so that DPDK cores are never used. Returns 0, or -1 with a message. Caller: executables/nr-uesoftmodem.c.
 */
int nr_core_machine_read(nr_core_machine_t *m, const char *args, char *error, size_t n)
{
  memset(m,0,sizeof(*m)); gethostname(m->host,sizeof(m->host)-1);
  char buf[16384];
  if (read_text("/sys/devices/system/cpu/online",buf,sizeof(buf)) || nr_core_parse_list(buf,m->online))
    return fail(error,n,"cannot read online CPU list (maximum supported CPU ID %d)", NR_CP_MAX_CPUS-1);
  cpu_set_t allowed;
  if (sched_getaffinity(0,sizeof(allowed),&allowed)) return fail(error,n,"cannot read process cpuset: %s",strerror(errno));
  for (int c=0;c<NR_CP_MAX_CPUS;c++) {
    m->allowed[c] = CPU_ISSET(c,&allowed);
    if (!m->online[c]) continue;
    m->package[c] = cpu_int(c,"topology/physical_package_id",0);
    m->cluster[c] = cpu_int(c,"topology/cluster_id",m->package[c]);
    m->sibling[c] = cpu_int(c,"topology/core_id",c);
    char siblings_path[256]; unsigned char siblings[NR_CP_MAX_CPUS];
    snprintf(siblings_path,sizeof(siblings_path),"/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",c);
    if (!read_text(siblings_path,buf,sizeof(buf)) && !nr_core_parse_list(buf,siblings))
      for (int j=0;j<NR_CP_MAX_CPUS;j++) if (siblings[j]) { m->sibling[c]=j; break; }
    m->capacity[c] = cpu_int(c,"cpu_capacity",0);
    if (!m->capacity[c]) m->capacity[c] = cpu_int(c,"cpufreq/cpuinfo_max_freq",1024000)/1000;
  }
  if (!read_text("/sys/devices/system/cpu/isolated",buf,sizeof(buf))) nr_core_parse_list(buf,m->isolated);
  if (!read_text("/sys/devices/system/cpu/nohz_full",buf,sizeof(buf))) nr_core_parse_list(buf,m->nohz);
  if (read_text("/proc/irq/default_smp_affinity",buf,sizeof(buf)) || nr_core_parse_hex(buf,m->irq))
    memcpy(m->irq,m->online,sizeof(m->irq));
  /* Reserve the union of all applicable config files: conservative across sudo/HOME changes. */
  bool dpdk = args && strstr(args,"use_dpdk=1");
  if (dpdk) {
    char homepath[4096]; snprintf(homepath,sizeof(homepath),"%s/.uhd/uhd.conf",getenv("HOME") ? getenv("HOME") : "");
    const char *paths[] = {"/etc/uhd/uhd.conf",homepath,"/root/.uhd/uhd.conf",getenv("UHD_CONFIG_FILE")};
    for (int i=0;i<4;i++) if (paths[i] && !read_text(paths[i],buf,sizeof(buf))) {
      if (nr_core_parse_uhd(buf,m->dpdk)<0) return fail(error,n,"invalid dpdk_corelist in %s",paths[i]);
    }
    int count=0; for(int c=0;c<NR_CP_MAX_CPUS;c++) count+=m->dpdk[c];
    if (!count) return fail(error,n,"use_dpdk=1 but no readable [use_dpdk=1] dpdk_corelist; cannot safely reserve lcores");
  }
  return 0;
}
/* print_mask: prints a CPU mask to stderr as ranges (for example "6-9,12"), or "none" when it is empty. Used by the core-plan log line. */
static void print_mask(const unsigned char mask[NR_CP_MAX_CPUS])
{
  bool first=true;
  for (int c=0;c<NR_CP_MAX_CPUS;c++) if (mask[c]) {
    int end=c; while (end+1<NR_CP_MAX_CPUS && mask[end+1]) end++;
    fprintf(stderr,"%s%d",first ? "" : ",",c); if(end!=c) fprintf(stderr,"-%d",end);
    first=false; c=end;
  }
  if(first) fprintf(stderr,"none");
}
/* env_for: returns the environment variable that overrides the core of a role (ISAC_UE_RT_CORE, ISAC_USRP_RING_CORE, ISAC_UHD_CTRL_CORES, ...), or NULL for roles without one. Caller: nr_core_plan_activate(). */
static const char *env_for(nr_core_role_t role)
{
  switch(role) {
    case NR_CP_UE:return "ISAC_UE_RT_CORE"; case NR_CP_RING:return "ISAC_USRP_RING_CORE";
    case NR_CP_UHD:return "ISAC_UHD_CTRL_CORES"; case NR_CP_USS:return "ISAC_PDCCH_USS_CORE";
    case NR_CP_GPU:return "ISAC_GPU_WORKER_CORE"; case NR_CP_CSIRS:return "ISAC_CSIRS_DISC_CORE";
    case NR_CP_NID:return "ISAC_PDCCH_NID_GPU_CORE"; default:return NULL;
  }
}
/* nr_core_plan_activate: makes a built plan active: moves the process and every existing thread (/proc/self/task) to the background CPUs with SCHED_OTHER, stores the plan, exports the role overrides (only when unset), and logs the plan.
 *   Logs warnings for shared physical CPUs. Returns 0, or -1 when an affinity or scheduler call fails. Caller: executables/nr-uesoftmodem.c (start-up).
 */
int nr_core_plan_activate(const nr_core_machine_t *m, const nr_core_plan_t *p)
{
  cpu_set_t set; CPU_ZERO(&set);
  for (int c=0;c<NR_CP_MAX_CPUS;c++) if (p->background[c]) CPU_SET(c,&set);
  if (sched_setaffinity(0,sizeof(set),&set)) return -1;
  /* Logging/configuration helpers may already exist. Changing only the main thread would leave
   * those helpers able to migrate onto DPDK CPUs on a host without isolcpus. No RT actors exist yet. */
  DIR *tasks = opendir("/proc/self/task");
  if (!tasks) return -1;
  struct dirent *entry; int saved_error = 0;
  while ((entry = readdir(tasks))) {
    char *end; long tid = strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end || tid <= 0 || tid > INT_MAX) continue;
    if (sched_setaffinity((pid_t)tid,sizeof(set),&set) && errno != ESRCH) { saved_error=errno; break; }
    struct sched_param sp = {0};
    if (sched_setscheduler((pid_t)tid,SCHED_OTHER,&sp) && errno != ESRCH) { saved_error=errno; break; }
  }
  closedir(tasks);
  if (saved_error) { errno=saved_error; return -1; }
  current=*p;
  for(int i=0;i<p->count;i++) {
    const char *env=env_for(p->a[i].role);
    if(env && p->a[i].role != NR_CP_UHD && !getenv(env)) { char value[32]; snprintf(value,sizeof(value),"%d",p->a[i].core); if(setenv(env,value,0)) return -1; }
  }
  if (!getenv("ISAC_UHD_CTRL_CORES")) {
    char value[NR_CP_MAX_ROLES * 6]; size_t pos = 0;
    for (int i = 0; i < p->count; i++) if (p->a[i].role == NR_CP_UHD)
      pos += snprintf(value + pos, sizeof(value) - pos, "%s%d", pos ? "," : "", p->a[i].core);
    if (pos && setenv("ISAC_UHD_CTRL_CORES", value, 0)) return -1;
  }
  active=true;
  int count=0; for(int c=0;c<NR_CP_MAX_CPUS;c++) count+=m->online[c];
  fprintf(stderr,"SENSING: CORE_PLAN host=%s cpus=%d isolated=",m->host,count); print_mask(m->isolated);
  fprintf(stderr," dpdk=");print_mask(m->dpdk);fprintf(stderr," irq=");print_mask(m->irq);
  fprintf(stderr," nohz_full=");print_mask(m->nohz);fprintf(stderr," allowed=");print_mask(m->allowed);fprintf(stderr," ->");
  for(int i=0;i<p->count;i++) fprintf(stderr," %s[%d]:%d%s",nr_core_role_name(p->a[i].role),p->a[i].index,p->a[i].core,p->a[i].explicit_core ? "(user)" : "");
  fprintf(stderr,"\n");
  for(int i=0;i<p->count;i++) if(p->a[i].shared)
    fprintf(stderr,"SENSING: CORE_PLAN warning: shared physical CPU %s[%d]:%d%s\n",nr_core_role_name(p->a[i].role),p->a[i].index,p->a[i].core,
            rt(p->a[i].role) ? " (RT role)" : "");
  return 0;
}
/* nr_core_plan_active: returns true once a plan has been activated by nr_core_plan_activate(). Callers: usrp_lib.cpp and the thread helpers. */
bool nr_core_plan_active(void) { return active; }
/* nr_core_plan_cpu: returns the planned CPU of role `role` with index `index`, or the fallback when no plan is active or the role is not in it. Callers: passive queues, the USRP ring and the PDCCH workers. */
int nr_core_plan_cpu(nr_core_role_t role, int index, int fallback)
{
  if(active) for(int i=0;i<current.count;i++) if(current.a[i].role==role && current.a[i].index==index) return current.a[i].core;
  return fallback;
}
/* nr_core_plan_thread_attr: sets the affinity and priority of a thread attribute object from the plan: one CPU when affinity >= 0 (must be a background or real-time CPU), else every background CPU.
 *   Priority is cleared (set to 0) unless the CPU is a real-time one. Returns 0, or an error code. Caller: nr_core_plan_create().
 */
int nr_core_plan_thread_attr(pthread_attr_t *attr, int affinity, int *priority)
{
  if(!active) return 0;
  cpu_set_t set; CPU_ZERO(&set);
  if(affinity>=0) {
    if(affinity>=NR_CP_MAX_CPUS || (!current.background[affinity] && !current.realtime[affinity])) return EINVAL;
    CPU_SET(affinity,&set);
  } else for(int c=0;c<NR_CP_MAX_CPUS;c++) if(current.background[c]) CPU_SET(c,&set);
  if(affinity<0 || !current.realtime[affinity]) *priority=0;
  return pthread_attr_setaffinity_np(attr,sizeof(set),&set);
}

/* nr_core_plan_create: creates a thread with the plan's affinity: an active plan gives the thread the CPU in `core` (or the background set) at SCHED_OTHER with explicit inheritance; with no plan it is a plain pthread_create().
 *   Uses the given attribute object or a local one. Returns the pthread_create() result.
 */
int nr_core_plan_create(pthread_t *thread, pthread_attr_t *attr, void *(*fn)(void *), void *arg, int core)
{
  if (!active) return pthread_create(thread, attr, fn, arg);
  pthread_attr_t local;
  bool owned = attr == NULL;
  int rc = 0;
  if (owned) { rc = pthread_attr_init(&local); if (rc) return rc; attr = &local; }
  int priority = 0;
  rc = nr_core_plan_thread_attr(attr, core, &priority);
  struct sched_param sp = {0};
  if (!rc) rc = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
  if (!rc) rc = pthread_attr_setschedpolicy(attr, SCHED_OTHER);
  if (!rc) rc = pthread_attr_setschedparam(attr, &sp);
  if (!rc) rc = pthread_create(thread, attr, fn, arg);
  if (owned) pthread_attr_destroy(attr);
  return rc;
}
