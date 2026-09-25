#include "nr_pdsch_qm_oracle.h"
#include <math.h>
#include <stddef.h>

int nr_pdsch_qm_of_mcs(uint8_t mcs, uint8_t mcs_table)
{
  if (mcs > 31 || mcs_table > 2)
    return 0;
  static const uint8_t first4[3] = {10, 5, 15}, first6[3] = {17, 11, 21}, first8[3] = {255, 20, 255};
  static const uint8_t reserved[3] = {29, 28, 29};
  if (mcs >= reserved[mcs_table])
    return 2 + 2 * (mcs - reserved[mcs_table]); /* reserved indices: Qm 2,4,6(,8) in order */
  if (mcs >= first8[mcs_table]) return 8;
  if (mcs >= first6[mcs_table]) return 6;
  if (mcs >= first4[mcs_table]) return 4;
  return 2;
}

/* Per-dimension squared residual to the nearest odd-integer level (clipped to +-lmax), after scaling
 * the stream to the grid's ideal power, divided by the uniform floor 1/3. */
static double t_on_grid(const int16_t *iq, uint32_t n, double p, int qm)
{
  const int lmax = (1 << (qm / 2)) - 1;
  double ms = 0.0;
  int nl = 0;
  for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nl++; }
  const double scale = sqrt(2.0 * ms / nl / p);
  double err = 0.0;
  for (uint32_t i = 0; i < 2 * n; i++) {
    const double v = iq[i] * scale;
    double s = 2.0 * floor(v / 2.0) + 1.0;
    if (s > lmax) s = lmax;
    else if (s < -lmax) s = -lmax;
    err += (v - s) * (v - s);
  }
  return (err / (2.0 * n)) / (1.0 / 3.0);
}

int nr_pdsch_qm_classify(const int16_t *iq, uint32_t n, double *t_best)
{
  if (t_best) *t_best = -1.0;
  if (iq == NULL || n < NR_QM_ORACLE_MIN_N)
    return 0;
  double p = 0.0;
  for (uint32_t i = 0; i < 2 * n; i++)
    p += (double)iq[i] * iq[i];
  p /= (double)n;
  if (p <= 0.0)
    return 0;
  static const int qms[4] = {2, 4, 6, 8};
  double t[4];
  int b = 0, s = -1;
  for (int k = 0; k < 4; k++) {
    t[k] = t_on_grid(iq, n, p, qms[k]);
    if (t[k] < t[b]) b = k;
  }
  for (int k = 0; k < 4; k++)
    if (k != b && (s < 0 || t[k] < t[s])) s = k;
  if (t_best) *t_best = t[b];
  return (t[b] < NR_QM_ORACLE_T_MAX && t[b] < NR_QM_ORACLE_RATIO * t[s]) ? qms[b] : 0;
}
