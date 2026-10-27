/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: wideband capture planner (pure). From the admitted cells (centre, grid, own sample rate) and what the radio reports it can stream
 * (its RX rate set and analog bandwidth), choose ONE capture: centre and rate, and for every cell its channelizer entry (decimation, shift,
 * passband) or the reason it cannot be served from this capture. Nothing here knows a host, a radio model or a cell:
 *   - span      = union of the cells' occupied bands (N_RB x 12 x SCS around each centre); centre = the middle of the span;
 *   - rates     = candidates R = r_max x 2^k (r_max = the largest cell rate), so every cell rate is R / 2^j with 2^j <= IQ_SPAN_MAX_DECIM (the
 *                 channelizer decimates by halfband stages); a candidate must be in the device's rate set; the smallest one that fits wins
 *                 (fewest bytes per second through memory);
 *   - fits      = every cell band lies inside the flat part of the capture: |cell centre - capture centre| + bw / 2 <= usable / 2, with
 *                 usable = min(IQ_SPAN_USABLE_FRAC x R, analog bandwidth when the device reports one);
 *   - cell 0 is the cell the receiver was started on: it is always served or the plan refuses (the caller keeps the direct radio path);
 *     the other cells are added in the order given while the capture can still hold them (greedy, admission order).
 * IQ_SPAN_USABLE_FRAC is a property of decimating radio front ends, not of a host: the standard NR 100 MHz carrier (273 PRB x 12 x 30 kHz =
 * 98.28 MHz) is streamed at 122.88 MS/s by every radio path of this receiver, i.e. a radio passes 0.8 of its rate flat. */
#ifndef IQ_SPAN_H
#define IQ_SPAN_H
#include <stddef.h>

#define IQ_SPAN_MAX_CELLS 16
#define IQ_SPAN_MAX_RANGES 64
#define IQ_SPAN_MAX_DECIM 16
#define IQ_SPAN_USABLE_FRAC 0.8

/* One interval of supported RX rates as the device reports it: lo..hi, step 0 = continuous, lo == hi = one discrete rate. */
typedef struct {
  double lo, hi, step;
} iq_rate_range_t;

typedef struct {
  iq_rate_range_t range[IQ_SPAN_MAX_RANGES];
  int n_ranges;        /* 0 = the device reports no rate set: every candidate is accepted (the device itself refuses a rate it cannot make) */
  double analog_bw_hz; /* RX analog bandwidth the device reports, 0 = unknown */
  unsigned max_rx;     /* RX channels the device streams, 0 = unknown */
} iq_span_dev_t;

typedef struct {
  const char *id;   /* for log lines; may be NULL */
  double centre_hz; /* the cell's grid centre (receiver -C) */
  int n_rb;         /* grid N_RB */
  double scs_hz;    /* subcarrier spacing */
  double rate;      /* the cell's own sample rate (what its UE instance expects: samples_per_subframe x 1000) */
  unsigned n_rx;    /* RX channels the cell uses */
} iq_span_cell_t;

typedef enum {
  IQ_SPAN_SERVED = 0,
  IQ_SPAN_OUT_OF_SPAN, /* the band does not fit the capture (too far from the others for any rate this device makes) */
  IQ_SPAN_RATE_FAMILY, /* its rate is not the capture rate / 2^j (e.g. 3/4 sampling next to a 2^k family) */
  IQ_SPAN_DEVICE_RATE, /* the rate it needs is not in the device's rate set */
  IQ_SPAN_CHANNELS,    /* more RX channels than the device streams */
  IQ_SPAN_INVALID,     /* missing centre / grid / rate */
} iq_span_reason_t;

typedef struct {
  int served;            /* 1 = has a channelizer entry */
  int reason;            /* iq_span_reason_t */
  int decim;             /* capture rate / cell rate (served) */
  double shift_hz;       /* cell centre - capture centre (served) */
  double passband_hz;    /* one-sided width its filters keep: iq_ddc_cell_passband(n_rb, scs, cell rate, 0) */
  double lo_hz, hi_hz;   /* its occupied band */
} iq_span_cell_plan_t;

typedef struct {
  int ok;                /* 0: not even cell 0 can be captured (the caller keeps the direct path) */
  double centre_hz, rate;/* the capture */
  double usable_hz;      /* flat width of the capture */
  double lo_hz, hi_hz;   /* span of the served cells */
  unsigned n_rx;         /* channels to stream: the most any served cell uses */
  int n_cells, n_served;
  iq_span_cell_plan_t cell[IQ_SPAN_MAX_CELLS];
} iq_span_plan_t;

/* Occupied half width of a cell (Hz): n_rb x 12 x scs / 2. */
double iq_span_cell_half_bw(const iq_span_cell_t *c);
/* 1 when `rate` is in the device's rate set (n_ranges 0: any rate). */
int iq_span_rate_supported(const iq_span_dev_t *dev, double rate);
/* Usable (flat) width of a capture at `rate` on `dev`. */
double iq_span_usable_hz(const iq_span_dev_t *dev, double rate);
/* The plan. Returns the number of served cells (0 when cell 0 cannot be captured, out->ok = 0); -1 on bad arguments. */
int iq_span_plan(const iq_span_dev_t *dev, const iq_span_cell_t *cells, int n, iq_span_plan_t *out);
/* Run time (no radio retune): can cell `c` be served from the capture `p` already running? Fills *o; returns 1 served / 0 not. */
int iq_span_fit(const iq_span_dev_t *dev, const iq_span_plan_t *p, const iq_span_cell_t *c, iq_span_cell_plan_t *o);
const char *iq_span_reason_name(int reason);
/* "MPCI_SPAN centre_hz=.. rate=.. usable_mhz=.. span_mhz=[lo..hi] n_rx=.. served=s/n cells=<id>:d<decim>:<shift MHz>,.. unserved=<id>:<reason>,.." */
int iq_span_format(const iq_span_plan_t *p, const iq_span_cell_t *cells, char *buf, size_t n);
#endif
