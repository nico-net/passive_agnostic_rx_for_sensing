/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI shared channelizer: attaches the wideband ring (read-only, same overrun/discontinuity/producer-lost rules as shm_iq.c) and
 * writes one NARROW ring per cell, <ring>_<tag>: same channels, rate = ring rate / decim, centre = cell centre, header meta copied.
 * Per cell and channel the work is iq_ddc (mixer to the cell centre + Kaiser halfband decimation), so a cell process attaching to
 * its ring at decim 1 (memcpy path of libshm_iq.so) gets what a direct wideband consumer would. Output timestamps are
 * (base_tick + ring_pos) / decim, the formula of shm_iq.c.
 *   shm_iq_chan --ring R --cell TAG:CENTRE_HZ:RATE[:BW_HZ] ... | --cells-json plan.json  [--threads N --cores L --gpu --block N]
 *   shm_iq_chan --selftest [--gpu]      synthetic tones: gain / alias / emulated-kernel and GPU-vs-CPU difference
 *   shm_iq_chan --bench SEC --cell ...  CPU/GPU throughput on synthetic noise, no ring
 * Backends: CPU = one iq_ddc per (cell, channel) on a thread pool; GPU = libshm_iq_chan_gpu.so (shm_iq_chan_gpu.cu). */
#define _GNU_SOURCE
#include "iq_ddc.h"
#include "shm_iq_chan_gpu.h"
#include "shm_iq_chan_kern.h"
#include "shm_iq_ring.h"
#include <complex.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXCELL 16
#define MAXCORES 64

typedef struct {
  char tag[32];
  double centre, rate, bw;
  int decim;              /* 0 = from rate */
  shm_iq_ring_t out;
  iq_ddc_t ddc[SHM_IQ_MAX_CH];
  int16_t *obuf[SHM_IQ_MAX_CH];
  uint64_t away_until;    /* dwell_until mirrored into the output ring (0 = not away) */
} cell_t;

typedef struct {
  int ncell, nch, threads, block, gpu, ncores, cores[MAXCORES];
  double in_rate, atten_db;
  cell_t cell[MAXCELL];
  const int16_t *const *in; /* current block, nch channels */
  /* pool */
  pthread_t th[MAXCORES];
  pthread_mutex_t mu;
  pthread_cond_t go, done;
  int gen, next, pending, quit;
  /* gpu */
  void *dl, *gpu_h;
  void *(*g_create)(double, int, int, const iq_ddc_t *const *, int);
  int (*g_shift)(void *, int, const iq_ddc_t *);
  void (*g_reset)(void *);
  int (*g_run)(void *, const int16_t *const *, int, int16_t *const *);
  float (*g_ms)(void *);
  void (*g_destroy)(void *);
} chan_t;

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

/* ---------------------------------------------------------------- CPU backend */
static void run_job(chan_t *c, int j, int n)
{
  cell_t *ce = &c->cell[j / c->nch];
  const int a = j % c->nch;
  iq_ddc_process(&ce->ddc[a], c->in[a], n, ce->obuf[a]);
}

static int g_job_n;
static void *worker(void *arg)
{
  chan_t *c = arg;
  int seen = 0;
  for (;;) {
    pthread_mutex_lock(&c->mu);
    while (c->gen == seen && !c->quit) pthread_cond_wait(&c->go, &c->mu);
    if (c->quit) { pthread_mutex_unlock(&c->mu); return NULL; }
    seen = c->gen;
    pthread_mutex_unlock(&c->mu);
    for (;;) {
      /* acquire pairs with cpu_run's release store: a worker still draining the previous generation can grab a job of the
       * next one without passing the mutex, and must then see that block's input and the DDC state other workers left */
      const int j = __atomic_fetch_add(&c->next, 1, __ATOMIC_ACQUIRE);
      if (j >= c->ncell * c->nch) break;
      run_job(c, j, g_job_n);
      pthread_mutex_lock(&c->mu);
      if (--c->pending == 0) pthread_cond_signal(&c->done);
      pthread_mutex_unlock(&c->mu);
    }
  }
}

static void cpu_run(chan_t *c, int n)
{
  if (c->threads <= 0) {
    for (int j = 0; j < c->ncell * c->nch; ++j) run_job(c, j, n);
    return;
  }
  pthread_mutex_lock(&c->mu);
  g_job_n = n; c->pending = c->ncell * c->nch; c->gen++;
  __atomic_store_n(&c->next, 0, __ATOMIC_RELEASE);
  pthread_cond_broadcast(&c->go);
  while (c->pending) pthread_cond_wait(&c->done, &c->mu);
  pthread_mutex_unlock(&c->mu);
}

static void pool_start(chan_t *c)
{
  pthread_mutex_init(&c->mu, NULL); pthread_cond_init(&c->go, NULL); pthread_cond_init(&c->done, NULL);
  for (int i = 0; i < c->threads; ++i) {
    pthread_create(&c->th[i], NULL, worker, c);
    if (c->ncores) {
      cpu_set_t s;
      CPU_ZERO(&s); CPU_SET(c->cores[i % c->ncores], &s);
      pthread_setaffinity_np(c->th[i], sizeof(s), &s);
    }
  }
}

