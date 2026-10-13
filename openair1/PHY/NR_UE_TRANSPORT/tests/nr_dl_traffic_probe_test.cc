/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLINDPIPE 2026-10-13: nr_dl_traffic_probe pure functions on synthetic slots. */
#include "nr_dl_traffic_probe.h"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

namespace {
constexpr int kFft = 512, kFco = 100, kRb = 30, kCells = NR_DLTP_NSYM * kRb;

/* Gaussian noise slot (sigma per I/Q), with a rectangle of PDSCH-like signal at +snr_db. */
struct Slot {
  std::vector<int16_t> rxF;
  std::vector<float> e, scratch;
  Slot(unsigned seed, float sigma) : rxF(2 * NR_DLTP_NSYM * kFft), e(kCells), scratch(kCells) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.f, sigma);
    for (auto &v : rxF) v = (int16_t)std::lround(n(rng));
  }
  void light(int sym0, int nsym, int rb0, int nrb, float amp) {
    for (int s = sym0; s < sym0 + nsym; ++s)
      for (int r = rb0; r < rb0 + nrb; ++r)
        for (int k = 0; k < 12; ++k) {
          const int bin = (kFco + 12 * r + k) % kFft;
          rxF[2 * (s * kFft + bin)] += (int16_t)amp;
          rxF[2 * (s * kFft + bin) + 1] += (int16_t)amp;
        }
  }
  float energy() {
    nr_dltp_energy(rxF.data(), kFft, kFco, kRb, e.data());
    return nr_dltp_floor(e.data(), kCells, scratch.data());
  }
};
const nr_dltp_geom_t kGeom = {kRb, 0, kRb, 0, 0, 0, 0, 0};
}  // namespace

TEST(DlTrafficProbe, FloorTracksNoiseMeanAndNoiseStaysBelowOccupancyBar) {
  nr_dltp_stats_t st = {};
  uint64_t lit = 0, cells = 0;
  for (unsigned seed = 1; seed <= 20; ++seed) {
    Slot s(seed, 100.f);
    const float floor = s.energy();
    /* 24 real Gaussians of sigma 100 per cell: mean 240000; the p10 estimator must land within 10 % */
    EXPECT_NEAR(floor, 240000.f, 24000.f);
    nr_dltp_slot(s.e.data(), &kGeom, floor, true, nullptr, 0, &st);
    lit += st.cells_lit; cells += st.cells; st.cells_lit = 0; st.cells = 0;
  }
  /* design false alarm 1e-4 per cell: 20 slots x 330 data cells = 6600 cells -> about 0.7 lit expected */
  EXPECT_LE(lit, 6u);
  EXPECT_EQ(st.pdsch_like_ungranted, 0u);
  EXPECT_EQ(nr_dltp_verdict(&st, nullptr, 0), 0); /* 20 slots: undecided */
}

TEST(DlTrafficProbe, UngrantedPdschIsPresentGrantedPdschIsAbsent) {
  for (int granted = 0; granted < 2; ++granted) {
    nr_dltp_stats_t st = {};
    for (unsigned seed = 1; seed <= 120; ++seed) {
      Slot s(seed, 100.f);
      if (seed % 10 == 0) s.light(2, 12, 5, 8, 400.f); /* 8 RB PDSCH, symbols 2..13, every 10th slot: +12 dB */
      const float floor = s.energy();
      nr_dltp_grant_t g = {0, (uint8_t)(seed % 20), 5, 8, 2, 12};
      nr_dltp_slot(s.e.data(), &kGeom, floor, true, granted ? &g : nullptr, granted ? 1 : 0, &st);
    }
    char buf[640];
    const int v = nr_dltp_verdict(&st, buf, sizeof(buf));
    EXPECT_EQ(st.pdsch_like, 12u * 8u);
    if (granted) {
      EXPECT_EQ(st.pdsch_like_ungranted, 0u);
      EXPECT_EQ(v, -1) << buf;
    } else {
      EXPECT_EQ(st.pdsch_like_ungranted, 12u * 8u);
      EXPECT_EQ(v, 1) << buf;
    }
  }
}

