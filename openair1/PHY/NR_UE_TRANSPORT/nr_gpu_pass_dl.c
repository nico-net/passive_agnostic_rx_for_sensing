/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4, host side of the DL graph pass: see nr_gpu_pass_dl.h and nr_gpu_pass.h. */
#include "nr_gpu_pass_dl.h"
#include "nr_gpu_pass_ul.h"
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_pdsch_gpu_exact.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_ldpc.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_pdsch_cpu_route.h"
#include "nr_llr_norm.h"
#include "nr_pdsch_evm_acc.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "common/utils/LOG/log.h"
#include "nr_gpu_pass_fp32_gate.h"
#include "nr_gpu_graph_pool.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

int nr_get_R_ldpc_decoder(int rvidx, int E, int BG, int Z, int *llrLen, int round);
int nr_get_E(uint32_t G, uint8_t C, uint8_t Qm, uint8_t Nl, uint8_t r);

/* where a pass was not taken (counted; the first few are logged) */
enum { FB_OFF, FB_CELL, FB_FO, FB_CHEST, FB_DEMOD, FB_TBPARAM, FB_MODULE, FB_SEG, FB_N };
static const char *const fb_name[FB_N] = {"off", "cell", "fo", "chest", "demod", "tbparam", "module", "seg"};
static __thread nr_gpu_pass_job_t *jobp_tls;
static struct {
  nr_gpu_pass_dl_try_fn try_;
  nr_gpu_pass_stats_fn stats;
  void (*call_begin)(void), (*call_end)(void), (*call_dirty)(void); /* v2 Task 4: the module's held-instance calls (ISAC_PDSCH_GPU_GRAPH_RETRY) */
  _Atomic int on;
  int nbrx, check, fp32; /* fp32: Task 5, the graphs run the FP32 demod (ISAC_GPU_PASS_FP32 / sm_89) */
  int v2;                /* DL GPU data path v2 (ISAC_PDSCH_GPU_GRAPH=1 without ISAC_GPU_PASS=1): the census is GPUGRAPH, the pool serves late threads */
  _Atomic uint64_t fb[FB_N], ev[NR_GPU_PASS_EV_N], gate[NR_GPU_PASS_GATE_N], runs, shadow_n, shadow_fn;
  _Atomic uint64_t segn[8]; /* segmented grants that reached the pass, by segment-count class 1, 2, 4, 8, 16, 32, 64, 128 */
  _Atomic uint64_t chest_why[16]; /* fallback[chest] by nr_pdsch_gpu_chest_pass_inputs reason: 1 cpu route, 2 exact chest off, 3 gpu delay off / not passive / chest_freq, 4 DM-RS type 2 (without _DMRS2),
                                   * 5 shape (N, nrx, Nl, DM-RS count), 6 buffers, 7 pilot span, 8 diag / delay share / DFT window / dump, 9 single branch, 10 non-contiguous */
} G;

int nr_gpu_pass_active(void) { return atomic_load_explicit(&G.on, memory_order_relaxed); }
int nr_gpu_graph_on(void) { return G.v2 && nr_gpu_pass_active(); }
int nr_gpu_pass_seg_max(void) { return nr_gpu_graph_flag("SEG128") ? 128 : nr_gpu_graph_flag("SPLIT") ? 32 : NR_GPU_PASS_MAX_SEG_FULL; } /* split: the 32-segment class; SEG128: 128 */
/* v2 Task 4: a decode call holds its pass instance across its bodies (RETRY needs SPLIT: the entry at D is a split launch). 1 = held, end it with nr_gpu_graph_call_end */
int nr_gpu_graph_call_begin(void)
{
  if (!G.call_begin || !nr_gpu_graph_flag("RETRY") || !nr_gpu_graph_flag("SPLIT"))
    return 0;
  G.call_begin();
  return 1;
}
void nr_gpu_graph_call_end(void) { G.call_end(); }
void nr_gpu_graph_call_dirty(void)
{
  if (G.call_dirty && nr_gpu_graph_flag("RETRY"))
    G.call_dirty();
}
/* the sub-flags, read once at warm-up (nr_gpu_graph_flag runs per grant) */
static const char *const g_sub_name[] = {"COVER", "SPLIT", "RETRY", "SLOT", "LEAN", "DMRS2", "SEG128"};
static int g_sub_on[sizeof(g_sub_name) / sizeof(g_sub_name[0])];
int nr_gpu_graph_flag(const char *sub)
{
  for (size_t i = 0; i < sizeof(g_sub_name) / sizeof(g_sub_name[0]); i++)
    if (!strcmp(sub, g_sub_name[i]))
      return g_sub_on[i] && nr_gpu_graph_on();
  return 0;
}
static nr_gpu_graph_pool_t g_pool;
static pthread_key_t g_pool_key; /* a claiming thread's set: returned to the pool when the thread exits */
static void pool_release(void *s) { nr_gpu_graph_pool_put(&g_pool, (nr_gpu_graph_set_t *)s); }
int nr_gpu_graph_pool_has_free(void) { return G.v2 && nr_gpu_graph_pool_avail(&g_pool); }
nr_gpu_pass_dl_out_t *nr_gpu_graph_pool_claim(uint8_t **shadow)
{
  nr_gpu_graph_set_t *s = nr_gpu_graph_pool_claim_set(&g_pool, G.v2);
  if (!G.v2)
    return NULL;
  static _Atomic int empty_logged;
  if (s || !atomic_exchange(&empty_logged, 1)) { /* every served claim (at most one per late thread), the first refusal only */
    char tn[32] = "?";
    pthread_getname_np(pthread_self(), tn, sizeof(tn));
    LOG_A(PHY, "SENSING: GPUGRAPH pool claim by thread %s: %s\n", tn, s ? "served" : "EMPTY (first refusal; counted in the census)");
  }
  if (!s)
    return NULL;
  pthread_setspecific(g_pool_key, s);
  nr_gpu_pass_dl_out_t *o = (nr_gpu_pass_dl_out_t *)s->out;
  o->llr = s->llr;
  jobp_tls = (nr_gpu_pass_job_t *)s->job;
  *shadow = s->shadow;
  return o;
}
int nr_gpu_pass_fp32_on(void) { return G.fp32; }

