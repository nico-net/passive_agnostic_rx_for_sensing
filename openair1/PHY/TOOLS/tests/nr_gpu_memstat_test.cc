/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* G1: the GPU memory ledger (nr_gpu_memstat.h): tags, scopes, per-cell sums, peak, thread safety, the GPUMEM line, and the booking done by the platform wrappers
 * (nr_gpu_alloc / nr_gpu_buf_alloc and their frees) on a mocked CUDA runtime. [OFFLINE VERIFIED on the machine that runs it; no device.] */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "nr_gpu_platform_internal.h"
#include "nr_gpu_mem.h"
#include "nr_gpu_memstat.h"

namespace {
int attrs[128];
std::string last_line;
void log_message(int, const char *m) {
  if (strstr(m, "GPUMEM")) last_line = m;
}
int get_count(int *n) { *n = 1; return 0; }
int get_device(int *d) { *d = 0; return 0; }
int attr(int *v, int a, int) { *v = attrs[a]; return 0; }
int mem_info(size_t *f, size_t *t) { *f = 1 << 20; *t = 2 << 20; return 0; }
int device_alloc(void **p, size_t n) { *p = malloc(n ? n : 1); return 0; }
int host_alloc(void **p, size_t n, unsigned) { *p = malloc(n ? n : 1); return 0; }
int release(void *p) { free(p); return 0; }
int device_ptr(void **d, void *h, unsigned) { *d = h; return 0; }
int reg(void *, size_t, unsigned) { return 0; }
int copy(void *d, const void *s, size_t b, int, void *) { memcpy(d, s, b); return 0; }
int stream_sync(void *) { return 0; }
int device_sync() { return 0; }
int set_flags(unsigned) { return 0; }
int get_last_error() { return 0; }
int memset_dev(void *p, int v, size_t n) { memset(p, v, n); return 0; }
int probe_ok() { return NR_GPU_PROBE_OK; }
const nr_gpu_runtime_t runtime = {get_count, get_device, attr, mem_info, device_alloc, host_alloc, release, release, device_ptr, reg, release, copy, stream_sync, device_sync, set_flags, get_last_error, memset_dev};
int g_cell = 0;
int cell_fn() { return g_cell; }

class Ledger : public testing::Test {
  void SetUp() override {
    memset(attrs, 0, sizeof(attrs));
    attrs[18] = attrs[19] = attrs[41] = attrs[89] = 1; attrs[75] = 12; attrs[76] = 1; attrs[88] = attrs[100] = 1; // GB10-like: SHARED tier
    nr_gpu_test_reset(&runtime);
    nr_gpu_test_set_probe(probe_ok);
    nr_gpu_mem_test_force_discrete(0);
    nr_gpu_set_logger(log_message);
    nr_gpu_memstat_reset_for_test();
    nr_gpu_memstat_set_cell_fn_for_test(cell_fn);
    g_cell = 0;
    last_line.clear();
  }
  void TearDown() override {
    nr_gpu_mem_test_force_discrete(0);
    nr_gpu_memstat_set_cell_fn_for_test(nullptr);
    nr_gpu_test_reset(&runtime);
    nr_gpu_set_logger(nullptr);
  }
};
uint64_t cur(int mod, int scope, int cell, int kind) {
  nr_gpu_memstat_t s;
  EXPECT_EQ(0, nr_gpu_memstat_get(mod, scope, cell, &s));
  return s.cur[kind];
}
} // namespace

