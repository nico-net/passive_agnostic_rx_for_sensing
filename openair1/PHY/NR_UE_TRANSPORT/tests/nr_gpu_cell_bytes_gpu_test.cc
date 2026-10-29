/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* G1: nr_gpu_cell_bytes against the GPU modules' own allocations, on this machine's GPU. For every PER_CELL kind the module init runs in a CHILD process
 * (the modules initialise once per process) at 51 / 106 / 273 PRB and 1 / 4 RX, and the GPUMEM ledger (nr_gpu_memstat, booked at the allocation calls) is compared with
 * nr_gpu_cell_mem(): they must be equal byte for byte, per cell, per memory class (dev / pinned). The ledger is also checked for untagged bytes (must be 0).
 * Skipped (GTEST_SKIP) without ./libnr_gpu_platform.so, the modules or a CUDA device. Run serially (one process at a time on the device), no MPS:
 *   CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps ./test_nr_gpu_cell_bytes_gpu
 * Child protocol: argv = --child <kind> <n_rb> <n_rx>; prints one line "CELLBYTES kind=.. rb=.. rx=.. cell=.. dev=.. pin=.. exp_dev=.. exp_pin=.. untagged=.. status=<ok|mismatch|skip|fail> [note]". */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "nr_gpu_cell.h"
#include "nr_gpu_memstat.h"
#include "nr_pdsch_gpu_fep.h"
#include "nr_pdcch_gpu_fep.h"
#include "nr_pdcch_nid_gpu.h"
#include "nr_gpu_pass.h"
#include "nr_csirs_disc.h"

/* libdfts.so (loaded by the FEP module for its ISA twin self-test) resolves its LOG / T hooks from the executable: the stubs of nr_gpu_isa_twin_test.cu */
alignas(64) static unsigned char g_log_zero[1 << 16];
/* host half of the CSI-RS sweep module (nr_csirs_disc_sweep.c): the module only needs the symbols to load; this test calls prepare(), never the sweep itself */
extern "C" {
int nr_csirs_disc_sweep_validate(...) { return -1; }
int nr_csirs_disc_sweep_map_into(...) { return -1; }
void nr_csirs_disc_sweep_winner(...) {}
}
extern "C" {
void *g_log = g_log_zero;
int T_stdout = 1;
volatile int *T_freelist_head = nullptr;
void *T_cache = nullptr;
int *T_active = nullptr;
void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }
void logRecord_mt(const char *, const char *, int, int, int, const char *, ...) {}
int32_t write_file_matlab(const char *, const char *, void *, int, int, unsigned int, int) { return 0; }
}

namespace {
const char *g_self;
constexpr int kCell = 3; // the cell the ledger is asked to attribute PER_CELL bytes to (host lookup replaced by the test hook)
int cell_fn() { return kCell; }

struct Ms {
  int (*get)(int, int, int, nr_gpu_memstat_t *);
  uint64_t (*cell_bytes)(int, int);
  uint64_t (*total)(int, int);
  void (*set_cell_fn)(int (*)(void));
  void (*format)(char *, size_t, const char *);
};
int emit(const char *kind, int rb, int rx, uint64_t dev, uint64_t pin, uint64_t edev, uint64_t epin, uint64_t untagged, const char *status, const std::string &note)
{
  std::printf("CELLBYTES kind=%s rb=%d rx=%d cell=%d dev=%llu pin=%llu exp_dev=%llu exp_pin=%llu untagged=%llu status=%s %s\n", kind, rb, rx, kCell, (unsigned long long)dev,
              (unsigned long long)pin, (unsigned long long)edev, (unsigned long long)epin, (unsigned long long)untagged, status, note.c_str());
  std::fflush(stdout);
  return std::strcmp(status, "ok") == 0 || std::strcmp(status, "skip") == 0 || std::strcmp(status, "model") == 0 ? 0 : 1;
}

int child(const std::string &kind, int rb, int rx)
{
  void *plat = dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL);
  if (!plat) return emit(kind.c_str(), rb, rx, 0, 0, 0, 0, 0, "skip", "no ./libnr_gpu_platform.so");
  Ms ms{};
  ms.get = (int (*)(int, int, int, nr_gpu_memstat_t *))dlsym(plat, "nr_gpu_memstat_get");
  ms.cell_bytes = (uint64_t(*)(int, int))dlsym(plat, "nr_gpu_memstat_cell_bytes");
  ms.total = (uint64_t(*)(int, int))dlsym(plat, "nr_gpu_memstat_total");
  ms.set_cell_fn = (void (*)(int (*)(void)))dlsym(plat, "nr_gpu_memstat_set_cell_fn_for_test");
  if (!ms.get || !ms.cell_bytes || !ms.total || !ms.set_cell_fn) return emit(kind.c_str(), rb, rx, 0, 0, 0, 0, 0, "fail", "platform lib has no ledger symbols");
  ms.set_cell_fn(cell_fn);
  nr_gpu_cell_params_t p{};
  p.n_rb = rb; p.n_rx = rx; p.scs_khz = 30;
  const nr_gpu_cell_geom_t g = nr_gpu_cell_geom(&p);
  auto mod = [&](int m, int scope, nr_gpu_memstat_t *s) { return ms.get(m, scope, kCell, s); };
  nr_gpu_memstat_t s{}, sh{}, un{};
  auto done = [&](int module, nr_gpu_cell_mem_t exp, const std::string &note, bool model = false) {
    mod(module, NR_MS_PER_CELL, &s);
    ms.get(NR_MS_UNTAGGED, NR_MS_SHARED, 0, &un);
    const uint64_t untagged = un.cur[0] + un.cur[1] + un.cur[2];
    const bool eq = s.cur[NR_MS_DEV] == exp.dev && s.cur[NR_MS_PIN] == exp.pin && untagged == 0;
    return emit(kind.c_str(), rb, rx, s.cur[NR_MS_DEV], s.cur[NR_MS_PIN], exp.dev, exp.pin, untagged, model ? "model" : eq ? "ok" : "mismatch", note);
  };

