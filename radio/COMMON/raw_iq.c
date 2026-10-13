/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "common_lib.h"
#include "assertions.h"
#include "raw_iq_source.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
  raw_iq_source source;
  pthread_mutex_t mutex;
  double speed;
  double gain[8];
  /* ISAC_RAW_IQ_DECIM=D (default 1, replay only): serve a recording made at rate D*fs at rate fs (windowed-sinc LPF + decimate),
   * so a 122.88 MS/s capture can drive -r 162/133/106/51 (61.44/30.72 MS/s). */
  unsigned decim;
  float *taps; int ntaps;
  int16_t *hist[8], *scr[8]; int scr_cap;
} replay_state;

#define DEC_TAPS 127
static void decim_init(replay_state *s)
{
  s->ntaps = DEC_TAPS; s->taps = malloc(sizeof(float) * DEC_TAPS);
  double sum = 0; const double fc = 0.45 / s->decim; /* cycles/input sample: passband edge 0.9 of the output Nyquist */
  for (int i = 0; i < DEC_TAPS; ++i) {
    double x = i - (DEC_TAPS - 1) / 2.0;
    double h = (x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x)) * (0.42 - 0.5 * cos(2 * M_PI * i / (DEC_TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (DEC_TAPS - 1)));
    s->taps[i] = (float)h; sum += h;
  }
  for (int i = 0; i < DEC_TAPS; ++i) s->taps[i] /= (float)sum;
  for (unsigned c = 0; c < 8; ++c) s->hist[c] = calloc(2 * DEC_TAPS, sizeof(int16_t));
}

static uint64_t integer_env(const char *name)
{
  const char *v = getenv(name);
  char *end = NULL;
  errno = 0;
  unsigned long long n = v ? strtoull(v, &end, 10) : 0;
  AssertFatal(v && *v && *v != '-' && !errno && end && !*end,
              "RAW_IQ_VOID missing/invalid %s\n", name);
  return n;
}

static int idle(openair0_device_t *d) { (void)d; return 0; }
static void finish(openair0_device_t *d)
{
  replay_state *s = d->priv;
  if (!s) return;
  fprintf(stderr, "RAW_IQ_END consumed=%" PRIu64 " clipped=%" PRIu64 "\n",
          s->source.position, s->source.clipped);
  raw_iq_close(&s->source);
  pthread_mutex_destroy(&s->mutex);
  free(s); d->priv = NULL;
}

static int tune(openair0_device_t *d, openair0_config_t *cfg)
{
  replay_state *s = d->priv;
  AssertFatal(cfg->sample_rate * s->decim == s->source.rate && cfg->rx_num_channels == s->source.channels,
              "RAW_IQ_VOID requested sample geometry differs from recording\n");
  for (unsigned c = 1; c < s->source.channels; ++c)
    AssertFatal(cfg->rx_freq[c] == cfg->rx_freq[0], "RAW_IQ_UNSUPPORTED separate channel tuning\n");
  pthread_mutex_lock(&s->mutex);
  int ret = raw_iq_tune(&s->source, cfg->rx_freq[0]);
  pthread_mutex_unlock(&s->mutex);
  AssertFatal(ret == 0, "RAW_IQ_UNSUPPORTED tuning outside stored Nyquist interval\n");
  fprintf(stderr, "RAW_IQ_DIGITAL_TUNE center_hz=%.3f requested_hz=%.3f\n",
          s->source.center, cfg->rx_freq[0]);
  return 0;
}

static int gain(openair0_device_t *d, openair0_config_t *cfg)
{
  replay_state *s = d->priv;
  for (unsigned c = 0; c < s->source.channels; ++c)
    AssertFatal(cfg->rx_gain[c] == s->gain[c], "RAW_IQ_UNSUPPORTED analog gain change on saved IQ\n");
  return 0;
}

static int no_tx(openair0_device_t *d, openair0_timestamp_t t, void **b, int n, int c, int f)
{
  (void)d; (void)t; (void)b; (void)n; (void)c; (void)f;
  AssertFatal(false, "RAW_IQ_VOID transmit attempted by passive replay\n");
  return -1;
}