/* Task 5: one FP32-shadowed grant (the old path decoded it too). flip: the CRC verdict (or the decoded-block count) differs; sign_bad: > 0.1 % of the LLR signs
 * differ. A sign failure, or CRC-verdict flips above 1 % of the shadowed grants after NR_FP32_GATE_MIN_N of them, count as a bad check and turn the pass off. 1 = disabled. */
int nr_gpu_pass_fp32_shadow(int flip, int sign_bad)
{
  static nr_fp32_gate_t gate;
  if (flip)
    nr_gpu_pass_note(NR_GPU_PASS_EV_FP32_FLIP);
  const int trip = nr_fp32_gate_note(&gate, flip);
  if (sign_bad || trip) {
    nr_gpu_pass_note(sign_bad ? NR_GPU_PASS_EV_SHADOW_FAIL_BAD : NR_GPU_PASS_EV_SHADOW_BAD);
    nr_gpu_pass_disable(sign_bad ? "FP32 pass LLR signs disagree with the old path (> 0.1 %)" : "FP32 pass CRC verdicts flip against the old path (> 1 %)");
    return 1;
  }
  return 0;
}

void nr_gpu_pass_note(int ev)
{
  if (ev >= 0 && ev < NR_GPU_PASS_EV_N)
    atomic_fetch_add_explicit(&G.ev[ev], 1, memory_order_relaxed);
}

void nr_gpu_pass_disable(const char *why)
{
  if (atomic_exchange(&G.on, 0))
    LOG_E(PHY, "SENSING: %s DISABLED: %s -- the old path decodes from now on\n", G.v2 ? "GPUGRAPH" : "GPUPASS", why);
}

void nr_gpu_pass_gate(int g)
{
  static const char *const gname[] = {"seg", "probe", "share", "gw", "ptrs", "tinterp", "harq", "ldpc_off", "chest_only", "occ", "nomem", "retry"};
  if (g >= 0 && g < NR_GPU_PASS_GATE_N) {
    /* Fix A+D: a first pass that leaves the GPU pass says why (first time per reason here; the census prints every count) */
    if (atomic_fetch_add_explicit(&G.gate[g], 1, memory_order_relaxed) == 0 && g < (int)(sizeof(gname) / sizeof(gname[0])))
      LOG_A(PHY, "SENSING: GPUPASS first grant kept off the pass: gate %s (the old path decodes this grant shape)\n", gname[g]);
  }
}

int nr_gpu_pass_shadow_due_fail(void)
{
  if (G.check <= 0)
    return 0;
  return (atomic_fetch_add_explicit(&G.shadow_fn, 1, memory_order_relaxed) + 1) % (uint64_t)G.check == 0;
}

void nr_gpu_pass_dl_prewarm(nr_gpu_pass_dl_out_t *out)
{
  /* the job (heap, ~50 KB) and the LLR copy buffer (3 MB), touched now so that no page fault or allocation happens on the pass path */
  static __thread int done;
  if (done)
    return;
  nr_gpu_pass_job_t *jp = (nr_gpu_pass_job_t *)aligned_alloc(64, (sizeof(*jp) + 63) & ~(size_t)63);
  int16_t *l = (int16_t *)malloc(1500000 * sizeof(int16_t));
  if (!jp || !l) {
    free(jp);
    free(l);
    return;
  }
  memset(jp, 0, sizeof(*jp));
  memset(l, 0, 1500000 * sizeof(int16_t));
  out->llr = l;
  jobp_tls = jp;
  done = 1;
}

int nr_gpu_pass_shadow_due(void)
{
  if (G.check <= 0)
    return 0;
  const uint64_t n = atomic_fetch_add_explicit(&G.shadow_n, 1, memory_order_relaxed) + 1;
  return n % (uint64_t)G.check == 0;
}

