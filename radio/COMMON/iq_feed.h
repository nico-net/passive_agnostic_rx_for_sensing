/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: radio -> wideband ring -> in-process channelizer -> per-cell rings, inside the receiver process.
 * One reader thread pulls ONE wideband capture from the radio through a read callback (the receiver passes the radio device's trx_read_func behind its
 * shutdown gate; tests pass a fake radio) and publishes it, with its timestamps, into an anonymous wideband ring (shm_iq_ring, zero copy: the device
 * writes straight into the ring slots). The channelizer (iq_chan, CPU backend with k derived threads, or a registered GPU backend) turns it into one
 * narrow ring per cell, registered as "<name>_<tag>", which the cell's UE instance reads through the shm_iq consumer device (A8).
 * Rules (tested by test_iq_feed):
 *   - timestamps: the wideband ring index follows the device timestamps (a forward jump is a gap, a backward one a new run); a device that repeats the
 *     previous read's timestamp (frozen stamps, X410 34B2AF1: every packet carries the stream start time) is continued from the sample count, counted
 *     and logged once; narrow rings carry (wide tick) / decim, so each cell's timing loop sees its own continuous clock;
 *   - short reads publish what was read (the excess is not lost and nothing restarts); a read error stops the reader (the radio driver owns recovery);
 *   - per-cell accounting: the narrow ring's consumer table (overruns / dropped / lag of the cell's UE), the channelizer's overruns of the wide ring
 *     (shortfall of the channelizer: affects every cell), the radio side (reads, short reads, gaps, frozen);
 *   - cells are created before the receiver's RX fence (no allocation at run time); a cell is removed / re-admitted at run time by pausing / resuming
 *     its entry (iq_chan_cell_pause / _resume), without stopping the radio.
 * No environment variable, no option: rates, sizes and thread counts come from the caller's plan (iq_span, host-RAM plan, core plan). */
#ifndef IQ_FEED_H
#define IQ_FEED_H
#include "iq_chan.h"
#include <stdint.h>

/* The radio read: n samples per channel of `nch` channels into buf[c] (sc16), first-sample timestamp in *ts (radio ticks at the capture rate).
 * Returns the samples read (<= n, > 0), or < 0 on error / shutdown. */
typedef int (*iq_feed_read_fn)(void *ctx, int64_t *ts, void **buf, int n, int nch);

typedef struct {
  iq_feed_read_fn read;
  void *read_ctx;
  unsigned nch;                 /* channels of the capture */
  double rate, centre_hz;       /* the capture (iq_span plan, as the device granted it) */
  const char *name;             /* wideband ring registry name; cell rings are "<name>_<tag>" */
  uint64_t wide_capacity;       /* samples per channel (shm_iq_ring_plan_capacity of the host-RAM plan); >= 8 blocks */
  size_t narrow_budget_bytes;   /* all cell rings together (0 = unlimited) */
  int block;                    /* samples per channel per radio read, 0 = IQ_CHAN_DEFAULT_BLOCK clipped to an eighth of the wide ring; multiple of 16 */
  const iq_chan_backend_t *backend; /* NULL = iq_chan_backend_select(load_hint) */
  const iq_chan_load_t *load_hint;
  const char *radio_id;         /* for the ring metadata (may be NULL) */
} iq_feed_cfg_t;

typedef struct iq_feed iq_feed_t;

typedef struct {
  uint64_t reads, samples, short_reads, read_errors, ts_gaps, ts_frozen, staged;
  int frozen;                   /* frozen-stamp mode detected */
  int running;                  /* reader thread alive */
  iq_chan_stats_t chan;
} iq_feed_stats_t;

typedef struct {
  char tag[32];
  int paused;
  uint64_t overruns, dropped, lag; /* summed over the cell ring's consumers (the UE instance of the cell) */
  uint64_t head;                   /* samples written into the cell ring */
} iq_feed_cell_stats_t;

