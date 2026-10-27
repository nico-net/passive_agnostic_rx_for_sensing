/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G5: GPU backend of the in-process channelizer (shm_iq_chan_gpu.cu) and the CHAN path selector (iq_chan_gpu_sel.c).
 *   test_iq_chan_gpu              unit cases (exit 77 without a CUDA device)
 *   test_iq_chan_gpu --discrete   the same with the platform forced to the DISCRETE tier (staged copies, path trial)
 *   test_iq_chan_gpu --bench S    idle bench: 1, 2, 4 cells at 245.76 MS/s x 4 RX, placements (a) rt and (b) group, S seconds each, GPU and CPU (1 thread)
 * Cases: (1) the old `shm_iq_chan --selftest --gpu` geometry, same input (2 cells, 2 ch, 245.76 MS/s, 8 x 65536): GPU vs iq_ddc, the old figure was
 * max 1 LSB / 129 of 786432 differing; (2) the self-check on decim 1 (shifted) / 2 / 4 / 8 / 16 x 4 ch at block sizes on and off the 4096 grid (6160,
 * 65536) and one below the filter history (1040); (3) the backend inside iq_chan (anonymous ring): bit-for-bit within 1 LSB of a fresh iq_ddc per
 * (cell, channel), live retune and a discontinuity (history reset); NULL output = paused slot not computed; (4) automatic selection: SHARED -> gpu,
 * DISCRETE -> trial verdict logged; (5) GPU ledger ("chan" bytes == api->bytes()) and budget (NR_GPU_MEM_CHAN dyn bytes, released at destroy);
 * (6) after the RX fence no GPU instance is created and iq_chan falls back to the CPU backend. */
#define _GNU_SOURCE
#include "iq_chan.h"
#include "iq_ddc.h"
#include "shm_iq_chan_gpu.h"
#include "nr_gpu_platform.h"
#include "nr_gpu_path.h"
#include "nr_gpu_membudget.h"
#include "nr_gpu_memstat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail = 1; return 1; } } while (0)

/* the old selftest's synthesis (shm_iq_chan.c synth): phase on a 1 kHz grid, exact per sample */
static void synth(int16_t *x, int n0, int n, double fs, const double *f, const double *amp, int nt)
{
  const int per = (int)lrint(fs / 1000.0);
  for (int k = 0; k < n; ++k) {
    double re = 0, im = 0;
    for (int t = 0; t < nt; ++t) {
      const long q = (long)lrint(f[t] / 1000.0);
      const double ph = 2 * M_PI * (double)(((q % per + per) % per) * (long)((n0 + k) % per) % per) / per;
      re += amp[t] * cos(ph); im += amp[t] * sin(ph);
    }
    x[2 * k] = (int16_t)lrint(re); x[2 * k + 1] = (int16_t)lrint(im);
  }
}

static const iq_chan_gpu_api_t *A;

static int test_old_figure(void)
{
  const double rate = 245.76e6, cen[2] = {-45e6, 70e6}, off[2][2] = {{-2e6, -3.5e6}, {2e6, -1e6}};
  const int N = 65536, NB = 8;
  const iq_chan_gpu_cell_t cells[2] = {{4, cen[0], 20e6}, {8, cen[1], 10e6}};
  int16_t *in[2];
  for (int a = 0; a < 2; ++a) {
    in[a] = malloc((size_t)N * NB * 4);
    const double f[2] = {cen[0] + off[0][a], cen[1] + off[1][a]}, am[2] = {3000, 3000};
    synth(in[a], 0, N * NB, rate, f, am, 2);
  }
  iq_chan_gpu_check_t r;
  CHECK(A->compare(2, rate, N, NB, cells, 2, (const int16_t *const *)in, &r) == 0, "compare ran");
  printf("OLD-SELFTEST geometry (2 cells decim 4/8 at -45/+70 MHz, 2 ch, 245.76 MS/s, 8 x 65536): max_abs_diff=%ld LSB differing=%ld/%ld (old figure: 1 LSB, 129/786432)\n",
         r.max_lsb, r.differing, r.total);
  CHECK(r.total == 786432, "total %ld", r.total);
  CHECK(r.max_lsb <= 1 && r.differing <= 129, "worse than the old figure");
  for (int a = 0; a < 2; ++a) free(in[a]);
  return 0;
}

