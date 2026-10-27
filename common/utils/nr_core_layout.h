/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * Host-derived CPU layout of the passive receiver (one topology probe at start-up -> one core plan, one log line
 * "SENSING: CORE_PLAN ...", read by every consumer instead of hard-coded CPU numbers).
 *
 * INPUT (nr_cl_topo_t): online CPUs, the process affinity as inherited (taskset / cgroup), isolcpus and nohz_full,
 * capacity (cpu_capacity, else cpufreq max, else equal), SMT siblings, L3 (else NUMA) locality, the NUMA node of the GPU,
 * RT capability (CAP_SYS_NICE or RLIMIT_RTPRIO), whether a radio is used, and the UHD DPDK lcores.
 *
 * RULES (nr_cl_build, pure):
 *  - A = online & allowed & !DPDK. Nothing is ever placed outside A, with one exception that is the operator's own
 *    reservation: when A is the kernel default (A == online minus isolated, i.e. not narrowed by taskset/cgroup), the
 *    isolated CPUs carry the radio IO busy-pollers (UHD control, then the USRP ring), as on the DGX (15, 16).
 *    Isolated CPUs inside a narrowed A are the RT set the operator intends: radio IO first, then the RT reader.
 *    Isolated CPUs never take pool, lane, housekeeping or background work. An explicit ISAC_UE_RT_CORE on an isolated
 *    CPU keeps that CPU for the reader (the radio IO then takes the other isolated CPUs, else big general CPUs).
 *  - RT reader (UEthread_0): highest capacity (10 % buckets), then the GPU's NUMA node, then the lowest number, never
 *    the housekeeping anchor (the lowest CPU, kept for kernel/housekeeping when |A| >= 3). Its SMT siblings stay idle
 *    when |A| >= 8. Radio IO without isolated CPUs: the next big CPUs, other L3 domain first, lowest number.
 *  - W = A minus the RT set. Pool order "near": RT's L3 domain, capacity, distance to RT, distinct physical cores first.
 *    Lane order "far": other domain, low capacity, far from RT, high number.
 *      W >= 14  SEP: lane = 5 far CPUs (dedicated float lane), housekeeping = 3 (anchor + leftovers), pool = the rest
 *               (W - 8, at most 32). This is today's DGX layout (pool 6,7,8,9,3,4, lane 10-14, housekeeping 0-2).
 *      6..13    COMPACT: housekeeping = the anchor, lane = 4 far CPUs (W >= 10), 3 (W 8..9) or 2 (W 6..7), pool = the rest.
 *               (sens6, isolcpus 1-2: RT 3, housekeeping 0, lane 10-13, pool 4,5,6,7,8,9.)
 *      <= 5     SMALL: one lane CPU (the anchor) for every unpinned thread, pool = the rest (none when W <= 1).
 *    GPU worker / USS / nID take lane CPUs 0 / 2 / 3 when the lane has them (else they float on the lane); the GPU
 *    check workers use the lane, the PDSCH recovery worker the lane minus the GPU worker and USS CPUs; the dashboard
 *    the last housekeeping CPU.
 *  - Explicit ISAC_* core variables stay operator overrides: ISAC_UE_RT_CORE, ISAC_USRP_RING_CORE, ISAC_UHD_CTRL_CORES,
 *    ISAC_PDSCH_CORES/ISAC_PUSCH_CORES (pool), ISAC_FLOAT_CORES (lane), ISAC_GPU_WORKER_CORE, ISAC_PDCCH_USS_CORE,
 *    ISAC_PDCCH_NID_GPU_CORE, ISAC_GPU_CHECK_CORES, ISAC_DASHBOARD_CORE; a value naming no usable CPU is ignored with a
 *    note in the plan line. ISAC_CORE_PLAN=0 switches the layout off (legacy site defaults 15 / 10-14 / built-in lists).
 *  - Without CAP_SYS_NICE (and RLIMIT_RTPRIO 0) every thread runs SCHED_OTHER; the plan is still applied as CPU
 *    affinity (sched_setaffinity needs no capability). ISAC_CORE_PIN_NOCAP=0 restores the old floating behaviour.
 * Without nr_core_layout_init(passive=true) nothing is active and every consumer keeps its legacy behaviour.
 */
