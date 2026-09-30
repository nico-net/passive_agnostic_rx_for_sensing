/* Offline stage-1 sweep: blind PDCCH DM-RS scrambling-ID discovery over all 65536 IDs x all symbols,
 * on snapshots dumped by the receiver (ISAC_COREMAP_IDSWEEP=<batches>, /tmp/passive_rx/idsweep_NNN.bin).
 * DSP is the receiver's own code, moved here because running it in-process starved the RT path.
 *   build: gcc -O3 -march=native -fopenmp -I<repo> idsweep_offline.c -o idsweep_offline -lm
 *   run:   ./idsweep_offline /tmp/passive_rx/idsweep_*.bin      (cumulative report after each file)
 *   check: ./idsweep_offline --selftest                         (synthetic, Point A at grid sc 7) */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct { int16_t r, i; } c16_t;
#include "openair1/PHY/gold.h"
#ifdef USE_GPU
int idsw_gpu_batch(int nsnap, int ncrb, int c_lo, int c_hi, int cs_lo, int cs_hi, int have_css0, int sps,
                   const int *slot, const int *sym, const float *yr, const float *yi, const float *py,
                   float *v_rest, float *v_css0);
#endif
#define IDSW_NID 65536
#define IDSW_NSYM 14
#define IDSW_BINS 4096
static struct {
  int nsc, point_a_sc, css0_crb, css0_nrb, pci, sps, n;
  int dur; /* consecutive symbols per snapshot entry (2026-09-24 "ISW2" format); 1 for every selftest
            * and for any real file, that field's stride is a no-op reduction to the original layout.
            * Only the FIRST symbol of each window is used by the analysis below -- the extra dur-1
            * captured symbols are stored on disk but not yet exploited by any scoring function here;
            * see nr_pdcch_coreset_map.c's own comment for why (coherent multi-symbol ranking is
            * follow-up work, not attempted in this pass). */
  int *slot, *sym;
  c16_t *re;
  float *zsum[2];
  uint16_t *hits[2];
  unsigned nsnap, batches;
} g_idsw;
/* Conjugated PDCCH DM-RS symbol m, bit-identical to nr_dmrs_rx.c's get_modulated(.., true), scaled
 * to +-1 +-j. */
static inline void idsw_pilot(const uint32_t *gold, int m, float *xr, float *xi)
{
  static const float tab[4][2] = {{1, -1}, {1, 1}, {-1, -1}, {-1, 1}};
  static const int swap2bits[4] = {0, 2, 1, 3};
  const uint8_t b = ((const uint8_t *)gold)[m / 4];
  const int idx = swap2bits[(b >> ((m * 2) & 7)) & 3];
  *xr = tab[idx][0];
  *xi = tab[idx][1];
}

static uint32_t idsw_cinit(int nid, int slot, int l)
{
  uint64_t x = ((uint64_t)g_idsw.sps * slot + l + 1) * ((uint64_t)(nid << 1) + 1);
  x <<= 17;
  x += (uint64_t)(nid << 1);
  return (uint32_t)(x % (1U << 31));
}

/* Robust median and sigma of v[0..n) in [0,1] via a histogram (O(n)). */
static void idsw_med_sig(const float *v, int n, float *med, float *sig)
{
  static uint32_t h[IDSW_BINS];
  memset(h, 0, sizeof(h));
  for (int i = 0; i < n; i++) {
    int b = (int)(v[i] * (IDSW_BINS - 1));
    h[b < 0 ? 0 : b >= IDSW_BINS ? IDSW_BINS - 1 : b]++;
  }
  int acc = 0, mb = 0;
  while (mb < IDSW_BINS - 1 && acc + (int)h[mb] < n / 2)
    acc += h[mb++];
  *med = (mb + 0.5f) / (IDSW_BINS - 1);
  memset(h, 0, sizeof(h));
  for (int i = 0; i < n; i++) {
    int b = (int)(fabsf(v[i] - *med) * (IDSW_BINS - 1));
    h[b >= IDSW_BINS ? IDSW_BINS - 1 : b]++;
  }
  acc = 0;
  int db = 0;
  while (db < IDSW_BINS - 1 && acc + (int)h[db] < n / 2)
    acc += h[db++];
  const float mad = (db + 0.5f) / (IDSW_BINS - 1);
  *sig = 1.4826f * mad > 1e-6f ? 1.4826f * mad : 1e-6f;
}

/* Best coherent 6-CRB |corr| over every start CRB in [c_lo, c_hi), skipping windows overlapping
 * [ex_lo, ex_hi). y/py are per-CRB DM-RS REs and power; m0 = CRB whose sequence index is 0. */
static float idsw_best(const float (*yr)[3], const float (*yi)[3], const float *py, int c_lo, int c_hi,
                       int ex_lo, int ex_hi, const uint32_t *gold, int m0)
{
  const int n = c_hi - c_lo;
  if (n < 6)
    return 0.0f;
  float ar[n + 1], ai[n + 1], ap[n + 1];
  ar[0] = ai[0] = ap[0] = 0.0f;
  for (int i = 0; i < n; i++) {
    const int c = c_lo + i;
    float sr = 0, si = 0;
    for (int q = 0; q < 3; q++) {
      float xr, xi;
      idsw_pilot(gold, 3 * (c - m0) + q, &xr, &xi);
      sr += yr[c][q] * xr - yi[c][q] * xi;
      si += yr[c][q] * xi + yi[c][q] * xr;
    }
    ar[i + 1] = ar[i] + sr;
    ai[i + 1] = ai[i] + si;
    ap[i + 1] = ap[i] + py[c];
  }
  float best = 0.0f;
  for (int i = 0; i + 6 <= n; i++) {
    const int c = c_lo + i;
    if (ex_hi > ex_lo && c < ex_hi && c + 6 > ex_lo)
      continue;
    const float re = ar[i + 6] - ar[i], im = ai[i + 6] - ai[i], p = ap[i + 6] - ap[i];
    const float v = p > 0.0f ? sqrtf((re * re + im * im) / (p * 36.0f)) : 0.0f; /* |x|^2 = 2 over 18 REs */
    if (v > best)
      best = v;
  }
  return best;
}

static void idsw_report(void)
{
  const float zmax_null = sqrtf(2.0f * logf((float)IDSW_NID)) + 2.0f;
  for (int r = 0; r < 2; r++) {
    int ts[5] = {-1, -1, -1, -1, -1}, ti[5];
    float tv[5];
    for (int s = 0; s < IDSW_NSYM; s++)
      for (int id = 0; id < IDSW_NID; id++) {
        const float v = g_idsw.hits[r][s * IDSW_NID + id] + g_idsw.zsum[r][s * IDSW_NID + id] * 1e-4f;
        for (int t = 0; t < 5; t++)
          if (ts[t] < 0 || v > tv[t]) {
            for (int u = 4; u > t; u--) {
              ts[u] = ts[u - 1];
              ti[u] = ti[u - 1];
              tv[u] = tv[u - 1];
            }
            ts[t] = s;
            ti[t] = id;
            tv[t] = v;
            break;
          }
      }
    char b[500];
    int u = 0;
    const float rn = sqrtf((float)(g_idsw.nsnap ? g_idsw.nsnap : 1));
    for (int t = 0; t < 5 && ts[t] >= 0; t++) {
      const int k = ts[t] * IDSW_NID + ti[t];
      u += snprintf(b + u, sizeof(b) - u, "id=%d/sym%d:hits=%u:z=%.1f ", ti[t], ts[t], g_idsw.hits[r][k],
                    g_idsw.zsum[r][k] / rn);
    }
    /* best competitor that is a DIFFERENT ID (the same ID on another symbol is not a competitor) */
    int oid = -1, osym = -1;
    float ov = -1e30f;
    for (int s = 0; s < IDSW_NSYM; s++)
      for (int id = 0; id < IDSW_NID; id++) {
        if (ts[0] >= 0 && id == ti[0])
          continue;
        const float v = g_idsw.hits[r][s * IDSW_NID + id] + g_idsw.zsum[r][s * IDSW_NID + id] * 1e-4f;
        if (v > ov) {
          ov = v;
          oid = id;
          osym = s;
        }
      }
    printf("IDSWEEP region=%s batch=%u snaps/sym=%u zbound=%.1f top: %s| best_other id=%d/sym%d hits=%u z=%.1f\n",
           r == 0 ? "css0" : "rest", g_idsw.batches, g_idsw.nsnap, zmax_null, b, oid, osym,
           oid >= 0 ? g_idsw.hits[r][osym * IDSW_NID + oid] : 0,
           oid >= 0 ? g_idsw.zsum[r][osym * IDSW_NID + oid] / rn : 0.0f);
  }
  fflush(stdout);
}


/* STAGE 1 -> LIST of significant (region, ID, symbol) cells. Under the null a cell's hits are Poisson
 * with rate lambda measured over all cells of the region; threshold k is the smallest with
 * N_cells * P(Poisson(lambda) >= k) < 0.01 (family-wise 1 %). Derived from the data only. */
static double poisson_tail(double lam, int k)
{
  double term = exp(-lam), cdf = 0.0;
  for (int i = 0; i < k; i++) {
    cdf += term;
    term *= lam / (i + 1);
  }
  return 1.0 - cdf > 0.0 ? 1.0 - cdf : 0.0;
}

