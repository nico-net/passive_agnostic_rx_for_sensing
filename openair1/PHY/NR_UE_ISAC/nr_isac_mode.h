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
} nr_isac_mode_cfg_t;

/* mode_str NULL/"" -> LEGACY (all inputs passed through unchanged). explicit_* < 0 = key not set.
 * Returns false on an unknown mode string. Traffic: no CFR source, decode-only PDSCH, PUSCH decode
 * may only be lowered. Sensing is reserved and resolves like LEGACY. Pure. */
bool nr_isac_mode_resolve(const char *mode_str, unsigned explicit_sources, int explicit_pdsch_decode,
                          int explicit_pusch_decode, nr_isac_mode_cfg_t *out);

/* Process-wide, set once by nr_isac_init(); LEGACY before. */
nr_isac_mode_t nr_isac_mode(void);
void nr_isac_mode_set(nr_isac_mode_t m);

#ifdef __cplusplus
}
#endif
#endif
