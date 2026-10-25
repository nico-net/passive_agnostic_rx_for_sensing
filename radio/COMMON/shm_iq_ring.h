/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1/C1: single-producer / many-consumer IQ broadcast ring.
 * One writer publishes sc16 samples (all channels); N readers read them, each with its own cursor.
 * The producer never waits for consumers: a consumer that falls more than `capacity` samples behind sees an overrun
 * and skips forward (its timestamps jump, so the receiver's own stream-continuity logic sees a gap).
 *
 * Two backings (mpci-main plan C1, inventory row 1):
 *   SHM_IQ_BACKING_ANON       the ring lives inside the process (private anonymous mapping, never shm_open). The multi-cell receiver uses it for the
 *                             wideband ring and for the channelizer's narrow per-cell rings; consumers are threads (views, reference counted).
 *   SHM_IQ_BACKING_POSIX_SHM  /dev/shm/<name>, for out-of-process tools (file publisher, a later out-of-process radio process) and the cross-process tests.
 * Both backings share one header and one set of read/write functions. */
#ifndef SHM_IQ_RING_H
#define SHM_IQ_RING_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SHM_IQ_MAGIC 0x5149484d53434153ULL /* "SACSMHIQ" */
#define SHM_IQ_VERSION 3u /* v2 = v1 + radio_id/time_source/away/dwell_until; v3 = v2 + consumer table (n_consumers, overrun accounting); v1/v2 rings are still accepted */
#define SHM_IQ_TS_UNKNOWN 0u
#define SHM_IQ_TS_FREE 1u /* radio clock free-running */
#define SHM_IQ_TS_PPS 2u  /* radio time aligned to PPS / external reference (comparable across radios) */
#define SHM_IQ_MAX_CH 8u
#define SHM_IQ_MAX_CONS 16u
#define SHM_IQ_HDR_BYTES 4096u

typedef enum { SHM_IQ_BACKING_ANON = 0, SHM_IQ_BACKING_POSIX_SHM = 1 } shm_iq_backing_t;

/* One slot per registered consumer (v3). Written by the consumer, read by anybody. */
typedef struct {
  _Atomic uint64_t pos;      /* the consumer's read cursor (sample index), updated with shm_iq_ring_consumer_update() */
  _Atomic uint64_t overruns; /* overruns this consumer suffered */
  _Atomic uint64_t dropped;  /* input samples it skipped (overruns and discontinuities) */
  _Atomic uint32_t active;
} shm_iq_cons_t;

typedef struct {
  uint64_t magic;
  uint32_t version, channels;
  uint64_t capacity;              /* samples per channel, power of two */
  double rate;                    /* samples/s of the ring */
  int64_t base_tick;              /* radio tick of ring index 0 (first sample ever written); published ts = base_tick + index */
  _Atomic uint64_t head;          /* index of the next sample to write (monotonic) */
  _Atomic uint64_t reserve;       /* seqlock-style: index up to which slots may be (over)written right now */
  _Atomic uint64_t valid_from;    /* first index of the current contiguous run (radio gap / retune / reload start a new run) */
  _Atomic uint64_t rf_epoch;      /* bumped on every retune and device reload */
  _Atomic uint64_t heartbeat_ns;  /* CLOCK_MONOTONIC of the last write */
  _Atomic int64_t tick_offset;    /* radio tick - index in the current run */
  _Atomic int32_t producer_pid;
  _Atomic uint64_t center_bits;   /* double: RF centre of the ring (Hz), as bits so it can be atomic */
  _Atomic uint64_t gaps;          /* radio discontinuities seen by the producer */
  /* --- v2 --- */
  char radio_id[48];              /* which radio produced this window (the radio's sdr address, else pid) */
  uint32_t time_source;           /* SHM_IQ_TS_* */
  _Atomic uint32_t away;          /* time-shared window: the radio is tuned elsewhere, nothing is written until dwell_until_ns */
  _Atomic uint64_t dwell_until_ns;/* CLOCK_MONOTONIC: when the producer expects to be back (consumers extend their timeout) */
  /* --- v3 --- */
  uint32_t n_consumers;           /* size of the consumer table in use (0 = consumers are not tracked: the v1/v2 behaviour) */
  uint32_t backing;               /* shm_iq_backing_t of the producer */
  _Atomic uint64_t overruns;      /* overruns summed over all tracked consumers */
  shm_iq_cons_t cons[SHM_IQ_MAX_CONS];
} shm_iq_hdr_t;

typedef struct {
  shm_iq_hdr_t *hdr;
  int16_t *data[SHM_IQ_MAX_CH];   /* channel c: capacity complex sc16 samples */
  uint64_t map_bytes;
  char name[64];
  _Atomic int new_run;            /* producer-local: new_epoch() was called, the next write starts a new run */
  int backing;                    /* shm_iq_backing_t */
  int owner;                      /* 1 = the creating (producer) handle, 0 = a consumer attach / view */
  int writable;                   /* the mapping can be written (consumer table updates need it) */
  void *anon;                     /* anonymous backing: reference-counted control block shared by the producer handle and all views */
} shm_iq_ring_t;

/* Producer: create a ring. capacity (samples per channel) is rounded up to a power of two.
 *   ANON:      the ring lives in this process; `name` may be NULL (reach it with shm_iq_ring_view()) or a registry name (reach it with shm_iq_ring_attach(),
 *              which tries the in-process registry before /dev/shm). Never calls shm_open.
 *   POSIX_SHM: create (or re-create with the new geometry) /dev/shm/<name>; `name` is required.
 * n_consumers (0..SHM_IQ_MAX_CONS): tracked consumers (cursor + overrun accounting); 0 = untracked. */
int shm_iq_ring_create(shm_iq_ring_t *r, shm_iq_backing_t backing, const char *name, unsigned n_rx, uint64_t capacity, double rate, double center_hz,
                       unsigned n_consumers);
/* Consumer: map an existing ring. In-process registry first (anonymous rings), then /dev/shm. Returns -1 (errno) if absent or not a valid ring. */
int shm_iq_ring_attach(shm_iq_ring_t *r, const char *name);
/* Consumer of an anonymous ring in this process: a view onto the producer handle (reference counted; the memory is freed when the last handle is detached). */
int shm_iq_ring_view(shm_iq_ring_t *view, const shm_iq_ring_t *producer);
void shm_iq_ring_detach(shm_iq_ring_t *r);
/* Fault in every page of an anonymous ring now (the receiver pre-faults its pools before RX). No-op for POSIX_SHM. */
void shm_iq_ring_prefault(shm_iq_ring_t *r);

/* Ring capacity from the host-RAM budget (pure function; R8: the ring comes from the host-RAM plan, never a fixed 2^27 samples).
 *   budget_bytes  what the caller may spend on these rings (e.g. a share of nr_host_membudget_headroom(); SIZE_MAX = unlimited)
 *   rate          samples/s of the ring        n_rx   channels        n_rings  rings of this geometry that must fit together
 *   target_ms     wanted duration of the ring  floor_ms below this duration the plan refuses (returns 0)
 * Returns the largest power of two <= ceil_pow2(rate x target_ms) whose n_rings x (header + n_rx x cap x 4 bytes) fits the budget, 0 when even floor_ms does not.
 * The wideband ring wants SHM_IQ_RING_WIDE_TARGET_MS (a 1.09 s ring gave 0 overruns where a 273 ms ring cascaded [MEASURED, DGX OTA 2026-10-15]); the old
 * rule "2^27 samples, halved when RAM is short" is this rule at 122.88 MS/s x 4 RX with the budget it had, "halved when short" being "fit the budget share". */
#define SHM_IQ_RING_WIDE_TARGET_MS 1000
#define SHM_IQ_RING_WIDE_FLOOR_MS 100
#define SHM_IQ_RING_NARROW_TARGET_MS 100
#define SHM_IQ_RING_NARROW_FLOOR_MS 20
uint64_t shm_iq_ring_plan_capacity(size_t budget_bytes, double rate, unsigned n_rx, unsigned n_rings, double target_ms, double floor_ms);
/* Bytes the ring maps (header + channels). */
uint64_t shm_iq_ring_bytes(unsigned n_rx, uint64_t capacity);

/* Producer: append n samples per channel read from the radio at radio tick `tick`. A tick that does not continue the
 * previous write starts a new contiguous run: forward gap: the index advances by the gap; backward (e.g. device reload):
 * the index advances by the wall time since the last write (>= 1), so consumer timestamps (base_tick + index) jump. */
void shm_iq_ring_write(shm_iq_ring_t *r, int64_t tick, void *const *bufs, int n, unsigned channels);
/* Producer, zero-copy variant of write(): reserve() raises `reserve` for up to `want` samples at the current head and returns the
 * per-channel slot pointers plus the contiguous count n <= want (never wraps the ring). The caller fills the slots (e.g. UHD recv),
 * then commit(tick, got) publishes them. commit returns 0 if the samples were published in place, -1 if `tick` does not continue the
 * run (first write, gap, backward, new epoch): nothing was published, the caller copies the samples out and uses write(). */
int shm_iq_ring_reserve(shm_iq_ring_t *r, int want, int16_t **slots);
int shm_iq_ring_commit(shm_iq_ring_t *r, int64_t tick, int got);
/* Producer: the radio was retuned or reloaded; the next write starts a new run (no index jump if its tick continues)
 * and rf_epoch is bumped. */
void shm_iq_ring_new_epoch(shm_iq_ring_t *r, double center_hz);

double shm_iq_ring_center(const shm_iq_ring_t *r);
/* Producer (v2 metadata). radio_id may be NULL. */
void shm_iq_ring_set_meta(shm_iq_ring_t *r, const char *radio_id, unsigned time_source);
/* Producer: time-shared window leaves (until_ns = expected return, CLOCK_MONOTONIC) / returns (new run + epoch bump;
 * `away` clears at the next write). */
void shm_iq_ring_set_away(shm_iq_ring_t *r, uint64_t until_ns);
void shm_iq_ring_return(shm_iq_ring_t *r, double center_hz);

/* Consumer: copy samples [pos, pos+n) of the first `channels` ring channels to dst[c] (sc16, n samples each).
 * Returns 0 on success, 1 if not yet written, -1 if (part of) the range was overwritten or precedes valid_from. */
int shm_iq_ring_copy(const shm_iq_ring_t *r, uint64_t pos, int n, unsigned channels, int16_t *const *dst);

/* Consumer table (v3, rings created with n_consumers > 0). register returns a slot id in [0, n_consumers) or -1 (errno ENOSYS: untracked ring or read-only
 * mapping; ENOSPC: all slots taken); every other call ignores a negative id, so a caller can use the id without checking. The producer never reads the table
 * to decide anything (it never waits for consumers); it is accounting for admission and diagnostics. */
int shm_iq_ring_consumer_register(shm_iq_ring_t *r);
void shm_iq_ring_consumer_release(shm_iq_ring_t *r, int id);
void shm_iq_ring_consumer_update(shm_iq_ring_t *r, int id, uint64_t pos);                 /* the consumer's cursor */
void shm_iq_ring_consumer_overrun(shm_iq_ring_t *r, int id, uint64_t dropped, int overrun); /* skipped `dropped` input samples; overrun=1 counts an overrun (0: a discontinuity) */
uint64_t shm_iq_ring_overruns(const shm_iq_ring_t *r);                                      /* sum over all tracked consumers */
uint64_t shm_iq_ring_consumer_overruns(const shm_iq_ring_t *r, int id);
uint64_t shm_iq_ring_consumer_dropped(const shm_iq_ring_t *r, int id);
uint64_t shm_iq_ring_consumer_lag(const shm_iq_ring_t *r, int id);                          /* head - cursor of that consumer (samples) */
uint64_t shm_iq_ring_max_lag(const shm_iq_ring_t *r);                                       /* slowest active consumer */

/* Radio side channel (POSIX_SHM rings only, cross-process): shm cells have no radio of their own, so CHPOW / RFCHAN / RFSTALL would never reach their logs. The publisher appends
 * text lines to /dev/shm/<ring>.radio (header lines "RFRING ch.. port= gain= freq_hz= rate=" survive rotation; "SENSING: CHPOW ..." and
 * "SENSING: RFSTALL ..." are the usrp_lib formats); every cell process polls the file and prints the lines to its own log, with RFRING
 * rewritten to the RFCHAN line of that cell (its tuning and rate). The file is capped (64 KiB) and best effort: no error is fatal. */
void shm_iq_radio_reset(const char *ring);                     /* publisher start: empty file */
void shm_iq_radio_append(const char *ring, const char *line);  /* one line without '\n' */
void shm_iq_radio_remove(const char *ring);
/* Consumer: copy complete lines written since *off into buf (NUL-terminated, '\n' separated), advance *off; returns bytes. A file that
 * was rotated or recreated (smaller than *off) is read from its start. */
size_t shm_iq_radio_poll(const char *ring, uint64_t *off, char *buf, size_t cap);
/* "SENSING: CHPOW sample=%llu dBFS ch0=.. ch1=.." from the last n samples of each channel (usrp_lib CHPOW format; sc16 full scale 32768) */
int shm_iq_radio_fmt_chpow(char *out, size_t cap, uint64_t sample, const int16_t *const *ch, unsigned nch, int n);

uint64_t shm_iq_now_ns(void);
#endif
