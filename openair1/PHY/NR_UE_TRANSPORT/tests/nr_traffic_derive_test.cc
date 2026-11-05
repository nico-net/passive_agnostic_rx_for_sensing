/* E7: derived DCI fields (nl, qm, tbs). Expected TBS values were computed by hand from TS 38.214 5.1.3.2 with a separate implementation of the
 * procedure and Table 5.1.3.2-1, not by calling nr_compute_tbs. */
#include "nr_traffic_derive.h"
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdsch_solver.h"
#include <gtest/gtest.h>
#include "common/utils/LOG/log.h"

static int nl_of(int bits, int type, unsigned f, int *cdm = nullptr, int *nsym = nullptr)
{
  uint8_t c; uint16_t p; int ns;
  if (!nr_pdcch_blind_dl_ap_lookup(bits, type, f, &c, &p, &ns)) return -1;
  if (cdm) *cdm = c;
  if (nsym) *nsym = ns;
  return __builtin_popcount(p);
}

TEST(TrafficDerive, AntennaPortTablesRowCounts)
{
  const struct { int bits, type, rows; } t[] = {{4, 0, 12}, {5, 0, 31}, {5, 1, 24}, {6, 1, 58}};
  for (auto &x : t) {
    for (int f = 0; f < x.rows; f++) EXPECT_GE(nl_of(x.bits, x.type, f), 1) << x.bits << " " << x.type << " " << f;
    EXPECT_EQ(nl_of(x.bits, x.type, x.rows), -1) << "first codepoint beyond the table";
    EXPECT_EQ(nl_of(x.bits, x.type, 63), -1);
  }
  /* pairs that name no table */
  EXPECT_EQ(nl_of(4, 1, 0), -1); EXPECT_EQ(nl_of(6, 0, 0), -1); EXPECT_EQ(nl_of(3, 0, 0), -1); EXPECT_EQ(nl_of(7, 1, 0), -1);
}

TEST(TrafficDerive, AntennaPortTableValuesTS38212)
{
  int cdm, ns;
  /* Table 7.3.1.2.2-1 (type 1, maxLength 1) */
  EXPECT_EQ(nl_of(4, 0, 0, &cdm), 1); EXPECT_EQ(cdm, 1);
  EXPECT_EQ(nl_of(4, 0, 2, &cdm), 2); EXPECT_EQ(cdm, 1);
  EXPECT_EQ(nl_of(4, 0, 3, &cdm), 1); EXPECT_EQ(cdm, 2);
  EXPECT_EQ(nl_of(4, 0, 9, &cdm), 3); EXPECT_EQ(cdm, 2);
  EXPECT_EQ(nl_of(4, 0, 10, &cdm), 4); EXPECT_EQ(cdm, 2);
  EXPECT_EQ(nl_of(4, 0, 11, &cdm), 2); EXPECT_EQ(cdm, 2);
  /* Table -2 (type 1, maxLength 2): 0..11 as -1 with one symbol, 12.. double symbol */
  EXPECT_EQ(nl_of(5, 0, 10, &cdm, &ns), 4); EXPECT_EQ(ns, 1);
  EXPECT_EQ(nl_of(5, 0, 12, &cdm, &ns), 1); EXPECT_EQ(ns, 2);
  EXPECT_EQ(nl_of(5, 0, 26, &cdm, &ns), 3); EXPECT_EQ(ns, 2);   /* ports 0,1,5 */
  EXPECT_EQ(nl_of(5, 0, 28, &cdm, &ns), 4); EXPECT_EQ(ns, 2);
  EXPECT_EQ(nl_of(5, 0, 30, &cdm, &ns), 4); EXPECT_EQ(ns, 2);
  /* Table -3 (type 2, maxLength 1) */
  EXPECT_EQ(nl_of(5, 1, 11, &cdm), 1); EXPECT_EQ(cdm, 3);
  EXPECT_EQ(nl_of(5, 1, 20, &cdm), 3); EXPECT_EQ(cdm, 3);
  EXPECT_EQ(nl_of(5, 1, 23, &cdm), 2); EXPECT_EQ(cdm, 3);
  /* Table -4 (type 2, maxLength 2) */
  EXPECT_EQ(nl_of(6, 1, 23, &cdm, &ns), 2); EXPECT_EQ(cdm, 2); EXPECT_EQ(ns, 1);
  EXPECT_EQ(nl_of(6, 1, 24, &cdm, &ns), 1); EXPECT_EQ(cdm, 3); EXPECT_EQ(ns, 2);
  EXPECT_EQ(nl_of(6, 1, 45, &cdm, &ns), 4); EXPECT_EQ(ns, 2);
  EXPECT_EQ(nl_of(6, 1, 57, &cdm, &ns), 2); EXPECT_EQ(cdm, 2); EXPECT_EQ(ns, 2);
}

