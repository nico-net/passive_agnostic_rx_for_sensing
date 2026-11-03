/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
/* iq_chan_offline core on a synthetic 4-channel input: a tone at the cell centre + a tone outside the cell band. Checks the length, the in-band tone (power, frequency),
 * the out-of-band attenuation and bit-exactness against iq_ddc driven directly. */
#include "iq_chan_offline_core.h"
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)

#define NCH 4
#define RATE 245760000.0
#define DEC 2
#define N (65536 * 3 + 4096) /* a partial last block, a multiple of DEC */
#define RING_C 3720000000.0
#define CELL_C 3759990000.0
#define F_IN 1.5e6   /* above the cell centre */
#define F_OUT -95e6  /* stopband (> 83.8 MHz from the cell centre); aliases to F_OUT + cell rate after the decimation, where it is measured */

static double tone_power(const int16_t *x, uint64_t n, double f, double rate)
{
  double re = 0, im = 0;
  for (uint64_t i = 0; i < n; ++i) {
    const double p = -2 * M_PI * f * (double)i / rate, c = cos(p), s = sin(p);
    re += x[2 * i] * c - x[2 * i + 1] * s;
    im += x[2 * i] * s + x[2 * i + 1] * c;
  }
  return (re * re + im * im) / ((double)n * n);
}

int main(void)
{
  char in[] = "/tmp/icoXXXXXX", out[600], p[1100];
  CHECK(mkdtemp(in), "mkdtemp");
  snprintf(out, sizeof(out), "%s/out", in);
  CHECK(!mkdir(out, 0700), "mkdir");
  static int16_t x[NCH][2 * N];
  for (int a = 0; a < NCH; ++a) {
    for (int i = 0; i < N; ++i) {
      const double t = (double)i / RATE, ph1 = 2 * M_PI * ((CELL_C + F_IN) - RING_C) * t, ph2 = 2 * M_PI * (CELL_C + F_OUT - RING_C) * t;
      x[a][2 * i] = (int16_t)lrint(6000 * cos(ph1) + 6000 * cos(ph2) + 50 * (a + 1));
      x[a][2 * i + 1] = (int16_t)lrint(6000 * sin(ph1) + 6000 * sin(ph2));
    }
    snprintf(p, sizeof(p), "%s/rx%d.sc16", in, a);
    FILE *f = fopen(p, "wb");
    CHECK(f && fwrite(x[a], 4, N, f) == N, "write input");
    fclose(f);
  }
  snprintf(p, sizeof(p), "%s/capture_info.json", in);
  FILE *f = fopen(p, "w");
  fprintf(f, "{\"channels\": 4, \"samples_per_channel\": %d, \"sample_rate_hz\": 245760000, \"rf_channels\": [{\"channel\": 0, \"frequency_hz\": 3720000000}], \"first_sample_tick\": 1000000\n}\n", N);
  fclose(f);
  ico_stats_t st;
  CHECK(!ico_channelize(in, out, CELL_C, 217, 30000, DEC, &st), "channelize");
  CHECK(st.out_samples == N / DEC, "length %" PRIu64, st.out_samples);

  for (int a = 0; a < NCH; ++a) {
    static int16_t y[2 * N / DEC], ref[2 * N / DEC];
    snprintf(p, sizeof(p), "%s/rx%d.sc16", out, a);
    f = fopen(p, "rb");
    CHECK(f && fread(y, 4, N / DEC, f) == N / DEC, "read output");
    fclose(f);
    iq_ddc_t d;
    CHECK(!iq_ddc_init(&d, RATE, DEC, iq_ddc_cell_passband(217, 30000, RATE / DEC, 0), IQ_CHAN_DEFAULT_ATTEN_DB), "ddc");
    iq_ddc_set_shift(&d, CELL_C - RING_C);
    for (int o = 0; o < N; o += 65536) { /* same blocks as the tool; the DC offset part cannot be told apart, so compare everything */
      const int n = N - o < 65536 ? N - o : 65536;
      iq_ddc_process(&d, x[a] + 2 * o, n, ref + o);
    }
    iq_ddc_free(&d);
    CHECK(!memcmp(y, ref, sizeof(y)), "ch%d differs from iq_ddc", a);
    const uint64_t skip = 4096, n = N / DEC - skip; /* skip the filter start-up */
    const double pin = tone_power(y + 2 * skip, n, F_IN, RATE / DEC), pout = tone_power(y + 2 * skip, n, F_OUT + RATE / DEC, RATE / DEC);
    const double want = 6000.0 * 6000.0; /* |A|^2 of the tone */
    const double db = 10 * log10(pin / want), att = 10 * log10(want / (pout > 1e-9 ? pout : 1e-9));
    printf("ch%d in-band %.3f dB re input, out-of-band attenuation %.1f dB\n", a, db, att);
    CHECK(fabs(db) < 0.5, "in-band tone off by %.3f dB", db);
    CHECK(att >= 60, "out-of-band tone only %.1f dB down", att);
    double best = 0; int bi = 0; /* tone frequency: the in-band tone beats +-0.2 MHz neighbours */
    for (int k = -2; k <= 2; ++k) { const double pw = tone_power(y + 2 * skip, n, F_IN + k * 2e5, RATE / DEC); if (pw > best) { best = pw; bi = k; } }
    CHECK(bi == 0, "tone frequency off by %d x 200 kHz", bi);
  }
  snprintf(p, sizeof(p), "rm -rf %s", in);
  return system(p) ? 1 : 0;
}
