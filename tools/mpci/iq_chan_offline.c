/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* iq_chan_offline [--backend cpu|gpu] <wideband dir> <out dir> <cell centre Hz> <n_rb> <scs_hz> <decim>: the receiver's channelizer applied to a wideband raw capture (see core). */
#include "iq_chan_offline_core.h"
#include "shm_iq_chan_gpu.h"
#include <dlfcn.h>

int main(int argc, char **argv)
{
  if (argc > 2 && !strcmp(argv[1], "--backend")) {
    if (!strcmp(argv[2], "gpu")) {
      /* iq_chan_gpu_load() finds the module next to libnr_gpu_platform.so, so load that (from the build dir, RTLD_GLOBAL) first */
      const char *pl = getenv("NR_GPU_PLATFORM_SO");
      if (!dlopen(pl ? pl : "libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "%s\n", dlerror()); return 3; }
      const iq_chan_gpu_api_t *a = iq_chan_gpu_load();
      if (!a || !a->available()) { fprintf(stderr, "no GPU backend module / CUDA device\n"); return 3; }
      ico_backend = a->backend;
    } else if (strcmp(argv[2], "cpu")) { fprintf(stderr, "backend: cpu|gpu\n"); return 2; }
    argc -= 2; argv += 2;
  }
  if (argc != 7) { fprintf(stderr, "usage: %s [--backend cpu|gpu] <wideband dir> <out dir> <cell centre Hz> <n_rb> <scs_hz> <decim>\n", argv[0]); return 2; }
  ico_stats_t st;
  const int rc = ico_channelize(argv[1], argv[2], atof(argv[3]), atoi(argv[4]), atof(argv[5]), atoi(argv[6]), &st);
  if (rc) return 1;
  for (int a = 0; a < 4; ++a) printf("ch%d rms=%.2f clipped=%" PRIu64 "\n", a, st.rms[a], st.clipped[a]);
  printf("out_samples=%" PRIu64 "\n", st.out_samples);
  return 0;
}
