/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_cb0_fixture.h"
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "common/utils/threadPool/thread-pool.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_extern.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"

uint8_t get_BG(uint32_t A, uint16_t R); /* openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c */
uint32_t nr_compute_tbs(uint16_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nb_symb_sch, uint16_t nb_dmrs_prb,
                        uint16_t nb_rb_oh, uint8_t tb_scaling, uint8_t Nl);
int32_t nrLDPC_coding_decoder(nrLDPC_slot_decoding_parameters_t *p); /* nrLDPC_coding_segment_decoder.c */

/* nr_mac_common.c (pulled in for get_BG) references this from code the fixture never runs; the softmodems define it. */
__attribute__((weak)) void *get_softmodem_params(void)
{
  return NULL;
}

static tpool_t g_pool;
static int g_init;

void cb0_fx_init(void)
{
  if (g_init)
    return;
  g_init = 1;
  crcTableInit();
  logInit();
  char p[] = "n"; /* no worker threads: tasks run inline in pushTpool's caller */
  initTpool(p, &g_pool, false);
  nr_td_cb0_set_ldpc_decoder((void *)&LDPCdecoder);
}

void cb0_fx_reinit_decoder(void)
{
  nr_td_cb0_set_ldpc_decoder((void *)&LDPCdecoder);
}

uint8_t cb0_fx_get_bg(uint32_t A, uint16_t R)
{
  return get_BG(A, R);
}

uint32_t cb0_fx_G(uint16_t nb_rb, uint16_t nsym, uint8_t nb_re_dmrs, uint16_t dmrs_len, uint8_t Qm, uint8_t Nl)
{
  return nr_get_G(nb_rb, nsym, nb_re_dmrs, dmrs_len, 0, Qm, Nl);
}

uint32_t cb0_fx_tbs(uint8_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nsym, uint16_t nb_dmrs_prb, uint8_t Nl)
{
  return nr_compute_tbs(Qm, R, nb_rb, nsym, nb_dmrs_prb, 0, 0, Nl);
}

int cb0_fx_encode(cb0_fx_t *fx, uint32_t seed, int amp)
{
  const uint32_t A = fx->A;
  fx->llr = NULL;
  fx->BG = get_BG(A, fx->R);
  uint8_t *a = calloc(A / 8 + 8, 1);
  for (uint32_t i = 0; i < A / 8; i++)
    a[i] = (uint8_t)(rand_r(&seed) >> 7);
  uint32_t crc, B;
  if (A > NR_MAX_PDSCH_TBS) { /* exactly nr_dlsch_coding.c */
    crc = crc24a(a, A) >> 8;
    a[A >> 3] = ((uint8_t *)&crc)[2];
    a[1 + (A >> 3)] = ((uint8_t *)&crc)[1];
    a[2 + (A >> 3)] = ((uint8_t *)&crc)[0];
    B = A + 24;
  } else {
    crc = crc16(a, A) >> 16;
    a[A >> 3] = ((uint8_t *)&crc)[1];
    a[1 + (A >> 3)] = ((uint8_t *)&crc)[0];
    B = A + 16;
  }
  unsigned int C, K, Z, F;
  if (nr_segmentation(NULL, NULL, B, &C, &K, &Z, &F, fx->BG) < 0) {
    free(a);
    return -1;
  }
  uint8_t **c = calloc(C, sizeof(*c));
  for (uint32_t r = 0; r < C; r++)
    c[r] = calloc(K / 8 + 64, 1);
  const int Kb = nr_segmentation(a, c, B, &C, &K, &Z, &F, fx->BG);
  fx->C = C; fx->K = K; fx->Z = Z; fx->F = F;
  fx->llr = calloc(fx->G + 64, sizeof(int16_t));
  int rc = 0;
  uint32_t off = 0;
  for (uint32_t r = 0; r < C && rc == 0; r++) {
    const int E = nr_get_E(fx->G, C, fx->Qm, fx->Nl, r);
    uint8_t dout[68 * 384] = {0};
    encoder_implemparams_t impp = {.Zc = Z, .Kb = Kb, .BG = fx->BG, .K = K, .gen_code = 0};
    unsigned char *in[1] = {c[r]};
    LDPCencoder(in, dout, &impp);
    uint8_t *e = calloc(E + 64, 1), *f = calloc(E + 64, 1);
    if (nr_rate_matching_ldpc(fx->tbslbrm, fx->BG, Z, dout, e, C, F, K - F - 2 * Z, fx->rv, E) != 0) {
      rc = -1;
    } else {
      nr_interleaving_ldpc(E, fx->Qm, e, f);
      for (int k = 0; k < E; k++)
        fx->llr[off + k] = f[k] ? -amp : amp;
      off += E;
    }
    free(e);
    free(f);
  }
  for (uint32_t r = 0; r < C; r++)
    free(c[r]);
  free(c);
  fx->payload = a;
  if (rc) {
    free(fx->llr);
    fx->llr = NULL;
    free(fx->payload);
    fx->payload = NULL;
  }
  return rc;
}

void cb0_fx_free(cb0_fx_t *fx)
{
  free(fx->llr);
  fx->llr = NULL;
  free(fx->payload);
  fx->payload = NULL;
}

uint32_t cb0_fx_tb_crc(const uint8_t *a, uint32_t A)
{
  return A > NR_MAX_PDSCH_TBS ? crc24a((uint8_t *)a, A) >> 8 : crc16((uint8_t *)a, A) >> 16;
}

nr_td_cb0_item_t cb0_fx_item(const cb0_fx_t *fx)
{
  nr_td_cb0_item_t it = {.llr = fx->llr, .G = fx->G, .Qm = fx->Qm, .Nl = fx->Nl, .rv = fx->rv, .tbs = fx->A,
                         .mcs_table = 0, .tbslbrm = fx->tbslbrm, .max_iter = 8, .R = fx->R, .bg = 0};
  return it;
}