static int g_list_id[40], g_list_sym[40], g_list_n;
static void idsw_list(void)
{
  g_list_n = 0;
  const size_t ncell = (size_t)IDSW_NSYM * IDSW_NID;
  for (int r = 0; r < 2; r++) {
    double tot = 0.0;
    for (size_t i = 0; i < ncell; i++)
      tot += g_idsw.hits[r][i];
    const double lam = tot / (double)ncell;
    int k = 1;
    while (k < 1000 && (double)ncell * poisson_tail(lam, k) >= 0.01)
      k++;
    char *done = calloc(ncell, 1);
    int nlist = 0;
    for (int pass = 0; pass < 40; pass++) {
      long bi = -1;
      unsigned bv = 0;
      for (size_t i = 0; i < ncell; i++)
        if (!done[i] && g_idsw.hits[r][i] >= (unsigned)k && g_idsw.hits[r][i] > bv) {
          bv = g_idsw.hits[r][i];
          bi = (long)i;
        }
      if (bi < 0)
        break;
      done[bi] = 1;
      if (r == 1 && g_list_n < 40) {
        g_list_id[g_list_n] = (int)(bi % IDSW_NID);
        g_list_sym[g_list_n++] = (int)(bi / IDSW_NID);
      }
      printf("STAGE1LIST region=%s id=%ld sym=%ld hits=%u (k=%d lambda=%.2e)\n", r ? "rest" : "css0",
             bi % IDSW_NID, bi / IDSW_NID, bv, k, lam);
      nlist++;
    }
    if (!nlist)
      printf("STAGE1LIST region=%s none (k=%d lambda=%.2e)\n", r ? "rest" : "css0", k, lam);
    free(done);
  }
  fflush(stdout);
}

static void idsw_process(void)
{
  const int nsc = g_idsw.nsc, pa = g_idsw.point_a_sc;
  /* CRBs whose 12 subcarriers lie wholly inside the grid */
  const int c_lo = pa >= 0 ? 0 : (-pa + 11) / 12;
  const int c_hi = (nsc - pa) / 12; /* exclusive */
  const int ncrb = c_hi;            /* arrays indexed by CRB from 0 */
  if (c_hi - c_lo < 6)
    return;
  const int cs_lo = g_idsw.css0_crb, cs_hi = g_idsw.css0_crb + g_idsw.css0_nrb;
  const bool have_css0 = g_idsw.css0_nrb >= 6 && cs_lo >= c_lo && cs_hi <= c_hi;
  const int words = (2 * 3 * ncrb) / 32 + 2;
  float(*yr)[3] = malloc(sizeof(float[3]) * ncrb), (*yi)[3] = malloc(sizeof(float[3]) * ncrb);
  float *py = malloc(sizeof(float) * ncrb), *v[2] = {malloc(sizeof(float) * IDSW_NID), malloc(sizeof(float) * IDSW_NID)};
  uint32_t *gold = malloc(sizeof(uint32_t) * words);
#ifdef USE_GPU
  float *gyr = NULL, *gyi = NULL, *gpy = NULL, *gvr = NULL, *gvc = NULL;
#endif
  if (!yr || !yi || !py || !v[0] || !v[1] || !gold)
    goto out;
  const float zb = sqrtf(2.0f * logf((float)IDSW_NID)) + 2.0f;
#ifdef USE_GPU
  /* whole batch on the GPU: all (snapshot, ID) correlations at once; standardisation stays on the CPU */
  gyr = malloc(sizeof(float) * (size_t)g_idsw.n * ncrb * 3); gyi = malloc(sizeof(float) * (size_t)g_idsw.n * ncrb * 3);
  gpy = malloc(sizeof(float) * (size_t)g_idsw.n * ncrb);
  gvr = malloc(sizeof(float) * (size_t)g_idsw.n * IDSW_NID); gvc = malloc(sizeof(float) * (size_t)g_idsw.n * IDSW_NID);
  for (int k = 0; k < g_idsw.n; k++) {
    const c16_t *re = g_idsw.re + (size_t)k * g_idsw.dur * nsc; /* window's FIRST symbol only */
    for (int c = 0; c < ncrb; c++) {
      float p = 0;
      for (int q = 0; q < 3; q++) {
        const c16_t y = c >= c_lo ? re[pa + 12 * c + 1 + 4 * q] : (c16_t){0, 0};
        gyr[((size_t)k * ncrb + c) * 3 + q] = y.r;
        gyi[((size_t)k * ncrb + c) * 3 + q] = y.i;
        p += (float)y.r * y.r + (float)y.i * y.i;
      }
      gpy[(size_t)k * ncrb + c] = p;
    }
  }
  const bool gpu_ok = idsw_gpu_batch(g_idsw.n, ncrb, c_lo, c_hi, cs_lo, cs_hi, have_css0, g_idsw.sps, g_idsw.slot,
                                     g_idsw.sym, gyr, gyi, gpy, gvr, gvc) == 0;
  if (!gpu_ok) fprintf(stderr, "GPU batch failed, falling back to CPU\n");
  const bool gpucheck = getenv("IDSW_GPUCHECK") != NULL;
#endif
  {
    for (int k = 0; k < g_idsw.n; k++) {
      const int s = g_idsw.sym[k];
      const c16_t *re = g_idsw.re + (size_t)k * g_idsw.dur * nsc; /* window's FIRST symbol only */
      for (int c = c_lo; c < c_hi; c++) {
        float p = 0;
        for (int q = 0; q < 3; q++) {
          const c16_t y = re[pa + 12 * c + 1 + 4 * q];
          yr[c][q] = y.r;
          yi[c][q] = y.i;
          p += (float)y.r * y.r + (float)y.i * y.i;
        }
        py[c] = p;
      }
      bool cpu = true;
#ifdef USE_GPU
      if (gpu_ok) {
        memcpy(v[1], gvr + (size_t)k * IDSW_NID, sizeof(float) * IDSW_NID);
        memcpy(v[0], gvc + (size_t)k * IDSW_NID, sizeof(float) * IDSW_NID);
        cpu = gpucheck && k < 4;
      }
#endif
      if (cpu) {
        float *cv0 = malloc(sizeof(float) * IDSW_NID), *cv1 = malloc(sizeof(float) * IDSW_NID);
#pragma omp parallel for schedule(static)
        for (int id = 0; id < IDSW_NID; id++) {
          uint32_t gold[words];
          uint32_t x1 = 0, x2 = idsw_cinit(id, g_idsw.slot[k], s);
          gold[0] = gold_generic(&x1, &x2, 1);
          for (int w = 1; w < words; w++)
            gold[w] = gold_generic(&x1, &x2, 0);
          cv1[id] = idsw_best(yr, yi, py, c_lo, c_hi, have_css0 ? cs_lo : 0, have_css0 ? cs_hi : 0, gold, 0);
          cv0[id] = have_css0 ? idsw_best(yr, yi, py, cs_lo, cs_hi, 0, 0, gold, cs_lo) : 0.0f;
        }
#ifdef USE_GPU
        if (gpu_ok) { /* check mode: report the largest GPU-vs-CPU difference, keep the GPU values */
          float d = 0;
          for (int id = 0; id < IDSW_NID; id++) d = fmaxf(d, fmaxf(fabsf(cv1[id] - v[1][id]), fabsf(cv0[id] - v[0][id])));
          printf("GPUCHECK snapshot %d: max |gpu - cpu| = %.2e\n", k, d);
        } else
#endif
        {
          memcpy(v[1], cv1, sizeof(float) * IDSW_NID);
          memcpy(v[0], cv0, sizeof(float) * IDSW_NID);
        }
        free(cv0);
        free(cv1);
      }
      for (int r = have_css0 ? 0 : 1; r < 2; r++) {
        float med, sig;
        idsw_med_sig(v[r], IDSW_NID, &med, &sig);
        float *zs = g_idsw.zsum[r] + (size_t)s * IDSW_NID;
        uint16_t *hs = g_idsw.hits[r] + (size_t)s * IDSW_NID;
        for (int id = 0; id < IDSW_NID; id++) {
          const float z = (v[r][id] - med) / sig;
          zs[id] += z;
          if (z > zb && hs[id] < UINT16_MAX)
            hs[id]++;
        }
      }
    }
  }
  g_idsw.nsnap += g_idsw.n / (g_idsw.sps > 0 ? g_idsw.sps : IDSW_NSYM);
  g_idsw.batches++;
  idsw_report();
out:
#ifdef USE_GPU
  free(gyr); free(gyi); free(gpy); free(gvr); free(gvc);
#endif
  free(yr);
  free(yi);
  free(py);
  free(v[0]);
  free(v[1]);
  free(gold);
}


static int load(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  int32_t h[9];
  if (fread(h, sizeof(h), 1, f) != 1 || h[0] != 0x32575349) { fprintf(stderr, "%s: bad header (not ISW2 -- "
      "rebuild the receiver and this tool together, see nr_pdcch_coreset_map.c)\n", path); fclose(f); return -1; }
  g_idsw.nsc = h[1]; g_idsw.point_a_sc = h[2]; g_idsw.css0_crb = h[3]; g_idsw.css0_nrb = h[4];
  g_idsw.pci = h[5]; g_idsw.sps = h[6]; g_idsw.n = h[7]; g_idsw.dur = h[8] > 0 ? h[8] : 1;
  g_idsw.slot = realloc(g_idsw.slot, sizeof(int) * g_idsw.n);
  g_idsw.sym = realloc(g_idsw.sym, sizeof(int) * g_idsw.n);
  g_idsw.re = realloc(g_idsw.re, sizeof(c16_t) * (size_t)g_idsw.n * g_idsw.dur * g_idsw.nsc);
  size_t ok = fread(g_idsw.slot, sizeof(int), g_idsw.n, f) + fread(g_idsw.sym, sizeof(int), g_idsw.n, f)
              + fread(g_idsw.re, sizeof(c16_t), (size_t)g_idsw.n * g_idsw.dur * g_idsw.nsc, f);
  fclose(f);
  if (ok != (size_t)g_idsw.n * 2 + (size_t)g_idsw.n * g_idsw.dur * g_idsw.nsc) {
    fprintf(stderr, "%s: short\n", path); return -1;
  }
  printf("%s: %d snapshots x %d symbol(s), nsc=%d PointA_sc=%d css0_crb=%d+%d pci=%d sps=%d\n", path, g_idsw.n,
         g_idsw.dur, g_idsw.nsc, g_idsw.point_a_sc, g_idsw.css0_crb, g_idsw.css0_nrb, g_idsw.pci, g_idsw.sps);
  return 0;
}

