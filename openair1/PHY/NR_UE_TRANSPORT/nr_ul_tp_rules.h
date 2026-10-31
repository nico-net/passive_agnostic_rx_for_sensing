/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_UL_TP_RULES_H
#define NR_UL_TP_RULES_H
#include <stdbool.h>
/* TS 38.211 6.3.1.4: DFT-s-OFDM (transform precoding) PUSCH spans M_RB = 2^a 3^b 5^c PRBs. A gNB never schedules anything else, so a reading
 * of a DCI 0_1 that combines transform precoding with another width is refuted by the specification, not merely unlikely. */
static inline bool nr_ul_tp_nrb_legal(int n)
{
  if (n < 1) return false;
  while (n % 2 == 0) n /= 2;
  while (n % 3 == 0) n /= 3;
  while (n % 5 == 0) n /= 5;
  return n == 1;
}
/* Refutation of a transform-precoding tail from its readings under the leading BWP hypothesis: a true TP cell reads only legal widths (a few
 * false DCIs aside), a CP-OFDM cell read as TP is illegal on ~70 % of the widths. */
static inline bool nr_ul_tp_tail_refuted(unsigned illegal, unsigned legal) { return illegal >= 4 && illegal >= 2u * legal; }
#endif
