/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1/M4 producer: with ISAC_SHM_IQ_PUB set, the process that owns the radio (any openair0 device: USRP, raw_iq
 * replay, rfsimulator) copies every sample it reads into shared rings, for shm_iq consumers (one per cell).
 * Unset (default): nothing is wrapped and the device behaves exactly as before.
 *   ISAC_SHM_IQ_PUB=name                 one ring with all channels (M1).
 *   ISAC_SHM_IQ_PUB=ringA:0-1,ringB:2+3  M4: one ring (window) per channel subset ('-' range, '+' list); ring channel k = radio channel subset[k].
 *   ISAC_SHM_IQ_PUB_FREQ=ringA=3598e6,ringB=3680e6  M4: per-ring LO (each ring's channels are tuned to it via the device set_freq,
 *                                         which tunes rx_freq[ch] per channel; a UE-initiated retune moves all by the same delta).
 *   ISAC_SHM_IQ_DWELL=ringA@3598e6:200,ringB@3680e6:100  M4: rings with the SAME channel subset time-share it; dwell in ms.
 *   ISAC_SHM_IQ_SETTLE_US (1000): samples after a dwell retune (counted in sample time from the first read after it) are discarded.
 *   ISAC_SHM_IQ_RADIO_ID, ISAC_SHM_IQ_TIME_SOURCE=free|pps: header v2 metadata. ISAC_SHM_IQ_CAPACITY: samples per channel (2^23). */
#define _GNU_SOURCE
#include "common_lib.h"
#include "shm_iq_ring.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  shm_iq_ring_t ring;
  char name[64];
  unsigned nch, ch[SHM_IQ_MAX_CH];
  double freq;        /* LO of this window, 0 = follow the device config */
  int dwell_ms;       /* > 0: member of the time-shared group */
  int away, pending_return;
} pub_ring_t;

static pub_ring_t g_r[SHM_IQ_MAX_CH];
static int g_nr, g_nd, g_cur; /* rings, dwell-group size, active dwell ring */
static int g_dwell_idx[SHM_IQ_MAX_CH];
static double g_chf[SHM_IQ_MAX_CH], g_delta, g_ue0, g_settle_ticks, g_rate, g_retune_ticks;
/* the dwell retune runs in the read thread, UE retunes (CFO / carrier) in other threads under the shared radio gate */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_managed, g_settle_pending;
static int64_t g_settle_end;
static uint64_t g_dwell_end;
static openair0_config_t *g_cfg;
static openair0_device_t *g_dev;
static int (*g_read)(openair0_device_t *, openair0_timestamp_t *, void **, int, int);
static int (*g_set_freq)(openair0_device_t *, openair0_config_t *);

static double ring_center(const pub_ring_t *p, const openair0_config_t *cfg)
{
  if (p->dwell_ms) return p->freq + g_delta; /* a window's centre is fixed, whichever window the LO is on */
  return g_chf[p->ch[0]] > 0 ? g_chf[p->ch[0]] + g_delta : cfg->rx_freq[p->ch[0]];
}

/* device set_freq with the per-channel LO map applied (usrp_set_rx_freq_all already tunes cfg->rx_freq[i] per channel).
 * On a copy: cfg->rx_freq[] must stay the UE's own carrier, the reference g_delta is measured against. */
static int apply_set_freq(openair0_device_t *d, openair0_config_t *cfg)
{
  openair0_config_t t = *cfg;
  for (unsigned c = 0; c < SHM_IQ_MAX_CH; ++c)
    if (g_chf[c] > 0) t.rx_freq[c] = g_chf[c] + g_delta;
  return g_set_freq(d, &t);
}

static void dwell_switch(openair0_device_t *d)
{
  pthread_mutex_lock(&g_lock);
  const uint64_t now = shm_iq_now_ns();
  uint64_t cycle = 0;
  for (int k = 0; k < g_nd; ++k) cycle += (uint64_t)g_r[g_dwell_idx[k]].dwell_ms * 1000000ull;
  pub_ring_t *old = &g_r[g_dwell_idx[g_cur]];
  g_cur = (g_cur + 1) % g_nd;
  pub_ring_t *nw = &g_r[g_dwell_idx[g_cur]];
  shm_iq_ring_set_away(&old->ring, now + cycle - (uint64_t)old->dwell_ms * 1000000ull);
  old->away = 1;
  for (unsigned k = 0; k < nw->nch; ++k) g_chf[nw->ch[k]] = nw->freq;
  const int ret = apply_set_freq(d, g_cfg);
  g_settle_pending = 1; nw->pending_return = 1; nw->away = 0; /* header flag clears at its first write */
  const uint64_t done = shm_iq_now_ns();
  g_dwell_end = done + (uint64_t)nw->dwell_ms * 1000000ull;
  g_retune_ticks = (double)(done - now) * 1e-9 * g_rate; /* the radio kept streaming while the LO moved */
  pthread_mutex_unlock(&g_lock);
  fprintf(stderr, "SHMIQ_PUB_DWELL from=/%s to=/%s center_hz=%.3f retune_us=%.0f ret=%d\n", old->name, nw->name, nw->freq,
          (double)(done - now) * 1e-3, ret);
}

