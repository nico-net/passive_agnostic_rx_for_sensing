/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1 tests: DDC response, ring semantics, and an end-to-end fan-out: one producer process publishes a two-cell
 * wideband signal, two consumer processes load the real libshm_iq.so device with different -C / sample rates and each
 * must see only its own cell. Usage: test_shm_iq <path to libshm_iq.so> */
#define _GNU_SOURCE
#include "../common_lib.h"
#include "../iq_ddc.h"
#include "../shm_iq_ring.h"
#include "../../../openair1/PHY/TOOLS/nr_host_membudget.h"
#include <complex.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* the C1 create takes the backing and the consumer count; the ported cases use POSIX shm rings without consumer tracking, as before */
#define RING_CREATE(r, name, ch, cap, rate, ctr) shm_iq_ring_create(r, SHM_IQ_BACKING_POSIX_SHM, name, ch, cap, rate, ctr, 0)
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)

/* the device's AssertFatal ends here */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s %s %d\n", file, line, function, s, assert);
  _exit(3);
}

static const double FS = 122.88e6;
static double gen_fs = 122.88e6; /* sample rate gen() synthesises at */

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
      double ph = fmod(2 * M_PI * f[t] / gen_fs * (double)(n0 + (uint64_t)k), 2 * M_PI);
      re += a[t] * cos(ph); im += a[t] * sin(ph);
    }
    x[2 * k] = (int16_t)lrint(re); x[2 * k + 1] = (int16_t)lrint(im);
  }
}

/* DDC: shift + decimate keeps an in-band tone at unit gain and rejects the tone that would alias onto the band */
static int test_ddc(double fs, int decim, double shift, double passband)
{
  gen_fs = fs;
  const int chunk = 6144, N = chunk * 11;
  const double fs_out = fs / decim, f_in = 0.6 * passband, f_alias = (decim > 1 ? fs_out : 0) - 0.5 * passband;
  const double f[2] = {shift + f_in, shift + f_alias + (decim > 1 ? 0 : 1e30)};
  const double a[2] = {8000, 8000};
  int16_t *x = malloc((size_t)N * 4), *y = malloc((size_t)N * 4);
  gen(x, N, 0, f, a, decim > 1 ? 2 : 1);
  iq_ddc_t d;
  CHECK(iq_ddc_init(&d, fs, decim, passband, 70) == 0, "init");
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
    printf("ddc fs=%.2f decim=%d taps=%d,%d,%d,%d gain=%.4f dB alias=%.1f dB\n", fs / 1e6, decim, d.st[0].ntaps, d.st[1].ntaps,
           d.st[2].ntaps, d.st[3].ntaps, 20 * log10(g), 20 * log10(leak + 1e-12));
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
  CHECK(RING_CREATE(&p, name, 2, 3000, FS, 3.5e9) == 0, "create");
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
  CHECK(RING_CREATE(&g_sp, name, 1, 1024, FS, 0) == 0 && shm_iq_ring_attach(&c, name) == 0, "create");
  pthread_t th;
  pthread_create(&th, NULL, stress_writer, NULL);
  static int16_t y[2 * 512];
  int16_t *dst[1] = {y};
  long ok = 0, torn = 0, bad = 0;
  while (atomic_load(&c.hdr->head) < 1024) sched_yield(); /* on a loaded host the writer may start late */
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

static int consumer(const char *lib, const char *ring, double rx, double rate, double bw, double own, double other)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  int (*init)(openair0_device_t *, openair0_config_t *) = (int (*)(openair0_device_t *, openair0_config_t *))dlsym(h, "device_init");
  CHECK(init, "device_init");
  openair0_device_t dev; memset(&dev, 0, sizeof(dev));
  openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.sdr_addrs = (char *)ring; cfg.sample_rate = rate; cfg.rx_bw = bw; cfg.rx_num_channels = 1; cfg.rx_freq[0] = rx;
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

/* The NR UE leaves rx_bw at 0 and sets num_rb_dl: the filters must follow the grid (51 PRB x 12 x 30 kHz = 18.36 MHz), not 95 % of the rate. */
static int test_passband(const char *lib)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  double (*pb)(const openair0_config_t *) = (double (*)(const openair0_config_t *))dlsym(h, "shm_iq_cell_passband");
  CHECK(pb, "shm_iq_cell_passband");
  openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.sample_rate = 30.72e6; cfg.num_rb_dl = 51;
  CHECK(fabs(pb(&cfg) - 9.18e6) < 1, "51 PRB grid passband %.0f", pb(&cfg));
  cfg.rx_bw = 20e6;
  CHECK(fabs(pb(&cfg) - 10e6) < 1, "explicit rx_bw wins");
  cfg.rx_bw = 0; cfg.num_rb_dl = 0;
  CHECK(fabs(pb(&cfg) - 0.5 * 0.95 * 30.72e6) < 1, "no grid info: 95 %% of the rate");
  cfg.sample_rate = 122.88e6; cfg.num_rb_dl = 273; /* 98.28 MHz occupied, < 95 % of 122.88 */
  CHECK(fabs(pb(&cfg) - 49.14e6) < 1, "273 PRB grid");
  printf("passband: 51 PRB 9.18 MHz, explicit rx_bw, fallback, 273 PRB ok\n");
  return 0;
}

