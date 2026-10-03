/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-grant CB0 scheduler for the elimination channel (td/cb0-cpu-wiring, 2026-10-03). PURE: no receiver state, no
 * clock, no locks -- the runtime glue (nr_td_cb0_wire.c) owns one scheduler under its own lock and passes the time in.
 *
 * HYPOTHESIS SET. Per grant, from the engine's ACTIVE set read BEFORE any decode of the grant, two-level:
 *   member h  <=>  mix(seed, abs_slot, gkey(h)) mod m1 == 0  AND  mix(seed', abs_slot, key(h)) mod m2 == 0,
 * plus always the grant's scheduled (full-TB) hypothesis. key(h) is the hypothesis VALUE (every field), gkey(h) its
 * GEOMETRY (S, L, k0, DM-RS mask; mapping type only for a mask-less entry) -- the part GrantWork shares one FEP / chest /
 * LLR computation over. Both are catalogue values, so neither moves when the catalogue is re-indexed; the seed is fixed
 * per (configuration, RNTI, TDA row). The rule is a function of (seed, abs_slot, catalogue key) only; m1 and m2 are
 * sized from the active-set counts (state known before the grant, never an outcome). Every active hypothesis is a member
 * with probability 1 / (m1 m2) on every slot. A hash and not a slot rotation: a rotation (abs_slot mod m) aliases with
 * a periodic TDD pattern (DDDSU: a hypothesis whose turn always falls on a U slot would never be tested), the 64-bit
 * mix of abs_slot does not.
 * Why two levels: a hypothesis whose geometry the grant has not computed yet costs a whole lazy FEP + chest + LLR
 * (~1.6 ms [MEASURED, DGX rfsim, task-GW GWTIM]) before its ~0.36 ms LDPC, and one GrantWork holds at most
 * NR_TD_GW_MAX_SIG signatures; a flat subset of ~25 hypotheses of a blind catalogue would touch ~25 geometries.
 *
 * BUDGET (per grant, CPU): g_target = clamp(floor(0.4 budget / (2 sig_us)), 1, NR_TD_CB0_MAX_GEO) geometries and
 * b_items = max(1, floor((budget - 2 g_target sig_us) / c_item)) items, c_item = us_per_iter * max_iter: a WORST-CASE item
 * (every iteration run, as for a wrong hypothesis). m1 = ceil(n_geo / g_target), m2 = ceil(ceil(n_active / m1) / b_items).
 * us_per_iter is the CPU cost of one LDPC iteration, measured on CPU batches as wall * threads_used / sum(iterations)
 * (EMA): a hardware speed, the ratio of two quantities that early termination moves together, so it does not follow
 * which hypotheses passed (equal per-iteration cost with different pass patterns gives the same estimate: unit test).
 * sig_us is the measured wall of the item-building phase per new signature (lazy computes). GPU batches (iterations not
 * reported) do not update us_per_iter.
 * TOKEN BUCKET. CB0 CPU is capped at cpu_pct % of the host (rate = cpu_pct/100 * ncpu CPU-us per us, capacity
 * 2 * budget_us). A grant whose planned cost (n_sigs * sig_us + n_items * c_item) exceeds the tokens is skipped WHOLE
 * (inadmissible, reason BUDGET), never partially decoded. After a batch the planned cost is refunded and the measured
 * CPU (build wall + batch wall * threads_used) charged. */
#ifndef NR_TD_CB0_SCHED_H
#define NR_TD_CB0_SCHED_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Inadmissibility reasons (bit positions). 0..12 have the meaning (and the position) of the engine's NR_TD_CB0_X_*
 * bits of the ELIM fix round (td/elim-channel); 13.. are runtime-only. */
enum {
  NR_TD_CB0_R_NOT_NEW_RV0 = 0, /* HARQ combining / reserved-MCS record (gw F_HARQ), or rv != 0 */
  NR_TD_CB0_R_GATED = 1,       /* the TB path did not credit this grant (decode error / unsupported / stale ticket) */
  NR_TD_CB0_R_IQ_STALE = 2,    /* IQ lifetime re-check after the decodes failed (nr_passive_credit_allowed) */
  NR_TD_CB0_R_LBRM = 3,        /* a member's LBRM n_L not latched with E > N_ref under another n_L (gw F_LBRM) */
  NR_TD_CB0_R_RV_RETRY = 4,    /* ISAC_RV_RETRY (gw F_RV_RETRY) */
  NR_TD_CB0_R_PRG_PTRS = 5,    /* the main decode ran a PRG / PT-RS arm (gw F_ARM) */
  NR_TD_CB0_R_MEMBER_STALE = 6,/* a member's shared work is STALE / FULL / failed (gw F_STALE / F_FULL, build error) */
  NR_TD_CB0_R_LLR_SCALE = 7,   /* ISAC_LLR_SCALE on */
  NR_TD_CB0_R_GPU_LLR = 8,     /* the grant was fed GPU LLRs (NR_GPU_FEP job) */
  NR_TD_CB0_R_LDPC_ERROR = 9,  /* backend failure (GPU error / timeout / breaker), mixed decoders, no decoder */
  NR_TD_CB0_R_RANK = 10,       /* Nl > ISAC_TD_CB0_RANK_MAX (default 4: K38 fixed, rank > 1 admissible) */
  NR_TD_CB0_R_DECODER = 11,    /* CB0 decoder less sensitive than the grant's TB decoder (CPU CB0, CUDA TB) or unknown */
  NR_TD_CB0_R_CONTRACT = 12,   /* the scheduled hypothesis is active but has no CB0 verdict in the batch */
  NR_TD_CB0_R_BUDGET = 13,     /* token bucket: skipped whole */
  NR_TD_CB0_R_NO_GRANTWORK = 14, /* ISAC_TD_GRANTWORK off (refused, logged once) */
  NR_TD_CB0_R_REINDEXED = 15,  /* the context's catalogue moved between the set and the feed: nothing credited */
  NR_TD_CB0_R_COUNT = 16
};
#define NR_TD_CB0_R_ENGINE_MASK 0x1FFFu /* reasons 0..12 = the engine's NR_TD_CB0_X_* bits */
extern const char *const nr_td_cb0_reason_name[NR_TD_CB0_R_COUNT];

/* Inputs of the per-grant admissibility decision; every one is known before the CB0 outcomes are read (the IQ
 * re-check and the backend status are about the computation, not about any verdict). */
typedef struct {
  bool tb_path_fed;      /* the TB path fed this grant's outcome to the sweep (same gate as the TB) */
  bool iq_ok_after;      /* nr_passive_credit_allowed() re-checked after the main decode AND the batch */
  uint32_t gw_flags;     /* OR of NR_TD_GW_F_* over the grant and every member */
  uint8_t rv;
  bool llr_scale, gpu_llr;
  bool member_error;     /* a member's CB0 input could not be built for a non-parameter reason */
  bool backend_failed;   /* nr_td_cb0_exec_t.failed / mixed, or an item came back ERR_DECODER / ERR_GPU */
  int nl, rank_max;
  uint8_t cb0_decoder;   /* NR_TD_CB0_DEC_* of the batch (0 = none) */
  uint8_t tb_decoder;    /* NRLDPC_DECODER_* of the grant's full decode (same codes: 1 CPU, 2 CUDA; 0 unknown) */
  bool contract;         /* the scheduled hypothesis is active but missing from the decoded batch */
} nr_td_cb0_adm_in_t;
/* Bitmask of NR_TD_CB0_R_* (0 = admissible). Pure. */
uint32_t nr_td_cb0_admissibility(const nr_td_cb0_adm_in_t *in);

/* ---- hypothesis set ---- */
uint64_t nr_td_cb0_mix64(uint64_t x);
/* Context seed: fixed per (configuration, RNTI, TDA row). */
uint64_t nr_td_cb0_ctx_seed(uint64_t configuration, uint16_t rnti, uint8_t tda);
/* Catalogue key: every field of the hypothesis value (independent of its index). */
uint64_t nr_td_cb0_hyp_key(const nr_pdsch_cfg_hypothesis_t *h);
/* Geometry key (what one GrantWork signature shares): S, L, k0, DM-RS mask (+ mapping type when the mask is 0). */
uint64_t nr_td_cb0_geo_key(const nr_pdsch_cfg_hypothesis_t *h);
/* ceil(n / B), >= 1. */
uint32_t nr_td_cb0_subset_m(int n, int B);
/* mix(seed, abs_slot, key) mod m == 0 (one level). */
bool nr_td_cb0_subset_hit(uint64_t seed, int64_t abs_slot, uint64_t key, uint32_t m);
/* Two levels: geometry level (m1) and hypothesis level (m2, independent salt). */
bool nr_td_cb0_subset_hit2(uint64_t seed, int64_t abs_slot, uint64_t gkey, uint32_t m1, uint64_t key, uint32_t m2);
/* sel[] <- positions k in [0, n) with subset_hit2(gkeys[k], keys[k]) or k == forced (-1 = none), ascending; at most
 * max_sel (beyond: dropped by position, the forced one kept). gkeys NULL = one level (m1 ignored). Returns the count. */
int nr_td_cb0_subset_select(uint64_t seed, int64_t abs_slot, const uint64_t *gkeys, const uint64_t *keys, int n, uint32_t m1,
                            uint32_t m2, int forced, int *sel, int max_sel);

/* ---- budget, cost estimate, token bucket ---- */
#define NR_TD_CB0_MAX_GEO 5 /* geometries per grant: <= 3 signatures each (MCS tables) + the main one <= NR_TD_GW_MAX_SIG */
#define NR_TD_CB0_SIGS_PER_GEO 2.0 /* planning: GrantWork signatures per geometry (one per distinct Qm of the 3 MCS tables at the
                                    * grant's MCS index: 1..3, typically 2); admission uses the exact upper bound */
typedef struct {
  double budget_us;   /* per-grant CPU budget (ISAC_TD_CB0_BUDGET_US) */
  double cpu_pct;     /* host CPU share cap (ISAC_TD_CB0_CPU_PCT) */
  int ncpu;           /* online CPUs */
  int max_iter;       /* LDPC iteration policy (8) */
  double us_per_iter; /* CPU-us per LDPC iteration (estimate) */
  double sig_us;      /* CPU-us per lazily computed GrantWork signature (estimate) */
  double tokens_us, cap_us;
  uint64_t last_ns;
  uint64_t updates;   /* estimator updates */
} nr_td_cb0_sched_t;
#define NR_TD_CB0_US_PER_ITER_INIT 45.0 /* [MEASURED, DGX GB10, task-BATCH] ~360 us per wrong CB0 at 8 iterations, 1 thread */
#define NR_TD_CB0_SIG_US_INIT 1600.0    /* [MEASURED, DGX rfsim 106 PRB, task-GW GWTIM] shared FEP+chest+LLR 1558 us */
void nr_td_cb0_sched_init(nr_td_cb0_sched_t *s, double budget_us, double cpu_pct, int ncpu, int max_iter);
/* Worst-case CPU cost of one item, and B = items per grant affordable within the whole budget (>= 1). */
double nr_td_cb0_sched_item_us(const nr_td_cb0_sched_t *s);
int nr_td_cb0_sched_B(const nr_td_cb0_sched_t *s);
typedef struct {
  uint32_t m1, m2;
  int g_target, b_items;
} nr_td_cb0_sizes_t;
/* Subset sizes for n_active hypotheses over n_geo geometries (see BUDGET above). */
void nr_td_cb0_sched_sizes(const nr_td_cb0_sched_t *s, int n_active, int n_geo, nr_td_cb0_sizes_t *z);
/* Refill to now_ns, then reserve n_sigs * sig_us + n_items * item_us. true = admitted (*planned_us set); false = skip
 * the grant whole. */
bool nr_td_cb0_sched_admit(nr_td_cb0_sched_t *s, uint64_t now_ns, int n_items, int n_sigs, double *planned_us);
/* After the grant: refund planned_us, charge build_ns (single thread) + wall_ns * max(1, min(threads, n_items))
 * (threads 0 = a GPU batch: the wall of the one waiting thread); update us_per_iter from a CPU batch with sum_iters > 0
 * and sig_us from a build phase that computed new_sigs > 0 signatures. */
void nr_td_cb0_sched_account(nr_td_cb0_sched_t *s, double planned_us, uint64_t wall_ns, int threads, int n_items,
                             uint32_t sum_iters, uint64_t build_ns, int new_sigs);

#ifdef __cplusplus
}
#endif
#endif
