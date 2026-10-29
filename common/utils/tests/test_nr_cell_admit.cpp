// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Unit tests of the cell admission model (B3). Hosts are SYNTHETIC FIXTURES (never inputs of the model), each labelled:
//   sens6   x86, RTX 4060 Ti 8188 MiB DISCRETE (7800 MiB free at probe = fixture), MemAvailable 9 GiB [MEASURED, sens6 snapshot, SENS6-READINESS-20261021 s3],
//           shared host bytes 82 MiB pinned [MEASURED, HOSTMEM-273-20261024 s4] + 937 MiB USRP ring [same report, s2]
//   dgx     DGX GB10 unified 128 GB (SHARED), MemAvailable 100 GiB [fixture; MPCI-DASH-REVIEW O5: 110 GB free], max_cells 4 [fixture for nr_cl_max_cells]
//   cloud4  4-cpu x86, no GPU, 16 GiB, max_cells 1 [fixture]
// Which cell is unserved on sens6 is a [CODE-READ estimate] until G1 supplies the measured per-cell GPU bytes.
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" {
#include "nr_cell_admit.h"
#include "PHY/TOOLS/nr_host_membudget.h"
#include "PHY/TOOLS/nr_gpu_membudget.h"
#include "PHY/TOOLS/nr_gpu_platform.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc_mem.h"
}
#define MIB ((size_t)1 << 20)

static nr_host_caps_t base_caps()
{
  nr_host_caps_t c;
  memset(&c, 0, sizeof c);
  c.mem_avail = SIZE_MAX;
  c.max_cells = NR_ADMIT_UNLIMITED_CELLS;
  c.chan_budget_msps = -1;
  c.consumers = 4;
  c.depth = 64;
  c.csirs_enabled = c.recovery_enabled = c.gw_enabled = 1;
  return c;
}
static nr_host_caps_t sens6()
{
  nr_host_caps_t c = base_caps();
  c.mem_avail = 9216 * MIB;
  c.host_shared_bytes = (82 + 937) * MIB;
  c.gpu_tier = NR_GPU_DISCRETE;
  c.gpu_free_bytes = 7800 * MIB;
  return c;
}
static nr_host_caps_t dgx()
{
  nr_host_caps_t c = base_caps();
  c.mem_avail = 102400 * MIB;
  c.host_shared_bytes = 1100 * MIB;
  c.gpu_tier = NR_GPU_SHARED;
  c.unified = 1;
  c.gpu_free_bytes = 102400 * MIB;
  c.max_cells = 4;
  return c;
}
static nr_host_caps_t cloud4()
{
  nr_host_caps_t c = base_caps();
  c.mem_avail = 16384 * MIB;
  c.max_cells = 1;
  return c;
}
static std::vector<nr_cell_desc_t> cells(int n, int rb, int rx = 4)
{
  std::vector<nr_cell_desc_t> v(n);
  for (auto &c : v) { nr_cell_desc_init(&c); c.n_rb = rb; c.scs_khz = 30; c.n_rx = rx; }
  return v;
}
static nr_admit_plan_t plan_of(const nr_host_caps_t &c, const std::vector<nr_cell_desc_t> &v, int *ns = nullptr)
{
  static nr_admit_plan_t p;
  nr_admit_plan_t out;
  const int r = nr_cell_admit_plan(&c, v.data(), (int)v.size(), &out);
  if (ns) *ns = r;
  p = out;
  return p;
}

TEST(NrCellAdmit, TierValuesFollowThePlatformEnum)
{
  EXPECT_EQ(0, (int)NR_GPU_NONE);
  EXPECT_EQ(1, (int)NR_GPU_SHARED);
  EXPECT_EQ(2, (int)NR_GPU_DISCRETE);
}

// ---- N = 1: the plan IS today's sizing. Replay of the real nr_host_membudget state API on the receiver's start-up sequence. ----
struct Fp { int fft, mu; size_t spf, rxdf, stride; };
static Fp fp_of(int rb)  // 30 kHz grids: fft from the sample-rate table [CODE nr_common.c get_samplerate_and_bw], prefixes [CODE nr_parms.c:119-120]
{
  Fp f;
  f.fft = rb <= 52 ? 1024 : rb <= 162 ? 2048 : 4096;
  f.mu = 1;
  f.spf = (size_t)f.fft * 30000 / 100;
  f.rxdf = (size_t)14 * f.fft;
  f.stride = (size_t)14 * (f.fft + f.fft / 128 * 9 + f.fft / 128 * (9 + (1 << f.mu)));
  return f;
}
static size_t td_bytes(const Fp &f, int rx, int cap)
{
  const size_t slot = ((f.stride * rx * 4 + 63) / 64) * 64, huge = 2 * MIB;
  return ((slot * (cap + 100) + huge - 1) / huge) * huge + 2 * (size_t)100 * rx * 14 * f.fft * 4;
}
static size_t gw_unit(int rb)
{
  size_t c = 4096, bytes = (size_t)rb * 12 * 14 * 4 * 8 * 2;
  while (c < bytes) c <<= 1;
  return c + 64;
}