#ifndef NR_CORE_LAYOUT_H
#define NR_CORE_LAYOUT_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_CL_MAX_CPUS 1024
#define NR_CL_MAX_POOL 32
#define NR_CL_SEP_MIN_W 14   /* pool >= 6 + lane 5 + housekeeping 3 */
#define NR_CL_SMALL_MAX_W 5
#define NR_CL_LANE 5
#define NR_CL_SEP_HK 3
#define NR_CL_MAX_CELLS 16 /* = NR_POOL_MAX_CELLS (nr_core_pool.h; asserted in nr_core_layout.c) */
#define NR_CL_MIN_POOL 2   /* floor of a hostable plan: DL cap N-1 and the scan reserve token need >= 2 tokens (W >= 3) */

typedef enum { NR_CL_SEP = 0, NR_CL_COMPACT, NR_CL_SMALL } nr_cl_mode_t;

typedef struct {
  int ncpu; /* CPU ids < ncpu are considered */
  unsigned char online[NR_CL_MAX_CPUS], allowed[NR_CL_MAX_CPUS], isolated[NR_CL_MAX_CPUS], nohz[NR_CL_MAX_CPUS], dpdk[NR_CL_MAX_CPUS];
  int cap[NR_CL_MAX_CPUS];  /* relative capacity, 0 = unknown (all equal) */
  int core[NR_CL_MAX_CPUS]; /* physical core key: equal keys = SMT siblings */
  int llc[NR_CL_MAX_CPUS];  /* L3 domain key (lowest CPU sharing the L3), -1 unknown */
  int node[NR_CL_MAX_CPUS]; /* NUMA node, -1 unknown */
  int gpu_node;             /* NUMA node of the GPU, -1 none/unknown */
  bool rt_capable, radio;
  char host[64];
} nr_cl_topo_t;

typedef struct { /* explicit operator overrides; -1 / has_* false = none */
  int rt, ring, gpu, uss, nid, dash;
  bool has_uhd, has_pool, has_lane, has_check;
  unsigned char uhd[NR_CL_MAX_CPUS], pool[NR_CL_MAX_CPUS], lane[NR_CL_MAX_CPUS], check[NR_CL_MAX_CPUS];
} nr_cl_over_t;

typedef struct {
  nr_cl_mode_t mode;
  int W, anchor;
  int n_cells;                 /* cells the plan was built for (1 = single cell, the historical layout) */
  int rt_cell[NR_CL_MAX_CELLS]; /* RT reader (slot thread) of each cell, -1 = none found; rt_cell[0] == rt */
  bool feed_req;               /* a wideband feed reader (G6 radio -> ring thread) was planned; feed = its CPU, -1 = none found (the reader then floats on the home lane) */
  int feed;                    /* the feed reader: one more RT-like I/O reader after rt_cell[], same ranking; -1 when not requested */
  int max_cells;               /* most cells this host can host with every reader distinct and pool >= NR_CL_MIN_POOL (0: not even one) */
  int rt, ring, uhd, gpu, uss, nid, dashboard; /* -1 = none / unpinned */
  int npool, pool[NR_CL_MAX_POOL];             /* ordered tokens */
  unsigned char usable[NR_CL_MAX_CPUS];  /* CPUs an explicit pin may name: online & (allowed | isolated) */
  unsigned char allowed[NR_CL_MAX_CPUS]; /* A */
  unsigned char uhd_mask[NR_CL_MAX_CPUS];
  unsigned char lane[NR_CL_MAX_CPUS];    /* mask of every thread without an explicit core (the float lane) */
  unsigned char home[NR_CL_MAX_CPUS];    /* idle mask of the pool members (A - pool - RT/IO) */
  unsigned char hk[NR_CL_MAX_CPUS];      /* housekeeping */
  unsigned char check[NR_CL_MAX_CPUS];   /* dmrsIdBg / gpuCheckBg in SEP/SMALL */
  unsigned char recov[NR_CL_MAX_CPUS];   /* pdschRecov idle mask */
  char src[256];                         /* which fields came from the environment, and ignored overrides */
} nr_core_layout_t;

