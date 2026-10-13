/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1 tests: DDC response, ring semantics, and an end-to-end fan-out: one producer process publishes a two-cell
 * wideband signal, two consumer processes load the real libshm_iq.so device with different -C / sample rates and each
 * must see only its own cell. Usage: test_shm_iq <path to libshm_iq.so> */
#define _GNU_SOURCE
#include "../common_lib.h"
#include "../iq_ddc.h"
#include "../shm_iq_ring.h"
#include <complex.h>
#include <pthread.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)

/* the device's AssertFatal ends here */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s %s %d\n", file, line, function, s, assert);
  _exit(3);
}

static const double FS = 122.88e6;

/* amplitude of exp(j 2 pi f n / fs) in x[skip..n) */
static double tone_amp(const int16_t *x, int n, int skip, double f, double fs)
{
  double complex acc = 0;
  for (int k = skip; k < n; ++k) acc += (x[2 * k] + I * x[2 * k + 1]) * cexp(-I * 2 * M_PI * f * k / fs);
  return cabs(acc) / (n - skip);
}

static void gen(int16_t *x, int n, uint64_t n0, const double *f, const double *a, int ntones)
{
  for (int k = 0; k < n; ++k) {
    double re = 0, im = 0;
    for (int t = 0; t < ntones; ++t) {
      double ph = fmod(2 * M_PI * f[t] / FS * (double)(n0 + (uint64_t)k), 2 * M_PI);
      re += a[t] * cos(ph); im += a[t] * sin(ph);
    }
    x[2 * k] = (int16_t)lrint(re); x[2 * k + 1] = (int16_t)lrint(im);
  }
}

/* DDC: shift + decimate keeps an in-band tone at unit gain and rejects the tone that would alias onto the band */
static int test_ddc(int decim, double shift, double passband)
{
  const int chunk = 6144, N = chunk * 11;
  const double fs_out = FS / decim, f_in = 0.6 * passband, f_alias = (decim > 1 ? fs_out : 0) - 0.5 * passband;
  const double f[2] = {shift + f_in, shift + f_alias + (decim > 1 ? 0 : 1e30)};
  const double a[2] = {8000, 8000};
  int16_t *x = malloc((size_t)N * 4), *y = malloc((size_t)N * 4);
  gen(x, N, 0, f, a, decim > 1 ? 2 : 1);
  iq_ddc_t d;
  CHECK(iq_ddc_init(&d, FS, decim, passband, 70) == 0, "init");
  iq_ddc_set_shift(&d, shift);
  int out = 0;
  for (int k = 0; k < N; k += chunk) out += iq_ddc_process(&d, x + 2 * k, chunk, y + 2 * out);
  CHECK(out == N / decim, "out %d", out);
  const double g = tone_amp(y, out, 600, f_in, fs_out) / 8000;
  CHECK(fabs(20 * log10(g)) < 0.1, "decim %d in-band gain %.4f dB", decim, 20 * log10(g));
  if (decim > 1) {
    /* f_alias - fs_out == -0.5 passband: lands inside the band if not filtered */
    const double leak = tone_amp(y, out, 600, f_alias - fs_out, fs_out) / 8000;
    CHECK(20 * log10(leak + 1e-12) < -60, "decim %d alias leak %.1f dB", decim, 20 * log10(leak + 1e-12));
    printf("ddc decim=%d taps=%d,%d gain=%.4f dB alias=%.1f dB\n", decim, d.st[0].ntaps, d.st[1].ntaps, 20 * log10(g),
           20 * log10(leak + 1e-12));
  } else {
    printf("ddc decim=1 shift=%.0f gain=%.4f dB\n", shift, 20 * log10(g));
  }
  iq_ddc_free(&d); free(x); free(y);
  return 0;
}