static void census(void)
{
  char buf[1024];
  if (G.stats)
    G.stats(buf, sizeof(buf));
  else
    buf[0] = 0;
  char pool[256] = "";
  if (G.v2)
    snprintf(pool, sizeof(pool), "GPUGRAPH pool[sets=%d claimed=%lu empty=%lu released=%lu] ", g_pool.n, (unsigned long)__atomic_load_n(&g_pool.claimed, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&g_pool.empty, __ATOMIC_RELAXED), (unsigned long)__atomic_load_n(&g_pool.released, __ATOMIC_RELAXED));
  if (G.v2) /* the census of the chest refusals (reasons of nr_pdsch_gpu_chest_pass_inputs) */
    snprintf(pool + strlen(pool), sizeof(pool) - strlen(pool), "chest_why[1..10=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu] ", (unsigned long)G.chest_why[1], (unsigned long)G.chest_why[2],
             (unsigned long)G.chest_why[3], (unsigned long)G.chest_why[4], (unsigned long)G.chest_why[5], (unsigned long)G.chest_why[6], (unsigned long)G.chest_why[7],
             (unsigned long)G.chest_why[8], (unsigned long)G.chest_why[9], (unsigned long)G.chest_why[10]);
  LOG_A(PHY,
        "SENSING: %s%s | host: fallback[off=%lu cell=%lu fo=%lu chest=%lu demod=%lu tbparam=%lu module=%lu seg=%lu] seg_grants[1=%lu 2=%lu 4=%lu 8=%lu 16=%lu 32=%lu 64=%lu 128=%lu] commit=%lu retry_old=%lu zero_tb=%lu lifetime=%lu "
        "shadow=%lu shadow_bad=%lu shadow_fail=%lu shadow_fail_bad=%lu cont_fail=%lu probe_done=%lu old_first=%lu fp32_flip=%lu gate[seg=%lu probe=%lu share=%lu gw=%lu ptrs=%lu tinterp=%lu harq=%lu ldpc_off=%lu chest_only=%lu occ=%lu nomem=%lu retry=%lu]\n",
        pool, buf, (unsigned long)G.fb[FB_OFF], (unsigned long)G.fb[FB_CELL], (unsigned long)G.fb[FB_FO], (unsigned long)G.fb[FB_CHEST],
        (unsigned long)G.fb[FB_DEMOD], (unsigned long)G.fb[FB_TBPARAM], (unsigned long)G.fb[FB_MODULE], (unsigned long)G.fb[FB_SEG],
        (unsigned long)G.segn[0], (unsigned long)G.segn[1], (unsigned long)G.segn[2], (unsigned long)G.segn[3], (unsigned long)G.segn[4], (unsigned long)G.segn[5], (unsigned long)G.segn[6], (unsigned long)G.segn[7], (unsigned long)G.ev[NR_GPU_PASS_EV_COMMIT],
        (unsigned long)G.ev[NR_GPU_PASS_EV_RETRY_OLD], (unsigned long)G.ev[NR_GPU_PASS_EV_ZERO_TB], (unsigned long)G.ev[NR_GPU_PASS_EV_LIFETIME],
        (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW], (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW_BAD], (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW_FAIL], (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW_FAIL_BAD],
        (unsigned long)G.ev[NR_GPU_PASS_EV_CONT_FAIL], (unsigned long)G.ev[NR_GPU_PASS_EV_PROBE], (unsigned long)G.ev[NR_GPU_PASS_EV_OLD_FIRST], (unsigned long)G.ev[NR_GPU_PASS_EV_FP32_FLIP], (unsigned long)G.gate[0], (unsigned long)G.gate[1],
        (unsigned long)G.gate[2], (unsigned long)G.gate[3], (unsigned long)G.gate[4], (unsigned long)G.gate[5], (unsigned long)G.gate[6], (unsigned long)G.gate[7],
        (unsigned long)G.gate[8], (unsigned long)G.gate[9], (unsigned long)G.gate[10], (unsigned long)G.gate[11]);
}

static int fb(int why)
{
  const uint64_t n = atomic_fetch_add_explicit(&G.fb[why], 1, memory_order_relaxed) + 1;
  if (n == 1)
    LOG_A(PHY, "SENSING: GPUPASS first fallback: %s\n", fb_name[why]);
  return 1;
}

