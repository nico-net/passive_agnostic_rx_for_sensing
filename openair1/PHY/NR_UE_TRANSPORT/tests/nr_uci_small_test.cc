#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "nr_pusch_uci_blind.h"
}

/* U3: UCI search for TBs with a 16-bit CRC. The oracle "decodes" iff the UL-SCH stream has the length of the true UCI footprint
 * (that is what a right hypothesis gives; the LDPC/CRC itself is covered by the pusch_ucisim bed). */
struct Oracle {
  uint32_t pass_len;  /* UL-SCH stream length that passes; 0: none */
  unsigned calls;
  uint32_t pass_len2; /* second length passing (a chance pass) */
};
static bool tb(void *o, const int16_t *, uint32_t g_ulsch)
{
  Oracle *p = (Oracle *)o;
  p->calls++;
  return g_ulsch == p->pass_len || (p->pass_len2 && g_ulsch == p->pass_len2);
}

static nr_uci_geom_t geom(int rb)
{
  nr_uci_geom_t g = {};
  g.rb = (uint16_t)rb;
  g.start_sym = 0;
  g.nsym = 14;
  g.dmrs_pos = 1u << 2;
  g.data_re_dmrs_rb = 0; /* type 1, 2 CDM groups without data */
  g.qm = 2;
  g.nl = 1;
  return g;
}

/* the stream length hypothesis (o_ack, b_ack) leaves on this geometry */
static uint32_t len_of(const nr_uci_geom_t &g0, unsigned o_ack, unsigned b_ack)
{
  nr_uci_geom_t g = g0;
  g.sum_k = 2560;
  nr_uci_hyp_t h = {};
  h.o_ack = (uint8_t)o_ack;
  h.b_ack = (uint8_t)b_ack;
  h.alpha = 3;
  nr_uci_counts_t c;
  EXPECT_TRUE(nr_uci_counts(&g, &h, &c));
  return (nr_uci_data_re(&g) - c.u_re) * g.qm * g.nl;
}

static nr_uci_search_out_t run(uint16_t rnti, int rb, Oracle *o, bool explore = true)
{
  nr_uci_geom_t g = geom(rb);
  const uint32_t G = nr_uci_data_re(&g) * g.qm;
  static int16_t llr[4096 * 8], work[4096 * 8];
  memset(llr, 1, sizeof(llr));
  nr_uci_search_in_t in = {};
  in.rnti = rnti;
  in.geom = g;
  in.tbs_bits = 400;
  in.bg = 2;
  in.dai = -1;
  in.llr = llr;
  in.G = G;
  in.work = work;
  in.tb = tb;
  in.opaque = o;
  in.explore = explore;
  in.budget_us = 2000000;
  nr_uci_search_out_t out;
  nr_pusch_uci_blind_search(&in, &out);
  return out;
}

TEST(UciSmall, FirstPassIsHeldSecondGrantDelivers)
{
  nr_pusch_uci_blind_forget(0x4601);
  /* the UE multiplexes 4 HARQ-ACK bits at beta index 9 */
  Oracle a = {len_of(geom(6), 4, 9), 0, 0};
  nr_uci_search_out_t o1 = run(0x4601, 6, &a);
  EXPECT_FALSE(o1.ok) << "a CRC16 pass under an unseen hypothesis must not be delivered";
  EXPECT_TRUE(o1.provisional);
  EXPECT_GT(o1.tb_tests, 0u);
  /* the second grant of the RNTI, another width and O (3 bits), same beta: consistent -> delivered */
  Oracle b = {len_of(geom(10), 3, 9), 0, 0};
  nr_uci_search_out_t o2 = run(0x4601, 10, &b);
  EXPECT_TRUE(o2.ok);
  EXPECT_FALSE(o2.provisional);
  EXPECT_GT(o2.h.o_ack, 2);
}

TEST(UciSmall, ChancePassOfOneGrantIsNeverDelivered)
{
  nr_pusch_uci_blind_forget(0x4602);
  /* a single chance pass at a footprint no other grant of the RNTI repeats */
  Oracle a = {len_of(geom(6), 7, 4), 0, 0};
  EXPECT_FALSE(run(0x4602, 6, &a).ok);
  /* the next grants are plain failures: nothing passes, the held pass is not promoted by time or count */
  Oracle none = {0, 0, 0};
  for (int i = 0; i < 20; i++)
    EXPECT_FALSE(run(0x4602, 6 + i % 3, &none).ok);
}

TEST(UciSmall, ConfirmationIsPerRnti)
{
  nr_pusch_uci_blind_forget(0x4603);
  nr_pusch_uci_blind_forget(0x4604);
  Oracle a = {len_of(geom(6), 4, 9), 0, 0};
  Oracle b = {len_of(geom(10), 4, 9), 0, 0};
  EXPECT_FALSE(run(0x4603, 6, &a).ok);
  EXPECT_TRUE(run(0x4604, 10, &b).provisional) << "another RNTI's pass does not confirm, it is held for its own RNTI";
  EXPECT_TRUE(run(0x4603, 10, &b).ok) << "the RNTI's own second grant confirms";
}

TEST(UciSmall, PunctClassUsesTheReservedRes)
{
  nr_pusch_uci_blind_forget(0x4605);
  Oracle a = {len_of(geom(6), 2, 0), 0, 0}; /* HARQ-ACK <= 2 bits: the UL-SCH keeps its length (reserved REs are punctured) */
  Oracle c = {len_of(geom(8), 2, 0), 0, 0};
  EXPECT_FALSE(run(0x4605, 6, &a).ok);
  EXPECT_TRUE(run(0x4605, 6, &a).ok);
  EXPECT_TRUE(run(0x4605, 8, &c).ok) << "after the confirmation the class is delivered at once";
}

TEST(UciSmall, NoExplorationNoDefaultSet)
{
  nr_pusch_uci_blind_forget(0x4606);
  Oracle a = {len_of(geom(6), 4, 9), 0, 0};
  nr_uci_search_out_t o = run(0x4606, 6, &a, false);
  EXPECT_FALSE(o.ok);
  EXPECT_EQ(o.tb_tests, 0u) << "the default set runs only when the caller's rate limiter allows it";
}

TEST(UciSmall, SearchIsBounded)
{
  nr_pusch_uci_blind_forget(0x4607);
  Oracle none = {0, 0, 0};
  nr_uci_search_out_t o = run(0x4607, 20, &none);
  EXPECT_FALSE(o.ok);
  EXPECT_LE(o.tb_tests, 1u + 6u * 16u);
}
