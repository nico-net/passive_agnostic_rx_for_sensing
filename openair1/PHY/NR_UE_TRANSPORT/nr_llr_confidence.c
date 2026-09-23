#include "nr_llr_confidence.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t g_right[4][NR_LLRCONF_BINS], g_wrong[4][NR_LLRCONF_BINS];
static _Atomic uint64_t g_agree[4], g_disagree[4];
static _Atomic int g_disabled;
static _Atomic uint64_t g_kept_sym, g_total_sym; /* masked-path REs kept vs offered (spec §6 counters) */

static int qidx(uint8_t qm) { return (qm == 2 || qm == 4 || qm == 6 || qm == 8) ? qm / 2 - 1 : -1; }

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

/* Median |LLR| of the grant: the per-TB scale every ratio below is expressed in. */
static float median_abs(const int16_t *llr, uint32_t G) {
  static __thread uint16_t *buf = NULL; static __thread uint32_t cap = 0;
  if (cap < G) { free(buf); buf = malloc(sizeof(*buf) * G); cap = buf ? G : 0; if (!buf) return 0.0f; }
  for (uint32_t i = 0; i < G; i++) buf[i] = (uint16_t)(llr[i] < 0 ? -(int)llr[i] : llr[i]);
  qsort(buf, G, sizeof(*buf), cmp_u16);  /* ponytail: O(G log G); quickselect if it shows in profiles */
  return (float)buf[G / 2];
}

static int bin_of(float r) { int b = (int)(r / NR_LLRCONF_BIN_W); return b < 0 ? 0 : (b >= NR_LLRCONF_BINS ? NR_LLRCONF_BINS - 1 : b); }

void nr_llrconf_observe(uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm); if (q < 0 || !llr || !truth || G < qm) return;
  const float med = median_abs(llr, G); if (med <= 0.0f) return;
  for (uint32_t m = 0; m < G / qm; m++) {
    int ok = 1; int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      if (a < mn) { mn = a; } if ((uint8_t)(v < 0) != (truth[m * qm + b] & 1)) ok = 0; }
    atomic_fetch_add_explicit(ok ? &g_right[q][bin_of(mn / med)] : &g_wrong[q][bin_of(mn / med)], 1, memory_order_relaxed);
  }
}

int nr_llrconf_threshold(uint8_t qm, float *tau) {
  const int q = qidx(qm); if (q < 0 || atomic_load(&g_disabled)) return 0;
  uint64_t tot = 0, wr = 0;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) { tot += g_right[q][b] + g_wrong[q][b]; }
  if (tot < NR_LLRCONF_MIN_SYMBOLS) return 0;
  uint64_t kept = tot;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) wr += g_wrong[q][b];
  for (int b = 0; b < NR_LLRCONF_BINS; b++) {
    if (kept > 0 && (double)wr / (double)kept < NR_LLRCONF_TARGET_ERR) { *tau = b * NR_LLRCONF_BIN_W; return 1; }
    kept -= g_right[q][b] + g_wrong[q][b]; wr -= g_wrong[q][b];
  }
  return 0;
}

uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau, uint8_t *bits, uint8_t *keep) {
  if (qidx(qm) < 0 || !llr || G < qm) return 0;
  const float med = median_abs(llr, G); uint32_t kept = 0;
  for (uint32_t m = 0; m < G / qm; m++) {
    int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      if (a < mn) { mn = a; } bits[m * qm + b] = (uint8_t)(v < 0); }
    keep[m] = (uint8_t)(med > 0.0f && (float)mn / med >= tau);
    kept += keep[m];
  }
  atomic_fetch_add_explicit(&g_kept_sym, kept, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_total_sym, G / qm, memory_order_relaxed);
  return kept;
}

void nr_llrconf_agreement(uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm); if (q < 0 || !llr || !truth) return;
  uint64_t a = 0, d = 0;
  for (uint32_t i = 0; i < G; i++) ((uint8_t)(llr[i] < 0) == (truth[i] & 1)) ? a++ : d++;
  const uint64_t A = atomic_fetch_add(&g_agree[q], a) + a, D = atomic_fetch_add(&g_disagree[q], d) + d;
  /* Raw bit agreement must be far above 50 % on CRC-OK grants; below it, sign or packing is wrong. */
  if (A + D >= 10000 && A < D && !atomic_exchange(&g_disabled, 1))
    fprintf(stderr, "SENSING: LLRCONF DISABLED -- CRC-OK hard decisions disagree with the re-encoded bits "
            "(agree=%llu disagree=%llu): LLR sign convention or bit packing mismatch\n",
            (unsigned long long)A, (unsigned long long)D);
}

int nr_llrconf_disabled(void) { return atomic_load(&g_disabled); }

void nr_llrconf_stats_dump(void) {
  for (int q = 0; q < 4; q++) { float tau = -1.0f; const int cal = nr_llrconf_threshold((uint8_t)(2 * q + 2), &tau);
    const uint64_t A = g_agree[q], D = g_disagree[q];
    fprintf(stderr, "SENSING: LLRCONF qm=%d calibrated=%d tau_rel=%.2f crc_ok_bit_agreement=%.4f disabled=%d\n",
            2 * q + 2, cal, tau, (A + D) ? (double)A / (double)(A + D) : 0.0, (int)g_disabled); }
  fprintf(stderr, "SENSING: LLRCONF masked_re kept=%llu offered=%llu\n",
          (unsigned long long)g_kept_sym, (unsigned long long)g_total_sym);
}

void nr_llrconf_reset(void) {
  memset((void *)g_right, 0, sizeof g_right); memset((void *)g_wrong, 0, sizeof g_wrong);
  memset((void *)g_agree, 0, sizeof g_agree); memset((void *)g_disagree, 0, sizeof g_disagree); g_disabled = 0;
  g_kept_sym = 0; g_total_sym = 0;
}