/* flags whose host paths the pass does not reproduce: any of them set = the pass stays off for the run (logged) */
static const char *incompatible_env(void)
{
  /* DEFERRED/DO NOT USE: ISAC_CHEST_OCC not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_CHEST_TINTERP not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_DMRS_FO_APPLY not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_PDSCH_EVM not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_RX_IRC not in the frozen config t26zE (2026-10-18); slated for removal */
  /* DEFERRED/DO NOT USE: ISAC_SFO_CORRECT not in the frozen config t26zE (2026-10-18); slated for removal */
  static const char *const nz[] = {"ISAC_PDSCH_EVM", "ISAC_SFO_CORRECT", "ISAC_DMRS_FO_APPLY", "ISAC_CHEST_OCC", "ISAC_RX_IRC", "ISAC_CHEST_TINTERP",
                                   /* DEFERRED/DO NOT USE: ISAC_DC_FIX not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_LLR_SCALE not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_PDSCH_ANT_GATE not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_PDSCH_LLR_DUMP not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_PDSCH_RM_GENIE not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_RX_BRANCH_FO not in the frozen config t26zE (2026-10-18); slated for removal */
                                   "ISAC_DC_FIX", "ISAC_PDSCH_ANT_GATE", "ISAC_LLR_SCALE", "ISAC_PDSCH_RM_GENIE", "ISAC_PDSCH_LLR_DUMP", "ISAC_RX_BRANCH_FO",
                                   /* DEFERRED/DO NOT USE: ISAC_MMSE2_DIAG not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_PDSCH_MMSE_AB not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_PDSCH_OWN_PDCCH_RM not in the frozen config t26zE (2026-10-18); slated for removal */
                                   /* DEFERRED/DO NOT USE: ISAC_TD_TB_CPU_WHILE_ACQ not in the frozen config t26zE (2026-10-18); slated for removal */
                                   "ISAC_PDSCH_MMSE_AB", "ISAC_MMSE2_DIAG", "ISAC_PDSCH_OWN_PDCCH_RM", "ISAC_TD_TB_CPU_WHILE_ACQ"};
  for (size_t i = 0; i < sizeof(nz) / sizeof(nz[0]); i++) {
    const char *e = getenv(nz[i]);
    if (e && atoi(e) != 0)
      return nz[i];
  }
  /* DEFERRED/DO NOT USE: ISAC_LLR_NORM not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_LLR_NORM");
  (void)e;
  return NULL;
}

static int env_list(const char *name, const char *dflt, int *v, int max)
{
  const char *e = getenv(name);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s", e && *e ? e : dflt);
  int n = 0;
  for (char *s = strtok(buf, ","); s && n < max; s = strtok(NULL, ","))
    v[n++] = atoi(s);
  return n;
}

void nr_gpu_pass_warmup(PHY_VARS_NR_UE *ue)
{
  /* DEFERRED/DO NOT USE: ISAC_GPU_PASS not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *e = getenv("ISAC_GPU_PASS");
  const char *eg = (getenv("ISAC_PDSCH_GPU_GRAPH") ?: "1");
  int dl_on = e && atoi(e) == 1;
  const int ul_on = 1; /* Fix A+D: the PUSCH pass (Task 7) is THE UL path (its own instance pool); grants outside its scope fall back, counted */
  if (eg && atoi(eg) == 1) { /* v2: the same module in v2 mode; never together with v1 */
    if (dl_on)
      LOG_W(PHY, "SENSING: GPUGRAPH refused: ISAC_GPU_PASS=1 (v1) is set; v2 off, v1 runs\n");
    else
      dl_on = G.v2 = 1;
    for (size_t i = 0; i < sizeof(g_sub_name) / sizeof(g_sub_name[0]); i++) {
      char n[64];
      snprintf(n, sizeof(n), "ISAC_PDSCH_GPU_GRAPH_%s", g_sub_name[i]);
      /* baked default on: COVER, SPLIT, RETRY, DMRS2 (the others stay off) */
      const char *v = getenv(n) ?: (i == 0 || i == 1 || i == 2 || i == 5 ? "1" : NULL);
      g_sub_on[i] = G.v2 && v && atoi(v) == 1;
    }
    g_sub_on[2] &= g_sub_on[1]; /* review S2: RETRY needs SPLIT (its D entry is a split launch) */
    g_sub_on[5] &= g_sub_on[1]; /* DMRS2 (DM-RS type 2 in the pass) needs SPLIT */
    g_sub_on[6] &= g_sub_on[1]; /* SEG128 (the 64- and 128-segment classes, C in chunks) needs SPLIT */
  }
  if ((!dl_on && !ul_on) || !ue)
    return;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const char *bad = dl_on ? incompatible_env() : NULL;
  if (bad) {
    LOG_W(PHY, "SENSING: GPUPASS requested but %s is set (a host path the pass does not reproduce): off\n", bad);
    if (!ul_on)
      return;
  }
  if (fp->ofdm_symbol_size != 4096 || fp->symbols_per_slot != 14 || fp->nb_antennas_rx < 1 || fp->nb_antennas_rx > 4) {
    LOG_W(PHY, "SENSING: GPUPASS needs N = 4096, 14 symbols, <= 4 RX: off\n");
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  nr_gpu_pass_config_fn cfgf = h ? (nr_gpu_pass_config_fn)dlsym(h, "nr_gpu_pass_config") : NULL;
  nr_gpu_pass_init_fn initf = h ? (nr_gpu_pass_init_fn)dlsym(h, "nr_gpu_pass_init") : NULL;
  nr_gpu_pass_warm_fn warmf = h ? (nr_gpu_pass_warm_fn)dlsym(h, "nr_gpu_pass_warm") : NULL;
  nr_gpu_demod_init_fn dinit = h ? (nr_gpu_demod_init_fn)dlsym(h, "nr_pdsch_gpu_demod_init") : NULL;
  nr_gpu_ldpc_init_fn linit = h ? (nr_gpu_ldpc_init_fn)dlsym(h, "nr_pdsch_gpu_ldpc_init") : NULL;
  G.try_ = h ? (nr_gpu_pass_dl_try_fn)dlsym(h, "nr_gpu_pass_dl_try") : NULL;
  { int (*fp32f)(void) = h ? (int (*)(void))dlsym(h, "nr_gpu_pass_fp32_default") : NULL; G.fp32 = fp32f ? fp32f() : 0; } /* before initf: the default classes read the same */
  G.stats = h ? (nr_gpu_pass_stats_fn)dlsym(h, "nr_gpu_pass_stats") : NULL;
  G.call_begin = h ? (void (*)(void))dlsym(h, "nr_gpu_pass_call_begin") : NULL;
  G.call_end = h ? (void (*)(void))dlsym(h, "nr_gpu_pass_call_end") : NULL;
  G.call_dirty = h ? (void (*)(void))dlsym(h, "nr_gpu_pass_call_dirty") : NULL;
  if (!cfgf || !initf || !warmf || !dinit || !linit || !G.try_ || dinit(fp->nb_antennas_rx) != 0 || linit() != 0) {
    LOG_W(PHY, "SENSING: GPUPASS requested but libpdsch_gpu.so has no usable graph pass: off\n");
    return;
  }
  nr_gpu_pass_cfg_t c = {.nbRx = fp->nb_antennas_rx, .n_rb_dl = fp->N_RB_DL, .delay_table = (const int16_t *)fp->delay_table[0],
                         .dt_stride = NR_MAX_OFDM_SYMBOL_SIZE, .max_delay_comp = MAX_DELAY_COMP, .v2 = G.v2, .split = G.v2 && g_sub_on[1],
                         /* DEFERRED/DO NOT USE: ISAC_PDSCH_GPU_GRAPH_MB not in the frozen config t26zE (2026-10-18); slated for removal */
                         .split_mb = 0 /* default cap, nr_gpu_pass_graph.cu */};
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE) { /* the graphs read the delay table on the device */
    static nr_gpu_buf_t dt;
    const size_t bytes = sizeof(fp->delay_table);
    if (nr_gpu_buf_alloc(&dt, bytes) != 0) {
      LOG_W(PHY, "SENSING: GPUPASS: no device copy of the delay table: off\n");
      return;
    }
    memcpy(dt.host, fp->delay_table, bytes);
    nr_gpu_buf_h2d_async(&dt, 0, bytes, NULL);
    c.delay_table = (const int16_t *)dt.dev;
  }
  if (cfgf(&c) != 0 || initf(0) != 0) {
    LOG_W(PHY, "SENSING: GPUPASS: nr_gpu_pass_config/init failed: off\n");
    return;
  }
  int (*split_on)(void) = (int (*)(void))dlsym(h, "nr_gpu_pass_split_on");
  if (c.split && split_on && !split_on()) { /* review S5: the instances alone reach the memory cap */
    LOG_E(PHY, "SENSING: GPUGRAPH: the pass instances alone reach ISAC_PDSCH_GPU_GRAPH_MB=%d: ISAC_PDSCH_GPU_GRAPH_SPLIT (and RETRY) off\n", c.split_mb);
    c.split = 0;
    g_sub_on[1] = g_sub_on[2] = g_sub_on[5] = g_sub_on[6] = 0;
  }
  if (!dl_on || bad) { /* the PUSCH pass alone */
    nr_gpu_pass_ul_warm(h, fp->nb_antennas_rx, G.fp32);
    return;
  }
  /* the class list: PRB classes x layers x the code block counts that can occur x DM-RS symbol counts, in the order of expected use, up to ISAC_GPU_PASS_CLASSES */
  int nd[4], nnd = env_list("ISAC_GPU_PASS_NDMRS", "3,2", nd, 4);
  /* DEFERRED/DO NOT USE: ISAC_GPU_PASS_CLASSES not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *ce = getenv("ISAC_GPU_PASS_CLASSES");
  const int budget = ce ? atoi(ce) : c.split ? 1 << 20 : 512; /* split: every class of the loops (sub-graphs are shared between classes; the module's memory cap bounds them) */
  /* DEFERRED/DO NOT USE: ISAC_PDSCH_DECODE_DIAG not in the frozen config t26zE (2026-10-18); slated for removal */
  const int evm = ((getenv("ISAC_PDSCH_EVM_GRANT") ?: "1") || (getenv("ISAC_PDSCH_SNR_GATE") ?: "1") || (getenv("ISAC_PDSCH_DECODE_DIAG") && atoi(getenv("ISAC_PDSCH_DECODE_DIAG")) != 0)) ? 1 : 0;
  static const int nrbs[] = {273, 217, 136, 96, 48, 24, 12, 6};
  static const int nls[] = {1, 2, 4, 3};
  int made = 0, failed = 0;
  /* split: contiguous classes also with 1 DM-RS symbol (only C keys are added; tim replay: 224-260 unseen grants per run had ndmrs 1) */
  int ndc[4], nndc = env_list("ISAC_GPU_PASS_NDMRS", c.split ? "3,2,1" : "3,2", ndc, 4);
  for (int di = 0; di < nndc; di++)
    for (int ni = 0; ni < 8 && made < budget; ni++)
      for (int li = 0; li < 4 && made < budget; li++) {
        const int nl = nls[li];
        if (nl > fp->nb_antennas_rx)
          continue;
        /* the demod variants: QPSK and higher can resolve different MMSE modes for 3 and 4 layers */
        int variants[2][2], nvar = 0;
        for (int qm = 2; qm <= 6; qm += 4) {
          int qmode = 0, align = 0;
          if (nr_pdsch_gpu_demod_pass_variant(ue, nl, qm, &qmode, &align) != 0)
            continue;
          int dup = 0;
          for (int v = 0; v < nvar; v++)
            dup |= variants[v][0] == qmode && variants[v][1] == align;
          if (!dup) {
            variants[nvar][0] = qmode;
            variants[nvar][1] = align;
            nvar++;
          }
        }
        /* code blocks per pass: from 1 to the most the grant can carry (256QAM, rate 0.93) */
        const int cmax = nrbs[ni] * nl * 132 * 8 * 93 / 100 / 8400 + 1;
        for (int ncb = 1; ncb <= 32 && made < budget; ncb <<= 1) {
          if (ncb / 2 >= cmax)
            break;
          for (int v = 0; v < nvar && made < budget; v++) {
            const nr_gpu_pass_shape_t sh = {(uint16_t)nr_gpu_pass_nrb_class(nrbs[ni]), 13, (uint16_t)nl, 6, (uint16_t)ndc[di], (uint16_t)ncb, (uint8_t)(1 | (G.fp32 ? NR_GPU_PASS_FP32_DEMOD : 0))};
            if (warmf(&sh, variants[v][0], variants[v][1], evm) == 0)
              made++;
            else
              failed++;
          }
        }
      }
  /* segmented (PRG / VRB-bundle) classes: segment-count classes 1, 2, 4, 8, 16 (32: split) on top of the contiguous ones. Own budget
   * (ISAC_GPU_PASS_SEG_CLASSES, default 1400): the passes with the widest grants are warmed first; a segmented grant of an unwarmed class stays on the old path (unseen). */
  /* DEFERRED/DO NOT USE: ISAC_GPU_PASS_SEG_CLASSES not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *se = getenv("ISAC_GPU_PASS_SEG_CLASSES");
  const int sbudget = se ? atoi(se) : c.split ? 1 << 20 : 1400;
  int smade = 0;
  static const int segc[] = {1, 2, 4, 8, 16, 32, 64, 128}, segnl[] = {1, 2, 3, 4};
  const int nsegc = c.split ? (g_sub_on[6] ? 8 : 6) : 5, segrows = c.split ? 1024 : 512; /* split: the 32-segment class and the module's 1024 chest rows; SEG128: 64, 128 in C chunks */
  if (G.stats) {
    char buf[1024];
    G.stats(buf, sizeof(buf));
    LOG_A(PHY, "SENSING: %s (contiguous classes only)\n", buf);
  }
  for (int li = 0; li < 4; li++) /* the layers that occur most first, then the DM-RS counts, the PRB classes and the segment classes */
    for (int di = 0; di < nnd; di++)
      for (int ni = 0; ni < 8 && smade < sbudget; ni++)
        for (int sc = 0; sc < nsegc && smade < sbudget; sc++) {
          const int nl = segnl[li], ns = segc[sc];
          const int rows = nd[di] * nl * fp->nb_antennas_rx * ns; /* SEG128 (ns > 32): chunks of whole (DM-RS symbol, layer) groups, at most 2048 rows (the module's c_groups_per_chunk) */
          if (nl > fp->nb_antennas_rx || ns > nrbs[ni] || (ns <= 32 ? rows > segrows : rows > 2048 || ns * fp->nb_antennas_rx > segrows))
            continue;
          int variants[2][2], nvar = 0;
          for (int qm = 2; qm <= 6; qm += 4) {
            int qmode = 0, align = 0;
            if (nr_pdsch_gpu_demod_pass_variant(ue, nl, qm, &qmode, &align) != 0)
              continue;
            int dup = 0;
            for (int v = 0; v < nvar; v++)
              dup |= variants[v][0] == qmode && variants[v][1] == align;
            if (!dup) {
              variants[nvar][0] = qmode;
              variants[nvar][1] = align;
              nvar++;
            }
          }
          const int cmax = nrbs[ni] * nl * 132 * 8 * 93 / 100 / 8400 + 1;
          for (int ncb = 1; ncb <= 32 && smade < sbudget; ncb <<= 1) {
            if (ncb / 2 >= cmax)
              break;
            for (int v = 0; v < nvar && smade < sbudget; v++) {
              nr_gpu_pass_shape_t sh = {(uint16_t)nr_gpu_pass_nrb_class(nrbs[ni]), 13, (uint16_t)nl, 6, (uint16_t)nd[di], (uint16_t)ncb, (uint8_t)(1 | (G.fp32 ? NR_GPU_PASS_FP32_DEMOD : 0)), (uint16_t)ns};
              if (warmf(&sh, variants[v][0], variants[v][1], evm) == 0)
                smade++;
              else
                failed++;
            }
          }
        }
  G.nbrx = fp->nb_antennas_rx;
  G.check = 0; /* Fix A+D: no shadow decode on the hot path; the pass-vs-reference comparison lives in the unit tests */
  if (G.v2 && made > 0 && g_pool.n == 0 && pthread_key_create(&g_pool_key, pool_release) == 0) /* the per-thread host sets for threads that reach the pass after the RX fence (the async recovery worker); once */
    LOG_A(PHY, "SENSING: GPUGRAPH v2 mode: %d of %d prewarmed host buffer sets for late threads\n",
          nr_gpu_graph_pool_fill(&g_pool, NR_GPU_GRAPH_POOL, sizeof(nr_gpu_pass_dl_out_t), sizeof(nr_gpu_pass_job_t)), NR_GPU_GRAPH_POOL);
  atomic_store(&G.on, made > 0);
  LOG_A(PHY, "SENSING: GPUPASS %s: %d shape classes (+ %d segmented) x %d instances captured (EVM branch %d, %d failed), shadow check every %d passes, %s memory model, demod %s\n",
        made > 0 ? "enabled" : "NOT enabled", made, smade, NR_GPU_PASS_RING, evm, failed, G.check, nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE", G.fp32 ? "FP32 (sign/CRC shadow checks)" : "FP64 (bit-exact)");
  if (G.stats) {
    char buf[1024];
    G.stats(buf, sizeof(buf));
    LOG_A(PHY, "SENSING: %s\n", buf);
  }
  if (ul_on)
    nr_gpu_pass_ul_warm(h, fp->nb_antennas_rx, G.fp32);
}

int nr_gpu_pass_dl_run(const nr_gpu_pass_dl_in_t *in, nr_gpu_pass_dl_out_t *out)
{
  if (!nr_gpu_pass_active())
    return fb(FB_OFF);
  {
    static _Atomic uint64_t calls;
    const uint64_t c = atomic_fetch_add(&calls, 1) + 1;
    if (c == 100 || (c % 2048) == 0)
      census();
  }
  const uint64_t t_entry = nr_gpu_pass_now_ns();
  PHY_VARS_NR_UE *ue = in->ue;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const fapi_nr_dl_cw_info_t *cw = in->cw;
  if (fp->nb_antennas_rx != G.nbrx || fp->ofdm_symbol_size != 4096 || fp->symbols_per_slot != 14 || cw->Nl > fp->nb_antennas_rx)
    return fb(FB_CELL);
  /* the exact FEP's own refusals: a frequency offset in the FEP, an unsynchronised UE, a per-branch offset */
  bool fo = ue->cont_fo_comp != 0 || !ue->is_synchronized || in->fep_fo != 0.0;
  for (int a = 0; a < fp->nb_antennas_rx && !fo; a++)
    fo = nr_ue_get_branch_fo_hz(a) != 0.0;
  if (fo || nr_pdsch_route_cpu())
    return fb(FB_FO);
  nr_gpu_pass_job_t *j = jobp_tls; /* allocated by nr_gpu_pass_dl_prewarm at consumer start */
  if (!j || !out->llr)
    return fb(FB_MODULE);
  memset(j, 0, offsetof(nr_gpu_pass_job_t, dj));
  j->t_entry_ns = t_entry;
  /* ---- channel estimate inputs. A segmented grant (PRG / VRB bundles): the chest runs per segment (classes of 1, 2, 4, 8, 16 segments; 32 in split mode) and the demod reads the
   * estimate in data order against the virtual contiguous allocation the old path gathers; here the gather is the demod geometry's RX index map. */
  const int nseg = in->seg ? in->nseg : 0;
  const int nsegc = nseg > 0 ? nr_gpu_pass_nseg_class(nseg) : 0;
  if (nseg > 0) {
    if (!nsegc || nseg > nr_gpu_pass_seg_max())
      return fb(FB_SEG);
    atomic_fetch_add_explicit(&G.segn[__builtin_ctz((unsigned)nsegc)], 1, memory_order_relaxed);
  }
  int ndmrs = 0, k0 = 0, nrb = 0, pil_nrb = 0, dtype = 1;
  const int wide = nseg == 0 && in->chest_fa && in->chest_cfg; /* v2 Task 2: the slot share's widened estimate band */
  const int chest_rc = nr_pdsch_gpu_chest_pass_inputs(ue, in->proc, wide ? in->chest_cfg : in->dlsch_config, wide ? in->chest_fa : in->freq_alloc, cw->Nl, in->dmrs_full, in->chest_diag, &ndmrs,
                                                      j->dmrs_sym, j->delta, j->pil, &k0, &nrb, nseg > 0 ? in->seg : NULL, nseg, &pil_nrb,
                                                      nr_gpu_graph_flag("DMRS2"), &dtype);
  if (chest_rc != 0) {
    atomic_fetch_add_explicit(&G.chest_why[chest_rc & 15], 1, memory_order_relaxed);
    return fb(FB_CHEST);
  }
  j->nbRx = fp->nb_antennas_rx;
  j->dmrs_type = (uint8_t)dtype;
  j->nl = cw->Nl;
  j->ndmrs = ndmrs;
  j->nrb = nrb;
  j->k0 = k0;
  if (nseg > 0) {
    j->seg_pil_nrb = pil_nrb;
    const int tot = nr_gpu_pass_seg_fill(j, in->seg, nseg, in->dlsch_config->BWPStart, in->dlsch_config->refPoint, fp->first_carrier_offset, fp->ofdm_symbol_size);
    if (tot != nrb || tot != in->freq_alloc->num_rbs)
      return fb(FB_SEG);
  }
  /* ---- demod job (the old GPU demod's own refusals and geometry) */
  NR_UE_DLSCH_t dlsch = {0};
  dlsch.cw_info = *cw;
  dlsch.rnti = in->rnti;
  dlsch.rnti_type = TYPE_C_RNTI_;
  dlsch.active = true;
  dlsch.max_ldpc_iterations = 8;
  NR_DL_UE_HARQ_t harq = {0};
  harq.status = NR_ACTIVE;
  harq.first_rx = 1;
  pdsch_scope_req_t scope = {.copy_chanest_to_scope = false, .copy_rxdataF_to_scope = false, .scope_rxdataF_offset = 0};
  const freq_alloc_bitmap_t fa_virt = set_bitmap_from_start_size(0, in->freq_alloc->num_rbs); /* the old path demodulates this virtual allocation of the gathered REs */
  nr_pdsch_gpu_demod_call_t call = {.ue = ue, .proc = in->proc, .dlsch = &dlsch, .freq_alloc = nseg > 0 ? &fa_virt : in->freq_alloc, .cfg = in->dlsch_config, .harq = &harq,
                                    .first_symbol = in->first_symbol, .probe_last_sym = in->probe ? in->probe_last_sym : -1, .est_size = in->est_size, .buf = in->rx_size_symbol,
                                    .rxdataF = (c16_t *)in->rxdataF, .scope = &scope, .ssb_rm = in->ssb_rm};
  if (nr_pdsch_gpu_demod_pass_job(&call, &j->dj, &j->geo_y, &j->geo_h) != 0)
    return fb(FB_DEMOD);
  j->llr_out_all = G.fp32; /* FP32: the shadow check of a CRC-OK pass compares LLR signs too */
  j->dj.demod_fp32 = G.fp32; /* the old GPU demod stays double; only the graph pass runs the float kernels */
  if (nr_pdsch_evm_acc.armed && !j->dj.want_evm) /* the EVM would need the CPU copy of the equaliser output */
    return fb(FB_DEMOD);
  if (nseg > 0) /* the virtual allocation's index map composed with the old path's RE gather (nr_gpu_pass.h); this thread's maps, rebuilt for every grant */
    if (nr_gpu_pass_seg_remap_geo((int16_t *)j->geo_y, (int)j->dj.ext, j->dj.sym_mask, j->dj.nre, fp->ofdm_symbol_size,
                                  (fp->first_carrier_offset + in->dlsch_config->BWPStart * NR_NB_SC_PER_RB) % fp->ofdm_symbol_size, in->seg, nseg) != 0)
      return fb(FB_SEG);
  uint32_t G_dem = 0;
  for (int m = 0; m < 14; m++)
    if (j->dj.sym_mask >> m & 1)
      G_dem += (uint32_t)j->dj.nre[m] * cw->qamModOrder * cw->Nl;
  /* ---- transport block parameters, as passive_ldpc_decode_core derives them (no HARQ combining: first transmission, d cleared) */
  unsigned C = 0, K = 0, Z = 0, F = 0;
  if (nr_segmentation(NULL, NULL, lenWithCrc(1, cw->TBS), &C, &K, &Z, &F, cw->ldpcBaseGraph) < 0 || C < 1 || C > NR_GPU_PASS_MAX_CB || (Z & 3) || (K & 7) || (K >> 3) > 1056
      || (in->probe ? G_dem > in->G || G_dem == 0 : G_dem != in->G) || in->G == 0) /* a probe past its horizon demodulates fewer LLRs; the rest stay 0 */
    return fb(FB_TBPARAM);
  const uint32_t n_dec = in->probe && C > 1 ? 1u : C; /* a probe decodes code block 0 (nb_segments_to_decode = 1 of passive_ldpc_decode_core) */
  const int ncb = nr_gpu_pass_ncb_class((int)n_dec);
  const int nrbc = nr_gpu_pass_nrb_class(nrb);
  if (!ncb || !nrbc || in->it_max < 1 || in->it_max > 17)
    return fb(FB_TBPARAM);
  const uint32_t E = (uint32_t)nr_get_E(in->G, (uint8_t)C, cw->qamModOrder, cw->Nl, 0);
  int llrLen = 0;
  uint32_t E2 = E, first = C;
  uint8_t R = (uint8_t)nr_get_R_ldpc_decoder(cw->rv, (int)E, cw->ldpcBaseGraph, (int)Z, &llrLen, 0), R2 = R;
  for (uint32_t r = 1; r < C; r++) {
    const int Er = nr_get_E(in->G, (uint8_t)C, cw->qamModOrder, cw->Nl, (uint8_t)r);
    if ((uint32_t)Er != E) {
      E2 = (uint32_t)Er;
      R2 = (uint8_t)nr_get_R_ldpc_decoder(cw->rv, Er, cw->ldpcBaseGraph, (int)Z, &llrLen, 0);
      first = r;
      break;
    }
  }
  j->A = cw->TBS;
  j->C = C; j->K = K; j->Z = Z; j->F = F; j->E = E; j->E2 = E2; j->first_rE2 = first; j->tbslbrm = in->tbslbrm;
  j->BG = cw->ldpcBaseGraph; j->R = R; j->R2 = R2; j->rv = cw->rv; j->Qm = cw->qamModOrder; j->it_max = (uint8_t)in->it_max;
  j->n_decode = n_dec;
  j->G_dem = G_dem;
  j->probe = (uint8_t)(in->probe != 0);
  j->gate_on = (uint8_t)(in->gate_on != 0);
  j->gate_thr_db = in->gate_thr_db;
  /* ---- LLR post: descrambling words (the gold sequence nr_codeword_unscrambling reads) and the K38 normalisation */
  const int words = (int)((in->G + 31) / 32);
  j->scr = gold_cache(((uint32_t)in->rnti << 15) + (0u << 14) + in->dlsch_config->dlDataScramblingId, words);
  j->G = in->G;
  j->norm = in->norm && in->G >= 64;
  j->norm_span = nr_llr_norm_span(in->G, nr_llr_norm_num_cb(cw->TBS, cw->ldpcBaseGraph));
  /* ---- front end */
  c16_t **iq = nr_pdsch_passive_iq_for_slot(in->proc->nr_slot_rx, ue->common_vars.rxdata);
  for (int a = 0; a < fp->nb_antennas_rx; a++)
    j->rxdata[a] = (const int16_t *)iq[a];
  j->ring_len = 2u * fp->samples_per_frame;
  j->ring_offset = get_samples_slot_timestamp(fp, in->proc->nr_slot_rx);
  j->symbol_rot = (const int16_t *)(fp->symbol_rotation[link_type_dl] + (in->proc->nr_slot_rx % fp->slots_per_subframe) * fp->symbols_per_slot);
  j->rxdataF = (int16_t *)in->rxdataF;
  j->rx_stride = in->rx_stride;
  j->copy_mask = 0x3fff;
  /* ---- chest post: nvar normalisation, per-branch substitution, DM-RS FO/SFO sums */
  j->number_symbols = in->dlsch_config->number_symbols;
  j->nvfix = in->nvfix;
  j->branch = in->branch;
  j->fo_first = ndmrs > 1 ? 0 : -1;
  j->fo_last = ndmrs > 1 ? ndmrs - 1 : -1;
  j->dt_d = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot * (double)(j->dmrs_sym[ndmrs - 1] - j->dmrs_sym[0]);
  j->df_d = (double)fp->subcarrier_spacing;
  j->shape = (nr_gpu_pass_shape_t){(uint16_t)nrbc, (uint16_t)in->dlsch_config->number_symbols, (uint16_t)cw->Nl, (uint16_t)cw->qamModOrder, (uint16_t)ndmrs,
                                   (uint16_t)ncb, (uint8_t)((j->dj.evm_fp64 ? 0 : 1) | (j->dj.demod_fp32 ? NR_GPU_PASS_FP32_DEMOD : 0)), (uint16_t)nsegc};
  /* ---- the pass */
  memset(&out->r, 0, sizeof(out->r));
  out->r.tb = in->tb;
  out->r.demod_out = &out->demod;
  out->r.brfo_rows = out->brfo_rows;
  int qm_sym = -1;
  uint32_t qm_n = 0;
  for (int m = in->dlsch_config->start_symbol; m < in->dlsch_config->start_symbol + in->dlsch_config->number_symbols; m++)
    if (j->dj.nre[m] > qm_n) {
      qm_n = j->dj.nre[m];
      qm_sym = m;
    }
  j->qm_sym = qm_sym;
  j->qm_n = qm_n > 4096 ? 4096 : qm_n;
  out->r.qm_row = out->qm_row;
  j->llr_out = out->llr;
  atomic_fetch_add_explicit(&G.runs, 1, memory_order_relaxed);
  const int rc = G.try_(j, &out->r);
  if (rc != 0) /* unseen class / no free instance / unsupported variant / error: counted by the module */
    return fb(FB_MODULE);
  for (int m = 0; m < 14; m++)
    out->dl_valid_re[m] = j->dj.sym_mask >> m & 1 ? j->dj.nre[m] : 0;
  out->qm_sym = qm_sym;
  out->qm_n = j->qm_n;
  out->ndmrs = ndmrs;
  out->dmrs_first = j->dmrs_sym[0];
  out->dmrs_last = j->dmrs_sym[ndmrs - 1];
  out->C = (int)C;
  out->K = K; out->F = F; out->Z = Z; out->E = E; out->R = R; out->BG = cw->ldpcBaseGraph;
  memcpy(out->nvar_ant, out->r.nvar_ant, sizeof(out->nvar_ant));
  {
    const uint64_t n = G.runs;
    if (n == 1 || n == 64 || (n & 1023) == 0)
      census();
  }
  return 0;
}