TEST(NrCellAdmit, OneCellHostEqualsMembudgetReplay)
{
  int compared = 0, shrunk = 0;
  const size_t avails[] = {1500, 2500, 3000, 4000, 6000, 9216, 16000, 102400};
  for (size_t a : avails)
    for (int rb : {51, 106, 217, 273})
      for (int rx : {4, 2})
        for (size_t shared : {(size_t)0, (size_t)1019 * MIB}) {
          char buf[32];
          snprintf(buf, sizeof buf, "%zu", a);
          setenv("NR_HOST_MEM_BUDGET_MB", buf, 1);
          nr_host_membudget_test_reset();
          const Fp f = fp_of(rb);
          const size_t iq = 2 * f.spf * 4 * rx, q = 4 * ((size_t)rx * f.rxdf * 4 + iq), ps = (size_t)rx * 14 * 24 * rb * 2;
          // the receiver's sequence (nr_pdsch_passive_queue.c:2355-2359, nr_csirs_disc_feed.c:508-543, nr_pdcch_blind_monitor_rt.c:9962-9983)
          if (shared) nr_host_membudget_commit(NR_HOST_MEM_PINNED, shared, 1.0);
          nr_host_membudget_commit(NR_HOST_MEM_QUEUE, q, 1.0);
          const long snap = nr_host_membudget_size(NR_HOST_MEM_SNAP, 10, 0, iq, 2, 8);
          const long cs = nr_host_membudget_size(NR_HOST_MEM_CSIRS, 30, 0, ps, 160, 640);
          long td = 0;
          if (cs) {
            const size_t b1 = td_bytes(f, rx, 1), per = (td_bytes(f, rx, 1025) - b1) / 1024, f0 = b1 > per ? b1 - per : 0;
            td = nr_host_membudget_size(NR_HOST_MEM_TD, 60, f0 + (2u << 20), per, 256, 1024);
            if (td) nr_host_membudget_commit(NR_HOST_MEM_TD, td_bytes(f, rx, (int)td), 1.0);
          }
          const size_t share = nr_host_membudget_share(NR_HOST_MEM_GW_POOL), unit = gw_unit(rb), full = 4 * 64 * 16;
          const long gw = nr_host_membudget_fit(share, 0, unit, 32, (long)full);
          nr_host_membudget_commit(NR_HOST_MEM_GW_POOL, gw * unit, 1.0);

          nr_host_caps_t c = base_caps();
          c.mem_avail = a * MIB;
          c.host_shared_bytes = shared;
          auto v = cells(1, rb, rx);
          int ns;
          const nr_admit_plan_t p = plan_of(c, v, &ns);
          char why[256];
          SCOPED_TRACE(testing::Message() << "avail_mb=" << a << " rb=" << rb << " rx=" << rx << " shared=" << shared);
          const int real_refuse = nr_host_membudget_over_hard(why, sizeof why);
          EXPECT_EQ(real_refuse != 0, p.refuse != 0);  // today's HOST_MEM REFUSE rule, unchanged
          EXPECT_EQ(real_refuse ? 0 : 1, ns);
          if (real_refuse) continue;
          compared++;
          if (snap < 8 || cs < 640 || td < 1024 || gw < (long)full) shrunk++;
          EXPECT_EQ(snap, p.cell[0].snap_holders);
          EXPECT_EQ(cs, p.cell[0].csirs_slots);
          EXPECT_EQ(td, p.cell[0].td_slots);
          EXPECT_EQ((long)gw, (long)p.cell[0].gw_buffers);
          EXPECT_EQ(nr_host_membudget_committed(), p.host_used);
          EXPECT_EQ(nr_host_membudget_total(), p.host_budget);
          EXPECT_EQ(nr_host_membudget_hard_limit(), p.host_hard);
        }
  EXPECT_GE(compared, 40);  // not vacuous: most cases are real comparisons, and many are shrunk by the budget
  EXPECT_GE(shrunk, 20);
  unsetenv("NR_HOST_MEM_BUDGET_MB");
  nr_host_membudget_test_reset();
}

TEST(NrCellAdmit, OneCellRefuseMatchesTheHardLimit)
{
  // a host so small that the primary's committed bytes exceed avail - 512 MiB: today REFUSE (exit 7); the plan sets refuse, serves nothing
  setenv("NR_HOST_MEM_BUDGET_MB", "1200", 1);
  nr_host_membudget_test_reset();
  const Fp f = fp_of(273);
  const size_t iq = 2 * f.spf * 4 * 4, q = 4 * (4 * f.rxdf * 4 + iq);
  nr_host_membudget_commit(NR_HOST_MEM_PINNED, 1019 * MIB, 1.0);
  nr_host_membudget_commit(NR_HOST_MEM_QUEUE, q, 1.0);
  char why[256];
  const int real_refuse = nr_host_membudget_over_hard(why, sizeof why);
  nr_host_caps_t c = base_caps();
  c.mem_avail = 1200 * MIB;
  c.host_shared_bytes = 1019 * MIB;
  int ns;
  const nr_admit_plan_t p = plan_of(c, cells(2, 273), &ns);
  EXPECT_EQ(1, real_refuse);
  EXPECT_EQ(1, p.refuse);
  EXPECT_EQ(0, ns);
  EXPECT_EQ(NR_ADMIT_RAM, p.cell[0].reason);
  unsetenv("NR_HOST_MEM_BUDGET_MB");
  nr_host_membudget_test_reset();
}

