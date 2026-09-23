#include "nr_llr_confidence.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static _Atomic uint64_t g_right[NR_LLRCONF_NSRC][4][NR_LLRCONF_BINS], g_wrong[NR_LLRCONF_NSRC][4][NR_LLRCONF_BINS];
static _Atomic uint64_t g_agree[NR_LLRCONF_NSRC][4], g_disagree[NR_LLRCONF_NSRC][4];
static _Atomic int g_disabled[NR_LLRCONF_NSRC];
static _Atomic uint64_t g_src[NR_LLRCONF_NSRC][NR_LLRCONF_NCNT]; /* spec §6, per source */
/* CRC-OK DM-RS SNR histogram per Qm: 0.1 dB bins over a range wide enough for any path's scale. */
#define SNR_LO_DB (-100.0f)
#define SNR_BIN_DB 0.1f
#define SNR_BINS 2500
static _Atomic uint64_t g_snr_hist[NR_LLRCONF_NSRC][4][SNR_BINS], g_snr_n[NR_LLRCONF_NSRC][4];

static int qidx(uint8_t qm) { return (qm == 2 || qm == 4 || qm == 6 || qm == 8) ? qm / 2 - 1 : -1; }
static int sidx(int src) { return (src >= 0 && src < NR_LLRCONF_NSRC) ? src : -1; }

/* Median |LLR| of the grant: the per-TB scale every ratio below is expressed in. Exact (the
 * (G/2)-th smallest |LLR|, as a sort would give) from a counting histogram, O(G + 32769): this runs
 * per grant on the RT thread. 32769 bins so |INT16_MIN| = 32768 keeps its own bin. */
float nr_llrconf_median_abs(const int16_t *llr, uint32_t G) {
  static __thread uint32_t *hist = NULL;
  if (G == 0 || (!hist && !(hist = malloc(sizeof(*hist) * 32769)))) return 0.0f;
  memset(hist, 0, sizeof(*hist) * 32769);
  for (uint32_t i = 0; i < G; i++) hist[llr[i] < 0 ? -(int)llr[i] : llr[i]]++;
  uint32_t c = 0;
  for (int v = 0; v < 32769; v++) { c += hist[v]; if (c > G / 2) return (float)v; }
  return 0.0f;
}

static int bin_of(float r) { int b = (int)(r / NR_LLRCONF_BIN_W); return b < 0 ? 0 : (b >= NR_LLRCONF_BINS ? NR_LLRCONF_BINS - 1 : b); }

/* Per-symbol float divisions dominated the RT cost. Both tests below are monotone in the integer
 * min|LLR| (0..32767), so each is replaced by integer edges found with the SAME float expression:
 * bit-identical results, O(log) divisions per grant instead of one per symbol. */
static int first_mn_bin(float med, int b) { int lo = 0, hi = 32768;
  while (lo < hi) { const int mid = (lo + hi) / 2; if (bin_of(mid / med) >= b) hi = mid; else lo = mid + 1; } return lo; }
static int first_mn_tau(float med, float tau) { int lo = 0, hi = 32768;
  while (lo < hi) { const int mid = (lo + hi) / 2; if ((float)mid / med >= tau) hi = mid; else lo = mid + 1; } return lo; }

void nr_llrconf_observe(int src, uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm), s = sidx(src); if (q < 0 || s < 0 || !llr || !truth || G < qm) return;
  const float med = nr_llrconf_median_abs(llr, G); if (med <= 0.0f) return;
  static __thread uint8_t *lut = NULL; /* min|LLR| -> bin, exactly bin_of(mn / med) */
  if (!lut && !(lut = malloc(32768))) return;
  for (int b = 0, e0 = 0; b < NR_LLRCONF_BINS; b++) {
    const int e1 = (b + 1 < NR_LLRCONF_BINS) ? first_mn_bin(med, b + 1) : 32768;
    memset(lut + e0, b, (size_t)(e1 - e0)); e0 = e1;
  }
  uint32_t right[NR_LLRCONF_BINS] = {0}, wrong[NR_LLRCONF_BINS] = {0}; /* one atomic per bin, not per symbol */
  for (uint32_t m = 0; m < G / qm; m++) {
    int bad = 0; int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      mn = a < mn ? a : mn; bad |= (v < 0) ^ (truth[m * qm + b] & 1); }
    (bad ? wrong : right)[lut[mn]]++;
  }
  for (int b = 0; b < NR_LLRCONF_BINS; b++) {
    if (right[b]) atomic_fetch_add_explicit(&g_right[s][q][b], right[b], memory_order_relaxed);
    if (wrong[b]) atomic_fetch_add_explicit(&g_wrong[s][q][b], wrong[b], memory_order_relaxed);
  }
}