/* Synthetic capture: Point A at grid sc 7 (NOT 0), CORESET#0 at CRB 10+48 carrying PCI-2 DM-RS on
 * symbol 0 (CORESET#0 reference), a dedicated CORESET carrying ID 12345 on symbol 3 at CRBs 100..111
 * (CRB-0 reference), unit-power noise everywhere, DM-RS amplitude 1.5 (~3.5 dB), 1 in 3 slots. */
static void selftest_make(void)
{
  g_idsw.nsc = 3276; g_idsw.point_a_sc = 7; g_idsw.css0_crb = 10; g_idsw.css0_nrb = 48; g_idsw.pci = 2;
  g_idsw.sps = 14; g_idsw.n = 14 * 24; g_idsw.dur = 1; /* single-symbol synthetic scene */
  g_idsw.slot = malloc(sizeof(int) * g_idsw.n); g_idsw.sym = malloc(sizeof(int) * g_idsw.n);
  g_idsw.re = malloc(sizeof(c16_t) * (size_t)g_idsw.n * g_idsw.nsc);
  srand(1);
  for (int k = 0; k < g_idsw.n; k++) {
    g_idsw.slot[k] = rand() % 20; g_idsw.sym[k] = k % 14;
    c16_t *re = g_idsw.re + (size_t)k * g_idsw.nsc;
    for (int i = 0; i < g_idsw.nsc; i++) {
      double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0), r = 700 * sqrt(-2 * log(u1));
      re[i].r = (int16_t)(r * cos(2 * M_PI * u2)); re[i].i = (int16_t)(r * sin(2 * M_PI * u2));
    }
    int id = -1, c_lo = 0, c_n = 0, m0 = 0;
    if (g_idsw.sym[k] == 0 && k % 3 == 0) { id = 2; c_lo = 10 + 6; c_n = 12; m0 = 10; }
    if (g_idsw.sym[k] == 3 && k % 3 == 0) { id = 12345; c_lo = 100; c_n = 12; m0 = 0; }
    if (id < 0) continue;
    uint32_t gold[300], x1 = 0, x2 = idsw_cinit(id, g_idsw.slot[k], g_idsw.sym[k]);
    gold[0] = gold_generic(&x1, &x2, 1);
    for (int w = 1; w < 300; w++) gold[w] = gold_generic(&x1, &x2, 0);
    for (int c = c_lo; c < c_lo + c_n; c++)
      for (int q = 0; q < 3; q++) {
        float xr, xi;
        idsw_pilot(gold, 3 * (c - m0) + q, &xr, &xi);           /* conj pilot */
        const int sc = g_idsw.point_a_sc + 12 * c + 1 + 4 * q;
        re[sc].r += (int16_t)(1.5 * 700 * xr); re[sc].i += (int16_t)(-1.5 * 700 * xi); /* transmit x = conj(conj) */
      }
  }
}


/* ======================= STAGE 2: decode-free CORESET geometry ranking =======================
 * Inputs: stage 1's winning (nID, start symbol) and the dumped snapshots. Nothing else.
 * - duration: consecutive symbols from the start symbol whose lit rate (for that nID) is >= half the
 *   start symbol's (DM-RS exists in every CORESET symbol);
 * - active snapshot: >= 1 six-CRB window whose coherence exceeds the null bound for 18 REs at a 1e-4
 *   false-alarm rate: rho^2 ~ Beta(1,17) under noise, so t = 1 - 1e-4^(1/17) -- statistics, not cell;
 * - hypotheses: RB-grid phase 0..5 x extent x mapping (non-interleaved; interleaved L in {2,6},
 *   R in {2,3,6}, EVERY shift), all ALs;
 * - score: per active snapshot, the best AL>=2 candidate's mean bundle excess coherence (a real
 *   multi-CCE PDCCH only lines up under the right mapping), summed. AL1 cannot separate mappings with
 *   6-RB bundles and is reported, not scored.
 * CORESET#0's CRBs (MIB) are masked: a known quantity whose SIB1 PDCCH would otherwise pollute the
 * dedicated evidence. Limitation: an interleaved CORESET overlapping CORESET#0 is under-explored. */
typedef struct {
  int phase, g0, ng, L, R, shift; /* L = 0: non-interleaved */
  double score;
  int al_win[5];
} st2_hyp_t;
typedef struct {
  double *re, *im, *p;  /* prefix sums over CRB, length c_hi+1 */
  double *rr, *ri, *rp; /* raw per-CRB values, length c_hi (explained energy is zeroed here) */
  int k;                /* snapshot index in g_idsw */
} st2_snap_t;

static int g_st2_clo, g_st2_chi;
static int g_st2_D = 1; /* CORESET duration: the per-CRB accumulators SUM this many consecutive symbols coherently */
static int g_found_dur[16];
static st2_hyp_t g_found[16], g_found_tie[16][64];
static int g_found_nid[16], g_found_nties[16], g_found_sym[16], g_nfound;

static inline double st2_exc(const st2_snap_t *s, int b, int B)
{
  if (b < g_st2_clo || b + B > g_st2_chi)
    return 0.0;
  const double re = s->re[b + B] - s->re[b], im = s->im[b + B] - s->im[b], p = s->p[b + B] - s->p[b];
  if (p <= 0.0)
    return 0.0;
  const double n = 3.0 * B * g_st2_D, rho2 = (re * re + im * im) / (p * 2.0 * n);
  return (rho2 - 1.0 / n) / (1.0 - 1.0 / n);
}

/* mean bundle excess of one candidate; bundles per TS 38.211 7.3.2.2 (duration 1) */
static double st2_cand(const st2_snap_t *s, const st2_hyp_t *h, int cce, int al)
{
  /* TS 38.211 7.3.2.2: REGs are numbered time-first (REG n = RB n/D, symbol n%D), a REG bundle is L consecutive
   * REGs, so a bundle spans L/D RBs on ALL D symbols; N_REG = 6*groups*D. D = 1 is the original geometry. */
  const int D = g_st2_D, b = h->L ? h->L : 6, nb = 6 * h->ng * D / b, cs = h->phase + 6 * h->g0;
  double sum = 0.0;
  int cnt = 0;
  for (int j = cce * 6 / b; j < (cce + al) * 6 / b; j++) {
    const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
    sum += st2_exc(s, cs + f * b / D, b / D);
    cnt++;
  }
  return cnt ? sum / cnt : 0.0;
}

static void st2_score(st2_hyp_t *h, const st2_snap_t *sn, int nact)
{
  static const int als[5] = {1, 2, 4, 8, 16};
  h->score = 0.0;
  memset(h->al_win, 0, sizeof(h->al_win));
  for (int k = 0; k < nact; k++) {
    double best = -1.0;
    int bal = -1;
    for (int ai = 1; ai < 5; ai++)
      for (int cce = 0; cce + als[ai] <= h->ng * g_st2_D; cce += als[ai]) {
        const double v = st2_cand(&sn[k], h, cce, als[ai]);
        if (v > best) {
          best = v;
          bal = ai;
        }
      }
    if (bal >= 0) {
      h->score += best;
      h->al_win[bal]++;
    }
  }
}

static int st2_cmp(const void *a, const void *b)
{
  const double x = ((const st2_hyp_t *)a)->score, y = ((const st2_hyp_t *)b)->score;
  return (x < y) - (x > y);
}

static void st2_fmt(const st2_hyp_t *h, int nact, char *o, size_t n)
{
  if (h->L)
    snprintf(o, n, "phase=%d crb=%d groups=%d interleaved L=%d R=%d shift=%d score=%.3f al2/4/8/16=%d/%d/%d/%d",
             h->phase, h->phase + 6 * h->g0, h->ng, h->L, h->R, h->shift, nact ? h->score / nact : 0.0,
             h->al_win[1], h->al_win[2], h->al_win[3], h->al_win[4]);
  else
    snprintf(o, n, "phase=%d crb=%d groups=%d NON-interleaved score=%.3f al2/4/8/16=%d/%d/%d/%d", h->phase,
             h->phase + 6 * h->g0, h->ng, nact ? h->score / nact : 0.0, h->al_win[1], h->al_win[2], h->al_win[3],
             h->al_win[4]);
}

/* Build prefix sums of the nID's DM-RS correlation for snapshot k (CORESET#0 CRBs masked). */
static void st2_prefix_body(st2_snap_t *s)
{
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++) {
    s->re[c + 1] = s->re[c] + s->rr[c];
    s->im[c + 1] = s->im[c] + s->ri[c];
    s->p[c + 1] = s->p[c] + s->rp[c];
  }
}

static void st2_build(int k, int nid, st2_snap_t *s)
{
  const int nsc = g_idsw.nsc, pa = g_idsw.point_a_sc, words = (2 * 3 * g_st2_chi) / 32 + 2;
  const int D = g_st2_D <= g_idsw.dur ? g_st2_D : g_idsw.dur; /* never read past the symbols the capture holds */
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++)
    s->rr[c] = s->ri[c] = s->rp[c] = 0.0; /* every caller allocates through st2_alloc() */
  /* per CRB: the pilot-derotated correlation SUMMED over the D symbols of the window (coherent within a REG
   * bundle, which spans all D symbols), each symbol with its own DM-RS sequence (c_init depends on the symbol) */
  for (int d = 0; d < D; d++) {
    uint32_t gold[words], x1 = 0, x2 = idsw_cinit(nid, g_idsw.slot[k], g_idsw.sym[k] + d);
    gold[0] = gold_generic(&x1, &x2, 1);
    for (int w = 1; w < words; w++)
      gold[w] = gold_generic(&x1, &x2, 0);
    const c16_t *re = g_idsw.re + ((size_t)k * g_idsw.dur + d) * nsc;
    for (int c = 0; c < g_st2_chi; c++) {
      const bool masked = c >= g_idsw.css0_crb && c < g_idsw.css0_crb + g_idsw.css0_nrb;
      if (c < g_st2_clo || masked)
        continue;
      double a = 0, b = 0, p = 0;
      for (int q = 0; q < 3; q++) {
        float xr, xi;
        idsw_pilot(gold, 3 * c + q, &xr, &xi);
        const c16_t y = re[pa + 12 * c + 1 + 4 * q];
        a += y.r * xr - y.i * xi;
        b += y.r * xi + y.i * xr;
        p += (double)y.r * y.r + (double)y.i * y.i;
      }
      s->rr[c] += a;
      s->ri[c] += b;
      s->rp[c] += p;
    }
  }
  st2_prefix_body(s);
  s->k = k;
}

