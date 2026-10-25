/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1/C1 consumer device "shm_iq" (libshm_iq.so, `device = { name = "shm_iq"; }`, loaded by name like raw_iq): a receiver reads an IQ ring and gets its
 * own cell's stream: digital shift to its -C and decimation to its -r (iq_ddc). The ring is found by name: first the in-process registry (an anonymous ring,
 * e.g. the narrow per-cell ring of the in-process channelizer iq_chan; needs the executable to export shm_iq_ring_* symbols so the module shares its registry),
 * then /dev/shm (a ring published by another process). On a ring that already is the cell's stream (ring rate = cell rate, ring centre = cell centre) the
 * device is a memcpy. No hardware access; transmit is refused (passive).
 * No environment variables, no options: the ring name is the device's sdr_addrs string (the existing config field), everything else is a constant.
 */
#define _GNU_SOURCE
#include "common_lib.h"
#include "assertions.h"
#include "iq_ddc.h"
#include "shm_iq_ring.h"
#include <errno.h>
#include <signal.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  shm_iq_ring_t ring;
  iq_ddc_t ddc[SHM_IQ_MAX_CH];
  unsigned channels;
  int decim;
  uint64_t pos, epoch, timeout_ns;
  double center;
  int16_t *raw[SHM_IQ_MAX_CH];
  int raw_cap;
  uint64_t reads, overruns, dropped, discont;
  int cons_id; /* slot in the ring's consumer table (-1: untracked ring) */
  char tag[32], ring_name[64];
  uint64_t radio_off;
  uint64_t radio_next_ns;
} shm_state;

#define ATTACH_S 30.0
#define TIMEOUT_MS 2000.0
#define ATTEN_DB 70.0

/* Half-width of the band the cell's filters must pass (Hz). The NR UE leaves cfg->rx_bw at 0 (only the RU/LTE paths set it), so the
 * old fallback "95 % of the sample rate" designed a 175-tap second stage for a 51-PRB cell at decim 4, too slow for the read thread
 * (31 overruns in 32 s on the c3610 replay). H3 rule (MPCI-DASH-REVIEW): the grid's occupied width num_rb_dl x 12 x SCS. The pure function is
 * iq_ddc_cell_passband() (iq_ddc.h), shared with the in-process channelizer. SCS: the config does not carry it for the passive UE
 * (nr_scs_for_raster = mu is 0 when unset), so the width is taken at max(30 kHz, 15 kHz << mu): a 15 kHz grid is over-covered, never under-covered. */
double shm_iq_cell_passband(const openair0_config_t *cfg)
{
  const double scs = 15e3 * (double)(1 << (cfg->nr_scs_for_raster > 0 && cfg->nr_scs_for_raster < 5 ? cfg->nr_scs_for_raster : 0));
  return iq_ddc_cell_passband(cfg->num_rb_dl, scs < 30e3 ? 30e3 : scs, cfg->sample_rate, cfg->rx_bw);
}

/* Radio facts of the publisher (shm_iq_ring.h side channel) into this cell's log, about once a second: CHPOW / RFSTALL as the publisher wrote
 * them, RFRING rewritten as this cell's RFCHAN line (its own tuning and rate, so the dashboard shows the cell's view, not the ring's). */
static void relay_radio(shm_state *s, const openair0_config_t *cfg)
{
  const uint64_t now = shm_iq_now_ns();
  if (now < s->radio_next_ns) return;
  s->radio_next_ns = now + 1000000000ull;
  char buf[4096];
  for (int it = 0; it < 20 && shm_iq_radio_poll(s->ring_name, &s->radio_off, buf, sizeof(buf)); ++it) { /* a late cell catches up on the history at once */
  char *save;
  for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
    int ch;
    char port[32];
    double gain, f, r, bw;
    if (sscanf(l, "RFRING ch%d port=%31s gain=%lf freq_hz=%lf rate=%lf bw_hz=%lf", &ch, port, &gain, &f, &r, &bw) == 6) {
      if (ch >= 0 && ch < cfg->rx_num_channels)
        fprintf(stderr, "[HW]   RFCHAN ch%d subdev=shm port=%s gain=%.2f range=[0.0..0.0] freq=%.6f MHz rate=%.6f Msps bw=%.6f MHz ring_freq=%.6f MHz\n", ch, port, gain,
                cfg->rx_freq[ch] / 1e6, cfg->sample_rate / 1e6, (bw < cfg->sample_rate ? bw : cfg->sample_rate) / 1e6, f / 1e6);
    } else fprintf(stderr, "[HW]   %s\n", l);
  }
  }
}