TEST(TrafficDerive, WrongTableSelectionChangesTheAnswer)
{
  /* the same 5-bit codepoint means different things under type 1 (Table -2) and type 2 (Table -3): a mutated selection is caught here */
  EXPECT_EQ(nl_of(5, 0, 20), 2); EXPECT_EQ(nl_of(5, 1, 20), 3);
  EXPECT_EQ(nl_of(5, 0, 11), 2); EXPECT_EQ(nl_of(5, 1, 11), 1);
  EXPECT_EQ(nl_of(5, 0, 24), 2); EXPECT_EQ(nl_of(5, 1, 24), -1);   /* beyond Table -3 */
}

TEST(TrafficDerive, McsTables)
{
  int qm; uint32_t r;
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 0, 0, &qm, &r));  EXPECT_EQ(qm, 2); EXPECT_EQ(r, 1200u);
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 0, 28, &qm, &r)); EXPECT_EQ(qm, 6); EXPECT_EQ(r, 9480u);
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 1, 27, &qm, &r)); EXPECT_EQ(qm, 8); EXPECT_EQ(r, 9480u);
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 2, 28, &qm, &r)); EXPECT_EQ(qm, 6); EXPECT_EQ(r, 7720u);
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 2, 0, &qm, &r));  EXPECT_EQ(qm, 2); EXPECT_EQ(r, 300u);
  /* reserved indices (retransmission codepoints) have no value of their own */
  for (int m = 29; m <= 31; m++) EXPECT_FALSE(nr_traffic_mcs_qr(0, 0, m, &qm, &r)) << m;
  for (int m = 28; m <= 31; m++) EXPECT_FALSE(nr_traffic_mcs_qr(0, 1, m, &qm, &r)) << m;
  for (int m = 29; m <= 31; m++) EXPECT_FALSE(nr_traffic_mcs_qr(0, 2, m, &qm, &r)) << m;
  EXPECT_FALSE(nr_traffic_mcs_qr(0, 3, 0, &qm, &r)); EXPECT_FALSE(nr_traffic_mcs_qr(0, -1, 0, &qm, &r));
  EXPECT_FALSE(nr_traffic_mcs_qr(0, 0, 32, &qm, &r)); EXPECT_FALSE(nr_traffic_mcs_qr(0, 0, -1, &qm, &r));
  EXPECT_TRUE(nr_traffic_mcs_qr(1, 4, 0, &qm, &r));   /* UL transform-precoding table */
  EXPECT_FALSE(nr_traffic_mcs_qr(1, 6, 0, &qm, &r));
  /* the same index under another table gives another modulation order: wrong table selection is visible */
  int q0, q1; uint32_t r0, r1;
  ASSERT_TRUE(nr_traffic_mcs_qr(0, 0, 20, &q0, &r0)); ASSERT_TRUE(nr_traffic_mcs_qr(0, 1, 20, &q1, &r1));
  EXPECT_NE(q0, q1);
}