static int test_fanout_rate(const char *lib, double fs, double offA, double rA, double bwA, double offB, double rB, double bwB)
{
  const double toneA = CENTER + offA + 1e6, toneB = CENTER + offB - 2e6;
  gen_fs = fs;
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_fan_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(RING_CREATE(&p, ring, 1, 1 << 22, fs, CENTER) == 0, "create");
  fflush(stdout);
  pid_t ca = fork();
  if (!ca) _exit(consumer(lib, ring, CENTER + offA, rA, bwA, toneA, toneB));
  pid_t cb = fork();
  if (!cb) _exit(consumer(lib, ring, CENTER + offB, rB, bwB, toneB, toneA));
  /* producer: real-time pacing, 0.5 ms chunks, until both consumers are done */
  const int chunk = (int)(fs / 2000);
  int16_t *x = malloc((size_t)chunk * 4);
  const double f[2] = {toneA - CENTER, toneB - CENTER}, a[2] = {6000, 6000};
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
  printf("fanout ok (ring %.2f MS/s, D=%d and D=%d)\n", fs / 1e6, (int)lrint(fs / rA), (int)lrint(fs / rB));
  return 0;
}

/* MPCI-DASH P0 (F2): the cell= tag of the SHMIQ_* lines is the ring id (the manager's "nrcell_<id>" ring without its prefix), not "pid<getpid()>" */
static int test_ring_id(const char *lib)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  void (*rid)(const char *, char *, size_t) = (void (*)(const char *, char *, size_t))dlsym(h, "shm_iq_ring_id");
  CHECK(rid, "shm_iq_ring_id");
  char t[32];
  rid("nrcell_pci415", t, sizeof(t)); CHECK(!strcmp(t, "pci415"), "manager ring -> id (%s)", t);
  rid("/nrcell_pci923", t, sizeof(t)); CHECK(!strcmp(t, "pci923"), "leading slash (%s)", t);
  rid("isac_iq", t, sizeof(t)); CHECK(!strcmp(t, "isac_iq"), "other ring keeps its name (%s)", t);
  rid("nrcell_", t, sizeof(t)); CHECK(!strcmp(t, "nrcell_"), "prefix alone is not an id (%s)", t);
  rid("", t, sizeof(t)); CHECK(!strcmp(t, "ring"), "never empty (%s)", t);
  rid("nrcell_abcdefghijklmnopqrstuvwxyz0123456789", t, sizeof(t)); CHECK(strlen(t) == 31 && !strncmp(t, "abcdefghij", 10), "truncated safely (%s)", t);
  CHECK(!strstr(t, "pid"), "no pid tag");
  printf("ring id tag: nrcell_<id> -> <id>, other names kept, never pid<n>\n");
  return 0;
}

static int test_fanout(const char *lib)
{
  int f = test_passband(lib) | test_ring_id(lib) | test_fanout_rate(lib, FS, 30e6, 61.44e6, 40e6, -25e6, 30.72e6, 20e6);
  /* 245.76 MS/s ring: 80 MHz cell at 122.88 (D=2) and 20 MHz cell at 30.72 (D=8) at different shifts */
  return f | test_fanout_rate(lib, 245.76e6, 60e6, 122.88e6, 80e6, -60e6, 30.72e6, 20e6);
}

/* run8 regression: a caught-up consumer that reads a heartbeat newer than its own clock read (producer wrote in between)
 * must keep waiting, not compute an unsigned-wrapped staleness and exit with SHMIQ_PRODUCER_LOST */
