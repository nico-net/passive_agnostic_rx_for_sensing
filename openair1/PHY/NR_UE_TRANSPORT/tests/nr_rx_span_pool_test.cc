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

#include <gtest/gtest.h>
extern "C" {
#include "nr_rx_span_pool.h"
#include "nr_rx_branch.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

static const int ALL4 = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);

// f_c(n): a sequence distinct per channel c so a channel mix-up is byte-detectable.
static c16_t f(int c, int n)
{
  return c16_t{(int16_t)(1000 * c + n), (int16_t)(-(1000 * c + n))};
}

// ---- G1 test 1 (routing): byte-exact channel routing under an identity map and, on the SAME
// published span (routing is an accessor-time decision, not a publish-time one), a permuted map.
TEST(SpanPoolRouting, ByteExactRoutingIdentityAndPermuted) {
  nr_rx_branch_set_t set_id, set_perm;
  ASSERT_EQ(nr_rx_branch_set_parse(&set_id, "0,1,2,3", "0:0,1:1,2:2,3:3", "rx"), 0);
  ASSERT_EQ(nr_rx_branch_set_parse(&set_perm, "0,1,2,3", "0:3,1:2,2:1,3:0", "rx"), 0);

  const int n_ch = 4, spb = 8;
  nr_rx_span_pool_t pool = {}; // zero-init: init()'s double-init guard reads pool->magic before
                                // touching the struct, so a genuinely-uninitialized stack local
                                // must not be passed in (see nr_rx_span_pool_init()'s header doc)
  ASSERT_EQ(nr_rx_span_pool_init(&pool, n_ch, spb, 1 + 4 * 1, /*hold_budget=*/1, ALL4, 30720000), 0);

  nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf, nullptr);
  for (int c = 0; c < n_ch; c++)
    for (int n = 0; n < spb; n++)
      buf->data[c * spb + n] = f(c, n);
  buf->n_samples = spb;
  buf->first_sample_ts = 123456;
  buf->absolute_slot = 77;
  buf->acq_epoch = 5;
  nr_rx_span_pool_publish(&pool, buf);

  nr_rx_span_lease_t taken[4];
  for (int b = 0; b < 4; b++) {
    taken[b] = nr_rx_span_pool_take(&pool, b);
    ASSERT_NE(taken[b].span, nullptr);
  }

  for (int b = 0; b < 4; b++) {
    nr_rx_span_view_t v_id = nr_rx_span_for_branch(taken[b].span, &set_id.b[b]);
    ASSERT_NE(v_id.samples, nullptr);
    EXPECT_EQ(v_id.branch_id, b);
    EXPECT_EQ(v_id.physical_channel, set_id.b[b].physical_channel);
    EXPECT_EQ(v_id.physical_channel, b); // identity map
    for (int n = 0; n < spb; n++) {
      c16_t expect = f(v_id.physical_channel, n);
      EXPECT_EQ(v_id.samples[n].r, expect.r) << "branch " << b << " n " << n;
      EXPECT_EQ(v_id.samples[n].i, expect.i) << "branch " << b << " n " << n;
    }
    nr_rx_span_view_t v_perm = nr_rx_span_for_branch(taken[b].span, &set_perm.b[b]);
    ASSERT_NE(v_perm.samples, nullptr);
    EXPECT_EQ(v_perm.branch_id, b);
    EXPECT_EQ(v_perm.physical_channel, set_perm.b[b].physical_channel);
    EXPECT_EQ(v_perm.physical_channel, 3 - b); // "0:3,1:2,2:1,3:0"
    for (int n = 0; n < spb; n++) {
      c16_t expect = f(v_perm.physical_channel, n);
      EXPECT_EQ(v_perm.samples[n].r, expect.r) << "branch " << b << " n " << n;
      EXPECT_EQ(v_perm.samples[n].i, expect.i) << "branch " << b << " n " << n;
    }
  }

  for (int b = 0; b < 4; b++)
    EXPECT_EQ(nr_rx_span_pool_release(&pool, b, taken[b]), 0);
  nr_rx_span_pool_destroy(&pool);
}

