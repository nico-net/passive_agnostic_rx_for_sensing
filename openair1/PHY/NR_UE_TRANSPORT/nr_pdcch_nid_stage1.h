/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
#include "nr_pdcch_nid_gpu.h"
#include <math.h>
#ifndef NID_FN
#define NID_FN
#endif
#define NID_IDS 65536
#define NID_CELLS (14 * NID_IDS)
#define NID_CAL 1024
/* Secondary symbols use a fixed, uniformly spaced null panel, independent of
 * the first-symbol screen. No selected-ID bias in median/MAD calibration.
 * The sum/sqrt(D) statistic requires z > 2 on EVERY constituent symbol. This
 * both rejects padded durations and lets us skip secondary Gold sweeps for
 * >95% of IDs. Lambda is measured AFTER this gate, separately for each D/sym.
 * See T19.md for the family-wise Poisson model and its limitations. */
NID_FN static inline double nid_poisson_tail(double lambda, int k)
{
  if (lambda == 0) return 0;
  if (k <= lambda) return 1; /* only upper tails are needed by the threshold */
  double term = exp(-lambda + k * log(lambda) - lgamma(k + 1.0)), sum = term;
  for (int j = k + 1; j < k + 10000; j++) {
    term *= lambda / j;
    sum += term;
    if (term < sum * 1e-15) break;
  }
  return sum;
}
NID_FN static inline int nid_threshold(double lambda, int families)
{
  int k = 1;
  while (k < 65536 && families * (double)NID_IDS * nid_poisson_tail(lambda, k) >= 0.01) k++;
  return k;
}
NID_FN static inline void nid_shortlist(const uint16_t *hits, const float *zs, int duration,
                                       nr_pdcch_nid_result_t *out)
{
  int families = 0;
  for (int d = 1; d <= duration; d++) families += 15 - d;
  out->list_n = 0;
  for (int r = 0; r < 2; r++) {
    float best = -1;
    out->hit_hash[r] = 1469598103934665603ULL;
    for (int d = 0; d < duration; d++) for (int s = 0; s < 14-d; s++) {
      int base = (2*d+r)*NID_CELLS+s*NID_IDS;
      double total = 0;
      for (int id = 0; id < NID_IDS; id++) {
        total += hits[base+id];
        out->hit_hash[r] = (out->hit_hash[r]^hits[base+id])*1099511628211ULL;
        if (!d) {
          float v = hits[base+id]+zs[base+id]*1e-4f;
          if (v > best) {best=v;out->top_id[r]=id;out->top_sym[r]=s;}
        }
      }
      double lambda = total / NID_IDS;
      int threshold = nid_threshold(lambda, families);
      if (!r) continue;
      out->lambda[d][s] = lambda;
      out->threshold[d][s] = threshold;
      for (int id = 0; id < NID_IDS; id++) {
        int h = hits[base+id];
        if (h < threshold) continue;
        /* Score of accepted occasions; coherent evidence across more symbols
         * wins over its strict subwindows. Stable scan order breaks ties. */
        float strength = zs[base+id];
        bool dominated = false;
        for (int i = 0; i < out->list_n; i++)
          if (out->list_id[i] == id && s < out->list_sym[i]+out->list_dur[i]
              && out->list_sym[i] < s+d+1 && strength <= out->list_score[i]) dominated=true;
        if (dominated) continue;
        // A longer interval may cover several previously selected subwindows.
        for (int i=0; i<out->list_n;) {
          if (out->list_id[i]!=id || s>=out->list_sym[i]+out->list_dur[i] || out->list_sym[i]>=s+d+1) {i++;continue;}
          for (int j=i; j+1<out->list_n; j++) {
            out->list_id[j]=out->list_id[j+1];out->list_sym[j]=out->list_sym[j+1];
            out->list_dur[j]=out->list_dur[j+1];out->list_score[j]=out->list_score[j+1];
          }
          out->list_n--;
        }
        int at=0;
        while (at<out->list_n && out->list_score[at]>=strength) at++;
        if (at>=40) continue;
        if (out->list_n<40) out->list_n++;
        for (int i=out->list_n-1;i>at;i--) {
          out->list_id[i]=out->list_id[i-1];out->list_sym[i]=out->list_sym[i-1];
          out->list_dur[i]=out->list_dur[i-1];out->list_score[i]=out->list_score[i-1];
        }
        out->list_id[at]=id;out->list_sym[at]=s;out->list_dur[at]=d+1;out->list_score[at]=strength;
      }
    }
  }
}
