/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* iq_chan_offline <wideband dir> <out dir> <cell centre Hz> <n_rb> <scs_hz> <decim>: the receiver's channelizer applied to a wideband raw capture (see core). */
#include "iq_chan_offline_core.h"

int main(int argc, char **argv)
{
  if (argc != 7) { fprintf(stderr, "usage: %s <wideband dir> <out dir> <cell centre Hz> <n_rb> <scs_hz> <decim>\n", argv[0]); return 2; }
  ico_stats_t st;
  const int rc = ico_channelize(argv[1], argv[2], atof(argv[3]), atoi(argv[4]), atof(argv[5]), atoi(argv[6]), &st);
  if (rc) return 1;
  for (int a = 0; a < 4; ++a) printf("ch%d rms=%.2f clipped=%" PRIu64 "\n", a, st.rms[a], st.clipped[a]);
  printf("out_samples=%" PRIu64 "\n", st.out_samples);
  return 0;
}
