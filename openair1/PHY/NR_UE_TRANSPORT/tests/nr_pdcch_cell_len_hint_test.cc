// Cell-wide DCI length hint (nr_pdcch_cell_len_hint.h): corroborated lengths outrank a single contradicting lock.
#include <gtest/gtest.h>
#include <cstring>
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_cell_len_hint.h"

namespace {
nr_cell_len_hint_t fresh() { nr_cell_len_hint_t h; memset(&h, 0, sizeof(h)); return h; }
}  // namespace

TEST(CellLenHint, FirstLockIsTheHint)
{
  auto h = fresh();
  nr_cell_len_hint_note(&h, 47);
  EXPECT_EQ(h.len[0], 47);
}

TEST(CellLenHint, OneContradictingLockDoesNotReplaceACorroboratedLength)
{  // i6D1: len 47 locked for two RNTIs, then a single len 49 (false-alarm RNTI)
  auto h = fresh();
  nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 49);
  EXPECT_EQ(h.len[0], 47);
  EXPECT_EQ(h.len[1], 49);
}

TEST(CellLenHint, TieGoesToTheNewerLock)
{
  auto h = fresh();
  nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 49);
  EXPECT_EQ(h.len[0], 49);
}

TEST(CellLenHint, ReconfigurationWinsOnceItIsCorroborated)
{
  auto h = fresh();
  nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 51);
  EXPECT_EQ(h.len[0], 47);
  nr_cell_len_hint_note(&h, 51);
  EXPECT_EQ(h.len[0], 51);
}

TEST(CellLenHint, FullListDropsTheWeakest)
{
  auto h = fresh();
  for (int i = 0; i < 3; i++) nr_cell_len_hint_note(&h, 47);
  nr_cell_len_hint_note(&h, 40);
  nr_cell_len_hint_note(&h, 41);
  nr_cell_len_hint_note(&h, 42);
  nr_cell_len_hint_note(&h, 43);
  EXPECT_EQ(h.len[0], 47);
  EXPECT_EQ(h.len[1], 43);
  EXPECT_EQ(h.cnt[1], 1u);
}