static int pub_read(openair0_device_t *d, openair0_timestamp_t *t, void **b, int n, int c)
{
  if (g_nd && shm_iq_now_ns() >= g_dwell_end) dwell_switch(d);
  int got = g_read(d, t, b, n, c);
  if (got <= 0) return got;
  const int64_t tick = (int64_t)*t;
  int64_t skip = 0;
  if (g_settle_pending) {
    /* the radio time at the retune command is >= this tick: discard the blocking retune plus the LO settle */
    g_settle_end = tick + (int64_t)(g_retune_ticks + g_settle_ticks);
    g_settle_pending = 0;
  }
  if (g_settle_end > tick) skip = g_settle_end - tick > got ? got : g_settle_end - tick;
  for (int i = 0; i < g_nr; ++i) {
    pub_ring_t *p = &g_r[i];
    if (p->away) continue;
    const int64_t sk = p->dwell_ms ? skip : 0; /* only the window that just returned is settling */
    if (sk >= got) continue;
    void *sub[SHM_IQ_MAX_CH];
    for (unsigned k = 0; k < p->nch; ++k) sub[k] = p->ch[k] < (unsigned)c ? (char *)b[p->ch[k]] + (size_t)sk * 4 : NULL; /* NULL: zeros */
    if (p->pending_return) {
      pthread_mutex_lock(&g_lock); /* centre with the current g_delta, not one a concurrent UE retune is replacing */
      shm_iq_ring_return(&p->ring, ring_center(p, g_cfg));
      pthread_mutex_unlock(&g_lock);
      p->pending_return = 0;
    }
    shm_iq_ring_write(&p->ring, tick + sk, sub, got - (int)sk, p->nch);
  }
  return got;
}

