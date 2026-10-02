/* DCI payload bits, right-aligned in little-endian 64-bit words.
 *
 * Existing blind DCI extractors start at pos = dci_length, subtract each field
 * width, then read (payload >> pos): bit zero is the last (LSB) payload bit.
 * The polar decoder extracts information bits into uint64_t words with bit
 * index i at word i / 64, bit i % 64, then removes the low CRC bits. Its
 * tail writes ceil(A/64) words. This type preserves the out[0] convention and extends it
 * to out[1] and out[2], with the first transmitted payload bit at index A-1.
 */
#ifndef NR_DCI_BITS_H
#define NR_DCI_BITS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* TS 38.212 clauses 7.3.2 (DCI CRC attachment) and 7.3.3 (polar coding
 * for DCI): A <= 140 payload bits, K = A + 24 <= 164 after CRC attachment. */
#define NR_DCI_MAX_PAYLOAD 140
#define NR_DCI_WORDS 3

typedef struct { uint64_t w[NR_DCI_WORDS]; } nr_dci_bits_t;

static inline nr_dci_bits_t nr_dci_bits_from_u64(uint64_t v)
{
  nr_dci_bits_t b = {{v, 0, 0}};
  return b;
}

/* Hex in transmission order. Preserve each legacy log's narrow padding; wide
 * payloads print every occupied word with 16 hex digits per word. */
static inline const char *nr_dci_bits_hex(const uint64_t *w, int len, bool padded, char out[49])
{
  if (len > 128)
    snprintf(out, 49, "%016llx%016llx%016llx", (unsigned long long)w[2],
             (unsigned long long)w[1], (unsigned long long)w[0]);
  else if (len > 64)
    snprintf(out, 49, "%016llx%016llx", (unsigned long long)w[1], (unsigned long long)w[0]);
  else
    snprintf(out, 49, padded ? "%016llx" : "%llx", (unsigned long long)w[0]);
  return out;
}

/* msb_pos counts from the first payload bit; invalid ranges return zero. */
static inline uint64_t nr_dci_bits_field(const nr_dci_bits_t *b, int len, int msb_pos, int width)
{
  if (!b || len < 0 || len > NR_DCI_MAX_PAYLOAD || msb_pos < 0 || width < 0 || width > 64 ||
      msb_pos > len || width > len - msb_pos)
    return 0;
  if (width == 0)
    return 0;
  const int low = len - msb_pos - width;
  const int word = low / 64;
  const int offset = low % 64;
  uint64_t value = b->w[word] >> offset;
  if (offset && word + 1 < NR_DCI_WORDS)
    value |= b->w[word + 1] << (64 - offset);
  return width == 64 ? value : value & ((UINT64_C(1) << width) - 1);
}

static inline bool nr_dci_bits_eq(const nr_dci_bits_t *a, const nr_dci_bits_t *b)
{
  return a->w[0] == b->w[0] && a->w[1] == b->w[1] && a->w[2] == b->w[2];
}

/* Hash only the declared payload bits and its length; spare high bits do not
 * affect the result. */
static inline uint32_t nr_dci_bits_hash(const nr_dci_bits_t *b, int len)
{
  if (!b || len < 0 || len > NR_DCI_MAX_PAYLOAD)
    return 0;
  uint32_t h = UINT32_C(2166136261);
  h = (h ^ (uint32_t)len) * UINT32_C(16777619);
  for (int i = 0; i < len; ++i)
    h = (h ^ (uint32_t)((b->w[i / 64] >> (i % 64)) & 1)) * UINT32_C(16777619);
  return h;
}

#endif /* NR_DCI_BITS_H */