int nr_llrconf_threshold(int src, uint8_t qm, float *tau) {
  const int q = qidx(qm), s = sidx(src); if (q < 0 || s < 0 || atomic_load(&g_disabled[s])) return 0;
  uint64_t tot = 0, wr = 0;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) { tot += g_right[s][q][b] + g_wrong[s][q][b]; }
  if (tot < NR_LLRCONF_MIN_SYMBOLS) return 0;
  uint64_t kept = tot;
  for (int b = 0; b < NR_LLRCONF_BINS; b++) wr += g_wrong[s][q][b];
  for (int b = 0; b < NR_LLRCONF_BINS; b++) {
    if (kept > 0 && (double)wr / (double)kept < NR_LLRCONF_TARGET_ERR) { *tau = b * NR_LLRCONF_BIN_W; return 1; }
    kept -= g_right[s][q][b] + g_wrong[s][q][b]; wr -= g_wrong[s][q][b];
  }
  return 0;
}

uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau, uint8_t *bits, uint8_t *keep) {
  if (qidx(qm) < 0 || !llr || G < qm) return 0;
  const float med = nr_llrconf_median_abs(llr, G); uint32_t kept = 0;
  const int t = med > 0.0f ? first_mn_tau(med, tau) : 32768; /* keep <=> (float)mn / med >= tau */
  for (uint32_t m = 0; m < G / qm; m++) {
    int mn = 32767;
    for (int b = 0; b < qm; b++) { const int16_t v = llr[m * qm + b]; const int a = v < 0 ? -v : v;
      if (a < mn) { mn = a; } bits[m * qm + b] = (uint8_t)(v < 0); }
    keep[m] = (uint8_t)(mn >= t);
    kept += keep[m];
  }
  return kept;
}

void nr_llrconf_agreement(int src, uint8_t qm, const int16_t *llr, const uint8_t *truth, uint32_t G) {
  const int q = qidx(qm), s = sidx(src); if (q < 0 || s < 0 || !llr || !truth) return;
  uint32_t dd = 0;
  for (uint32_t i = 0; i < G; i++) dd += (uint32_t)((llr[i] < 0) ^ (truth[i] & 1)); /* branchless: vectorises */
  const uint64_t d = dd, a = (uint64_t)G - dd;
  const uint64_t A = atomic_fetch_add(&g_agree[s][q], a) + a, D = atomic_fetch_add(&g_disagree[s][q], d) + d;
  /* Raw bit agreement must be far above 50 % on CRC-OK grants; below it, sign or packing is wrong. */
  if (A + D >= 10000 && A < D && !atomic_exchange(&g_disabled[s], 1))
    fprintf(stderr, "SENSING: LLRCONF DISABLED (src=%s) -- CRC-OK hard decisions disagree with the re-encoded bits "
            "(agree=%llu disagree=%llu): LLR sign convention or bit packing mismatch\n",
            s == NR_LLRCONF_SRC_DL ? "dl" : "ul", (unsigned long long)A, (unsigned long long)D);
}

int nr_llrconf_disabled(int src) { const int s = sidx(src); return s < 0 ? 1 : atomic_load(&g_disabled[s]); }

void nr_llrconf_stats_dump(void) {
  static const char *src_name[NR_LLRCONF_NSRC] = {"dl", "ul"};
  for (int s = 0; s < NR_LLRCONF_NSRC; s++)
  for (int q = 0; q < 4; q++) { float tau = -1.0f; const int cal = nr_llrconf_threshold(s, (uint8_t)(2 * q + 2), &tau);
    const uint64_t A = g_agree[s][q], D = g_disagree[s][q];
    fprintf(stderr, "SENSING: LLRCONF src=%s qm=%d calibrated=%d tau_rel=%.2f crc_ok_bit_agreement=%.4f disabled=%d\n",
            src_name[s], 2 * q + 2, cal, tau, (A + D) ? (double)A / (double)(A + D) : 0.0, (int)g_disabled[s]); }
  for (int s = 0; s < NR_LLRCONF_NSRC; s++)
    fprintf(stderr, "SENSING: LLRCONF src=%s submitted_crc_ok=%llu submitted_masked=%llu snr_gate_rejected=%llu "
            "masked_re kept=%llu offered=%llu\n", src_name[s],
            (unsigned long long)g_src[s][NR_LLRCONF_CNT_CRC_OK], (unsigned long long)g_src[s][NR_LLRCONF_CNT_MASKED],
            (unsigned long long)g_src[s][NR_LLRCONF_CNT_SNR_REJECT], (unsigned long long)g_src[s][NR_LLRCONF_CNT_RE_KEPT],
            (unsigned long long)g_src[s][NR_LLRCONF_CNT_RE_OFFERED]);
}