static int test_heartbeat_ahead(const char *lib)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_hb_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(RING_CREATE(&p, ring, 1, 1 << 20, 61.44e6, CENTER) == 0, "create");
  static int16_t x[2 * 30720];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 0, b, 30720, 1);
  atomic_store(&p.hdr->heartbeat_ns, shm_iq_now_ns() + 10000000000ull); /* every consumer poll sees hb > now */
  fflush(stdout);
  pid_t c = fork();
  if (!c) {
    void *h = dlopen(lib, RTLD_NOW);
    int (*init)(openair0_device_t *, openair0_config_t *) = h ? dlsym(h, "device_init") : NULL;
    openair0_device_t dev; memset(&dev, 0, sizeof(dev));
    openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sdr_addrs = (char *)ring; cfg.sample_rate = 61.44e6; cfg.rx_bw = 40e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER;
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


/* the publisher's radio side channel reaches the cell's log: RFRING -> this cell's RFCHAN line, CHPOW / RFSTALL verbatim */
static int test_radio_relay(const char *lib)
{
  char ring[64], logf[96];
  snprintf(ring, sizeof(ring), "isac_iq_rr_%d", (int)getpid());
  snprintf(logf, sizeof(logf), "/tmp/shm_iq_relay_%d.log", (int)getpid());
  shm_iq_ring_t p;
  CHECK(RING_CREATE(&p, ring, 1, 1 << 20, 61.44e6, CENTER) == 0, "create");
  static int16_t x[2 * 30720];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 0, b, 30720, 1);
  shm_iq_ring_write(&p, 30720, b, 30720, 1);
  shm_iq_radio_reset(ring);
  shm_iq_radio_append(ring, "RFRING ch0 port=RX1 gain=38.00 freq_hz=3600000000 rate=61440000 bw_hz=61440000");
  shm_iq_radio_append(ring, "SENSING: CHPOW sample=61440 dBFS ch0=-43.10");
  shm_iq_radio_append(ring, "SENSING: RFSTALL SHM_PUB reason=overflow overflows=1 timeouts=0 resyncs=0");
  fflush(stdout);
  pid_t c;
  int st;
  /* a cell that reads from the start of the ring (no start(): cursor 0) so one read succeeds without a live producer */
  c = fork();
  if (!c) {
    if (!freopen(logf, "w", stderr)) _exit(5);
    setvbuf(stderr, NULL, _IONBF, 0);
    void *h = dlopen(lib, RTLD_NOW);
    int (*init)(openair0_device_t *, openair0_config_t *) = h ? dlsym(h, "device_init") : NULL;
    openair0_device_t dev; memset(&dev, 0, sizeof(dev));
    openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sdr_addrs = (char *)ring; cfg.sample_rate = 61.44e6; cfg.rx_bw = 40e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER + 7e6;
    if (!init || init(&dev, &cfg)) _exit(2);
    openair0_timestamp_t ts;
    void *d[1] = {x};
    /* no start(): the cursor is at 0, the two blocks are there */
    _exit(dev.trx_read_func(&dev, &ts, d, 30720, 1) == 30720 ? 0 : 3);
  }
  waitpid(c, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "relay consumer exited %d", WEXITSTATUS(st));
  char *log = calloc(1, 1 << 16);
  FILE *f = fopen(logf, "r");
  CHECK(f, "log");
  const size_t k = fread(log, 1, (1 << 16) - 1, f);
  fclose(f);
  (void)k;
  CHECK(strstr(log, "RFCHAN ch0 subdev=shm port=RX1 gain=38.00") && strstr(log, "freq=3607.000000 MHz rate=61.440000 Msps"), "RFCHAN of this cell: %s", log);
  CHECK(strstr(log, "SENSING: CHPOW sample=61440 dBFS ch0=-43.10") && strstr(log, "SENSING: RFSTALL SHM_PUB reason=overflow"), "CHPOW/RFSTALL relayed");
  free(log);
  unlink(logf);
  shm_iq_radio_remove(ring);
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", ring); shm_unlink(path);
  printf("radio relay ok\n");
  return 0;
}

/* --- header v2, away-aware consumer --- */
static const double FR = 30.72e6; /* fake radio rate */
static int test_v2_header(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_v2_%d", (int)getpid());
  shm_iq_ring_t p, c;
  CHECK(RING_CREATE(&p, name, 1, 2048, FS, 3.5e9) == 0, "create");
  shm_iq_ring_set_meta(&p, "radioX", SHM_IQ_TS_PPS);
  CHECK(shm_iq_ring_attach(&c, name) == 0 && c.hdr->version == SHM_IQ_VERSION && !strcmp(c.hdr->radio_id, "radioX") && c.hdr->time_source == SHM_IQ_TS_PPS
        && !atomic_load(&c.hdr->away), "v2 attach");
  shm_iq_ring_detach(&c);
  p.hdr->version = 1; /* a v1 producer: consumers keep accepting it */
  CHECK(shm_iq_ring_attach(&c, name) == 0, "v1 attach");
  shm_iq_ring_detach(&c);
  p.hdr->version = 4;
  CHECK(shm_iq_ring_attach(&c, name) != 0, "future version refused");
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("v2 header ok\n");
  return 0;
}

/* device reload while a window is away: its first write after the return has a backward tick; the index must jump by
 * the time it was away (the M1 reload rule), not by 1 sample because the return refreshed the heartbeat */
static int test_return_after_reload(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_rr_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(RING_CREATE(&p, name, 1, 1 << 16, FR, 3.6e9) == 0, "create");
  static int16_t x[2 * 4096];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 1000000, b, 4096, 1);
  shm_iq_ring_set_away(&p, shm_iq_now_ns() + 20000000ull);
  struct timespec w = {0, 20000000};
  nanosleep(&w, NULL);
  shm_iq_ring_return(&p, 3.6e9);
  shm_iq_ring_write(&p, 0, b, 4096, 1); /* new device clock */
  const uint64_t vf = atomic_load(&p.hdr->valid_from);
  CHECK(vf >= 4096 + (uint64_t)(0.02 * FR), "return after reload jumps %llu samples (< 20 ms)", (unsigned long long)(vf - 4096));
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("return after reload ok jump=%llu\n", (unsigned long long)(vf - 4096));
  return 0;
}

/* a consumer of an away ring (producer pid gone, heartbeat stale > timeout) waits and resumes after a forward jump;
 * the same situation without the away flag is PRODUCER_LOST (exit_function -> exit code 3) */