static int test_selfcheck(int block)
{
  iq_chan_load_t l = {.in_rate = 245.76e6, .nch = 4, .n_cells = 5};
  const int dec[5] = {1, 2, 4, 8, 16};
  for (int k = 0; k < 5; ++k) { l.cell[k].decim = dec[k]; l.cell[k].shifted = 1; l.cell[k].passband_hz = 0.4 * l.in_rate / dec[k]; }
  iq_chan_gpu_check_t r;
  CHECK(iq_chan_gpu_selfcheck(&l, block, &r) == 0, "selfcheck ran");
  printf("SELFCHECK block=%d decim 1/2/4/8/16 x 4 ch: max_abs_diff=%ld LSB differing=%ld/%ld %s\n", block, r.max_lsb, r.differing, r.total, r.ok ? "PASS" : "FAIL");
  CHECK(r.ok, "selfcheck block %d", block);
  return 0;
}

/* through iq_chan: GPU backend named explicitly, compare every output ring with a fresh iq_ddc; retune + discontinuity on the way */
static int test_in_chan(void)
{
  const double rate = 122.88e6;
  const unsigned NCH = 4;
  const int block = 65536, nb = 12;
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 19, rate, 3.6e9, 2) == 0, "ring"); /* 8 blocks: slots are reused while the test runs */
  iq_chan_cfg_t cfg = {.in = &in, .block = block, .backend = A->backend};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  const int dec[3] = {1, 4, 2};
  const double sh[3] = {10.065e6, -39.945e6, 0};
  int id[3];
  shm_iq_ring_t v[3];
  for (int k = 0; k < 3; ++k) {
    iq_chan_entry_t e = {.shift_hz = sh[k], .decim = dec[k], .passband_hz = 0.4 * rate / dec[k]};
    snprintf(e.tag, sizeof e.tag, "c%d", k);
    CHECK((id[k] = iq_chan_cell_add(c, &e)) >= 0, "add");
    CHECK(shm_iq_ring_view(&v[k], iq_chan_cell_ring(c, id[k])) == 0, "view");
  }
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(!strcmp(st.backend, "gpu"), "backend %s", st.backend);
  /* input */
  int16_t *blk[NCH];
  for (unsigned a = 0; a < NCH; ++a) blk[a] = malloc((size_t)block * 4);
  uint32_t s = 1;
  iq_ddc_t ref[3][4];
  for (int k = 0; k < 3; ++k)
    for (unsigned a = 0; a < NCH; ++a) { iq_ddc_init(&ref[k][a], rate, dec[k], 0.4 * rate / dec[k], IQ_CHAN_DEFAULT_ATTEN_DB); iq_ddc_set_shift(&ref[k][a], sh[k]); }
  int16_t *want = malloc((size_t)block * 4), *got = malloc((size_t)block * 4);
  long maxd = 0, nd = 0, tot = 0;
  uint64_t rpos[3] = {0, 0, 0};
  iq_chan_seek(c, 0);
  for (int b = 0; b < nb; ++b) {
    for (unsigned a = 0; a < NCH; ++a)
      for (int i = 0; i < 2 * block; ++i) { s = s * 1664525u + 1013904223u; blk[a][i] = (int16_t)((int)(s >> 20) - 2048 + (int)(3000 * sin(0.001 * i + a))); }
    void *bufs[NCH];
    for (unsigned a = 0; a < NCH; ++a) bufs[a] = blk[a];
    shm_iq_ring_write(&in, 1000 + (int64_t)b * block, bufs, block, NCH);
    if (b == 6) { /* live retune of cell 1 (history kept, phase continuous) on both sides */
      CHECK(iq_chan_cell_retune(c, id[1], -30e6, 0) == 0, "retune");
      for (unsigned a = 0; a < NCH; ++a) { iq_ddc_set_shift(&ref[1][a], -30e6); iq_ddc_reset(&ref[1][a]); }
    }
    CHECK(iq_chan_step(c) == 1, "step %d", b);
    for (int k = 0; k < 3; ++k) {
      const int no = block / dec[k];
      for (unsigned a = 0; a < NCH; ++a) {
        iq_ddc_process(&ref[k][a], blk[a], block, want);
        int16_t *tmp[SHM_IQ_MAX_CH];
        for (unsigned q = 0; q < NCH; ++q) tmp[q] = malloc((size_t)no * 4);
        CHECK(shm_iq_ring_copy(&v[k], rpos[k], no, NCH, tmp) == 0, "copy cell %d", k);
        for (int i = 0; i < 2 * no; ++i) { const long d = labs((long)tmp[a][i] - want[i]); maxd = d > maxd ? d : maxd; nd += d != 0; tot++; }
        for (unsigned q = 0; q < NCH; ++q) free(tmp[q]);
      }
      rpos[k] += (uint64_t)no;
    }
  }
  printf("IN-CHAN gpu backend through iq_chan (3 cells: decim 1 +10.065 MHz, decim 4 -39.945 MHz (retuned to -30 MHz at block 6), decim 2 unshifted; 4 ch, 12 x 65536 at 122.88 MS/s): "
         "max_abs_diff=%ld LSB differing=%ld/%ld\n", maxd, nd, tot);
  CHECK(maxd <= 1 && nd * 1000 <= tot, "in-chan equality");
  for (int k = 0; k < 3; ++k) { shm_iq_ring_detach(&v[k]); for (unsigned a = 0; a < NCH; ++a) iq_ddc_free(&ref[k][a]); }
  free(want); free(got);
  for (unsigned a = 0; a < NCH; ++a) free(blk[a]);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  return 0;
}