static void st2_prefix(st2_snap_t *s)
{
  s->re[0] = s->im[0] = s->p[0] = 0.0;
  for (int c = 0; c < g_st2_chi; c++) {
    s->re[c + 1] = s->re[c] + s->rr[c];
    s->im[c + 1] = s->im[c] + s->ri[c];
    s->p[c + 1] = s->p[c] + s->rp[c];
  }
}

static st2_snap_t st2_alloc(void)
{
  st2_snap_t s;
  s.re = malloc(sizeof(double) * (g_st2_chi + 1));
  s.im = malloc(sizeof(double) * (g_st2_chi + 1));
  s.p = malloc(sizeof(double) * (g_st2_chi + 1));
  s.rr = malloc(sizeof(double) * g_st2_chi);
  s.ri = malloc(sizeof(double) * g_st2_chi);
  s.rp = malloc(sizeof(double) * g_st2_chi);
  s.k = -1;
  return s;
}

static int *g_st2_litpos, g_st2_nlit, g_st2_litcap; /* lit window starts, for a robust hull */
static bool st2_lit(const st2_snap_t *s, int *lo, int *hi)
{
  const double t = 1.0 - pow(1e-4, 1.0 / (18.0 * g_st2_D - 1.0)); /* Beta(1, n-1), n = 18 REs per symbol x D */
  bool any = false;
  for (int c = g_st2_clo; c + 6 <= g_st2_chi; c++) {
    const double re = s->re[c + 6] - s->re[c], im = s->im[c + 6] - s->im[c], p = s->p[c + 6] - s->p[c];
    if (p > 0 && (re * re + im * im) / (p * 36.0 * g_st2_D) > t) {
      if (g_st2_litpos && g_st2_nlit < g_st2_litcap)
        g_st2_litpos[g_st2_nlit++] = c;
      if (!any || c < *lo) *lo = c;
      if (!any || c + 5 > *hi) *hi = c + 5;
      any = true;
    }
  }
  return any;
}

/* Runs on the snapshots currently loaded in g_idsw (one or more files concatenated by the caller). */

/* per-snapshot best AL>=2 candidate score of one hypothesis */
static void st2_per_snap(const st2_hyp_t *h, const st2_snap_t *sn, int nact, double *out)
{
  static const int als[4] = {2, 4, 8, 16};
  for (int k = 0; k < nact; k++) {
    double best = 0.0;
    for (int ai = 0; ai < 4; ai++)
      for (int cce = 0; cce + als[ai] <= h->ng * g_st2_D; cce += als[ai]) {
        const double v = st2_cand(&sn[k], h, cce, als[ai]);
        if (v > best) best = v;
      }
    out[k] = best;
  }
}

/* paired t of real vs decoy per-snapshot scores */
static double st2_paired_t(const double *a, const double *b, int n)
{
  double m = 0, v = 0;
  for (int i = 0; i < n; i++) m += a[i] - b[i];
  m /= n;
  for (int i = 0; i < n; i++) v += (a[i] - b[i] - m) * (a[i] - b[i] - m);
  v = n > 1 ? v / (n - 1) : 0;
  return v > 0 ? m / sqrt(v / n) : 0.0;
}

/* Remove every AL>=2 candidate of h that is significantly lit (candidate-level test below).
 * Zeroes the raw arrays over its bundles and rebuilds the prefix sums. */
static int st2_remove(st2_snap_t *s, const st2_hyp_t *h)
{
  static const int als[5] = {1, 2, 4, 8, 16};
  const int D = g_st2_D, b = h->L ? h->L : 6, nb = 6 * h->ng * D / b, cs = h->phase + 6 * h->g0;
  const double n = 3.0 * b, t = 1.0 - pow(1e-3, 1.0 / (n - 1.0)); /* REs per bundle = 3 per REG whatever D */
  int removed = 0;
  /* AL >= 2 only: those candidates are the hypothesis's evidence. An AL1 candidate (one 6-RB bundle)
   * fits every mapping with 6-RB bundles, so letting it claim energy wiped out a second, interleaved
   * CORESET of the same nID (selftest3, CORESET C). */
  for (int ai = 4; ai >= 1; ai--)
    for (int cce = 0; cce + als[ai] <= h->ng * D; cce += als[ai]) {
      /* candidate-level test: under noise each bundle's excess has sd ~ 1/(n-1) (Beta(1,n-1)), so the
       * mean over m bundles has sd 1/((n-1) sqrt m); lit if the candidate mean exceeds 3.09 sd (1e-3) */
      const int m = als[ai] * 6 / b;
      const double thr = 3.09 / ((n - 1.0) * sqrt((double)m));
      bool all = st2_cand(s, h, cce, als[ai]) > thr;
      /* AND every bundle above 2 sd (1/(n-1) each): a half-lit candidate -- one real bundle next to an
       * empty one -- must not claim the real bundle (it swallowed a second CORESET in selftest3) */
      for (int j = cce * 6 / b; j < (cce + als[ai]) * 6 / b && all; j++) {
        const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
        all = st2_exc(s, cs + f * b / D, b / D) > 2.0 / (n - 1.0);
      }
      (void)t;
      if (!all) continue;
      for (int j = cce * 6 / b; j < (cce + als[ai]) * 6 / b; j++) {
        const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
        for (int c = cs + f * b / D; c < cs + f * b / D + b / D; c++) s->rr[c] = s->ri[c] = s->rp[c] = 0.0;
      }
      st2_prefix(s);
      removed++;
    }
  return removed;
}


/* Robust hull of lit window starts (2 %/98 % quantiles) over the CURRENT (residual) snapshots. min/max
 * is set by one false-alarm window: on run s01f it pushed every phase-0 extent off the grid. */
