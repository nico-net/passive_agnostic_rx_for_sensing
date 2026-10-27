/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci DBG2: the PDCCH nID sweep is per cell. Two cells start their own sweep (fake device module, host transport double); a batch pushed
 * by cell 1 produces records for cell 1 only, cell 0's records / passes / generation stay untouched, and each stop releases only its cell.
 * Before the fix (one process-wide worker): the second start returned at once (the device of cell 0), cell 1's batch ran on cell 0's
 * worker and its records were published to cell 0. */
#include "nr_pdcch_nid_gpu.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
void nr_pdcch_nid_gpu_test_policy(int dur, int window, int batch_size, bool duty, unsigned acquire_ms, unsigned maintain_ms, unsigned recheck_ms);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void wait_ms(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }

int main()
{
  setenv("ISAC_GPU_BUDGET", "0", 1); /* as test_nr_pdcch_nid_worker: no shared GPU budget in a unit test */
  logInit();
  const int dur = 3, batch = 64;
  nr_pdcch_nid_gpu_test_policy(dur, 2, batch, false, 5000, 500, 10000);
  nr_pdcch_nid_batch_t b = {};
  b.cfg = {432, 0, 0, 6, 2, 14, dur};
  std::vector<nr_pdcch_nid_iq_t> fft(512 * dur);
  auto push_batch = [&]() {
    for (int k = 0; k < batch; k++) {
      for (int d = 0; d < dur; d++)
        for (int i = 0; i < 432; i++) fft[d * 512 + (499 + i) % 512] = nr_pdcch_nid_iq_t{(int16_t)(k + d + i), (int16_t)(-k - d - i)};
      int rc, tries = 0;
      do {
        rc = nr_pdcch_nid_gpu_push(fft.data(), 512, 499, k / 14, k % 14, &b.cfg);
        if (rc) wait_ms(1);
      } while (rc && ++tries < 5000);
      if (rc) return false;
    }
    return true;
  };
  nr_pdcch_nid_record_t rec[16];
  uint64_t gen0 = 0, gen1 = 0;

  nr_cell_bind(0);
  CHECK(nr_pdcch_nid_gpu_start(36) == 0 && nr_pdcch_nid_gpu_running());
  nr_cell_bind(1);
  CHECK(!nr_pdcch_nid_gpu_running()); /* cell 1 has no sweep yet (process-wide: cell 0's) */
  CHECK(nr_pdcch_nid_gpu_start(36) == 0 && nr_pdcch_nid_gpu_running());

  /* cell 1 pushes one batch: one pass and one record, for cell 1 */
  CHECK(push_batch());
  int n = -1;
  for (int i = 0; i < 3000 && n < 0; i++) { wait_ms(1); n = nr_pdcch_nid_gpu_records(rec, 16, &gen1); }
  CHECK(n == 1 && nr_pdcch_nid_gpu_passes() == 1);

  /* cell 0 saw nothing */
  nr_cell_bind(0);
  CHECK(nr_pdcch_nid_gpu_passes() == 0);
  CHECK(nr_pdcch_nid_gpu_records(rec, 16, &gen0) == -1 && gen0 == 0);
  CHECK(nr_pdcch_nid_gpu_snapshot(rec, 16, &gen0) == 0);

  /* cell 0's own batch: its own pass */
  CHECK(push_batch());
  n = -1;
  for (int i = 0; i < 3000 && n < 0; i++) { wait_ms(1); n = nr_pdcch_nid_gpu_records(rec, 16, &gen0); }
  CHECK(n == 1 && nr_pdcch_nid_gpu_passes() == 1);

  /* stopping cell 0 leaves cell 1 running */
  nr_pdcch_nid_gpu_stop();
  CHECK(!nr_pdcch_nid_gpu_running());
  nr_cell_bind(1);
  CHECK(nr_pdcch_nid_gpu_running() && nr_pdcch_nid_gpu_passes() == 1);
  nr_pdcch_nid_gpu_stop();
  CHECK(!nr_pdcch_nid_gpu_running());
  puts("PASS nID sweep per cell: own worker, records, passes and stop per cell");
  return 0;
}