void nr_llrconf_reset(void) {
  memset((void *)g_right, 0, sizeof g_right); memset((void *)g_wrong, 0, sizeof g_wrong);
  memset((void *)g_agree, 0, sizeof g_agree); memset((void *)g_disagree, 0, sizeof g_disagree); memset((void *)g_disabled, 0, sizeof g_disabled);
  memset((void *)g_src, 0, sizeof g_src); memset((void *)g_snr_hist, 0, sizeof g_snr_hist);
  memset((void *)g_snr_n, 0, sizeof g_snr_n);
}

void nr_llrconf_pack(const uint8_t *bits, uint32_t G, uint8_t *packed) {
  for (uint32_t j = 0; j < G / 8; j++) {
    const uint8_t *h = &bits[8 * j];
    packed[j] = (uint8_t)(h[0] | h[1] << 1 | h[2] << 2 | h[3] << 3 | h[4] << 4 | h[5] << 5 | h[6] << 6 | h[7] << 7);
  }
  if (G & 7) {
    uint8_t t = 0;
    for (uint32_t i = G & ~7u; i < G; i++) t |= (uint8_t)(bits[i] << (i & 7));
    packed[G >> 3] = t;
  }
}

void nr_llrconf_unpack(const uint8_t *packed, uint32_t G, uint8_t *bits) {
  for (uint32_t j = 0; j < G / 8; j++) {
    const uint8_t c = packed[j];
    for (int b = 0; b < 8; b++) bits[8 * j + b] = (c >> b) & 1;
  }
  for (uint32_t i = G & ~7u; i < G; i++) bits[i] = (packed[i >> 3] >> (i & 7)) & 1;
}

static int snr_bin(float snr_db) { const int b = (int)floorf((snr_db - SNR_LO_DB) / SNR_BIN_DB);
  return b < 0 ? 0 : (b >= SNR_BINS ? SNR_BINS - 1 : b); }

void nr_llrconf_snr_observe(int src, uint8_t qm, float snr_db) {
  const int q = qidx(qm), s = sidx(src); if (q < 0 || s < 0 || !isfinite(snr_db)) return;
  atomic_fetch_add_explicit(&g_snr_hist[s][q][snr_bin(snr_db)], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_snr_n[s][q], 1, memory_order_relaxed);
}

/* O(SNR_BINS) worst case, typically a short scan: the p05 sits in the low tail. */
int nr_llrconf_snr_eligible(int src, uint8_t qm, float snr_db) {
  const int q = qidx(qm), s = sidx(src); if (q < 0 || s < 0 || !isfinite(snr_db)) return 0;
  const uint64_t n = atomic_load_explicit(&g_snr_n[s][q], memory_order_relaxed);
  if (n < NR_LLRCONF_MIN_SNR_GRANTS) return 0;
  const uint64_t target = (uint64_t)ceil(NR_LLRCONF_SNR_QUANTILE * (double)n);
  uint64_t c = 0; int b = 0;
  for (; b < SNR_BINS; b++) { c += atomic_load_explicit(&g_snr_hist[s][q][b], memory_order_relaxed); if (c >= target) break; }
  return snr_bin(snr_db) >= b;
}

void nr_llrconf_count(int src, int what, uint64_t n) {
  if (src >= 0 && src < NR_LLRCONF_NSRC && what >= 0 && what < NR_LLRCONF_NCNT && n)
    atomic_fetch_add_explicit(&g_src[src][what], n, memory_order_relaxed);
}

uint64_t nr_llrconf_counter(int src, int what) {
  return (src >= 0 && src < NR_LLRCONF_NSRC && what >= 0 && what < NR_LLRCONF_NCNT) ? atomic_load(&g_src[src][what]) : 0;
}