TEST(TrafficDerive, McsAgreesWithTheSolverTables)
{
  for (int t = 0; t < 3; t++)
    for (int m = 0; m < 32; m++) {
      int qm = 0; uint32_t r = 0; int sq = 0, sr = 0;
      const bool a = nr_traffic_mcs_qr(0, t, m, &qm, &r), b = nr_pdsch_solver_mcs_qr(t, m, &sq, &sr);
      ASSERT_EQ(a, b) << t << " " << m;
      if (a) { EXPECT_EQ(qm, sq); EXPECT_LE(std::abs((int)r - sr * 5), 5) << t << " " << m; }
    }
}

struct TbsCase { int qm; uint32_t r; int rb, sym, type, cdm, dsyms, xoh, nl; uint32_t tbs; };
TEST(TrafficDerive, TbsTS38214Hand)
{
  const TbsCase c[] = {
    {2, 1200, 1, 14, 0, 2, 1, 0, 1, 32},        /* Ninfo 36.6 -> n = 3 -> 32 */
    {2, 1200, 10, 12, 0, 2, 1, 0, 1, 304},      /* Ninfo 309.4 */
    {4, 6580, 20, 12, 0, 2, 1, 0, 1, 6784},     /* Ninfo 6785.6: above 3824, R > 1/4, Np 6784 <= 8424 */
    {6, 9480, 106, 12, 0, 1, 1, 0, 1, 81976},   /* Np > 8424: code block count 10 */
    {6, 9480, 106, 12, 0, 1, 1, 18, 2, 139376}, /* xOverhead 18, 2 layers */
    {8, 9480, 106, 12, 0, 1, 2, 0, 4, 417976},  /* 256QAM, 4 layers, two DM-RS symbols */
    {2, 1200, 106, 12, 1, 3, 1, 6, 1, 3104},    /* type 2, 3 CDM groups, quantised through Table 5.1.3.2-1 */
    {6, 4380, 51, 14, 0, 2, 2, 12, 1, 17424},
    {2, 1200, 52, 13, 0, 2, 1, 0, 1, 1800},
    {4, 3400, 273, 13, 1, 2, 2, 0, 2, 102416},
    {6, 4380, 24, 14, 0, 2, 1, 0, 1, 9480},
    {2, 1200, 3, 5, 0, 1, 1, 0, 1, 32},
    {2, 1200, 52, 14, 0, 1, 1, 0, 1, 1928},     /* N'RE capped at 156 */
    {2, 1200, 106, 14, 0, 2, 1, 0, 1, 3848},    /* Ninfo 3875.6 just above 3824, R <= 1/4 branch */
    {4, 6580, 40, 12, 0, 2, 1, 0, 1, 13576},
    {4, 6580, 13, 12, 0, 2, 1, 0, 1, 4352},
    {4, 6580, 14, 12, 0, 2, 1, 0, 1, 4736},
    {6, 4380, 30, 12, 0, 2, 1, 0, 1, 10248},
  };
  for (const auto &x : c)
    EXPECT_EQ(nr_traffic_tbs(x.qm, x.r, x.rb, x.sym, x.type, x.cdm, x.dsyms, x.xoh, x.nl), x.tbs)
        << "qm " << x.qm << " r " << x.r << " rb " << x.rb << " sym " << x.sym;
}

TEST(TrafficDerive, TbsUnprovenInputsAreZero)
{
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, -1, 1), 0u) << "xOverhead not proven";
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, 7, 1), 0u) << "xOverhead not a legal value";
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, 0, 5), 0u) << "two codewords";
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, 0, 0), 0u);
  EXPECT_EQ(nr_traffic_tbs(0, 9480, 106, 12, 0, 1, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 0, 106, 12, 0, 1, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 0, 12, 0, 1, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 0, 0, 1, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 0, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 2, 1, 1, 0, 1), 0u);
  EXPECT_EQ(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 0, 0, 1), 0u);
  /* xOverhead matters: a mutated (unproven) value would print a different TBS */
  EXPECT_NE(nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, 0, 1), nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 1, 18, 1));
}