TEST(DlTrafficProbe, SingleSymbolCsiRsIsNotPdschLikeAndMasksApply) {
  nr_dltp_stats_t st = {};
  Slot s(7, 100.f);
  s.light(6, 1, 0, kRb, 400.f);   /* one-symbol CSI-RS-like burst over the carrier */
  s.light(0, 1, 20, 6, 400.f);    /* PDCCH-like RBs 20..25 in symbol 0 */
  s.light(0, 2, 2, 4, 400.f);     /* CORESET#0 RBs 2..5 symbols 0-1: masked */
  const float floor = s.energy();
  nr_dltp_geom_t g = kGeom;
  g.css0_rb0 = 2; g.css0_rb1 = 6; g.css0_nsym = 2;
  g.ssb_rb0 = 26; g.ssb_rb1 = 30;
  nr_dltp_slot(s.e.data(), &g, floor, false, nullptr, 0, &st);
  EXPECT_EQ(st.pdsch_like, 0u);
  EXPECT_GE(st.cells_lit, 24u); /* the CSI-RS symbol: 26 RBs minus SSB band (4) = 26 cells */
  EXPECT_EQ(st.ctrl_rbs[0], 6u);
  EXPECT_EQ(st.ctrl_slots[0], 1u);
  EXPECT_EQ(st.ctrl_rbs[1], 0u);
  EXPECT_EQ(st.slots_tdd_unknown, 1u);
}

TEST(DlTrafficProbe, GrantRegistryReturnsOnlyTheSlotsGrants) {
  nr_dltp_note_grant(100, 3, 10, 20, 2, 12);
  nr_dltp_note_grant(100, 4, 0, 5, 1, 13);
  nr_dltp_note_grant(100, 3, 40, 2, 0, 14);
  nr_dltp_note_grant(100, 3, -1, 2, 0, 14); /* invalid: ignored */
  nr_dltp_grant_t out[8];
  const int n = nr_dltp_grants_for(100, 3, out, 8);
  ASSERT_EQ(n, 2);
  EXPECT_EQ(out[0].rb0, 10); EXPECT_EQ(out[1].rb0, 40);
  EXPECT_EQ(nr_dltp_grants_for(101, 3, out, 8), 0);
}

TEST(DlTrafficProbe, CoresetDiscStateMissedVsNoTrafficVsLocked) {
  nr_cdisc_t c = {};
  nr_cdisc_in_t in = {0.0, 0, 0, 500, 5000, 10.0, 2}; /* 5000 RB-slots over 500 probed slots: 10/slot */
  nr_cdisc_out_t o;
  nr_cdisc_step(&c, &in, &o);                       /* no carrier: cannot sweep yet */
  EXPECT_EQ(c.state, NR_CDISC_SEARCHING); EXPECT_STREQ(o.reason, "carrier_pending");
  in.carrier_ok = 1; in.now_s = 5.0;                /* armed at 5 s */
  nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_SEARCHING); EXPECT_EQ(o.recover, 0);
  in.now_s = 14.9; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_SEARCHING);           /* still inside the grace */
  in.now_s = 15.1; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_MISSED); EXPECT_EQ(o.recover, 1); EXPECT_EQ(o.changed, 1);
  in.now_s = 20.0; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_MISSED); EXPECT_EQ(o.recover, 0);   /* back-off: next at +2 x miss_s */
  in.now_s = 35.2; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(o.recover, 1);
  in.now_s = 80.0; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(o.recover, 0);                          /* max_recover = 2 reached */
  in.window_rbslots = 3; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_NO_TRAFFIC);          /* 3 RB-slots in 500 probed slots: nothing to miss */
  in.window_probed = 50; in.window_rbslots = 100; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_MISSED);              /* bursty but 2 RB-slots/slot, >= 64 total */
  in.window_probed = 5000; in.window_rbslots = 100; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_SEARCHING);           /* 0.02/slot: neither present nor absent */
  in.window_rbslots = 5000; in.bank_count = 1; nr_cdisc_step(&c, &in, &o);
  EXPECT_EQ(c.state, NR_CDISC_LOCKED); EXPECT_EQ(c.recoveries, 0);
}

TEST(DlTrafficProbe, PdschLikeInsideCoreset0IsNotCountedForTheWatch) {
  nr_dltp_stats_t st = {};
  Slot s(7, 100.f);
  s.light(3, 8, 2, 4, 400.f);     /* SIB1-like PDSCH inside CORESET#0 RBs 2..5 (symbols 3..10) */
  s.light(3, 8, 20, 4, 400.f);    /* UE-like PDSCH outside, RBs 20..23 */
  const float floor = s.energy();
  nr_dltp_geom_t g = kGeom;
  g.css0_rb0 = 2; g.css0_rb1 = 6; g.css0_nsym = 2;
  nr_dltp_slot(s.e.data(), &g, floor, true, nullptr, 0, &st);
  EXPECT_EQ(st.pdsch_like_ungranted, 8u);
  EXPECT_EQ(st.pdsch_like_ungranted_nocss0, 4u);
}
