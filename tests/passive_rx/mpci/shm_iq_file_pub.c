/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Offline stand-in for shm_iq_uhd_pub: publishes a recorded raw IQ capture (rx<c>.sc16 + capture_info.json, the
 * capture_raw / raw_iq format) into a shm IQ ring at a wall-clock paced speed, so the M1 fan-out (shm_iq consumers,
 * shm_iq_chan) can be validated on real cells without a radio. Same ring API and tick mapping as the UHD publisher
 * (tick = first_sample_tick + index). --shift N applies the X4x0 rxshift of usrp_lib (arithmetic >> N, default 2), as
 * raw_iq ISAC_RAW_IQ_SHIFT=2 does for single-process replays.
 *   shm_iq_file_pub --dir CAPDIR [--ring isac_iq] [--speed 0.25] [--chunk 61440] [--shift 2] [--capacity 2^23] [--hold-s 0] [--go-file F]
 * --go-file: publish one chunk (consumers need head > 0 to finish device_init), then wait until F exists before pacing the rest, so
 * consumers started in between read the capture from its start like a raw_iq replay does.
 * Log: SHMIQ_FILEPUB_READY / _STAT (every 5 s wall) / _END. Exits at the end of the capture (consumers then see
 * PRODUCER_LOST after their timeout and stop cleanly), or on SIGINT. */
#define _GNU_SOURCE
#include "shm_iq_ring.h"
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* first number after "key": in a small JSON file (capture_info.json is flat for the keys used here) */
static double jnum(const char *s, const char *key)
{
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(s, pat);
  if (!p || !(p = strchr(p + strlen(pat), ':'))) { fprintf(stderr, "SHMIQ_FILEPUB_ERR capture_info.json has no %s\n", key); exit(2); }
  return strtod(p + 1, NULL);
}