/* a fully proven CRC-solved DCI 1_1: 5-bit type-1 layout (Table -2), codepoint 2 (ports 0,1, CDM 1), MCS 28 of table 0, 106 PRB, S 2, L 12, add_pos 1 */
static nr_traffic_dl_in_t proven()
{
  nr_traffic_dl_in_t d{};
  d.layout_trusted = true; d.layout_crc = true; d.ap_bits = 5; d.dmrs_type = 0; d.ap_field = 2; d.rd_cdm = 1; d.rd_ports = 0x3;
  d.mcs = 28; d.mcs_table = 0; d.table_evidence = 2; d.sv_ok = d.sv_crc = true; d.sv_ov = false; d.sv_add_pos = 1; d.sv_table = 0; d.sv_S = 2; d.sv_L = 12;
  d.nb_rb = 106; d.S = 2; d.L = 12; d.dmrs_mask = (1u << 2) | (1u << 9); d.expect_mask = d.dmrs_mask; d.xoh = 0; d.k0 = 0; d.slot_dl_syms = 14; d.class_c = true;
  return d;
}
static nr_traffic_derived_t run(const nr_traffic_dl_in_t &d) { nr_traffic_derived_t o; nr_traffic_derive_dl(&d, &o); return o; }

TEST(TrafficDerive, ProvenRecordDerivesAll)
{
  const auto o = run(proven());
  EXPECT_EQ(o.nl, 2); EXPECT_EQ(o.qm, 6);
  EXPECT_EQ(o.tbs, (int32_t)nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 2, 0, 2));
  EXPECT_GT(o.tbs, 0);
}

TEST(TrafficDerive, EachUnprovenInputNullsItsFields)
{
  auto expect = [](nr_traffic_dl_in_t d, int nl, int qm, bool tbs, const char *why) {
    const auto o = run(d);
    EXPECT_EQ(o.nl, nl) << why; EXPECT_EQ(o.qm, qm) << why; EXPECT_EQ(o.tbs > 0, tbs) << why;
  };
  auto d = proven();
  d.layout_trusted = false; expect(d, -1, -1, false, "untrusted layout");
  d = proven(); d.ap_field = -1; expect(d, -1, 6, false, "no codepoint: no nl, hence no tbs");
  d = proven(); d.rd_ports = 0x1; expect(d, -1, 6, false, "reading disagrees with the layout's table");
  d = proven(); d.rd_cdm = 2; expect(d, -1, 6, false, "cdm disagrees");
  d = proven(); d.dmrs_type = 1; d.ap_field = 11; expect(d, -1, 6, false, "wrong table (5 bits, type 2) disagrees with the reading");
  d = proven(); d.ap_field = 31; expect(d, -1, 6, false, "codepoint beyond the table");
  d = proven(); d.table_evidence = 0; expect(d, 2, -1, false, "table unproven");
  d = proven(); d.sv_ok = false; expect(d, 2, -1, false, "no solver state");
  d = proven(); d.sv_table = 1; expect(d, 2, -1, false, "solver table disagrees with the reading's");
  d = proven(); d.mcs = 29; expect(d, 2, -1, false, "reserved MCS (table 0)");
  d = proven(); d.mcs_table = 1; d.sv_table = 1; d.mcs = 28; expect(d, 2, -1, false, "reserved MCS (table 1)");
  d = proven(); d.mcs_table = 2; d.sv_table = 2; d.mcs = 30; expect(d, 2, -1, false, "reserved MCS (table 2)");
  d = proven(); d.layout_crc = false; d.table_evidence = 1; expect(d, 2, -1, false, "PHYS layout needs Qm consensus");
  d = proven(); d.layout_crc = false; d.sv_crc = false; expect(d, 2, 6, false, "PHYS layout: qm with consensus, never tbs");
  d = proven(); d.sv_crc = false; expect(d, 2, 6, false, "solver says no full TB passed");
  d = proven(); d.sv_ov = true; expect(d, 2, 6, false, "special-slot override on file");
  d = proven(); d.class_c = false; expect(d, 2, 6, false, "not a C-RNTI grant");
  d = proven(); d.sv_S = -1; expect(d, 2, 6, false, "S unknown for this TDA index");
  d = proven(); d.S = 3; expect(d, 2, 6, false, "S disagrees with the learnt one");
  d = proven(); d.L = 11; expect(d, 2, 6, false, "L disagrees with the learnt one");
  d = proven(); d.xoh = -1; expect(d, 2, 6, false, "xOverhead not proven");
  d = proven(); d.slot_dl_syms = 9; expect(d, 2, 6, false, "TDD special slot");
  d = proven(); d.slot_dl_syms = 0; expect(d, 2, 6, false, "UL slot");
  d = proven(); d.slot_dl_syms = -1; expect(d, 2, 6, false, "TDD pattern unknown");
  d = proven(); d.k0 = -1; expect(d, 2, 6, false, "PDSCH slot unknown");
  d = proven(); d.k0 = 2; expect(d, 2, 6, true, "known offset, full DL slot");
  d = proven(); d.expect_mask = 0; expect(d, 2, 6, false, "mask not cross-checked");
  d = proven(); d.expect_mask = (1u << 2) | (1u << 8); expect(d, 2, 6, false, "mask disagrees with the learnt add_pos");
  d = proven(); d.ap_field = 12; d.rd_ports = 0x1; d.rd_cdm = 2; expect(d, 1, 6, false, "double-symbol DM-RS row: nl and qm, no tbs");
  d = proven(); d.expect_mask = d.dmrs_mask = (1u << 2) | (1u << 6) | (1u << 10); d.nb_rb = 106; (void)d;
  const auto ok = run(d);
  EXPECT_GT(ok.tbs, 0) << "three DM-RS symbols are counted";
  EXPECT_EQ(ok.tbs, (int32_t)nr_traffic_tbs(6, 9480, 106, 12, 0, 1, 3, 0, 2));
}