/* a NULL output (paused slot) is not computed: its phase does not advance, so after "resume" (reset) it equals a fresh filter */
static int test_paused_slot(void)
{
  const iq_chan_geom_t g = {.in_rate = 122.88e6, .nch = 1, .block = 8192};
  void *x = A->backend->create(&g);
  CHECK(x, "create");
  iq_chan_entry_t e = {.decim = 2, .passband_hz = 20e6};
  void *cell = A->backend->prepare(x, &e);
  CHECK(cell, "prepare");
  A->backend->install(x, 0, cell);
  A->backend->set_shift(x, 0, 5e6);
  int16_t *in = calloc(8192, 4), *o = calloc(4096, 4);
  for (int i = 0; i < 2 * 8192; ++i) in[i] = (int16_t)(i * 7 % 2001 - 1000);
  int16_t *outs[IQ_CHAN_MAX_CELLS * SHM_IQ_MAX_CH] = {0};
  iq_chan_gpu_stats_t s0, s1;
  A->stats(x, &s0);
  CHECK(A->backend->run(x, (const int16_t *const *)&in, 8192, outs) == 0, "run with no output");
  A->stats(x, &s1);
  CHECK(s1.runs == s0.runs, "a block with only paused slots launches nothing");
  outs[0] = o;
  CHECK(A->backend->run(x, (const int16_t *const *)&in, 8192, outs) == 0, "run");
  iq_ddc_t d;
  iq_ddc_init(&d, 122.88e6, 2, 20e6, IQ_CHAN_DEFAULT_ATTEN_DB);
  iq_ddc_set_shift(&d, 5e6);
  int16_t *w = calloc(4096, 4);
  iq_ddc_process(&d, in, 8192, w);
  long maxd = 0;
  for (int i = 0; i < 2 * 4096; ++i) { const long q = labs((long)o[i] - w[i]); maxd = q > maxd ? q : maxd; }
  CHECK(maxd <= 1, "paused slot advanced its state (max %ld)", maxd);
  printf("PAUSED slot: no launch, no state change (first computed block == fresh iq_ddc, max %ld LSB)\n", maxd);
  iq_ddc_free(&d);
  A->backend->release(x, A->backend->retire(x, 0));
  A->backend->destroy(x);
  free(in); free(o); free(w);
  return 0;
}

