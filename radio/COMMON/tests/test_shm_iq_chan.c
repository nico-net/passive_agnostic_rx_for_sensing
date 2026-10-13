/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI shared channelizer test (CPU backend). A producer process publishes a 4-antenna 245.76 MS/s wideband ring (one tone per
 * antenna and per cell); the real shm_iq_chan executable (started from a plan.json) writes <ring>_A (decim 4) and <ring>_B (decim 8);
 * the test reads them with shm_iq_ring_copy and checks: own tone at 0 dB on the right antenna, other antennas / other cell < -60 dB,
 * timestamps continuous (no gaps) and equal to (base_tick + ring_pos) / decim after an input discontinuity (a gap in EVERY output
 * ring), a producer retune (rf_epoch + new shifts), a 1 s publisher stall (channelizer and its rings' writer stay alive, then a
 * gap), `away` mirrored with a moved return time, and a clean channelizer exit on producer loss.
 * Usage: test_shm_iq_chan <path to shm_iq_chan> */
#define _GNU_SOURCE
#include "../shm_iq_ring.h"
#include <complex.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); goto fail; } } while (0)

#define FS 245.76e6
#define PER 245760 /* one 1 ms period: every tone is a multiple of 1 kHz */
#define NCH 4
#define BASE_TICK 1000000005LL /* deliberately not a multiple of the decimations */
static const double C0 = 3.6e9, CELL[2] = {3.6e9 - 45e6, 3.6e9 + 70e6}, BW[2] = {40e6, 20e6};
static const int DEC[2] = {4, 8};
static const double OFF[2][NCH] = {{-2e6, -3.5e6, -5e6, -6.5e6}, {2e6, -1e6, 4e6, -3e6}}; /* tone offsets from the cell centre, per antenna */
static const double AMP = 3000;

typedef struct { volatile int cmd, ack, stop; volatile double centre; volatile long jump; volatile uint64_t until; } ctl_t;
enum { CMD_NONE, CMD_JUMP, CMD_RETUNE, CMD_STALL, CMD_AWAY };

static void pattern(int16_t *p[NCH], double ring_centre)
{
  for (int a = 0; a < NCH; ++a)
    for (int k = 0; k < PER; ++k) {
      double re = 0, im = 0;
      for (int c = 0; c < 2; ++c) {
        const long q = lrint((CELL[c] + OFF[c][a] - ring_centre) / 1000.0);
        const double ph = 2 * M_PI * (double)((((q % PER) + PER) % PER) * (long)k % PER) / PER;
        re += AMP * cos(ph); im += AMP * sin(ph);
      }
      p[a][2 * k] = (int16_t)lrint(re); p[a][2 * k + 1] = (int16_t)lrint(im);
    }
}

static void producer(const char *ring, ctl_t *ctl)
{
  shm_iq_ring_t r;
  if (shm_iq_ring_create(&r, ring, NCH, 1u << 22, FS, C0)) _exit(10);
  shm_iq_ring_set_meta(&r, "radiotest", SHM_IQ_TS_PPS);
  int16_t *pat[NCH];
  for (int a = 0; a < NCH; ++a) pat[a] = malloc((size_t)PER * 4);
  pattern(pat, C0);
  const int B = 61440;
  int16_t *blk[NCH];
  for (int a = 0; a < NCH; ++a) blk[a] = malloc((size_t)B * 4);
  int64_t tick = BASE_TICK;
  while (!ctl->stop) {
    if (ctl->cmd == CMD_JUMP) { tick += ctl->jump; ctl->cmd = CMD_NONE; ctl->ack = 1; }
    else if (ctl->cmd == CMD_RETUNE) {
      pattern(pat, ctl->centre);
      shm_iq_ring_new_epoch(&r, ctl->centre);
      ctl->cmd = CMD_NONE; ctl->ack = 1;
    } else if (ctl->cmd == CMD_STALL) { /* RFSTALL-like: no write for 1 s (> the channelizer's 300 ms timeout), the radio clock moved on */
      usleep(1000000);
      tick += 245760;
      ctl->cmd = CMD_NONE; ctl->ack = 1;
    } else if (ctl->cmd == CMD_AWAY) { /* time-shared window: away, then the expected return moves 1 s later */
      const uint64_t u = shm_iq_now_ns() + 2000000000ull;
      shm_iq_ring_set_away(&r, u);
      usleep(400000);
      shm_iq_ring_set_away(&r, u + 1000000000ull);
      ctl->until = u + 1000000000ull; ctl->ack = 1;
      while (ctl->cmd == CMD_AWAY && !ctl->stop) usleep(1000);
      shm_iq_ring_return(&r, ctl->centre);
    }
    for (int a = 0; a < NCH; ++a)
      for (int k = 0; k < B;) {
        const int i = (int)((tick + k) % PER), n = B - k < PER - i ? B - k : PER - i;
        memcpy(blk[a] + 2 * k, pat[a] + 2 * i, (size_t)n * 4);
        k += n;
      }
    shm_iq_ring_write(&r, tick, (void *const *)blk, B, NCH);
    tick += B;
    usleep(4000); /* 1/16 of real time: the test machine is small */
  }
  _exit(0); /* ring left behind: its heartbeat stops and this pid is gone (waitpid'ed by the test) */
}