TEST(NrCellAdmit, OneCellGpuEqualsMembudget)
{
  for (size_t free_mib : {(size_t)3000, (size_t)5000, (size_t)7800, (size_t)24000, (size_t)0})
    for (int rb : {51, 106, 273}) {
      nr_host_caps_t c = base_caps();
      c.gpu_tier = NR_GPU_DISCRETE;
      c.gpu_free_bytes = free_mib * MIB;
      const nr_admit_plan_t p = plan_of(c, cells(1, rb));
      const Fp f = fp_of(rb);
      const size_t B = nr_gpu_membudget_calc(free_mib * MIB);
      const size_t share = nr_gpu_membudget_share_of(B, NR_GPU_MEM_CSIRS);
      const int want = nr_csirs_disc_mem_capacity(share, rb, 4, f.fft, f.stride, 100, 640);
      SCOPED_TRACE(testing::Message() << "free_mib=" << free_mib << " rb=" << rb);
      EXPECT_EQ(want, p.cell[0].gpu_csirs_slots);
      EXPECT_EQ(B, p.gpu_budget);
    }
  // [CODE-READ comment nr_gpu_elastic.h: the 8 GB card cut the ring to 221 of 640 slots and the nID window to 6 of 10 at 273 PRB]: same order here
  nr_host_caps_t c = sens6();
  const nr_admit_plan_t p = plan_of(c, cells(1, 273));
  EXPECT_GE(p.cell[0].gpu_csirs_slots, 160);
  EXPECT_LT(p.cell[0].gpu_csirs_slots, 640);
  EXPECT_EQ(7, p.nid_window); // G1: 7 of 10 with the exact nID bytes of nr_gpu_cell.h (was 6 with the census-derived fixed part: 776 MiB total at window 10 vs 767 MiB exact)
}

TEST(NrCellAdmit, UnifiedHostBudgetPaysTheGpu)
{
  nr_host_caps_t a = dgx(), b = dgx();
  b.unified = 0;
  const nr_admit_plan_t pa = plan_of(a, cells(1, 217)), pb = plan_of(b, cells(1, 217));
  EXPECT_LT(pa.host_budget, pb.host_budget);
  EXPECT_EQ(pb.host_budget - pa.host_budget, pa.gpu_used + pa.gpu_graph_bytes);
}

// ---- profiles ----
TEST(NrCellAdmit, Sens6TwoCells106ServedTwoCells273SecondUnservedGpuMem)
{
  int ns;
  nr_admit_plan_t p = plan_of(sens6(), cells(2, 106), &ns);
  EXPECT_EQ(2, ns);
  EXPECT_EQ(NR_ADMIT_OK, p.binding);
  p = plan_of(sens6(), cells(2, 273), &ns);  // [CODE-READ estimate until G1]
  EXPECT_EQ(1, ns);
  EXPECT_TRUE(p.cell[0].served);
  EXPECT_FALSE(p.cell[1].served);
  EXPECT_EQ(NR_ADMIT_GPU_MEM, p.cell[1].reason);
  EXPECT_EQ(NR_ADMIT_GPU_MEM, p.binding);
  EXPECT_EQ(0, p.refuse);
  // the same two cells at 217 PRB (the plan's original wording): reported, not asserted as served or not
  p = plan_of(sens6(), cells(2, 217), &ns);
  EXPECT_GE(ns, 1);
  p = plan_of(sens6(), cells(1, 273), &ns);
  EXPECT_EQ(1, ns);
}

TEST(NrCellAdmit, Sens6RamBindsWhenTheHostIsSmall)
{
  nr_host_caps_t c = sens6();
  c.mem_avail = 4000 * MIB;
  int ns;
  const nr_admit_plan_t p = plan_of(c, cells(3, 106), &ns);
  EXPECT_EQ(1, ns);
  EXPECT_EQ(NR_ADMIT_RAM, p.cell[1].reason);
  EXPECT_EQ(NR_ADMIT_RAM, p.cell[2].reason);
  EXPECT_EQ(0, p.refuse);  // cell k > 0 never refuses the process
}