static void pool_stop(chan_t *c)
{
  pthread_mutex_lock(&c->mu); c->quit = 1; pthread_cond_broadcast(&c->go); pthread_mutex_unlock(&c->mu);
  for (int i = 0; i < c->threads; ++i) pthread_join(c->th[i], NULL);
  c->threads = 0;
}

/* ---------------------------------------------------------------- GPU backend (dlopen) */
static int gpu_load(chan_t *c)
{
  char path[512], exe[400];
  const char *env = getenv("ISAC_SHM_IQ_CHAN_GPU_LIB");
  ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (k > 0) { exe[k] = 0; snprintf(path, sizeof(path), "%s/libshm_iq_chan_gpu.so", dirname(exe)); } else snprintf(path, sizeof(path), "libshm_iq_chan_gpu.so");
  c->dl = dlopen(env && *env ? env : path, RTLD_NOW);
  if (!c->dl) { fprintf(stderr, "SHMIQ_CHAN_ERR --gpu: %s (build with -DENABLE_LDPC_CUDA=ON)\n", dlerror()); return -1; }
  int (*abi)(void) = (int (*)(void))dlsym(c->dl, "chan_gpu_abi");
  c->g_create = (void *(*)(double, int, int, const iq_ddc_t *const *, int))dlsym(c->dl, "chan_gpu_create");
  c->g_shift = (int (*)(void *, int, const iq_ddc_t *))dlsym(c->dl, "chan_gpu_set_shift");
  c->g_reset = (void (*)(void *))dlsym(c->dl, "chan_gpu_reset");
  c->g_run = (int (*)(void *, const int16_t *const *, int, int16_t *const *))dlsym(c->dl, "chan_gpu_run");
  c->g_ms = (float (*)(void *))dlsym(c->dl, "chan_gpu_last_ms");
  c->g_destroy = (void (*)(void *))dlsym(c->dl, "chan_gpu_destroy");
  if (!abi || abi() != CHAN_GPU_ABI || !c->g_create || !c->g_shift || !c->g_reset || !c->g_run || !c->g_ms || !c->g_destroy) {
    fprintf(stderr, "SHMIQ_CHAN_ERR GPU module ABI mismatch\n");
    return -1;
  }
  return 0;
}

static int gpu_start(chan_t *c)
{
  const iq_ddc_t *des[MAXCELL];
  for (int k = 0; k < c->ncell; ++k) des[k] = &c->cell[k].ddc[0];
  c->gpu_h = c->g_create(c->in_rate, c->nch, c->ncell, des, c->block);
  return c->gpu_h ? 0 : -1;
}

/* ---------------------------------------------------------------- common */
static void set_shifts(chan_t *c, double ring_centre)
{
  for (int k = 0; k < c->ncell; ++k) {
    cell_t *ce = &c->cell[k];
    const double shift = ce->centre - ring_centre;
    for (int a = 0; a < c->nch; ++a) iq_ddc_set_shift(&ce->ddc[a], shift);
    if (c->gpu_h) c->g_shift(c->gpu_h, k, &ce->ddc[0]);
    if (fabs(shift) + 0.5 * ce->bw > 0.5 * c->in_rate)
      fprintf(stderr, "SHMIQ_CHAN_WARN cell=%s band %.3f MHz +- %.3f MHz leaves the ring window\n", ce->tag, ce->centre / 1e6, 0.5 * ce->bw / 1e6);
    fprintf(stderr, "SHMIQ_CHAN_TUNE cell=%s ring_centre_hz=%.3f cell_centre_hz=%.3f shift_hz=%.3f decim=%d\n", ce->tag, ring_centre, ce->centre, shift,
            ce->decim);
  }
}

static void reset_all(chan_t *c)
{
  for (int k = 0; k < c->ncell; ++k)
    for (int a = 0; a < c->nch; ++a) iq_ddc_reset(&c->cell[k].ddc[a]);
  if (c->gpu_h) c->g_reset(c->gpu_h);
}

/* resolve decim / bandwidth, design the filters, allocate output blocks; in_rate and nch must be known */
static int chan_setup(chan_t *c)
{
  for (int k = 0; k < c->ncell; ++k) {
    cell_t *ce = &c->cell[k];
    if (!ce->decim) {
      const double r = c->in_rate / ce->rate;
      ce->decim = (int)lrint(r);
      if (fabs(r - ce->decim) > 1e-9) { fprintf(stderr, "SHMIQ_CHAN_ERR cell %s: ring rate %.0f / cell rate %.0f is not an integer\n", ce->tag, c->in_rate, ce->rate); return -1; }
    }
    if (ce->decim < 1 || ce->decim > 16 || (ce->decim & (ce->decim - 1))) { fprintf(stderr, "SHMIQ_CHAN_ERR cell %s: decim %d not 1,2,4,8,16\n", ce->tag, ce->decim); return -1; }
    ce->rate = c->in_rate / ce->decim;
    if (c->block % ce->decim) return -1;
    /* same passband rule as the shm_iq.c consumer: the cell's channel bandwidth, else 95 % of its sample rate */
    const double pass = 0.5 * (ce->bw > 0 && ce->bw < 0.95 * ce->rate ? ce->bw : 0.95 * ce->rate);
    if (ce->bw <= 0 || ce->bw >= 0.95 * ce->rate) ce->bw = 0.95 * ce->rate;
    for (int a = 0; a < c->nch; ++a) {
      if (iq_ddc_init(&ce->ddc[a], c->in_rate, ce->decim, pass, c->atten_db)) { fprintf(stderr, "SHMIQ_CHAN_ERR cell %s: DDC design failed (passband %.0f Hz)\n", ce->tag, pass); return -1; }
      ce->obuf[a] = malloc((size_t)(c->block / ce->decim) * 4);
      if (!ce->obuf[a]) return -1;
    }
  }
  if (c->gpu) { c->threads = 0; if (gpu_load(c) || gpu_start(c)) return -1; } /* no pool: chan_free must not join threads never started */
  else if (c->threads > 0) pool_start(c);
  return 0;
}

