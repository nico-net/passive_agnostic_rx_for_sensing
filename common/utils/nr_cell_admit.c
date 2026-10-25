/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Cell admission model (B3). See nr_cell_admit.h for the policy. Every coefficient below carries its source:
 *   [CODE file:line]   the sizing expression or constant of the receiver, read from the tree at mpci-main @9b2d7c3f6e
 *   [MEASURED ...]     a census / measurement quoted in the named report
 *   [HYPOTHESIS ...]   a model choice with no measurement yet; the named task replaces it
 * The traceability table of every coefficient is docs/superpowers/reports/csd/MPCI-B3-20261025.md. */
#include "nr_cell_admit.h"
#include <stdio.h>
#include <string.h>
#include "PHY/TOOLS/nr_host_membudget.h"
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc_mem.h"

#define MIB ((size_t)1 << 20)
/* ---- host coefficients ---- */
#define H_CSIRS_CAP 640         /* [CODE nr_csirs_disc.h:18 NR_CSIRS_DISC_DEFAULT_CAPACITY] */
#define H_CSIRS_FLOOR 160       /* [CODE nr_csirs_disc_feed.c:507 lo = min(capacity,160)] */
#define H_CSIRS_PCT 30          /* [CODE nr_csirs_disc_feed.c:508] */
#define H_NSYM 14               /* [CODE nr_csirs_disc.h:21 NR_CSIRS_DISC_NSYM] */
#define H_TD_CAP 1024           /* [CODE nr_csirs_disc_feed.c:530 td_capacity] */
#define H_TD_FLOOR 256          /* [CODE nr_csirs_disc_feed.c:536] */
#define H_TD_PCT 60             /* [CODE nr_csirs_disc_feed.c:536] */
#define H_TD_BATCH 100          /* [CODE nr_csirs_disc.h:19 NR_CSIRS_DISC_DEFAULT_BATCH] */
#define H_SNAP_N 8              /* [CODE nr_pdsch_passive_queue.c:2358 g_rsnap_n = 8] */
#define H_SNAP_FLOOR 2          /* [CODE nr_pdsch_passive_queue.c:2359] */
#define H_SNAP_PCT 10           /* [CODE nr_pdsch_passive_queue.c:2359] */
#define H_GW_MAX_SIG 16         /* [CODE nr_td_grantwork.h:36 NR_TD_GW_MAX_SIG] */
#define H_GW_HDR 64             /* [CODE nr_td_grantwork.c:17 GW_HDR] */
#define H_GW_CLASSES 24         /* [CODE nr_td_grantwork.c:19 GW_POOL_CLASSES] */
#define H_GW_FLOOR_SETS 8       /* [CODE nr_pdcch_blind_monitor_rt.c:9971 floor_n = consumers * 8] */
#define H_GW_WEIGHT 60          /* [CODE nr_host_membudget.c:13 host_weight[GW_POOL] = 60] */
/* ---- GPU coefficients (MiB) ---- */
#define G_LDPC_POOL_MIB 620     /* [MEASURED, sens6 RTX 4060 Ti census at 273 PRB 4 RX, nr_gpu_membudget.h:12 comment: ldpc pool 620] */
#define G_LDPC_CB0_MIB 874      /* [same census: ldpc cb0 874] */
#define G_TD_CB0_MIB 556        /* [same census: td_cb0 556] */
#define G_PASS_CENSUS_MIB 361   /* [same census: pass 361 = instances + one HARQ store] */
#define G_HARQ_BYTES ((size_t)2048 * 66 * 384 * 2) /* [CODE nr_gpu_pass.h:312-313: NR_GPU_HQ_BLK x NR_GPU_HQ_SEG x int16 = 99 MiB; SENS6-READINESS-20261021 s3] */
#define G_CSIRS_LOCAL_MIB 512   /* [CODE nr_csirs_disc_mem.h:30 local memory of refine_kernel, in nr_csirs_disc_mem_fixed()]: one set of kernels per process */
#define G_NID_CENSUS_MIB 776    /* [MEASURED, same census: pdcch_nid 776 at 273 PRB, window 10] */
#define G_NID_BATCH 336         /* [CODE nr_pdcch_nid_gpu.h:10 NR_PDCCH_NID_BATCH] */
#define G_NID_WIN_FULL 10       /* [CODE nr_gpu_elastic.h comment "6 of 10"; census window] */
#define G_NID_WIN_FLOOR 5       /* [CODE nr_pdcch_nid_gpu.cu:595 lo = min(window,5)] */
#define G_NID_DUR 3             /* [HYPOTHESIS, G1: CORESET duration of the nID window = the maximum, 3 symbols (nr_pdcch_nid_gpu.cu:578 window_bytes dur)] */
#define G_NID_STAT ((size_t)6 * 14 * 65536 * 6) /* [CODE nr_pdcch_nid_gpu.cu:21-23 STAT = 6 x CELLS, CELLS = 14 x IDS, IDS = 65536] x (2 + 4) B per window batch */
#define G_GRAPH_FLOOR_MIB 512   /* [HYPOTHESIS, G1: graph executables floor; SENS6-READINESS s3 proposes "floor 1 GB" for pool+graph; the common classes warm first] */
#define G_GRAPH_CAP_MIN_MIB 2048 /* [CODE nr_gpu_pass_graph.cu:1066-1071 split_cap_auto clamp [2048, 3072] MB, quarter of the device memory] */
#define G_GRAPH_CAP_MAX_MIB 3072

