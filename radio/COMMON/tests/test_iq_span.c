/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: tests of the wideband capture planner (iq_span). Cells come from the inventory fixture
 * tests/passive_rx/mpci/inventory/live_tuned_20261023.json (DEIB live tunings, parsed with the receiver's own inventory parser) and from synthetic sets.
 * Devices are FIXTURES (labelled), never read from a host:
 *   x410_ddc   X410 with a DDC image, MCR 491.52 MHz: rates 491.52 MHz / n, n = 1..64; analog bandwidth 400 MHz [INFERRED, ZBX spec]
 *   x410_usrp  the same device as usrp_lib offers it to the planner: the device set intersected with the rates usrp_lib configures (no 491.52)
 *   uc200      MCR 245.76 MHz image: 245.76 / n
 *   b210_like  continuous 0.2 .. 61.44 MHz, analog 56 MHz (synthetic)
 * Cell sample rate: the smallest power of two >= 12 N_RB subcarriers x SCS (= get_samplerate_and_bw without 3/4 sampling for 51, 106, 162, 217, 273 PRB). */
#include "../iq_span.h"
#include "../../../common/utils/nr_cell_inventory.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-3)

static double cell_rate(int n_rb, double scs)
{
  int f = 1;
  while (f < n_rb * 12) f <<= 1;
  return f * scs;
}

static iq_span_dev_t dev_mcr_div(double mcr, int nmax, double analog)
{
  iq_span_dev_t d;
  memset(&d, 0, sizeof(d));
  for (int n = 1; n <= nmax && d.n_ranges < IQ_SPAN_MAX_RANGES; ++n) d.range[d.n_ranges++] = (iq_rate_range_t){mcr / n, mcr / n, 0};
  d.analog_bw_hz = analog;
  d.max_rx = 4;
  return d;
}
static iq_span_dev_t dev_x410_usrp(void)
{
  /* usrp_lib's configured rates (its sample-rate switch) that 491.52 MHz / n makes */
  static const double known[] = {245.76e6, 122.88e6, 61.44e6, 30.72e6, 15.36e6, 7.68e6, 1.92e6};
  iq_span_dev_t d;
  memset(&d, 0, sizeof(d));
  for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) d.range[d.n_ranges++] = (iq_rate_range_t){known[i], known[i], 0};
  d.analog_bw_hz = 400e6;
  d.max_rx = 4;
  return d;
}

static nr_cell_inventory_t g_inv;
static int load_fixture(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
  static char buf[1 << 16];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  char err[200];
  if (nr_cell_inventory_parse(buf, n, &g_inv, err, sizeof(err)) != 0) { fprintf(stderr, "fixture rejected: %s\n", err); return -1; }
  return 0;
}
static iq_span_cell_t inv_cell(const char *id)
{
  for (int i = 0; i < g_inv.n; ++i)
    if (!strcmp(g_inv.cell[i].id, id)) {
      const nr_cell_desc_t *d = &g_inv.cell[i].desc;
      return (iq_span_cell_t){.id = g_inv.cell[i].id, .centre_hz = d->centre_hz, .n_rb = d->n_rb, .scs_hz = d->scs_khz * 1e3,
                              .rate = cell_rate(d->n_rb, d->scs_khz * 1e3), .n_rx = 4};
    }
  fprintf(stderr, "no fixture cell %s\n", id);
  exit(1);
}
static void show(const iq_span_plan_t *p, const iq_span_cell_t *c)
{
  char line[1024];
  iq_span_format(p, c, line, sizeof(line));
  printf("  %s\n", line);
}