// ---- G1 test 3 (stall): branch 2 never takes; 10 publishes at hold_budget=3 must drop exactly
// the oldest 7 for branch 2 only, leave branches 0/1/3 untouched, never stall the producer, and
// never let a consumer observe an overwritten buffer.
TEST(SpanPoolStall, SlowBranchDropsOldestWithoutStarvingProducerOrSiblings) {
  const int n_active = 4, hold_budget = 3;
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, 1 + n_active * hold_budget,
                                  hold_budget, ALL4, 0),
            0);

  for (int sp = 0; sp < 10; sp++) {
    nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
    ASSERT_NE(buf, nullptr) << "sp=" << sp;
    buf->data[0] = c16_t{(int16_t)sp, (int16_t)sp}; // fill with span index -- overwrite detector
    buf->n_samples = 1;
    buf->absolute_slot = (uint64_t)sp;
    buf->first_sample_ts = 1000 + (uint64_t)sp;
    buf->acq_epoch = 0;
    nr_rx_span_pool_publish(&pool, buf);

    // Branches 0, 1, 3 keep up: take + verify + release immediately, every publish.
    for (int b : {0, 1, 3}) {
      nr_rx_span_lease_t s = nr_rx_span_pool_take(&pool, (uint8_t)b);
      ASSERT_NE(s.span, nullptr) << "branch " << b << " sp " << sp;
      EXPECT_EQ(s.span->data[0].r, sp) << "branch " << b << " sp " << sp << " -- overwritten buffer";
      EXPECT_EQ(s.span->absolute_slot, (uint64_t)sp);
      EXPECT_EQ(s.span->first_sample_ts, 1000u + (uint64_t)sp);
      EXPECT_EQ(nr_rx_span_pool_release(&pool, (uint8_t)b, s), 0);
    }
    // Branch 2 never takes.
  }

  EXPECT_EQ(pool.producer_stalls, 0u); // free list never went empty across all 10 acquires
  for (int b : {0, 1, 3}) {
    EXPECT_EQ(pool.branch[b].dropped_spans, 0u) << "branch " << b;
    EXPECT_EQ(pool.branch[b].samples_dropped, 0u) << "branch " << b;
  }
  EXPECT_EQ(pool.branch[2].dropped_spans, 7u);
  EXPECT_EQ(pool.branch[2].samples_dropped, 7u); // n_samples=1 per span

  // Branch 2's surviving ring holds the newest 3 spans (7, 8, 9), oldest-first.
  for (int expect : {7, 8, 9}) {
    nr_rx_span_lease_t s = nr_rx_span_pool_take(&pool, 2);
    ASSERT_NE(s.span, nullptr);
    EXPECT_EQ(s.span->data[0].r, expect);
    EXPECT_EQ(nr_rx_span_pool_release(&pool, 2, s), 0);
  }
  EXPECT_EQ(nr_rx_span_pool_take(&pool, 2).span, nullptr); // ring now empty

  nr_rx_span_pool_destroy(&pool);
}

// ---- G1 test 3 support (refcount): a span held by 2 branches returns to the free list only
// after both release; a second release of the same fully-released span is rejected, not a double
// free.
TEST(SpanPoolRefcount, ReturnsToFreeListOnlyAfterAllReleasesAndRejectsDoubleRelease) {
  const int active_mask = (1 << 0) | (1 << 1);
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, 1 + 2 * 1, /*hold_budget=*/1,
                                  active_mask, 0),
            0);

  nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf, nullptr);
  buf->n_samples = 1;
  const int buf_id = buf->buf_id;
  nr_rx_span_pool_publish(&pool, buf);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 2);

  auto in_free_list = [&](int id) {
    for (int i = 0; i < pool.n_free; i++)
      if (pool.free_list[i] == id)
        return true;
    return false;
  };
  EXPECT_FALSE(in_free_list(buf_id));

  nr_rx_span_lease_t s0 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s0.span, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 0, s0), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 1);
  EXPECT_FALSE(in_free_list(buf_id)); // branch 1 still holds it

  nr_rx_span_lease_t s1 = nr_rx_span_pool_take(&pool, 1);
  ASSERT_NE(s1.span, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 1, s1), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);
  EXPECT_TRUE(in_free_list(buf_id));

  // ---- Fix round 2, test (b): double release of the SAME lease, once refcount is already 0, is
  // rejected -- not a double free. (lease_keys[idx] was cleared to 0 by the first release, and 0
  // never matches a real, nonzero lease key.)
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 1, s1), -1);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);

  nr_rx_span_pool_destroy(&pool);
}