/* passive_ldpc_decode_core (nr_pdsch_passive_decode.c), minus its HARQ / statistics side effects. */
static int rx_decode(const nr_td_cb0_item_t *it, int probe, int *seg0_ok, const uint8_t *expect, int *bits_match)
{
  if (bits_match)
    *bits_match = 0;
  nrLDPC_TB_decoding_parameters_t *TB = calloc(1, sizeof(*TB));
  decode_abort_t ab;
  init_abort(&ab);
  uint32_t processed = 0;
  nrLDPC_slot_decoding_parameters_t slot = {.frame = 0, .slot = 0, .nb_TBs = 1, .threadPool = &g_pool, .TBs = TB};
  TB->harq_unique_pid = 0;
  TB->G = it->G;
  TB->Qm = it->Qm;
  TB->nb_layers = it->Nl;
  TB->BG = it->bg ? it->bg : get_BG(it->tbs, it->R);
  TB->A = it->tbs;
  TB->processedSegments = &processed;
  if (nr_segmentation(NULL, NULL, lenWithCrc(1, TB->A), &TB->C, &TB->K, &TB->Z, &TB->F, TB->BG) < 0) {
    free(TB);
    return -1;
  }
  TB->max_ldpc_iterations = it->max_iter;
  TB->rv_index = it->rv;
  TB->tbslbrm = it->tbslbrm;
  TB->abort_decode = &ab;
  set_abort(&ab, false);
  TB->llr = (short *)it->llr;
  uint8_t *c = calloc((size_t)TB->C * (TB->K >> 3) + 64, 1);
  uint8_t *b = calloc((size_t)TB->C * (TB->K >> 3) + 64, 1);
  int16_t *d = calloc((size_t)TB->C * 68 * 384, sizeof(int16_t));
  TB->c = c;
  TB->d = d;
  int llrLen;
  TB->E = nr_get_E(TB->G, TB->C, TB->Qm, TB->nb_layers, 0);
  TB->E2 = TB->E;
  TB->first_rE2 = TB->C;
  TB->R = nr_get_R_ldpc_decoder(TB->rv_index, TB->E, TB->BG, TB->Z, &llrLen, 0);
  for (uint32_t r = 1; r < TB->C; r++) {
    const int Er = nr_get_E(TB->G, TB->C, TB->Qm, TB->nb_layers, r);
    if (Er != TB->E) {
      TB->E2 = Er;
      TB->R2 = nr_get_R_ldpc_decoder(TB->rv_index, Er, TB->BG, TB->Z, &llrLen, 0);
      TB->first_rE2 = r;
      break;
    }
  }
  TB->d_to_be_cleared = true;
  const uint32_t C_full = TB->C;
  TB->nb_segments_to_decode = (probe && C_full > 1) ? 1 : 0;
  const uint32_t C_dec = TB->nb_segments_to_decode ? 1 : C_full;
  for (uint32_t r = 0; r < TB->C; r++)
    TB->decodeSuccess[r] = false;
  nrLDPC_coding_decoder(&slot);
  int pass;
  uint32_t seg_ok = 0;
  for (uint32_t r = 0; r < C_dec; r++)
    seg_ok += TB->decodeSuccess[r];
  if (seg0_ok)
    *seg0_ok = TB->decodeSuccess[0];
  if (probe && C_full > 1) {
    pass = seg_ok == 1;
    if (pass) {
      const uint32_t seg_bytes = (TB->K >> 3) - (TB->F >> 3) - 3;
      uint32_t i = 0;
      while (i < seg_bytes && c[i] == 0)
        i++;
      if (i == seg_bytes)
        pass = 0;
    }
  } else if (seg_ok != C_full) {
    pass = 0;
  } else {
    uint32_t offset = 0, r_offset = 0;
    for (uint32_t r = 0; r < C_full; r++) {
      const uint32_t seg_bytes = (TB->K >> 3) - (TB->F >> 3) - ((C_full > 1) ? 3 : 0);
      memcpy(b + offset, c + r_offset, seg_bytes);
      offset += seg_bytes;
      r_offset += (TB->K >> 3);
    }
    pass = 1;
    const uint32_t sz = it->tbs / 8;
    if (C_full > 1 && !check_crc(b, lenWithCrc(1, it->tbs), crcType(1, it->tbs)))
      pass = 0;
    if (pass && expect && bits_match) {
      const uint32_t crc = cb0_fx_tb_crc(b, it->tbs);
      const int ncrc = it->tbs > NR_MAX_PDSCH_TBS ? 3 : 2;
      int crc_ok = 1;
      for (int q = 0; q < ncrc; q++)
        crc_ok &= b[sz + q] == ((uint8_t *)&crc)[ncrc - 1 - q];
      *bits_match = memcmp(b, expect, sz) == 0 && crc_ok;
    }
    if (pass && b[sz] == 0 && b[sz + 1] == 0) {
      uint32_t i = 0;
      while (i < sz && b[i] == 0)
        i++;
      if (i == sz)
        pass = 0;
    }
  }
  free(c);
  free(b);
  free(d);
  free(TB);
  return pass;
}

int cb0_fx_rx_decode(const nr_td_cb0_item_t *it, int probe, int *seg0_ok)
{
  return rx_decode(it, probe, seg0_ok, NULL, NULL);
}

int cb0_fx_rx_decode_bits(const nr_td_cb0_item_t *it, const uint8_t *expect, int *bits_match)
{
  return rx_decode(it, 0, NULL, expect, bits_match);
}