static void retune(shm_state *s, openair0_config_t *cfg)
{
  s->center = shm_iq_ring_center(&s->ring);
  const double shift = cfg->rx_freq[0] - s->center, rate = s->ring.hdr->rate;
  for (unsigned c = 1; c < s->channels; ++c)
    AssertFatal(cfg->rx_freq[c] == cfg->rx_freq[0], "SHMIQ separate channel tuning unsupported\n");
  const double half = shm_iq_cell_passband(cfg);
  if (fabs(shift) + half > 0.5 * rate)
    fprintf(stderr, "SHMIQ_WARN cell=%s band %.3f MHz +- %.3f MHz leaves the ring window %.3f MHz +- %.3f MHz\n", s->tag,
            cfg->rx_freq[0] / 1e6, half / 1e6, s->center / 1e6, 0.5 * rate / 1e6);
  for (unsigned c = 0; c < s->channels; ++c) iq_ddc_set_shift(&s->ddc[c], shift);
  fprintf(stderr, "SHMIQ_TUNE cell=%s rx_hz=%.3f ring_center_hz=%.3f shift_hz=%.3f decim=%d epoch=%" PRIu64 "\n", s->tag,
          cfg->rx_freq[0], s->center, shift, s->decim, s->epoch);
}

static int tune(openair0_device_t *d, openair0_config_t *cfg)
{
  retune(d->priv, cfg);
  return 0;
}

static int gain(openair0_device_t *d, openair0_config_t *cfg)
{
  shm_state *s = d->priv;
  (void)cfg;
  /* the analog gain belongs to the radio process shared by every cell */
  fprintf(stderr, "SHMIQ_GAIN_IGNORED cell=%s\n", s->tag);
  return 0;
}

static int idle(openair0_device_t *d) { (void)d; return 0; }

static int start(openair0_device_t *d)
{
  shm_state *s = d->priv;
  uint64_t h = atomic_load(&s->ring.hdr->head);
  s->pos = h - h % (uint64_t)s->decim; /* live: start at the newest data */
  fprintf(stderr, "SHMIQ_START cell=%s pos=%" PRIu64 "\n", s->tag, s->pos);
  return 0;
}

static void finish(openair0_device_t *d)
{
  shm_state *s = d->priv;
  if (!s) return;
  uint64_t clipped = 0;
  for (unsigned c = 0; c < s->channels; ++c) { clipped += s->ddc[c].clipped; iq_ddc_free(&s->ddc[c]); free(s->raw[c]); }
  fprintf(stderr, "SHMIQ_END cell=%s reads=%" PRIu64 " overruns=%" PRIu64 " dropped_in=%" PRIu64 " discont=%" PRIu64
                  " clipped=%" PRIu64 "\n", s->tag, s->reads, s->overruns, s->dropped, s->discont, clipped);
  shm_iq_ring_consumer_release(&s->ring, s->cons_id);
  shm_iq_ring_detach(&s->ring);
  free(s);
  d->priv = NULL;
}

static int no_tx(openair0_device_t *d, openair0_timestamp_t t, void **b, int n, int c, int f)
{
  (void)d; (void)t; (void)b; (void)n; (void)c; (void)f;
  AssertFatal(false, "SHMIQ transmit attempted by a passive consumer\n");
  return -1;
}

