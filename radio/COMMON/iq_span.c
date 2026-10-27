/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: wideband capture planner, see iq_span.h. Pure: no environment, no file, no clock. */
#include "iq_span.h"
#include "iq_ddc.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

double iq_span_cell_half_bw(const iq_span_cell_t *c) { return 0.5 * c->n_rb * 12.0 * c->scs_hz; }

int iq_span_rate_supported(const iq_span_dev_t *dev, double rate)
{
  if (!dev || !(rate > 0)) return 0;
  if (dev->n_ranges <= 0) return 1;
  for (int i = 0; i < dev->n_ranges && i < IQ_SPAN_MAX_RANGES; ++i) {
    const iq_rate_range_t *r = &dev->range[i];
    const double eps = 1e-6 * rate;
    if (rate < r->lo - eps || rate > r->hi + eps) continue;
    if (!(r->step > 0)) return 1;
    const double k = (rate - r->lo) / r->step;
    if (fabs(k - floor(k + 0.5)) < 1e-6) return 1;
  }
  return 0;
}

double iq_span_usable_hz(const iq_span_dev_t *dev, double rate)
{
  double u = IQ_SPAN_USABLE_FRAC * rate;
  if (dev && dev->analog_bw_hz > 0 && dev->analog_bw_hz < u) u = dev->analog_bw_hz;
  return u;
}

/* ratio = a / b as a power of two <= IQ_SPAN_MAX_DECIM, else 0 */
static int pow2_ratio(double a, double b)
{
  if (!(a > 0) || !(b > 0)) return 0;
  const double q = a / b;
  const long r = lround(q);
  if (r < 1 || r > IQ_SPAN_MAX_DECIM || (r & (r - 1)) || fabs(q - (double)r) > 1e-9 * q) return 0;
  return (int)r;
}

static int cell_valid(const iq_span_cell_t *c) { return c->centre_hz > 0 && c->n_rb > 0 && c->scs_hz > 0 && c->rate > 0 && iq_span_cell_half_bw(c) < 0.5 * c->rate; }

typedef struct {
  double rate, centre, usable, lo, hi;
} capture_t;

/* Can the set (cells[idx[0..m)]) be captured? Returns IQ_SPAN_SERVED and fills *cap, or the reason. */
static int feasible(const iq_span_dev_t *dev, const iq_span_cell_t *cells, const int *idx, int m, capture_t *cap)
{
  double lo = INFINITY, hi = -INFINITY, rmax = 0, rmin = INFINITY;
  for (int j = 0; j < m; ++j) {
    const iq_span_cell_t *c = &cells[idx[j]];
    const double h = iq_span_cell_half_bw(c);
    if (c->centre_hz - h < lo) lo = c->centre_hz - h;
    if (c->centre_hz + h > hi) hi = c->centre_hz + h;
    if (c->rate > rmax) rmax = c->rate;
    if (c->rate < rmin) rmin = c->rate;
  }
  const double centre = 0.5 * (lo + hi);
  int family = 0, supported = 0;
  for (int k = 0; (1 << k) <= IQ_SPAN_MAX_DECIM; ++k) {
    const double R = rmax * (double)(1 << k);
    if (!pow2_ratio(R, rmin)) break; /* R / rmin above the largest decimation (or not a power of two) */
    int fam_ok = 1;
    for (int j = 0; j < m && fam_ok; ++j) fam_ok = pow2_ratio(R, cells[idx[j]].rate) != 0;
    if (!fam_ok) { family = 1; break; } /* rmax / rate_j is not a power of two: no k can fix it */
    if (!iq_span_rate_supported(dev, R)) continue;
    supported = 1;
    const double usable = iq_span_usable_hz(dev, R);
    int fits = 1;
    for (int j = 0; j < m && fits; ++j) {
      const iq_span_cell_t *c = &cells[idx[j]];
      fits = fabs(c->centre_hz - centre) + iq_span_cell_half_bw(c) <= 0.5 * usable + 1e-6;
    }
    if (fits) {
      *cap = (capture_t){.rate = R, .centre = centre, .usable = usable, .lo = lo, .hi = hi};
      return IQ_SPAN_SERVED;
    }
  }
  if (!family) /* also check the family over the whole set when the loop stopped on rmin */
    for (int j = 0; j < m; ++j)
      if (!pow2_ratio(rmax, cells[idx[j]].rate) && !pow2_ratio(rmax * 2, cells[idx[j]].rate)) { family = 1; break; }
  return family ? IQ_SPAN_RATE_FAMILY : !supported ? IQ_SPAN_DEVICE_RATE : IQ_SPAN_OUT_OF_SPAN;
}

static void fill_cell(const iq_span_cell_t *c, double rate, double centre, iq_span_cell_plan_t *o)
{
  o->served = 1;
  o->reason = IQ_SPAN_SERVED;
  o->decim = pow2_ratio(rate, c->rate);
  o->shift_hz = c->centre_hz - centre;
  o->passband_hz = iq_ddc_cell_passband(c->n_rb, c->scs_hz, c->rate, 0);
}

