/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Standalone UHD publisher: the UHD-free recv -> ring step (C, so it links with shm_iq_ring.c; tested with a fake source in
 * tests/test_shm_iq_uhd_core.c). */
#ifndef SHM_IQ_UHD_CORE_H
#define SHM_IQ_UHD_CORE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef __cplusplus
#include "shm_iq_ring.h" /* C++ callers use the opaque pub_t only: the ring header uses C11 _Atomic */
#endif

#define PUB_MAX_CH 8 /* = SHM_IQ_MAX_CH (the ring header is C-only) */
typedef enum { PUB_RX_OK, PUB_RX_OVERFLOW, PUB_RX_TIMEOUT, PUB_RX_OTHER } pub_rx_status_t;
/* Fill dst[c] (ring slot memory) with up to n sc16 samples per channel; report their count and the radio tick of the first one. */
typedef pub_rx_status_t (*pub_recv_fn)(void *ctx, int16_t *const *dst, int n, int64_t *tick, int *got);

typedef struct pub pub_t;
typedef struct { uint64_t samples, overflows, timeouts, others, inplace, copied, head, resyncs, shortened, staged; int consecutive_timeouts; } pub_stats_t;

/* Default ring capacity (samples per channel): 2^27 (1.09 s at 122.88 MS/s, 0.55 s at 245.76; the 2^25 default of 2026-10-15 was 273 ms and one
 * 508-cell stall cascaded into 31 overruns), halved while the ring (4096 + channels * cap * 4 bytes, in /dev/shm) would take more than
 * half of what is free above `keep_free_bytes` (never below 2^23). mem_avail_bytes = MemAvailable. */
uint64_t pub_default_capacity(unsigned channels, uint64_t mem_avail_bytes, uint64_t keep_free_bytes);
/* ts: 1 free-running clock, 2 PPS-aligned (SHM_IQ_TS_*). NULL on failure (errno). */
pub_t *pub_open(const char *ring, unsigned channels, uint64_t capacity, double rate, double center_hz, const char *radio_id, unsigned ts);
void pub_close(pub_t *p);
const pub_stats_t *pub_stats(pub_t *p); /* head refreshed */
uint64_t pub_capacity(const pub_t *p);
/* Radio side channel (shm_iq_ring.h, /dev/shm/<ring>.radio): the cells print these lines in their own logs. pub_radio_reset() at start,
 * pub_radio_line() appends one line, pub_radio_chpow() appends the CHPOW line for the last n published samples (ISAC_USRP_CHPOW_S format). */
void pub_radio_reset(pub_t *p);
void pub_radio_line(pub_t *p, const char *line);
void pub_radio_chpow(pub_t *p, int n);
#ifndef __cplusplus
shm_iq_ring_t *pub_ring(pub_t *p);
#endif

/* One recv straight into the ring slots (reserve -> recv -> commit); only a run break (first write, overflow gap) takes the
 * copy path through shm_iq_ring_write(), which turns the timestamp jump into a ring gap. Returns samples published. */
int pub_step(pub_t *p, pub_recv_fn recv, void *ctx, int want);
/* Multi-streamer (one recv thread per channel): the coordinator reserves the chunk (before ANY thread writes a slot), each thread
 * recv()s its own channel into slots[c] and reports one pub_chres_t, the coordinator calls pub_mc_finish() once all have arrived.
 * A chunk is published only if every channel is OK with the SAME tick and count. Otherwise nothing is published and the action tells
 * the caller what to do: OVERFLOW / MISMATCH (a channel timed out or tick/count differ; `resyncs`) / OTHER -> stop, drain and
 * re-arm ALL streamers with one timed START (the next chunk's tick jump becomes a ring gap); IDLE = every channel timed out.
 * Additive API: the ring semantics are unchanged. Returns samples published. */
typedef struct { pub_rx_status_t st; int64_t tick; int got; } pub_chres_t;
typedef enum { PUB_MC_OK, PUB_MC_IDLE, PUB_MC_OVERFLOW, PUB_MC_MISMATCH, PUB_MC_OTHER } pub_mc_act_t;
int pub_mc_reserve(pub_t *p, int want, int16_t **slots); /* contiguous samples per channel reserved; slots[c] is where channel c recv()s */
/* How many samples channel c should recv() into slots[c] this chunk: n (the pub_mc_reserve result) minus the samples carried over.
 * A chunk whose channels delivered DIFFERENT counts from the SAME start tick is published at the shortest count (not a restart): the
 * excess of the longer channels is kept and opens the next chunk (res[c].tick is the tick of the newly received samples only).
 * Only a difference above a quarter of the chunk, a tick difference, an error or a timeout is still MISMATCH/OVERFLOW/OTHER. */
int pub_mc_want(const pub_t *p, unsigned c, int n);
int pub_mc_finish(pub_t *p, const pub_chres_t *res, int16_t *const *slots, pub_mc_act_t *act);
/* Sample-rate tick of a packet from its master-clock tick (decim = MCR / rate, integer; 1 without a DDC). Floor division: a
 * stream started on an odd MCR tick sits on x.5 sample ticks, and rounding those can flip between packets (spurious 1-tick gaps).
 * Never earlier than the scheduled START tick: a restarted X410 stream can repeat the previous START timestamp (usrp_ring_recovery.h). */
int64_t pub_tick(int64_t mcr_ticks, int64_t decim, int64_t start_tick);
/* usrp_lib.cpp hands the receiver X4x0/X3x0/N3x0 samples arithmetically shifted right by 2 (rxshift, simde srai_epi16): apply the
 * same in place to n sc16 samples so ring consumers see the digital level of a single-process receiver. shift 0 = no-op. */
void pub_rx_shift(int16_t *x, int n, int shift);
/* X410 34B2AF1 stamps every RX packet with the stream START time (usrp_lib USRP_TS_FROZEN, 2026-10-06). Per stream: the first
 * packet after a (re)start fixes `first`; a later packet stamped BEFORE the expected `next` (the start stamp, or the slowly
 * decreasing stamps seen on air 2026-10-15) switches the stream to frozen mode for good, and from then on ticks come from the
 * sample count (loss is still reported by UHD error codes and handled by the restart). A forward jump stays a gap.
 * reset_run() at every (re)start. Returns the tick to publish for these `got` samples. */
typedef struct { int64_t first, next; int frozen; } pub_tick_track_t;
void pub_tick_reset_run(pub_tick_track_t *t);
int64_t pub_tick_track(pub_tick_track_t *t, int64_t tick, int got);
#ifdef __cplusplus
}
#endif
#endif