int main(int argc, char **argv)
{
  const char *dir = NULL, *ring = "isac_iq", *go = NULL;
  double speed = 0.25, hold_s = 0;
  int chunk = 61440, shift = 2;
  uint64_t capacity = 1ull << 23;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *a = argv[i], *v = argv[i + 1];
    if (!strcmp(a, "--dir")) dir = v;
    else if (!strcmp(a, "--ring")) ring = v;
    else if (!strcmp(a, "--speed")) speed = atof(v);
    else if (!strcmp(a, "--chunk")) chunk = atoi(v);
    else if (!strcmp(a, "--shift")) shift = atoi(v);
    else if (!strcmp(a, "--capacity")) capacity = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--hold-s")) hold_s = atof(v);
    else if (!strcmp(a, "--go-file")) go = v;
    else { fprintf(stderr, "unknown option %s\n", a); return 1; }
  }
  if (!dir || !(speed > 0 && speed <= 1) || chunk < 1024 || shift < 0 || shift > 15) {
    fprintf(stderr, "usage: shm_iq_file_pub --dir CAPDIR [--ring R] [--speed (0,1]] [--chunk N>=1024] [--shift 0..15] [--capacity N] [--hold-s S]\n");
    return 1;
  }
  char path[1024], info[8192] = {0};
  snprintf(path, sizeof(path), "%s/capture_info.json", dir);
  FILE *f = fopen(path, "r");
  if (!f || fread(info, 1, sizeof(info) - 1, f) == 0) { perror(path); return 2; }
  fclose(f);
  const unsigned nch = (unsigned)jnum(info, "channels");
  const uint64_t samples = (uint64_t)jnum(info, "samples_per_channel"), tick0 = (uint64_t)jnum(info, "first_sample_tick");
  const double rate = jnum(info, "sample_rate_hz"), centre = jnum(info, "frequency_hz");
  if (nch < 1 || nch > SHM_IQ_MAX_CH) { fprintf(stderr, "SHMIQ_FILEPUB_ERR channels %u\n", nch); return 2; }
  const int16_t *iq[SHM_IQ_MAX_CH];
  for (unsigned c = 0; c < nch; ++c) {
    snprintf(path, sizeof(path), "%s/rx%u.sc16", dir, c);
    const int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || (uint64_t)st.st_size < samples * 4) { perror(path); return 2; }
    void *p = mmap(NULL, samples * 4, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    madvise(p, samples * 4, MADV_SEQUENTIAL);
    iq[c] = p;
  }
  shm_iq_ring_t r;
  if (shm_iq_ring_create(&r, ring, nch, capacity, rate, centre)) { perror("ring"); return 2; }
  shm_iq_ring_set_meta(&r, "file", SHM_IQ_TS_FREE);
  int16_t *buf[SHM_IQ_MAX_CH];
  for (unsigned c = 0; c < nch; ++c) buf[c] = malloc((size_t)chunk * 4);
  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
  fprintf(stderr, "SHMIQ_FILEPUB_READY ring=/%s dir=%s channels=%u rate=%.0f center_hz=%.3f samples=%" PRIu64 " first_tick=%" PRIu64
                  " speed=%.3f shift=%d capacity=%" PRIu64 "\n", ring, dir, nch, rate, centre, samples, tick0, speed, shift, r.hdr->capacity);
  uint64_t t0 = shm_iq_now_ns(), pos = 0, late_max_us = 0;
  if (go) { /* prime one chunk, then wait for the go file; pacing restarts from it */
    for (unsigned c = 0; c < nch; ++c)
      for (int k = 0; k < 2 * chunk; ++k) buf[c][k] = (int16_t)(iq[c][k] >> shift);
    shm_iq_ring_write(&r, (int64_t)tick0, (void *const *)buf, chunk, nch);
    pos = (uint64_t)chunk;
    fprintf(stderr, "SHMIQ_FILEPUB_PRIMED waiting for %s\n", go);
    while (!g_stop && access(go, F_OK) != 0) { atomic_store(&r.hdr->heartbeat_ns, shm_iq_now_ns()); usleep(20000); } /* alive, not stalled */
    t0 = shm_iq_now_ns() - (uint64_t)((double)pos / rate / speed * 1e9);
    fprintf(stderr, "SHMIQ_FILEPUB_GO\n");
  }
  uint64_t next_stat = t0 + 5000000000ull;
  while (!g_stop && pos < samples) {
    const int n = samples - pos < (uint64_t)chunk ? (int)(samples - pos) : chunk;
    /* pace: sample pos+n is due at t0 + (pos+n)/rate/speed (wall-anchored, no drift) */
    const uint64_t due = t0 + (uint64_t)((double)(pos + n) / rate / speed * 1e9);
    uint64_t now = shm_iq_now_ns();
    if (now < due) { struct timespec w = {(time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull)}; nanosleep(&w, NULL); }
    else if ((now - due) / 1000 > late_max_us) late_max_us = (now - due) / 1000;
    for (unsigned c = 0; c < nch; ++c) {
      const int16_t *s = iq[c] + 2 * pos;
      for (int k = 0; k < 2 * n; ++k) buf[c][k] = (int16_t)(s[k] >> shift); /* arithmetic shift = simde_mm256_srai_epi16 in usrp_lib */
    }
    shm_iq_ring_write(&r, (int64_t)(tick0 + pos), (void *const *)buf, n, nch);
    pos += (uint64_t)n;
    if ((now = shm_iq_now_ns()) > next_stat) {
      fprintf(stderr, "SHMIQ_FILEPUB_STAT t=%.1f pos=%" PRIu64 " capture_s=%.3f late_max_us=%" PRIu64 "\n", (now - t0) * 1e-9, pos, pos / rate, late_max_us);
      late_max_us = 0; next_stat = now + 5000000000ull;
    }
  }
  fprintf(stderr, "SHMIQ_FILEPUB_END pos=%" PRIu64 " capture_s=%.3f wall_s=%.1f %s\n", pos, pos / rate, (shm_iq_now_ns() - t0) * 1e-9,
          g_stop ? "stopped" : "eof");
  for (double h = 0; h < hold_s && !g_stop; h += 0.1) usleep(100000);
  shm_unlink(r.name);
  return 0;
}