static int read_iq(openair0_device_t *d, openair0_timestamp_t *t, void **b, int n, int c)
{
  replay_state *s = d->priv;
  AssertFatal(n > 0 && c == s->source.channels, "RAW_IQ_VOID invalid read geometry\n");
  const int D = (int)s->decim, m = n * D;
  pthread_mutex_lock(&s->mutex);
  if ((uint64_t)m > s->source.samples - s->source.position) {
    fprintf(stderr, "RAW_IQ_EOF consumed=%" PRIu64 " remaining=%" PRIu64
                    " clipped=%" PRIu64 " no_loop=1 no_padding=1\n",
            s->source.position, s->source.samples - s->source.position, s->source.clipped);
    pthread_mutex_unlock(&s->mutex);
    exit_function(__FILE__, __FUNCTION__, __LINE__, "raw IQ replay EOF", OAI_EXIT_NORMAL);
    /* Never return invented samples if the application's exit hook returns. */
    exit(0);
  }
  uint64_t ts;
  int got;
  if (D == 1) {
    got = raw_iq_read(&s->source, b, n, c, &ts);
  } else {
    if (m > s->scr_cap) {
      for (int k = 0; k < 8; ++k) { free(s->scr[k]); s->scr[k] = malloc((size_t)(m + DEC_TAPS) * 4); }
      s->scr_cap = m;
    }
    void *in[8];
    /* scratch = [DEC_TAPS-1 history samples][m new samples] */
    for (int k = 0; k < c; ++k) { in[k] = s->scr[k] + 2 * (DEC_TAPS - 1); memcpy(s->scr[k], s->hist[k], 4 * (DEC_TAPS - 1)); }
    got = raw_iq_read(&s->source, in, m, c, &ts) == m ? n : -1;
    ts /= D;
    for (int k = 0; k < c && got == n; ++k) {
      const int16_t *x = s->scr[k]; int16_t *y = b[k];
      for (int i = 0; i < n; ++i) {
        float re = 0, im = 0; const int16_t *p = x + 2 * (i * D);
        for (int j = 0; j < DEC_TAPS; ++j) { re += s->taps[j] * p[2 * (DEC_TAPS - 1 - j)]; im += s->taps[j] * p[2 * (DEC_TAPS - 1 - j) + 1]; }
        y[2 * i] = (int16_t)lrintf(fminf(fmaxf(re, -32768.f), 32767.f)); y[2 * i + 1] = (int16_t)lrintf(fminf(fmaxf(im, -32768.f), 32767.f));
      }
      memcpy(s->hist[k], x + 2 * m, 4 * (DEC_TAPS - 1));
    }
  }
  pthread_mutex_unlock(&s->mutex);
  AssertFatal(got == n, "RAW_IQ_VOID read failure\n");
  *t = ts;
  double delay = m / s->source.rate / s->speed;
  struct timespec wait = {.tv_sec = (time_t)delay,
                         .tv_nsec = (long)((delay - floor(delay)) * 1e9)};
  while (nanosleep(&wait, &wait) && errno == EINTR) {}
  return got;
}

/* device_init: replay device constructor (RAW_IQ): opens the saved IQ recording named by ISAC_RAW_IQ_* (directory, sample count, first tick, channels 1-8, rate, centre, shift), sets the pacing speed (ISAC_RAW_IQ_SPEED, 0 < speed <= 1), and installs the replay hooks (read_iq, tune, gain, finish).
 *   Optional single or double stream gaps (ISAC_RAW_IQ_GAP_AT_S/_S and _GAP2_*) are armed for replay only. Fails with an assertion on bad settings. Called by the openair0 device loader and the tests.
 */