static int test_ledger(void)
{
  iq_chan_load_t l = {.in_rate = 245.76e6, .nch = 4, .n_cells = 2};
  l.cell[0].decim = 2; l.cell[0].shifted = 1; l.cell[0].passband_hz = 0.4 * 122.88e6;
  l.cell[1].decim = 2; l.cell[1].shifted = 1; l.cell[1].passband_hz = 0.4 * 122.88e6;
  const uint64_t d0 = nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_DEV) + nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_PIN);
  const size_t dyn0 = nr_gpu_membudget_committed_total();
  const iq_chan_geom_t g = {.in_rate = l.in_rate, .nch = 4, .block = IQ_CHAN_DEFAULT_BLOCK};
  void *x = A->backend->create(&g);
  CHECK(x, "create");
  iq_chan_entry_t e = {.decim = 2, .passband_hz = 0.4 * 122.88e6};
  void *c0 = A->backend->prepare(x, &e), *c1 = A->backend->prepare(x, &e);
  CHECK(c0 && c1, "prepare");
  const uint64_t d1 = nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_DEV) + nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_PIN);
  nr_gpu_memstat_t ms;
  nr_gpu_memstat_get(NR_MS_CHAN, NR_MS_SHARED, 0, &ms);
  const size_t want = A->bytes(&l, 0), dyn = nr_gpu_membudget_committed_total() - dyn0;
  printf("LEDGER 2 cells decim 2 x 4 ch, block 65536: booked %llu B (chan dev %llu pin %llu), bytes() %zu, budget committed %zu (%s tier)\n", (unsigned long long)(d1 - d0),
         (unsigned long long)ms.cur[NR_MS_DEV], (unsigned long long)ms.cur[NR_MS_PIN], want, dyn, A->tier_shared() ? "shared" : "discrete");
  CHECK(d1 - d0 == want && dyn == want, "ledger / budget equal to bytes()");
  A->backend->release(x, c0); A->backend->release(x, c1); A->backend->destroy(x);
  const uint64_t d2 = nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_DEV) + nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_PIN);
  CHECK(d2 == d0 && nr_gpu_membudget_committed_total() == dyn0, "released at destroy");
  return 0;
}

static int test_select(int discrete)
{
  nr_gpu_path_test_reset();
  iq_chan_gpu_sel_test_reset();
  iq_chan_load_t l = {.in_rate = 245.76e6, .nch = 4, .n_cells = 2};
  for (int k = 0; k < 2; ++k) { l.cell[k].decim = 2; l.cell[k].shifted = 1; l.cell[k].passband_hz = 0.4 * 122.88e6; }
  const iq_chan_backend_t *b = iq_chan_backend_select(&l);
  const int st = nr_gpu_path_state(NR_GPU_PATH_CHAN);
  printf("SELECT %s tier: backend=%s path state=%d (%s)\n", discrete ? "discrete" : "shared", b->name, st, nr_gpu_path_test_last_line());
  if (!discrete) CHECK(!strcmp(b->name, "gpu") && st == NR_GPU_PATH_GPU, "SHARED -> gpu");
  else CHECK(st == NR_GPU_PATH_GPU || st == NR_GPU_PATH_CPU, "DISCRETE -> trial finished");
  CHECK(iq_chan_backend_select(&l) == b, "one decision per process");
  return 0;
}

/* core-pool leases (MPCI-CHANPOOL hooks): the GPU backend leases for its CPU side only, releases before the GPU wait, registers a demand of 1 */
static int g_held, g_held_max, g_acq, g_rel, g_dem = -1;
static int fk_acq(void *c, int b) { (void)c; (void)b; g_acq++; if (++g_held > g_held_max) g_held_max = g_held; return 0; }
static void fk_rel(void *c) { (void)c; g_rel++; g_held--; }
static void fk_dem(void *c, int t) { (void)c; g_dem = t; }
static int fk_max(void *c) { (void)c; return 2; }
static int test_lease(void)
{
  const iq_chan_lease_t L = {fk_acq, fk_rel, fk_dem, fk_max, NULL};
  iq_chan_set_lease(&L);
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, 2, 1 << 20, 122.88e6, 3.6e9, 1) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = 65536, .backend = A->backend};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t e = {.shift_hz = 5e6, .decim = 4, .passband_hz = 10e6};
  CHECK(iq_chan_cell_add(c, &e) >= 0, "add");
  iq_chan_seek(c, 0);
  int16_t *b[2];
  for (int a = 0; a < 2; ++a) b[a] = calloc(65536, 4);
  void *bufs[2] = {b[0], b[1]};
  for (int k = 0; k < 6; ++k) { shm_iq_ring_write(&in, 77 + (int64_t)k * 65536, bufs, 65536, 2); CHECK(iq_chan_step(c) == 1, "step"); }
  CHECK(g_acq >= 6 && g_acq == g_rel && g_held == 0 && g_held_max == 1 && g_dem == 1, "leases acq %d rel %d max %d demand %d", g_acq, g_rel, g_held_max, g_dem);
  printf("LEASE gpu backend: %d CHAN leases over 6 blocks, all released, at most 1 held, demand 1 (%s tier)\n", g_acq, A->tier_shared() ? "shared" : "discrete");
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  for (int a = 0; a < 2; ++a) free(b[a]);
  iq_chan_set_lease(NULL);
  return 0;
}