_Static_assert(NR_ADMIT_PLAN_MAX >= NR_CELL_MAX + 1, "plan holds an over-full inventory");

/* ---- helpers ---- */
static size_t sat_sub(size_t a, size_t b) { return a == SIZE_MAX ? a : (b >= a ? 0 : a - b); }
static size_t min_z(size_t a, size_t b) { return a < b ? a : b; }
/* share * w / W without losing the whole when w == W (one cell: exact). */
static size_t split(size_t x, int w, int W)
{
  if (x == SIZE_MAX) return SIZE_MAX;
  return w == W ? x : x / (size_t)W * (size_t)w;
}

typedef struct { int n_rb, scs, rx, mu, fft; size_t fs, spf, rxdf, stride; int shifted; } geom_t;
/* Largest grid per SCS (TS 38.101-1/-2 maximum bandwidth) for an unknown n_rb. [SPEC, not in the repo] */
static int max_grid(int scs) { return scs == 15 ? 270 : scs == 60 ? 135 : scs == 120 ? 264 : 273; }
static void geom_of(const nr_cell_desc_t *c, geom_t *g)
{
  g->scs = c->scs_khz > 0 ? c->scs_khz : 30;
  g->n_rb = c->n_rb > 0 ? c->n_rb : max_grid(g->scs);
  g->rx = c->n_rx > 0 ? (c->n_rx > 4 ? 4 : c->n_rx) : 4;
  g->mu = g->scs == 15 ? 0 : g->scs == 30 ? 1 : g->scs == 60 ? 2 : 3;
  /* FFT size: the smallest power of two above the occupied subcarriers; reproduces get_samplerate_and_bw's table at mu=1 for 51, 106, 133, 162 (30.72/61.44 MS/s), 189, 217, 273
   * (122.88 MS/s) [CODE common/utils/nr/nr_common.c:716 get_samplerate_and_bw; ref/mpci-old mpci_auto.py RATE_OF]. */
  int f = 128;
  while (f <= g->n_rb * 12) f <<= 1;
  g->fft = f;
  g->fs = (size_t)f * (size_t)g->scs * 1000;
  g->spf = g->fs / 100;                       /* samples_per_frame = 10 x samples_per_subframe [CODE nr_parms.c:318] */
  g->rxdf = (size_t)14 * f;                   /* samples_per_slot_wCP = symbols_per_slot x ofdm_symbol_size [CODE nr_parms.c:311] */
  g->stride = (size_t)14 * ((size_t)f + (size_t)(f / 128 * 9) + (size_t)(f / 128 * (9 + (1 << g->mu)))); /* [CODE nr_csirs_disc_feed.c:529 / nr_csirs_disc_td.c:85: 14 x (fft + prefix0 + prefix)] */
  g->shifted = !(c->window_centre_hz != 0 && c->centre_hz == c->window_centre_hz);
}