/* every served cell lies inside the usable band, its decimation is a power of two and rate = capture / decim */
static int invariants(const iq_span_dev_t *d, const iq_span_plan_t *p, const iq_span_cell_t *c, int n)
{
  CHECK(p->n_cells == n, "n_cells");
  int s = 0;
  for (int i = 0; i < n; ++i) {
    const iq_span_cell_plan_t *x = &p->cell[i];
    if (!x->served) { CHECK(x->reason != IQ_SPAN_SERVED, "unserved without a reason"); continue; }
    ++s;
    CHECK(x->decim >= 1 && x->decim <= IQ_SPAN_MAX_DECIM && !(x->decim & (x->decim - 1)), "decim %d", x->decim);
    CHECK(NEAR(p->rate / x->decim, c[i].rate), "rate");
    CHECK(NEAR(x->shift_hz, c[i].centre_hz - p->centre_hz), "shift");
    CHECK(fabs(x->shift_hz) + iq_span_cell_half_bw(&c[i]) <= 0.5 * p->usable_hz + 1e-3, "cell %d outside the usable band", i);
    CHECK(x->passband_hz > 0 && x->passband_hz < 0.5 * c[i].rate, "passband");
  }
  CHECK(s == p->n_served, "n_served");
  CHECK(iq_span_rate_supported(d, p->rate), "rate not in the device set");
  CHECK(!p->ok || p->cell[0].served, "cell 0 must be served when ok");
  return 0;
}

static int test_deib_pairs(void)
{
  const iq_span_dev_t devs[3] = {dev_mcr_div(491.52e6, 64, 400e6), dev_x410_usrp(), dev_mcr_div(245.76e6, 64, 400e6)};
  for (int k = 0; k < 3; ++k) {
    iq_span_plan_t p;
    /* Vodafone 415 (217 PRB at 3680.01 MHz) + Iliad 403 (51 PRB at 3630.00 MHz): span 3620.82 .. 3719.07 = 98.25 MHz <= 0.8 x 122.88 */
    iq_span_cell_t a[2] = {inv_cell("pci415"), inv_cell("pci403")};
    CHECK(iq_span_plan(&devs[k], a, 2, &p) == 2 && p.ok, "415+403 plan");
    if (invariants(&devs[k], &p, a, 2)) return 1;
    CHECK(NEAR(p.rate, 122.88e6), "415+403 rate %.0f", p.rate);
    CHECK(NEAR(p.centre_hz, 3669945000.0), "415+403 centre %.0f", p.centre_hz);
    CHECK(p.cell[0].decim == 1 && NEAR(p.cell[0].shift_hz, 10065000.0), "415 entry");
    CHECK(p.cell[1].decim == 4 && NEAR(p.cell[1].shift_hz, -39945000.0), "403 entry");
    CHECK(p.n_rx == 4, "n_rx");
    if (k == 1) show(&p, a);
    /* Vodafone 415 + TIM 923 (217 PRB at 3759.99 MHz): span 3640.95 .. 3799.05 = 158.1 MHz -> 245.76 MS/s */
    iq_span_cell_t b[2] = {inv_cell("pci415"), inv_cell("pci923")};
    CHECK(iq_span_plan(&devs[k], b, 2, &p) == 2 && p.ok, "415+923 plan");
    if (invariants(&devs[k], &p, b, 2)) return 1;
    CHECK(NEAR(p.rate, 245.76e6), "415+923 rate %.0f", p.rate);
    CHECK(NEAR(p.centre_hz, 3720000000.0), "415+923 centre %.0f", p.centre_hz);
    CHECK(p.cell[0].decim == 2 && NEAR(p.cell[0].shift_hz, -39990000.0), "415 entry");
    CHECK(p.cell[1].decim == 2 && NEAR(p.cell[1].shift_hz, 39990000.0), "923 entry");
    if (k == 1) show(&p, b);
  }
  return 0;
}

