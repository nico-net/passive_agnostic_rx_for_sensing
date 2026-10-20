/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CORE_PLAN_H
#define NR_CORE_PLAN_H
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded by Linux's fixed cpu_set_t. Larger/sparse CPU IDs are rejected, never truncated. */
#define NR_CP_MAX_CPUS 1024
#define NR_CP_MAX_ROLES 256
#define NR_CP_AUTO (-1)
typedef struct {
  unsigned char online[NR_CP_MAX_CPUS], allowed[NR_CP_MAX_CPUS];
  unsigned char isolated[NR_CP_MAX_CPUS], nohz[NR_CP_MAX_CPUS], irq[NR_CP_MAX_CPUS], dpdk[NR_CP_MAX_CPUS];
  int capacity[NR_CP_MAX_CPUS], package[NR_CP_MAX_CPUS], cluster[NR_CP_MAX_CPUS], sibling[NR_CP_MAX_CPUS];
  char host[128];
} nr_core_machine_t;
typedef enum {
  NR_CP_UE, NR_CP_RING, NR_CP_UHD, NR_CP_DL, NR_CP_UL, NR_CP_SYNC,
  NR_CP_SCAN, NR_CP_USS, NR_CP_PDSCH, NR_CP_PUSCH, NR_CP_POOL,
  NR_CP_GPU, NR_CP_CSIRS, NR_CP_NID, NR_CP_STATS, NR_CP_ROLE_COUNT
} nr_core_role_t;
typedef struct { nr_core_role_t role; int index, core; bool explicit_core, shared; } nr_core_assignment_t;
typedef struct {
  nr_core_assignment_t a[NR_CP_MAX_ROLES];
  int count;
  unsigned char background[NR_CP_MAX_CPUS], realtime[NR_CP_MAX_CPUS];
} nr_core_plan_t;
/* Pure parser/solver: no environment, no filesystem, no process affinity changes. */
int nr_core_parse_list(const char *, unsigned char mask[NR_CP_MAX_CPUS]);
int nr_core_parse_hex(const char *, unsigned char mask[NR_CP_MAX_CPUS]);
int nr_core_parse_uhd(const char *, unsigned char mask[NR_CP_MAX_CPUS]);
int nr_core_plan_add(nr_core_plan_t *, nr_core_role_t, int count, int first_core);
int nr_core_plan_build(const nr_core_machine_t *, nr_core_plan_t *, char *error, size_t);
const char *nr_core_role_name(nr_core_role_t);
/* Detect online/topology/cpuset and UHD config. usrp_args determines whether DPDK is active. */
int nr_core_machine_read(nr_core_machine_t *, const char *usrp_args, char *error, size_t);
/* Called once before receiver workers; exports unset envs, restricts inherited helper affinity. */
int nr_core_plan_activate(const nr_core_machine_t *, const nr_core_plan_t *);
bool nr_core_plan_active(void);
int nr_core_plan_cpu(nr_core_role_t, int index, int fallback);
/* True for a core the critical lane uses (UE/ring/UHD/DL/UL/sync/scan/PDSCH/PUSCH roles); false without an active plan. */
bool nr_core_plan_is_critical(int core);
void nr_core_plan_mark_critical_for_test(int core); /* core < 0 clears */
/* threadCreate hook: affinity is set in pthread attributes BEFORE the thread can run.
 * Background workers use SCHED_OTHER so forced sharing cannot starve lower FIFO priorities. */
int nr_core_plan_thread_attr(pthread_attr_t *, int affinity, int *priority);
/* For raw pthread background helpers, including those spawned by RT actors. */
int nr_core_plan_create(pthread_t *, pthread_attr_t *, void *(*)(void *), void *, int core);
/* ISAC_FLOAT_CORES=<cpu list> (unset = off, offline CPUs dropped): the core lane of every receiver thread without an
 * explicit core; only without an active core plan (an active plan owns all masks). threadCreate()/nr_core_plan_create() with core -1 use it. */
/* Called once early in main(): moves every existing task that still has the main thread's inherited mask
 * to the float lane (explicitly pinned tasks keep their core). Threads created later inherit it. No-op when off. */
int nr_core_float_apply(void);
/* For a background thread created by a pinned thread: move the calling thread to the float lane. No-op when off. */
void nr_core_float_enter(void);
/* nr_env_list_nth: the k-th entry (0-based) of the comma list in env `name` ("17,18"), or `dflt` when unset/short/malformed. */
#include <stdlib.h>
#include <string.h>
/* nr_env_core: getenv for the site core lists, with the on-air (t26yE) site map as the default when unset. */
static inline const char *nr_env_core(const char *name)
{
  static const char *const dflt[][2] = {{"ISAC_PDSCH_CORES", "6,7,8,9,3,4"}, {"ISAC_PUSCH_CORES", "8,9"}, {"ISAC_GPU_WORKER_CORE", "10"},
                                        {"ISAC_UHD_CTRL_CORES", "15"}, {"ISAC_PDCCH_USS_CORE", "12"}, {"ISAC_PDCCH_NID_GPU_CORE", "13"}};
  const char *e = getenv(name);
  for (unsigned i = 0; !e && i < sizeof(dflt) / sizeof(dflt[0]); i++)
    if (!strcmp(name, dflt[i][0])) e = dflt[i][1];
  return e;
}
static inline int nr_env_list_nth(const char *name, int k, int dflt)
{
  const char *cl = nr_env_core(name);
  for (int j = 0; cl && *cl; j++) {
    char *end;
    const long c = strtol(cl, &end, 10);
    if (end == cl) break;
    if (j == k) return (int)c;
    cl = (*end == ',') ? end + 1 : end;
  }
  return dflt;
}
#ifdef __cplusplus
}
#endif
#endif