static int test_ring(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_test_%d", (int)getpid());
  shm_iq_ring_t p, c;
  CHECK(shm_iq_ring_create(&p, name, 2, 3000, FS, 3.5e9) == 0, "create");
  CHECK(p.hdr->capacity == 4096, "capacity %llu", (unsigned long long)p.hdr->capacity);
  CHECK(shm_iq_ring_attach(&c, name) == 0, "attach");
  int16_t a[2][2000], b[2][2000];
  int16_t *dst[2] = {b[0], b[1]};
  for (int k = 0; k < 2000; ++k) a[0][k] = (int16_t)k, a[1][k] = (int16_t)-k;
  void *src[2] = {a[0], a[1]};
  CHECK(shm_iq_ring_copy(&c, 0, 10, 2, dst) == 1, "empty ring must say not yet");
  for (int w = 0; w < 5; ++w) shm_iq_ring_write(&p, 1000 + w * 1000, src, 1000, 2); /* ticks 1000..5999, index 0..4999 */
  CHECK(p.hdr->base_tick == 1000, "base_tick");
  CHECK(shm_iq_ring_copy(&c, 4000, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000) && !memcmp(b[1], a[1], 4000), "copy");
  CHECK(shm_iq_ring_copy(&c, 500, 100, 2, dst) == -1, "overwritten range must fail");
  CHECK(shm_iq_ring_copy(&c, 4900, 200, 2, dst) == 1, "future range must wait");
  shm_iq_ring_write(&p, 7000, src, 1000, 2); /* forward gap of 1000 ticks */
  CHECK(atomic_load(&p.hdr->head) == 7000 && atomic_load(&p.hdr->valid_from) == 6000 && atomic_load(&p.hdr->gaps) == 1, "gap");
  CHECK(shm_iq_ring_copy(&c, 5500, 100, 2, dst) == -1, "pre-gap range must fail");
  shm_iq_ring_new_epoch(&p, 3.6e9);
  shm_iq_ring_write(&p, 8000, src, 1000, 2); /* retune on a continuous clock: new run, no index jump */
  CHECK(atomic_load(&p.hdr->head) == 8000 && atomic_load(&p.hdr->valid_from) == 7000 && shm_iq_ring_center(&c) == 3.6e9, "epoch");
  CHECK(shm_iq_ring_copy(&c, 7000, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000), "post-epoch copy");
  CHECK(shm_iq_ring_copy(&c, 6500, 600, 2, dst) == -1, "pre-retune range must fail");
  shm_iq_ring_new_epoch(&p, 3.6e9);
  shm_iq_ring_write(&p, 50, src, 1000, 2); /* device reload: the radio clock restarts */
  const uint64_t vf = atomic_load(&p.hdr->valid_from);
  /* a consumer caught up at 8000 must see its next timestamp jump, otherwise the receiver misses the stream break */
  CHECK(vf > 8000 && atomic_load(&p.hdr->head) == vf + 1000 && atomic_load(&p.hdr->gaps) == 2, "reload index jump vf=%llu",
        (unsigned long long)vf);
  CHECK(shm_iq_ring_copy(&c, 8000, 10, 2, dst) != 0, "pre-reload position must not read new-run data");
  CHECK(shm_iq_ring_copy(&c, vf, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000), "post-reload copy");
  shm_iq_ring_detach(&c); shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("ring ok\n");
  return 0;
}

/* torn-read detection: a writer races a reader that hugs the overwrite edge; every copy reported valid must be exact */
static shm_iq_ring_t g_sp;
static atomic_int g_stop;
static void *stress_writer(void *arg)
{
  (void)arg;
  static int16_t x[2 * 333];
  void *b[1] = {x};
  for (uint64_t t = 0; !atomic_load(&g_stop); t += 333) {
    for (int k = 0; k < 333; ++k) { x[2 * k] = (int16_t)(t + k); x[2 * k + 1] = (int16_t)((t + k) >> 16); }
    shm_iq_ring_write(&g_sp, (int64_t)t, b, 333, 1);
  }
  return NULL;
}