static void chan_free(chan_t *c)
{
  if (c->threads > 0) pool_stop(c);
  if (c->gpu_h) c->g_destroy(c->gpu_h);
  for (int k = 0; k < c->ncell; ++k)
    for (int a = 0; a < c->nch; ++a) { iq_ddc_free(&c->cell[k].ddc[a]); free(c->cell[k].obuf[a]); }
}

/* returns 0, or -1 on a GPU error (the caller stops cleanly) */
static int chan_block(chan_t *c, const int16_t *const *in, int n, double *gpu_ms)
{
  c->in = in;
  if (c->gpu) {
    int16_t *outs[MAXCELL * SHM_IQ_MAX_CH];
    for (int k = 0; k < c->ncell; ++k)
      for (int a = 0; a < c->nch; ++a) outs[k * c->nch + a] = c->cell[k].obuf[a];
    if (c->g_run(c->gpu_h, in, n, outs)) { fprintf(stderr, "SHMIQ_CHAN_ERR GPU run failed\n"); return -1; }
    if (gpu_ms) *gpu_ms = c->g_ms(c->gpu_h);
  } else cpu_run(c, n);
  return 0;
}

/* ---------------------------------------------------------------- plan.json (tolerant scanner, no JSON library in the tree) */
static const char *jkey(const char *s, const char *end, const char *key)
{
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(s, pat);
  if (!p || p >= end) return NULL;
  p = strchr(p + strlen(pat), ':');
  if (!p || p >= end) return NULL;
  do ++p; while (*p == ' ' || *p == '\n' || *p == '\t');
  return p;
}

