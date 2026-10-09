#ifndef NR_PDCCH_SCAN_LLR_CACHE_H
#define NR_PDCCH_SCAN_LLR_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A pass may revisit the same CORESET with a different search space. The LLRs
 * depend on the exact channel-estimation extent and its autoscale population:
 * overlapping but unequal extents cannot be sliced bit-identically. */
enum { NR_PDCCH_SCAN_LLR_ENTRIES = 8 };
typedef struct {
  int64_t absolute_slot;
  const void *samples;
  uint64_t fo_bits;
  uint16_t n_rb, rb_offset, bwp_start, n_id;
  uint16_t fft_size, first_carrier_offset, carrier_rb;
  uint8_t n_ant, first_symbol, duration, coreset_type;
  uint8_t frequency_domain_resource[6];
} nr_pdcch_scan_llr_key_t;

typedef struct {
  nr_pdcch_scan_llr_key_t key;
  size_t count;
  int16_t *data;
  bool valid;
} nr_pdcch_scan_llr_entry_t;

typedef struct {
  nr_pdcch_scan_llr_entry_t entry[NR_PDCCH_SCAN_LLR_ENTRIES];
  unsigned next;
} nr_pdcch_scan_llr_cache_t;

static inline bool nr_pdcch_scan_llr_key_equal(const nr_pdcch_scan_llr_key_t *a,
                                                const nr_pdcch_scan_llr_key_t *b)
{
  return a->absolute_slot == b->absolute_slot && a->samples == b->samples && a->fo_bits == b->fo_bits
         && a->n_rb == b->n_rb && a->rb_offset == b->rb_offset && a->bwp_start == b->bwp_start
         && a->n_id == b->n_id && a->fft_size == b->fft_size
         && a->first_carrier_offset == b->first_carrier_offset && a->carrier_rb == b->carrier_rb
         && memcmp(a->frequency_domain_resource, b->frequency_domain_resource, 6) == 0
         && a->n_ant == b->n_ant
         && a->first_symbol == b->first_symbol && a->duration == b->duration
         && a->coreset_type == b->coreset_type;
}

static inline bool nr_pdcch_scan_llr_get(const nr_pdcch_scan_llr_cache_t *cache,
                                         const nr_pdcch_scan_llr_key_t *key, void *out, size_t bytes)
{
  for (int i = 0; i < NR_PDCCH_SCAN_LLR_ENTRIES; i++) {
    const nr_pdcch_scan_llr_entry_t *e = &cache->entry[i];
    if (e->valid && e->count == bytes && nr_pdcch_scan_llr_key_equal(&e->key, key)) {
      memcpy(out, e->data, bytes);
      return true;
    }
  }
  return false;
}

/* Storage is reserved once per scan thread. Failed allocation leaves the CPU
 * path unchanged. Caller owns the allocated blocks for the thread lifetime. */
static inline bool nr_pdcch_scan_llr_put(nr_pdcch_scan_llr_cache_t *cache,
                                         const nr_pdcch_scan_llr_key_t *key, const void *src,
                                         size_t bytes, size_t capacity)
{
  if (bytes > capacity) return false;
  nr_pdcch_scan_llr_entry_t *e = &cache->entry[cache->next++ % NR_PDCCH_SCAN_LLR_ENTRIES];
  if (!e->data) {
    e->data = (int16_t *)malloc(capacity);
    if (!e->data) { e->valid = false; return false; }
  }
  memcpy(e->data, src, bytes);
  e->key = *key;
  e->count = bytes;
  e->valid = true;
  return true;
}

#endif