int iq_span_plan(const iq_span_dev_t *dev, const iq_span_cell_t *cells, int n, iq_span_plan_t *out)
{
  if (!dev || !cells || !out || n < 1 || n > IQ_SPAN_MAX_CELLS) return -1;
  memset(out, 0, sizeof(*out));
  out->n_cells = n;
  int idx[IQ_SPAN_MAX_CELLS], m = 0;
  capture_t cap = {0};
  for (int i = 0; i < n; ++i) {
    const iq_span_cell_t *c = &cells[i];
    iq_span_cell_plan_t *o = &out->cell[i];
    const double h = cell_valid(c) ? iq_span_cell_half_bw(c) : 0;
    o->lo_hz = c->centre_hz - h;
    o->hi_hz = c->centre_hz + h;
    if (!cell_valid(c)) { o->reason = IQ_SPAN_INVALID; }
    else if (dev->max_rx && c->n_rx > dev->max_rx) { o->reason = IQ_SPAN_CHANNELS; }
    else {
      idx[m] = i;
      capture_t t;
      const int r = feasible(dev, cells, idx, m + 1, &t);
      if (r == IQ_SPAN_SERVED) { cap = t; ++m; o->served = 1; }
      else o->reason = r;
    }
    if (i == 0 && !o->served) return 0; /* the started cell cannot be captured: no plan, the direct path stays */
  }
  out->ok = 1;
  out->rate = cap.rate;
  out->centre_hz = cap.centre;
  out->usable_hz = cap.usable;
  out->lo_hz = cap.lo;
  out->hi_hz = cap.hi;
  for (int j = 0; j < m; ++j) {
    fill_cell(&cells[idx[j]], cap.rate, cap.centre, &out->cell[idx[j]]);
    if (cells[idx[j]].n_rx > out->n_rx) out->n_rx = cells[idx[j]].n_rx;
  }
  out->n_served = m;
  return m;
}

int iq_span_fit(const iq_span_dev_t *dev, const iq_span_plan_t *p, const iq_span_cell_t *c, iq_span_cell_plan_t *o)
{
  (void)dev;
  memset(o, 0, sizeof(*o));
  if (!p || !p->ok || !c || !cell_valid(c)) { o->reason = IQ_SPAN_INVALID; return 0; }
  const double h = iq_span_cell_half_bw(c);
  o->lo_hz = c->centre_hz - h;
  o->hi_hz = c->centre_hz + h;
  if (c->n_rx > p->n_rx) { o->reason = IQ_SPAN_CHANNELS; return 0; }
  if (!pow2_ratio(p->rate, c->rate)) { o->reason = IQ_SPAN_RATE_FAMILY; return 0; }
  if (fabs(c->centre_hz - p->centre_hz) + h > 0.5 * p->usable_hz + 1e-6) { o->reason = IQ_SPAN_OUT_OF_SPAN; return 0; }
  fill_cell(c, p->rate, p->centre_hz, o);
  return 1;
}

const char *iq_span_reason_name(int r)
{
  switch (r) {
    case IQ_SPAN_SERVED: return "served";
    case IQ_SPAN_OUT_OF_SPAN: return "out_of_span";
    case IQ_SPAN_RATE_FAMILY: return "rate_family";
    case IQ_SPAN_DEVICE_RATE: return "device_rate";
    case IQ_SPAN_CHANNELS: return "channels";
    default: return "invalid";
  }
}

int iq_span_format(const iq_span_plan_t *p, const iq_span_cell_t *cells, char *buf, size_t n)
{
  if (!buf || !n) return 0;
  size_t o = (size_t)snprintf(buf, n, "MPCI_SPAN ok=%d centre_hz=%.0f rate=%.0f usable_mhz=%.3f span_mhz=[%.3f..%.3f] n_rx=%u served=%d/%d cells=", p->ok,
                              p->centre_hz, p->rate, p->usable_hz / 1e6, p->lo_hz / 1e6, p->hi_hz / 1e6, p->n_rx, p->n_served, p->n_cells);
  int first = 1;
  for (int i = 0; i < p->n_cells && o < n; ++i)
    if (p->cell[i].served) {
      o += (size_t)snprintf(buf + o, n - o, "%s%s:d%d:%+.3f", first ? "" : ",", cells && cells[i].id ? cells[i].id : "?", p->cell[i].decim, p->cell[i].shift_hz / 1e6);
      first = 0;
    }
  if (first && o < n) o += (size_t)snprintf(buf + o, n - o, "-");
  if (o < n) o += (size_t)snprintf(buf + o, n - o, " unserved=");
  first = 1;
  for (int i = 0; i < p->n_cells && o < n; ++i)
    if (!p->cell[i].served) {
      o += (size_t)snprintf(buf + o, n - o, "%s%s:%s", first ? "" : ",", cells && cells[i].id ? cells[i].id : "?", iq_span_reason_name(p->cell[i].reason));
      first = 0;
    }
  if (first && o < n) o += (size_t)snprintf(buf + o, n - o, "-");
  return (int)(o < n ? o : n - 1);
}
