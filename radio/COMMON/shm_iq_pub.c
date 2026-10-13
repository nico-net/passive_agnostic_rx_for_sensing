/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1 producer: with ISAC_SHM_IQ_PUB=<ring name> set, the process that owns the radio (any openair0 device: USRP,
 * raw_iq replay, rfsimulator) copies every sample it reads into the shared ring, for shm_iq consumers (one per cell).
 * Unset (default): nothing is wrapped and the device behaves exactly as before.
 * ISAC_SHM_IQ_CAPACITY: ring length in samples per channel (default 2^23 = 68 ms at 122.88 MS/s). */
#define _GNU_SOURCE
#include "common_lib.h"
#include "shm_iq_ring.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* shortcut: one published device per process (the UE has one radio); make this per-device for multi-radio (M4) */
static shm_iq_ring_t g_ring;
static openair0_device_t *g_dev;
static int (*g_read)(openair0_device_t *, openair0_timestamp_t *, void **, int, int);
static int (*g_set_freq)(openair0_device_t *, openair0_config_t *);

static int pub_read(openair0_device_t *d, openair0_timestamp_t *t, void **b, int n, int c)
{
  int got = g_read(d, t, b, n, c);
  if (got > 0) shm_iq_ring_write(&g_ring, *t, b, got, (unsigned)c);
  return got;
}

static int pub_set_freq(openair0_device_t *d, openair0_config_t *cfg)
{
  int ret = g_set_freq(d, cfg);
  shm_iq_ring_new_epoch(&g_ring, cfg->rx_freq[0]);
  fprintf(stderr, "SHMIQ_PUB_RETUNE center_hz=%.3f epoch=%llu\n", cfg->rx_freq[0],
          (unsigned long long)atomic_load(&g_ring.hdr->rf_epoch));
  return ret;
}

void shm_iq_pub_maybe_wrap(openair0_device_t *d, openair0_config_t *cfg)
{
  const char *name = getenv("ISAC_SHM_IQ_PUB");
  if (!name || !*name || !d->trx_read_func) return;
  if (g_dev && g_dev != d) { /* a second RU would interleave its samples into the same ring */
    fprintf(stderr, "SHMIQ_PUB_SKIP second device not published\n");
    return;
  }
  if (g_ring.hdr) { /* device reload (nrue_ru_reinit): same ring, new run */
    shm_iq_ring_new_epoch(&g_ring, cfg->rx_freq[0]);
  } else {
    const char *cv = getenv("ISAC_SHM_IQ_CAPACITY");
    uint64_t cap = cv && *cv ? strtoull(cv, NULL, 10) : (1ull << 23);
    if (shm_iq_ring_create(&g_ring, name, (unsigned)cfg->rx_num_channels, cap, cfg->sample_rate, cfg->rx_freq[0]) != 0) {
      fprintf(stderr, "SHMIQ_PUB_FAIL ring=/%s: %s\n", name, strerror(errno));
      abort();
    }
  }
  g_dev = d;
  g_read = d->trx_read_func; d->trx_read_func = pub_read;
  if (d->trx_set_freq_func) { g_set_freq = d->trx_set_freq_func; d->trx_set_freq_func = pub_set_freq; }
  fprintf(stderr, "SHMIQ_PUB_READY ring=/%s channels=%d rate=%.0f center_hz=%.3f capacity=%llu bytes=%llu\n", name,
          cfg->rx_num_channels, cfg->sample_rate, cfg->rx_freq[0], (unsigned long long)g_ring.hdr->capacity,
          (unsigned long long)g_ring.map_bytes);
}