static bool st2_hull(st2_snap_t *sn, int nact, int *qlo, int *qhi)
{
  g_st2_nlit = 0;
  for (int k = 0; k < nact; k++) {
    int lo, hi;
    st2_lit(&sn[k], &lo, &hi);
  }
  if (g_st2_nlit == 0)
    return false;
  int *v = g_st2_litpos;
  for (int i = 1; i < g_st2_nlit; i++) { int x = v[i], j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
  *qlo = v[(int)(0.02 * (g_st2_nlit - 1))];
  *qhi = v[(int)(0.98 * (g_st2_nlit - 1))] + 5;
  return true;
}

/* Hypothesis set. Non-interleaved: every start CRB on every phase, extent to the grid edge (extent only
 * bounds containment). Interleaved: the extent sets N_REG, so +-2 groups around the (residual) robust
 * hull, clipped to the grid; L in {2,6}, R in {2,3,6}, every shift. */
static size_t st2_gen(st2_hyp_t *H, size_t cap, int qlo, int qhi)
{
  size_t nh = 0;
  for (int ph = 0; ph < 6; ph++) {
    for (int cs = ph; cs + 6 <= g_st2_chi && nh < cap; cs += 6) {
      if (cs < g_st2_clo) continue;
      H[nh++] = (st2_hyp_t){ph, (cs - ph) / 6, (g_st2_chi - cs) / 6, 0, 0, 0, 0.0, {0}};
    }
    const int gl = (int)floor((qlo - ph) / 6.0), gh = (int)floor((qhi - ph) / 6.0);
    /* a lit 6-CRB window can start up to 5 CRBs before the CORESET and end up to 5 after it, so the
     * window->group conversion is uncertain by one group at each edge */
    for (int g0 = gl - 2; g0 <= gl + 1; g0++)
      for (int ge = gh - 1; ge <= gh + 2; ge++) {
        int a0 = g0, a1 = ge;
        while (ph + 6 * a0 < g_st2_clo) a0++;
        while (ph + 6 * (a1 + 1) > g_st2_chi) a1--;
        const int ng = a1 - a0 + 1;
        if (ng < 2 || (a0 != g0 && g0 != gl) || (a1 != ge && ge != gh)) continue;
        /* L is a multiple of the duration (38.211 7.3.2.2): {2,6} for D = 1, 2; {3,6} for D = 3 */
        const int Ls[2] = {g_st2_D == 3 ? 3 : 2, 6};
        static const int Rs[3] = {2, 3, 6};
        for (int li = 0; li < 2; li++)
          for (int ri = 0; ri < 3; ri++) {
            const int nb = 6 * ng * g_st2_D / Ls[li];
            if (nb % Rs[ri]) continue;
            for (int sh = 0; sh < nb && nh < cap; sh++)
              H[nh++] = (st2_hyp_t){ph, a0, ng, Ls[li], Rs[ri], sh, 0.0, {0}};
          }
      }
  }
  return nh;
}


/* JOINT-CONSISTENCY GATE (2026-09-25). Root cause shared by --selftest4 il and --selftest5 (AL1-only): a
 * hypothesis's per-snapshot best-CCE score cannot tell "these bundles are lit because a permutation coincidentally
 * mapped a real (differently-shaped or differently-scheduled) signal onto them" from "these bundles are lit
 * because this IS the CORESET's real geometry" -- especially since interleaving deliberately spreads real energy
 * across the WHOLE band, so almost any hypothesis covering the same range picks up SOME of it. The fix: a REAL
 * CORESET's used CCEs are, before interleaving, CONTIGUOUS runs of 6/L nominal bundle indices (one CCE = 6/L
 * bundles). Take every bundle position that is lit in AT LEAST ONE active (residual) snapshot, run it through
 * the CANDIDATE hypothesis's OWN inverse permutation, and check whether the result explains WHOLE nominal CCEs
 * (all 6/L members lit) rather than scattered fragments (some but not all lit) -- a wrong hypothesis's inverse
 * map has no reason to land on whole CCEs; a right one's does, because that IS what a real scheduler transmits.
 * Independent of st2_score's ranking (which stays as-is, since it is what finds a good CANDIDATE region/L/R to
 * test in the first place) -- this runs ONCE on the winning hypothesis, as an extra accept/reject gate. */
static double st2_consistency(const st2_snap_t *sn, int nact, const st2_hyp_t *h)
{
  /* PER-SNAPSHOT, not OR-aggregated across snapshots: a real scheduler can use a DIFFERENT CCE on every
   * occasion (AL1-heavy scheduling especially), so ORing "lit in ANY snapshot" over many occasions lights up
   * most of the band regardless of hypothesis, and the whole-vs-fragment distinction disappears. Instead:
   * for EACH snapshot, only THAT snapshot's own transmission (if any) should explain WHOLE CCEs when mapped
   * through the candidate geometry; a wrong geometry should smear even a single snapshot's one real,
   * correctly-shaped signal into partial hits. Aggregate as the MEDIAN per-snapshot full-fraction (robust to a
   * handful of empty/noise-only snapshots, unlike a mean). */
  const int D = g_st2_D, L = h->L ? h->L : 6, R = h->L ? h->R : 1, b_rb = L / D;
  const int nb = 6 * h->ng * D / L, cs = h->phase + 6 * h->g0, per_cce = 6 / L;
  const double n = 3.0 * L, thr = 2.0 / (n - 1.0);
  double *frac = malloc(sizeof(double) * nact);
  int nf = 0;
  uint8_t *lit = malloc((size_t)nb);
  for (int k = 0; k < nact; k++) {
    for (int f = 0; f < nb; f++)
      lit[f] = st2_exc(&sn[k], cs + f * b_rb, b_rb) > thr;
    int full = 0, partial = 0;
    for (int j0 = 0; j0 + per_cce <= nb; j0 += per_cce) {
      int hit = 0;
      for (int dj = 0; dj < per_cce; dj++) {
        const int j = j0 + dj;
        const int f = h->L ? ((j % R) * (nb / R) + j / R + h->shift) % nb : j;
        hit += lit[f];
      }
      if (hit == per_cce) full++;
      else if (hit > 0) partial++;
    }
    if (full + partial > 0)
      frac[nf++] = (double)full / (full + partial);
  }
  free(lit);
  if (nf == 0) { free(frac); return 0.0; }
  for (int i = 1; i < nf; i++) { double x = frac[i]; int j = i - 1; while (j >= 0 && frac[j] > x) { frac[j + 1] = frac[j]; j--; } frac[j + 1] = x; }
  const double med = frac[nf / 2];
  free(frac);
  return med;
}

static int stage2(int nid, int sym0, bool quiet)
{
  const int pa = g_idsw.point_a_sc;
  g_st2_clo = pa >= 0 ? 0 : (-pa + 11) / 12;
  g_st2_chi = (g_idsw.nsc - pa) / 12;
  g_st2_D = 1; /* duration detection reads each window's FIRST symbol only */
  st2_snap_t tmp = st2_alloc();
  /* duration from per-symbol lit rates */
  int nsym[IDSW_NSYM] = {0}, nlit[IDSW_NSYM] = {0};
  for (int k = 0; k < g_idsw.n; k++) {
    const int s = g_idsw.sym[k];
    if (s < sym0 || s > sym0 + 2) continue;
    int lo, hi;
    st2_build(k, nid, &tmp);
    nsym[s]++;
    nlit[s] += st2_lit(&tmp, &lo, &hi);
  }
  const double r0 = nsym[sym0] ? (double)nlit[sym0] / nsym[sym0] : 0.0;
  int dur = 1;
  while (dur < 3 && sym0 + dur < IDSW_NSYM && nsym[sym0 + dur]
         && (double)nlit[sym0 + dur] / nsym[sym0 + dur] >= 0.5 * r0)
    dur++;
  printf("STAGE2 nID=%d start_sym=%d lit rate sym%d..%d: %.3f %.3f %.3f -> duration %d\n", nid, sym0, sym0, sym0 + 2,
         r0, nsym[sym0 + 1] ? (double)nlit[sym0 + 1] / nsym[sym0 + 1] : 0.0,
         nsym[sym0 + 2] ? (double)nlit[sym0 + 2] / nsym[sym0 + 2] : 0.0, dur);
  g_st2_D = 1;
  if (dur != 1) {
    if (g_idsw.dur < dur) {
      printf("STAGE2 duration %d needs snapshots of %d consecutive symbols (this capture holds %d): re-run the receiver with "
             "ISAC_COREMAP_IDSWEEP_DUR=%d and start symbol %d\n", dur, dur, g_idsw.dur, dur, sym0);
      return 1;
    }
    g_st2_D = dur; /* coherent ranking over the D symbols of each window */
    printf("STAGE2 duration %d: ranking geometry coherently over %d consecutive symbols per snapshot\n", dur, dur);
  }
  /* active snapshots */
  st2_snap_t *sn = calloc(g_idsw.n, sizeof(st2_snap_t));
  int nact = 0, hlo = 1 << 30, hhi = -1;
  g_st2_litcap = 1 << 22;
  g_st2_litpos = malloc(sizeof(int) * g_st2_litcap);
  g_st2_nlit = 0;
  for (int k = 0; k < g_idsw.n; k++) {
    if (g_idsw.sym[k] != sym0) continue;
    st2_build(k, nid, &tmp);
    int lo, hi;
    if (!st2_lit(&tmp, &lo, &hi)) continue;
    if (lo < hlo) hlo = lo;
    if (hi > hhi) hhi = hi;
    sn[nact] = st2_alloc();
    memcpy(sn[nact].rr, tmp.rr, sizeof(double) * g_st2_chi);
    memcpy(sn[nact].ri, tmp.ri, sizeof(double) * g_st2_chi);
    memcpy(sn[nact].rp, tmp.rp, sizeof(double) * g_st2_chi);
    sn[nact].k = k;
    st2_prefix(&sn[nact]);
    nact++;
  }
  { /* phase evidence: per phase p, summed excess of 6-CRB windows aligned to p, and 12-CRB blocks */
    double w6[6] = {0}, w12[6] = {0};
    int best12[6] = {0};
    for (int k = 0; k < nact; k++) {
      double bv = -1; int bp = 0;
      for (int c = g_st2_clo; c + 12 <= g_st2_chi; c++) {
        const double e6 = st2_exc(&sn[k], c, 6), e12 = st2_exc(&sn[k], c, 12);
        if (e6 > 0) w6[c % 6] += e6;
        if (e12 > 0) w12[c % 6] += e12;
        if (e12 > bv) { bv = e12; bp = c % 6; }
      }
      best12[bp]++;
    }
    printf("STAGE2 phase evidence (CRB mod 6): 6-CRB aligned excess %.1f %.1f %.1f %.1f %.1f %.1f | 12-CRB %.1f %.1f %.1f %.1f %.1f %.1f | best 12-CRB block start %d %d %d %d %d %d\n",
           w6[0], w6[1], w6[2], w6[3], w6[4], w6[5], w12[0], w12[1], w12[2], w12[3], w12[4], w12[5],
           best12[0], best12[1], best12[2], best12[3], best12[4], best12[5]);
  }
  printf("STAGE2 active snapshots %d of %d at sym%d; lit CRB hull [%d, %d]; null bound rho2 > %.3f (FA 1e-4/window)\n",
         nact, nsym[sym0], sym0, hlo, hhi, 1.0 - pow(1e-4, 1.0 / (18.0 * g_st2_D - 1.0)));
  if (nact == 0)
    return 1;
  int qlo = hlo, qhi = hhi;
  st2_hull(sn, nact, &qlo, &qhi);
  printf("STAGE2 robust lit hull (2/98%% of %d lit windows): CRB [%d, %d]\n", g_st2_nlit, qlo, qhi);
  const size_t cap = 1 << 20;
  st2_hyp_t *H = malloc(sizeof(st2_hyp_t) * cap);
  size_t nh = st2_gen(H, cap, qlo, qhi);
  /* Decoy snapshots for the significance test: the SAME active snapshots correlated with three IDs
   * that are not nid -- the null is measured on this data, not assumed. */
  const int dec_id[3] = {(nid + 7919) & 0xffff, (nid + 30011) & 0xffff, (nid ^ 0x5a5a) & 0xffff};
  st2_snap_t *dec[3];
  for (int d = 0; d < 3; d++) {
    dec[d] = calloc(nact, sizeof(st2_snap_t));
    for (int k = 0; k < nact; k++) {
      dec[d][k] = st2_alloc();
      st2_build(sn[k].k, dec_id[d], &dec[d][k]);
    }
  }
  double *vr = malloc(sizeof(double) * nact), *vd = malloc(sizeof(double) * nact);
  int first_L = -1, ncs = 0;
  char b[300];
  for (int it = 0; it < 6; it++) {
    if (it > 0) { /* interleaved extents from what is LEFT: the first hull was dominated by CORESET #1 */
      if (!st2_hull(sn, nact, &qlo, &qhi)) {
        printf("STAGE2 stop after %d CORESET(s): no lit windows left\n", ncs);
        break;
      }
      nh = st2_gen(H, cap, qlo, qhi);
      printf("STAGE2   residual robust hull CRB [%d, %d]\n", qlo, qhi);
    }
    printf("STAGE2 iteration %d: scoring %zu hypotheses over %d active snapshots\n", it + 1, nh, nact);
    fflush(stdout);
    { const char *dbg = getenv("ISAC_ST2_DEBUG_HYP");
      if (dbg) { int L,R,sh,ph,g0; sscanf(dbg, "%d,%d,%d,%d,%d", &L,&R,&sh,&ph,&g0);
        st2_hyp_t probe = {ph, g0, (int)((qhi - (ph + 6*g0))/6), L, R, sh, 0.0, {0}};
        st2_score(&probe, sn, nact);
        printf("STAGE2   DEBUG probe L=%d R=%d shift=%d phase=%d g0=%d ng=%d -> score=%.4f (mean %.4f)\n",
               L, R, sh, ph, g0, probe.ng, probe.score, nact ? probe.score/nact : 0.0);
      }
    }
#pragma omp parallel for schedule(dynamic, 64)
    for (size_t i = 0; i < nh; i++) {
      st2_score(&H[i], sn, nact);
      /* JOINT-CONSISTENCY GATE, baked into ranking (2026-09-25): a candidate whose winning shift/R/extent
       * explains only FRAGMENTS of CCEs (not whole ones) when inverse-mapped is disqualified BEFORE sorting,
       * not just checked on whatever raw score ranked #1 -- a wrong-but-overlapping geometry can score higher
       * on raw excess alone than the true, correctly-shaped one (measured: 0.70 vs 0.37 on --selftest4 il),
       * so filtering only the top few ranks after the fact is not enough when the truth ranks past rank 200
       * (measured on the same test) among tens of thousands of candidates. */
      if (st2_consistency(sn, nact, &H[i]) < 0.5)
        H[i].score = 0.0;
    }
    qsort(H, nh, sizeof(st2_hyp_t), st2_cmp);
    for (size_t i = 0; i < nh && i < 3; i++) {
      st2_fmt(&H[i], nact, b, sizeof(b));
      printf("STAGE2   #%zu %s\n", i + 1, b);
    }
    if (it == 0 && !quiet) {
      int bni = -1, bil = -1;
      for (size_t i = 0; i < nh && (bni < 0 || bil < 0); i++) {
        if (!H[i].L && bni < 0) bni = (int)i;
        if (H[i].L && bil < 0) bil = (int)i;
      }
      if (bni >= 0) { st2_fmt(&H[bni], nact, b, sizeof(b)); printf("STAGE2   best non-interleaved (rank %d): %s\n", bni + 1, b); }
      if (bil >= 0) { st2_fmt(&H[bil], nact, b, sizeof(b)); printf("STAGE2   best interleaved     (rank %d): %s\n", bil + 1, b); }
    }
    /* H[0] is now guaranteed consistency-gated (score forced to 0 above otherwise, so it can only reach rank 0
     * with a genuine 0.0 raw score too, which the significance test below will reject anyway). */
    const size_t widx = 0;
    if (H[0].score <= 0.0) {
      printf("STAGE2 stop after %d CORESET(s): no consistency-gated candidate has nonzero score\n", ncs);
      break;
    }
    st2_per_snap(&H[widx], sn, nact, vr);
    double tmin = 1e30;
    for (int d = 0; d < 3; d++) {
      st2_per_snap(&H[widx], dec[d], nact, vd);
      const double t = st2_paired_t(vr, vd, nact);
      if (t < tmin) tmin = t;
    }
    if (tmin < 5.0) {
      printf("STAGE2 stop after %d CORESET(s): best remaining geometry not significant (min paired t vs decoys %.1f < 5)\n",
             ncs, tmin);
      break;
    }
    /* what decode-free evidence cannot separate: hypotheses tied with the winner */
    int ties = 0, smin = 1 << 30, smax = -1, shmin = 1 << 30, shmax = -1;
    for (size_t i = 0; i < nh; i++)
      if (H[i].L == H[widx].L && H[i].R == H[widx].R && H[i].phase == H[widx].phase
          && fabs(H[i].score - H[widx].score) <= 1e-9 * (1.0 + fabs(H[widx].score))) {
        ties++;
        const int c2 = H[i].phase + 6 * H[i].g0;
        if (c2 < smin) smin = c2;
        if (c2 > smax) smax = c2;
        if (H[i].shift < shmin) shmin = H[i].shift;
        if (H[i].shift > shmax) shmax = H[i].shift;
      }
    ncs++;
    st2_fmt(&H[widx], nact, b, sizeof(b));
    printf("STAGE2 CORESET #%d nID=%d sym=%d: %s | min t vs decoys %.1f | %d tied (start CRB %d..%d, shift %d..%d)\n",
           ncs, nid, sym0, b, tmin, ties, smin, smax, shmin, shmax);
    if (first_L < 0) first_L = H[widx].L;
    if (g_nfound < 16) {
      g_found[g_nfound] = H[widx];
      g_found_nid[g_nfound] = nid;
      g_found_sym[g_nfound] = sym0;
      g_found_dur[g_nfound] = g_st2_D;
      g_found_nties[g_nfound] = 0;
      for (size_t i = 0; i < nh && g_found_nties[g_nfound] < 64; i++)
        if (H[i].L == H[0].L && H[i].R == H[0].R && H[i].phase == H[0].phase
            && fabs(H[i].score - H[0].score) <= 1e-9 * (1.0 + fabs(H[0].score)))
          g_found_tie[g_nfound][g_found_nties[g_nfound]++] = H[i];
      g_nfound++;
    }
    int nrem = 0;
    for (int k = 0; k < nact; k++) nrem += st2_remove(&sn[k], &H[widx]);
    printf("STAGE2   removed %d candidates explained by CORESET #%d\n", nrem, ncs);
    fflush(stdout);
    if (nrem == 0) break;
  }
  return first_L < 0 ? -1 : first_L;
}

/* Synthetic stage-2 capture: Point A at grid sc 7; dedicated CORESET at CRB 60, 30 groups, duration 1,
 * mapping = non-interleaved ("ni") or interleaved L=6 R=2 shift=7 ("il"); one AL2 PDCCH (random even
 * CCE) in every 2nd symbol-0 snapshot, nID 12345, amplitude 1.5 over unit noise. */
static void selftest2_make(bool il)
{
  selftest_make(); /* noise + stage-1 plants; its symbol-3 ID-12345 plant is harmless here */
  st2_hyp_t h = {0, 10, 30, il ? 6 : 0, il ? 2 : 0, il ? 7 : 0, 0.0, {0}};
  for (int k = 0; k < g_idsw.n; k++) {
    if (g_idsw.sym[k] != 0 || (k / 14) % 2) continue;
    c16_t *re = g_idsw.re + (size_t)k * g_idsw.nsc;
    uint32_t gold[300], x1 = 0, x2 = idsw_cinit(12345, g_idsw.slot[k], 0);
    gold[0] = gold_generic(&x1, &x2, 1);
    for (int w = 1; w < 300; w++) gold[w] = gold_generic(&x1, &x2, 0);
    const int cce = 2 * (rand() % (h.ng / 2)), b = h.L ? h.L : 6, nb = 6 * h.ng / b;
    for (int j = cce * 6 / b; j < (cce + 2) * 6 / b; j++) {
      const int f = h.L ? ((j % h.R) * (nb / h.R) + j / h.R + h.shift) % nb : j;
      for (int c = 60 + f * b; c < 60 + f * b + b; c++)
        for (int q = 0; q < 3; q++) {
          float xr, xi;
          idsw_pilot(gold, 3 * c + q, &xr, &xi);
          const int sc = g_idsw.point_a_sc + 12 * c + 1 + 4 * q;
          re[sc].r += (int16_t)(1.5 * 700 * xr);
          re[sc].i += (int16_t)(-1.5 * 700 * xi);
        }
    }
  }
}



/* Synthetic 2-SYMBOL CORESET capture (ISW2, dur = 2): Point A at grid sc 7; a duration-2 CORESET at CRB 60, 30
 * groups (N_REG = 360), starting at symbol 0, nID 12345, non-interleaved ("ni") or interleaved L=6 R=2 shift=7
 * ("il"); one AL2 PDCCH (random even CCE) in every 2nd slot's windows that start at symbol 0 or 1.
 * The DM-RS is planted from the SPEC's REG numbering, written out REG by REG (time-first: REG n = RB n/D, symbol
 * n%D), NOT from the analysis code's bundle-extent shortcut -- so the two are independent and their agreement is
 * evidence the shortcut is right. */
static void selftest4_make(bool il)
{
  const int D = 2, nid = 12345, cs = 60, ng = 30, Lb = 6, R = 2, sh = 7, nreg = 6 * ng * D, nb = nreg / Lb;
  g_idsw.nsc = 3276; g_idsw.point_a_sc = 7; g_idsw.css0_crb = 10; g_idsw.css0_nrb = 48; g_idsw.pci = 2;
  g_idsw.sps = 14; g_idsw.n = 14 * 24; g_idsw.dur = D;
  g_idsw.slot = malloc(sizeof(int) * g_idsw.n); g_idsw.sym = malloc(sizeof(int) * g_idsw.n);
  g_idsw.re = malloc(sizeof(c16_t) * (size_t)g_idsw.n * D * g_idsw.nsc);
  srand(4);
  for (int k = 0; k < g_idsw.n; k++) {
    g_idsw.slot[k] = rand() % 20; g_idsw.sym[k] = k % 14;
    for (int d = 0; d < D; d++) {
      c16_t *re = g_idsw.re + ((size_t)k * D + d) * g_idsw.nsc;
      for (int i = 0; i < g_idsw.nsc; i++) {
        double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0), r = 700 * sqrt(-2 * log(u1));
        re[i].r = (int16_t)(r * cos(2 * M_PI * u2)); re[i].i = (int16_t)(r * sin(2 * M_PI * u2));
      }
    }
  }
  for (int k = 0; k < g_idsw.n; k++) {
    if (g_idsw.sym[k] > 1 || (k / 14) % 2) continue;
    const int cce = 2 * (rand() % (nreg / 6 / 2));
    for (int j = cce; j < cce + 2; j++) { /* bundle index of this CCE's bundles (L = 6: one bundle per CCE) */
      int f = j;
      if (il) { const int C = nb / R, r = j % R, c = j / R; f = (r * C + c + sh) % nb; } /* 38.211 7.3.2.2 */
      for (int n = f * Lb; n < f * Lb + Lb; n++) { /* the bundle's REGs */
        const int rb = n / D, l = n % D;            /* time-first numbering; the CORESET starts at symbol 0 */
        const int d = l - g_idsw.sym[k];            /* which slice of THIS window carries symbol l */
        if (d < 0 || d >= D) continue;
        c16_t *re = g_idsw.re + ((size_t)k * D + d) * g_idsw.nsc;
        uint32_t gold[300], x1 = 0, x2 = idsw_cinit(nid, g_idsw.slot[k], l);
        gold[0] = gold_generic(&x1, &x2, 1);
        for (int w = 1; w < 300; w++) gold[w] = gold_generic(&x1, &x2, 0);
        for (int q = 0; q < 3; q++) {
          float xr, xi;
          const int c = cs + rb;
          idsw_pilot(gold, 3 * c + q, &xr, &xi);
          const int sc = g_idsw.point_a_sc + 12 * c + 1 + 4 * q;
          re[sc].r += (int16_t)(1.5 * 700 * xr);
          re[sc].i += (int16_t)(-1.5 * 700 * xi);
        }
      }
    }
  }
}