TEST(NrCellAdmit, DgxServesManyCellsLimitIsCoresOrChannelizer)
{
  int ns;
  nr_admit_plan_t p = plan_of(dgx(), cells(3, 217), &ns);
  EXPECT_EQ(3, ns);
  EXPECT_EQ(NR_ADMIT_OK, p.binding);
  p = plan_of(dgx(), cells(6, 217), &ns);
  EXPECT_EQ(4, ns);
  EXPECT_EQ(NR_ADMIT_CORES, p.cell[4].reason);
  EXPECT_EQ(NR_ADMIT_CORES, p.cell[5].reason);
  // channelizer: 250 MS/s budget; the cell at the window centre needs none; each 273 PRB shifted cell costs 122.88 MS/s
  nr_host_caps_t c = dgx();
  c.max_cells = NR_ADMIT_UNLIMITED_CELLS;
  c.chan_budget_msps = 250;
  auto v = cells(4, 273);
  for (auto &x : v) { x.window_centre_hz = 3.7e9; x.centre_hz = 3.7e9; }
  p = plan_of(c, v, &ns);
  EXPECT_EQ(4, ns);  // zero shift everywhere: the channelizer is not used
  for (int i = 0; i < 4; i++) v[i].centre_hz = 3.7e9 + 20e6 * i;
  p = plan_of(c, v, &ns);
  EXPECT_EQ(3, ns);  // cell 0 at the centre, two shifted cells fit 245.76 <= 250, the third does not
  EXPECT_EQ(NR_ADMIT_CHANNELIZER, p.cell[3].reason);
  c.chan_budget_msps = -1;
  p = plan_of(c, v, &ns);
  EXPECT_EQ(4, ns);  // unknown = unlimited
}

TEST(NrCellAdmit, Cloud4CpuHost)
{
  int ns;
  nr_admit_plan_t p = plan_of(cloud4(), cells(3, 106), &ns);
  EXPECT_EQ(1, ns);
  EXPECT_EQ(NR_ADMIT_CORES, p.cell[1].reason);
  EXPECT_EQ(0u, p.gpu_used);
  EXPECT_EQ(0, p.cell[0].gpu_csirs_slots);
  EXPECT_EQ(1024, p.cell[0].td_slots);
}

TEST(NrCellAdmit, ZeroCells)
{
  int ns = -5;
  const nr_admit_plan_t p = plan_of(sens6(), {}, &ns);
  EXPECT_EQ(0, ns);
  EXPECT_EQ(0, p.n);
  EXPECT_EQ(0, p.refuse);
  nr_admit_plan_t q;
  const nr_host_caps_t sc = sens6();
  EXPECT_EQ(0, nr_cell_admit_plan(&sc, nullptr, 0, &q));
}

TEST(NrCellAdmit, SeventeenCellsOverTheSlotLimit)
{
  nr_host_caps_t c = base_caps();  // no cores limit, unlimited RAM, no GPU: only NR_CELL_MAX binds
  int ns;
  const nr_admit_plan_t p = plan_of(c, cells(17, 51), &ns);
  EXPECT_EQ(NR_CELL_MAX, ns);
  EXPECT_FALSE(p.cell[16].served);
  EXPECT_EQ(NR_ADMIT_MAX, p.cell[16].reason);
  EXPECT_STREQ("max", nr_admit_reason_name(p.cell[16].reason));
  const std::vector<nr_cell_desc_t> big(NR_ADMIT_PLAN_MAX + 1);
  nr_admit_plan_t q;
  EXPECT_EQ(-1, nr_cell_admit_plan(&c, big.data(), (int)big.size(), &q));
  EXPECT_EQ(-1, nr_cell_admit_plan(nullptr, big.data(), 1, &q));
  EXPECT_EQ(-1, nr_cell_admit_plan(&c, big.data(), -1, &q));
}

// ---- policy ----
TEST(NrCellAdmit, AdmissionOrderPrimaryThenMaintainAcquireNew)
{
  nr_host_caps_t c = dgx();
  c.max_cells = 3;
  auto v = cells(5, 106);
  v[0].state = NR_CELLD_NEW; v[1].state = NR_CELLD_NEW; v[2].state = NR_CELLD_ACQUIRE; v[3].state = NR_CELLD_MAINTAIN; v[4].state = NR_CELLD_LOST;
  int ns;
  const nr_admit_plan_t p = plan_of(c, v, &ns);
  EXPECT_EQ(3, ns);
  EXPECT_EQ(0, p.cell[0].order);
  EXPECT_EQ(1, p.cell[3].order);
  EXPECT_EQ(2, p.cell[2].order);
  EXPECT_TRUE(p.cell[0].served && p.cell[3].served && p.cell[2].served);
  EXPECT_FALSE(p.cell[1].served);
  EXPECT_EQ(NR_ADMIT_CORES, p.cell[1].reason);
  EXPECT_EQ(NR_ADMIT_LOST, p.cell[4].reason);
  EXPECT_FALSE(p.cell[4].served);
}

