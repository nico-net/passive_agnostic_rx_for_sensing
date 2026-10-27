/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* g5_fast_pub: TEST TOOL (MPCI G5 in-context bench), not part of the receiver. Same job and arguments as mpci_file_pub (stored raw IQ captures published as
 * POSIX-shm IQ rings, looping, one paced clock), but fast enough for 245.76 MS/s x 4 RX in real time: the capture files are mapped (pre-faulted into the page
 * cache at start, so the first loop does not read the disk) and every block is written straight into the ring slots (shm_iq_ring_reserve / commit) with the
 * right shift applied in one vectorisable pass; pacing is on absolute time. mpci_file_pub (raw_iq_read per 30720-sample block) reaches about 0.23x real time
 * at 245.76 MS/s x 4 on the DGX [Bench, DGX GB10, 2026-10-27], which made the channelizer load 4x lighter than on air.
 * usage: g5_fast_pub <speed> <seconds> <ring>=<dir>,<samples>,<first_tick>,<channels>,<rate>,<centre_hz>,<shift> [...]   (= mpci_file_pub)
 * Prints MPCI_PUB / MPCI_PUB_END lines (same grammar) and MPCI_PUB_RATE with the achieved real-time factor. Stops on SIGINT/SIGTERM, removes its rings. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "shm_iq_ring.h"

#define MAX_SRC 8
#define BLOCK 65536

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef struct {
  char ring[64], dir[512];
  unsigned long long samples, first_tick;
  unsigned channels, shift;
  double rate, centre;
  const int16_t *map[SHM_IQ_MAX_CH];
  shm_iq_ring_t r;
  int16_t *stage[SHM_IQ_MAX_CH];
  uint64_t pos, tick, loops, written;
} src_t;

static double now_s(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

static void shift_copy(int16_t *restrict d, const int16_t *restrict s, size_t n, unsigned sh)
{
  for (size_t i = 0; i < n; ++i) d[i] = (int16_t)(s[i] >> sh);
}

int main(int argc, char **argv)
{
  if (argc < 4) { fprintf(stderr, "usage: %s <speed> <seconds> <ring>=<dir>,<samples>,<first_tick>,<channels>,<rate>,<centre_hz>,<shift> ...\n", argv[0]); return 2; }
  const double speed = atof(argv[1]), secs = atof(argv[2]);
  if (!(speed > 0 && speed <= 1.0) || !(secs > 0)) { fprintf(stderr, "speed in (0,1], seconds > 0\n"); return 2; }
  src_t P[MAX_SRC];
  int n = 0;
  for (int i = 3; i < argc && n < MAX_SRC; i++, n++) {
    src_t *p = &P[n];
    memset(p, 0, sizeof(*p));
    const char *eq = strchr(argv[i], '=');
    if (!eq || eq - argv[i] >= (int)sizeof(p->ring)) return 2;
    memcpy(p->ring, argv[i], (size_t)(eq - argv[i]));
    if (sscanf(eq + 1, "%511[^,],%llu,%llu,%u,%lf,%lf,%u", p->dir, &p->samples, &p->first_tick, &p->channels, &p->rate, &p->centre, &p->shift) != 7
        || p->channels < 1 || p->channels > SHM_IQ_MAX_CH) { fprintf(stderr, "bad source %s\n", argv[i]); return 2; }
    for (unsigned c = 0; c < p->channels; c++) {
      char f[600];
      snprintf(f, sizeof f, "%s/rx%u.sc16", p->dir, c);
      const int fd = open(f, O_RDONLY);
      if (fd < 0) { perror(f); return 1; }
      void *m = mmap(NULL, (size_t)p->samples * 4, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0); /* populate: page cache now, not during the run */
      close(fd);
      if (m == MAP_FAILED) { perror("mmap"); return 1; }
      p->map[c] = m;
      p->stage[c] = malloc((size_t)BLOCK * 4);
    }
    const uint64_t cap = shm_iq_ring_plan_capacity((size_t)-1, p->rate, p->channels, 1, SHM_IQ_RING_WIDE_TARGET_MS, SHM_IQ_RING_WIDE_FLOOR_MS);
    if (shm_iq_ring_create(&p->r, SHM_IQ_BACKING_POSIX_SHM, p->ring, p->channels, cap, p->rate, p->centre, SHM_IQ_MAX_CONS)) { perror("ring"); return 1; }
    shm_iq_ring_set_meta(&p->r, "g5_fast_pub", SHM_IQ_TS_FREE);
    shm_iq_ring_prefault(&p->r);
    p->tick = p->first_tick;
    printf("MPCI_PUB ring=/%s dir=%s rate=%.0f centre_hz=%.0f channels=%u capacity=%llu samples=%llu speed=%.3f\n", p->ring, p->dir, p->rate, p->centre,
           p->channels, (unsigned long long)cap, p->samples, speed);
  }
  fflush(stdout);
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  const double t0 = now_s();
  double published_s = 0;
  while (!g_stop && now_s() - t0 < secs) {
    for (int i = 0; i < n; i++) {
      src_t *p = &P[i];
      int want = (int)llround((double)BLOCK * p->rate / P[0].rate);
      if (p->pos >= p->samples) { p->pos = 0; p->loops++; shm_iq_ring_new_epoch(&p->r, p->centre); } /* loop = new run, as mpci_file_pub */
      if ((uint64_t)want > p->samples - p->pos) want = (int)(p->samples - p->pos);
      int16_t *slots[SHM_IQ_MAX_CH];
      const int room = shm_iq_ring_reserve(&p->r, want, slots);
      const int direct = room == want;
      for (unsigned c = 0; c < p->channels; c++) shift_copy(direct ? slots[c] : p->stage[c], p->map[c] + 2 * p->pos, (size_t)want * 2, p->shift);
      if (!direct || shm_iq_ring_commit(&p->r, (int64_t)p->tick, want) != 0) {
        if (direct) for (unsigned c = 0; c < p->channels; c++) memcpy(p->stage[c], slots[c], (size_t)want * 4);
        shm_iq_ring_write(&p->r, (int64_t)p->tick, (void *const *)p->stage, want, p->channels);
      }
      p->pos += (uint64_t)want; p->tick += (uint64_t)want; p->written += (uint64_t)want;
    }
    published_s += (double)BLOCK / P[0].rate;
    const double w = t0 + published_s / speed - now_s();
    if (w > 0) { const struct timespec ts = {(time_t)w, (long)((w - (time_t)w) * 1e9)}; nanosleep(&ts, NULL); }
  }
  const double el = now_s() - t0;
  for (int i = 0; i < n; i++) {
    printf("MPCI_PUB_RATE ring=/%s rt_factor=%.3f (signal %.1f s in %.1f s)\n", P[i].ring, (double)P[i].written / P[i].rate / el, (double)P[i].written / P[i].rate, el);
    printf("MPCI_PUB_END ring=/%s written=%llu loops=%llu overruns=%llu\n", P[i].ring, (unsigned long long)P[i].written, (unsigned long long)P[i].loops,
           (unsigned long long)shm_iq_ring_overruns(&P[i].r));
    shm_iq_ring_detach(&P[i].r);
    char path[96];
    snprintf(path, sizeof(path), "/%s", P[i].ring);
    shm_unlink(path);
  }
  return 0;
}