static void skip_to(shm_state *s, uint64_t to, int overrun)
{
  to += (uint64_t)(s->decim - (int)(to % (uint64_t)s->decim)) % (uint64_t)s->decim;
  s->dropped += to - s->pos;
  if (overrun) s->overruns++; else s->discont++;
  shm_iq_ring_consumer_overrun(&s->ring, s->cons_id, to - s->pos, overrun);
  fprintf(stderr, "SHMIQ_%s cell=%s skipped_in=%" PRIu64 " new_pos=%" PRIu64 "\n", overrun ? "OVERRUN" : "DISCONT", s->tag,
          to - s->pos, to);
  s->pos = to;
  for (unsigned c = 0; c < s->channels; ++c) iq_ddc_reset(&s->ddc[c]);
}

static int read_iq(openair0_device_t *d, openair0_timestamp_t *t, void **b, int n, int c)
{
  shm_state *s = d->priv;
  shm_iq_hdr_t *h = s->ring.hdr;
  AssertFatal(n > 0 && c == (int)s->channels, "SHMIQ invalid read geometry\n");
  const int m = n * s->decim;
  AssertFatal((uint64_t)m <= h->capacity / 2, "SHMIQ read of %d raw samples does not fit the ring (%" PRIu64 ")\n", m, h->capacity);
  if (m > s->raw_cap) {
    for (unsigned k = 0; k < s->channels; ++k) {
      free(s->raw[k]);
      s->raw[k] = malloc((size_t)m * 4);
      AssertFatal(s->raw[k], "SHMIQ allocation\n");
    }
    s->raw_cap = m;
  }
  relay_radio(s, d->openair0_cfg);
  for (;;) {
    uint64_t ep = atomic_load(&h->rf_epoch);
    if (ep != s->epoch) { s->epoch = ep; retune(s, d->openair0_cfg); }
    int r = shm_iq_ring_copy(&s->ring, s->pos, m, s->channels, s->raw);
    if (r == 0) break;
    if (r < 0) {
      uint64_t vf = atomic_load(&h->valid_from), head = atomic_load(&h->head);
      if (s->pos < vf) skip_to(s, vf, 0);
      else skip_to(s, head, 1); /* fell behind by more than the ring: jump to the newest data */
      continue;
    }
    /* the producer may stamp a heartbeat newer than our clock read: clamp, an unsigned wrap would read as ~584 years stale */
    const uint64_t hb = atomic_load(&h->heartbeat_ns), now = shm_iq_now_ns();
    const uint64_t stale = now > hb ? now - hb : 0;
    const pid_t pid = atomic_load(&h->producer_pid);
    /* a live producer may be stalled for tens of seconds (RFSTALL device reload, initial sync): keep waiting up to 60 s */
    /* time-shared window (v2): the producer is tuned elsewhere until dwell_until_ns; only then does the timeout start */
    const int away = h->version >= 2 && atomic_load(&h->away); /* before dwell_until: the producer stores them in the other order */
    const int away_wait = away && now < atomic_load(&h->dwell_until_ns) + s->timeout_ns;
    if (!away_wait && stale > s->timeout_ns && ((kill(pid, 0) != 0 && errno == ESRCH) || stale > 60000000000ull)) {
      fprintf(stderr, "SHMIQ_PRODUCER_LOST cell=%s pid=%d pos=%" PRIu64 "\n", s->tag, (int)atomic_load(&h->producer_pid), s->pos);
      exit_function(__FILE__, __FUNCTION__, __LINE__, "shm IQ producer lost", OAI_EXIT_NORMAL);
      exit(1);
    }
    relay_radio(s, d->openair0_cfg); /* a stalled publisher is exactly when RFSTALL lines matter; self-limited to 1 Hz */
    /* shortcut: 20 us polling instead of a futex; fine for a few consumers, upgrade if many cells share one core */
    struct timespec w = {0, 20000};
    nanosleep(&w, NULL);
  }
  for (unsigned k = 0; k < s->channels; ++k) iq_ddc_process(&s->ddc[k], s->raw[k], m, b[k]);
  *t = (openair0_timestamp_t)((uint64_t)h->base_tick + s->pos) / s->decim;
  s->pos += (uint64_t)m;
  shm_iq_ring_consumer_update(&s->ring, s->cons_id, s->pos);
  s->reads++;
  return n;
}

