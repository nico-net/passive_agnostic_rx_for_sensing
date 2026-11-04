#ifndef NR_ISAC_MODE_H
#define NR_ISAC_MODE_H
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_ISAC_MODE_LEGACY = 0, NR_ISAC_MODE_TRAFFIC = 1, NR_ISAC_MODE_SENSING = 2 } nr_isac_mode_t;
typedef struct {
  nr_isac_mode_t mode;
  unsigned sources;
  int pdsch_decode;
  int pusch_decode;
  bool narrowed;
  int min_prb; /* [sensing] sense_min_prb: a record is wideband when valid_cfr_prbs >= min_prb */
} nr_isac_mode_cfg_t;

/* mode_str NULL/"" -> LEGACY (all inputs passed through unchanged). explicit_* < 0 = key not set.
 * Returns false on an unknown mode string. Traffic: no CFR source, decode-only PDSCH, PUSCH decode
 * may only be lowered. Sensing: sources ssb|csi_rs|pdsch_dmrs|pdsch_data|pusch_dmrs|srs (+prach only when explicit), pdsch_decode 2, pusch_decode 1;
 * an explicit key narrows these, never widens (cut values set `narrowed`). min_prb = 128 in every mode. Pure. */
bool nr_isac_mode_resolve(const char *mode_str, unsigned explicit_sources, int explicit_pdsch_decode,
                          int explicit_pusch_decode, nr_isac_mode_cfg_t *out);

/* Process-wide, set once by nr_isac_init(); LEGACY before. */
nr_isac_mode_t nr_isac_mode(void);
void nr_isac_mode_set(nr_isac_mode_t m);

/* [sensing] sense_min_prb, default 128. _set returns false (and restores 128) for a value < 1. */
int nr_isac_sense_min_prb(void);
bool nr_isac_sense_min_prb_set(int v);

#ifdef __cplusplus
}
#endif
#endif