TEST(NrCellAdmit, ARefusedCellNeverChangesTheOthers)
{
  const nr_admit_plan_t a = plan_of(sens6(), cells(2, 106));
  auto v = cells(2, 106);
  auto extra = cells(1, 273);
  v.push_back(extra[0]);  // third, refused
  const nr_admit_plan_t b = plan_of(sens6(), v);
  ASSERT_FALSE(b.cell[2].served);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(a.cell[i].csirs_slots, b.cell[i].csirs_slots);
    EXPECT_EQ(a.cell[i].td_slots, b.cell[i].td_slots);
    EXPECT_EQ(a.cell[i].gw_buffers, b.cell[i].gw_buffers);
    EXPECT_EQ(a.cell[i].gpu_csirs_slots, b.cell[i].gpu_csirs_slots);
    EXPECT_EQ(a.cell[i].host_bytes, b.cell[i].host_bytes);
    EXPECT_EQ(a.cell[i].gpu_bytes, b.cell[i].gpu_bytes);
  }
  EXPECT_EQ(a.host_used, b.host_used);
  EXPECT_EQ(a.gpu_used, b.gpu_used);
}

TEST(NrCellAdmit, ScalableSharesSplitByWeight)
{
  nr_host_caps_t c = sens6();
  c.mem_avail = 8000 * MIB;
  c.gpu_tier = NR_GPU_NONE;
  auto v = cells(2, 106);
  v[1].weight = 2;
  const nr_admit_plan_t p = plan_of(c, v);
  ASSERT_TRUE(p.cell[0].served && p.cell[1].served);
  EXPECT_GT(p.cell[1].gw_buffers, p.cell[0].gw_buffers);
  auto e = cells(2, 106);
  const nr_admit_plan_t q = plan_of(c, e);
  EXPECT_EQ(q.cell[0].gw_buffers, q.cell[1].gw_buffers);
}

TEST(NrCellAdmit, PrimaryIsNeverDisplacedAndOnlyItRefuses)
{
  nr_host_caps_t c = sens6();
  c.mem_avail = 1200 * MIB;
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(2, 273), &ns);
  EXPECT_EQ(1, p.refuse);
  EXPECT_EQ(0, ns);
  c.mem_avail = 9216 * MIB;
  p = plan_of(c, cells(1, 51), &ns);
  EXPECT_EQ(0, p.refuse);
  EXPECT_EQ(1, ns);
}

// ---- cost model ----
TEST(NrCellAdmit, CostRulesUnknownFieldsAndMonotonicity)
{
  nr_host_caps_t caps = sens6();
  nr_cell_desc_t d;
  nr_cell_desc_init(&d);
  nr_cell_cost_t c0, c273, c106, c106rx2, c15;
  d.scs_khz = 30; d.n_rb = 0;
  ASSERT_EQ(0, nr_cell_cost(&d, &caps, &c0));
  d.n_rb = 273; nr_cell_cost(&d, &caps, &c273);
  EXPECT_EQ(273, c0.n_rb);                 // unknown grid sized as the maximum of its SCS
  EXPECT_EQ(c273.host_fixed, c0.host_fixed);
  d.scs_khz = 0; d.n_rb = 0; d.n_rx = 0;
  nr_cell_cost(&d, &caps, &c15);
  EXPECT_EQ(30, c15.scs_khz);
  EXPECT_EQ(4, c15.n_rx);
  d.scs_khz = 15; nr_cell_cost(&d, &caps, &c15);
  EXPECT_EQ(270, c15.n_rb);
  d.scs_khz = 30; d.n_rb = 106; d.n_rx = 4; nr_cell_cost(&d, &caps, &c106);
  d.n_rx = 2; nr_cell_cost(&d, &caps, &c106rx2);
  EXPECT_LT(c106.host_fixed, c273.host_fixed);
  EXPECT_LT(c106.gpu_floor, c273.gpu_floor);
  EXPECT_LT(c106rx2.host_fixed, c106.host_fixed);
  EXPECT_LT(c106rx2.gpu_full, c106.gpu_full);
  EXPECT_LE(c106.host_floor, c106.host_full);
  EXPECT_NEAR(61.44, c106.msps, 1e-9);     // [CODE get_samplerate_and_bw mu=1: 106 PRB -> 61.44 MS/s]
  EXPECT_NEAR(122.88, c273.msps, 1e-9);
  d.n_rb = 51; nr_cell_cost(&d, &caps, &c15);
  EXPECT_NEAR(30.72, c15.msps, 1e-9);
  d.n_rb = 217; nr_cell_cost(&d, &caps, &c15);
  EXPECT_NEAR(122.88, c15.msps, 1e-9);
  d.n_rb = 133; nr_cell_cost(&d, &caps, &c15);
  EXPECT_NEAR(61.44, c15.msps, 1e-9);
  // GPU tier NONE: no device bytes
  caps.gpu_tier = NR_GPU_NONE;
  d.n_rb = 106; nr_cell_cost(&d, &caps, &c106);
  EXPECT_EQ(0u, c106.gpu_fixed + c106.gpu_floor + c106.gpu_full);
  EXPECT_EQ(-1, nr_cell_cost(nullptr, &caps, &c106));
}