TEST_F(Ledger, TagsScopesAndPerCellSums) {
  uint32_t p = nr_gpu_memstat_enter(NR_MS_PASS, NR_MS_SHARED, 0);
  nr_gpu_memstat_note(NR_MS_DEV, 1000);
  nr_gpu_memstat_note(NR_MS_PIN, 24);
  nr_gpu_memstat_leave(p);
  p = nr_gpu_memstat_enter(NR_MS_CSIRS, NR_MS_PER_CELL, 2);
  nr_gpu_memstat_note(NR_MS_DEV, 500);
  nr_gpu_memstat_leave(p);
  p = nr_gpu_memstat_enter(NR_MS_PDCCH_NID, NR_MS_PER_CELL, 2);
  nr_gpu_memstat_note(NR_MS_DEV, 70);
  nr_gpu_memstat_note(NR_MS_MGD, 9);
  nr_gpu_memstat_leave(p);
  nr_gpu_memstat_note_to(NR_MS_CSIRS, NR_MS_PER_CELL, 5, NR_MS_DEV, 11);
  EXPECT_EQ(1000u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_DEV));
  EXPECT_EQ(24u, cur(NR_MS_PASS, NR_MS_SHARED, 7, NR_MS_PIN)) << "SHARED ignores the cell";
  EXPECT_EQ(500u, cur(NR_MS_CSIRS, NR_MS_PER_CELL, 2, NR_MS_DEV));
  EXPECT_EQ(11u, cur(NR_MS_CSIRS, NR_MS_PER_CELL, 5, NR_MS_DEV));
  EXPECT_EQ(0u, cur(NR_MS_CSIRS, NR_MS_SHARED, 0, NR_MS_DEV)) << "PER_CELL bytes are not SHARED bytes";
  EXPECT_EQ(570u, nr_gpu_memstat_cell_bytes(2, NR_MS_DEV));
  EXPECT_EQ(9u, nr_gpu_memstat_cell_bytes(2, NR_MS_MGD));
  EXPECT_EQ(11u, nr_gpu_memstat_cell_bytes(5, NR_MS_DEV));
  EXPECT_EQ(0u, nr_gpu_memstat_cell_bytes(0, NR_MS_DEV));
  EXPECT_EQ(1000u, nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_DEV));
  EXPECT_EQ(581u, nr_gpu_memstat_total(NR_MS_PER_CELL, NR_MS_DEV));
  EXPECT_EQ(6u, nr_gpu_memstat_total_count());
  EXPECT_EQ(0u, cur(NR_MS_UNTAGGED, NR_MS_SHARED, 0, NR_MS_DEV));
}

TEST_F(Ledger, PeakSurvivesRelease) {
  const uint32_t tag = nr_ms_pack(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 1);
  nr_gpu_memstat_note_tag(tag, NR_MS_DEV, 100);
  nr_gpu_memstat_note_tag(tag, NR_MS_DEV, 300);
  nr_gpu_memstat_release_tag(tag, NR_MS_DEV, 250);
  nr_gpu_memstat_t s;
  ASSERT_EQ(0, nr_gpu_memstat_get(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 1, &s));
  EXPECT_EQ(150u, s.cur[NR_MS_DEV]);
  EXPECT_EQ(400u, s.peak[NR_MS_DEV]);
  EXPECT_EQ(2u, s.n[NR_MS_DEV]);
  EXPECT_EQ(400u, nr_gpu_memstat_total_peak(NR_MS_DEV));
  nr_gpu_memstat_release_tag(tag, NR_MS_DEV, 1u << 30); // over-release saturates at 0, never wraps
  EXPECT_EQ(0u, cur(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 1, NR_MS_DEV));
}

TEST_F(Ledger, OutOfRangeCellAndBadModuleAreVisibleAsUntagged) {
  nr_gpu_memstat_note_to(NR_MS_PASS, NR_MS_PER_CELL, NR_GPU_MEMSTAT_CELLS, NR_MS_DEV, 64);
  nr_gpu_memstat_note_to(NR_MS_MOD_N + 3, NR_MS_SHARED, 0, NR_MS_DEV, 32);
  EXPECT_EQ(96u, cur(NR_MS_UNTAGGED, NR_MS_SHARED, 0, NR_MS_DEV));
  nr_gpu_memstat_t s;
  EXPECT_EQ(-1, nr_gpu_memstat_get(NR_MS_MOD_N, NR_MS_SHARED, 0, &s));
  EXPECT_EQ(-1, nr_gpu_memstat_get(NR_MS_PASS, NR_MS_PER_CELL, -1, &s));
}

