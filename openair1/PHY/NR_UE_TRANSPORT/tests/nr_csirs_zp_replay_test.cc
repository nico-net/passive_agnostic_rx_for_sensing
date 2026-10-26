/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Offline replay of a recorded blind-ZP feature stream (ISAC_CSIRS_ZP_DUMP, nr_csirs_disc.c zp_dump) through the detector: prints what it proposes and the dark-cell table of every proposal.
 * Skipped when ZP_REPLAY_FILE is not set. ZP_REPLAY_EVAL (default 320) is the number of slots between two evaluations, as in the receiver. */
#include "nr_csirs_disc.h"
#include "nr_csirs_disc_zp.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

TEST(CsirsZpReplay, RecordedStream)
{
  const char *path = getenv("ZP_REPLAY_FILE");
  if (!path)
    GTEST_SKIP() << "ZP_REPLAY_FILE not set";
  FILE *f = fopen(path, "rb");
  ASSERT_NE(f, nullptr);
  uint32_t hdr[4];
  ASSERT_EQ(fread(hdr, sizeof(hdr), 1, f), 1u);
  ASSERT_EQ(hdr[0], 0x31465a50u);
  const int nrb = (int)hdr[1];
  const size_t fl = hdr[2];
  ASSERT_EQ(fl, nr_csirs_zp_feat_len(nrb));
  auto *det = nr_csirs_zp_new(nrb);
  ASSERT_NE(det, nullptr);
  const int every = getenv("ZP_REPLAY_EVAL") ? atoi(getenv("ZP_REPLAY_EVAL")) : 320;
  std::vector<float> feat(fl);
  uint32_t abs = 0;
  uint64_t n = 0;
  while (fread(&abs, 4, 1, f) == 1 && fread(feat.data(), sizeof(float), fl, f) == fl) {
    nr_csirs_zp_feed(det, abs, feat.data());
    if (++n % every)
      continue;
    nr_csirs_disc_fp_t fp[24];
    const int nf = nr_csirs_zp_propose(det, fp, 24);
    printf("slots %llu (abs %u): %d footprints\n", (unsigned long long)n, abs, nf);
    for (int i = 0; i < nf; ++i) {
      char cells[1024];
      nr_csirs_zp_cells(det, fp[i].period, fp[i].offset, cells, sizeof(cells));
      printf("  sym=0x%x sc=0x%x dens=%u rb=%u:%u P/O=%u/%u z=%.1f cells[%s]\n", fp[i].sym_mask, fp[i].sc_mask, fp[i].density, fp[i].rb_start, fp[i].n_rb, fp[i].period, fp[i].offset, fp[i].strength, cells);
    }
  }
  printf("%llu slots replayed\n", (unsigned long long)n);
  nr_csirs_zp_free(det);
  fclose(f);
}