/* ---- host module sizes (the receiver's own expressions) ---- */
static size_t h_iq_set(const geom_t *g) { return 2 * g->spf * sizeof(int16_t) * 2 * (size_t)g->rx; }           /* [CODE nr_pdsch_passive_queue.c:2353 iq_set = 2 x samples_per_frame x sizeof(c16) x nb_antennas_rx] */
static size_t h_queue(const geom_t *g, const nr_host_caps_t *k)                                                /* [CODE nr_pdsch_passive_queue.c:2355] */
{
  const size_t cons = k->consumers < 1 ? 1 : (size_t)k->consumers;
  return cons * ((size_t)g->rx * g->rxdf * 4 + (k->csirs_enabled ? h_iq_set(g) : 0));
}
static size_t h_csirs_slot(const geom_t *g) { return (size_t)g->rx * H_NSYM * 24 * (size_t)g->n_rb * sizeof(int16_t); } /* [CODE nr_csirs_disc_feed.c:504, TD front end: no snapshot ring] */
static size_t h_td_bytes(const geom_t *g, int cap)                                                             /* [CODE nr_csirs_disc_td.c:82-88 nr_csirs_disc_td_bytes] */
{
  const size_t slot = ((g->stride * (size_t)g->rx * 4 + 63) / 64) * 64, huge = 2 * MIB;
  const size_t pool = ((slot * (size_t)(cap + H_TD_BATCH) + huge - 1) / huge) * huge;
  return pool + 2 * (size_t)H_TD_BATCH * (size_t)g->rx * 14 * (size_t)g->fft * 4;
}
static size_t h_gw_llr(const geom_t *g) { return (size_t)g->n_rb * 12 * 14 * 4 * 8 * sizeof(int16_t); }        /* [CODE nr_pdcch_blind_monitor_rt.c:9965] */
static size_t h_gw_unit(const geom_t *g)                                                                       /* [CODE nr_td_grantwork.c:47 nr_td_gw_alloc_bytes] */
{
  size_t c = 4096, bytes = h_gw_llr(g);
  int k = 0;
  while (c < bytes && k < H_GW_CLASSES - 1) { c <<= 1; k++; }
  return c >= bytes ? c + H_GW_HDR : 0;
}
static size_t h_gw_full(const nr_host_caps_t *k)                                                               /* [CODE nr_pdcch_blind_monitor_rt.c:9967 consumers x depth x NR_TD_GW_MAX_SIG; dl_queue_shape clamps at :9895] */
{
  if (!k->gw_enabled) return 0;
  int cons = k->consumers < 1 ? 1 : (k->consumers > 6 ? 6 : k->consumers); /* NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS 6, nr_pdsch_passive_queue.h:82 */
  int depth = k->depth > 0 ? k->depth : 8 * cons;
  if (depth < 2) depth = 2;
  if (depth > 64) depth = 64;                                                                                  /* NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH 64, :77 */
  return (size_t)cons * (size_t)depth * H_GW_MAX_SIG;
}
static size_t h_gw_floor(const nr_host_caps_t *k)
{
  const size_t full = h_gw_full(k);
  int cons = k->consumers < 1 ? 1 : (k->consumers > 6 ? 6 : k->consumers);
  return min_z((size_t)cons * H_GW_FLOOR_SETS, full);
}