int device_init(openair0_device_t *d, openair0_config_t *cfg)
{
  shm_state *s = calloc(1, sizeof(*s));
  AssertFatal(s, "SHMIQ allocation\n");
  const char *name = cfg->sdr_addrs && *cfg->sdr_addrs ? cfg->sdr_addrs : "isac_iq";
  snprintf(s->tag, sizeof(s->tag), "pid%d", (int)getpid());
  const double attach_s = ATTACH_S;
  const uint64_t t0 = shm_iq_now_ns();
  /* wait for the producer's ring and its first sample (base_tick is set by the first write) */
  while (shm_iq_ring_attach(&s->ring, name) != 0 || atomic_load(&s->ring.hdr->head) == 0) {
    if (s->ring.hdr) shm_iq_ring_detach(&s->ring);
    AssertFatal(shm_iq_now_ns() - t0 < (uint64_t)(attach_s * 1e9), "SHMIQ no ring /%s after %.0f s\n", name, attach_s);
    usleep(10000);
  }
  shm_iq_hdr_t *h = s->ring.hdr;
  s->cons_id = shm_iq_ring_consumer_register(&s->ring);
  const double ratio = h->rate / cfg->sample_rate;
  s->decim = (int)lrint(ratio);
  AssertFatal(fabs(ratio - s->decim) < 1e-9 && s->decim >= 1 && s->decim <= 16 && !(s->decim & (s->decim - 1)),
              "SHMIQ ring rate %.0f / cell rate %.0f is not a power of two <= 16\n", h->rate, cfg->sample_rate);
  /* shortcut: a cell takes ring channels 0..N-1; add a channel map when cells need different antenna subsets */
  AssertFatal(cfg->rx_num_channels >= 1 && (unsigned)cfg->rx_num_channels <= h->channels,
              "SHMIQ cell wants %d channels, ring has %u\n", cfg->rx_num_channels, h->channels);
  s->channels = (unsigned)cfg->rx_num_channels;
  /* preserve the cell's channel bandwidth (or 95 % of its sample rate when the application did not say) */
  const double passband = shm_iq_cell_passband(cfg);
  const double atten = ATTEN_DB;
  for (unsigned c = 0; c < s->channels; ++c)
    AssertFatal(iq_ddc_init(&s->ddc[c], h->rate, s->decim, passband, atten) == 0, "SHMIQ DDC design failed (passband %.0f Hz)\n",
                passband);
  snprintf(s->ring_name, sizeof(s->ring_name), "%s", name);
  s->timeout_ns = (uint64_t)(TIMEOUT_MS * 1e6);
  s->epoch = atomic_load(&h->rf_epoch);
  d->priv = s; d->openair0_cfg = cfg; d->type = NONE_DEV;
  for (unsigned c = 0; c < s->channels; ++c) cfg->rx_gain_offset[c] = 0;
  cfg->tx_sample_advance = 0;
  d->trx_start_func = start; d->trx_stop_func = idle;
  d->trx_get_stats_func = idle; d->trx_reset_stats_func = idle;
  d->trx_end_func = finish; d->trx_set_freq_func = tune;
  d->trx_set_gains_func = gain; d->trx_write_func = no_tx;
  d->trx_read_func = read_iq;
  retune(s, cfg);
  fprintf(stderr, "SHMIQ_READY cell=%s ring=/%s channels=%u/%u ring_rate=%.0f decim=%d taps=%d,%d capacity=%" PRIu64
                  " producer_pid=%d radio=%s time_source=%u\n", s->tag, name, s->channels, h->channels, h->rate, s->decim, s->ddc[0].st[0].ntaps,
          s->ddc[0].st[1].ntaps, h->capacity, (int)atomic_load(&h->producer_pid),
          h->version >= 2 && h->radio_id[0] ? h->radio_id : "-", h->version >= 2 ? h->time_source : 0u);
  return 0;
}