TEST_F(Ledger, ScopesNestRestoreAndAreThreadLocal) {
  const uint32_t outer = nr_gpu_memstat_enter(NR_MS_LDPC_DEC, NR_MS_SHARED, 0);
  EXPECT_EQ(0u, outer) << "no scope before: UNTAGGED/SHARED";
  {
    NrMsScope inner(NR_MS_CSIRS, NR_MS_PER_CELL, 3);
    nr_gpu_memstat_note(NR_MS_DEV, 5);
    inner.set(NR_MS_CSIRS, NR_MS_SHARED);
    nr_gpu_memstat_note(NR_MS_DEV, 7);
    std::thread([] { nr_gpu_memstat_note(NR_MS_DEV, 1); }).join(); // another thread has its own (empty) scope
  }
  nr_gpu_memstat_note(NR_MS_DEV, 2); // back in the outer scope
  nr_gpu_memstat_leave(outer);
  EXPECT_EQ(5u, cur(NR_MS_CSIRS, NR_MS_PER_CELL, 3, NR_MS_DEV));
  EXPECT_EQ(7u, cur(NR_MS_CSIRS, NR_MS_SHARED, 0, NR_MS_DEV));
  EXPECT_EQ(2u, cur(NR_MS_LDPC_DEC, NR_MS_SHARED, 0, NR_MS_DEV));
  EXPECT_EQ(1u, cur(NR_MS_UNTAGGED, NR_MS_SHARED, 0, NR_MS_DEV));
}

TEST_F(Ledger, PerCellScopeWithoutCellTakesTheHostCell) {
  g_cell = 4;
  {
    NrMsScope s(NR_MS_PDCCH_NID, NR_MS_PER_CELL); // cell = -1
    nr_gpu_memstat_note(NR_MS_PIN, 40);
  }
  g_cell = 1;
  {
    NrMsScope s(NR_MS_PDCCH_NID, NR_MS_PER_CELL);
    nr_gpu_memstat_note(NR_MS_PIN, 2);
  }
  EXPECT_EQ(40u, cur(NR_MS_PDCCH_NID, NR_MS_PER_CELL, 4, NR_MS_PIN));
  EXPECT_EQ(2u, cur(NR_MS_PDCCH_NID, NR_MS_PER_CELL, 1, NR_MS_PIN));
  nr_gpu_memstat_set_cell_fn_for_test(nullptr); // by name: this test binary has no nr_cell_tls_idx_, so cell 0
  EXPECT_EQ(0, nr_gpu_memstat_host_cell());
}

TEST_F(Ledger, ThreadSafetyExactTotals) {
  constexpr int T = 8, N = 20000;
  std::vector<std::thread> th;
  for (int t = 0; t < T; t++)
    th.emplace_back([t] {
      NrMsScope s(NR_MS_PASS, NR_MS_PER_CELL, t % 4);
      for (int i = 0; i < N; i++) {
        nr_gpu_memstat_note(NR_MS_DEV, 64);
        nr_gpu_memstat_note(NR_MS_PIN, 8);
        if (i & 1) nr_gpu_memstat_release_tag(nr_gpu_memstat_current(), NR_MS_DEV, 64);
      }
    });
  for (auto &x : th) x.join();
  for (int c = 0; c < 4; c++) {
    EXPECT_EQ((uint64_t)2 * (N / 2) * 64, nr_gpu_memstat_cell_bytes(c, NR_MS_DEV)) << c;
    EXPECT_EQ((uint64_t)2 * N * 8, nr_gpu_memstat_cell_bytes(c, NR_MS_PIN)) << c;
  }
  EXPECT_EQ((uint64_t)T * 2 * N, nr_gpu_memstat_total_count());
  EXPECT_GE(nr_gpu_memstat_total_peak(NR_MS_DEV), nr_gpu_memstat_total(NR_MS_PER_CELL, NR_MS_DEV));
}