static int test_ring_stress(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_stress_%d", (int)getpid());
  shm_iq_ring_t c;
  CHECK(shm_iq_ring_create(&g_sp, name, 1, 1024, FS, 0) == 0 && shm_iq_ring_attach(&c, name) == 0, "create");
  pthread_t th;
  pthread_create(&th, NULL, stress_writer, NULL);
  static int16_t y[2 * 512];
  int16_t *dst[1] = {y};
  long ok = 0, torn = 0, bad = 0;
  for (int it = 0; it < 2000000 && ok < 20000; ++it) {
    const uint64_t head = atomic_load(&c.hdr->head);
    if (head < 1024) continue;
    const uint64_t pos = head - 1024 + (uint64_t)(it % 200);
    if (shm_iq_ring_copy(&c, pos, 512, 1, dst) != 0) { torn++; continue; }
    ok++;
    for (int k = 0; k < 512; ++k)
      if (y[2 * k] != (int16_t)(pos + k) || y[2 * k + 1] != (int16_t)((pos + k) >> 16)) { bad++; break; }
  }
  atomic_store(&g_stop, 1);
  pthread_join(th, NULL);
  shm_iq_ring_detach(&c); shm_iq_ring_detach(&g_sp);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("ring stress ok=%ld rejected=%ld undetected_torn=%ld\n", ok, torn, bad);
  CHECK(bad == 0 && ok > 0, "torn copies returned as valid");
  return 0;
}

/* --- end to end: producer process + two consumer processes through libshm_iq.so --- */
static const double CENTER = 3600e6;
static const double TONE_A = CENTER + 30e6 + 1e6, TONE_B = CENTER - 25e6 - 2e6; /* cell A at +30 MHz, cell B at -25 MHz */

static int consumer(const char *lib, const char *ring, double rx, double rate, double bw, double own, double other)
{
  setenv("ISAC_SHM_IQ_NAME", ring, 1);
  setenv("ISAC_SHM_IQ_CELL", rate > 40e6 ? "A" : "B", 1);
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  int (*init)(openair0_device_t *, openair0_config_t *) = (int (*)(openair0_device_t *, openair0_config_t *))dlsym(h, "device_init");
  CHECK(init, "device_init");
  openair0_device_t dev; memset(&dev, 0, sizeof(dev));
  openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.sample_rate = rate; cfg.rx_bw = bw; cfg.rx_num_channels = 1; cfg.rx_freq[0] = rx;
  CHECK(init(&dev, &cfg) == 0, "init");
  CHECK(dev.trx_start_func(&dev) == 0, "start");
  const int n = (int)(rate / 2000), reads = 40; /* 0.5 ms per read */
  int16_t *buf = malloc((size_t)n * reads * 4);
  openair0_timestamp_t ts, ts0 = 0;
  for (int r = 0; r < reads; ++r) {
    void *b[1] = {buf + 2 * (size_t)n * r};
    CHECK(dev.trx_read_func(&dev, &ts, b, n, 1) == n, "read");
    if (r == 0) ts0 = ts;
    else CHECK(ts == ts0 + (openair0_timestamp_t)n * r, "timestamp continuity r=%d ts=%lld", r, (long long)(ts - ts0));
  }
  const int tot = n * reads;
  const double g = tone_amp(buf, tot, 2000, own - rx, rate) / 6000;
  double fo = fmod(other - rx, rate); /* where the other cell's tone would alias */
  if (fo > rate / 2) fo -= rate;
  if (fo < -rate / 2) fo += rate;
  const double leak = tone_amp(buf, tot, 2000, fo, rate) / 6000;
  printf("cell %s rate=%.2f MS/s own=%.3f dB other(alias %.2f MHz)=%.1f dB\n", rate > 40e6 ? "A" : "B", rate / 1e6,
         20 * log10(g), fo / 1e6, 20 * log10(leak + 1e-12));
  fflush(stdout);
  CHECK(fabs(20 * log10(g)) < 0.2, "own tone gain");
  CHECK(20 * log10(leak + 1e-12) < -60, "other cell leaks");
  dev.trx_end_func(&dev);
  return 0;
}

static int test_fanout(const char *lib)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_fan_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, ring, 1, 1 << 22, FS, CENTER) == 0, "create");
  fflush(stdout);
  pid_t ca = fork();
  if (!ca) _exit(consumer(lib, ring, CENTER + 30e6, 61.44e6, 40e6, TONE_A, TONE_B));
  pid_t cb = fork();
  if (!cb) _exit(consumer(lib, ring, CENTER - 25e6, 30.72e6, 20e6, TONE_B, TONE_A));
  /* producer: real-time pacing, 0.5 ms chunks, until both consumers are done */
  const int chunk = 61440;
  int16_t *x = malloc((size_t)chunk * 4);
  const double f[2] = {TONE_A - CENTER, TONE_B - CENTER}, a[2] = {6000, 6000};
  int done = 0, st, ok = 1;
  uint64_t n0 = 0;
  for (int it = 0; done < 2 && it < 40000; ++it, n0 += chunk) {
    gen(x, chunk, n0, f, a, 2);
    void *b[1] = {x};
    shm_iq_ring_write(&p, (int64_t)(5000 + n0), b, chunk, 1);
    struct timespec w = {0, 500000};
    nanosleep(&w, NULL);
    pid_t r;
    while ((r = waitpid(-1, &st, WNOHANG)) > 0) { done++; ok &= WIFEXITED(st) && WEXITSTATUS(st) == 0; }
  }
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", ring); shm_unlink(path);
  free(x);
  CHECK(done == 2 && ok, "consumers failed (done=%d)", done);
  printf("fanout ok\n");
  return 0;
}

