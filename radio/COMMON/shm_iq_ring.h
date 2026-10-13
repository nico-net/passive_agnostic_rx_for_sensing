/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1: single-producer / many-consumer IQ broadcast ring in POSIX shared memory.
 * One radio process publishes sc16 samples (all channels); N receiver processes read them, each with its own cursor.
 * The producer never waits for consumers: a consumer that falls more than `capacity` samples behind sees an overrun
 * and skips forward (its timestamps jump, so the receiver's own stream-continuity logic sees a gap). */
#ifndef SHM_IQ_RING_H
#define SHM_IQ_RING_H
#include <stdatomic.h>
#include <stdint.h>

#define SHM_IQ_MAGIC 0x5149484d53434153ULL /* "SACSMHIQ" */
#define SHM_IQ_VERSION 1u
#define SHM_IQ_MAX_CH 8u
#define SHM_IQ_HDR_BYTES 4096u

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
} shm_iq_hdr_t;

typedef struct {
  shm_iq_hdr_t *hdr;
  int16_t *data[SHM_IQ_MAX_CH];   /* channel c: capacity complex sc16 samples */
  uint64_t map_bytes;
  char name[64];
} shm_iq_ring_t;

/* Producer: create (or re-create with the new geometry) /dev/shm/<name>. capacity is rounded up to a power of two. */
int shm_iq_ring_create(shm_iq_ring_t *r, const char *name, unsigned channels, uint64_t capacity, double rate, double center_hz);
/* Consumer: map an existing ring read-only. Returns -1 (errno) if absent or not a valid ring. */
int shm_iq_ring_attach(shm_iq_ring_t *r, const char *name);
void shm_iq_ring_detach(shm_iq_ring_t *r);

/* Producer: append n samples per channel read from the radio at radio tick `tick`. A tick that does not continue the
 * previous write starts a new contiguous run (forward gap: the index advances by the gap; backward: index continues). */
void shm_iq_ring_write(shm_iq_ring_t *r, int64_t tick, void *const *bufs, int n, unsigned channels);
/* Producer: the radio was retuned or reloaded; the next write starts a new run and rf_epoch is bumped. */
void shm_iq_ring_new_epoch(shm_iq_ring_t *r, double center_hz);

double shm_iq_ring_center(const shm_iq_ring_t *r);

/* Consumer: copy samples [pos, pos+n) of the first `channels` ring channels to dst[c] (sc16, n samples each).
 * Returns 0 on success, 1 if not yet written, -1 if (part of) the range was overwritten or precedes valid_from. */
int shm_iq_ring_copy(const shm_iq_ring_t *r, uint64_t pos, int n, unsigned channels, int16_t *const *dst);

uint64_t shm_iq_now_ns(void);
#endif