static int test_away_consumer(const char *lib, int away)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_aw_%d_%d", (int)getpid(), away);
  shm_iq_ring_t p;
  CHECK(RING_CREATE(&p, ring, 1, 1 << 20, 61.44e6, CENTER) == 0, "create");
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
    
    void *h = dlopen(lib, RTLD_NOW);
    int (*init)(openair0_device_t *, openair0_config_t *) = h ? dlsym(h, "device_init") : NULL;
    openair0_device_t dev; memset(&dev, 0, sizeof(dev));
    openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sdr_addrs = (char *)ring; cfg.sample_rate = 61.44e6; cfg.rx_bw = 40e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER;
    if (!init || init(&dev, &cfg) || dev.trx_start_func(&dev)) _exit(2);
    openair0_timestamp_t ts;
    void *d[1] = {x};
    if (dev.trx_read_func(&dev, &ts, d, 30720, 1) != 30720) _exit(2);
    _exit(ts >= 1000000 ? 0 : 4); /* forward timestamp jump */
  }
  struct timespec w = {0, 50000000};
  nanosleep(&w, NULL); /* consumer is polling with the head at its position */
  if (away) shm_iq_ring_set_away(&p, shm_iq_now_ns() + 1500000000ull);
  w.tv_sec = 2; w.tv_nsec = 600000000; nanosleep(&w, NULL); /* > 2 s device timeout, < dwell_until + timeout */
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


/* ================================================================ C1 additions: anonymous backing, N consumers, capacity plan ================================================ */
/* shm_open is counted with the linker (-Wl,--wrap=shm_open on this target): every shm_open of shm_iq_ring.c in this executable goes through here, whichever
 * function calls it, so "the anonymous path never calls shm_open" is a property of the binary, not of the test's own behaviour. */
extern int __real_shm_open(const char *, int, mode_t);
static atomic_int g_shm_open_calls;
int __wrap_shm_open(const char *name, int flags, mode_t mode)
{
  atomic_fetch_add(&g_shm_open_calls, 1);
  return __real_shm_open(name, flags, mode);
}

static int shm_dir_has(const char *prefix)
{
  FILE *p = popen("ls /dev/shm 2>/dev/null", "r");
  char l[256];
  int n = 0;
  if (!p) return 0;
  while (fgets(l, sizeof(l), p)) n += strncmp(l, prefix, strlen(prefix)) == 0;
  pclose(p);
  return n;
}

static int test_anon_basic(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_anon_%d", (int)getpid());
  const int before = atomic_load(&g_shm_open_calls);
  shm_iq_ring_t p, c, v, u;
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_ANON, name, 2, 3000, FS, 3.5e9, 3) == 0, "create anon");
  CHECK(p.backing == SHM_IQ_BACKING_ANON && p.hdr->capacity == 4096 && p.hdr->version == SHM_IQ_VERSION && p.hdr->n_consumers == 3, "anon geometry");
  CHECK(shm_iq_ring_create(&u, SHM_IQ_BACKING_ANON, name, 2, 3000, FS, 3.5e9, 3) == -1 && errno == EEXIST, "registry names are unique");
  CHECK(shm_iq_ring_attach(&c, name) == 0 && c.hdr == p.hdr && c.backing == SHM_IQ_BACKING_ANON && !c.owner, "attach by name finds the in-process ring");
  CHECK(shm_iq_ring_view(&v, &p) == 0 && v.hdr == p.hdr, "view");
  CHECK(shm_iq_ring_create(&u, SHM_IQ_BACKING_ANON, NULL, 1, 2048, FS, 0, 0) == 0, "unnamed anon ring");
  shm_iq_ring_prefault(&u);
  shm_iq_ring_detach(&u);
  int16_t a[2][2000], b[2][2000];
  int16_t *dst[2] = {b[0], b[1]};
  for (int k = 0; k < 2000; ++k) a[0][k] = (int16_t)k, a[1][k] = (int16_t)-k;
  void *src[2] = {a[0], a[1]};
  CHECK(shm_iq_ring_copy(&c, 0, 10, 2, dst) == 1, "empty anon ring says not yet");
  for (int w = 0; w < 5; ++w) shm_iq_ring_write(&p, 1000 + w * 1000, src, 1000, 2);
  CHECK(shm_iq_ring_copy(&v, 4000, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000) && !memcmp(b[1], a[1], 4000), "copy through a view");
  CHECK(shm_iq_ring_copy(&c, 500, 100, 2, dst) == -1, "overwritten range fails on the anon ring too");
  shm_iq_ring_detach(&p); /* the producer handle goes first: the registry name is gone, the views keep the memory (ASAN would flag a use after unmap) */
  CHECK(shm_iq_ring_copy(&v, 4000, 1000, 2, dst) == 0 && !memcmp(b[0], a[0], 4000), "view outlives the producer handle");
  shm_iq_ring_detach(&c); shm_iq_ring_detach(&v);
  CHECK(atomic_load(&g_shm_open_calls) == before, "the anonymous path called shm_open (%d calls)", atomic_load(&g_shm_open_calls) - before);
  /* a name that is not (or no longer) in the in-process registry falls back to /dev/shm: that attach is a POSIX attach and is allowed to call shm_open */
  CHECK(shm_iq_ring_attach(&u, name) != 0, "name unregistered with the producer handle");
  CHECK(shm_dir_has("isac_iq_anon_") == 0, "a file appeared in /dev/shm");
  /* control: the counter does see the POSIX path */
  char pn[64];
  snprintf(pn, sizeof(pn), "isac_iq_ctl_%d", (int)getpid());
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_POSIX_SHM, pn, 1, 2048, FS, 0, 0) == 0, "posix create");
  CHECK(atomic_load(&g_shm_open_calls) > before, "control: the POSIX path must be counted");
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", pn); shm_unlink(path);
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_POSIX_SHM, NULL, 1, 2048, FS, 0, 0) == -1, "posix backing needs a name");
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_ANON, NULL, 9, 2048, FS, 0, 0) == -1, "more than SHM_IQ_MAX_CH channels refused");
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_ANON, NULL, 1, 2048, FS, 0, SHM_IQ_MAX_CONS + 1) == -1, "too many consumers refused");
  printf("anonymous backing ok (shm_open calls on the anonymous path: 0)\n");
  return 0;
}