TEST(NrCellAdmit, FormatLine)
{
  int ns;
  const nr_admit_plan_t p = plan_of(sens6(), cells(2, 273), &ns);
  char line[512];
  EXPECT_GT(nr_admit_plan_format(&p, line, sizeof line), 0);
  EXPECT_NE(nullptr, strstr(line, "MPCI_ADMIT cells=1 of 2 reason=gpu_mem refuse=0"));
  EXPECT_NE(nullptr, strstr(line, "unserved=[1:gpu_mem]"));
}

// ---------------------------------------------------------------------------------------------------------------------
// B6: the core pool against the decode demand of the served cells; the PRACH heap and the sense rings booked per cell.
// Host = DGX GB10 fixture with the pool the layout gives k cells (nr_cl_build_nf, 2026-10-29: 6 / 10 / 9 / 8 / 7 tokens for 1..5 cells, wideband feed) and the
// channelizer's one reserved token. Before B6 the same layout gave 2 cells 7 tokens (MEASURED on air).
static nr_host_caps_t dgx_pool(bool old_layout = false)
{
  nr_host_caps_t c = dgx();
  const int now[] = {0, 6, 10, 9, 8, 7}, was[] = {0, 6, 7, 7, 6, 6};
  for (int k = 1; k <= 5; k++) c.pool_tokens[k] = old_layout ? was[k] : now[k];
  c.chan_tokens = 1;
  // one running 217 PRB 4 RX 30 kHz cell, measured by the pool: 1.9 essential tokens, 6.2 % of the essential leases waited (the numbers of the DGX OTA 2026-10-28, now INPUTS)
  c.pool_meas_n = 1; c.pool_meas_ess[0] = 1.9; c.pool_meas_grid[0] = 217.0 * 4.0; c.pool_meas_wait = 0.062;
  return c;
}
TEST(NrCellAdmitPool, ErlangCMatchesTheMeasuredEnds)
{
  EXPECT_DOUBLE_EQ(nr_pool_wait_prob(6, 0), 0.0);
  EXPECT_DOUBLE_EQ(nr_pool_wait_prob(6, 6), 1.0);
  EXPECT_DOUBLE_EQ(nr_pool_wait_prob(0, 1), 1.0);
  EXPECT_NEAR(nr_pool_wait_prob(1, 0.5), 0.5, 1e-12); // M/M/1: P(wait) = rho
  EXPECT_NEAR(nr_pool_wait_prob(2, 1.0), 1.0 / 3.0, 1e-12); // textbook value
  // solo, 6 tokens, 2.7 erlangs (the 2-cell per-cell total taken as the solo proxy, [INFERRED]): 6.5 % modelled against 3.9-6.2 % of decode / scan leases that waited [MEASURED, DGX, mp28m_S1/S2]
  EXPECT_NEAR(nr_pool_wait_prob(6, 2.7), 0.065, 0.003);
  // 2 cells, 7 tokens - 1 reserved, 5.5 erlangs: 78 % modelled against 64 % / 68 % measured [mp28m_P2]
  EXPECT_NEAR(nr_pool_wait_prob(6, 5.5), 0.78, 0.01);
  for (int c = 1; c < 12; c++)
    for (double a = 0.5; a < c; a += 0.5) {
      EXPECT_LE(nr_pool_wait_prob(c + 1, a), nr_pool_wait_prob(c, a) + 1e-12) << "more servers never wait more";
      EXPECT_LE(nr_pool_wait_prob(c, a), nr_pool_wait_prob(c, a + 0.25) + 1e-12) << "more load never waits less";
    }
}

TEST(NrCellAdmitPool, DemandIsMeasuredAndScalesWithTheNewCellsOwnGrid)
{
  nr_cell_desc_t c;
  nr_cell_desc_init(&c);
  c.n_rb = 217; c.scs_khz = 30; c.n_rx = 4;
  nr_host_caps_t k = dgx_pool();
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 1.9, 1e-9);
  c.n_rb = 106; // scaled down too: no clamp at an anchor any more, the ratio of the grids is the charge
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 1.9 * 106.0 / 217.0, 1e-9);
  c.n_rb = 273;
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 1.9 * 273.0 / 217.0, 1e-9);
  c.n_rb = 217; c.scs_khz = 60; // twice the slots per second
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 3.8, 1e-9);
  c.scs_khz = 30; c.n_rx = 2;
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 0.95, 1e-9);
  k.pool_meas_ess[0] = 3.8; // the running cell measured twice as busy: the newcomer is charged twice
  c.n_rx = 4;
  EXPECT_NEAR(nr_cell_pool_demand(&k, &c), 3.8, 1e-9);
  c.placeholder = 1; // a spare slot runs nothing
  EXPECT_DOUBLE_EQ(nr_cell_pool_demand(&k, &c), 0.0);
  EXPECT_DOUBLE_EQ(nr_cell_pool_demand(&k, nullptr), 0.0);
  nr_host_caps_t none = dgx_pool();
  none.pool_meas_n = 0; // nothing running or measured: no charge to derive
  c.placeholder = 0;
  EXPECT_DOUBLE_EQ(nr_cell_pool_demand(&none, &c), 0.0);
}

