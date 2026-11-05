/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* I1 (srsq review): noise-only false-valid rate of PUSCH DM-RS and SRS through the production estimators (fixture in nr_ul_noise_only_fixture.c), >= 1e5 PRBs each, against the DL rule on the
 * same estimates. Linked into test_nr_srs_real_inject. */
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
extern "C" {
typedef struct { uint64_t prbs, valid_ul, valid_dl; double ratio; uint64_t nratio; } ul_noise_t;
int ul_noise_only_run(int which, int trials, ul_noise_t *res);
}
static void check(const char *name, int which, int trials)
{
  ul_noise_t r;
  ASSERT_EQ(ul_noise_only_run(which, trials, &r), 0);
  printf("NOISE %s: UL rule %llu/%llu valid (%.4f %%), DL rule on the same estimates %llu (%.4f %%); mean|H|^2/noise_var %.3f\n", name, (unsigned long long)r.valid_ul, (unsigned long long)r.prbs,
         100.0 * r.valid_ul / r.prbs, (unsigned long long)r.valid_dl, 100.0 * r.valid_dl / r.prbs, r.ratio / r.nratio);
  EXPECT_GE(r.prbs, 100000u);
  EXPECT_LE(r.valid_ul, r.valid_dl + r.prbs / 10000) << name << ": UL false-valid above the DL rule on identical noise";
  EXPECT_LE(r.valid_ul, r.prbs / 1000) << name << ": noise exported as valid";
}
TEST(UlNoiseOnly, PuschDmrs) { check("PUSCH_DMRS", 0, 1100); }
TEST(UlNoiseOnly, SrsComb2) { check("SRS comb2", 2, 470); }
TEST(UlNoiseOnly, SrsComb4) { check("SRS comb4", 4, 470); }
/* positive control of the same fixture: a 15 dB signal on 10 of 100 PRBs must be found there (valid_ul = valid inside, valid_dl = valid outside the signal PRBs) */
TEST(UlNoiseOnly, PuschDmrsPositiveControl)
{
  ul_noise_t r;
  ASSERT_EQ(ul_noise_only_run(1, 100, &r), 0);
  printf("CONTROL PUSCH_DMRS 15 dB on PRBs 40..49: valid inside %llu of %u, outside (beyond 2 PRBs of the signal) %llu of %u\n", (unsigned long long)r.valid_ul, 1000u, (unsigned long long)r.valid_dl, 8800u);
  EXPECT_GE(r.valid_ul, 700u);
  EXPECT_LE(r.valid_dl, 20u);
}
