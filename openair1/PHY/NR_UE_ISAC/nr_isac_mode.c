#include "nr_isac_mode.h"
#include <string.h>

static nr_isac_mode_t g_mode = NR_ISAC_MODE_LEGACY; /* written once at init, before any reader thread */

nr_isac_mode_t nr_isac_mode(void) { return g_mode; }
void nr_isac_mode_set(nr_isac_mode_t m) { g_mode = m; }

#define SENSE_MIN_PRB_DEFAULT 128
static int g_min_prb = SENSE_MIN_PRB_DEFAULT; /* written once at init */
int nr_isac_sense_min_prb(void) { return g_min_prb; }
bool nr_isac_sense_min_prb_set(int v)
{
  g_min_prb = v >= 1 ? v : SENSE_MIN_PRB_DEFAULT;
  return v >= 1;
}

#define SENSE_MIN_PRB_FLOOR_DEFAULT 32
#define SENSE_CONT_TARGET_MS_DEFAULT 25
static int g_min_prb_floor = SENSE_MIN_PRB_FLOOR_DEFAULT, g_cont_target_ms = SENSE_CONT_TARGET_MS_DEFAULT; /* written once at init */
int nr_isac_sense_min_prb_floor(void) { return g_min_prb_floor; }
bool nr_isac_sense_min_prb_floor_set(int v)
{
  g_min_prb_floor = v >= 1 ? v : SENSE_MIN_PRB_FLOOR_DEFAULT;
  return v >= 1;
}
int nr_isac_sense_cont_target_ms(void) { return g_cont_target_ms; }
bool nr_isac_sense_cont_target_ms_set(int v)
{
  g_cont_target_ms = v >= 1 ? v : SENSE_CONT_TARGET_MS_DEFAULT;
  return v >= 1;
}

/* source bit positions = enum nr_isac_source in nr_isac.h (checked by nr_isac_mode_test) */
#define B(n) (1u << (n))
#define SENSING_SET (B(6) /*ssb*/ | B(0) /*csi_rs*/ | B(1) /*pdsch_dmrs*/ | B(2) /*pdsch_data*/ | B(4) /*pusch_dmrs*/ | B(8) /*srs*/)
#define SENSING_PRACH B(7)

bool nr_isac_mode_resolve(const char *mode_str, unsigned explicit_sources, int explicit_pdsch_decode,
                          int explicit_pusch_decode, nr_isac_mode_cfg_t *out)
{
  out->mode = NR_ISAC_MODE_LEGACY;
  out->sources = explicit_sources;
  out->pdsch_decode = explicit_pdsch_decode;
  out->pusch_decode = explicit_pusch_decode;
  out->narrowed = false;
  out->min_prb = SENSE_MIN_PRB_DEFAULT;
  if (mode_str == NULL || mode_str[0] == '\0' || strcmp(mode_str, "legacy") == 0) return true;
  if (strcmp(mode_str, "sensing") == 0) {
    out->mode = NR_ISAC_MODE_SENSING;
    const unsigned allowed = SENSING_SET | SENSING_PRACH;
    out->sources = explicit_sources & allowed ? explicit_sources & allowed : SENSING_SET; /* nothing valid left (0 would read as "unset"): the default set, flagged narrowed below */
    out->pdsch_decode = explicit_pdsch_decode < 0 || explicit_pdsch_decode > 2 ? 2 : explicit_pdsch_decode;
    out->pusch_decode = explicit_pusch_decode < 0 || explicit_pusch_decode > 1 ? 1 : explicit_pusch_decode;
    out->narrowed = (explicit_sources & ~allowed) != 0 || explicit_pdsch_decode > 2 || explicit_pusch_decode > 1;
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