TEST(NrCellAdmitPool, NothingMeasuredChecksTheFloorOnly)
{
  nr_host_caps_t c = dgx_pool();
  c.pool_meas_n = 0; // the first plan of a process: no lease has run
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(3, 217), &ns);
  EXPECT_EQ(ns, 3); // 9 - ... tokens of the 3-cell layout - 1 = 8 >= 2 x 3
  EXPECT_DOUBLE_EQ(p.pool_load, 0.0);
  EXPECT_DOUBLE_EQ(p.pool_unit, 0.0);
  c.pool_tokens[3] = 5; // eff 4 < 6: the structural floor still refuses
  p = plan_of(c, cells(3, 217), &ns);
  EXPECT_EQ(ns, 2);
  EXPECT_EQ(p.cell[2].reason, NR_ADMIT_POOL);
}

TEST(NrCellAdmitPool, WaitTargetFollowsTheMeasuredWaitOfTheRunningCells)
{
  nr_host_caps_t c = dgx_pool();
  int ns;
  c.pool_meas_wait = 0.30; // the running cell already waits a lot: a second one that models below that is let in
  nr_admit_plan_t p = plan_of(c, cells(2, 217), &ns);
  EXPECT_EQ(ns, 2);
  EXPECT_NEAR(p.pool_target, 0.30, 1e-12);
  c.pool_meas_wait = 0.0; // never waited: the target is the Erlang C of the running state (6 tokens, 1.9 erlangs)
  const double model = nr_pool_wait_prob(6, 1.9);
  p = plan_of(c, cells(2, 217), &ns);
  EXPECT_NEAR(p.pool_model_now, model, 1e-12);
  EXPECT_EQ(ns, nr_pool_wait_prob(9, 3.8) <= model ? 2 : 1);
  c.pool_meas_wait = -1.0; // unknown: the model alone
  p = plan_of(c, cells(2, 217), &ns);
  EXPECT_NEAR(p.pool_target, model, 1e-12);
}

TEST(NrCellAdmitPool, DgxServesTwoCellsOfThe217PrbClassAndReportsTheThird)
{
  const nr_host_caps_t c = dgx_pool();
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(3, 217), &ns);
  EXPECT_EQ(ns, 2);
  EXPECT_TRUE(p.cell[0].served);
  EXPECT_TRUE(p.cell[1].served);
  EXPECT_FALSE(p.cell[2].served);
  EXPECT_EQ(p.cell[2].reason, NR_ADMIT_POOL);
  EXPECT_EQ(p.binding, NR_ADMIT_POOL);
  EXPECT_STREQ("pool", nr_admit_reason_name(p.cell[2].reason));
  ASSERT_TRUE(p.pool_checked);
  EXPECT_EQ(p.pool_tokens_eff, 9); // 10 tokens of the 2-cell layout - the channelizer's 1
  EXPECT_NEAR(p.pool_load, 3.8, 1e-9);
  EXPECT_LE(p.pool_wait, p.pool_target);
  char l[512];
  EXPECT_GT(nr_admit_pool_format(&p, l, sizeof l), 0);
  EXPECT_NE(nullptr, strstr(l, "MPCI_ADMIT_POOL cells=2 tokens_eff=9 load=3.80 wait_pct=1.8 target_pct=6.2 meas_cells=1 meas_load=1.90 unit=0.00219 meas_wait_pct=6.2 model_now_pct=")) << l;
  char m[1024];
  nr_admit_plan_format(&p, m, sizeof m);
  EXPECT_NE(nullptr, strstr(m, "MPCI_ADMIT cells=2 of 3 reason=pool refuse=0")) << m; // the MPCI_ADMIT grammar is unchanged
  EXPECT_NE(nullptr, strstr(m, "unserved=[2:pool]")) << m;
}

TEST(NrCellAdmitPool, ThePoolOfBeforeB6WouldNotHaveServedTheSecondCellHealthy)
{ // 7 tokens - 1 = 6 for 2 cells of 1.9 erlangs of essential work: modelled 24 % waits; the measured run had 64-68 % of the leases waiting with the sheddable work on top
  const nr_host_caps_t c = dgx_pool(true);
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(2, 217), &ns);
  EXPECT_EQ(ns, 1);
  EXPECT_EQ(p.cell[1].reason, NR_ADMIT_POOL);
}