TEST(TrafficDerive, MutationUnprovenXoverheadWouldChangeTbs)
{
  auto d = proven(); d.xoh = 18;
  const auto a = run(d); d.xoh = 0; const auto b = run(d);
  EXPECT_NE(a.tbs, b.tbs);
  d.xoh = -1; EXPECT_EQ(run(d).tbs, -1);
}

TEST(TrafficDerive, AllLayoutTablesGiveNlTrustedOnly)
{
  /* every (width, type) pair with its first and last codepoint reaches nl through the lookup, with the reading it implies */
  const struct { int bits, type, last; } t[] = {{4, 0, 11}, {5, 0, 30}, {5, 1, 23}, {6, 1, 57}};
  for (auto &x : t)
    for (int f : {0, x.last}) {
      uint8_t cdm; uint16_t ports; int ns;
      ASSERT_TRUE(nr_pdcch_blind_dl_ap_lookup(x.bits, x.type, f, &cdm, &ports, &ns));
      nr_traffic_dl_in_t d{};
      d.layout_trusted = true; d.ap_bits = x.bits; d.dmrs_type = x.type; d.ap_field = f; d.rd_cdm = cdm; d.rd_ports = ports;
      EXPECT_EQ(run(d).nl, __builtin_popcount(ports));
      d.layout_trusted = false; EXPECT_EQ(run(d).nl, -1);
    }
}

extern "C" {
void *uniqCfg = nullptr;
unsigned int *nr_gold_pdcch(int, int, unsigned short, int, int) { return nullptr; }
void nr_pdcch_dmrs_ref(const unsigned int *, void *, unsigned short) {}
void *get_softmodem_params(void) { static char z[65536]; return z; }
void exit_function(const char *, const char *, int, const char *s, int) { fprintf(stderr, "%s\n", s ? s : ""); abort(); }
}
int main(int argc, char **argv)
{
  logInit();   /* nr_compute_tbs logs */
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
