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

  const nr_rx_span_t *taken[4];
  for (int b = 0; b < 4; b++) {
    taken[b] = nr_rx_span_pool_take(&pool, b);
    ASSERT_NE(taken[b], nullptr);
  }

  for (int b = 0; b < 4; b++) {
    nr_rx_span_view_t v_id = nr_rx_span_for_branch(taken[b], &set_id.b[b]);
    ASSERT_NE(v_id.samples, nullptr);
    EXPECT_EQ(v_id.branch_id, b);
    EXPECT_EQ(v_id.physical_channel, set_id.b[b].physical_channel);
    EXPECT_EQ(v_id.physical_channel, b); // identity map
    for (int n = 0; n < spb; n++) {
      c16_t expect = f(v_id.physical_channel, n);
      EXPECT_EQ(v_id.samples[n].r, expect.r) << "branch " << b << " n " << n;
      EXPECT_EQ(v_id.samples[n].i, expect.i) << "branch " << b << " n " << n;
    }
    nr_rx_span_view_t v_perm = nr_rx_span_for_branch(taken[b], &set_perm.b[b]);
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
      const nr_rx_span_t *s = nr_rx_span_pool_take(&pool, (uint8_t)b);
      ASSERT_NE(s, nullptr) << "branch " << b << " sp " << sp;
      EXPECT_EQ(s->data[0].r, sp) << "branch " << b << " sp " << sp << " -- overwritten buffer";
      EXPECT_EQ(s->absolute_slot, (uint64_t)sp);
      EXPECT_EQ(s->first_sample_ts, 1000u + (uint64_t)sp);
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
    const nr_rx_span_t *s = nr_rx_span_pool_take(&pool, 2);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->data[0].r, expect);
    EXPECT_EQ(nr_rx_span_pool_release(&pool, 2, s), 0);
  }
  EXPECT_EQ(nr_rx_span_pool_take(&pool, 2), nullptr); // ring now empty

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

  const nr_rx_span_t *s0 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s0, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 0, s0), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 1);
  EXPECT_FALSE(in_free_list(buf_id)); // branch 1 still holds it

  const nr_rx_span_t *s1 = nr_rx_span_pool_take(&pool, 1);
  ASSERT_NE(s1, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 1, s1), 0);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);
  EXPECT_TRUE(in_free_list(buf_id));

  // Double release (any branch id) once refcount is already 0 is rejected, not a double free.
  EXPECT_EQ(nr_rx_span_pool_release(&pool, 1, s1), -1);
  EXPECT_EQ(pool.bufs[buf_id].refcount, 0);

  nr_rx_span_pool_destroy(&pool);
}

// ---- Fix round 1 (review IMPORTANT finding): refcount alone is one shared integer and cannot
// tell WHICH branch holds a share, so releasing through the WRONG branch_id must be rejected
// rather than silently spending a different branch's still-needed share.
//
// Structural note on what a 1-bit-per-branch holders mask CAN and CANNOT catch (worth recording,
// not just asserting): the module authenticates a release() call solely by the branch_id
// argument's bit -- it has no separate notion of "which thread/branch actually made this call".
// So if BOTH branches still genuinely hold their share (neither has released yet), a call naming
// branch_id=0 is *indistinguishable* from a real branch-0 release, even if branch 1's code passed
// 0 by mistake -- bit 0 is legitimately set, so that call correctly succeeds and clears branch 0's
// own not-yet-spent share. This is not a gap in the fix: it is what happens for ANY correct
// design gated on a per-branch existence bit rather than a per-take token, and it does not create
// a use-after-free -- the shared refcount still requires exactly n_active_branches distinct bit
// clears before the buffer returns to the free list, so a "mislabeled" release still only ever
// consumes one real outstanding share, never more. What the bit MUST catch, and does, is a
// release naming a branch_id whose share is ALREADY spent (a real double release, or -- the
// scenario below -- releasing through a DIFFERENT branch's id after that branch's own share was
// already legitimately released): that is unambiguous and exactly what silently corrupted
// accounting under the old bare-integer refcount.
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
  const nr_rx_span_t *s0 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s0, nullptr);
  ASSERT_EQ(nr_rx_span_pool_release(&pool, 0, s0), 0);
  ASSERT_EQ(pool.bufs[buf_id].refcount, 1);
  ASSERT_EQ(pool.bufs[buf_id].holders, (uint8_t)0x2); // only branch 1's bit remains

  // Branch 1 takes its own (still outstanding) share -- the span is genuinely still alive
  // (refcount=1, not yet freed) -- but then mistakenly tries to release through branch 0's id
  // (already spent) instead of its own.
  const nr_rx_span_t *s1 = nr_rx_span_pool_take(&pool, 1);
  ASSERT_NE(s1, nullptr);
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

  const nr_rx_span_t *s1 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1->acq_epoch, 3u); // still 3: publishing epoch-4 b2 did not mutate the already-queued b1
  set.b[0].acq_epoch = 4; // simulate the branch having since moved to epoch 4
  EXPECT_NE(s1->acq_epoch, set.b[0].acq_epoch); // consumer's own decision: this span is stale

  const nr_rx_span_t *s2 = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s2, nullptr);
  EXPECT_EQ(s2->acq_epoch, 4u);
  EXPECT_EQ(s2->acq_epoch, set.b[0].acq_epoch); // consumer's own decision: this span is current

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
  const nr_rx_span_t *s = nr_rx_span_pool_take(&pool, 0);
  ASSERT_NE(s, nullptr);
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
