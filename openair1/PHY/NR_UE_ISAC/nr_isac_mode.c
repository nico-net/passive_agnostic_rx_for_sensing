#include "nr_isac_mode.h"
#include <string.h>

static nr_isac_mode_t g_mode = NR_ISAC_MODE_LEGACY; /* written once at init, before any reader thread */

nr_isac_mode_t nr_isac_mode(void) { return g_mode; }
void nr_isac_mode_set(nr_isac_mode_t m) { g_mode = m; }

bool nr_isac_mode_resolve(const char *mode_str, unsigned explicit_sources, int explicit_pdsch_decode,
                          int explicit_pusch_decode, nr_isac_mode_cfg_t *out)
{
  out->mode = NR_ISAC_MODE_LEGACY;
  out->sources = explicit_sources;
  out->pdsch_decode = explicit_pdsch_decode;
  out->pusch_decode = explicit_pusch_decode;
  out->narrowed = false;
  if (mode_str == NULL || mode_str[0] == '\0' || strcmp(mode_str, "legacy") == 0) return true;
  if (strcmp(mode_str, "sensing") == 0) {
    out->mode = NR_ISAC_MODE_SENSING; /* reserved: behaves as LEGACY until the sensing plan lands */
    return true;
  }
  if (strcmp(mode_str, "traffic") != 0) return false;
  out->mode = NR_ISAC_MODE_TRAFFIC;
  out->sources = 0;
  out->pdsch_decode = explicit_pdsch_decode == 0 ? 0 : 1;
  out->pusch_decode = explicit_pusch_decode == 0 ? 0 : 1;
  out->narrowed = explicit_sources != 0 || explicit_pdsch_decode > 1 || explicit_pusch_decode > 1;
  return true;
}
