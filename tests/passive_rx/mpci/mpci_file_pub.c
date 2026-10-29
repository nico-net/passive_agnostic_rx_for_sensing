/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci_file_pub: TEST TOOL (multi-PCI plan A8 replay fixture), not part of the receiver. Publishes stored raw IQ captures (the raw_iq
 * directory format: rx<c>.sc16 + capture_info.json, read with raw_iq_source as the raw_iq device does) as POSIX-shm IQ rings
 * (radio/COMMON/shm_iq_ring.h), one ring per capture, so ONE receiver process serves one cell per ring through the shm_iq consumer device:
 * the receiver's cell k reads the ring "nrcell_<inventory id>" (executables/nr-uesoftmodem.c, A8). The captures are published at their own
 * centre and rate (the consumer device shifts / decimates to the cell's -C / -r); all rings advance on ONE paced clock.
 *
 * usage: mpci_file_pub <speed> <seconds> <ring>=<dir>,<samples>,<first_tick>,<channels>,<rate>,<centre_hz>,<shift> [<ring>=... ...]
 *   speed    fraction of real time (e.g. 0.25, as the raw_iq replays), seconds = wall time to publish (the capture loops, each loop
 *            starts a new run: the consumers see a stream discontinuity), shift = right shift of the samples (the raw_iq replays use 2).
 * MPCI-LIVE: an argument "watch=<path of cells.json>" (before the sources) makes the publisher follow the inventory like the in-process channelizer does: a ring whose
 * id ("nrcell_<id>") no longer appears in the file is paused (marked away until SHM_IQ_AWAY_PAUSED: its consumer's read returns PAUSED, the receiver parks the slot
 * thread), a ring whose id is back is resumed (new run: a forward gap). The file is polled twice a second; the match is a plain text search for "\"<id>\"".
 * Stops on SIGINT/SIGTERM, removes its rings. Arguments of a test tool, not options of the receiver. */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "raw_iq_source.h"
#include "shm_iq_ring.h"

#define MAX_SRC 8
#define BLOCK 30720 /* 0.25 ms at 122.88 MS/s */

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef struct {
  char ring[64], dir[512];
  uint64_t samples, first_tick;
  unsigned channels, shift;
  double rate, centre;
  raw_iq_source src;
  shm_iq_ring_t r;
  int16_t *buf[SHM_IQ_MAX_CH];
  uint64_t tick, loops, written;
  int paused;
} pub_t;

static int parse(const char *a, pub_t *p)
{
  const char *eq = strchr(a, '=');
  if (!eq || eq - a >= (int)sizeof(p->ring))
    return -1;
  memcpy(p->ring, a, (size_t)(eq - a));
  p->ring[eq - a] = 0;
  char dir[512];
  unsigned long long n, t;
  if (sscanf(eq + 1, "%511[^,],%llu,%llu,%u,%lf,%lf,%u", dir, &n, &t, &p->channels, &p->rate, &p->centre, &p->shift) != 7)
    return -1;
  snprintf(p->dir, sizeof(p->dir), "%s", dir);
  p->samples = n;
  p->first_tick = t;
  return p->channels >= 1 && p->channels <= SHM_IQ_MAX_CH && p->rate > 0 ? 0 : -1;
}

static double now_s(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + 1e-9 * t.tv_nsec;
}

int main(int argc, char **argv)
{
  if (argc < 4) {
    fprintf(stderr, "usage: %s <speed> <seconds> <ring>=<dir>,<samples>,<first_tick>,<channels>,<rate>,<centre_hz>,<shift> ...\n", argv[0]);
    return 2;
  }
  const double speed = atof(argv[1]), secs = atof(argv[2]);
  if (!(speed > 0 && speed <= 1.0) || !(secs > 0)) {
    fprintf(stderr, "speed must be in (0,1], seconds > 0\n");
    return 2;
  }
  pub_t P[MAX_SRC];
  int n = 0;
  const char *watch = NULL;
  int first = 3;
  if (argc > 3 && !strncmp(argv[3], "watch=", 6)) {
    watch = argv[3] + 6;
    first = 4;
  }
  for (int i = first; i < argc && n < MAX_SRC; i++, n++) {
    memset(&P[n], 0, sizeof(P[n]));
    if (parse(argv[i], &P[n])) {
      fprintf(stderr, "bad source '%s'\n", argv[i]);
      return 2;
    }
  }
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  for (int i = 0; i < n; i++) {
    pub_t *p = &P[i];
    if (raw_iq_open(&p->src, p->dir, p->samples, p->first_tick, p->channels, p->rate, p->centre, p->shift)) {
      fprintf(stderr, "MPCI_PUB cannot open %s: %s\n", p->dir, strerror(errno));
      return 1;
    }
    /* ring duration 1 s (the wideband target of shm_iq_ring.h): the capacity comes from the rate, not a constant */
    const uint64_t cap = shm_iq_ring_plan_capacity((size_t)-1, p->rate, p->channels, 1, SHM_IQ_RING_WIDE_TARGET_MS, SHM_IQ_RING_WIDE_FLOOR_MS);
    if (shm_iq_ring_create(&p->r, SHM_IQ_BACKING_POSIX_SHM, p->ring, p->channels, cap, p->rate, p->centre, SHM_IQ_MAX_CONS)) {
      fprintf(stderr, "MPCI_PUB cannot create ring /%s: %s\n", p->ring, strerror(errno));
      return 1;
    }
    shm_iq_ring_set_meta(&p->r, "mpci_file_pub", SHM_IQ_TS_FREE);
    for (unsigned c = 0; c < p->channels; c++)
      p->buf[c] = malloc((size_t)BLOCK * 4);
    p->tick = p->first_tick;
    printf("MPCI_PUB ring=/%s dir=%s rate=%.0f centre_hz=%.0f channels=%u capacity=%llu samples=%llu speed=%.3f\n", p->ring, p->dir, p->rate, p->centre,
           p->channels, (unsigned long long)cap, (unsigned long long)p->samples, speed);
  }
  fflush(stdout);
  const double t0 = now_s();
  double published_s = 0; /* signal time published so far (all rings advance together, on the first ring's rate) */
  double next_watch = 0;
  while (!g_stop && now_s() - t0 < secs) {
    if (watch && now_s() >= next_watch) { /* follow the inventory: pause the rings of cells that left it, resume those that came back */
      next_watch = now_s() + 0.5;
      static char txt[65536];
      FILE *fp = fopen(watch, "r");
      size_t len = fp ? fread(txt, 1, sizeof(txt) - 1, fp) : 0;
      if (fp)
        fclose(fp);
      txt[len] = 0;
      for (int i = 0; fp && i < n; i++) {
        pub_t *p = &P[i];
        char key[96];
        snprintf(key, sizeof(key), "\"%s\"", strncmp(p->ring, "nrcell_", 7) ? p->ring : p->ring + 7);
        const int present = strstr(txt, key) != NULL;
        if (!present && !p->paused) {
          shm_iq_ring_set_away(&p->r, SHM_IQ_AWAY_PAUSED);
          p->paused = 1;
          printf("MPCI_PUB ring=/%s paused (left the inventory)\n", p->ring);
          fflush(stdout);
        } else if (present && p->paused) {
          shm_iq_ring_return(&p->r, p->centre);
          p->paused = 0;
          printf("MPCI_PUB ring=/%s resumed (back in the inventory)\n", p->ring);
          fflush(stdout);
        }
      }
    }
    for (int i = 0; i < n; i++) {
      pub_t *p = &P[i];
      const int want = (int)llround((double)BLOCK * p->rate / P[0].rate);
      uint64_t ts;
      int got = raw_iq_read(&p->src, (void **)p->buf, want < BLOCK ? want : BLOCK, p->channels, &ts);
      if (got <= 0) { /* end of the capture: loop, as a new run (a discontinuity for the consumers) */
        p->src.position = 0;
        p->loops++;
        shm_iq_ring_new_epoch(&p->r, p->centre);
        got = raw_iq_read(&p->src, (void **)p->buf, want < BLOCK ? want : BLOCK, p->channels, &ts);
        if (got <= 0)
          continue;
      }
      if (!p->paused)
        shm_iq_ring_write(&p->r, (int64_t)p->tick, (void *const *)p->buf, got, p->channels);
      p->tick += (uint64_t)got; /* a paused ring's clock keeps running: the resume is a forward gap */
      p->written += (uint64_t)got;
    }
    published_s += (double)BLOCK / P[0].rate;
    const double due = t0 + published_s / speed, w = due - now_s();
    if (w > 0)
      usleep((useconds_t)(w * 1e6));
  }
  for (int i = 0; i < n; i++) {
    printf("MPCI_PUB_END ring=/%s written=%llu loops=%llu overruns=%llu\n", P[i].ring, (unsigned long long)P[i].written, (unsigned long long)P[i].loops,
           (unsigned long long)shm_iq_ring_overruns(&P[i].r));
    shm_iq_ring_detach(&P[i].r);
    char path[96];
    snprintf(path, sizeof(path), "/%s", P[i].ring);
    shm_unlink(path);
    shm_iq_radio_remove(P[i].ring);
    raw_iq_close(&P[i].src);
  }
  return 0;
}
