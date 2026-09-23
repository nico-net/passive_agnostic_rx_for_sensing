#ifndef NR_PDCCH_USS_TRACKER_H
#define NR_PDCCH_USS_TRACKER_H

#include <stdint.h>

#define NR_PDCCH_USS_TRACKER_TOP 4
#define NR_PDCCH_USS_TRACKER_AL 5

typedef struct {
  uint16_t rb_offset;
  uint16_t span_rb;
  uint16_t shift;
  uint16_t dmrs_id;
  uint8_t duration;
  uint8_t bundle;
  uint8_t interleaver;
  uint8_t first_symbol;
} nr_pdcch_uss_geometry_t;

typedef struct {
  uint16_t cce;
  uint8_t al;
  float p_real; /* raw normalized DMRS correlation; never used as identity */
  float sigma;  /* same-AL dispersion for this occasion */
  float score;  /* non-negative DMRS occupancy weight; identity remains separate */
} nr_pdcch_uss_observation_t;

/* Non-blocking producer. It copies at most one strongest observation per AL into a bounded
 * persistent history and wakes a background scorer. Duplicate geometry/slot observations replace
 * rather than create recurrence. */
void nr_pdcch_uss_tracker_observe(const nr_pdcch_uss_geometry_t *geometry,
                                  uint32_t absolute_slot,
                                  uint16_t slot_in_frame,
                                  uint16_t n_cces,
                                  const nr_pdcch_uss_observation_t *observation,
                                  int n_observation);

/* Current multi-slot RNTI shortlist for exactly this geometry. These are hypotheses, not verified
 * identities; callers must still require exact CRC recurrence and PDSCH/PUSCH CRC. */
int nr_pdcch_uss_tracker_top(const nr_pdcch_uss_geometry_t *geometry,
                             uint16_t *rnti,
                             int max_rnti);

/* Latest unconfirmed shortlist. Use only for one bounded decode probe; never launch a sweep from it. */
int nr_pdcch_uss_tracker_peek(const nr_pdcch_uss_geometry_t *geometry,
                              uint16_t *rnti,
                              int max_rnti);

#endif