/* run8 regression: a caught-up consumer that reads a heartbeat newer than its own clock read (producer wrote in between)
 * must keep waiting, not compute an unsigned-wrapped staleness and exit with SHMIQ_PRODUCER_LOST */
static int test_heartbeat_ahead(const char *lib)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_hb_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, ring, 1, 1 << 20, 61.44e6, CENTER) == 0, "create");
  static int16_t x[2 * 30720];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 0, b, 30720, 1);
  atomic_store(&p.hdr->heartbeat_ns, shm_iq_now_ns() + 10000000000ull); /* every consumer poll sees hb > now */
  fflush(stdout);
  pid_t c = fork();
  if (!c) {
    setenv("ISAC_SHM_IQ_NAME", ring, 1);
    void *h = dlopen(lib, RTLD_NOW);
    int (*init)(openair0_device_t *, openair0_config_t *) = h ? dlsym(h, "device_init") : NULL;
    openair0_device_t dev; memset(&dev, 0, sizeof(dev));
    openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 61.44e6; cfg.rx_bw = 40e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER;
    if (!init || init(&dev, &cfg) || dev.trx_start_func(&dev)) _exit(2);
    openair0_timestamp_t ts;
    void *d[1] = {x};
    _exit(dev.trx_read_func(&dev, &ts, d, 30720, 1) == 30720 ? 0 : 2);
  }
  struct timespec w = {0, 200000000};
  nanosleep(&w, NULL); /* the consumer polls an empty ring meanwhile */
  shm_iq_ring_write(&p, 30720, b, 30720, 1);
  int st;
  waitpid(c, &st, 0);
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", ring); shm_unlink(path);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "consumer exited %d with a live producer", WEXITSTATUS(st));
  printf("heartbeat ahead ok\n");
  return 0;
}

/* --- M4: header v2, channel-subset rings, dwell scheduler (fake device), away-aware consumer --- */
static const double FR = 30.72e6; /* fake radio rate */
typedef struct {
  double freq[4];
  int nch, nset;
  int64_t tick, first_after_set;
  double set_log[64];
  int64_t first_tick_log[64];
  int pending_first;
} fake_t;
static fake_t g_fk;
static const double BB[4] = {1e6, 2e6, 3e6, 4e6};          /* subset test: channel c carries BB[c] */
static const double RF1 = 3598e6 + 5e6, RF2 = 3680e6 + 7e6; /* dwell test: two cells at absolute RF */
static int g_world_dwell;

static int fk_read(openair0_device_t *d, openair0_timestamp_t *t, void **b, int n, int c)
{
  (void)d;
  struct timespec w = {0, 500000};
  nanosleep(&w, NULL);
  if (g_fk.pending_first) { g_fk.first_tick_log[g_fk.nset - 1] = g_fk.tick; g_fk.pending_first = 0; }
  for (int ch = 0; ch < c; ++ch) {
    int16_t *x = b[ch];
    for (int k = 0; k < n; ++k) {
      const double i = (double)(g_fk.tick + k);
      double re = 0, im = 0;
      if (!g_world_dwell) { re = 6000 * cos(2 * M_PI * BB[ch] / FR * i); im = 6000 * sin(2 * M_PI * BB[ch] / FR * i); }
      else {
        const double rf[2] = {RF1, RF2};
        for (int q = 0; q < 2; ++q) {
          const double f = rf[q] - g_fk.freq[ch];
          if (fabs(f) < 0.45 * FR) { re += 6000 * cos(2 * M_PI * f / FR * i); im += 6000 * sin(2 * M_PI * f / FR * i); }
        }
      }
      x[2 * k] = (int16_t)lrint(re); x[2 * k + 1] = (int16_t)lrint(im);
    }
  }
  *t = (openair0_timestamp_t)g_fk.tick;
  g_fk.tick += n;
  return n;
}

