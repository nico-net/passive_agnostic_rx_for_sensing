#include <gtest/gtest.h>
#include <string>
#include "nr_prach_passive_test_bridge.h"

class PassivePrach : public ::testing::Test {
 protected:
  void SetUp() override { test_prach_init(); }
};

TEST_F(PassivePrach, PrachOccasionTable)
{
  test_prach_occasion_t o{};
  ASSERT_TRUE(test_prach_occasion(98, 1, 19, 1, &o));
  EXPECT_GE(o.format, 4);
  EXPECT_GE(o.start_symbol, 0);
  EXPECT_LT(o.start_symbol, 14);
  EXPECT_GT(o.num_occasions, 0);
  EXPECT_FALSE(test_prach_occasion(98, 1, 18, 1, &o));
  EXPECT_FALSE(test_prach_occasion(256, 1, 19, 1, &o));
}

TEST_F(PassivePrach, PrachMatchesRarRapid)
{
  const uint16_t ra_rnti = 1 + 2 + 14 * 5 + 1120;
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, 17), 1);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, 18), 0);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti + 1, 17), -1);
  const uint8_t two_rars[] = {
      0xc1, 0, 0, 0, 0, 0, 0, 1,
      0x51, 0, 0, 0, 0, 0, 0, 2};
  test_prach_rar_entry_t entries[2]{};
  ASSERT_EQ(test_prach_rar_entries(two_rars, sizeof(two_rars), entries, 2), 2);
  EXPECT_EQ(entries[0].rapid, 1);
  EXPECT_EQ(entries[1].rapid, 17);
  EXPECT_EQ(test_prach_rar_relation(10, 5, 2, 1, 17, ra_rnti, entries[1].rapid), 1);
  EXPECT_EQ(test_prach_rar_age(20479, 0, 20), 1);
  EXPECT_EQ(test_prach_rar_age(20479 + 20480, 1, 20), 2);
  EXPECT_EQ(test_prach_rar_age(200, 199, 20), 20479);
  EXPECT_EQ(test_prach_ra_slot(19, 1, 4), 19);
  EXPECT_EQ(test_prach_ra_slot(19, 1, 0), 9);
}

TEST_F(PassivePrach, PrachDetectsSynthPreamble)
{
  const auto zero = test_prach_synth(7, 0);
  EXPECT_EQ(zero.delay, 0);
  const auto found = test_prach_synth(7, 8);
  EXPECT_EQ(found.preamble, 0);
  EXPECT_NEAR(found.delay, 4, 1);
  EXPECT_GT(found.energy, 0);
}

TEST_F(PassivePrach, PrachRsRecordFields)
{
  char line[768]{};
  test_prach_json(1, line, sizeof(line));
  const std::string matched(line);
  EXPECT_NE(matched.find("\"schema\":2,\"type\":\"rs\",\"source\":\"prach\""), std::string::npos);
  EXPECT_NE(matched.find("\"abs_slot\":1234"), std::string::npos);
  EXPECT_NE(matched.find("\"epoch\":4,\"identity_gen\":2,\"dedicated_epoch\":3"), std::string::npos);
  EXPECT_NE(matched.find("\"preamble\":17,\"energy\":420,\"delay_samples\":32"), std::string::npos);
  EXPECT_NE(matched.find("\"occasion\":{\"frame\":61,\"slot\":14,\"fdm_index\":1,\"symbol\":2}"), std::string::npos);
  EXPECT_NE(matched.find("\"rar_match\":true,\"rar_rapid\":17,\"rar_ta\":20"), std::string::npos);
  test_prach_json(0, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":false"), std::string::npos);
  test_prach_json(-1, line, sizeof(line));
  EXPECT_NE(std::string(line).find("\"rar_match\":null,\"rar_rapid\":null,\"rar_ta\":null"), std::string::npos);
}
