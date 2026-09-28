#ifndef NR_SSB_RATE_MATCH_H
#define NR_SSB_RATE_MATCH_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

enum { NR_SSB_RM_MAX_RB = 275 };
typedef struct {
  int frame, slot, pci;
  uint16_t symbols;
  uint16_t first_crb, last_crb;
} nr_ssb_rm_event_t;
typedef struct {
  uint16_t symbols;
  uint8_t prb[NR_SSB_RM_MAX_RB];
} nr_ssb_rm_mask_t;

/* TS 38.214 5.1.4: the entire containing PRB, including partial edge PRBs. */
static inline uint16_t nr_ssb_rm_excluded(const nr_ssb_rm_mask_t *mask, int symbol, int bwp_rb)
{
  return mask && symbol >= 0 && symbol < 14 && bwp_rb >= 0 && bwp_rb < NR_SSB_RM_MAX_RB
      && ((mask->symbols >> symbol) & 1) && mask->prb[bwp_rb] ? 0xfff : 0;
}
static inline bool nr_ssb_rm_event_valid(const nr_ssb_rm_event_t *e, int frame, int slot, int pci)
{
  return e && e->frame == frame && e->slot == slot && e->pci == pci && e->symbols != 0
      && !(e->symbols & ~0x3fff) && e->first_crb <= e->last_crb && e->last_crb < NR_SSB_RM_MAX_RB;
}

static inline nr_ssb_rm_event_t nr_ssb_rm_event(int frame, int slot, int pci, int first_sc, uint16_t symbols)
{
  nr_ssb_rm_event_t e = {frame, slot, pci, 0, 0, 0};
  if (first_sc < 0 || first_sc + 239 >= 12 * NR_SSB_RM_MAX_RB)
    return e;
  e.symbols = symbols;
  e.first_crb = first_sc / 12;
  e.last_crb = (first_sc + 239) / 12;
  return e;
}

/* Build once in the same BWP/data order used by the extractor. `physical` is
 * the data-ordered BWP-relative list for a segmented/interleaved allocation;
 * NULL gives ordinary BWP order. */
static inline nr_ssb_rm_mask_t nr_ssb_rm_map(const nr_ssb_rm_event_t *e, int frame, int slot, int pci,
                                            int bwp_start, int nrb, const uint16_t *physical)
{
  nr_ssb_rm_mask_t m = {0, {0}};
  if (!nr_ssb_rm_event_valid(e, frame, slot, pci) || nrb < 0 || nrb > NR_SSB_RM_MAX_RB)
    return m;
  m.symbols = e->symbols;
  for (int rb = 0; rb < nrb; ++rb) {
    const int crb = bwp_start + (physical ? physical[rb] : rb);
    m.prb[rb] = crb >= e->first_crb && crb <= e->last_crb;
  }
  return m;
}

/* False-alarm gate of nr_ssb_rm_sync_present(): the squared radius |S|^2 above which a sum S of
 * `nvalid` independent, uniformly distributed unit phasors (noise only) lands with probability at most
 * `pfa`. Each projection of a phasor on a fixed direction is bounded by 1 with variance 1/2, so
 * Bernstein gives P(proj >= t) <= exp(-t^2 / (nvalid + 2t/3)); |S| >= r puts the projection on one of
 * K = 16 equally spaced directions at >= r cos(pi/K), hence P(|S| >= r) <= K exp(-t^2/(nvalid + 2t/3))
 * with t = r cos(pi/K). Solved for t at K exp(...) = pfa. (The former Hoeffding form, 4 exp(-|S|^2/(4N)),
 * needed a coherence of 0.83 at N = 127; this bound needs 0.51 for the same 1e-9.) Deliberately
 * conservative: at N = 127, pfa = 1e-9 the exact asymptotic (Rayleigh) tail exp(-r^2/N) of the noise-only
 * statistic is ~1e-14, i.e. the gate is about five orders of magnitude stricter than its target. */
static inline double nr_ssb_rm_sync_r2(int nvalid, double pfa)
{
  const int K = 16;
  const double L = log(K / pfa);
  const double t = (2.0 * L / 3.0 + sqrt(4.0 * L * L / 9.0 + 4.0 * L * nvalid)) / 2.0;
  const double r = t / cos(M_PI / K);
  return r * r;
}

/* PSS and SSS occupy the same 127 subcarriers two symbols apart. Their product
 * removes a frequency-selective channel and any common inter-symbol CFO phase.
 * Unit normalization prevents a few large interferers from dominating. A fixed
 * false-alarm probability (1e-9 per trial, nr_ssb_rm_sync_r2()), not an
 * RF-amplitude threshold, sets the gate. Structured interferers are tested
 * separately; this is not a guarantee under arbitrary interference. No timing
 * history is reused. Inputs are 127 interleaved complex int16 samples, already
 * FFT-unwrapped. */
static inline bool nr_ssb_rm_sync_present(const int16_t *pss, const int16_t *sss, int pci)
{
  if (!pss || !sss || pci < 0 || pci > 1007) return false;
  int8_t p[127] = {0, 1, 1, 0, 1, 1, 1};
  int8_t a[127] = {1}, b[127] = {1};
  for (int n = 0; n < 120; ++n) {
    p[n + 7] = p[n + 4] ^ p[n];
    a[n + 7] = a[n + 4] ^ a[n];
    b[n + 7] = b[n + 1] ^ b[n];
  }
  const int id1 = pci / 3, id2 = pci % 3;
  const int m0 = 15 * (id1 / 112) + 5 * id2, m1 = id1 % 112;
  double sr = 0, si = 0;
  int nvalid = 0;
  for (int n = 0; n < 127; ++n) {
    const double pr = pss[2*n], pi = pss[2*n+1], qr = sss[2*n], qi = sss[2*n+1];
    const double r = pr * qr + pi * qi, im = pr * qi - pi * qr;
    const double mag = hypot(r, im);
    if (mag == 0) continue;
    const int sign = (1 - 2 * p[(n + 43 * id2) % 127])
                  * (1 - 2 * a[(n + m0) % 127]) * (1 - 2 * b[(n + m1) % 127]);
    sr += sign * r / mag;
    si += sign * im / mag;
    ++nvalid;
  }
  // Too few occupied bins (< 17) cannot meet the bound, even with perfect coherence.
  return nvalid > 0 && sr * sr + si * si > nr_ssb_rm_sync_r2(nvalid, 1e-9);
}
#endif