static int fk_set_freq(openair0_device_t *d, openair0_config_t *cfg)
{
  (void)d;
  for (int c = 0; c < g_fk.nch; ++c) g_fk.freq[c] = cfg->rx_freq[c];
  if (g_fk.nset < 64) { g_fk.set_log[g_fk.nset] = cfg->rx_freq[0]; g_fk.nset++; g_fk.pending_first = 1; }
  return 0;
}

static void fk_init(openair0_device_t *d, openair0_config_t *cfg, int nch, int dwell_world)
{
  memset(&g_fk, 0, sizeof(g_fk)); memset(d, 0, sizeof(*d)); memset(cfg, 0, sizeof(*cfg));
  g_fk.nch = nch; g_world_dwell = dwell_world;
  cfg->sample_rate = FR; cfg->rx_num_channels = nch; cfg->time_source = external;
  for (int c = 0; c < nch; ++c) cfg->rx_freq[c] = 3600e6;
  d->trx_read_func = fk_read; d->trx_set_freq_func = fk_set_freq; d->openair0_cfg = cfg;
}

static int test_v2_header(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_v2_%d", (int)getpid());
  shm_iq_ring_t p, c;
  CHECK(shm_iq_ring_create(&p, name, 1, 2048, FS, 3.5e9) == 0, "create");
  shm_iq_ring_set_meta(&p, "radioX", SHM_IQ_TS_PPS);
  CHECK(shm_iq_ring_attach(&c, name) == 0 && c.hdr->version == 2 && !strcmp(c.hdr->radio_id, "radioX") && c.hdr->time_source == SHM_IQ_TS_PPS
        && !atomic_load(&c.hdr->away), "v2 attach");
  shm_iq_ring_detach(&c);
  p.hdr->version = 1; /* a v1 producer: consumers keep accepting it */
  CHECK(shm_iq_ring_attach(&c, name) == 0, "v1 attach");
  shm_iq_ring_detach(&c);
  p.hdr->version = 3;
  CHECK(shm_iq_ring_attach(&c, name) != 0, "future version refused");
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("v2 header ok\n");
  return 0;
}

/* two rings from one 4-channel producer: ringA = channels 0-1, ringB = 2+3; each ring only has its subset's tones */
static int child_subsets(void)
{
  char a[48], b[48];
  snprintf(a, sizeof(a), "isac_iq_sa_%d", (int)getpid()); snprintf(b, sizeof(b), "isac_iq_sb_%d", (int)getpid());
  char spec[128]; snprintf(spec, sizeof(spec), "%s:0-1,%s:2+3", a, b);
  setenv("ISAC_SHM_IQ_PUB", spec, 1);
  setenv("ISAC_SHM_IQ_RADIO_ID", "fake-radio", 1);
  setenv("ISAC_SHM_IQ_CAPACITY", "65536", 1);
  openair0_device_t d; openair0_config_t cfg;
  fk_init(&d, &cfg, 4, 0);
  shm_iq_pub_maybe_wrap(&d, &cfg);
  static int16_t buf[4][2 * 3072];
  void *bp[4] = {buf[0], buf[1], buf[2], buf[3]};
  openair0_timestamp_t t;
  for (int r = 0; r < 8; ++r) CHECK(d.trx_read_func(&d, &t, bp, 3072, 4) == 3072, "read");
  shm_iq_ring_t ra, rb;
  CHECK(shm_iq_ring_attach(&ra, a) == 0 && shm_iq_ring_attach(&rb, b) == 0, "attach");
  CHECK(ra.hdr->channels == 2 && rb.hdr->channels == 2 && !strcmp(ra.hdr->radio_id, "fake-radio") && ra.hdr->time_source == SHM_IQ_TS_PPS, "geometry/meta");
  static int16_t y[2][2 * 4096];
  int16_t *dst[2] = {y[0], y[1]};
  const shm_iq_ring_t *rr[2] = {&ra, &rb};
  for (int q = 0; q < 2; ++q) {
    CHECK(shm_iq_ring_copy(rr[q], 4096, 4096, 2, dst) == 0, "copy");
    for (int k = 0; k < 2; ++k) {
      const int ch = 2 * q + k;
      for (int o = 0; o < 4; ++o) {
        const double amp = tone_amp(y[k], 4096, 0, BB[o], FR) / 6000;
        if (o == ch) CHECK(amp > 0.95 && amp < 1.05, "ring %d slot %d own tone %.3f", q, k, amp);
        else CHECK(amp < 0.02, "ring %d slot %d sees tone of ch%d (%.3f)", q, k, o, amp);
      }
    }
  }
  shm_unlink(ra.name); shm_unlink(rb.name);
  printf("channel subsets ok\n");
  return 0;
}