/* AL1-ONLY pathological case (2026-09-24 macro-readiness notes): a NON-interleaved CORESET whose PDCCHs are
 * ALWAYS sent at AL1 (one 6-RB bundle = one CCE, since a non-interleaved bundle IS a CCE at L=6/full-size).
 * With only ONE bundle observed per occasion, the per-snapshot best-CCE scorer has nearly no constraining power
 * at AL1 for an interleaved hypothesis: a single point is explainable by SOME shift under nearly every
 * interleaved (L,R) pairing, since interleaving is just a permutation. Ground truth: non-interleaved, CRB 60,
 * 30 groups (= 30 possible CCEs), a different random CCE each occasion (matching real AL1-heavy scheduling,
 * which reuses the SS's CCE candidates rather than always the same one). */
static void selftest5_make(void)
{
  const int nid = 12345, cs = 60, ng = 30;
  g_idsw.nsc = 3276; g_idsw.point_a_sc = 7; g_idsw.css0_crb = 10; g_idsw.css0_nrb = 48; g_idsw.pci = 2;
  g_idsw.sps = 14; g_idsw.n = 14 * 24; g_idsw.dur = 1;
  g_idsw.slot = malloc(sizeof(int) * g_idsw.n); g_idsw.sym = malloc(sizeof(int) * g_idsw.n);
  g_idsw.re = malloc(sizeof(c16_t) * (size_t)g_idsw.n * g_idsw.nsc);
  srand(5);
  for (int k = 0; k < g_idsw.n; k++) {
    g_idsw.slot[k] = rand() % 20; g_idsw.sym[k] = k % 14;
    c16_t *re0 = g_idsw.re + (size_t)k * g_idsw.nsc;
    for (int i = 0; i < g_idsw.nsc; i++) {
      double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0), r = 700 * sqrt(-2 * log(u1));
      re0[i].r = (int16_t)(r * cos(2 * M_PI * u2)); re0[i].i = (int16_t)(r * sin(2 * M_PI * u2));
    }
  }
  for (int k = 0; k < g_idsw.n; k++) {
    if (g_idsw.sym[k] != 0 || (k / 14) % 2) continue; /* one occasion in every 2nd slot, like selftest2/4 */
    const int cce = rand() % ng; /* AL1: exactly one CCE = one 6-RB bundle, no interleaving to undo */
    c16_t *re = g_idsw.re + (size_t)k * g_idsw.nsc;
    uint32_t gold[300], x1 = 0, x2 = idsw_cinit(nid, g_idsw.slot[k], 0);
    gold[0] = gold_generic(&x1, &x2, 1);
    for (int w = 1; w < 300; w++) gold[w] = gold_generic(&x1, &x2, 0);
    for (int c = cs + cce * 6; c < cs + cce * 6 + 6; c++)
      for (int q = 0; q < 3; q++) {
        float xr, xi;
        idsw_pilot(gold, 3 * c + q, &xr, &xi);
        const int sc = g_idsw.point_a_sc + 12 * c + 1 + 4 * q;
        re[sc].r += (int16_t)(1.5 * 700 * xr);
        re[sc].i += (int16_t)(-1.5 * 700 * xi);
      }
  }
}

