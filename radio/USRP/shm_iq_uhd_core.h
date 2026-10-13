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

typedef enum { PUB_RX_OK, PUB_RX_OVERFLOW, PUB_RX_TIMEOUT, PUB_RX_OTHER } pub_rx_status_t;
/* Fill dst[c] (ring slot memory) with up to n sc16 samples per channel; report their count and the radio tick of the first one. */
typedef pub_rx_status_t (*pub_recv_fn)(void *ctx, int16_t *const *dst, int n, int64_t *tick, int *got);

typedef struct pub pub_t;
typedef struct { uint64_t samples, overflows, timeouts, others, inplace, copied, head; int consecutive_timeouts; } pub_stats_t;

/* ts: 1 free-running clock, 2 PPS-aligned (SHM_IQ_TS_*). NULL on failure (errno). */
pub_t *pub_open(const char *ring, unsigned channels, uint64_t capacity, double rate, double center_hz, const char *radio_id, unsigned ts);
void pub_close(pub_t *p);
const pub_stats_t *pub_stats(pub_t *p); /* head refreshed */
uint64_t pub_capacity(const pub_t *p);
#ifndef __cplusplus
shm_iq_ring_t *pub_ring(pub_t *p);
#endif

/* One recv straight into the ring slots (reserve -> recv -> commit); only a run break (first write, overflow gap) takes the
 * copy path through shm_iq_ring_write(), which turns the timestamp jump into a ring gap. Returns samples published. */
int pub_step(pub_t *p, pub_recv_fn recv, void *ctx, int want);
/* Sample-rate tick of a packet from its master-clock tick (decim = MCR / rate, integer; 1 without a DDC). Floor division: a
 * stream started on an odd MCR tick sits on x.5 sample ticks, and rounding those can flip between packets (spurious 1-tick gaps).
 * Never earlier than the scheduled START tick: a restarted X410 stream can repeat the previous START timestamp (usrp_ring_recovery.h). */
int64_t pub_tick(int64_t mcr_ticks, int64_t decim, int64_t start_tick);
#ifdef __cplusplus
}
#endif
#endif
