/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
/* Work bounds, independent of budget enablement. Two snapshots retain the
 * cooperative sweep tile. Secondary Gold work must never launch all 65536
 * survivors together (noise/degenerate inputs can fill the screen). */
enum { NR_NID_SWEEP_SNAPSHOTS = 2, NR_NID_SECONDARY_IDS = 256,
       NR_NID_SCORE_HYPOTHESES = 32, NR_NID_SCORE_SNAPSHOTS = 32 };
/* nr_nid_chunk_count: number of items in the chunk that starts at `first` out of `total`, capped at `limit` (0 when first is past the end). Inline. Caller: tests/nr_gpu_budget_test.cc (the chunk split is checked there). */
static inline int nr_nid_chunk_count(int total, int first, int limit)
{
  return first < total ? (total - first < limit ? total - first : limit) : 0;
}