/* Plant one AL2 PDCCH of hypothesis h (CRB-0 referenced DM-RS of nid) into snapshot k. */
static void st3_plant(int k, int nid, const st2_hyp_t *h)
{
  c16_t *re = g_idsw.re + (size_t)k * g_idsw.nsc;
  uint32_t gold[300], x1 = 0, x2 = idsw_cinit(nid, g_idsw.slot[k], g_idsw.sym[k]);
  gold[0] = gold_generic(&x1, &x2, 1);
  for (int w = 1; w < 300; w++) gold[w] = gold_generic(&x1, &x2, 0);
  const int cce = 2 * (rand() % (h->ng / 2)), b = h->L ? h->L : 6, nb = 6 * h->ng / b, cs = h->phase + 6 * h->g0;
  for (int j = cce * 6 / b; j < (cce + 2) * 6 / b; j++) {
    const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
    for (int c = cs + f * b; c < cs + f * b + b; c++)
      for (int q = 0; q < 3; q++) {
        float xr, xi;
        idsw_pilot(gold, 3 * c + q, &xr, &xi);
        const int sc = g_idsw.point_a_sc + 12 * c + 1 + 4 * q;
        re[sc].r += (int16_t)(1.5 * 700 * xr);
        re[sc].i += (int16_t)(-1.5 * 700 * xi);
      }
  }
}

/* Three CORESETs, Point A at grid sc 7:
 *   A: nID 12345, NON-interleaved, CRB 60, 15 groups, symbol 0
 *   C: nID 12345, interleaved L6 R2 shift 3, CRB 162, 16 groups, symbol 0   (same nID and symbol as A)
 *   B: nID 777,   NON-interleaved, CRB 60, 20 groups, symbol 5
 * One AL2 PDCCH per CORESET in every 2nd capture of its symbol. */
static const st2_hyp_t ST3_A = {0, 10, 15, 0, 0, 0, 0.0, {0}}, ST3_C = {0, 27, 16, 6, 2, 3, 0.0, {0}},
                       ST3_B = {0, 10, 20, 0, 0, 0, 0.0, {0}};
static void selftest3_make(void)
{
  g_idsw.nsc = 3276; g_idsw.point_a_sc = 7; g_idsw.css0_crb = 10; g_idsw.css0_nrb = 48; g_idsw.pci = 2;
  g_idsw.sps = 14; g_idsw.n = 14 * 24; g_idsw.dur = 1; /* single-symbol synthetic scene */
  g_idsw.slot = malloc(sizeof(int) * g_idsw.n); g_idsw.sym = malloc(sizeof(int) * g_idsw.n);
  g_idsw.re = malloc(sizeof(c16_t) * (size_t)g_idsw.n * g_idsw.nsc);
  srand(3);
  for (int k = 0; k < g_idsw.n; k++) {
    g_idsw.slot[k] = rand() % 20; g_idsw.sym[k] = k % 14;
    c16_t *re = g_idsw.re + (size_t)k * g_idsw.nsc;
    for (int i = 0; i < g_idsw.nsc; i++) {
      double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0), r = 700 * sqrt(-2 * log(u1));
      re[i].r = (int16_t)(r * cos(2 * M_PI * u2)); re[i].i = (int16_t)(r * sin(2 * M_PI * u2));
    }
    if ((k / 14) % 2) continue;
    if (g_idsw.sym[k] == 0) { st3_plant(k, 12345, &ST3_A); st3_plant(k, 12345, &ST3_C); }
    if (g_idsw.sym[k] == 5) st3_plant(k, 777, &ST3_B);
  }
}