iq_feed_t *iq_feed_create(const iq_feed_cfg_t *cfg); /* NULL (errno) */
void iq_feed_destroy(iq_feed_t *f);                   /* stops everything; cell rings stay valid for their remaining views */
/* A cell entry (before the RX fence). Returns the entry id (>= 0) or -errno. paused = 1: created paused (e.g. outside the capture span). */
int iq_feed_cell_add(iq_feed_t *f, const char *tag, double centre_hz, int decim, double passband_hz, int paused);
int iq_feed_cell_pause(iq_feed_t *f, int id);
int iq_feed_cell_resume(iq_feed_t *f, int id);
shm_iq_ring_t *iq_feed_cell_ring(iq_feed_t *f, int id);
shm_iq_ring_t *iq_feed_wide_ring(iq_feed_t *f);
iq_chan_t *iq_feed_chan(iq_feed_t *f);
/* One radio read into the wideband ring on the calling thread (the reader thread's body; tests). 1 = samples published, -1 = read error. */
int iq_feed_step(iq_feed_t *f);
/* Start the reader thread (pinned to reader_cpu >= 0, SCHED_FIFO prio when > 0 and allowed) and the channelizer (iq_chan_start on chan_cpus: k derived). */
int iq_feed_start(iq_feed_t *f, int reader_cpu, int reader_prio, const int *chan_cpus, int n_chan_cpus);
/* Stop the reader (it leaves at its next read: the caller stops the radio first, so a blocked read returns) and the channelizer. */
void iq_feed_stop(iq_feed_t *f);
void iq_feed_stats(const iq_feed_t *f, iq_feed_stats_t *st);
int iq_feed_cell_stats(iq_feed_t *f, int id, iq_feed_cell_stats_t *st); /* 0, or -1 no entry */
/* "MPCI_FEED rate=.. centre_hz=.. reads=.. short=.. gaps=.. frozen=.. read_err=.. chan_overruns=.. chan_lag_max=.. threads=k/need cells=<tag>:ov=..,drop=..,lag=..[,paused];.." */
int iq_feed_format(iq_feed_t *f, char *buf, size_t n);
/* MPCI-DASH P3 CHAN_CELL: one line per cell ref, '\n'-separated, written to buf (NUL terminated), the bytes written returned: "SENSING: CHAN_CELL cell=<k> pci=<pci> ring=<tag> win_ms=<ms>
 * blocks=<D> out_samples=<D> write_us_mean=<> write_us_max=<window> run_us_attrib=<D> lag_now=<samples> overruns_seen=<D> paused=<0|1>". run_us_attrib is the channelizer's
 * backend run() time of the window split over the cells by their output samples (one backend call serves all cells): an attribution, not a measurement per cell. With fewer than two refs
 * (one cell) nothing is written and 0 is returned; the first call only opens the windows (0). The caller passes the entry id, the cell index and the PCI (-1 unknown) of each cell. */
typedef struct {
  int id, cell, pci;
} iq_feed_cell_ref_t;
int iq_feed_format_chan_cells(iq_feed_t *f, uint64_t now_ns, const iq_feed_cell_ref_t *refs, int nrefs, char *buf, size_t cap);

/* Which input path a receiver with `n_cells` admitted cells takes (pure; the N = 1 invariant: one cell is always the direct radio path). */
typedef enum {
  IQ_FEED_DIRECT = 0,   /* one cell: the radio device is the cell's RU, no channelizer hop (byte-identical to the single-cell receiver) */
  IQ_FEED_RINGS,        /* several cells, published rings (configured device shm_iq): A8's path */
  IQ_FEED_WIDEBAND,     /* several cells, a radio: one wideband capture through the in-process channelizer (this module) */
  IQ_FEED_STARTED_ONLY, /* several cells, but rfsim / offline replay input: the started cell only (logged by the caller) */
} iq_feed_mode_t;
iq_feed_mode_t iq_feed_input_mode(int n_cells, const char *device_name, int sim_or_replay);
const char *iq_feed_mode_name(iq_feed_mode_t m);
#endif
