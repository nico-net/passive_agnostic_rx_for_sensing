#include <gtest/gtest.h>
extern "C" {
#include "nr_passive_job_epoch.h"
}

static uint32_t fake_epoch;
static uint32_t read_fake_epoch(void) { return fake_epoch; }

TEST(PassiveQueueEpoch, OldEpochJobDropped)
{
  fake_epoch = 4;
  const uint32_t stamped = nr_passive_job_epoch_stamp(true, read_fake_epoch);
  EXPECT_EQ(stamped, 4u);
  EXPECT_FALSE(nr_passive_job_epoch_old(true, stamped, read_fake_epoch));
  fake_epoch = 5;
  EXPECT_TRUE(nr_passive_job_epoch_old(true, stamped, read_fake_epoch));
  EXPECT_FALSE(nr_passive_job_epoch_old(false, stamped, read_fake_epoch));
  EXPECT_EQ(nr_passive_job_epoch_stamp(false, read_fake_epoch), 0u);
}