/* N consumers of one anonymous ring: the producer never waits, a slow consumer overruns alone, the table accounts per consumer */
typedef struct { shm_iq_ring_t ring; int id, delay_us; long ok, bad, torn, rejects; atomic_int *stop; } cons_arg_t;
static atomic_int g_cstop;
static void *cons_thread(void *p)
{
  cons_arg_t *a = p;
  static __thread int16_t y[2 * 512];
  int16_t *dst[1] = {y};
  uint64_t pos = 0;
  while (!atomic_load(a->stop)) {
    const int r = shm_iq_ring_copy(&a->ring, pos, 512, 1, dst);
    if (r == 1) { struct timespec w = {0, 20000}; nanosleep(&w, NULL); continue; }
    if (r < 0) {
      const uint64_t vf = atomic_load(&a->ring.hdr->valid_from), head = atomic_load(&a->ring.hdr->head);
      const int ov = pos >= vf;
      const uint64_t to = ov ? head : vf;
      shm_iq_ring_consumer_overrun(&a->ring, a->id, to - pos, ov);
      a->rejects++;
      pos = to;
      continue;
    }
    for (int k = 0; k < 512; ++k)
      if (y[2 * k] != (int16_t)(pos + k) || y[2 * k + 1] != (int16_t)((pos + k) >> 16)) { a->bad++; break; }
    a->ok++;
    pos += 512;
    shm_iq_ring_consumer_update(&a->ring, a->id, pos);
    if (a->delay_us) { struct timespec w = {0, a->delay_us * 1000L}; nanosleep(&w, NULL); }
  }
  return NULL;
}

static int test_n_consumers(void)
{
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_ANON, NULL, 1, 1 << 18, FS, 0, 3) == 0, "create");
  cons_arg_t c[3];
  memset(c, 0, sizeof(c));
  const int delay[3] = {0, 3000, 0}; /* consumer 1 sleeps 3 ms per block: far slower than the producer */
  int ids[4];
  for (int i = 0; i < 3; ++i) {
    CHECK(shm_iq_ring_view(&c[i].ring, &p) == 0, "view");
    ids[i] = shm_iq_ring_consumer_register(&c[i].ring);
    CHECK(ids[i] == i, "slot %d", ids[i]);
    c[i].id = ids[i]; c[i].delay_us = delay[i]; c[i].stop = &g_cstop;
  }
  shm_iq_ring_t v4;
  CHECK(shm_iq_ring_view(&v4, &p) == 0, "view 4");
  CHECK(shm_iq_ring_consumer_register(&v4) == -1 && errno == ENOSPC, "a 4th consumer must be refused");
  shm_iq_ring_consumer_release(&c[2].ring, ids[2]);
  CHECK((ids[3] = shm_iq_ring_consumer_register(&v4)) == 2, "released slot is reused");
  shm_iq_ring_consumer_release(&v4, ids[3]);
  CHECK(shm_iq_ring_consumer_register(&c[2].ring) == 2, "consumer 2 back");
  shm_iq_ring_detach(&v4);
  atomic_store(&g_cstop, 0);
  pthread_t th[3];
  for (int i = 0; i < 3; ++i) pthread_create(&th[i], NULL, cons_thread, &c[i]);
  static int16_t x[2 * 333];
  void *b[1] = {x};
  const uint64_t t0 = shm_iq_now_ns();
  uint64_t t = 0;
  while (t < 3000000 || shm_iq_now_ns() - t0 < 300000000ull) { /* >= 3 M samples and >= 0.3 s so the slow consumer is lapped many times */
    for (int k = 0; k < 333; ++k) { x[2 * k] = (int16_t)(t + k); x[2 * k + 1] = (int16_t)((t + k) >> 16); }
    shm_iq_ring_write(&p, (int64_t)t, b, 333, 1);
    t += 333;
    if ((t / 333) % 8 == 0) { struct timespec w = {0, 20000}; nanosleep(&w, NULL); } /* about 25 MS/s: the fast consumers keep up (ring = 10 ms), the 3 ms/512-sample one cannot */
  }
  const uint64_t wrote_ns = shm_iq_now_ns() - t0;
  atomic_store(&g_cstop, 1);
  for (int i = 0; i < 3; ++i) pthread_join(th[i], NULL);
  const uint64_t o0 = shm_iq_ring_consumer_overruns(&p, 0), o1 = shm_iq_ring_consumer_overruns(&p, 1), o2 = shm_iq_ring_consumer_overruns(&p, 2);
  printf("n-consumers: wrote %llu samples in %.2f s; consumer ok blocks %ld/%ld/%ld, overruns %llu/%llu/%llu (ring total %llu), dropped %llu/%llu/%llu, undetected torn %ld/%ld/%ld\n",
         (unsigned long long)t, wrote_ns * 1e-9, c[0].ok, c[1].ok, c[2].ok, (unsigned long long)o0, (unsigned long long)o1, (unsigned long long)o2,
         (unsigned long long)shm_iq_ring_overruns(&p), (unsigned long long)shm_iq_ring_consumer_dropped(&p, 0), (unsigned long long)shm_iq_ring_consumer_dropped(&p, 1),
         (unsigned long long)shm_iq_ring_consumer_dropped(&p, 2), c[0].bad, c[1].bad, c[2].bad);
  CHECK(c[0].bad == 0 && c[1].bad == 0 && c[2].bad == 0, "a copy reported valid was torn");
  CHECK(o0 == 0 && o2 == 0, "the fast consumers must not overrun (%llu, %llu)", (unsigned long long)o0, (unsigned long long)o2);
  CHECK(o1 > 0 && c[1].ok > 0, "the slow consumer must overrun and still read valid blocks (%llu overruns, %ld ok)", (unsigned long long)o1, c[1].ok);
  CHECK(shm_iq_ring_overruns(&p) == o0 + o1 + o2, "ring total = sum of consumers");
  CHECK(shm_iq_ring_consumer_dropped(&p, 1) >= o1, "dropped samples accounted");
  CHECK(shm_iq_ring_max_lag(&p) >= shm_iq_ring_consumer_lag(&p, 0), "max lag >= lag of consumer 0");
  CHECK(atomic_load(&p.hdr->head) == t, "producer wrote everything: it never waits for consumers");
  for (int i = 0; i < 3; ++i) { shm_iq_ring_consumer_release(&c[i].ring, ids[i]); shm_iq_ring_detach(&c[i].ring); }
  shm_iq_ring_detach(&p);
  /* an untracked ring (n_consumers 0) and a v2 header ignore the table */
  shm_iq_ring_t u;
  CHECK(shm_iq_ring_create(&u, SHM_IQ_BACKING_ANON, NULL, 1, 2048, FS, 0, 0) == 0, "untracked create");
  CHECK(shm_iq_ring_consumer_register(&u) == -1 && errno == ENOSYS, "untracked ring has no consumer slots");
  shm_iq_ring_consumer_update(&u, -1, 5); shm_iq_ring_consumer_overrun(&u, -1, 1, 1); /* negative ids are ignored */
  CHECK(shm_iq_ring_overruns(&u) == 0, "ignored");
  shm_iq_ring_detach(&u);
  printf("n consumers ok\n");
  return 0;
}