static int pub_set_freq(openair0_device_t *d, openair0_config_t *cfg)
{
  pthread_mutex_lock(&g_lock);
  g_cfg = cfg;
  if (g_managed) g_delta = cfg->rx_freq[0] - g_ue0; /* UE-initiated retune (CFO / carrier): same delta on every window */
  const int ret = g_managed ? apply_set_freq(d, cfg) : g_set_freq(d, cfg);
  for (int i = 0; i < g_nr; ++i) {
    if (g_r[i].away) continue;
    const double ctr = ring_center(&g_r[i], cfg);
    shm_iq_ring_new_epoch(&g_r[i].ring, ctr);
    fprintf(stderr, "SHMIQ_PUB_RETUNE center_hz=%.3f epoch=%llu\n", ctr, (unsigned long long)atomic_load(&g_r[i].ring.hdr->rf_epoch));
  }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static int ring_by_name(const char *n)
{
  for (int i = 0; i < g_nr; ++i) if (!strcmp(g_r[i].name, n)) return i;
  fprintf(stderr, "SHMIQ_PUB_FAIL unknown ring '%s' in ISAC_SHM_IQ_PUB_FREQ/DWELL\n", n);
  abort();
}

static void parse_subset(pub_ring_t *p, char *s, int nch)
{
  char *sp = NULL;
  for (char *tok = strtok_r(s, "+", &sp); tok; tok = strtok_r(NULL, "+", &sp)) {
    char *e;
    long a = strtol(tok, &e, 10), b = a;
    if (e != tok && *e == '-') b = strtol(e + 1, &e, 10);
    if (e == tok || *e || b < a) { fprintf(stderr, "SHMIQ_PUB_FAIL ring %s: bad channel list '%s'\n", p->name, tok); abort(); }
    for (long c = a; c <= b; ++c) {
      if (c < 0 || c >= nch || p->nch >= SHM_IQ_MAX_CH) { fprintf(stderr, "SHMIQ_PUB_FAIL ring %s channel %ld (radio has %d)\n", p->name, c, nch); abort(); }
      p->ch[p->nch++] = (unsigned)c;
    }
  }
}

static void parse_env(const char *spec, int nch)
{
  char *buf = strdup(spec), *sp = NULL;
  for (char *tok = strtok_r(buf, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
    pub_ring_t *p = &g_r[g_nr];
    if (g_nr >= (int)SHM_IQ_MAX_CH) { fprintf(stderr, "SHMIQ_PUB_FAIL too many rings\n"); abort(); }
    char *colon = strchr(tok, ':');
    if (colon) *colon = 0;
    snprintf(p->name, sizeof(p->name), "%s", tok);
    for (int i = 0; i < g_nr; ++i)
      if (!strcmp(g_r[i].name, p->name)) { fprintf(stderr, "SHMIQ_PUB_FAIL ring %s listed twice\n", p->name); abort(); }
    if (colon) parse_subset(p, colon + 1, nch);
    else for (int c = 0; c < nch; ++c) p->ch[p->nch++] = (unsigned)c;
    g_nr++;
  }
  free(buf);
}

static void parse_freq_dwell(void)
{
  const char *fv = getenv("ISAC_SHM_IQ_PUB_FREQ"), *dv = getenv("ISAC_SHM_IQ_DWELL");
  if (fv && *fv) {
    char *buf = strdup(fv), *sp = NULL;
    for (char *tok = strtok_r(buf, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
      char *eq = strchr(tok, '=');
      if (!eq) continue;
      *eq = 0;
      g_r[ring_by_name(tok)].freq = atof(eq + 1);
    }
    free(buf);
  }
  if (dv && *dv) {
    char *buf = strdup(dv), *sp = NULL;
    for (char *tok = strtok_r(buf, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
      char *at = strchr(tok, '@'), *co = at ? strchr(at, ':') : NULL;
      if (!co) { fprintf(stderr, "SHMIQ_PUB_FAIL ISAC_SHM_IQ_DWELL entry '%s' is not name@hz:ms\n", tok); abort(); }
      *at = 0; *co = 0;
      pub_ring_t *p = &g_r[ring_by_name(tok)];
      if (p->dwell_ms) { fprintf(stderr, "SHMIQ_PUB_FAIL ring %s listed twice in ISAC_SHM_IQ_DWELL\n", tok); abort(); }
      p->freq = atof(at + 1); p->dwell_ms = atoi(co + 1);
      if (p->freq <= 0 || p->dwell_ms <= 0) { fprintf(stderr, "SHMIQ_PUB_FAIL bad dwell entry for %s\n", tok); abort(); }
      g_dwell_idx[g_nd++] = (int)(p - g_r);
    }
    free(buf);
    for (int k = 1; k < g_nd; ++k) {
      const pub_ring_t *a = &g_r[g_dwell_idx[0]], *b = &g_r[g_dwell_idx[k]];
      if (a->nch != b->nch || memcmp(a->ch, b->ch, a->nch * sizeof(a->ch[0]))) {
        fprintf(stderr, "SHMIQ_PUB_FAIL dwell rings must share one channel subset\n");
        abort();
      }
    }
    if (g_nd == 1) { g_r[g_dwell_idx[0]].dwell_ms = 0; g_nd = 0; } /* nothing to alternate with: a static window at its freq */
  }
  /* rings sharing a radio channel must agree on its LO, and a dwell channel's LO moves under every ring that has it */
  for (int i = 0; i < g_nr; ++i)
    for (int j = i + 1; j < g_nr; ++j) {
      const pub_ring_t *a = &g_r[i], *b = &g_r[j];
      int shared = 0;
      for (unsigned x = 0; x < a->nch; ++x)
        for (unsigned y = 0; y < b->nch; ++y) shared |= a->ch[x] == b->ch[y];
      if (shared && ((a->dwell_ms > 0) != (b->dwell_ms > 0) || (!a->dwell_ms && a->freq != b->freq))) {
        fprintf(stderr, "SHMIQ_PUB_FAIL rings %s and %s share a channel but not its LO\n", a->name, b->name);
        abort();
      }
    }
}

void shm_iq_pub_maybe_wrap(openair0_device_t *d, openair0_config_t *cfg)
{
  const char *spec = getenv("ISAC_SHM_IQ_PUB");
  if (!spec || !*spec || !d->trx_read_func) return;
  if (g_dev && g_dev != d) { /* a second RU would interleave its samples into the same ring */
    fprintf(stderr, "SHMIQ_PUB_SKIP second device not published\n");
    return;
  }
  g_cfg = cfg;
  g_rate = cfg->sample_rate;
  const int reload = g_nr > 0;
  if (!reload) {
    parse_env(spec, cfg->rx_num_channels);
    parse_freq_dwell();
    const char *cv = getenv("ISAC_SHM_IQ_CAPACITY"), *sv = getenv("ISAC_SHM_IQ_SETTLE_US"), *rid = getenv("ISAC_SHM_IQ_RADIO_ID"),
               *tsv = getenv("ISAC_SHM_IQ_TIME_SOURCE");
    uint64_t cap = cv && *cv ? strtoull(cv, NULL, 10) : (1ull << 23);
    const double settle_us = sv && *sv ? atof(sv) : 1000.0;
    g_settle_ticks = settle_us * 1e-6 * cfg->sample_rate;
    for (int k = 0; k < g_nd; ++k)
      if (g_r[g_dwell_idx[k]].dwell_ms * 1000.0 <= 2 * settle_us) { /* the settle discard would eat the dwell: never published */
        fprintf(stderr, "SHMIQ_PUB_FAIL dwell %d ms of %s is not > 2 x ISAC_SHM_IQ_SETTLE_US\n", g_r[g_dwell_idx[k]].dwell_ms, g_r[g_dwell_idx[k]].name);
        abort();
      }
    char idbuf[48];
    if (!rid || !*rid) {
      if (cfg->sdr_addrs && *cfg->sdr_addrs) snprintf(idbuf, sizeof(idbuf), "%s", cfg->sdr_addrs);
      else snprintf(idbuf, sizeof(idbuf), "pid%d", (int)getpid());
      rid = idbuf;
    }
    unsigned ts = cfg->time_source == internal ? SHM_IQ_TS_FREE : (cfg->time_source == external || cfg->time_source == gpsdo) ? SHM_IQ_TS_PPS : SHM_IQ_TS_UNKNOWN;
    if (tsv && *tsv) ts = !strcmp(tsv, "pps") ? SHM_IQ_TS_PPS : !strcmp(tsv, "free") ? SHM_IQ_TS_FREE : SHM_IQ_TS_UNKNOWN;
    for (int i = 0; i < g_nr; ++i) {
      pub_ring_t *p = &g_r[i];
      if (shm_iq_ring_create(&p->ring, p->name, p->nch, cap, cfg->sample_rate, p->freq > 0 ? p->freq : cfg->rx_freq[p->ch[0]]) != 0) {
        fprintf(stderr, "SHMIQ_PUB_FAIL ring=/%s: %s\n", p->name, strerror(errno));
        abort();
      }
      shm_iq_ring_set_meta(&p->ring, rid, ts);
      if (p->freq > 0 && !p->dwell_ms) for (unsigned k = 0; k < p->nch; ++k) g_chf[p->ch[k]] = p->freq;
    }
    if (g_nd) {
      pub_ring_t *a = &g_r[g_dwell_idx[0]];
      for (unsigned k = 0; k < a->nch; ++k) g_chf[a->ch[k]] = a->freq;
      uint64_t cycle = 0;
      for (int k = 0; k < g_nd; ++k) cycle += (uint64_t)g_r[g_dwell_idx[k]].dwell_ms * 1000000ull;
      const uint64_t now = shm_iq_now_ns();
      for (int k = 1; k < g_nd; ++k) { shm_iq_ring_set_away(&g_r[g_dwell_idx[k]].ring, now + cycle); g_r[g_dwell_idx[k]].away = 1; }
      g_dwell_end = now + (uint64_t)a->dwell_ms * 1000000ull;
    }
    for (unsigned c = 0; c < SHM_IQ_MAX_CH; ++c) g_managed |= g_chf[c] > 0;
    const char *map = getenv("ISAC_RX_CHAN_MAP");
    if (g_managed && map && *map) { /* rx_freq[i] tunes UHD channel i, the subsets name OAI antennas: they would disagree */
      fprintf(stderr, "SHMIQ_PUB_FAIL per-ring LO (ISAC_SHM_IQ_PUB_FREQ/DWELL) with ISAC_RX_CHAN_MAP: give subsets in UHD order and unset the map\n");
      abort();
    }
    g_ue0 = cfg->rx_freq[0];
  } else { /* device reload: g_ue0/g_delta stay (cfg holds the UE carrier); the new device clock restarts the tick count */
    for (int i = 0; i < g_nr; ++i) if (!g_r[i].away) shm_iq_ring_new_epoch(&g_r[i].ring, ring_center(&g_r[i], cfg));
    if (g_nd) g_dwell_end = shm_iq_now_ns() + (uint64_t)g_r[g_dwell_idx[g_cur]].dwell_ms * 1000000ull;
    g_settle_end = INT64_MIN; g_settle_pending = 0;
  }
  g_dev = d;
  g_read = d->trx_read_func; d->trx_read_func = pub_read;
  if (d->trx_set_freq_func) { g_set_freq = d->trx_set_freq_func; d->trx_set_freq_func = pub_set_freq; }
  for (int i = 0; i < g_nr; ++i)
    fprintf(stderr, "SHMIQ_PUB_READY ring=/%s channels=%d rate=%.0f center_hz=%.3f capacity=%llu bytes=%llu radio=%s subset_n=%u dwell_ms=%d\n",
            g_r[i].name, (int)g_r[i].nch, cfg->sample_rate, ring_center(&g_r[i], cfg), (unsigned long long)g_r[i].ring.hdr->capacity,
            (unsigned long long)g_r[i].ring.map_bytes, g_r[i].ring.hdr->radio_id, g_r[i].nch, g_r[i].dwell_ms);
  if (g_managed && g_set_freq) apply_set_freq(d, cfg); /* put each channel subset on its LO now (default off: g_managed == 0) */
}