/* ---- GPU module sizes ---- */
static size_t g_csirs_fixed(const geom_t *g)           /* per-cell part of nr_csirs_disc_mem_fixed: the local-memory reservation is one per process */
{
  const size_t f = nr_csirs_disc_mem_fixed(g->n_rb, g->rx, g->fft, g->stride, H_TD_BATCH);
  return f > G_CSIRS_LOCAL_MIB * MIB ? f - G_CSIRS_LOCAL_MIB * MIB : 0;
}
static size_t g_csirs_slot(const geom_t *g) { return nr_csirs_disc_mem_per_slot(g->n_rb, g->rx); }
static size_t g_nid_batch(int rb)                       /* window_bytes(rb, dur, 1) without the st2_snap_t term (type size not in a header) [CODE nr_pdcch_nid_gpu.cu:579-583] */
{
  const size_t n = G_NID_BATCH, sc = 12 * (size_t)rb;
  return n * G_NID_DUR * sc * 4 + 2 * n * sizeof(int) + n * sizeof(int) * 2 + ((4 * n + 1) * 6 * 276 + 2 * n) * sizeof(double) + G_NID_STAT;
}
static size_t g_nid_fixed(void)                         /* [census 776 MiB at 273 PRB, window 10] minus 10 model windows */
{
  const size_t full = G_NID_CENSUS_MIB * MIB, w = G_NID_WIN_FULL * g_nid_batch(273);
  return full > w ? full - w : 0;
}
static size_t g_graph_cap(size_t free_bytes)
{
  size_t q = free_bytes ? free_bytes / 4 : G_GRAPH_CAP_MAX_MIB * MIB;
  if (q < G_GRAPH_CAP_MIN_MIB * MIB) q = G_GRAPH_CAP_MIN_MIB * MIB;
  if (q > G_GRAPH_CAP_MAX_MIB * MIB) q = G_GRAPH_CAP_MAX_MIB * MIB;
  return q;
}

int nr_cell_cost(const nr_cell_desc_t *c, const nr_host_caps_t *caps, nr_cell_cost_t *out)
{
  if (!c || !caps || !out) return -1;
  geom_t g;
  geom_of(c, &g);
  memset(out, 0, sizeof *out);
  out->n_rb = g.n_rb; out->scs_khz = g.scs; out->n_rx = g.rx; out->shifted = g.shifted; out->msps = (double)g.fs / 1e6;
  out->host_fixed = h_queue(&g, caps);
  size_t fl = 0, fu = 0;
  if (caps->recovery_enabled && caps->csirs_enabled) { fl += H_SNAP_FLOOR * h_iq_set(&g); fu += H_SNAP_N * h_iq_set(&g); }
  if (caps->csirs_enabled) { fl += H_CSIRS_FLOOR * h_csirs_slot(&g) + h_td_bytes(&g, H_TD_FLOOR); fu += H_CSIRS_CAP * h_csirs_slot(&g) + h_td_bytes(&g, H_TD_CAP); }
  fl += h_gw_floor(caps) * h_gw_unit(&g); fu += h_gw_full(caps) * h_gw_unit(&g);
  out->host_floor = fl; out->host_full = fu;
  if (caps->gpu_tier != 0) {
    out->gpu_fixed = G_HARQ_BYTES + (caps->csirs_enabled ? g_csirs_fixed(&g) : 0);
    if (caps->csirs_enabled) { out->gpu_floor = H_CSIRS_FLOOR * g_csirs_slot(&g); out->gpu_full = H_CSIRS_CAP * g_csirs_slot(&g); }
  }
  return 0;
}

/* ---- evaluation of one candidate set ---- */
enum { R_SNAP = 1, R_CSIRS = 2, R_TD = 4, R_GW = 8, R_GSLOTS = 16 };
typedef struct {
  int snap, csirs, td, gslots;
  size_t gw, hbytes, gbytes;
  unsigned ready;
} cres_t;
typedef struct {
  cres_t c[NR_ADMIT_PLAN_MAX];
  int nid;
  size_t hB, hH, hused, gB, gused, gshared, graph;
  int gpu_total_ok, host_total_ok;
} eval_t;

static unsigned required_mask(const nr_host_caps_t *k)
{
  unsigned m = 0;
  if (k->csirs_enabled) { m |= R_CSIRS | R_TD; if (k->recovery_enabled) m |= R_SNAP; if (k->gpu_tier != 0) m |= R_GSLOTS; }
  if (k->gw_enabled) m |= R_GW;
  return m;
}