void nr_cl_over_none(nr_cl_over_t *);
/* Pure: no environment, filesystem or affinity access. Returns 0. */
int nr_cl_build(const nr_cl_topo_t *, const nr_cl_over_t *, nr_core_layout_t *);
/* Pure, N cells (1..NR_CL_MAX_CELLS, clamped): one RT reader per cell (Decision F2 (a)), each taken by the ranking of the single
 * reader (isolated RT set first, else general CPUs minus the anchor and the radio IO; capacity buckets, GPU NUMA node, lowest
 * number; with |C| >= 8 a CPU whose SMT sibling is already a reader is taken only when nothing else is left). Cell 0's reader and the
 * radio IO are those of nr_cl_build; the pool, the float lane and the housekeeping are built on the reduced W with the same
 * SEP / COMPACT / SMALL rules. nr_cl_build(t, o, L) == nr_cl_build_n(t, o, 1, L) (struct and CORE_PLAN line).
 * Returns 0 when all n_cells readers exist and the pool has >= NR_CL_MIN_POOL tokens, else 1 (the layout is still filled best effort;
 * cells without a reader have rt_cell = -1; n_cells > max_cells). */
int nr_cl_build_n(const nr_cl_topo_t *, const nr_cl_over_t *, int n_cells, nr_core_layout_t *);
/* As nr_cl_build_n plus, when `feed`, ONE MORE reader after the cells' readers for the wideband feed (radio read -> wideband ring: a real-time I/O thread,
 * as the cells' slot threads are): taken by the same ranking and off the same W, so the pool, lane and housekeeping are built on the reduced W. The CORE_PLAN
 * line then carries " feed=<cpu>". feed = false is nr_cl_build_n exactly (struct and line). */
int nr_cl_build_nf(const nr_cl_topo_t *, const nr_cl_over_t *, int n_cells, bool feed, nr_core_layout_t *);
/* Most cells the host can host (the value of nr_core_layout_t.max_cells), for the admission model. */
int nr_cl_max_cells(const nr_cl_topo_t *, const nr_cl_over_t *);
/* The CORE_PLAN line (without trailing newline). */
int nr_cl_format(const nr_cl_topo_t *, const nr_core_layout_t *, char *buf, size_t n);
const char *nr_cl_mode_name(nr_cl_mode_t);
int nr_cl_mask_str(const unsigned char *mask, int ncpu, char *buf, size_t n); /* "0-2,10" or "-" */

/* Probe of this process/host (the affinity is read as inherited: call before any affinity change). */
int nr_cl_topo_read(nr_cl_topo_t *, bool radio);
void nr_cl_over_from_env(nr_cl_over_t *);

/* Process-wide plan. init: once, first thing in main(); no-op unless passive (and unless ISAC_CORE_PLAN=0). Logs the
 * CORE_PLAN line to stderr and exports every planned key that is unset (nr_core_layout_env_default) so that all
 * existing readers (getenv / nr_env_core / the radio library) see the plan instead of the built-in site numbers. */
void nr_core_layout_init(bool passive, bool radio);
/* Same with the cell count the caller decided (the CORE_PLAN line then lists the extra readers when n_cells > 1). */
void nr_core_layout_init_n(bool passive, bool radio, int n_cells);
/* Same, with the wideband feed reader planned (nr_cl_build_nf); the receiver passes feed = (several cells && a radio). feed = false is nr_core_layout_init_n. */
void nr_core_layout_init_nf(bool passive, bool radio, int n_cells, bool feed);
/* The feed reader's CPU of the active plan: -1 when inactive / not planned / not found. */
int nr_core_layout_feed_cpu(void);
/* threadCreate: true when an explicit affinity is applied although the process has no CAP_SYS_NICE (layout active and
 * ISAC_CORE_PIN_NOCAP != 0). */
bool nr_core_layout_pin_nocap(void);
const nr_core_layout_t *nr_core_layout(void); /* NULL when inactive */
bool nr_core_layout_active(void);
/* threadCreate hook: an explicit core that this host cannot use becomes -1 (logged once). Identity when inactive. */
int nr_core_layout_filter_cpu(int core, const char *who);
/* Planned value of an ISAC_* core key (NULL = not a layout key or inactive; "" = planned unpinned, "-1" for the nID core). */
const char *nr_core_layout_env_default(const char *name);
/* Test hook: activate a plan built from a fixture (passive process emulation). */
void nr_core_layout_set_for_test(const nr_cl_topo_t *, const nr_cl_over_t *);
#ifdef __cplusplus
}
#endif
#endif