static int test_fence(void)
{
  nr_gpu_rx_started();
  const iq_chan_geom_t g = {.in_rate = 122.88e6, .nch = 2, .block = 65536};
  CHECK(!A->backend->create(&g), "no GPU instance after the RX fence");
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, 2, 1 << 20, 122.88e6, 3.6e9, 1) == 0, "ring");
  iq_chan_load_t l = {.in_rate = 122.88e6, .nch = 2, .n_cells = 1};
  l.cell[0].decim = 2;
  iq_chan_cfg_t cfg = {.in = &in, .load_hint = &l};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create falls back");
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(!strcmp(st.backend, "cpu"), "cpu after the fence (%s)", st.backend);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("FENCE after nr_gpu_rx_started: no GPU instance, iq_chan uses the cpu backend\n");
  return 0;
}

static int g_bblock = IQ_CHAN_DEFAULT_BLOCK;
static int bench(double secs)
{
  printf("BENCH 245.76 MS/s x 4 RX, block %d, free running, %.1f s per row\n", g_bblock, secs);
  printf("| cells (decim) | placement | SMs | x real time | GPU ms per ms of signal | wall ms per ms of signal | GPU time share | CPU 1 thread x real time |\n|---|---|---|---|---|---|---|---|\n");
  const int sets[3][4] = {{2, 0, 0, 0}, {2, 2, 0, 0}, {2, 2, 4, 8}};
  const int ncs[3] = {1, 2, 4};
  for (int si = 0; si < 3; ++si) {
    iq_chan_load_t l = {.in_rate = 245.76e6, .nch = 4, .n_cells = (unsigned)ncs[si]};
    char nm[64] = "";
    for (int k = 0; k < ncs[si]; ++k) {
      l.cell[k].decim = sets[si][k]; l.cell[k].shifted = 1;
      l.cell[k].passband_hz = iq_ddc_cell_passband(sets[si][k] == 2 ? 217 : sets[si][k] == 4 ? 106 : 51, 30e3, l.in_rate / sets[si][k], 0);
      snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "%s%d", k ? "+" : "", sets[si][k]);
    }
    iq_chan_bench_t cb;
    const int cok = iq_chan_cpu_bench(&l, g_bblock, secs, &cb) == 0;
    for (int p = IQ_CHAN_GPU_PLACE_RT; p <= IQ_CHAN_GPU_PLACE_GROUP; ++p) {
      A->set_placement(p);
      iq_chan_gpu_bench_t b;
      if (A->bench(&l, g_bblock, secs, &b)) { printf("| %d (%s) | %s | bench failed |\n", ncs[si], nm, p == 1 ? "(a) rt" : "(b) group"); continue; }
      const double sig_ms = 1e3 * g_bblock / l.in_rate;
      printf("| %d (%s) | %s | %d | %.2f | %.3f | %.3f | %.1f %% | %.3f |\n", ncs[si], nm, p == 1 ? "(a) rt" : "(b) group", b.sms, b.rt_factor, b.gpu_ms_per_block / sig_ms,
             b.wall_ms_per_block / sig_ms, 100.0 * b.gpu_ms_per_block / sig_ms, cok ? cb.rt_factor : -1);
    }
    A->set_placement(0);
  }
  return 0;
}

int main(int argc, char **argv)
{
  int discrete = 0;
  double bsecs = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--discrete")) discrete = 1;
    else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bsecs = atof(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) g_bblock = atoi(argv[++i]);
  }
  if (discrete) nr_gpu_platform_test_force_model(NR_GPU_DISCRETE);
  const nr_gpu_caps_t *caps = nr_gpu_platform_probe();
  A = iq_chan_gpu_load();
  if (!caps || caps->model == NR_GPU_NONE || !A || !A->available()) { printf("SKIP no CUDA device / module (%s)\n", A ? "module ok" : "no module"); return 77; }
  printf("device %s tier=%s\n", caps->name, A->tier_shared() ? "shared" : "discrete");
  if (bsecs > 0) return bench(bsecs);
  test_old_figure();
  test_selfcheck(65536);
  test_selfcheck(6160);
  test_selfcheck(1040);
  test_in_chan();
  test_paused_slot();
  test_ledger();
  test_lease();
  test_select(discrete);
  test_fence();
  printf("%s\n", g_fail ? "SOME FAILED" : "ALL OK");
  return g_fail;
}