int device_init(openair0_device_t *d, openair0_config_t *cfg)
{
  replay_state *s = calloc(1, sizeof(*s));
  AssertFatal(s, "RAW_IQ_VOID allocation\n");
  uint64_t channels = integer_env("ISAC_RAW_IQ_CHANNELS");
  uint64_t shift = integer_env("ISAC_RAW_IQ_SHIFT");
  AssertFatal(channels >= 1 && channels <= 8 && shift <= 15, "RAW_IQ_VOID invalid channel/shift\n");
  int ret = raw_iq_open(&s->source, getenv("ISAC_RAW_IQ_DIRECTORY"),
                       integer_env("ISAC_RAW_IQ_SAMPLES"), integer_env("ISAC_RAW_IQ_FIRST_TICK"),
                       channels, integer_env("ISAC_RAW_IQ_RATE"), integer_env("ISAC_RAW_IQ_CENTER"), shift);
  AssertFatal(ret == 0, "RAW_IQ_VOID cannot open exact saved IQ: %s\n", strerror(errno));
  char *end = NULL;
  const char *speed = getenv("ISAC_RAW_IQ_SPEED");
  s->speed = speed ? strtod(speed, &end) : 0;
  AssertFatal(speed && end && !*end && isfinite(s->speed) && s->speed > 0 && s->speed <= 1,
              "RAW_IQ_VOID invalid pacing speed\n");
  { const char *dv = getenv("ISAC_RAW_IQ_DECIM"); s->decim = dv ? (unsigned)atoi(dv) : 1;
    AssertFatal(s->decim >= 1 && s->decim <= 4 && channels <= 8, "RAW_IQ_VOID invalid ISAC_RAW_IQ_DECIM\n");
    if (s->decim > 1) decim_init(s); }
  pthread_mutex_init(&s->mutex, NULL);
  d->priv = s; d->openair0_cfg = cfg; d->type = NONE_DEV;
  for (unsigned c = 0; c < channels; ++c) {
    s->gain[c] = cfg->rx_gain[c]; cfg->rx_gain_offset[c] = 0;
  }
  cfg->tx_sample_advance = 0;
  d->trx_start_func = idle; d->trx_stop_func = idle;
  d->trx_get_stats_func = idle; d->trx_reset_stats_func = idle;
  d->trx_end_func = finish; d->trx_set_freq_func = tune;
  d->trx_set_gains_func = gain; d->trx_write_func = no_tx;
  d->trx_read_func = read_iq;
  tune(d, cfg);
  {
    /* ISAC_RAW_IQ_GAP_AT_S / ISAC_RAW_IQ_GAP_S: inject one stream discontinuity (seconds into
     * the recording, seconds skipped). Replay-only; there is no live-radio equivalent. */
    const char *gat = getenv("ISAC_RAW_IQ_GAP_AT_S"), *glen = getenv("ISAC_RAW_IQ_GAP_S");
    if (gat && glen && atof(gat) > 0 && atof(glen) > 0) {
      s->source.gap_at  = (uint64_t)(atof(gat)  * s->source.rate);
      s->source.gap_len = (uint64_t)(atof(glen) * s->source.rate);
      fprintf(stderr, "RAW_IQ_GAP armed at_s=%s len_s=%s\n", gat, glen);
    }
    const char *gat2 = getenv("ISAC_RAW_IQ_GAP2_AT_S"), *glen2 = getenv("ISAC_RAW_IQ_GAP2_S");
    if (gat2 && glen2 && atof(gat2) > 0 && atof(glen2) > 0) {
      s->source.gap2_at = (uint64_t)(atof(gat2) * s->source.rate);
      s->source.gap2_len = (uint64_t)(atof(glen2) * s->source.rate);
      fprintf(stderr, "RAW_IQ_GAP armed gap=2 at_s=%s len_s=%s\n", gat2, glen2);
    }
  }
  fprintf(stderr, "RAW_IQ_READY channels=%u samples=%" PRIu64 " first_tick=%" PRIu64
                  " rate=%.0f speed=%g shift=%u hardware_access=NONE memory_locking=disabled_for_offline_replay\n",
          s->source.channels, s->source.samples, s->source.first_tick,
          s->source.rate, s->speed, s->source.shift);
  return 0;
}
