/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* P14 Stage A (removal table 2.4, row 2). AOA_ENABLE / AOA_UL_ENABLE used to be an undocumented
 * environment override of the `[sensing] aoa_enable` / `aoa_ul_enable` keys. The override is
 * removed. Silently ignoring a stale launcher variable would leave an operator believing it still
 * steers AoA, so the keys are counted here and reported by the caller.
 *
 * Deliberately its own translation unit, with no dependency beyond getenv(): it is compiled into
 * BOTH the real pipeline and the ENABLE_ISAC_SENSING=OFF stub build (one definition, no stub
 * duplicate), and the offline parity test links it without dragging in the configuration module. */
#include <stdlib.h>
#include "PHY/NR_UE_ISAC/nr_isac.h"

int nr_isac_obsolete_env_keys(void)
{
  static const char *const obsolete[] = {"AOA_ENABLE", "AOA_UL_ENABLE"};
  int present = 0;
  for (unsigned i = 0; i < sizeof(obsolete) / sizeof(obsolete[0]); ++i)
    if (getenv(obsolete[i]) != NULL)
      ++present;
  return present;
}