TEST(NrCellAdmitPool, PrimaryAlwaysServedAndNoTableMeansNoCheck)
{
  nr_host_caps_t c = dgx_pool();
  c.pool_tokens[1] = 1; // a pool far too small even for the primary: it is served as today (nothing else can make the process refuse)
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(1, 217), &ns);
  EXPECT_EQ(ns, 1);
  EXPECT_TRUE(p.cell[0].served);
  nr_host_caps_t none = dgx();                     // no pool table (caps of before): the check is skipped, 4 cells by the core limit
  p = plan_of(none, cells(5, 217), &ns);
  EXPECT_EQ(ns, 4);
  EXPECT_FALSE(p.pool_checked);
  EXPECT_EQ(p.cell[4].reason, NR_ADMIT_CORES);
  char l[64];
  EXPECT_EQ(nr_admit_pool_format(&p, l, sizeof l), 0);
}

TEST(NrCellAdmitPool, TwoTokensPerCellIsTheFloorAndCoresComeFirst)
{
  nr_host_caps_t c = dgx_pool();
  for (int k = 1; k <= 5; k++) c.pool_tokens[k] = 4 + k; // eff = 3 + k... for k >= 2: 4 + k - 1
  int ns;
  nr_admit_plan_t p = plan_of(c, cells(5, 51), &ns); // 51 PRB cells are charged the anchor demand
  for (int i = 0; i < p.n; i++)
    if (p.cell[i].served) { EXPECT_GE(p.pool_tokens_eff, 2 * p.n_served); }
  c.max_cells = 1;
  p = plan_of(c, cells(3, 217), &ns);
  EXPECT_EQ(ns, 1);
  EXPECT_EQ(p.cell[1].reason, NR_ADMIT_CORES) << "the core limit is named before the pool";
}

TEST(NrCellAdmitPool, SpareSlotsCarryNoDemand)
{
  nr_host_caps_t c = dgx_pool();
  std::vector<nr_cell_desc_t> v = cells(3, 217);
  v[2].placeholder = 1; // a spare slot appended to a 2-cell inventory
  v[2].state = NR_CELLD_NEW;
  int ns;
  nr_admit_plan_t p = plan_of(c, v, &ns);
  EXPECT_EQ(ns, 3);
  EXPECT_EQ(p.pool_tokens_eff, 8); // the 3-cell layout (9 tokens) - 1
  EXPECT_NEAR(p.pool_load, 3.8, 1e-9);
  EXPECT_LE(p.pool_wait, p.pool_target);
}

TEST(NrCellAdmitPool, PrachHeapAndSenseRingsAreBookedPerCell)
{
  // the sense rings at the [sensing] defaults, 4 RX: 53.4 MiB [MEASURED, MPCI-CELLFEAT-20261029 "about 53 MiB"]
  const size_t sense = nr_cell_sense_ring_bytes(32, 46200, 1024, 4);
  EXPECT_GE(sense, 53 * MIB);
  EXPECT_LT(sense, 54 * MIB);
  EXPECT_EQ(nr_cell_sense_ring_bytes(0, 46200, 1024, 4), 0u);
  EXPECT_EQ(nr_cell_sense_ring_bytes(32, 46200, 1024, 0), 0u);
  EXPECT_GT(nr_cell_sense_ring_bytes(32, 46200, 1024, 4), nr_cell_sense_ring_bytes(32, 46200, 1024, 1)) << "scales with the antennas";
  nr_host_caps_t a = dgx(), b = dgx();
  b.prach_heap_bytes = 70 * MIB + 400 * 1024; // the PRACH heap is a build constant (~70 MB)
  b.sense_frames = 32; b.sense_max_re = 46200; b.sense_rec_slots = 1024;
  a.mem_avail = b.mem_avail = SIZE_MAX; // unlimited: the scalable shares do not move, the difference is exactly the booked arenas
  int na, nb;
  nr_admit_plan_t pa = plan_of(a, cells(2, 106), &na), pb = plan_of(b, cells(2, 106), &nb);
  ASSERT_EQ(na, 2);
  ASSERT_EQ(nb, 2);
  for (int i = 0; i < 2; i++) EXPECT_EQ(pb.cell[i].host_bytes, pa.cell[i].host_bytes + b.prach_heap_bytes + sense) << i;
  nr_cell_cost_t ca, cb;
  nr_cell_desc_t d = cells(1, 106)[0];
  ASSERT_EQ(nr_cell_cost(&d, &a, &ca), 0);
  ASSERT_EQ(nr_cell_cost(&d, &b, &cb), 0);
  EXPECT_EQ(cb.host_fixed, ca.host_fixed + b.prach_heap_bytes + sense);
  // booked on a small host: the second cell no longer fits where it did before
  nr_host_caps_t s = cloud4();
  s.max_cells = NR_ADMIT_UNLIMITED_CELLS;
  s.mem_avail = 6000 * MIB;
  int n0, n1;
  nr_admit_plan_t p0 = plan_of(s, cells(4, 217), &n0);
  s.prach_heap_bytes = 400 * MIB; // exaggerated on purpose to show the effect on a small host
  nr_admit_plan_t p1 = plan_of(s, cells(4, 217), &n1);
  EXPECT_LE(n1, n0);
  (void)p0; (void)p1;
}
