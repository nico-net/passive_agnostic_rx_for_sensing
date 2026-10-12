/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdsch_rm_genie.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"

#define RING 128
typedef struct { bool used; uint16_t rnti; uint32_t A, B; int frame, slot; uint8_t *tb; uint64_t seq; } ent_t;
static ent_t g_ring[RING];
static uint64_t g_seq;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static LDPC_encoderfunc_t *g_enc;
static int g_nlines_map, g_nlines_gen;

int nr_pdsch_rm_genie_level(void)
{
  static int s = -1;
  if (s < 0) { const char *e = getenv("ISAC_PDSCH_RM_GENIE"); s = e ? atoi(e) : 0; }
  return s;
}

bool nr_pdsch_rm_genie_on(void)
{
  return nr_pdsch_rm_genie_level() >= 1;
}

/* Expected transmitted bits f[0..G) for TB bytes tb (B/8, CRC included), rv. Returns 0 on success. */
static int expect_bits(const uint8_t *tb, uint32_t B, uint8_t BG, uint8_t Qm, uint8_t rv, uint32_t tbslbrm, uint32_t G, uint8_t *f)
{
  if (!g_enc) {
    void *h = dlopen("libldpc.so", RTLD_NOW | RTLD_LOCAL);
    if (h) g_enc = (LDPC_encoderfunc_t *)dlsym(h, "LDPCencoder");
    if (!g_enc) return -1;
  }
  unsigned int C, K, Z, F;
  if (nr_segmentation(NULL, NULL, B, &C, &K, &Z, &F, BG) < 0 || C != 1) return -1;
  uint8_t *a = calloc(B / 8 + 8, 1);
  uint8_t *c[1] = {calloc(K / 8 + 64, 1)};
  memcpy(a, tb, B / 8);
  const int Kb = nr_segmentation(a, c, B, &C, &K, &Z, &F, BG);
  uint8_t *dout = calloc(68 * 384, 1), *e = calloc(G + 64, 1);
  encoder_implemparams_t impp = {.Zc = Z, .Kb = Kb, .BG = BG, .K = K, .gen_code = 0};
  g_enc(c, dout, &impp);
  int rc = nr_rate_matching_ldpc(tbslbrm, BG, Z, dout, e, 1, F, K - F - 2 * Z, rv, G);
  if (rc == 0) nr_interleaving_ldpc(G, Qm, e, f);
  free(a); free(c[0]); free(dout); free(e);
  return rc;
}

/* per-position error % over the Qm*Nl LLRs of one RE group (layer/bit phase), appended to out */
static void phases(const int16_t *llr, const uint8_t *f, uint32_t G, unsigned P, char *out, size_t n)
{
  uint32_t er[16] = {0}, cnt[16] = {0};
  if (P > 16) P = 16;
  for (uint32_t i = 0; i < G; i++) {
    cnt[i % P]++;
    er[i % P] += (llr[i] < 0) != (f[i] != 0);
  }
  size_t o = strlen(out);
  for (unsigned p = 0; p < P; p++)
    o += snprintf(out + o, n - o, "%s%.0f", p ? "," : "", 100.0 * er[p] / (cnt[p] ? cnt[p] : 1));
}

static double g_zero_pct; /* of the last buckets() call, % of exactly-zero LLRs (unfilled / erased) */
static void buckets(const int16_t *llr, const uint8_t *f, uint32_t G, char *out, size_t n, double *tot)
{
  uint32_t errs = 0, zero = 0;
  size_t o = 0;
  for (int b = 0; b < 8; b++) {
    const uint32_t lo = (uint64_t)G * b / 8, hi = (uint64_t)G * (b + 1) / 8;
    uint32_t er = 0;
    for (uint32_t i = lo; i < hi; i++) {
      if (llr[i] == 0) { zero++; continue; }
      er += ((llr[i] < 0) != (f[i] != 0));
    }
    errs += er;
    o += snprintf(out + o, n - o, "%s%.0f", b ? "," : "", 100.0 * er / (hi - lo));
  }
  g_zero_pct = 100.0 * zero / G;
  *tot = 100.0 * errs / G;
}

