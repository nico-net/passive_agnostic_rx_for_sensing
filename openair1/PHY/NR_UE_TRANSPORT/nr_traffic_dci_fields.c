/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_traffic_dci_fields.h"

static void unknown(nr_dci_harq_fields_t *o) { o->dai = o->tpc = o->pri = o->k1 = -1; }

static int fld(nr_dci_bits_t p, uint16_t len, int pos, int w) { return (int)nr_dci_bits_field(&p, len, pos, w); }

bool nr_dci10_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, uint16_t fdra_bits, nr_dci_harq_fields_t *out)
{
  if (!out)
    return false;
  unknown(out);
  const int pos = 1 + fdra_bits + 4 + 1 + 5 + 1 + 2 + 4;  /* up to and including HARQ pid */
  if (dci_len > NR_DCI_MAX_PAYLOAD || dci_len < pos + 2 + 2 + 3 + 3)
    return false;
  out->dai = (int8_t)fld(payload, dci_len, pos, 2);
  out->tpc = (int8_t)fld(payload, dci_len, pos + 2, 2);
  out->pri = (int8_t)fld(payload, dci_len, pos + 4, 3);
  out->k1 = (int8_t)fld(payload, dci_len, pos + 7, 3);
  return true;
}

bool nr_dci11_harq_fields(nr_dci_bits_t payload, uint16_t dci_len, const nr_dci11_layout_t *l,
                          const nr_dci11_offsets_t *off, nr_dci_harq_fields_t *out)
{
  if (!out)
    return false;
  unknown(out);
  if (!l || !off || dci_len > NR_DCI_MAX_PAYLOAD || off->ant_ports < l->pre_ant || off->ant_ports > dci_len)
    return false;
  /* Extras (one-shot, group idx, NFI, n groups, retx, enh. type 3) follow K1 in the sweep's pre_ant, so they do not shift the
   * offsets we read, but they widen the sum: reachable extra-bit sums are collected as a mask, and distinct (tb2,harq,dai,tpc2,k1)
   * tuples are counted, not combinations. */
  int n[5];
  const uint8_t *t[5];
  for (int i = 0; i < 5; i++)
    t[i] = nr_dci11_switch_widths((nr_dci11_switch_t)i, &n[i]);
  uint64_t ex = 1;  /* bit s set: extras can sum to s */
  for (int i = NR_DCI11_SW_ONESHOT; i <= NR_DCI11_SW_ENHT3; i++) {
    int m;
    const uint8_t *w = nr_dci11_switch_widths((nr_dci11_switch_t)i, &m);
    uint64_t nx = 0;
    for (int j = 0; j < m; j++)
      nx |= ex << w[j];
    ex = nx;
  }
  int found = 0, tb2 = 0, harq = 0, dai = 0, tpc2 = 0, k1 = 0;
  for (int a = 0; a < n[0]; a++)
    for (int b = 0; b < n[1]; b++)
      for (int c = 0; c < n[2]; c++)
        for (int d = 0; d < n[3]; d++)
          for (int e = 0; e < n[4]; e++) {
            const int rest = l->pre_ant - (t[0][a] + t[1][b] + t[2][c] + 2 /*TPC*/ + t[3][d] + 3 /*PRI*/ + t[4][e]);
            if (rest >= 0 && rest < 64 && ((ex >> rest) & 1)) {
              found++;
              tb2 = t[0][a]; harq = t[1][b]; dai = t[2][c]; tpc2 = t[3][d]; k1 = t[4][e];
            }
          }
  if (found != 1)
    return false;
  int pos = off->ant_ports - l->pre_ant + tb2 + harq;  /* group start, then skip TB2 and HARQ pid */
  if (dai) out->dai = (int8_t)fld(payload, dci_len, pos, dai);  /* semi-static codebook: no DAI field, stays -1 */
  pos += dai;
  out->tpc = (int8_t)fld(payload, dci_len, pos, 2);
  pos += 2 + tpc2;
  out->pri = (int8_t)fld(payload, dci_len, pos, 3);
  pos += 3;
  if (k1) out->k1 = (int8_t)fld(payload, dci_len, pos, k1);
  return true;
}