static int test_deib_all(void)
{
  /* every fixture cell, the started cell (415) first, then file order: greedy admission into one capture */
  static const char *ids[] = {"pci415", "pci923", "pci508", "pci403", "pci675", "pci4", "pci380"};
  iq_span_cell_t c[7];
  for (int i = 0; i < 7; ++i) c[i] = inv_cell(ids[i]);
  iq_span_plan_t p;
  /* through usrp_lib (max 245.76 MS/s): 415, 923, 403, 380 fit 3620.82 .. 3799.05 (178.2 MHz <= 196.6); 508 / PCI 4 (3560.85 ..) and 675 (3415.83 ..) do not */
  const iq_span_dev_t u = dev_x410_usrp();
  CHECK(iq_span_plan(&u, c, 7, &p) == 4, "usrp set: served %d", p.n_served);
  if (invariants(&u, &p, c, 7)) return 1;
  CHECK(NEAR(p.rate, 245.76e6) && p.cell[1].served && p.cell[3].served && p.cell[6].served, "usrp set: 923 403 380 served");
  CHECK(!p.cell[2].served && p.cell[2].reason == IQ_SPAN_OUT_OF_SPAN && !p.cell[4].served && !p.cell[5].served, "usrp set: 508 675 4 unserved");
  CHECK(p.cell[6].decim == 2 && NEAR(p.cell[6].shift_hz, p.cell[0].shift_hz), "co-channel 380 = 415 entry");
  show(&p, c);
  /* the raw device set (491.52 MS/s present): 383.2 MHz <= min(0.8 x 491.52, 400) -> all 7 at 491.52 */
  const iq_span_dev_t r = dev_mcr_div(491.52e6, 64, 400e6);
  CHECK(iq_span_plan(&r, c, 7, &p) == 7 && NEAR(p.rate, 491.52e6), "raw set: %d at %.0f", p.n_served, p.rate);
  if (invariants(&r, &p, c, 7)) return 1;
  /* the analog bandwidth binds: 350 MHz < 383.2 MHz -> 675 out, the rest served */
  iq_span_dev_t r2 = r;
  r2.analog_bw_hz = 350e6;
  CHECK(iq_span_plan(&r2, c, 7, &p) == 6 && !p.cell[4].served && p.cell[4].reason == IQ_SPAN_OUT_OF_SPAN, "analog bound");
  if (invariants(&r2, &p, c, 7)) return 1;
  return 0;
}

static int test_synthetic(void)
{
  iq_span_plan_t p;
  const iq_span_dev_t x = dev_mcr_div(491.52e6, 64, 400e6);
  /* N = 1: the started cell alone is captured at its own rate, at its own centre (the channelizer entry is a memcpy) */
  iq_span_cell_t one[1] = {inv_cell("pci415")};
  CHECK(iq_span_plan(&x, one, 1, &p) == 1 && NEAR(p.rate, 122.88e6) && NEAR(p.centre_hz, 3680010000.0) && p.cell[0].decim == 1 && p.cell[0].shift_hz == 0, "N=1");
  /* a cell outside any capture (3300 MHz next to 415: 419 MHz > 400 MHz analog) */
  iq_span_cell_t far[2] = {inv_cell("pci415"), {.id = "far3300", .centre_hz = 3300e6, .n_rb = 51, .scs_hz = 30e3, .rate = 30.72e6, .n_rx = 4}};
  CHECK(iq_span_plan(&x, far, 2, &p) == 1 && !p.cell[1].served && p.cell[1].reason == IQ_SPAN_OUT_OF_SPAN, "far cell");
  CHECK(NEAR(p.centre_hz, 3680010000.0) && NEAR(p.rate, 122.88e6), "far cell: the capture stays the started cell's");
  if (invariants(&x, &p, far, 2)) return 1;
  /* 3/4 sampling family next to 2^k: 92.16 MS/s is not 122.88 x 2^-j */
  iq_span_cell_t fam[2] = {inv_cell("pci415"), {.id = "tq", .centre_hz = 3700e6, .n_rb = 106, .scs_hz = 30e3, .rate = 46.08e6, .n_rx = 4}};
  CHECK(iq_span_plan(&x, fam, 2, &p) == 1 && p.cell[1].reason == IQ_SPAN_RATE_FAMILY, "rate family: %s", iq_span_reason_name(p.cell[1].reason));
  /* device without 122.88: the pair goes to 245.76 */
  iq_span_dev_t only = {.n_ranges = 1, .range = {{245.76e6, 245.76e6, 0}}, .analog_bw_hz = 0, .max_rx = 4};
  iq_span_cell_t a[2] = {inv_cell("pci415"), inv_cell("pci403")};
  CHECK(iq_span_plan(&only, a, 2, &p) == 2 && NEAR(p.rate, 245.76e6) && p.cell[0].decim == 2 && p.cell[1].decim == 8, "only 245.76");
  /* device that makes no usable rate: the plan refuses (the caller keeps the direct radio path) */
  iq_span_dev_t none = {.n_ranges = 1, .range = {{100e6, 100e6, 0}}, .max_rx = 4};
  CHECK(iq_span_plan(&none, a, 2, &p) == 0 && !p.ok && p.cell[0].reason == IQ_SPAN_DEVICE_RATE, "no rate: %s", iq_span_reason_name(p.cell[0].reason));
  /* channel count: a 2-channel device cannot stream a 4-RX cell */
  iq_span_dev_t two = x;
  two.max_rx = 2;
  CHECK(iq_span_plan(&two, a, 2, &p) == 0 && p.cell[0].reason == IQ_SPAN_CHANNELS, "channels");
  /* continuous device (B210-like, synthetic): two 51-PRB cells 20 MHz apart -> 38.36 MHz span at 61.44 MS/s (continuous range, analog 56 MHz) */
  iq_span_dev_t b210 = {.n_ranges = 1, .range = {{0.2e6, 61.44e6, 0}}, .analog_bw_hz = 56e6, .max_rx = 2};
  iq_span_cell_t s[2] = {{.id = "s1", .centre_hz = 2.0e9, .n_rb = 51, .scs_hz = 30e3, .rate = 30.72e6, .n_rx = 2},
                         {.id = "s2", .centre_hz = 2.02e9, .n_rb = 51, .scs_hz = 30e3, .rate = 30.72e6, .n_rx = 1}};
  CHECK(iq_span_plan(&b210, s, 2, &p) == 2 && NEAR(p.rate, 61.44e6) && NEAR(p.centre_hz, 2.01e9) && p.cell[0].decim == 2 && p.n_rx == 2, "b210-like");
  if (invariants(&b210, &p, s, 2)) return 1;
  /* stepped range: 10 MHz steps from 20 MHz: 61.44 is not on the grid, 122.88 neither -> refuse */
  iq_span_dev_t step = {.n_ranges = 1, .range = {{20e6, 500e6, 10e6}}, .max_rx = 4};
  CHECK(!iq_span_rate_supported(&step, 61.44e6) && iq_span_rate_supported(&step, 120e6) && iq_span_plan(&step, s, 2, &p) == 0, "stepped range");
  /* bad input */
  iq_span_cell_t bad[1] = {{.id = "bad", .centre_hz = 0, .n_rb = 51, .scs_hz = 30e3, .rate = 30.72e6, .n_rx = 1}};
  CHECK(iq_span_plan(&x, bad, 1, &p) == 0 && p.cell[0].reason == IQ_SPAN_INVALID, "invalid");
  CHECK(iq_span_plan(NULL, bad, 1, &p) == -1 && iq_span_plan(&x, bad, 0, &p) == -1, "bad args");
  return 0;
}