void nr_pdsch_rm_genie_note(uint16_t rnti, int frame, int slot, uint32_t A, const uint8_t *tb, uint8_t BG, uint8_t Qm,
                            uint8_t Nl, uint8_t rv, uint32_t tbslbrm, uint32_t G, const int16_t *llr, uint8_t mcs)
{
  const uint32_t B = A + (A > 3824 ? 24 : 16);
  uint8_t *f = calloc(G + 64, 1);
  if (expect_bits(tb, B, BG, Qm, rv, tbslbrm, G, f) == 0 && g_nlines_map < 400) {
    char bk[96]; double tot; char ph[96] = ""; 
    buckets(llr, f, G, bk, sizeof(bk), &tot);
    phases(llr, f, G, (unsigned)Qm * Nl, ph, sizeof(ph));
    pthread_mutex_lock(&g_mu);
    g_nlines_map++;
    pthread_mutex_unlock(&g_mu);
    LOG_A(PHY, "SENSING: RMMAP rnti=0x%04x fs=%d.%d mcs=%u rv=%u nl=%u A=%u G=%u err_pct=%.1f by_eighth=[%s] by_phase=[%s] zero_pct=%.1f\n", rnti, frame, slot,
          mcs, rv, Nl, A, G, tot, bk, ph, g_zero_pct);
  }
  free(f);
  pthread_mutex_lock(&g_mu);
  ent_t *e = &g_ring[g_seq % RING];
  free(e->tb);
  e->used = true; e->rnti = rnti; e->A = A; e->B = B; e->frame = frame; e->slot = slot; e->seq = g_seq++;
  e->tb = malloc(B / 8);
  memcpy(e->tb, tb, B / 8);
  pthread_mutex_unlock(&g_mu);
}

void nr_pdsch_rm_genie_fail(uint16_t rnti, int frame, int slot, uint32_t A, uint8_t BG, uint8_t Qm, uint8_t Nl, uint8_t rv,
                            uint32_t tbslbrm, uint32_t G, const int16_t *llr, uint8_t mcs)
{
  uint8_t *f = calloc(G + 64, 1);
  double best = 0; int bfr = -1, bsl = -1, brv = -1, ncand = 0; char bbk[96] = "", bph[96] = ""; double bz = 0;
  uint8_t *tbc = malloc(A / 8 + 16);
  for (int i = 0; i < RING; i++) {
    pthread_mutex_lock(&g_mu);
    ent_t en = g_ring[i];
    if (en.used && en.rnti == rnti && en.A == A) memcpy(tbc, en.tb, en.B / 8);
    pthread_mutex_unlock(&g_mu);
    if (!(en.used && en.rnti == rnti && en.A == A)) continue;
    ncand++;
    for (int r = 0; r < 4; r++) {
      if (expect_bits(tbc, en.B, BG, Qm, r, tbslbrm, G, f) != 0) continue;
      char bk[96]; double tot;
      buckets(llr, f, G, bk, sizeof(bk), &tot);
      if (100.0 - tot > best) { best = 100.0 - tot; bfr = en.frame; bsl = en.slot; brv = r; memcpy(bbk, bk, sizeof(bk)); bz = g_zero_pct; bph[0] = 0; phases(llr, f, G, (unsigned)Qm * Nl, bph, sizeof(bph)); }
    }
  }
  if (ncand && g_nlines_gen < 600) {
    pthread_mutex_lock(&g_mu);
    g_nlines_gen++;
    pthread_mutex_unlock(&g_mu);
    LOG_A(PHY, "SENSING: RMGENIE rnti=0x%04x fs=%d.%d mcs=%u dci_rv=%u nl=%u A=%u G=%u cand=%d best_agree_pct=%.1f from=%d.%d at_rv=%d err_by_eighth=[%s] by_phase=[%s] zero_pct=%.1f\n",
          rnti, frame, slot, mcs, rv, Nl, A, G, ncand, best, bfr, bsl, brv, bbk, bph, bz);
  }
  free(tbc);
  free(f);
}

/* Index into the ring of an earlier CRC-passing TB of this RNTI with identical payload (or -1); frame/slot of it in *fs. */
int nr_pdsch_rm_genie_same_as(uint16_t rnti, uint32_t A, const uint8_t *tb, int *frame, int *slot)
{
  int found = -1;
  const uint32_t B = A + (A > 3824 ? 24 : 16);
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < RING && found < 0; i++)
    if (g_ring[i].used && g_ring[i].rnti == rnti && g_ring[i].A == A && !memcmp(g_ring[i].tb, tb, B / 8)) {
      found = i; *frame = g_ring[i].frame; *slot = g_ring[i].slot;
    }
  pthread_mutex_unlock(&g_mu);
  return found;
}

/* RMDCI line budget (the raw DCI bits of every grant, pass or fail, for field-alignment analysis). */
bool nr_pdsch_rm_genie_dci_budget(void)
{
  static _Atomic int left = 6000;
  return atomic_fetch_sub(&left, 1) > 0;
}