/* unset env => the device is untouched; plain name => one ring with all channels, samples identical to the device output */
static int child_plain(void)
{
  openair0_device_t d; openair0_config_t cfg;
  unsetenv("ISAC_SHM_IQ_PUB");
  fk_init(&d, &cfg, 2, 0);
  shm_iq_pub_maybe_wrap(&d, &cfg);
  CHECK(d.trx_read_func == fk_read && d.trx_set_freq_func == fk_set_freq, "unset env must not wrap");
  char a[48]; snprintf(a, sizeof(a), "isac_iq_pl_%d", (int)getpid());
  setenv("ISAC_SHM_IQ_PUB", a, 1); setenv("ISAC_SHM_IQ_CAPACITY", "65536", 1);
  unsetenv("ISAC_SHM_IQ_PUB_FREQ"); unsetenv("ISAC_SHM_IQ_DWELL");
  shm_iq_pub_maybe_wrap(&d, &cfg);
  static int16_t buf[2][2 * 3072];
  void *bp[2] = {buf[0], buf[1]};
  openair0_timestamp_t t;
  CHECK(d.trx_read_func(&d, &t, bp, 3072, 2) == 3072, "read");
  shm_iq_ring_t r;
  static int16_t y[2][2 * 3072];
  int16_t *dst[2] = {y[0], y[1]};
  CHECK(shm_iq_ring_attach(&r, a) == 0 && r.hdr->channels == 2 && shm_iq_ring_copy(&r, 0, 3072, 2, dst) == 0
        && !memcmp(y[0], buf[0], sizeof(y[0])) && !memcmp(y[1], buf[1], sizeof(y[1])), "plain ring content");
  CHECK(shm_iq_ring_center(&r) == 3600e6 && !atomic_load(&r.hdr->away), "plain ring centre");
  d.trx_set_freq_func(&d, &cfg);
  CHECK(atomic_load(&r.hdr->rf_epoch) == 1 && g_fk.nset == 1 && g_fk.freq[1] == 3600e6, "plain retune path");
  shm_unlink(r.name);
  printf("plain single ring ok\n");
  return 0;
}