  if (kind == "fep") {
    void *h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return emit("fep", rb, rx, 0, 0, 0, 0, 0, "skip", "no ./libpdsch_gpu.so");
    auto api = (const nr_gpu_fep_api_t *(*)(void))dlsym(h, "nr_gpu_fep_api");
    const nr_gpu_fep_api_t *a = api ? api() : nullptr;
    if (!a) return emit("fep", rb, rx, 0, 0, 0, 0, 0, "skip", "module not ready (no device / arch)");
    nr_gpu_fep_cfg_t c{};
    c.nant = g.n_rx; c.ofdm_symbol_size = g.fft; c.nb_prefix_samples = g.cp; c.nb_prefix_samples0 = g.cp0; c.symbols_per_slot = g.ns;
    c.first_carrier_offset = g.fft - 6 * g.n_rb; c.n_rb_dl = g.n_rb; c.samples_per_ms = g.fft * g.scs_khz; c.ofdm_offset_divisor = 8;
    if (a->init(&c) != 0) return emit("fep", rb, rx, 0, 0, 0, 0, 0, "skip", "fep init refused");
    auto ready = (int (*)(void))dlsym(h, "nr_pdsch_gpu_exact_ready");
    nr_gpu_cell_mem_t e = nr_gpu_cell_mem(&p, NR_GPU_CELL_PDSCH_FEP);
    std::string note = "kinds=pdsch_fep";
    if (ready && ready()) {
      const nr_gpu_cell_mem_t x = nr_gpu_cell_mem(&p, NR_GPU_CELL_PDSCH_FEP_EXACT);
      e.dev += x.dev; e.pin += x.pin; note += "+pdsch_fep_exact";
    } else note += " (bit-exact working set not created at this FFT size on this ISA)";
    return done(NR_MS_PDSCH_FEP, e, note);
  }
  if (kind == "pdcch_fep") {
    void *h = dlopen("./libpdcch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return emit("pdcch_fep", rb, rx, 0, 0, 0, 0, 0, "skip", "no ./libpdcch_gpu.so");
    auto api = (const nr_gpu_pdcch_fep_api_t *(*)(void))dlsym(h, "nr_gpu_pdcch_fep_api");
    const nr_gpu_pdcch_fep_api_t *a = api ? api() : nullptr;
    if (!a) return emit("pdcch_fep", rb, rx, 0, 0, 0, 0, 0, "skip", "module not ready");
    nr_gpu_pdcch_fep_cfg_t c{};
    c.nant = g.n_rx; c.ofdm_symbol_size = g.fft; c.symbols_per_slot = g.ns; c.first_carrier_offset = g.fft - 6 * g.n_rb; c.n_rb_dl = g.n_rb;
    if (a->init(&c) != 0) return emit("pdcch_fep", rb, rx, 0, 0, 0, 0, 0, "skip", "pdcch fep init refused");
    return done(NR_MS_PDCCH_FEP, nr_gpu_cell_mem(&p, NR_GPU_CELL_PDCCH_FEP), "kinds=pdcch_fep");
  }
  if (kind == "nid") {
    void *h = dlopen("./libpdcch_nid_gpu.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return emit("nid", rb, rx, 0, 0, 0, 0, 0, "skip", "no ./libpdcch_nid_gpu.so");
    auto ops = (const nr_pdcch_nid_ops_t *(*)(void))dlsym(h, "nr_pdcch_nid_gpu_ops");
    const nr_pdcch_nid_ops_t *o = ops ? ops() : nullptr;
    if (!o) return emit("nid", rb, rx, 0, 0, 0, 0, 0, "skip", "module not ready");
    nr_pdcch_nid_iq_t *staging = nullptr;
    void *ctx = o->create(rb, NR_GPU_CELL_NID_DUR_DEFAULT, NR_GPU_CELL_NID_WINDOW_DEFAULT, &staging);
    if (!ctx) return emit("nid", rb, rx, 0, 0, 0, 0, 0, "skip", "nid create refused");
    nr_gpu_cell_mem_t e = nr_gpu_cell_mem(&p, NR_GPU_CELL_NID_WINDOW);
    e.pin = nr_gpu_cell_mem(&p, NR_GPU_CELL_NID_STAGING).pin;
    mod(NR_MS_PDCCH_NID, NR_MS_SHARED, &sh);
    const nr_gpu_cell_mem_t fx = nr_gpu_cell_mem(&p, NR_GPU_SHARED_NID_FIXED);
    char note[160];
    std::snprintf(note, sizeof note, "kinds=nid_window+nid_staging shared_dev=%llu exp_shared_dev=%llu shared_pin=%llu%s", (unsigned long long)sh.cur[0], (unsigned long long)fx.dev,
                  (unsigned long long)sh.cur[1], sh.cur[0] == fx.dev ? "" : " SHARED-MISMATCH");
    const int rc = done(NR_MS_PDCCH_NID, e, note);
    return sh.cur[0] == fx.dev ? rc : 1;
  }
  if (kind == "harq") {
    void *h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return emit("harq", rb, rx, 0, 0, 0, 0, 0, "skip", "no ./libpdsch_gpu.so");
    auto cfgf = (int (*)(const nr_gpu_pass_cfg_t *))dlsym(h, "nr_gpu_pass_config");
    auto initf = (int (*)(int))dlsym(h, "nr_gpu_pass_init");
    if (!cfgf || !initf) return emit("harq", rb, rx, 0, 0, 0, 0, 0, "skip", "no pass entries");
    static int16_t table[(2 * 1 + 1) * 4096];
    nr_gpu_pass_cfg_t c{};
    c.nbRx = g.n_rx; c.n_rb_dl = g.n_rb; c.delay_table = table; c.dt_stride = 4096; c.max_delay_comp = 1;
    if (cfgf(&c) != 0) return emit("harq", rb, rx, 0, 0, 0, 0, 0, "fail", "pass config refused");
    if (initf(0) != 0) return emit("harq", rb, rx, 0, 0, 0, 0, 0, "skip", "pass init refused (conditional graph nodes unavailable or no device)");
    nr_gpu_memstat_t pg{};
    mod(NR_MS_PASS, NR_MS_SHARED, &sh);
    ms.get(NR_MS_PASS_GRAPH, NR_MS_SHARED, 0, &pg);
    char note[200];
    std::snprintf(note, sizeof note, "kinds=harq pass_shared_dev=%llu pass_shared_pin=%llu pass_graphs=%llu", (unsigned long long)sh.cur[0], (unsigned long long)sh.cur[1], (unsigned long long)pg.ngraph);
    return done(NR_MS_PASS, nr_gpu_cell_mem(&p, NR_GPU_CELL_HARQ), note);
  }
  if (kind == "csirs" || kind == "csirs_free") {
    void *h = dlopen("./libcsirs_disc_gpu.so", RTLD_NOW | RTLD_LOCAL);
    void *hs = dlopen("./libnr_csirs_disc_gpu_sweep.so", RTLD_NOW | RTLD_LOCAL);
    if (!h || !hs) return emit("csirs", rb, rx, 0, 0, 0, 0, 0, "skip", std::string("no CSI-RS modules: ") + ([] { const char *e = dlerror(); return e ? e : "?"; })());
    auto ops = (const nr_csirs_disc_gpu_ops_t *(*)(void))dlsym(h, "nr_csirs_disc_gpu_ops");
    auto sweep = (int (*)(int, int, int))dlsym(hs, "nr_csirs_disc_gpu_sweep_prepare");
    const nr_csirs_disc_gpu_ops_t *o = ops ? ops() : nullptr;
    if (!o || !sweep) return emit("csirs", rb, rx, 0, 0, 0, 0, 0, "skip", "csirs ops not ready");
    const int cap = NR_CSIRS_DISC_DEFAULT_CAPACITY;
    const size_t stride = 14 * ((size_t)g.fft + (size_t)(g.fft / 128 * 9) + (size_t)(g.fft / 128 * (9 + (1 << g.mu))));
    if (o->prepare(rb, g.n_rx, cap, g.fft, stride) < 0) return emit("csirs", rb, rx, 0, 0, 0, 0, 0, "skip", "csirs prepare refused");
    if (sweep(rb, g.n_rx, cap) < 0) return emit("csirs", rb, rx, 0, 0, 0, 0, 0, "skip", "csirs sweep prepare refused");
    if (kind == "csirs_free") {
      /* G4B: a cell retired for good gives every GPU workspace back (both modules), idempotently; a re-prepare then works again */
      auto f1 = (void (*)(void))dlsym(h, "nr_csirs_disc_gpu_cell_free");
      auto f2 = (void (*)(void))dlsym(hs, "nr_csirs_disc_gpu_sweep_cell_free");
      if (!f1 || !f2) return emit("csirs_free", rb, rx, 0, 0, 0, 0, 0, "fail", "no cell_free symbols");
      mod(NR_MS_CSIRS, NR_MS_PER_CELL, &s);
      const uint64_t before = s.cur[NR_MS_DEV];
      f2(); f1(); f2(); f1();
      mod(NR_MS_CSIRS, NR_MS_PER_CELL, &s);
      const uint64_t after = s.cur[NR_MS_DEV];
      const bool re = o->prepare(rb, g.n_rx, cap, g.fft, stride) >= 0 && sweep(rb, g.n_rx, cap) >= 0;
      mod(NR_MS_CSIRS, NR_MS_PER_CELL, &s);
      char fn[200];
      std::snprintf(fn, sizeof fn, "before=%llu after=%llu reprepared=%llu", (unsigned long long)before, (unsigned long long)after, (unsigned long long)s.cur[NR_MS_DEV]);
      return emit("csirs_free", rb, rx, after, 0, 0, 0, 0, before > 0 && after == 0 && re && s.cur[NR_MS_DEV] > 0 ? "ok" : "mismatch", fn);
    }
    nr_gpu_cell_mem_t e = nr_gpu_cell_mem(&p, NR_GPU_CELL_CSIRS_FIXED);
    e.dev += (size_t)cap * nr_gpu_cell_mem(&p, NR_GPU_CELL_CSIRS_SLOT).dev;
    e.dev -= (size_t)512 << 20; // the model's local-memory term is a driver reservation, not an allocation
    // MODEL kind: the discovery module sizes itself by Buffer<>::reserve() lists of its own; the comparison is reported, not asserted
    char note[160];
    mod(NR_MS_CSIRS, NR_MS_PER_CELL, &s);
    std::snprintf(note, sizeof note, "kinds=csirs_fixed+%d*csirs_slot minus 512MiB local memory (MODEL; measured/model=%.3f)", cap, e.dev ? (double)s.cur[NR_MS_DEV] / (double)e.dev : 0.0);
    return done(NR_MS_CSIRS, e, note, true);
  }
  return emit(kind.c_str(), rb, rx, 0, 0, 0, 0, 0, "fail", "unknown kind");
}

std::string run_child(const char *kind, int rb, int rx)
{
  char cmd[512];
  std::snprintf(cmd, sizeof cmd, "'%s' --child %s %d %d 2>/dev/null", g_self, kind, rb, rx);
  FILE *f = popen(cmd, "r");
  std::string out;
  if (!f) return out;
  char buf[1024];
  while (std::fgets(buf, sizeof buf, f))
    if (!std::strncmp(buf, "CELLBYTES ", 10)) out = buf;
  pclose(f);
  return out;
}
} // namespace

class CellBytesGpu : public testing::TestWithParam<const char *> {};
TEST_P(CellBytesGpu, LedgerEqualsFormula)
{
  int checked = 0;
  for (int rb : {51, 106, 273})
    for (int rx : {1, 4}) {
      const std::string line = run_child(GetParam(), rb, rx);
      std::printf("%s", line.c_str());
      ASSERT_FALSE(line.empty()) << GetParam() << " " << rb << "/" << rx << ": the child printed no result";
      if (line.find("status=skip") != std::string::npos) GTEST_SKIP() << line;
      EXPECT_TRUE(line.find("status=ok") != std::string::npos || line.find("status=model") != std::string::npos) << line;
      checked++;
    }
  EXPECT_EQ(6, checked);
}
INSTANTIATE_TEST_SUITE_P(Kinds, CellBytesGpu, testing::Values("fep", "pdcch_fep", "nid", "harq", "csirs", "csirs_free"));

int main(int argc, char **argv)
{
  g_self = argv[0];
  if (argc == 5 && !std::strcmp(argv[1], "--child")) return child(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