TEST_F(Ledger, PlatformWrappersBookAndRelease) {
  void *a, *b;
  {
    NrMsScope s(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 2);
    a = nr_gpu_alloc(NR_GPU_DEVICE, 1000);
    b = nr_gpu_alloc(NR_GPU_STAGING, 300);
  }
  ASSERT_TRUE(a && b);
  EXPECT_EQ(1000u, cur(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 2, NR_MS_DEV));
  EXPECT_EQ(300u, cur(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 2, NR_MS_PIN));
  EXPECT_EQ(0, nr_gpu_free(a)); // the free books against the allocating tag, whatever scope the freeing thread is in
  EXPECT_EQ(0u, cur(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 2, NR_MS_DEV));
  EXPECT_EQ(300u, cur(NR_MS_PDSCH_FEP, NR_MS_PER_CELL, 2, NR_MS_PIN));
  EXPECT_EQ(0, nr_gpu_free(b));
  EXPECT_EQ(0u, nr_gpu_memstat_cell_bytes(2, NR_MS_PIN));
  // outside any scope: visible as untagged
  void *c = nr_gpu_alloc(NR_GPU_DEVICE, 77);
  ASSERT_TRUE(c);
  EXPECT_EQ(77u, cur(NR_MS_UNTAGGED, NR_MS_SHARED, 0, NR_MS_DEV));
  nr_gpu_free(c);
}

TEST_F(Ledger, BufferWrappersDirectAndDiscrete) {
  nr_gpu_buf_t d{}, v{}, o{};
  {
    NrMsScope s(NR_MS_PASS, NR_MS_SHARED);
    ASSERT_EQ(0, nr_gpu_buf_alloc(&d, 4096)); // DIRECT (SHARED tier): one mapped pinned block
    EXPECT_EQ(4096u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_PIN));
    EXPECT_EQ(0u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_DEV));
    nr_gpu_mem_test_force_discrete(1);
    ASSERT_EQ(0, nr_gpu_buf_alloc(&v, 1000));      // DISCRETE: pinned mirror + device copy
    ASSERT_EQ(0, nr_gpu_buf_alloc_dev(&o, 500));   // DISCRETE device-only
  }
  EXPECT_EQ(4096u + 1000u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_PIN));
  EXPECT_EQ(1000u + 500u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_DEV));
  nr_gpu_buf_free(&v);
  nr_gpu_buf_free(&o);
  EXPECT_EQ(4096u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_PIN));
  EXPECT_EQ(0u, cur(NR_MS_PASS, NR_MS_SHARED, 0, NR_MS_DEV));
  nr_gpu_buf_free(&d);
  EXPECT_EQ(0u, nr_gpu_memstat_total(NR_MS_SHARED, NR_MS_PIN));
}

TEST_F(Ledger, GpumemLineGrammarAndFenceLog) {
  {
    NrMsScope s(NR_MS_PASS, NR_MS_SHARED);
    nr_gpu_memstat_note(NR_MS_DEV, 4096);
    nr_gpu_memstat_note(NR_MS_PIN, 100);
    nr_gpu_memstat_note_graph();
  }
  nr_gpu_memstat_note_to(NR_MS_CSIRS, NR_MS_PER_CELL, 1, NR_MS_DEV, 2000);
  nr_gpu_memstat_note_to(NR_MS_PDCCH_NID, NR_MS_PER_CELL, 1, NR_MS_PIN, 300);
  nr_gpu_memstat_note_to(NR_MS_CSIRS, NR_MS_PER_CELL, 3, NR_MS_MGD, 50);
  char line[2048];
  const int n = nr_gpu_memstat_format(line, sizeof line, "fence");
  ASSERT_GT(n, 0);
  ASSERT_LT(n, (int)sizeof line);
  const std::string l = line;
  EXPECT_EQ(0u, l.find("GPUMEM at=fence dev=6096 pin=400 mgd=50 peak_dev=6096 peak_pin=400 peak_mgd=50 allocs=5 untagged=0 rss_anon_kb="));
  EXPECT_NE(std::string::npos, l.find(" cells=2 shared=[pass:4096/100/0/2+1g] cell1=[csirs:2000/0/0/1 pdcch_nid:0/300/0/1] cell3=[csirs:0/0/50/1]")) << l;
  // snprintf semantics: a short buffer reports the full length
  char tiny[16];
  EXPECT_EQ(n, nr_gpu_memstat_format(tiny, sizeof tiny, "fence"));
  // the RX fence logs the line once through the platform logger
  nr_gpu_rx_started();
  EXPECT_NE(std::string::npos, last_line.find("SENSING: GPUMEM at=fence dev=6096")) << last_line;
  EXPECT_GE(nr_gpu_memstat_rss_anon_kb(), 0);
}