static void eval_gpu(const nr_host_caps_t *k, const geom_t *G, const int *w, const int *set, int ns, eval_t *e)
{
  e->gB = SIZE_MAX; e->gused = 0; e->gshared = 0; e->graph = 0; e->nid = 0; e->gpu_total_ok = 1;
  if (k->gpu_tier == 0 || ns == 0) return;
  const size_t B = nr_gpu_membudget_calc(k->gpu_free_bytes);
  e->gB = B;
  int W = 0, rbmax = 0;
  for (int i = 0; i < ns; i++) { W += w[set[i]]; if (G[set[i]].n_rb > rbmax) rbmax = G[set[i]].n_rb; }
  /* process-wide contexts: each scales to its own share when that is smaller than the need (nr_gpu_membudget.h) */
  size_t shared;
  if (k->gpu_shared_bytes) shared = k->gpu_shared_bytes;
  else {
    const size_t pass_inst = G_PASS_CENSUS_MIB * MIB - G_HARQ_BYTES; /* instances without the HARQ store (that one is per cell) */
    shared = min_z(G_LDPC_POOL_MIB * MIB, nr_gpu_membudget_share_of(B, NR_GPU_MEM_LDPC_POOL)) + min_z(G_LDPC_CB0_MIB * MIB, nr_gpu_membudget_share_of(B, NR_GPU_MEM_LDPC_CB0))
           + min_z(G_TD_CB0_MIB * MIB, nr_gpu_membudget_share_of(B, NR_GPU_MEM_TD_CB0)) + min_z(pass_inst, nr_gpu_membudget_share_of(B, NR_GPU_MEM_PASS));
  }
  /* above one cell the graph module's share beyond its floor is lent to the per-cell modules (graphs are keyed by shape class, not by cell: plan B.6 item 3)
   * [HYPOTHESIS, G1 replaces: one cell keeps today's weights exactly] */
  size_t extra = 0;
  if (ns > 1 && B != SIZE_MAX) {
    const size_t sg = nr_gpu_membudget_share_of(B, NR_GPU_MEM_GRAPH);
    extra = sg > G_GRAPH_FLOOR_MIB * MIB ? sg - G_GRAPH_FLOOR_MIB * MIB : 0;
  }
  const int wc = nr_gpu_mem_weight[NR_GPU_MEM_CSIRS], wn = nr_gpu_mem_weight[NR_GPU_MEM_PDCCH_NID];
  const size_t csirs_pool = B == SIZE_MAX ? SIZE_MAX : nr_gpu_membudget_share_of(B, NR_GPU_MEM_CSIRS) + extra / (size_t)(wc + wn) * (size_t)wc;
  const size_t nid_pool = B == SIZE_MAX ? SIZE_MAX : nr_gpu_membudget_share_of(B, NR_GPU_MEM_PDCCH_NID) + extra / (size_t)(wc + wn) * (size_t)wn;
  size_t used = shared;
  if (k->csirs_enabled) {
    size_t sumfix = 0;
    for (int i = 0; i < ns; i++) sumfix += g_csirs_fixed(&G[set[i]]);
    const size_t need_pool = G_CSIRS_LOCAL_MIB * MIB + sumfix;
    const size_t left = csirs_pool == SIZE_MAX ? SIZE_MAX : (csirs_pool > need_pool ? csirs_pool - need_pool : 0);
    int any = 0;
    for (int i = 0; i < ns; i++) {
      const geom_t *g = &G[set[i]];
      const size_t fx = g_csirs_fixed(g);
      const size_t sh = left == SIZE_MAX ? SIZE_MAX : fx + split(left, w[set[i]], W);
      const long s = nr_gpu_membudget_fit(sh, fx, g_csirs_slot(g), H_CSIRS_FLOOR, H_CSIRS_CAP);
      e->c[set[i]].gslots = (int)s;
      if (s) { e->c[set[i]].gbytes += fx + (size_t)s * g_csirs_slot(g); any = 1; }
    }
    if (any) used += G_CSIRS_LOCAL_MIB * MIB;
  }
  for (int i = 0; i < ns; i++) { e->c[set[i]].gbytes += G_HARQ_BYTES; used += e->c[set[i]].gbytes; }
  { /* one shared nID context, sized for the widest served grid (single-flight like discovery, plan B.6 item 5) [HYPOTHESIS, G1] */
    const long wv = nr_gpu_membudget_fit(nid_pool, g_nid_fixed(), g_nid_batch(rbmax), G_NID_WIN_FLOOR, G_NID_WIN_FULL);
    e->nid = (int)wv;
    if (wv) used += g_nid_fixed() + (size_t)wv * g_nid_batch(rbmax);
  }
  e->gshared = shared; e->gused = used;
  const size_t cap = g_graph_cap(k->gpu_free_bytes);
  e->graph = B == SIZE_MAX ? cap : (B > used ? min_z(cap, B - used) : 0);
  e->gpu_total_ok = B == SIZE_MAX || used + G_GRAPH_FLOOR_MIB * MIB <= B;
}

