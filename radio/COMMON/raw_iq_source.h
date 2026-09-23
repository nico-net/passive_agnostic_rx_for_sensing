/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef RAW_IQ_SOURCE_H
#define RAW_IQ_SOURCE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
  const int16_t *iq[8];
  uint64_t samples, position, first_tick, clipped;
  unsigned channels, shift;
  double rate, center, frequency, phase;
  /* Offline-only fault injection: skip gap_len samples the first time position reaches gap_at.
   * The returned timestamp jumps with position, so the receiver's own RXDISCONT continuity test
   * sees exactly what a real stream loss looks like. 0 = disabled. */
  uint64_t gap_at, gap_len; int gap_done;
} raw_iq_source;
int raw_iq_open(raw_iq_source *s, const char *root, uint64_t samples,
                uint64_t first_tick, unsigned channels, double rate,
                double center, unsigned shift);
void raw_iq_close(raw_iq_source *s);
int raw_iq_tune(raw_iq_source *s, double frequency);
int raw_iq_read(raw_iq_source *s, void **buffers, int count,
                unsigned channels, uint64_t *timestamp);
#endif