static int load_plan(chan_t *c, const char *file, char *ring, size_t ringlen)
{
  FILE *f = fopen(file, "r");
  if (!f) { perror(file); return -1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  char *s = calloc(1, (size_t)len + 1);
  if (!s || fread(s, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(s); return -1; }
  fclose(f);
  const char *end = s + len;
  const char *r = jkey(s, end, "ring");
  if (r && *r == '"' && !ring[0]) { r++; snprintf(ring, ringlen, "%.*s", (int)strcspn(r, "\""), r); }
  /* the cells array is the one whose first element is an object (windows[].cells lists tags) */
  const char *p = s;
  for (;;) {
    p = strstr(p, "\"cells\"");
    if (!p) { free(s); fprintf(stderr, "no cells in %s\n", file); return -1; }
    p += 7;
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\t') ++p;
    if (*p == '[') { const char *q = p + 1; while (*q == ' ' || *q == '\n' || *q == '\t') ++q; if (*q == '{') break; }
  }
  for (const char *q = p + 1; *q && *q != ']';) {
    if (*q != '{') { ++q; continue; }
    int depth = 0;
    const char *e = q;
    do { if (*e == '{') depth++; else if (*e == '}') depth--; ++e; } while (depth && *e);
    if (c->ncell >= MAXCELL) { free(s); return -1; }
    cell_t *ce = &c->cell[c->ncell];
    const char *t = jkey(q, e, "tag"), *C = jkey(q, e, "C_hz"), *d = jkey(q, e, "decim"), *bw = jkey(q, e, "bw_hz"), *gr = jkey(q, e, "r");
    if (!t || *t != '"' || !C || !(d || gr)) { fprintf(stderr, "plan cell needs tag, C_hz and decim (or r)\n"); free(s); return -1; }
    snprintf(ce->tag, sizeof(ce->tag), "%.*s", (int)strcspn(t + 1, "\""), t + 1);
    ce->centre = atof(C); ce->bw = bw ? atof(bw) : 0;
    if (d) ce->decim = atoi(d);
    else { /* grid r (N_RB) -> sample rate, the table of mpci_auto.py RATE_OF; decim follows from the ring rate */
      const int g = atoi(gr);
      ce->rate = g == 51 ? 30.72e6 : g <= 162 ? 61.44e6 : 122.88e6;
    }
    c->ncell++;
    q = e;
  }
  free(s);
  return 0;
}

static int parse_cell(chan_t *c, const char *a)
{
  if (c->ncell >= MAXCELL) return -1;
  cell_t *ce = &c->cell[c->ncell];
  char *dup = strdup(a), *save, *tag = strtok_r(dup, ":", &save), *C = strtok_r(NULL, ":", &save), *R = strtok_r(NULL, ":", &save), *B = strtok_r(NULL, ":", &save);
  if (!tag || !C || !R) { free(dup); return -1; }
  snprintf(ce->tag, sizeof(ce->tag), "%s", tag);
  ce->centre = atof(C); ce->rate = atof(R); ce->bw = B ? atof(B) : 0;
  free(dup);
  c->ncell++;
  return 0;
}

static void parse_cores(chan_t *c, const char *s)
{
  char *d = strdup(s), *save;
  for (char *t = strtok_r(d, ",", &save); t && c->ncores < MAXCORES; t = strtok_r(NULL, ",", &save)) {
    int lo, hi;
    if (sscanf(t, "%d-%d", &lo, &hi) == 2) { for (int k = lo; k <= hi && c->ncores < MAXCORES; ++k) c->cores[c->ncores++] = k; }
    else c->cores[c->ncores++] = atoi(t);
  }
  free(d);
}

/* ---------------------------------------------------------------- synthetic tests */
static const double TWOPI = 6.283185307179586;

/* amplitude of exp(j 2 pi f n / fs) in x[skip..skip+len) */
static double tone_amp(const int16_t *x, int skip, int len, double f, double fs)
{
  double complex acc = 0;
  for (int k = 0; k < len; ++k) acc += (x[2 * (skip + k)] + I * x[2 * (skip + k) + 1]) * cexp(-I * TWOPI * fmod(f * (skip + k) / fs, 1.0));
  return cabs(acc) / len;
}

/* tones on a 1 kHz grid so one 1 ms period is exact; a[ch] tones at centre-relative offsets, per cell */
static void synth(int16_t *x, int n0, int n, double fs, const double *f, const double *amp, int nt)
{
  const int per = (int)lrint(fs / 1000.0);
  for (int k = 0; k < n; ++k) {
    double re = 0, im = 0;
    for (int t = 0; t < nt; ++t) {
      const long q = (long)lrint(f[t] / 1000.0);
      const double ph = TWOPI * (double)(((q % per + per) % per) * (long)((n0 + k) % per) % per) / per;
      re += amp[t] * cos(ph); im += amp[t] * sin(ph);
    }
    x[2 * k] = (int16_t)lrint(re); x[2 * k + 1] = (int16_t)lrint(im);
  }
}

/* host emulation of the GPU data flow (shm_iq_chan_gpu.cu) built on the shared kernel header; state kept across blocks */
typedef struct { float *x[IQ_DDC_MAX_STAGES + 1][2]; int stride[IQ_DDC_MAX_STAGES + 1], H[IQ_DDC_MAX_STAGES + 1]; double phase; } emu_t;

static void emu_init(emu_t *e, const iq_ddc_t *d, int nmax)
{
  memset(e, 0, sizeof(*e));
  int n = nmax;
  for (int s = 0; s <= d->nstages; ++s, n /= 2) {
    e->H[s] = s < d->nstages ? d->st[s].ntaps - 1 : 0; e->stride[s] = e->H[s] + n;
    e->x[s][0] = calloc((size_t)e->stride[s], 4); e->x[s][1] = calloc((size_t)e->stride[s], 4);
  }
}

static void emu_free(emu_t *e, const iq_ddc_t *d) { for (int s = 0; s <= d->nstages; ++s) { free(e->x[s][0]); free(e->x[s][1]); } }

static void emu_run(emu_t *e, const iq_ddc_t *d, const int16_t *in, int n, int16_t *out)
{
  const double step = -2 * M_PI * d->shift_hz / d->rate_in;
  float *ph = malloc((size_t)(n / IQ_DDC_ROT_BLOCK) * 8);
  for (int b = 0; b < n / IQ_DDC_ROT_BLOCK; ++b) {
    ph[2 * b] = (float)cos(e->phase); ph[2 * b + 1] = (float)sin(e->phase);
    e->phase = remainder(e->phase + step * IQ_DDC_ROT_BLOCK, 2 * M_PI);
  }
  for (int i = 0; i < n; ++i)
    ck_mix(in + 2 * i, d->rot + 2 * (i % IQ_DDC_ROT_BLOCK), ph[2 * (i / IQ_DDC_ROT_BLOCK)], ph[2 * (i / IQ_DDC_ROT_BLOCK) + 1],
           &e->x[0][0][e->H[0] + i], &e->x[0][1][e->H[0] + i]);
  int ns = n, s = 0;
  for (; s < d->nstages; ++s, ns /= 2) {
    const int c = (d->st[s].ntaps - 1) / 2;
    for (int m = 0; m < ns / 2; ++m)
      ck_halfband(e->x[s][0], e->x[s][1], m, c, d->st[s].npairs, d->st[s].pair, &e->x[s + 1][0][e->H[s + 1] + m], &e->x[s + 1][1][e->H[s + 1] + m]);
    for (int i = 0; i < e->H[s]; ++i) { e->x[s][0][i] = e->x[s][0][ns + i]; e->x[s][1][i] = e->x[s][1][ns + i]; }
  }
  for (int m = 0; m < ns; ++m) { out[2 * m] = ck_out(e->x[s][0][m]); out[2 * m + 1] = ck_out(e->x[s][1][m]); }
  free(ph);
}

static double db(double a) { return 20 * log10(a > 1e-9 ? a : 1e-9); }

/* returns 0 on pass */
static int selftest(chan_t *c, int gpu)
{
  /* two cells around a baseband ring centre of 0 Hz: A decim 4 (61.44 MS/s, 40 MHz), B decim 8 (30.72 MS/s, 20 MHz) */
  const double cen[2] = {-45e6, 70e6}; /* spaced so that each cell lies in the other's first-stage stopband */
  const double off[2][2] = {{-2e6, -3.5e6}, {2e6, -1e6}}; /* [cell][channel] tone offsets from the cell centre */
  const double AMP = 3000;
  c->nch = 2; c->in_rate = 245.76e6; c->block = 65536; c->atten_db = 70; c->ncell = 2; c->gpu = 0;
  snprintf(c->cell[0].tag, 32, "A"); c->cell[0].centre = cen[0]; c->cell[0].decim = 4; c->cell[0].bw = 40e6;
  snprintf(c->cell[1].tag, 32, "B"); c->cell[1].centre = cen[1]; c->cell[1].decim = 8; c->cell[1].bw = 20e6;
  if (chan_setup(c)) return 1;
  set_shifts(c, 0);
  const int NB = 8, N = c->block;
  int16_t *in[2], *res[2][2], *gres[2][2];
  for (int a = 0; a < 2; ++a) in[a] = malloc((size_t)N * NB * 4);
  for (int a = 0; a < 2; ++a)
    for (int k = 0; k < 2; ++k) { res[k][a] = malloc((size_t)N * NB / c->cell[k].decim * 4); gres[k][a] = malloc((size_t)N * NB / c->cell[k].decim * 4); }
  /* antenna a carries one tone of cell A and one of cell B, at that cell's own offset for this antenna */
  for (int a = 0; a < 2; ++a) {
    const double f[2] = {cen[0] + off[0][a], cen[1] + off[1][a]}, am[2] = {AMP, AMP};
    synth(in[a], 0, N * NB, c->in_rate, f, am, 2);
  }
  /* CPU backend */
  for (int b = 0; b < NB; ++b) {
    const int16_t *blk[2] = {in[0] + 2 * (size_t)b * N, in[1] + 2 * (size_t)b * N};
    if (chan_block(c, blk, N, NULL)) return 1;
    for (int k = 0; k < 2; ++k)
      for (int a = 0; a < 2; ++a) memcpy(res[k][a] + 2 * (size_t)b * N / c->cell[k].decim, c->cell[k].obuf[a], (size_t)N / c->cell[k].decim * 4);
  }
  int fail = 0;
  for (int k = 0; k < 2; ++k) {
    const cell_t *ce = &c->cell[k];
    const int nout = N * NB / ce->decim, skip = 2048, len = nout - skip;
    for (int a = 0; a < 2; ++a) {
      const double own = db(tone_amp(res[k][a], skip, len, off[k][a], ce->rate) / AMP);
      const double wrongant = db(tone_amp(res[k][a], skip, len, off[k][1 - a], ce->rate) / AMP);
      /* the other cell's tone of this antenna, folded into this cell's output band */
      double fo = fmod(cen[1 - k] + off[1 - k][a] - cen[k], ce->rate);
      if (fo > ce->rate / 2) fo -= ce->rate; else if (fo < -ce->rate / 2) fo += ce->rate;
      const double alias = db(tone_amp(res[k][a], skip, len, fo, ce->rate) / AMP);
      const int ok = fabs(own) < 0.05 && wrongant < -60 && alias < -60;
      printf("SELFTEST cpu cell=%s ch=%d own_gain_db=%.4f other_antenna_tone_db=%.1f other_cell_tone_db=%.1f (folded %.3f MHz) %s\n", ce->tag, a, own, wrongant,
             alias, fo / 1e6, ok ? "PASS" : "FAIL");
      fail |= !ok;
    }
  }
  /* host emulation of the GPU kernels vs iq_ddc: expected identical to <= 1 LSB (no FMA on generic x86; ARM contracts) */
  {
    long maxd = 0, nd = 0, tot = 0;
    for (int k = 0; k < 2; ++k)
      for (int a = 0; a < 2; ++a) {
        emu_t e;
        emu_init(&e, &c->cell[k].ddc[0], N);
        iq_ddc_t ref_d;
        iq_ddc_init(&ref_d, c->in_rate, c->cell[k].decim, 0.5 * c->cell[k].bw, c->atten_db);
        iq_ddc_set_shift(&ref_d, c->cell[k].centre);
        int16_t *o = malloc((size_t)N * 4), *o2 = malloc((size_t)N * 4);
        for (int b = 0; b < NB; ++b) {
          emu_run(&e, &ref_d, in[a] + 2 * (size_t)b * N, N, o);
          iq_ddc_process(&ref_d, in[a] + 2 * (size_t)b * N, N, o2);
          for (int i = 0; i < 2 * N / c->cell[k].decim; ++i) { long d = labs((long)o[i] - o2[i]); if (d > maxd) maxd = d; nd += d != 0; tot++; }
        }
        free(o); free(o2); emu_free(&e, &ref_d); iq_ddc_free(&ref_d);
      }
    const int ok = maxd <= 1;
    printf("SELFTEST emu(gpu kernel bodies, host) vs iq_ddc: max_abs_diff=%ld LSB differing=%ld/%ld %s\n", maxd, nd, tot, ok ? "PASS" : "FAIL");
    fail |= !ok;
  }
  if (gpu) {
    chan_t g;
    memcpy(&g, c, sizeof(g));
    g.gpu = 1; g.threads = 0; g.gpu_h = NULL; g.dl = NULL;
    /* independent DDC designs for the GPU instance: chan_setup allocates its own state */
    for (int k = 0; k < 2; ++k) for (int a = 0; a < 2; ++a) { memset(&g.cell[k].ddc[a], 0, sizeof(iq_ddc_t)); g.cell[k].obuf[a] = NULL; }
    if (chan_setup(&g)) { printf("SELFTEST gpu setup FAIL\n"); return 1; }
    set_shifts(&g, 0);
    double ms_max = 0, ms_sum = 0;
    for (int b = 0; b < NB; ++b) {
      const int16_t *blk[2] = {in[0] + 2 * (size_t)b * N, in[1] + 2 * (size_t)b * N};
      double ms;
      if (chan_block(&g, blk, N, &ms)) { printf("SELFTEST gpu run FAIL\n"); return 1; }
      ms_sum += ms; if (ms > ms_max) ms_max = ms;
      for (int k = 0; k < 2; ++k)
        for (int a = 0; a < 2; ++a) memcpy(gres[k][a] + 2 * (size_t)b * N / g.cell[k].decim, g.cell[k].obuf[a], (size_t)N / g.cell[k].decim * 4);
    }
    long maxd = 0, nd = 0, tot = 0;
    for (int k = 0; k < 2; ++k)
      for (int a = 0; a < 2; ++a)
        for (int i = 0; i < 2 * N * NB / c->cell[k].decim; ++i) { long d = labs((long)gres[k][a][i] - res[k][a][i]); if (d > maxd) maxd = d; nd += d != 0; tot++; }
    const int ok = maxd <= 2 && nd * 1000 <= tot;
    printf("SELFTEST gpu vs cpu: max_abs_diff=%ld LSB differing=%ld/%ld gpu_ms/block avg=%.3f max=%.3f (block %d samples = %.3f ms of signal) %s\n", maxd, nd, tot,
           ms_sum / NB, ms_max, N, 1e3 * N / c->in_rate, ok ? "PASS" : "FAIL");
    fail |= !ok;
    chan_free(&g);
  }
  chan_free(c);
  printf("SELFTEST %s\n", fail ? "FAIL" : "PASS");
  return fail;
}

static int bench(chan_t *c, double secs)
{
  if (!c->ncell) { fprintf(stderr, "--bench needs --cell\n"); return 1; }
  c->nch = c->nch ? c->nch : 4;
  if (chan_setup(c)) return 1;
  set_shifts(c, 0);
  const int N = c->block;
  int16_t *in[SHM_IQ_MAX_CH];
  for (int a = 0; a < c->nch; ++a) {
    in[a] = malloc((size_t)N * 4);
    for (int i = 0; i < 2 * N; ++i) in[a][i] = (int16_t)((rand() & 0x3ff) - 512);
  }
  const int16_t *blk[SHM_IQ_MAX_CH];
  for (int a = 0; a < c->nch; ++a) blk[a] = in[a];
  if (chan_block(c, blk, N, NULL)) return 1;
  const double t0 = now_s();
  struct timespec c0;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
  long blocks = 0;
  double gms = 0;
  while (now_s() - t0 < secs) { double m = 0; if (chan_block(c, blk, N, &m)) return 1; gms += m; blocks++; }
  const double el = now_s() - t0;
  struct timespec c1;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
  const double cpu = (c1.tv_sec - c0.tv_sec) + 1e-9 * (c1.tv_nsec - c0.tv_nsec), rt = (double)blocks * N / c->in_rate / el;
  printf("BENCH backend=%s cells=%d ch=%d in_rate=%.2fMS/s threads=%d block=%d: %.3f x realtime, process cpu %.2f core-s per s of input%s", c->gpu ? "gpu" : "cpu",
         c->ncell, c->nch, c->in_rate / 1e6, c->threads, N, rt, cpu / el / rt, "");
  if (c->gpu) printf(", gpu %.3f ms/block", gms / (double)blocks);
  printf("\n");
  chan_free(c);
  return 0;
}

/* ---------------------------------------------------------------- live channelizer */
static int run_live(chan_t *c, const char *ring, double attach_s, double timeout_ms, double out_ms, double stat_s)
{
  shm_iq_ring_t in;
  const uint64_t t0 = shm_iq_now_ns();
  while (shm_iq_ring_attach(&in, ring) != 0 || atomic_load(&in.hdr->head) == 0) {
    if (in.hdr) shm_iq_ring_detach(&in);
    if (g_stop || shm_iq_now_ns() - t0 > (uint64_t)(attach_s * 1e9)) { fprintf(stderr, "SHMIQ_CHAN_ERR no ring /%s\n", ring); return 1; }
    usleep(10000);
  }
  shm_iq_hdr_t *h = in.hdr;
  c->nch = (int)h->channels; c->in_rate = h->rate;
  if ((uint64_t)c->block > h->capacity / 2) { fprintf(stderr, "SHMIQ_CHAN_ERR block %d exceeds half the ring (%" PRIu64 ")\n", c->block, h->capacity); return 1; }
  if (chan_setup(c)) return 1;
  int dmax = 1;
  for (int k = 0; k < c->ncell; ++k) {
    cell_t *ce = &c->cell[k];
    if (ce->decim > dmax) dmax = ce->decim;
    char name[64];
    for (const char *p = ce->tag; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) { fprintf(stderr, "SHMIQ_CHAN_ERR bad tag %s\n", ce->tag); return 1; }
    snprintf(name, sizeof(name), "%s_%s", ring, ce->tag);
    uint64_t cap = (uint64_t)(ce->rate * out_ms * 1e-3), min = 8 * (uint64_t)(c->block / ce->decim);
    if (cap < min) cap = min;
    if (shm_iq_ring_create(&ce->out, name, h->channels, cap, ce->rate, ce->centre)) { perror(name); return 1; }
    if (h->version >= 2) shm_iq_ring_set_meta(&ce->out, h->radio_id, h->time_source);
    fprintf(stderr, "SHMIQ_CHAN_CELL cell=%s ring=/%s rate=%.0f decim=%d centre_hz=%.3f capacity=%" PRIu64 " taps=%d\n", ce->tag, name, ce->rate, ce->decim, ce->centre, ce->out.hdr->capacity,
            c->cell[k].ddc[0].nstages ? c->cell[k].ddc[0].st[0].ntaps : 0);
  }
  uint64_t epoch = atomic_load(&h->rf_epoch), pos;
  set_shifts(c, shm_iq_ring_center(&in));
  { uint64_t hd = atomic_load(&h->head); pos = hd - hd % (uint64_t)dmax; }
  const uint64_t timeout_ns = (uint64_t)(timeout_ms * 1e6);
  int16_t *raw[SHM_IQ_MAX_CH];
  for (int a = 0; a < c->nch; ++a) raw[a] = malloc((size_t)c->block * 4);
  fprintf(stderr, "SHMIQ_CHAN_READY ring=/%s channels=%d rate=%.0f cells=%d backend=%s threads=%d block=%d producer_pid=%d\n", ring, c->nch, c->in_rate, c->ncell,
          c->gpu ? "gpu" : "cpu", c->threads, c->block, (int)atomic_load(&h->producer_pid));
  uint64_t blocks = 0, overruns = 0, discont = 0, dropped = 0;
  double tproc_sum = 0, tproc_max = 0, lag_max = 0, gms_sum = 0, next_stat = now_s() + stat_s;
  int rc = 0;
  while (!g_stop) {
    const uint64_t ep = atomic_load(&h->rf_epoch);
    if (ep != epoch) { /* producer retune: new shifts from the new centre, every output ring starts a new run and bumps its epoch */
      epoch = ep;
      set_shifts(c, shm_iq_ring_center(&in));
      for (int k = 0; k < c->ncell; ++k) shm_iq_ring_new_epoch(&c->cell[k].out, c->cell[k].centre);
      reset_all(c); /* the new run must not start with old-tuned filter history */
    }
    const int r = shm_iq_ring_copy(&in, pos, c->block, (unsigned)c->nch, raw);
    /* a retune between the epoch read above and the copy: redo this block with the new shifts (old-tuned samples below the
     * new valid_from then fail the copy and are skipped as a DISCONT, so no old-tuned block leaks into the new run) */
    if (r == 0 && atomic_load(&h->rf_epoch) != epoch) continue;
    if (r == 0) {
      const double ts = now_s();
      const uint64_t head = atomic_load(&h->head);
      double ms = 0;
      if (chan_block(c, (const int16_t *const *)raw, c->block, &ms)) { rc = 2; break; }
      for (int k = 0; k < c->ncell; ++k) {
        cell_t *ce = &c->cell[k];
        void *bufs[SHM_IQ_MAX_CH];
        for (int a = 0; a < c->nch; ++a) bufs[a] = ce->obuf[a];
        shm_iq_ring_write(&ce->out, (int64_t)(((uint64_t)h->base_tick + pos) / (uint64_t)ce->decim), bufs, c->block / ce->decim, (unsigned)c->nch);
        ce->away_until = 0;
      }
      const double el = now_s() - ts, lag = (double)(head - pos) / c->in_rate * 1e3; /* backlog when the block started, incl. the block itself */
      tproc_sum += el; if (el > tproc_max) tproc_max = el;
      if (lag > lag_max) lag_max = lag;
      gms_sum += ms;
      pos += (uint64_t)c->block; blocks++;
      if (stat_s > 0 && now_s() > next_stat) {
        fprintf(stderr, "SHMIQ_CHAN_STAT blocks=%" PRIu64 " proc_ms avg=%.3f max=%.3f gpu_ms_avg=%.3f backlog_ms_max=%.3f block_ms=%.3f overruns=%" PRIu64 " discont=%" PRIu64 "\n",
                blocks, 1e3 * tproc_sum / (double)blocks, 1e3 * tproc_max, gms_sum / (double)blocks, lag_max, 1e3 * c->block / c->in_rate, overruns, discont);
        tproc_max = 0; lag_max = 0; next_stat = now_s() + stat_s;
      }
      continue;
    }
    if (r < 0) {
      const uint64_t vf = atomic_load(&h->valid_from), head = atomic_load(&h->head);
      const int ov = pos >= vf;
      uint64_t to = ov ? head : vf;
      to += (uint64_t)(dmax - (int)(to % (uint64_t)dmax)) % (uint64_t)dmax;
      dropped += to - pos;
      if (ov) overruns++; else discont++;
      fprintf(stderr, "SHMIQ_CHAN_%s skipped_in=%" PRIu64 " new_pos=%" PRIu64 "\n", ov ? "OVERRUN" : "DISCONT", to - pos, to);
      pos = to;
      reset_all(c);
      continue;
    }
    /* not yet written: producer-lost rule of shm_iq.c */
    const uint64_t hb = atomic_load(&h->heartbeat_ns), now = shm_iq_now_ns(), stale = now > hb ? now - hb : 0;
    const int away = h->version >= 2 && atomic_load(&h->away);
    if (away) { /* time-shared window elsewhere: tell the cell consumers to extend their timeout too (and follow a moved return time) */
      const uint64_t until = atomic_load(&h->dwell_until_ns);
      for (int k = 0; k < c->ncell; ++k)
        if (c->cell[k].away_until != until) { shm_iq_ring_set_away(&c->cell[k].out, until); c->cell[k].away_until = until; }
    }
    const int away_wait = away && now < atomic_load(&h->dwell_until_ns) + timeout_ns;
    if (!away_wait && stale > timeout_ns && ((kill(atomic_load(&h->producer_pid), 0) != 0 && errno == ESRCH) || stale > 60000000000ull)) {
      fprintf(stderr, "SHMIQ_CHAN_PRODUCER_LOST pid=%d pos=%" PRIu64 "\n", (int)atomic_load(&h->producer_pid), pos);
      break;
    }
    struct timespec w = {0, 20000}; /* shortcut: polling like shm_iq.c; a futex wake needs a ring change */
    nanosleep(&w, NULL);
  }
  fprintf(stderr, "SHMIQ_CHAN_END blocks=%" PRIu64 " overruns=%" PRIu64 " discont=%" PRIu64 " dropped_in=%" PRIu64 "\n", blocks, overruns, discont, dropped);
  /* the output rings stay mapped by their consumers; unlinking only stops new attaches. Their heartbeat stops with this process. */
  for (int k = 0; k < c->ncell; ++k) { shm_unlink(c->cell[k].out.name); shm_iq_ring_detach(&c->cell[k].out); }
  chan_free(c);
  shm_iq_ring_detach(&in);
  for (int a = 0; a < c->nch; ++a) free(raw[a]);
  return rc;
}

int main(int argc, char **argv)
{
  static chan_t c;
  char ring[64] = "";
  double attach_s = 30, timeout_ms = 2000, out_ms = 100, stat_s = 5, bench_s = 0;
  int do_selftest = 0;
  c.block = 65536; c.atten_db = 70; c.in_rate = 245.76e6;
  if (getenv("ISAC_SHM_IQ_NAME")) snprintf(ring, sizeof(ring), "%s", getenv("ISAC_SHM_IQ_NAME"));
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
    if (!strcmp(a, "--ring")) { snprintf(ring, sizeof(ring), "%s", v); ++i; }
    else if (!strcmp(a, "--cell")) { if (parse_cell(&c, v)) { fprintf(stderr, "bad --cell %s (TAG:CENTRE_HZ:RATE[:BW_HZ])\n", v); return 1; } ++i; }
    else if (!strcmp(a, "--cells-json")) { if (load_plan(&c, v, ring, sizeof(ring))) return 1; ++i; }
    else if (!strcmp(a, "--threads")) { c.threads = atoi(v); ++i; }
    else if (!strcmp(a, "--cores")) { parse_cores(&c, v); ++i; }
    else if (!strcmp(a, "--block")) { c.block = atoi(v); ++i; }
    else if (!strcmp(a, "--timeout-ms")) { timeout_ms = atof(v); ++i; }
    else if (!strcmp(a, "--out-ms")) { out_ms = atof(v); ++i; }
    else if (!strcmp(a, "--attach-s")) { attach_s = atof(v); ++i; }
    else if (!strcmp(a, "--stat-s")) { stat_s = atof(v); ++i; }
    else if (!strcmp(a, "--atten-db")) { c.atten_db = atof(v); ++i; }
    else if (!strcmp(a, "--in-rate")) { c.in_rate = atof(v); ++i; }
    else if (!strcmp(a, "--nch")) { c.nch = atoi(v); ++i; }
    else if (!strcmp(a, "--bench")) { bench_s = atof(v); ++i; }
    else if (!strcmp(a, "--gpu")) c.gpu = 1;
    else if (!strcmp(a, "--selftest")) do_selftest = 1;
    else { fprintf(stderr, "unknown option %s\n", a); return 1; }
  }
  if (c.block < 16384 || c.block % IQ_DDC_ROT_BLOCK || c.block % 16) { fprintf(stderr, "--block must be a multiple of %d and >= 16384\n", IQ_DDC_ROT_BLOCK); return 1; }
  if (do_selftest) { int g = c.gpu; return selftest(&c, g); }
  if (bench_s > 0) return bench(&c, bench_s);
  if (!c.ncell) { fprintf(stderr, "no cells: --cell TAG:CENTRE_HZ:RATE[:BW_HZ] or --cells-json plan.json\n"); return 1; }
  if (!ring[0]) snprintf(ring, sizeof(ring), "isac_iq");
  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
  return run_live(&c, ring, attach_s, timeout_ms, out_ms, stat_s);
}