static void eval_host(const nr_host_caps_t *k, const geom_t *G, const int *w, const int *set, int ns, eval_t *e)
{
  size_t B = nr_host_membudget_calc(k->mem_avail);
  size_t H = k->mem_avail == SIZE_MAX ? SIZE_MAX : sat_sub(k->mem_avail, NR_HOST_MB_HARD_RESERVE);
  if (k->unified && ns) { /* the GPU commitments come out of the same RAM [CODE nr_host_membudget.c:225-228, nr_gpu_platform.c:346] */
    const size_t gpu = e->gused + (e->gB == SIZE_MAX ? 0 : e->graph);
    if (B != SIZE_MAX) B = sat_sub(B, gpu);
    if (H != SIZE_MAX) H = sat_sub(H, gpu);
  }
  e->hB = B; e->hH = H;
  const size_t Bs = sat_sub(B, k->host_shared_bytes);
  size_t Ftot = 0;
  int W = 0;
  for (int i = 0; i < ns; i++) { Ftot += h_queue(&G[set[i]], k); W += w[set[i]]; }
  const size_t left = Bs == SIZE_MAX ? SIZE_MAX : sat_sub(Bs, Ftot);
  size_t total = k->host_shared_bytes;
  for (int i = 0; i < ns; i++) {
    const geom_t *g = &G[set[i]];
    cres_t *r = &e->c[set[i]];
    const size_t q = h_queue(g, k);
    const size_t A = left == SIZE_MAX ? SIZE_MAX : q + split(left, w[set[i]], W);
    size_t hr = sat_sub(A, q), bytes = q, fixedc = q;
    /* The sequence of the receiver at start-up, on this cell's share A (one cell: A = B - shared, so every line below is the live code on the live state):
     * snapshot holders (queue start), CSI-RS ring, time-domain pool, then the GrantWork pool last. */
    if (k->recovery_enabled && k->csirs_enabled) {
      const long s = nr_host_membudget_fit_pct(hr, 0, H_SNAP_PCT, 0, h_iq_set(g), H_SNAP_FLOOR, H_SNAP_N);
      r->snap = (int)s; if (s) { const size_t b = (size_t)s * h_iq_set(g); bytes += b; fixedc += b; hr = sat_sub(hr, b); }
    }
    if (k->csirs_enabled) {
      const long s = nr_host_membudget_fit_pct(hr, 0, H_CSIRS_PCT, 0, h_csirs_slot(g), H_CSIRS_FLOOR, H_CSIRS_CAP);
      r->csirs = (int)s;
      if (s) {
        const size_t b = (size_t)s * h_csirs_slot(g); bytes += b; fixedc += b; hr = sat_sub(hr, b);
        const size_t b1 = h_td_bytes(g, 1), per = (h_td_bytes(g, H_TD_CAP + 1) - b1) / (size_t)H_TD_CAP, f0 = b1 > per ? b1 - per : 0;
        const long t = nr_host_membudget_fit_pct(hr, 0, H_TD_PCT, f0 + 2 * MIB, per, H_TD_FLOOR, H_TD_CAP);
        r->td = (int)t;
        if (t) { const size_t tb = h_td_bytes(g, (int)t); bytes += tb; fixedc += tb; }
      }
    }
    const size_t full = h_gw_full(k);
    if (full) {
      const size_t share = A == SIZE_MAX ? SIZE_MAX : sat_sub(A, fixedc) / 100 * H_GW_WEIGHT;
      const long f = nr_host_membudget_fit(share, 0, h_gw_unit(g), (long)h_gw_floor(k), (long)full);
      r->gw = (size_t)f; bytes += r->gw * h_gw_unit(g);
    }
    r->hbytes = bytes; total += bytes;
    r->ready = (r->snap ? R_SNAP : 0) | (r->csirs ? R_CSIRS : 0) | (r->td ? R_TD : 0) | (r->gw ? R_GW : 0) | (r->gslots ? R_GSLOTS : 0);
  }
  e->hused = total;
  e->host_total_ok = H == SIZE_MAX || total <= H;
}

