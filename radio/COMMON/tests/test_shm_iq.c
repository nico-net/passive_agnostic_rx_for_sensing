/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1 tests: DDC response, ring semantics, and an end-to-end fan-out: one producer process publishes a two-cell
 * wideband signal, two consumer processes load the real libshm_iq.so device with different -C / sample rates and each
 * must see only its own cell. Usage: test_shm_iq <path to libshm_iq.so> */
#define _GNU_SOURCE
#include "../common_lib.h"
#include "../iq_ddc.h"
#include "../shm_iq_ring.h"
#include <complex.h>
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
  shm_iq_ring_write(&p, 50, src, 1000, 2); /* after a retune the radio clock may restart */
  CHECK(atomic_load(&p.hdr->head) == 8000 && atomic_load(&p.hdr->valid_from) == 7000 && shm_iq_ring_center(&c) == 3.6e9, "epoch");
  CHECK(shm_iq_ring_copy(&c, 7000, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000), "post-epoch copy");
  shm_iq_ring_detach(&c); shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("ring ok\n");
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

int main(int argc, char **argv)
{
  int fail = 0;
  fail |= test_ddc(1, 17.3e6, 20e6);
  fail |= test_ddc(2, -11e6, 20e6);   /* 106 PRB class: 40 MHz channel at 61.44 MS/s */
  fail |= test_ddc(2, 5e6, 30e6);     /* 162 PRB class: 60 MHz channel at 61.44 MS/s */
  fail |= test_ddc(4, 31e6, 10e6);    /* 51 PRB class: 20 MHz channel at 30.72 MS/s */
  fail |= test_ring();
  if (argc > 1) fail |= test_fanout(argv[1]);
  printf(fail ? "FAILED\n" : "ALL OK\n");
  return fail;
}