/* dwell scheduler: one channel alternates between two window centres */
static int child_dwell(void)
{
  char a[48], b[48];
  snprintf(a, sizeof(a), "isac_iq_da_%d", (int)getpid()); snprintf(b, sizeof(b), "isac_iq_db_%d", (int)getpid());
  char spec[128], dw[160];
  snprintf(spec, sizeof(spec), "%s,%s", a, b);
  snprintf(dw, sizeof(dw), "%s@3598e6:40,%s@3680e6:30", a, b);
  setenv("ISAC_SHM_IQ_PUB", spec, 1); setenv("ISAC_SHM_IQ_DWELL", dw, 1); setenv("ISAC_SHM_IQ_SETTLE_US", "2000", 1);
  setenv("ISAC_SHM_IQ_CAPACITY", "262144", 1); unsetenv("ISAC_SHM_IQ_PUB_FREQ");
  openair0_device_t d; openair0_config_t cfg;
  fk_init(&d, &cfg, 1, 1);
  shm_iq_pub_maybe_wrap(&d, &cfg);
  CHECK(g_fk.nset == 1 && g_fk.set_log[0] == 3598e6, "initial LO");
  shm_iq_ring_t ra, rb;
  CHECK(shm_iq_ring_attach(&ra, a) == 0 && shm_iq_ring_attach(&rb, b) == 0, "attach");
  CHECK(atomic_load(&rb.hdr->away) == 1 && atomic_load(&ra.hdr->away) == 0, "initial away flags");
  static int16_t buf[2 * 3072];
  void *bp[1] = {buf};
  openair0_timestamp_t t;
  uint64_t head_b_at_away = 0, seen_away_a = 0, seen_away_b = 0;
  uint64_t bad_away = 0;
  for (int r = 0; r < 400; ++r) { /* ~0.5 ms each = 200 ms, ~3 returns each */
    CHECK(d.trx_read_func(&d, &t, bp, 3072, 1) == 3072, "read");
    const shm_iq_ring_t *rr[2] = {&ra, &rb};
    for (int q = 0; q < 2; ++q)
      if (atomic_load(&rr[q]->hdr->away)) {
        (q ? &seen_away_b : &seen_away_a)[0]++;
        if (atomic_load(&rr[q]->hdr->dwell_until_ns) <= shm_iq_now_ns() - 100000000ull) bad_away++;
      }
    (void)head_b_at_away;
  }
  CHECK(bad_away == 0, "away without a dwell_until in the future/recent");
  CHECK(seen_away_a > 0 && seen_away_b > 0, "both rings must have been away (a=%llu b=%llu)", (unsigned long long)seen_away_a,
        (unsigned long long)seen_away_b);
  CHECK(g_fk.nset >= 4, "LO retunes %d", g_fk.nset);
  for (int k = 0; k < g_fk.nset; ++k) CHECK(g_fk.set_log[k] == ((k & 1) ? 3680e6 : 3598e6), "LO sequence %d: %.0f", k, g_fk.set_log[k]);
  CHECK(atomic_load(&ra.hdr->rf_epoch) >= 1 && atomic_load(&rb.hdr->rf_epoch) >= 1, "epochs bumped a=%llu b=%llu",
        (unsigned long long)atomic_load(&ra.hdr->rf_epoch), (unsigned long long)atomic_load(&rb.hdr->rf_epoch));
  CHECK(atomic_load(&ra.hdr->gaps) >= 1 && atomic_load(&rb.hdr->gaps) >= 1, "forward timestamp jumps");
  /* settle: the current run of each ring starts >= settle ticks after the first read following the retune that preceded it */
  const double settle = 2e-3 * FR;
  const shm_iq_ring_t *rr[2] = {&ra, &rb};
  for (int q = 0; q < 2; ++q) {
    const shm_iq_hdr_t *h = rr[q]->hdr;
    if (atomic_load(&h->head) <= atomic_load(&h->valid_from) + 4096) continue; /* currently away or just returned: no run to check */
    const int64_t run_tick = atomic_load(&h->tick_offset) + (int64_t)atomic_load(&h->valid_from);
    int ok = 0;
    for (int k = 1; k < g_fk.nset; ++k)
      if (g_fk.set_log[k] == (q ? 3680e6 : 3598e6) && run_tick >= g_fk.first_tick_log[k] + (int64_t)settle && run_tick < g_fk.first_tick_log[k] + (int64_t)settle + 3072)
        ok = 1;
    CHECK(ok, "ring %d run start tick %lld does not honour settle", q, (long long)run_tick);
  }
  /* content: a ring holds only its own cell's tone (RF1 in ring A at +5 MHz of its centre, RF2 in B at +7 MHz) */
  static int16_t y[2 * 2048];
  int16_t *dst[1] = {y};
  for (int q = 0; q < 2; ++q) {
    const shm_iq_hdr_t *h = rr[q]->hdr;
    const uint64_t head = atomic_load(&h->head), vf = atomic_load(&h->valid_from);
    if (head < vf + 2048) continue;
    CHECK(shm_iq_ring_copy(rr[q], head - 2048, 2048, 1, dst) == 0, "copy");
    const double own = q ? 7e6 : 5e6, oth = q ? 5e6 : 7e6;
    const double ga = tone_amp(y, 2048, 0, own, FR) / 6000, go = tone_amp(y, 2048, 0, oth, FR) / 6000;
    CHECK(ga > 0.95 && ga < 1.05 && go < 0.02, "ring %d content own=%.3f other=%.3f", q, ga, go);
  }
  shm_unlink(ra.name); shm_unlink(rb.name);
  printf("dwell scheduler ok retunes=%d gapsA=%llu gapsB=%llu epochA=%llu epochB=%llu\n", g_fk.nset, (unsigned long long)atomic_load(&ra.hdr->gaps),
         (unsigned long long)atomic_load(&rb.hdr->gaps), (unsigned long long)atomic_load(&ra.hdr->rf_epoch), (unsigned long long)atomic_load(&rb.hdr->rf_epoch));
  return 0;
}