/* the consumer table through a POSIX ring and a second process (cross-process accounting) */
static int test_posix_consumer_table(void)
{
  char name[64];
  snprintf(name, sizeof(name), "isac_iq_ct_%d", (int)getpid());
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_POSIX_SHM, name, 1, 4096, FS, 0, 2) == 0, "create");
  static int16_t x[2 * 1000];
  void *b[1] = {x};
  shm_iq_ring_write(&p, 0, b, 1000, 1);
  fflush(stdout);
  pid_t c = fork();
  if (!c) {
    shm_iq_ring_t r;
    if (shm_iq_ring_attach(&r, name)) _exit(2);
    const int id = shm_iq_ring_consumer_register(&r);
    if (id < 0) _exit(3);
    shm_iq_ring_consumer_update(&r, id, 400);
    shm_iq_ring_consumer_overrun(&r, id, 77, 1);
    _exit(0);
  }
  int st;
  waitpid(c, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child exit %d", WEXITSTATUS(st));
  CHECK(shm_iq_ring_consumer_overruns(&p, 0) == 1 && shm_iq_ring_consumer_dropped(&p, 0) == 77 && shm_iq_ring_consumer_lag(&p, 0) == 600 && shm_iq_ring_overruns(&p) == 1,
        "child's accounting visible to the producer (lag %llu)", (unsigned long long)shm_iq_ring_consumer_lag(&p, 0));
  shm_iq_ring_detach(&p);
  char path[80]; snprintf(path, sizeof(path), "/%s", name); shm_unlink(path);
  printf("cross-process consumer table ok\n");
  return 0;
}

static void *late_writer(void *p)
{
  static int16_t x[2 * 30720];
  void *b[1] = {x};
  struct timespec w = {0, 50000000};
  nanosleep(&w, NULL);
  shm_iq_ring_write((shm_iq_ring_t *)p, 5000 + 6 * 30720, b, 30720, 1);
  return NULL;
}

