/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Test baseline for the Technique D engine's process-wide modes (2026-10-04 review of "fastest combination default-on").
 *
 * Since 2026-10-04 the receiver defaults are field book fb2 (ISAC_TD_FIELDBOOK) and CB0 elimination ON (ISAC_TD_CB0_ELIM). Both are
 * process-wide, read once, and some fixtures change them: without a pin, a test's mode depended on the shell's environment and on
 * which test ran before it. A suite that includes this header and registers the listener starts EVERY test from the BASELINE
 *     fb0 (field book off: the destructive cell/RNTI prior prune) and CB0 elimination off,
 * i.e. the engine these suites were written against (bit-identical to the pre-fb2 / pre-CB0 engine). OnTestStart runs BEFORE the
 * fixture's SetUp, so a fixture or a test body that needs another mode pins it explicitly; tests of the receiver defaults call
 * nr_td_test::pin_defaults() (the modes an EMPTY environment resolves to). */
#ifndef NR_TD_TEST_BASELINE_H
#define NR_TD_TEST_BASELINE_H
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
extern "C" {
#include "nr_pdsch_config_sweep.h"
}

namespace nr_td_test {
constexpr int kBaselineFieldBook = 0; /* fb0 */
constexpr int kBaselineCb0 = 0;       /* CB0 elimination off */

inline void pin(int fieldbook, int cb0)
{
  nr_pdsch_config_sweep_fieldbook_set_mode(fieldbook);
  nr_pdsch_config_sweep_cb0_elim_env_set(cb0);
}
inline void pin_baseline() { pin(kBaselineFieldBook, kBaselineCb0); }

/* Scoped environment variable (restored on exit). value == nullptr: unset. */
struct ScopedEnv {
  std::string name, old;
  bool had;
  ScopedEnv(const char *n, const char *value) : name(n)
  {
    const char *o = getenv(n);
    had = o != nullptr;
    if (had)
      old = o;
    if (value)
      setenv(n, value, 1);
    else
      unsetenv(n);
  }
  ~ScopedEnv()
  {
    if (had)
      setenv(name.c_str(), old.c_str(), 1);
    else
      unsetenv(name.c_str());
  }
};

/* Resolve both modes from an EMPTY environment: the receiver's defaults (fb2 + CB0 elimination on since 2026-10-04). */
inline void pin_defaults()
{
  ScopedEnv fb("ISAC_TD_FIELDBOOK", nullptr), cb0("ISAC_TD_CB0_ELIM", nullptr);
  nr_pdsch_config_sweep_fieldbook_set_mode(-1);
  nr_pdsch_config_sweep_cb0_elim_env_set(-1);
  (void)nr_pdsch_config_sweep_cb0_elim_env(); /* resolve now, while the environment is empty */
}

struct BaselineListener : testing::EmptyTestEventListener {
  void OnTestStart(const testing::TestInfo &) override { pin_baseline(); }
};
/* Call once per test binary (e.g. `static const bool reg = nr_td_test::register_baseline();` in ONE translation unit). */
inline bool register_baseline()
{
  testing::UnitTest::GetInstance()->listeners().Append(new BaselineListener);
  return true;
}
} // namespace nr_td_test
#endif
