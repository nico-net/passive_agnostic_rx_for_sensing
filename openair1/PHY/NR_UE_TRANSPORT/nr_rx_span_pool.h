/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* adaptive_RX_pipeline.md, Stage 1 / P04 (immutable buffer delivery, foundation-only). Refcounted,
 * channel-routed IQ span delivery from a single producer (AcquisitionOwner, plan sec 3.1) to the
 * NR_RX_BRANCH_MAX consumer branches (P03's nr_rx_branch_t). No sample copies: a published span is
 * shared by reference and is immutable until every branch holding it releases it.
 *
 * Deliberately NOT wired into the nr-ue.c read loop yet -- see the P04 brief's "Controller scope
 * ruling": that file carries another session's uncommitted edits, so this module ships standalone
 * with its G1 tests run in pure form (a synthetic producer/consumer harness in the test, not the
 * real RT loop). Wiring is P05+.
 *
 * Field/behaviour -> plan section mapping:
 *   - nr_rx_span_t.physical_channel_data, n_ch    -> plan sec 3.1 "publishes immutable
 *                                                     channel-specific sample spans".
 *   - first_sample_ts, absolute_slot, acq_epoch    -> plan sec 3.2 schema: 64-bit first-sample
 *                                                     timestamp, absolute slot, RF continuity epoch.
 *   - hold_budget / per-branch ring + drop counters -> plan sec 4 P04: "bounded ownership/refcounts
 *                                                     ... a slow branch must not read overwritten
 *                                                     samples or hold all other branches
 *                                                     indefinitely. Define and count drop policy
 *                                                     per branch." G1 test 3.
 *   - refcount / acquire-publish-take-release        -> plan sec 4 P04: "bounded ownership/refcounts
 *                                                     or explicit copies", no sample copies chosen.
 *
 * No threads created, no hardware access, no globals -- pool state lives entirely in the
 * caller-owned nr_rx_span_pool_t.
 */
#ifndef NR_RX_SPAN_POOL_H
#define NR_RX_SPAN_POOL_H

#include <stdint.h>
#include <pthread.h>
#include "common/platform_types.h"
#include "nr_rx_branch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ring depth per branch (plan sec 4: "bounded ownership"). Kept small and fixed -- this is a
 * delivery pipeline, not a general queue; a branch that cannot keep up with hold_budget spans is,
 * by policy, meant to drop, not buffer indefinitely. */
#define NR_RX_SPAN_POOL_MAX_HOLD 8

/* One producer-filled buffer. Immutable to consumers once published (nr_rx_span_pool_take()
 * returns a const pointer); the producer never reuses a buffer while refcount > 0 (asserted in
 * nr_rx_span_pool_acquire()). Samples are channel-major: channel c, sample n is at
 * data[c * samples_per_buf + n]. */
typedef struct {
  c16_t *data;               /* n_ch * samples_per_buf, owned by the pool, allocated once at init;
                                 channel-major: channel c sample n is data[c*samples_per_buf + n].
                                 CONSUMERS MUST GO THROUGH nr_rx_span_channel()/
                                 nr_rx_span_for_branch() TO READ THIS -- in C, a const
                                 nr_rx_span_t* (what take() returns) does not make the pointee
                                 `data` const, so nothing stops code reaching in here directly and
                                 reading (or writing) another branch's channel. The accessors are
                                 the only enforcement there is; treat direct access to this field
                                 outside this module as a bug. */
  uint64_t first_sample_ts;  /* producer-set on acquire; plan sec 3.2 "64-bit first-sample timestamp" */
  uint64_t absolute_slot;    /* producer-set on acquire; plan sec 3.2 "absolute slot" */
  uint32_t acq_epoch;        /* producer-set on acquire; plan sec 3.2 "RF continuity epoch" */
  uint32_t n_samples;        /* producer-set on acquire; <= samples_per_buf */
  int n_ch;                  /* set once at pool init, constant for the buffer's lifetime -- carried
                                 per-buffer (not looked up via the pool) so nr_rx_span_channel() and
                                 nr_rx_span_for_branch() can take just the span, per the brief's
                                 signatures */
  int samples_per_buf;       /* set once at pool init, constant; the channel stride */
  int buf_id;                /* index into the pool's buffer array; -1 = invalid/unacquired */
  int refcount;               /* number of branches currently holding this span; always equal to
                                 __builtin_popcount(holders) -- asserted at every point that
                                 touches either (fix round 1, review IMPORTANT finding) */
  uint8_t holders;            /* bit b set = branch b currently holds a ref on this span (queued in
                                 its ring and/or taken-but-not-released). NR_RX_BRANCH_MAX is 4, so
                                 a byte is enough. SECOND LINE OF DEFENCE ONLY (fix round 2) -- a
                                 bare existence bit cannot tell whether a release() call naming
                                 branch_id actually came from that branch: if branch 0 mistakenly
                                 calls release() with branch_id=1 while branch 1's bit is still
                                 legitimately set, the bit-only check cannot distinguish that from a
                                 real branch-1 release, and it SUCCEEDS -- spending branch 1's share
                                 while branch 1 is still reading it. Branch 0's own later legitimate
                                 release then drops refcount to 0 under branch 1, and the producer
                                 can reacquire/overwrite the buffer while branch 1 is still reading
                                 it: a real use-after-free, not merely a narrower one. (Fix round 1's
                                 header comment claimed the holders bit alone "closes that hole" --
                                 it does not; that claim was wrong and is corrected here.) The actual
                                 fix is the lease token below (nr_rx_span_pool_t.lease_keys /
                                 nr_rx_span_lease_t) -- an identity the caller cannot fabricate,
                                 which a bit never can be. holders/refcount remain as a redundant
                                 consistency check (asserted against each other, and independently
                                 gated in release()) but are not what makes release() safe. */
} nr_rx_span_t;

/* fix round 2 (review IMPORTANT finding, round 2): a lease token returned by
 * nr_rx_span_pool_take(), required by nr_rx_span_pool_release() to prove the CALLER is the branch
 * that actually took this specific span -- closing the hole the holders bitmask alone could not
 * (see nr_rx_span_t.holders' comment above for the concrete UAF trace). key is drawn from a
 * per-pool monotonically increasing counter (nr_rx_span_pool_t.next_lease_key, never 0) each time
 * take() succeeds, stored in that (branch_id, buf_id) pair's slot in
 * nr_rx_span_pool_t.lease_keys, and cleared back to 0 by a successful release(). Because the
 * counter never repeats for the life of the pool, a lease from an EARLIER occupant of the same
 * buf_id (already released and since republished) also fails to validate -- release() checks
 * against the CURRENT stored key, not merely "is this branch's bit set". span is NULL (key is then
 * meaningless, always 0) when take() had nothing to return. */
typedef struct {
  const nr_rx_span_t *span;
  uint32_t key; /* 0 = no lease (take() returned nothing, or this lease was already released) */
} nr_rx_span_lease_t;

/* Channel pointer + identity triple, returned by nr_rx_span_for_branch() so a branch cannot
 * trivially read a channel other than its own (plan sec 4 "Channel routing"). */
typedef struct {
  const c16_t *samples;   /* NULL if branch_id/physical_channel invalid for this span/pool */
  uint8_t branch_id;
  int8_t physical_channel;
} nr_rx_span_view_t;

typedef struct {
  int buf_id[NR_RX_SPAN_POOL_MAX_HOLD]; /* FIFO ring of held span buf_ids, oldest at head */
  int n_held;
  uint64_t samples_dropped;
  uint64_t dropped_spans;
} nr_rx_span_branch_state_t;

typedef struct {
  nr_rx_span_t *bufs;        /* n_buf entries */
  c16_t *storage;            /* backing store for every bufs[i].data, freed as one block */
  int n_buf;
  int n_ch;
  int samples_per_buf;
  int hold_budget;           /* <= NR_RX_SPAN_POOL_MAX_HOLD */
  uint64_t sample_rate_hz;   /* caller-supplied, constant for pool lifetime; plan sec 3.2 "Run /
                                 hardware" row lists sample rate. A run-wide hardware constant, not
                                 per-span -- a retune is an RF discontinuity (P05, acq_epoch bump),
                                 not a mid-run sample-rate change this pool needs to track per
                                 buffer. Stored here (one field, no per-span cost) purely so a
                                 consumer can read it back; 0 = not supplied. run_id/rx_id are
                                 deliberately NOT duplicated here -- they already live on P03's
                                 nr_rx_branch_set_t/nr_rx_branch_t, which every caller of this pool
                                 already has. */

  int *free_list;            /* n_buf-capacity stack of free buf_ids */
  int n_free;

  nr_rx_span_branch_state_t branch[NR_RX_BRANCH_MAX];
  uint8_t branch_active[NR_RX_BRANCH_MAX]; /* which branch slots participate in publish() */
  uint8_t n_active_branches;

  uint32_t *lease_keys;       /* fix round 2: NR_RX_BRANCH_MAX * n_buf u32 slots, allocated with the
                                 pool; slot [branch_id * n_buf + buf_id] holds the key take() issued
                                 for that (branch, buffer) pair, or 0 if branch_id has no outstanding
                                 taken-but-unreleased lease on buf_id. A few hundred bytes even at
                                 4 branches * dozens of buffers. */
  uint32_t next_lease_key;    /* monotonically increasing, never 0 (0 is the sentinel for "no
                                 lease"); incremented and used fresh on every successful take() */

  uint64_t producer_stalls;  /* count of acquire() calls that found no free buffer (should be 0
                                 by construction when n_buf >= 1 + n_branch*hold_budget; the
                                 module asserts rather than silently blocking, since there are no
                                 threads here to wait on) */

  pthread_mutex_t lock;      /* ponytail: one lock for the whole pool, protecting the ring/refcount
                                 bookkeeping above -- the RT loop publishes once per slot (~2 kHz),
                                 nowhere near enough to make per-branch lock-free rings worth the
                                 complexity. Revisit if profiling ever shows contention. */

  uint32_t magic;            /* fix round 1 (review MINOR): set to NR_RX_SPAN_POOL_MAGIC on a
                                 successful init(), cleared to 0 by destroy(). init() on a struct
                                 that already carries the magic is rejected (-1) instead of leaking
                                 the live bufs/storage/free_list allocations and re-initializing a
                                 mutex that may still be locked/in use. */
} nr_rx_span_pool_t;

#define NR_RX_SPAN_POOL_MAGIC 0x53504e31u /* "SPN1" -- arbitrary, just needs to not be 0 */

/* Allocates bufs/free_list/lease_keys and initializes the pool for n_ch channels, samples_per_buf samples
 * per channel per buffer, n_buf total buffers and hold_budget spans held per active branch.
 * active_branches (bitmask, bit i = branch i participates in future publish()/take()/release()
 * calls; use nr_rx_branch_set_t.b[i].state != NR_RXB_DISABLED to build it) fixes n_active_branches
 * for the life of the pool. sample_rate_hz is stored verbatim (plan sec 3.2 schema field; 0 if the
 * caller doesn't have/need one) and never interpreted by this module.
 *
 * Rejects (returns -1, LOG_E(PHY, ...), *pool left zeroed) when: pool/n_ch/samples_per_buf/n_buf/
 * hold_budget are invalid (<=0, hold_budget > NR_RX_SPAN_POOL_MAX_HOLD, n_ch > NR_RX_BRANCH_MAX),
 * active_branches has no bit set or a bit >= NR_RX_BRANCH_MAX, n_buf < 1 + n_active_branches *
 * hold_budget (the capacity precondition that makes producer starvation impossible by
 * construction -- see nr_rx_span_pool_acquire()), or pool is already a live/initialized pool
 * (fix round 1: double-init is rejected rather than leaking the previous bufs/storage/free_list
 * allocations and re-initializing a mutex that may still be in use -- *pool is left UNTOUCHED,
 * not zeroed, in this one case, since zeroing it would itself discard the live allocation
 * pointers needed to ever free them).
 * Returns 0 on success. */
int nr_rx_span_pool_init(nr_rx_span_pool_t *pool, int n_ch, int samples_per_buf, int n_buf,
                          int hold_budget, uint8_t active_branches, uint64_t sample_rate_hz);

/* Frees bufs/free_list/lease_keys. Safe to call on an already-freed/zeroed pool. Does not check outstanding
 * refcounts -- caller must ensure no span is held before destroying the pool (same discipline as
 * freeing a buffer while it's in use anywhere else). */
void nr_rx_span_pool_destroy(nr_rx_span_pool_t *pool);

/* Producer side. Pops a buffer off the free list, asserts its refcount == 0 (a buffer must never
 * be handed to the producer while a consumer still holds it -- programming error, not a runtime
 * condition, hence assert not a returned error), resets buf_id/refcount/n_samples and returns its
 * pointer for the caller to fill (channel-major, up to n_ch * samples_per_buf c16_t) and stamp
 * first_sample_ts/absolute_slot/acq_epoch/n_samples before calling nr_rx_span_pool_publish().
 *
 * Returns NULL and increments pool->producer_stalls if the free list is empty -- unreachable when
 * n_buf >= 1 + n_active_branches*hold_budget (asserted at init), kept as a real return-NULL path
 * rather than a second assert so a future caller with a looser capacity margin degrades instead of
 * aborting. */
nr_rx_span_t *nr_rx_span_pool_acquire(nr_rx_span_pool_t *pool);

/* Publishes buf (previously returned by acquire()): sets refcount = n_active_branches, sets
 * buf->holders' bit for every active branch, and enqueues buf_id into every active branch's ring.
 * Per branch, if that branch's ring is already at hold_budget, the OLDEST entry in THAT branch's
 * ring is released first (that branch's holders bit cleared, refcount--, and if it hits 0 the
 * buffer returns to the free list) and branch.samples_dropped += buf->n_samples,
 * branch.dropped_spans++ -- other branches' rings are untouched (plan sec 4: "a slow branch must
 * not ... hold all other branches indefinitely"). No-op (LOG_E) if buf is NULL or not a buffer
 * this pool owns. */
void nr_rx_span_pool_publish(nr_rx_span_pool_t *pool, nr_rx_span_t *buf);

/* Consumer side. Pops branch_id's oldest ring entry (FIFO -- publish order) and returns a lease on
 * it: {span, key} with key freshly drawn from the pool's monotonic counter and recorded as the
 * CURRENT valid key for (branch_id, span->buf_id). release() must eventually be called exactly
 * once with this exact lease. Returns {NULL, 0} if branch_id is inactive/out-of-range or its ring
 * is empty. Does not touch the ring/refcount/lease state of any other branch. */
nr_rx_span_lease_t nr_rx_span_pool_take(nr_rx_span_pool_t *pool, uint8_t branch_id);

/* Decrements the leased span's refcount on branch_id's behalf; when it reaches 0 the buffer is
 * returned to the free list. Returns 0 on success, -1 (LOG_E naming branch_id and buf_id, no state
 * change) if pool/lease.span is NULL, branch_id is out of range, OR lease.key does not equal the
 * CURRENT key recorded for (branch_id, span->buf_id) -- this is the fix round 2 mechanism (review
 * IMPORTANT finding, round 2) that actually closes the hazard nr_rx_span_t.holders' comment
 * describes: a plain per-branch existence bit cannot tell whether a release() call genuinely came
 * from the branch it names, so releasing through the WRONG active branch_id could silently spend a
 * different branch's still-needed share while that branch keeps reading -- a real use-after-free
 * once that branch's own later legitimate release (or a drop) hits refcount 0 and the producer
 * reacquires the buffer. A lease is an identity the caller cannot fabricate (it can only be
 * obtained from THIS branch's own prior take() call on THIS buffer occupancy -- the key is
 * per-pool monotonic and never repeats, so a stale lease from an earlier, already-released
 * occupant of the same recycled buf_id also fails to validate). holders/refcount are kept and
 * checked as a second, redundant line of defence (should always agree with the key check given the
 * invariants; asserted, not just checked) -- not the primary guarantee. */
int nr_rx_span_pool_release(nr_rx_span_pool_t *pool, uint8_t branch_id, nr_rx_span_lease_t lease);

/* Returns the channel pointer for branch's own physical_channel within span, plus the
 * (branch_id, physical_channel) identity, so a branch cannot casually read a different channel's
 * samples than the one it's authorized for. samples is NULL (branch_id/physical_channel invalid
 * for this span's channel count, or span/branch NULL) on any misuse. */
nr_rx_span_view_t nr_rx_span_for_branch(const nr_rx_span_t *span, const nr_rx_branch_t *branch);

/* Direct channel accessor (plan sec 4: "nr_rx_span_channel(span, physical_channel)"). Returns NULL
 * if span is NULL or physical_channel is out of [0, span->n_ch) -- callers that don't have an
 * nr_rx_branch_t handy (e.g. tests) can still validate routing this way; nr_rx_span_for_branch()
 * is the branch-safe wrapper most real callers should use. */
const c16_t *nr_rx_span_channel(const nr_rx_span_t *span, int physical_channel);

#ifdef __cplusplus
}
#endif
#endif