/* The consumer device on an anonymous ring: found through the in-process registry (no shm_open), memcpy path at ring rate = cell rate, overrun accounted in the table */
static int test_anon_device(const char *lib)
{
  char ring[64];
  snprintf(ring, sizeof(ring), "isac_iq_ad_%d", (int)getpid());
  const int before = atomic_load(&g_shm_open_calls);
  shm_iq_ring_t p;
  CHECK(shm_iq_ring_create(&p, SHM_IQ_BACKING_ANON, ring, 1, 1 << 16, 61.44e6, CENTER, 2) == 0, "create");
  static int16_t x[2 * 30720], y[2 * 30720];
  for (int k = 0; k < 30720; ++k) { x[2 * k] = (int16_t)(k * 3); x[2 * k + 1] = (int16_t)(-k); }
  void *b[1] = {x};
  shm_iq_ring_write(&p, 5000, b, 30720, 1);
  shm_iq_ring_write(&p, 5000 + 30720, b, 30720, 1);
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  int (*init)(openair0_device_t *, openair0_config_t *) = (int (*)(openair0_device_t *, openair0_config_t *))dlsym(h, "device_init");
  CHECK(init, "device_init");
  openair0_device_t dev; memset(&dev, 0, sizeof(dev));
  openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.sdr_addrs = (char *)ring; cfg.sample_rate = 61.44e6; cfg.rx_num_channels = 1; cfg.rx_freq[0] = CENTER; cfg.num_rb_dl = 106;
  CHECK(init(&dev, &cfg) == 0, "init");
  CHECK(atomic_load(&p.hdr->cons[0].active) == 1, "the device registered in the consumer table");
  /* no start(): the cursor is at 0 and the two blocks are there */
  void *d[1] = {y};
  openair0_timestamp_t ts;
  CHECK(dev.trx_read_func(&dev, &ts, d, 30720, 1) == 30720 && !memcmp(x, y, sizeof(x)) && ts == 5000, "decim-1 shift-0 read is a copy of the ring (ts %llu)", (unsigned long long)ts);
  CHECK(shm_iq_ring_consumer_lag(&p, 0) == 30720, "lag after the first read");
  /* lap the consumer: write more than the capacity while it sleeps */
  for (int w = 0; w < 4; ++w) shm_iq_ring_write(&p, 5000 + 2 * 30720 + w * 30720, b, 30720, 1);
  /* the consumer skips to the newest data and waits for the next block: a thread delivers it 50 ms later */
  static shm_iq_ring_t *g_late_ring;
  g_late_ring = &p;
  pthread_t th;
  pthread_create(&th, NULL, late_writer, g_late_ring);
  CHECK(dev.trx_read_func(&dev, &ts, d, 30720, 1) == 30720, "read after the overrun");
  pthread_join(th, NULL);
  CHECK(shm_iq_ring_overruns(&p) == 1 && shm_iq_ring_consumer_overruns(&p, 0) == 1 && shm_iq_ring_consumer_dropped(&p, 0) > 0, "overrun accounted (ring %llu, consumer %llu)",
        (unsigned long long)shm_iq_ring_overruns(&p), (unsigned long long)shm_iq_ring_consumer_overruns(&p, 0));
  dev.trx_end_func(&dev);
  CHECK(atomic_load(&p.hdr->cons[0].active) == 0, "slot released at device end");
  CHECK(atomic_load(&g_shm_open_calls) == before, "the device on an anonymous ring called shm_open");
  shm_iq_ring_detach(&p);
  printf("anonymous ring through the consumer device ok\n");
  return 0;
}

/* ring capacity from the host-RAM plan: fixtures are synthetic hosts (labels: fixture, not measured), the budget goes through nr_host_membudget_calc() */
static int test_capacity_plan(void)
{
  const size_t GiB = (size_t)1 << 30;
  const double R122 = 122.88e6, R245 = 245.76e6;
  /* fixture "DGX GB10": unified 128 GB host, MemAvailable 110 GB; ring share = 50 % of the host budget (plan_usrp_ring uses 50 % of the headroom) */
  const size_t dgx = nr_host_membudget_calc(110 * GiB) / 2;
  /* fixture "sens6 x86": 14.8 GB host, MemAvailable 10 GB */
  const size_t s6 = nr_host_membudget_calc(10 * GiB) / 2;
  /* fixture "cloud x86 4 cpu": MemAvailable 6 GB */
  const size_t cl = nr_host_membudget_calc(6 * GiB) / 2;
  const uint64_t W = SHM_IQ_RING_WIDE_TARGET_MS, F = SHM_IQ_RING_WIDE_FLOOR_MS;
  const uint64_t d122 = shm_iq_ring_plan_capacity(dgx, R122, 4, 1, W, F), d245 = shm_iq_ring_plan_capacity(dgx, R245, 4, 1, W, F);
  printf("capacity plan [fixture DGX GB10 110 GB avail, share %zu MB]: 122.88 MS/s x4 -> 2^%d samples, 245.76 MS/s x4 -> 2^%d\n", dgx >> 20, __builtin_ctzll(d122), __builtin_ctzll(d245));
  CHECK(d122 == (1ull << 27), "DGX fixture at 122.88 MS/s must give the old default 2^27, got %llu", (unsigned long long)d122);
  CHECK(d245 == (1ull << 28), "DGX fixture at 245.76 MS/s: the same 1.09 s duration, got %llu", (unsigned long long)d245);
  const uint64_t s122 = shm_iq_ring_plan_capacity(s6, R122, 4, 1, W, F), s245 = shm_iq_ring_plan_capacity(s6, R245, 4, 1, W, F);
  printf("capacity plan [fixture sens6 x86 10 GB avail, share %zu MB]: 122.88 -> 2^%d, 245.76 -> 2^%d\n", s6 >> 20, __builtin_ctzll(s122), __builtin_ctzll(s245));
  CHECK(s122 == (1ull << 27) && s245 == (1ull << 27), "sens6 fixture %llu %llu", (unsigned long long)s122, (unsigned long long)s245);
  const uint64_t c122 = shm_iq_ring_plan_capacity(cl, R122, 4, 1, W, F);
  printf("capacity plan [fixture cloud x86 6 GB avail, share %zu MB]: 122.88 -> 2^%d\n", cl >> 20, __builtin_ctzll(c122));
  CHECK(c122 == (1ull << 26), "cloud fixture %llu", (unsigned long long)c122);
  /* properties over a sweep: power of two, fits the budget, never above the wanted duration's ceil_pow2, monotone in the budget */
  uint64_t prev = 0;
  for (size_t mb = 16; mb <= 65536; mb += mb / 4 + 1) {
    const size_t budget = mb << 20;
    const uint64_t cap = shm_iq_ring_plan_capacity(budget, R122, 4, 1, W, F);
    if (cap) {
      CHECK(!(cap & (cap - 1)) && shm_iq_ring_bytes(4, cap) <= budget && cap <= (1ull << 27), "sweep %zu MB cap %llu", mb, (unsigned long long)cap);
      CHECK(cap >= prev, "monotone");
      CHECK((double)cap >= R122 * F * 1e-3, "floor");
      prev = cap;
    } else CHECK(prev == 0, "once served always served");
  }
  CHECK(shm_iq_ring_plan_capacity(SIZE_MAX, R122, 4, 1, W, F) == (1ull << 27), "unlimited budget = wanted duration");
  CHECK(shm_iq_ring_plan_capacity(shm_iq_ring_bytes(4, 1 << 24) - 1, R122, 4, 1, W, F) == 0, "below the 100 ms floor: refused (0)");
  CHECK(shm_iq_ring_plan_capacity(4 * shm_iq_ring_bytes(4, 1 << 26), R122, 4, 4, W, F) == (1ull << 26), "four rings share the budget");
  CHECK(shm_iq_ring_plan_capacity(SIZE_MAX, 30.72e6, 4, 1, SHM_IQ_RING_NARROW_TARGET_MS, SHM_IQ_RING_NARROW_FLOOR_MS) == (1ull << 22), "narrow 30.72 MS/s x 100 ms -> 2^22");
  CHECK(shm_iq_ring_plan_capacity(1, 0, 4, 1, W, F) == 0 && shm_iq_ring_plan_capacity(1, R122, 0, 1, W, F) == 0 && shm_iq_ring_plan_capacity(1, R122, 4, 0, W, F) == 0, "bad input refused");
  printf("capacity plan ok\n");
  return 0;
}