/* a consumer of an away ring (producer pid gone, heartbeat stale > timeout) waits and resumes after a forward jump;
 * the same situation without the away flag is PRODUCER_LOST (exit_function -> exit code 3) */
static int test_away_consumer(const char *lib, int away)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_aw_%d_%d", (int)getpid(), away);
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, ring, 1, 1 << 20, 61.44e6, CENTER) == 0, "create");
  static int16_t x[2 * 30720];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 0, b, 30720, 1);
  pid_t dead = fork();
  if (!dead) _exit(0);
  waitpid(dead, NULL, 0);
  atomic_store(&p.hdr->producer_pid, (int32_t)dead);
  fflush(stdout);
  pid_t c = fork();
  if (!c) {
    setenv("ISAC_SHM_IQ_NAME", ring, 1); setenv("ISAC_SHM_IQ_TIMEOUT_MS", "300", 1);
    void *h = dlopen(lib, RTLD_NOW);
    int (*init)(openair0_device_t *, openair0_config_t *) = h ? dlsym(h, "device_init") : NULL;
    openair0_device_t dev; memset(&dev, 0, sizeof(dev));
    openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 61.44e6; cfg.rx_bw = 40e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER;
    if (!init || init(&dev, &cfg) || dev.trx_start_func(&dev)) _exit(2);
    openair0_timestamp_t ts;
    void *d[1] = {x};
    if (dev.trx_read_func(&dev, &ts, d, 30720, 1) != 30720) _exit(2);
    _exit(ts >= 1000000 ? 0 : 4); /* forward timestamp jump */
  }
  struct timespec w = {0, 50000000};
  nanosleep(&w, NULL); /* consumer is polling with the head at its position */
  if (away) shm_iq_ring_set_away(&p, shm_iq_now_ns() + 1000000000ull);
  w.tv_nsec = 700000000; nanosleep(&w, NULL); /* > 300 ms timeout, < dwell_until + timeout */
  shm_iq_ring_return(&p, CENTER);
  shm_iq_ring_write(&p, 1000000, b, 30720, 1);
  shm_iq_ring_write(&p, 1030720, b, 30720, 1);
  int st;
  waitpid(c, &st, 0);
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", ring); shm_unlink(path);
  if (away) CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "away-ring consumer exit %d", WEXITSTATUS(st));
  else CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3, "control: no away flag must lose the producer (exit %d)", WEXITSTATUS(st));
  printf("away consumer away=%d ok\n", away);
  return 0;
}

static int in_child(int (*fn)(void))
{
  fflush(stdout);
  pid_t c = fork();
  if (!c) _exit(fn());
  int st;
  waitpid(c, &st, 0);
  return !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(int argc, char **argv)
{
  int fail = 0;
  fail |= test_ddc(1, 17.3e6, 20e6);
  fail |= test_ddc(2, -11e6, 20e6);   /* 106 PRB class: 40 MHz channel at 61.44 MS/s */
  fail |= test_ddc(2, 5e6, 30e6);     /* 162 PRB class: 60 MHz channel at 61.44 MS/s */
  fail |= test_ddc(4, 31e6, 10e6);    /* 51 PRB class: 20 MHz channel at 30.72 MS/s */
  fail |= test_ring();
  fail |= test_ring_stress();
  fail |= test_v2_header();
  fail |= in_child(child_plain);
  fail |= in_child(child_subsets);
  fail |= in_child(child_dwell);
  if (argc > 1) fail |= test_away_consumer(argv[1], 1);
  if (argc > 1) fail |= test_away_consumer(argv[1], 0);
  if (argc > 1) fail |= test_fanout(argv[1]);
  if (argc > 1) fail |= test_heartbeat_ahead(argv[1]);
  printf(fail ? "FAILED\n" : "ALL OK\n");
  return fail;
}