// ---- Fix round 2 (review IMPORTANT finding, round 2 -- corrects fix round 1's claim). The
// re-review traced the concrete hazard a bare per-branch existence bit cannot prevent: if branch 0
// mistakenly calls release() with branch_id=1 while branch 1's bit is still legitimately set (both
// branches still genuinely hold their share), the bit-only check cannot tell that call apart from a
// real branch-1 release -- it succeeds, spending branch 1's share while branch 1 is still reading.
// Branch 0's own later legitimate release then drops refcount to 0 UNDER branch 1, and the producer
// can reacquire/overwrite the buffer while branch 1 still reads it: a real use-after-free. Fix round
// 1's header comment claiming the holders bit alone "closes that hole", and this test file's own
// prior "does not create a use-after-free either way" structural note, were both WRONG and are
// retracted by this comment (see nr_rx_span_pool.h's nr_rx_span_t.holders field doc for the same
// correction in the header).
//
// The fix is a lease token (nr_rx_span_lease_t): an identity the caller cannot fabricate, which a
// bit never can be. This test is fix round 2's requested test (a): the impersonation case now
// REJECTS, in the literal "both branches still hold their share" form that a bare bit could never
// achieve.
TEST(SpanPoolRefcount, ImpersonationWhileBothStillHoldIsRejectedByLeaseKey) {
  const int active_mask = (1 << 0) | (1 << 1);
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, 1 + 2 * 1, /*hold_budget=*/1,
                                  active_mask, 0),
            0);

  nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf, nullptr);
  buf->n_samples = 1;
  buf->data[0] = c16_t{(int16_t)42, (int16_t)42}; // sentinel content, checked "still readable" below
  const int buf_id = buf->buf_id;
  nr_rx_span_pool_publish(&pool, buf); // refcount=2, holders={bit0,bit1}
  ASSERT_EQ(pool.bufs[buf_id].refcount, 2);
  ASSERT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x3);

  // Both branches take their own copy -- BOTH genuinely still hold their share at this point,
  // which is exactly the case a bare existence bit cannot arbitrate (bit 0 AND bit 1 are both
  // legitimately set) but a lease key can, because each branch's key is distinct.
  nr_rx_span_lease_t lease0 = nr_rx_span_pool_take(&pool, 0);
  nr_rx_span_lease_t lease1 = nr_rx_span_pool_take(&pool, 1);
  ASSERT_NE(lease0.span, nullptr);
  ASSERT_NE(lease1.span, nullptr);
  ASSERT_NE(lease0.key, lease1.key) << "distinct takes must mint distinct keys";
  ASSERT_NE(lease0.key, 0u);
  ASSERT_NE(lease1.key, 0u);

  // Branch 0 mistakenly calls release() with branch_id=1, presenting its OWN lease (lease0). The
  // key recorded for (branch=1, buf_id) is lease1's key, not lease0's -- mismatch, rejected.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 1, lease0), -1);
  // And, the other direction ("with branch 1's lease it cannot have it" -- branch 0 cannot
  // release ITS OWN slot with branch 1's lease either): the key recorded for (branch=0, buf_id) is
  // lease0's key, not lease1's -- also a mismatch, also rejected.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, lease1), -1);

  // Neither rejected attempt touched anything: the span is still alive for both branches exactly
  // as before, and branch 1's data is genuinely still readable (not corrupted, not freed, not
  // reused by the producer).
  EXPECT_EQ(pool.bufs[buf_id].refcount, 2) << "no rejected release may have decremented";
  EXPECT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x3) << "both holder bits still set";
  ASSERT_NE(lease1.span, nullptr);
  EXPECT_EQ(lease1.span->data[0].r, 42) << "branch 1's span is still alive and readable";

  // The correct releases (each branch with its OWN lease) then free it exactly once.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, lease0), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 1);
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 1, lease1), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);
  EXPECT_EQ(pool.bufs[buf_id].holders, (uint8_t)0);

  int occurrences = 0;
  for (int i = 0; i < pool.n_free; i++)
    if (pool.free_list[i] == buf_id)
      occurrences++;
  EXPECT_EQ(occurrences, 1) << "freed exactly once, not double-freed by the rejected attempts";
  EXPECT_EQ(pool.n_free, 3);

  nr_rx_span_pool_destroy(&pool);
}