static int test_passband_scs(const char *lib)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  double (*pb)(const openair0_config_t *) = (double (*)(const openair0_config_t *))dlsym(h, "shm_iq_cell_passband");
  CHECK(pb, "shm_iq_cell_passband");
  openair0_config_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.sample_rate = 122.88e6; cfg.num_rb_dl = 106; cfg.nr_scs_for_raster = 2; /* 60 kHz */
  CHECK(fabs(pb(&cfg) - 0.5 * 106 * 12 * 60e3) < 1, "60 kHz grid passband %.0f", pb(&cfg));
  cfg.nr_scs_for_raster = 1;
  CHECK(fabs(pb(&cfg) - 0.5 * 106 * 12 * 30e3) < 1, "30 kHz grid");
  printf("passband follows the SCS (H3: num_rb_dl x 12 x SCS) ok\n");
  return 0;
}

int main(int argc, char **argv)
{
  int fail = 0;
  fail |= test_ddc(FS, 1, 17.3e6, 20e6);
  fail |= test_ddc(FS, 2, -11e6, 20e6);   /* 106 PRB class: 40 MHz channel at 61.44 MS/s */
  fail |= test_ddc(FS, 2, 5e6, 30e6);     /* 162 PRB class: 60 MHz channel at 61.44 MS/s */
  fail |= test_ddc(FS, 4, 31e6, 10e6);    /* 51 PRB class: 20 MHz channel at 30.72 MS/s */
  fail |= test_ddc(245.76e6, 1, 40e6, 20e6);
  fail |= test_ddc(245.76e6, 4, -37e6, 20e6);  /* 245.76 -> 61.44, 40 MHz channel */
  fail |= test_ddc(245.76e6, 8, 53e6, 10e6);   /* 245.76 -> 30.72, 20 MHz channel */
  fail |= test_ddc(491.52e6, 8, -91e6, 20e6);  /* 491.52 -> 61.44, 40 MHz channel */
  fail |= test_ddc(491.52e6, 16, 77e6, 10e6);  /* 491.52 -> 30.72, 20 MHz channel */
  fail |= test_ring();
  fail |= test_anon_basic();
  fail |= test_n_consumers();
  fail |= test_posix_consumer_table();
  fail |= test_capacity_plan();
  fail |= test_ring_stress();
  fail |= test_v2_header();
  fail |= test_return_after_reload();
  if (argc > 1) fail |= test_away_consumer(argv[1], 1);
  if (argc > 1) fail |= test_away_consumer(argv[1], 0);
  if (argc > 1) fail |= test_fanout(argv[1]);
  if (argc > 1) fail |= test_heartbeat_ahead(argv[1]);
  if (argc > 1) fail |= test_anon_device(argv[1]);
  if (argc > 1) fail |= test_passband_scs(argv[1]);
  if (argc > 1) fail |= test_radio_relay(argv[1]);
  printf(fail ? "FAILED\n" : "ALL OK\n");
  return fail;
}
