#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "nr_pusch_passive_ul_ids.h"
}
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
}

/* U4/UE-ID table: 64 entries; a full table used to drop every later UE (g_ue_ids_dropped). It now recycles the least
 * recently seen RNTI that never passed a CRC first, and a CRC-proven one only when nothing else is left. */
static void dl_ctx(nr_ue_ctx_t *dl, uint16_t rnti)
{
  memset(dl, 0, sizeof(*dl));
  dl->rnti = rnti;
  dl->cfg[NR_UEP_DCI_LEN_DL].value = 40;
  dl->cfg[NR_UEP_DCI_LEN_DL].verif = NR_UEV_TRUSTED;
}
static nr_ue_ul_ticket_t seed(uint16_t rnti)
{
  nr_ue_ctx_t dl;
  dl_ctx(&dl, rnti);
  nr_ue_ul_ticket_t t = {};
  t.rnti = rnti;
  nr_pusch_passive_ul_ids_seed(&t, &dl, 421);
  return t;
}

TEST(UlIdsEvict, FullTableRecyclesTheOldestUnprovenRnti)
{
  nr_pusch_passive_ul_ids_reset_all();
  nr_ue_ul_ticket_t t[64];
  for (int i = 0; i < 64; i++)
    t[i] = seed(0x1000 + i);
  const uint64_t dr0 = nr_pusch_passive_ul_ids_dropped();
  const uint64_t ev0 = nr_pusch_passive_ul_ids_evicted(); /* cumulative counters: compare deltas */
  /* RNTI 0 and 1 are proven; 2 is the oldest unproven; refresh 3 so it is no longer the oldest */
  nr_pusch_passive_ul_ids_confirm(&t[0], 0, 421, 421, 5000, 10);
  nr_pusch_passive_ul_ids_confirm(&t[1], 0, 421, 421, 5000, 10);
  nr_pusch_passive_ul_ids_failed(&t[3]);
  seed(0x2000);
  EXPECT_EQ(nr_pusch_passive_ul_ids_dropped(), dr0);
  EXPECT_EQ(nr_pusch_passive_ul_ids_evicted() - ev0, 1u);
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x2000));
  EXPECT_FALSE(nr_pusch_passive_ul_ids_has(0x1002));
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x1000)); /* proven: kept although least recently seen */
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x1001));
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x1003));
}

TEST(UlIdsEvict, AllProvenEvictsTheLeastRecentlySeen)
{
  nr_pusch_passive_ul_ids_reset_all();
  nr_ue_ul_ticket_t t[64];
  for (int i = 0; i < 64; i++) {
    t[i] = seed(0x1000 + i);
    nr_pusch_passive_ul_ids_confirm(&t[i], 0, 421, 421, 5000, 10 + i);
  }
  nr_pusch_passive_ul_ids_confirm(&t[0], 0, 421, 421, 5000, 99); /* RNTI 0 seen again: 1 is now the oldest */
  const uint64_t ev1 = nr_pusch_passive_ul_ids_evicted();
  seed(0x3000);
  EXPECT_EQ(nr_pusch_passive_ul_ids_evicted() - ev1, 1u);
  EXPECT_FALSE(nr_pusch_passive_ul_ids_has(0x1001));
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x1000));
  EXPECT_TRUE(nr_pusch_passive_ul_ids_has(0x3000));
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