// ---- Still-valid supplementary case from fix round 1 (updated to the lease API): releasing
// through a branch_id whose own share was ALREADY legitimately spent is also rejected (a stale/
// zeroed lease slot never matches a real nonzero lease key).
TEST(SpanPoolRefcount, ReleaseThroughAnAlreadyReleasedBranchIdIsRejectedNotASilentFree) {
  const int active_mask = (1 << 0) | (1 << 1);
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, 1 + 2 * 1, /*hold_budget=*/1,
                                  active_mask, 0),
            0);

  nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf, nullptr);
  buf->n_samples = 1;
  const int buf_id = buf->buf_id;
  nr_rx_span_pool_publish(&pool, buf); // refcount=2, holders={bit0,bit1}
  ASSERT_EQ(pool.bufs[buf_id].refcount, 2);
  ASSERT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x3);

  // Branch 0 takes and correctly releases its own share first (a normal, legitimate release).
  nr_rx_span_lease_t s0 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s0.span, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 0, s0), 0);
  ASSERT_EQ(pool.bufs[buf_id].refcount, 1);
  ASSERT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x2); // only branch 1's bit remains

  // Branch 1 takes its own (still outstanding) share -- the span is genuinely still alive
  // (refcount=1, not yet freed) -- but then mistakenly tries to release through branch 0's id
  // (already spent) instead of its own.
  nr_rx_span_lease_t s1 = nr_rx_span_pool_take(&pool, 1);
  ASSERT_NE(s1.span, nullptr);
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, s1), -1);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 1) << "wrong-branch release must not have decremented";
  EXPECT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x2) << "branch 1's own bit is still set";

  // The correct release then frees it exactly once.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 1, s1), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);
  EXPECT_EQ(pool.bufs[buf_id].holders, (uint8_t)0);

  // buf_id is back on the free list exactly ONCE (not double-freed by the earlier rejected
  // wrong-branch release plus the correct one). n_buf=3, only this one buffer was ever acquired,
  // so all 3 slots being free with buf_id appearing exactly once is the full, precise check.
  int occurrences = 0;
  for (int i = 0; i < pool.n_free; i++)
    if (pool.free_list[i] == buf_id)
      occurrences++;
  EXPECT_EQ(occurrences, 1);
  EXPECT_EQ(pool.n_free, 3);

  nr_rx_span_pool_destroy(&pool);
}

// ---- Fix round 2, test (c): a lease from an EARLIER, already-released occupant of a recycled
// buf_id must not validate against the CURRENT occupant's lease slot. n_buf=2/hold_budget=1/1
// active branch deterministically reuses the same buf_id on the second acquire() (LIFO free
// list, only one buffer ever in flight), which is what makes "republish same buf_id" exact rather
// than probabilistic.
TEST(SpanPoolRefcount, StaleLeaseFromARepublishedBufferIsRejected) {
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, /*n_buf=*/2, /*hold_budget=*/1,
                                  (1 << 0), 0),
            0);

  nr_rx_span_t *buf1 = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf1, nullptr);
  buf1->n_samples = 1;
  const int buf_id = buf1->buf_id;
  nr_rx_span_pool_publish(&pool, buf1);
  nr_rx_span_lease_t lease1 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(lease1.span, nullptr);
  ASSERT_EQ(lease1.span->buf_id, buf_id);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 0, lease1), 0); // buffer fully released, back on free list

  // Republish: acquire() deterministically returns the SAME buf_id (only one buffer was ever used,
  // LIFO free list), take() mints a NEW key for this fresh occupancy.
  nr_rx_span_t *buf2 = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf2, nullptr);
  ASSERT_EQ(buf2->buf_id, buf_id) << "test relies on deterministic buf_id reuse";
  buf2->n_samples = 1;
  nr_rx_span_pool_publish(&pool, buf2);
  nr_rx_span_lease_t lease2 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(lease2.span, nullptr);
  ASSERT_EQ(lease2.span->buf_id, buf_id);
  EXPECT_GT(lease2.key, lease1.key) << "keys are per-pool monotonic, never repeat";

  // The OLD lease (from the first, already-released occupancy) must be rejected against the
  // buffer's CURRENT (second) occupancy -- it is not merely "some branch's bit", it is a stale key.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, lease1), -1);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 1) << "stale release must not have decremented the live lease";

  // The correct (current) lease releases it normally.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, lease2), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);

  nr_rx_span_pool_destroy(&pool);
}

