/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* FIX1102 C: the receiver's CPU channelizer applied offline to a wideband raw capture directory (rx<N>.sc16 + capture_info.json), written as a per-cell raw capture
 * directory of the same format. It drives iq_chan_backend_cpu exactly as iq_chan does for one entry (shift = cell centre - ring centre, design
 * iq_ddc_cell_passband(n_rb, scs, cell rate, 0), atten IQ_CHAN_DEFAULT_ATTEN_DB, block IQ_CHAN_DEFAULT_BLOCK), so the output equals the live cell ring. Header-only so
 * the test can include the core. */
#ifndef IQ_CHAN_OFFLINE_CORE_H
#define IQ_CHAN_OFFLINE_CORE_H
#include "iq_chan.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double rms[SHM_IQ_MAX_CH]; uint64_t clipped[SHM_IQ_MAX_CH]; uint64_t out_samples; } ico_stats_t;

static int ico_num(const char *txt, const char *key, double *v)
{
  const char *p = strstr(txt, key);
  if (!p || !(p = strchr(p, ':'))) return -1;
  *v = strtod(p + 1, NULL);
  return 0;
}

/* returns 0, or a negative step number on failure (message on stderr) */
static int ico_channelize(const char *in_dir, const char *out_dir, double cell_centre, int n_rb, double scs_hz, int decim, ico_stats_t *st)
{
  char path[1024], txt[8192];
  snprintf(path, sizeof(path), "%s/capture_info.json", in_dir);
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return -1; }
  txt[fread(txt, 1, sizeof(txt) - 1, f)] = 0;
  fclose(f);
  double nch_d, spc_d, rate, tick, ring_centre;
  if (ico_num(txt, "\"channels\"", &nch_d) || ico_num(txt, "\"samples_per_channel\"", &spc_d) || ico_num(txt, "\"sample_rate_hz\"", &rate) ||
      ico_num(txt, "\"first_sample_tick\"", &tick) || ico_num(txt, "\"frequency_hz\"", &ring_centre)) { fprintf(stderr, "capture_info.json: missing field\n"); return -2; }
  const unsigned nch = (unsigned)nch_d;
  const uint64_t spc = (uint64_t)spc_d, first_tick = (uint64_t)tick; /* exact below 2^53 */
  if (nch < 1 || nch > SHM_IQ_MAX_CH || decim < 1 || (int64_t)first_tick % decim) { fprintf(stderr, "bad channels/decim/first_tick\n"); return -3; }
  const double cell_rate = rate / decim;
  const int block = IQ_CHAN_DEFAULT_BLOCK;
  const iq_chan_geom_t g = {.in_rate = rate, .nch = nch, .block = block};
  const iq_chan_entry_t e = {.centre_hz = cell_centre, .decim = decim, .passband_hz = iq_ddc_cell_passband(n_rb, scs_hz, cell_rate, 0)};
  void *ctx = iq_chan_backend_cpu.create(&g);
  void *cell = ctx ? iq_chan_backend_cpu.prepare(ctx, &e) : NULL;
  if (!cell) { fprintf(stderr, "channelizer design failed\n"); return -4; }
  iq_chan_backend_cpu.install(ctx, 0, cell);
  iq_chan_backend_cpu.set_shift(ctx, 0, cell_centre - ring_centre);
  FILE *in[SHM_IQ_MAX_CH], *out[SHM_IQ_MAX_CH];
  int16_t *ib[SHM_IQ_MAX_CH], *ob[SHM_IQ_MAX_CH];
  const int16_t *inp[SHM_IQ_MAX_CH];
  int16_t *outp[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH] = {0};
  double acc[SHM_IQ_MAX_CH] = {0};
  for (unsigned a = 0; a < nch; ++a) {
    snprintf(path, sizeof(path), "%s/rx%u.sc16", in_dir, a);
    in[a] = fopen(path, "rb");
    snprintf(path, sizeof(path), "%s/rx%u.sc16", out_dir, a);
    out[a] = fopen(path, "wb");
    ib[a] = malloc((size_t)block * 4);
    ob[a] = malloc((size_t)block / decim * 4);
    if (!in[a] || !out[a] || !ib[a] || !ob[a]) { perror(path); return -5; }
    inp[a] = ib[a];
    outp[a] = ob[a];
    if (st) st->clipped[a] = 0;
  }
  snprintf(path, sizeof(path), "%s/timestamps.tsv", out_dir);
  FILE *ts = fopen(path, "w");
  if (!ts) { perror(path); return -5; }
  fprintf(ts, "offset_samples\tcount_samples\tfirst_sample_tick\n");
  uint64_t done = 0, outn = 0;
  int rc = 0, blocks = 0;
  while (done < spc && !rc) {
    uint64_t n = spc - done < (uint64_t)block ? spc - done : (uint64_t)block;
    n -= n % (uint64_t)decim; /* shortcut: a tail shorter than decim is dropped */
    if (!n) break;
    for (unsigned a = 0; a < nch; ++a)
      if (fread(ib[a], 4, n, in[a]) != n) { fprintf(stderr, "short read rx%u at %" PRIu64 "\n", a, done); rc = -6; }
    if (rc) break;
    if (iq_chan_backend_cpu.run(ctx, inp, (int)n, outp) < 0) { fprintf(stderr, "run failed\n"); rc = -7; break; }
    const uint64_t m = n / (uint64_t)decim;
    for (unsigned a = 0; a < nch; ++a) {
      if (fwrite(ob[a], 4, m, out[a]) != m) { perror("write"); rc = -8; }
      for (uint64_t i = 0; i < 2 * m; ++i) {
        acc[a] += (double)ob[a][i] * ob[a][i];
        if (ob[a][i] >= 32767 || ob[a][i] <= -32768) if (st) st->clipped[a]++;
      }
    }
    fprintf(ts, "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n", outn, m, first_tick / (uint64_t)decim + outn);
    outn += m; done += n; ++blocks;
  }
  if (st) { st->out_samples = outn; for (unsigned a = 0; a < nch; ++a) st->rms[a] = outn ? sqrt(acc[a] / (2.0 * (double)outn)) : 0; }
  fclose(ts);
  for (unsigned a = 0; a < nch; ++a) { fclose(in[a]); if (fclose(out[a])) rc = -8; free(ib[a]); free(ob[a]); }
  iq_chan_backend_cpu.release(ctx, iq_chan_backend_cpu.retire(ctx, 0));
  iq_chan_backend_cpu.destroy(ctx);
  if (rc) return rc;
  snprintf(path, sizeof(path), "%s/capture_info.json", out_dir);
  f = fopen(path, "w");
  if (!f) { perror(path); return -9; }
  fprintf(f, "{\n  \"format\": \"sc16_le\",\n  \"channels\": %u,\n  \"samples_per_channel\": %" PRIu64 ",\n  \"sample_rate_hz\": %.0f,\n  \"duration_s\": %g,\n", nch, outn, cell_rate,
          (double)outn / cell_rate);
  fprintf(f, "  \"clock_source\": \"internal\",\n  \"time_source\": \"internal\",\n  \"external_cell_synchronization\": false,\n  \"receiver_corrections_applied\": false,\n");
  fprintf(f, "  \"rf_channels\": [\n");
  for (unsigned a = 0; a < nch; ++a)
    fprintf(f, "    {\"channel\": %u, \"frequency_hz\": %.0f, \"rate_hz\": %.0f, \"gain_db\": 38, \"bandwidth_hz\": 400000000, \"antenna\": \"RX1\"}%s\n", a, cell_centre, cell_rate,
            a + 1 < nch ? "," : "");
  fprintf(f, "  ],\n  \"first_sample_tick\": %" PRIu64 ",\n  \"timestamp_tick_rate_hz\": %.0f,\n  \"blocks\": %d,\n  \"rx_metadata_errors\": 0,\n", first_tick / (uint64_t)decim, cell_rate, blocks);
  fprintf(f, "  \"timestamp_discontinuities\": 0,\n  \"timestamp_mode\": \"frozen_start_tick\",\n  \"status\": \"COMPLETE_CONTIGUOUS_RAW_IQ\",\n");
  fprintf(f, "  \"derived_from\": \"iq_chan_offline (receiver CPU channelizer) of a wideband capture\"\n}\n");
  return fclose(f) ? -9 : 0;
}
#endif