static void eval_set(const nr_host_caps_t *k, const geom_t *G, const int *w, const int *set, int ns, eval_t *e)
{
  memset(e, 0, sizeof *e);
  eval_gpu(k, G, w, set, ns, e);
  eval_host(k, G, w, set, ns, e);
}

const char *nr_admit_reason_name(int r)
{
  static const char *const n[] = {"none", "cores", "ram", "gpu_mem", "channelizer", "max", "lost"};
  return r >= 0 && r <= NR_ADMIT_LOST ? n[r] : "?";
}

int nr_cell_admit_plan(const nr_host_caps_t *caps, const nr_cell_desc_t *cells, int n, nr_admit_plan_t *plan)
{
  if (!caps || !plan || n < 0 || n > NR_ADMIT_PLAN_MAX || (n && !cells)) return -1;
  memset(plan, 0, sizeof *plan);
  plan->n = n;
  plan->host_budget = plan->host_hard = plan->host_used = plan->gpu_budget = SIZE_MAX;
  geom_t G[NR_ADMIT_PLAN_MAX];
  int w[NR_ADMIT_PLAN_MAX], order[NR_ADMIT_PLAN_MAX], no = 0;
  for (int i = 0; i < n; i++) {
    geom_of(&cells[i], &G[i]);
    w[i] = cells[i].weight > 0 ? cells[i].weight : 1;
    plan->cell[i].order = -1;
  }
  /* order: the primary (first non-LOST cell of the inventory) first, then MAINTAIN, ACQUIRE, NEW in inventory order; LOST cells are never admitted */
  int primary = -1;
  for (int i = 0; i < n; i++) if (cells[i].state != NR_CELLD_LOST) { primary = i; break; }
  if (primary >= 0) order[no++] = primary;
  for (int cls = NR_CELLD_MAINTAIN; cls >= NR_CELLD_NEW; cls--)
    for (int i = 0; i < n; i++) if (i != primary && cells[i].state == cls) order[no++] = i;
  for (int i = 0; i < n; i++) if (cells[i].state == NR_CELLD_LOST) plan->cell[i].reason = NR_ADMIT_LOST; /* not admitted, not a limit */
  int set[NR_ADMIT_PLAN_MAX], ns = 0;
  eval_t cur, nxt;
  memset(&cur, 0, sizeof cur);
  double chan_load = 0;
  const unsigned req = required_mask(caps);
  for (int oi = 0; oi < no; oi++) {
    const int ci = order[oi];
    int reason = NR_ADMIT_OK;
    plan->cell[ci].order = oi;
    set[ns] = ci;
    eval_set(caps, G, w, set, ns + 1, &nxt);
    if (oi == 0) {
      /* the primary: today's rules, unchanged. A module below its floor is off; only the hard limit refuses. */
      if (!nxt.host_total_ok) {
        plan->refuse = 1;
        plan->cell[ci].reason = NR_ADMIT_RAM;
        plan->host_budget = nxt.hB; plan->host_hard = nxt.hH; plan->host_used = nxt.hused;
        for (int oj = 1; oj < no; oj++) { plan->cell[order[oj]].reason = NR_ADMIT_RAM; plan->cell[order[oj]].order = oj; }
        plan->binding = NR_ADMIT_RAM;
        return 0;
      }
    } else {
      if (ns >= NR_CELL_MAX) reason = NR_ADMIT_MAX;
      else if (caps->max_cells != NR_ADMIT_UNLIMITED_CELLS && ns >= (caps->max_cells < 1 ? 1 : caps->max_cells)) reason = NR_ADMIT_CORES;
      else if (G[ci].shifted && caps->chan_budget_msps >= 0 && chan_load + (double)G[ci].fs / 1e6 > caps->chan_budget_msps + 1e-9) reason = NR_ADMIT_CHANNELIZER;
      else {
        /* gpu: the newcomer and every cell that had its device ring keep it, the shared nID context stays ready, the total fits */
        int gok = nxt.gpu_total_ok && (!(req & R_GSLOTS) || nxt.c[ci].gslots) && (cur.nid == 0 || nxt.nid);
        for (int i = 0; i < ns && gok; i++) if ((cur.c[set[i]].ready & R_GSLOTS) && !nxt.c[set[i]].gslots) gok = 0;
        if (!gok && caps->gpu_tier != 0) reason = NR_ADMIT_GPU_MEM;
        else {
          int hok = nxt.host_total_ok && (nxt.c[ci].ready & (req & ~(unsigned)R_GSLOTS)) == (req & ~(unsigned)R_GSLOTS);
          for (int i = 0; i < ns && hok; i++) {
            const unsigned had = cur.c[set[i]].ready & ~(unsigned)R_GSLOTS;
            if ((nxt.c[set[i]].ready & had) != had) hok = 0;
          }
          if (!hok) reason = NR_ADMIT_RAM;
        }
      }
    }
    if (reason != NR_ADMIT_OK) {
      plan->cell[ci].reason = reason;
      if (!plan->binding) plan->binding = reason;
      continue;
    }
    set[ns++] = ci;
    cur = nxt;
    if (G[ci].shifted) chan_load += (double)G[ci].fs / 1e6;
    plan->cell[ci].served = 1;
    plan->cell[ci].reason = NR_ADMIT_OK;
  }
  plan->n_served = ns;
  if (ns) {
    for (int i = 0; i < ns; i++) {
      nr_admit_cell_t *o = &plan->cell[set[i]];
      const cres_t *r = &cur.c[set[i]];
      o->snap_holders = r->snap; o->csirs_slots = r->csirs; o->td_slots = r->td; o->gw_buffers = r->gw; o->gpu_csirs_slots = r->gslots;
      o->host_bytes = r->hbytes; o->gpu_bytes = r->gbytes;
    }
    plan->host_budget = cur.hB; plan->host_hard = cur.hH; plan->host_used = cur.hused;
    plan->gpu_budget = cur.gB; plan->gpu_used = cur.gused; plan->gpu_shared = cur.gshared; plan->nid_window = cur.nid; plan->gpu_graph_bytes = cur.graph;
  }
  return ns;
}

int nr_admit_plan_format(const nr_admit_plan_t *p, char *out, size_t n)
{
#define MB(x) ((x) == SIZE_MAX ? -1LL : (long long)((x) >> 20))
  int k = snprintf(out, n, "MPCI_ADMIT cells=%d of %d reason=%s refuse=%d host_budget_mb=%lld host_used_mb=%lld gpu_budget_mb=%lld gpu_used_mb=%lld nid_window=%d unserved=[", p->n_served, p->n,
                   nr_admit_reason_name(p->binding), p->refuse, MB(p->host_budget), MB(p->host_used), MB(p->gpu_budget), MB(p->gpu_used), p->nid_window);
  for (int i = 0, first = 1; i < p->n && k > 0 && (size_t)k < n; i++)
    if (!p->cell[i].served) { k += snprintf(out + k, n - k, "%s%d:%s", first ? "" : " ", i, nr_admit_reason_name(p->cell[i].reason)); first = 0; }
  if (k > 0 && (size_t)k < n) k += snprintf(out + k, n - k, "]");
  return k;
#undef MB
}