/* sorted CRB set of one candidate */
static int st3_rbset(const st2_hyp_t *h, int cce, int al, int *out)
{
  const int b = h->L ? h->L : 6, nb = 6 * h->ng / b, cs = h->phase + 6 * h->g0;
  int n = 0;
  for (int j = cce * 6 / b; j < (cce + al) * 6 / b; j++) {
    const int f = h->L ? ((j % h->R) * (nb / h->R) + j / h->R + h->shift) % nb : j;
    for (int c = cs + f * b; c < cs + f * b + b; c++) out[n++] = c;
  }
  for (int i = 1; i < n; i++) { int x = out[i], j = i - 1; while (j >= 0 && out[j] > x) { out[j + 1] = out[j]; j--; } out[j + 1] = x; }
  return n;
}
/* every AL2 candidate RB set of truth t is an AL2 candidate RB set of found geometry f */
static bool st3_equiv(const st2_hyp_t *t, const st2_hyp_t *f)
{
  int a[64], b2[64];
  for (int c = 0; c + 2 <= t->ng; c += 2) {
    const int na = st3_rbset(t, c, 2, a);
    bool hit = false;
    for (int d = 0; d + 2 <= f->ng && !hit; d += 2) {
      const int nb = st3_rbset(f, d, 2, b2);
      hit = na == nb && !memcmp(a, b2, sizeof(int) * na);
    }
    if (!hit) return false;
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s files... | --selftest\n", argv[0]); return 2; }
  if (strcmp(argv[1], "--selftest2") == 0 && argc > 2) { /* ni | il */
    const bool il = strcmp(argv[2], "il") == 0;
    selftest2_make(il);
    const int top_L = stage2(12345, 0, false);
    const bool pass = il ? (top_L == 6) : (top_L == 0);
    printf("SELFTEST2 planted=%s top=%s %s\n", il ? "interleaved L6" : "non-interleaved",
           top_L ? "interleaved" : "non-interleaved", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  if (strcmp(argv[1], "--selftest4") == 0 && argc > 2) { /* ni | il : a duration-2 CORESET, dur=2 capture */
    const bool il = strcmp(argv[2], "il") == 0;
    selftest4_make(il);
    const int top_L = stage2(12345, 0, false);
    const bool pass = g_nfound >= 1 && g_found_dur[0] == 2 && g_found[0].ng >= 2
                      && (il ? (top_L == 6 && g_found[0].R == 2) : (top_L == 0));
    printf("SELFTEST4 planted=D2 %s top_L=%d dur=%d %s\n", il ? "interleaved L6 R2" : "non-interleaved", top_L,
           g_nfound ? g_found_dur[0] : -1, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  if (strcmp(argv[1], "--selftest5") == 0) { /* AL1-only non-interleaved: the pathological case, ground truth
                                                * top_L == 0 */
    selftest5_make();
    const int top_L = stage2(12345, 0, false);
    const bool pass = (top_L == 0);
    printf("SELFTEST5 planted=AL1-only-non-interleaved top_L=%d %s\n", top_L, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  if (strcmp(argv[1], "--selftest3") == 0) {
    for (int r = 0; r < 2; r++) {
      g_idsw.zsum[r] = calloc((size_t)IDSW_NSYM * IDSW_NID, sizeof(float));
      g_idsw.hits[r] = calloc((size_t)IDSW_NSYM * IDSW_NID, sizeof(uint16_t));
    }
    selftest3_make();
    idsw_process();
    idsw_list();
    for (int i = 0; i < g_list_n; i++) {
      bool covered = false; /* a symbol already inside an accepted CORESET of the same nID */
      for (int j = 0; j < i; j++) covered |= g_list_id[j] == g_list_id[i];
      if (!covered) stage2(g_list_id[i], g_list_sym[i], true);
    }
    /* pass: list == {(12345,0),(777,5)}; CORESETs found == {A, C} for 12345 and {B} for 777 */
    bool list_ok = g_list_n == 2;
    for (int i = 0; i < g_list_n; i++)
      list_ok &= (g_list_id[i] == 12345 && g_list_sym[i] == 0) || (g_list_id[i] == 777 && g_list_sym[i] == 5);
    int gotA = 0, gotC = 0, gotB = 0, other = 0;
    for (int i = 0; i < g_nfound; i++) {
      const st2_hyp_t *f = &g_found[i];
      const int cs = f->phase + 6 * f->g0;
      if (g_found_nid[i] == 12345 && f->L == 0 && f->phase == 0 && cs <= 60 && (60 - cs) % 12 == 0) gotA++;
      else if (g_found_nid[i] == 12345 && f->L) { /* truth inside the reported tie class (same RB sets) */
        bool in = false;
        for (int t = 0; t < g_found_nties[i] && !in; t++) in = st3_equiv(&ST3_C, &g_found_tie[i][t]);
        printf("SELFTEST3 interleaved CORESET tie class: %d members, truth %s\n", g_found_nties[i], in ? "INSIDE" : "not inside");
        if (in) gotC++; else other++;
      }
      else if (g_found_nid[i] == 777 && f->L == 0 && f->phase == 0 && cs <= 60 && (60 - cs) % 12 == 0) gotB++;
      else other++;
    }
    const bool pass = list_ok && gotA == 1 && gotC == 1 && gotB == 1 && other == 0;
    printf("SELFTEST3 list=%s A=%d C=%d B=%d other=%d %s\n", list_ok ? "ok" : "WRONG", gotA, gotC, gotB, other,
           pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  if (strcmp(argv[1], "--stage2") == 0 && argc > 3) { /* --stage2 <stage-1 report> files... */
    /* stage-1 hand-off: every STAGE1LIST region=rest entry (fallback: the top line of the last report) */
    FILE *r = fopen(argv[2], "r");
    char line[2048];
    int lid[40], lsym[40], nl = 0, tid = -1, tsym = -1;
    while (r && fgets(line, sizeof(line), r)) {
      int a, b;
      if (sscanf(line, "STAGE1LIST region=rest id=%d sym=%d", &a, &b) == 2 && nl < 40) { lid[nl] = a; lsym[nl++] = b; }
      const char *t = strstr(line, "region=rest") ? strstr(line, "top: id=") : NULL;
      if (t) sscanf(t, "top: id=%d/sym%d", &tid, &tsym);
    }
    if (r) fclose(r);
    if (nl == 0 && tid >= 0) { lid[0] = tid; lsym[0] = tsym; nl = 1; }
    if (nl == 0) { fprintf(stderr, "no stage-1 result in %s\n", argv[2]); return 2; }
    for (int i = 0; i < nl; i++) printf("STAGE2 input from stage 1 (%s): nID=%d start symbol=%d\n", argv[2], lid[i], lsym[i]);
    /* concatenate all files' snapshots */
    int tot = 0; int *sl = NULL, *sy = NULL; c16_t *rr = NULL;
    for (int i = 3; i < argc; i++) {
      if (load(argv[i]) != 0) continue;
      /* stride is dur*nsc per snapshot (2026-09-24 "ISW2" format); dur=1 reduces to the original
       * layout. Files are assumed to share one dur, exactly like the pre-existing, unchecked nsc
       * assumption right below -- not newly introduced here. */
      sl = realloc(sl, sizeof(int) * (tot + g_idsw.n)); sy = realloc(sy, sizeof(int) * (tot + g_idsw.n));
      rr = realloc(rr, sizeof(c16_t) * (size_t)(tot + g_idsw.n) * g_idsw.dur * g_idsw.nsc);
      memcpy(sl + tot, g_idsw.slot, sizeof(int) * g_idsw.n); memcpy(sy + tot, g_idsw.sym, sizeof(int) * g_idsw.n);
      memcpy(rr + (size_t)tot * g_idsw.dur * g_idsw.nsc, g_idsw.re,
             sizeof(c16_t) * (size_t)g_idsw.n * g_idsw.dur * g_idsw.nsc);
      tot += g_idsw.n;
    }
    g_idsw.slot = sl; g_idsw.sym = sy; g_idsw.re = rr; g_idsw.n = tot;
    for (int i = 0; i < nl; i++) {
      bool seen = false; /* further symbols of an nID already analysed belong to its CORESETs */
      for (int j = 0; j < i; j++) seen |= lid[j] == lid[i];
      if (!seen) stage2(lid[i], lsym[i], false);
    }
    printf("STAGE2 SUMMARY: %d CORESET(s)\n", g_nfound);
    /* hand-off to the live receiver (nr_pdcch_blind_monitor_discovered_poll). Grid RB = CRB + Point A/12,
     * representable only when Point A falls on an RB boundary of the grid; otherwise say so. The lowest
     * start of a non-interleaved tie class is used: a superset that decodes at every class member. */
    if (((g_idsw.point_a_sc % 12) + 12) % 12 != 0) {
      printf("STAGE2 HANDOFF not written: Point A at grid subcarrier %d is not RB-aligned\n", g_idsw.point_a_sc);
    } else if (g_nfound > 0) {
      FILE *o = fopen("/tmp/coresets_discovered.tmp", "w");
      if (!o) perror("STAGE2 HANDOFF /tmp/coresets_discovered.tmp");
      for (int i = 0; o && i < g_nfound; i++) {
        st2_hyp_t h = g_found[i];
        if (!h.L)
          for (int t = 0; t < g_found_nties[i]; t++)
            if (g_found_tie[i][t].g0 * 6 + g_found_tie[i][t].phase < h.g0 * 6 + h.phase) {
              h = g_found_tie[i][t];
              h.ng = (g_st2_chi - (h.phase + 6 * h.g0)) / 6;
            }
        const int grid_rb = h.phase + 6 * h.g0 + g_idsw.point_a_sc / 12;
        fprintf(o, "CORESET nid=%d sym=%d dur=%d grid_rb=%d groups=%d L=%d R=%d shift=%d ties=%d\n", g_found_nid[i],
                g_found_sym[i], g_found_dur[i], grid_rb, h.ng, h.L, h.R, h.shift, g_found_nties[i]);
        printf("STAGE2 HANDOFF CORESET nid=%d sym=%d grid_rb=%d groups=%d L=%d R=%d shift=%d\n", g_found_nid[i],
               g_found_sym[i], grid_rb, h.ng, h.L, h.R, h.shift);
      }
      if (o) { fclose(o); rename("/tmp/coresets_discovered.tmp", "/tmp/coresets_discovered.txt"); }
    }
    for (int i = 0; i < g_nfound; i++) {
      char b[300];
      st2_fmt(&g_found[i], 1, b, sizeof(b));
      printf("STAGE2 SUMMARY #%d nID=%d %s (%d tied)\n", i + 1, g_found_nid[i], b, g_found_nties[i]);
    }
    return 0;
  }
  const bool st = strcmp(argv[1], "--selftest") == 0;
  for (int r = 0; r < 2; r++) {
    g_idsw.zsum[r] = calloc((size_t)IDSW_NSYM * IDSW_NID, sizeof(float));
    g_idsw.hits[r] = calloc((size_t)IDSW_NSYM * IDSW_NID, sizeof(uint16_t));
  }
  if (st) {
    g_idsw.sps = 14;
    selftest_make();
    idsw_process();
    idsw_list();
    /* pass = both planted IDs top their region on the planted symbol */
    int ok = 1;
    for (int r = 0; r < 2; r++) {
      const int want_id = r ? 12345 : 2, want_sym = r ? 3 : 0;
      float best = -1; int bi = -1, bs = -1;
      for (int s = 0; s < IDSW_NSYM; s++)
        for (int id = 0; id < IDSW_NID; id++) {
          const float v = g_idsw.hits[r][s * IDSW_NID + id] + g_idsw.zsum[r][s * IDSW_NID + id] * 1e-4f;
          if (v > best) { best = v; bi = id; bs = s; }
        }
      printf("SELFTEST region=%s top=%d/sym%d want=%d/sym%d %s\n", r ? "rest" : "css0", bi, bs, want_id, want_sym,
             (bi == want_id && bs == want_sym) ? "PASS" : "FAIL");
      ok &= bi == want_id && bs == want_sym;
    }
    return ok ? 0 : 1;
  }
  for (int i = 1; i < argc; i++)
    if (load(argv[i]) == 0)
      idsw_process();
  idsw_list();
  return 0;
}