static int test_fit_live(void)
{
  const iq_span_dev_t u = dev_x410_usrp();
  iq_span_cell_t a[2] = {inv_cell("pci415"), inv_cell("pci403")};
  iq_span_plan_t p;
  CHECK(iq_span_plan(&u, a, 2, &p) == 2, "plan");
  iq_span_cell_plan_t o;
  /* co-channel 380 (same centre as 415) fits the running 122.88 MS/s capture */
  const iq_span_cell_t c380 = inv_cell("pci380"), c923 = inv_cell("pci923"), c508 = inv_cell("pci508");
  CHECK(iq_span_fit(&u, &p, &c380, &o) == 1 && o.decim == 1 && NEAR(o.shift_hz, 10065000.0), "380 fits");
  /* 923 / 508 are outside the running capture: no radio retune at run time -> unserved */
  CHECK(iq_span_fit(&u, &p, &c923, &o) == 0 && o.reason == IQ_SPAN_OUT_OF_SPAN, "923 out");
  CHECK(iq_span_fit(&u, &p, &c508, &o) == 0 && o.reason == IQ_SPAN_OUT_OF_SPAN, "508 out");
  /* a cell whose rate is above the capture rate cannot be made by decimation */
  iq_span_cell_t wide = c380;
  wide.rate = 245.76e6;
  CHECK(iq_span_fit(&u, &p, &wide, &o) == 0 && o.reason == IQ_SPAN_RATE_FAMILY, "rate above capture");
  return 0;
}

int main(int argc, char **argv)
{
  const char *fx = argc > 1 ? argv[1] : "tests/passive_rx/mpci/inventory/live_tuned_20261023.json";
  if (load_fixture(fx)) return 1;
  int fail = 0;
  fail |= test_deib_pairs();
  fail |= test_deib_all();
  fail |= test_synthetic();
  fail |= test_fit_live();
  printf("%s\n", fail ? "FAILED" : "ALL OK");
  return fail;
}