static double tone_db(const int16_t *x, int len, double f, double fs)
{
  double complex acc = 0;
  for (int k = 0; k < len; ++k) acc += (x[2 * k] + I * x[2 * k + 1]) * cexp(-I * 2 * M_PI * fmod(f * k / fs, 1.0));
  return 20 * log10(cabs(acc) / len / AMP + 1e-9);
}

static int attach_wait(shm_iq_ring_t *r, const char *name, double s)
{
  for (int i = 0; i < (int)(s * 100); ++i) {
    if (shm_iq_ring_attach(r, name) == 0 && r->hdr->head > 0) return 0;
    if (r->hdr) shm_iq_ring_detach(r);
    usleep(10000);
  }
  return -1;
}

static int wait_head(shm_iq_ring_t *r, uint64_t want, double s)
{
  for (int i = 0; i < (int)(s * 100); ++i) { if (atomic_load(&r->hdr->head) >= want) return 0; usleep(10000); }
  return -1;
}

/* tones of cell c over [pos, pos+len) of its ring; ring_centre = where the producer's window is */
static int check_tones(shm_iq_ring_t *r, int c, uint64_t pos, double ring_centre, const char *what)
{
  const int len = 32768;
  int16_t *buf[NCH];
  for (int a = 0; a < NCH; ++a) buf[a] = malloc((size_t)len * 4);
  const double fs = FS / DEC[c];
  int rc = 0;
  if (shm_iq_ring_copy(r, pos, len, NCH, buf) != 0) { fprintf(stderr, "FAIL %s cell %d: range not readable\n", what, c); rc = 1; }
  for (int a = 0; a < NCH && !rc; ++a) {
    const double own = tone_db(buf[a], len, OFF[c][a], fs);
    double worst_ant = -200, worst_other = -200;
    for (int b = 0; b < NCH; ++b) if (b != a) { double d = tone_db(buf[a], len, OFF[c][b], fs); if (d > worst_ant) worst_ant = d; }
    double fo = fmod(CELL[1 - c] + OFF[1 - c][a] - CELL[c], fs);
    if (fo > fs / 2) fo -= fs; else if (fo < -fs / 2) fo += fs;
    worst_other = tone_db(buf[a], len, fo, fs);
    printf("  %s cell %c ant %d: own %.4f dB, other antennas %.1f dB, other cell (folded %.2f MHz) %.1f dB\n", what, 'A' + c, a, own, worst_ant, fo / 1e6, worst_other);
    if (fabs(own) > 0.05 || worst_ant > -60 || worst_other > -60) { fprintf(stderr, "FAIL tone levels\n"); rc = 1; }
  }
  (void)ring_centre;
  for (int a = 0; a < NCH; ++a) free(buf[a]);
  return rc;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s <shm_iq_chan>\n", argv[0]); return 2; }
  char ring[64], ringA[80], ringB[80], plan[128], logf[128];
  snprintf(ring, sizeof(ring), "chtest%d", (int)getpid());
  snprintf(ringA, sizeof(ringA), "%s_A", ring); snprintf(ringB, sizeof(ringB), "%s_B", ring);
  snprintf(plan, sizeof(plan), "/tmp/%s_plan.json", ring); snprintf(logf, sizeof(logf), "/tmp/%s_chan.log", ring);
  FILE *f = fopen(plan, "w");
  if (!f) return 2;
  fprintf(f, "{\"version\": 2, \"ring\": \"%s\", \"centre_hz\": %.0f, \"windows\": [{\"ring\": \"%s\", \"cells\": [\"A\", \"B\"]}], \"cells\": ["
             "{\"tag\": \"A\", \"C_hz\": %.0f, \"r\": 106, \"decim\": 4, \"bw_hz\": %.0f, \"env\": {}}, "
             "{\"tag\": \"B\", \"C_hz\": %.0f, \"r\": 51, \"decim\": 8, \"bw_hz\": %.0f, \"env\": {}}], \"unserved\": []}\n",
          ring, C0, ring, CELL[0], BW[0], CELL[1], BW[1]);
  fclose(f);
  ctl_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  shm_iq_ring_t in = {0}, rA = {0}, rB = {0};
  shm_iq_ring_t *rr[2] = {&rA, &rB};
  pid_t pp = fork(), cp = -1;
  if (pp == 0) producer(ring, ctl);
  int ok = 0, waited_p = 0;
  CHECK(attach_wait(&in, ring, 10) == 0, "no input ring");
  cp = fork();
  if (cp == 0) {
    if (!freopen(logf, "w", stderr)) _exit(126);
    execl(argv[1], argv[1], "--cells-json", plan, "--threads", "2", "--timeout-ms", "300", "--out-ms", "40", "--stat-s", "0", (char *)NULL);
    _exit(127);
  }
  CHECK(attach_wait(&rA, ringA, 20) == 0 && attach_wait(&rB, ringB, 20) == 0, "no output rings");
  for (int c = 0; c < 2; ++c) {
    shm_iq_hdr_t *h = rr[c]->hdr;
    CHECK(h->channels == NCH && fabs(h->rate - FS / DEC[c]) < 1e-6 && fabs(shm_iq_ring_center(rr[c]) - CELL[c]) < 1e-3, "header cell %d: ch %u rate %.0f centre %.1f", c, h->channels, h->rate,
          shm_iq_ring_center(rr[c]));
    CHECK(h->version >= 2 && !strcmp(h->radio_id, "radiotest") && h->time_source == SHM_IQ_TS_PPS, "meta not copied (%s, %u)", h->radio_id, h->time_source);
    CHECK(wait_head(rr[c], 200000, 30) == 0, "cell %d produced no data", c);
  }
  printf("-- steady state\n");
  uint64_t s0[2] = {rA.hdr->valid_from + 60000, rB.hdr->valid_from + 60000};
  for (int c = 0; c < 2; ++c) {
    CHECK(wait_head(rr[c], s0[c] + 32768, 30) == 0, "short");
    CHECK(check_tones(rr[c], c, s0[c], C0, "steady") == 0, "tones");
  }
  for (int c = 0; c < 2; ++c) CHECK(rr[c]->hdr->gaps == 0, "cell %d: %lu spurious gaps before the discontinuity", c, (unsigned long)rr[c]->hdr->gaps);
  { /* first-run timestamps are (base_tick + pos0 + k D) / D for the common start pos0 (a multiple of the largest decimation) */
    const int64_t tA = rA.hdr->tick_offset + (int64_t)rA.hdr->valid_from, tB = rB.hdr->tick_offset + (int64_t)rB.hdr->valid_from;
    int found = 0;
    for (int64_t p0 = ((tA * 4 - BASE_TICK) / 8) * 8 - 16; p0 < ((tA * 4 - BASE_TICK) / 8) * 8 + 32 && !found; p0 += 8)
      found = (BASE_TICK + p0) / 4 == tA && (BASE_TICK + p0) / 8 == tB;
    CHECK(found, "output start ticks %ld / %ld do not come from one input position", (long)tA, (long)tB);
  }
  printf("-- input discontinuity\n");
  uint64_t gaps0[2] = {rA.hdr->gaps, rB.hdr->gaps};
  ctl->jump = 1000003; ctl->cmd = CMD_JUMP;
  for (int i = 0; i < 1000 && !ctl->ack; ++i) usleep(5000);
  CHECK(ctl->ack, "producer did not jump");
  ctl->ack = 0;
  for (int c = 0; c < 2; ++c) {
    for (int i = 0; i < 2000 && rr[c]->hdr->gaps == gaps0[c]; ++i) usleep(5000);
    CHECK(rr[c]->hdr->gaps > gaps0[c], "cell %d: input discontinuity did not become a gap", c);
    usleep(100000);
    const uint64_t vf = rr[c]->hdr->valid_from, vfi = in.hdr->valid_from, alig = (vfi + 7) / 8 * 8;
    const int64_t ts = rr[c]->hdr->tick_offset + (int64_t)vf, want = (in.hdr->base_tick + (int64_t)alig) / DEC[c];
    CHECK(ts == want, "cell %d: timestamp after the gap %ld, direct consumer would give %ld", c, (long)ts, (long)want);
    CHECK(wait_head(rr[c], vf + 100000, 30) == 0, "no data after the gap");
    CHECK(check_tones(rr[c], c, vf + 65536, C0, "post-gap") == 0, "tones");
    printf("  cell %c: gap -> valid_from %lu, ts %ld == (input tick)/%d\n", 'A' + c, (unsigned long)vf, (long)ts, DEC[c]);
  }
  printf("-- producer retune\n");
  const uint64_t ep0[2] = {rA.hdr->rf_epoch, rB.hdr->rf_epoch};
  ctl->centre = C0 + 10e6; ctl->cmd = CMD_RETUNE;
  for (int i = 0; i < 1000 && !ctl->ack; ++i) usleep(5000);
  CHECK(ctl->ack, "producer did not retune");
  for (int c = 0; c < 2; ++c) {
    for (int i = 0; i < 2000 && rr[c]->hdr->rf_epoch == ep0[c]; ++i) usleep(5000);
    CHECK(rr[c]->hdr->rf_epoch > ep0[c], "cell %d: retune not propagated", c);
    usleep(100000);
    const uint64_t vf = rr[c]->hdr->valid_from;
    CHECK(wait_head(rr[c], vf + 140000, 30) == 0, "no data after retune");
    CHECK(check_tones(rr[c], c, vf + 100000, C0 + 10e6, "post-retune") == 0, "tones");
    CHECK(fabs(shm_iq_ring_center(rr[c]) - CELL[c]) < 1e-3, "cell centre changed");
  }
  printf("-- publisher stall 1 s\n");
  {
    const uint64_t g1[2] = {rA.hdr->gaps, rB.hdr->gaps};
    ctl->ack = 0; ctl->cmd = CMD_STALL;
    for (int i = 0; i < 1000 && !ctl->ack; ++i) usleep(5000);
    CHECK(ctl->ack, "producer did not stall");
    CHECK(waitpid(cp, NULL, WNOHANG) == 0, "channelizer exited during a 1 s publisher stall");
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 2000 && rr[c]->hdr->gaps == g1[c]; ++i) usleep(5000);
      CHECK(rr[c]->hdr->gaps > g1[c], "cell %d: stall did not become a gap", c);
      CHECK(wait_head(rr[c], rr[c]->hdr->valid_from + 70000, 30) == 0, "cell %d: no data after the stall", c);
      CHECK(kill(atomic_load(&rr[c]->hdr->producer_pid), 0) == 0, "cell %d: output writer gone", c);
    }
  }
  printf("-- away, return time moved\n");
  {
    ctl->ack = 0; ctl->cmd = CMD_AWAY;
    for (int i = 0; i < 1000 && !ctl->ack; ++i) usleep(5000);
    CHECK(ctl->ack, "producer did not go away");
    usleep(150000); /* away for 550 ms > the 300 ms timeout */
    CHECK(waitpid(cp, NULL, WNOHANG) == 0, "channelizer exited while the publisher was away");
    for (int c = 0; c < 2; ++c)
      CHECK(atomic_load(&rr[c]->hdr->away) && atomic_load(&rr[c]->hdr->dwell_until_ns) == ctl->until, "cell %d: away %u dwell %lu, publisher's %lu", c,
            (unsigned)rr[c]->hdr->away, (unsigned long)rr[c]->hdr->dwell_until_ns, (unsigned long)ctl->until);
    const uint64_t h1[2] = {rA.hdr->head, rB.hdr->head};
    ctl->cmd = CMD_NONE;
    for (int c = 0; c < 2; ++c) {
      CHECK(wait_head(rr[c], h1[c] + 70000, 30) == 0, "cell %d: no data after the return", c);
      CHECK(!atomic_load(&rr[c]->hdr->away), "cell %d: away not cleared", c);
    }
  }
  printf("-- producer loss\n");
  ctl->stop = 1;
  waitpid(pp, NULL, 0); waited_p = 1;
  int st = -1;
  for (int i = 0; i < 1000; ++i) { if (waitpid(cp, &st, WNOHANG) == cp) break; usleep(10000); st = -1; }
  CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "channelizer did not exit cleanly after producer loss (status %d)", st);
  cp = -1;
  { /* the cell consumers' rule: stale heartbeat and the writer pid gone */
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "grep -q SHMIQ_CHAN_PRODUCER_LOST %s", logf);
    CHECK(system(cmd) == 0, "no SHMIQ_CHAN_PRODUCER_LOST in %s", logf);
    for (int c = 0; c < 2; ++c) {
      CHECK(kill(atomic_load(&rr[c]->hdr->producer_pid), 0) != 0 && errno == ESRCH, "cell %d: output writer pid still alive", c);
      CHECK(shm_iq_now_ns() - atomic_load(&rr[c]->hdr->heartbeat_ns) > 300000000ull, "cell %d: heartbeat not stale", c);
    }
  }
  printf("test_shm_iq_chan PASS\n");
  ok = 1;
fail:
  if (cp > 0) { kill(cp, SIGINT); waitpid(cp, NULL, 0); }
  if (!waited_p) { ctl->stop = 1; if (pp > 0) waitpid(pp, NULL, 0); }
  snprintf(ringA, sizeof(ringA), "/%s", ring);
  shm_unlink(ringA);
  unlink(plan);
  if (ok) unlink(logf);
  return ok ? 0 : 1;
}