// ---- G1 test 4 (epoch carry): acq_epoch set on acquire is visible on take, unaffected by a
// later acquire/publish under a different epoch; the consumer (not this module) is the one that
// compares it against a branch's own epoch to decide old/new.
TEST(SpanPoolEpoch, AcqEpochCarriesThroughWithoutOldNewMixing) {
  nr_rx_branch_set_t set;
  ASSERT_EQ(nr_rx_branch_set_parse(&set, "0", "0:0", nullptr), 0);

  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, 1 + 1 * 2, /*hold_budget=*/2,
                                  (1 << 0), 0),
            0);

  nr_rx_span_t *b1 = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(b1, nullptr);
  b1->n_samples = 1;
  b1->acq_epoch = 3;
  nr_rx_span_pool_publish(&pool, b1);

  // Producer moves on to epoch 4 for the next span (simulates an RF discontinuity between the
  // two publishes -- nr_rx_branch_set_rf_discontinuity() is what would bump a real branch's
  // acq_epoch to 4 at this point).
  nr_rx_span_t *b2 = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(b2, nullptr);
  b2->n_samples = 1;
  b2->acq_epoch = 4;
  nr_rx_span_pool_publish(&pool, b2);

  nr_rx_span_lease_t s1 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s1.span, nullptr);
  EXPECT_EQ(s1.span->acq_epoch, 3u); // still 3: publishing epoch-4 b2 did not mutate the already-queued b1
  set.b[0].acq_epoch = 4; // simulate the branch having since moved to epoch 4
  EXPECT_NE(s1.span->acq_epoch, set.b[0].acq_epoch); // consumer's own decision: this span is stale

  nr_rx_span_lease_t s2 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s2.span, nullptr);
  EXPECT_EQ(s2.span->acq_epoch, 4u);
  EXPECT_EQ(s2.span->acq_epoch, set.b[0].acq_epoch); // consumer's own decision: this span is current

  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, s1), 0);
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, s2), 0);
  nr_rx_span_pool_destroy(&pool);
}

// ---- Init-time validation, mirroring nr_rx_branch_test.cc's rejection-path coverage.
TEST(SpanPoolInit, RejectsInvalidArgumentsAndUndersizedPool) {
  nr_rx_span_pool_t pool = {}; // zero-init: see the double-init guard note above
  EXPECT_EQ(nr_rx_span_pool_init(nullptr, 1, 1, 1, 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 0, 1, 1, 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, NR_RX_BRANCH_MAX + 1, 1, 100, 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 0, 1, 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 0, 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 100, 0, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 100, NR_RX_SPAN_POOL_MAX_HOLD + 1, 1, 0), -1);
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 100, 1, 0, 0), -1);               // no active branch
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 100, 1, (1 << NR_RX_BRANCH_MAX), 0), -1); // OOB bit
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, /*n_buf=*/1, /*hold_budget=*/1, ALL4, 0),
            -1); // 1 < 1 + 4*1
  ASSERT_EQ(nr_rx_span_pool_init(&pool, 1, 1, /*n_buf=*/5, /*hold_budget=*/1, ALL4, 0), 0); // exact fit
  EXPECT_EQ(pool.n_active_branches, 4);
  nr_rx_span_pool_destroy(&pool);
}

// ---- Fix round 1 (review MINOR): double-init on a live pool is rejected (-1), not a leak of the
// previous bufs/storage/free_list allocations or a re-init of an in-use mutex.
TEST(SpanPoolInit, RejectsDoubleInitAndLeavesLivePoolIntact) {
  nr_rx_span_pool_t pool = {};
  ASSERT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/2, /*spb=*/4, /*n_buf=*/5, /*hold_budget=*/1,
                                  ALL4, /*sample_rate_hz=*/1000),
            0);
  const int n_buf_before = pool.n_buf;
  const uint64_t rate_before = pool.sample_rate_hz;

  // Re-init on the same, still-live pool must fail and must not disturb it.
  EXPECT_EQ(nr_rx_span_pool_init(&pool, /*n_ch=*/1, /*spb=*/1, /*n_buf=*/3, /*hold_budget=*/1,
                                  (1 << 0), /*sample_rate_hz=*/9999),
            -1);
  EXPECT_EQ(pool.n_buf, n_buf_before);
  EXPECT_EQ(pool.sample_rate_hz, rate_before);

  // The live pool still works normally after the rejected re-init attempt.
  nr_rx_span_t *buf = nr_rx_span_pool_acquire(&pool);
  ASSERT_NE(buf, nullptr);
  nr_rx_span_pool_publish(&pool, buf);
  nr_rx_span_lease_t s = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s.span, nullptr);
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 0, s), 0);

  nr_rx_span_pool_destroy(&pool);

  // After a real destroy(), init() succeeds again (magic was cleared).
  EXPECT_EQ(nr_rx_span_pool_init(&pool, 1, 1, 3, 1, (1 << 0), 0), 0);
  nr_rx_span_pool_destroy(&pool);
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
