/* Generic RE-pattern rate-match entry (nr_re_pattern.h, SLOTPHASE).
 * (1) A pattern that a CSI-RS PDU can also express (TRS row 1, density-0.5 row 2, a two-symbol row 5) gives bit-exact the same exclusion bitmaps, RB masks,
 *     unavailable-RE counts, CSI-FULL erase masks and the same RE index maps of nr_dlsch_extract_geometry (the maps of the GPU demod and of the GPU graph
 *     pass) as the CSI-RS PDU, for every symbol and RB.  (2) A pattern no CSI-RS row expresses (the PCI 923 footprints sym=0x3300 sc=0xff0, sym=0x200
 *     sc=0x444) removes exactly sym x sc REs per RB of its range, from the geometry too.  (3) A pattern on a DM-RS RE is dropped by
 *     nr_dlsch_csi_drop_dmrs_overlap like a CSI-RS PDU is.  PLAIN C: links PHY_NR_UE like test_nr_csi_rb_extent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_re_pattern.h"
#include "PHY/NR_UE_TRANSPORT/nr_ssb_rate_match.h"
#include "executables/nr-uesoftmodem.h"
#include "common/config/config_load_configmodule.h"

double cpuf;
configmodule_interface_t *uniqCfg = NULL;
openair0_config_t openair0_cfg_g[MAX_CARDS] = {};
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}
static nrUE_params_t nrUE_params;
nrUE_params_t *get_nrUE_params(void)
{
  return &nrUE_params;
}
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h" /* per-cell slot clock (A8): replaces the extern of the former process global */
NR_UE_CELL_CLOCK_DEFINE_PRODUCER();
NR_UE_CELL_CLOCK_DEFINE_REBASE();
void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo)
{
}
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s);
  abort();
}


extern __thread int nr_dlsch_csi_extent_override;
extern uint32_t nr_dlsch_extract_geometry(const NR_DL_FRAME_PARMS *fp, int chest_time, c16_t *rx_idx, uint32_t est_sz, int32_t *est_idx,
                                          uint32_t ext_sz, c16_t *out_y, int32_t *out_h, unsigned char symbol,
                                          fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, const freq_alloc_bitmap_t *fa, const nr_ssb_rm_mask_t *ssb_rm);
static int failures;
#define CHECK(c, ...)                                             \
  do {                                                            \
    if (!(c)) {                                                   \
      failures++;                                                 \
      fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); \
      fprintf(stderr, __VA_ARGS__);                               \
      fprintf(stderr, "\n");                                      \
    }                                                             \
  } while (0)

#define NRB 20
#define NFFT 256
static void csirs(fapi_nr_dl_config_csirs_pdu_rel15_t *p, int row, int start, int n, int l0, int fd, int density)
{
  memset(p, 0, sizeof(*p));
  p->csi_type = 1;
  p->row = row;
  p->start_rb = start;
  p->nr_of_rbs = n;
  p->symb_l0 = l0;
  p->freq_domain = fd;
  p->freq_density = density;
}
static void alloc(freq_alloc_bitmap_t *fa, int first, int n)
{
  memset(fa, 0, sizeof(*fa));
  fa->first_rb = first;
  fa->last_rb = first + n - 1;
  fa->num_rbs = n;
  for (int rb = first; rb < first + n; rb++)
    fa->bitmap[rb / 32] |= 1u << (rb % 32);
}

static NR_DL_FRAME_PARMS *fp;
static c16_t *rx_idx, *oy_a, *oy_b;
static int32_t *est_idx, *oh_a, *oh_b;
#define EST_SZ (14 * NFFT)
#define EXT_SZ ((NRB * 12 + 15) & ~15)

/* the RE index maps of all 14 symbols under cfg; returns the total RE count */
static uint32_t geometry(fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, const freq_alloc_bitmap_t *fa, c16_t *oy, int32_t *oh, uint32_t *nre)
{
  static const nr_ssb_rm_mask_t none = {0};
  uint32_t tot = 0;
  memset(oy, 0, sizeof(c16_t) * 14 * EXT_SZ);
  memset(oh, 0, sizeof(int32_t) * 14 * EXT_SZ);
  for (int m = 0; m < 14; m++) {
    nre[m] = nr_dlsch_extract_geometry(fp, 0, rx_idx, EST_SZ, est_idx, EXT_SZ, oy + (size_t)m * EXT_SZ, oh + (size_t)m * EXT_SZ, m, cfg, fa, &none);
    tot += nre[m];
  }
  return tot;
}

/* the equivalence: cfg_a holds CSI-RS PDUs, cfg_b the patterns; same everything, for both extent modes */
static void equivalent(const char *what, fapi_nr_dl_config_dlsch_pdu_rel15_t *a, fapi_nr_dl_config_dlsch_pdu_rel15_t *b, const freq_alloc_bitmap_t *fa)
{
  for (int ext = 0; ext <= 1; ext++) {
    nr_dlsch_csi_extent_override = ext;
    for (int m = 0; m < 14; m++) {
      CHECK(nr_dlsch_csi_overlap_bitmap(a, m) == nr_dlsch_csi_overlap_bitmap(b, m), "%s ext=%d sym %d bitmap 0x%x vs 0x%x", what, ext, m,
            nr_dlsch_csi_overlap_bitmap(a, m), nr_dlsch_csi_overlap_bitmap(b, m));
      for (int crb = 0; crb < NRB + 6; crb++)
        CHECK(nr_dlsch_csi_rb_mask(a, m, crb) == nr_dlsch_csi_rb_mask(b, m, crb), "%s ext=%d sym %d crb %d rb_mask", what, ext, m, crb);
    }
    CHECK(nr_dlsch_csi_unav_res(a, fa) == nr_dlsch_csi_unav_res(b, fa), "%s ext=%d unav %u vs %u", what, ext, nr_dlsch_csi_unav_res(a, fa),
          nr_dlsch_csi_unav_res(b, fa));
    uint32_t na[14], nb[14];
    const uint32_t ta = geometry(a, fa, oy_a, oh_a, na), tb = geometry(b, fa, oy_b, oh_b, nb);
    CHECK(ta == tb, "%s ext=%d geometry RE count %u vs %u", what, ext, ta, tb);
    CHECK(memcmp(na, nb, sizeof(na)) == 0, "%s ext=%d per-symbol RE counts", what, ext);
    CHECK(memcmp(oy_a, oy_b, sizeof(c16_t) * 14 * EXT_SZ) == 0 && memcmp(oh_a, oh_b, sizeof(int32_t) * 14 * EXT_SZ) == 0, "%s ext=%d RE index maps differ", what, ext);
  }
  /* the CSI-FULL erase list takes the same entries */
  nr_dlsch_csi_extent_override = 1;
  nr_dlsch_csi_erase_set(a->csiRsForRateMatching, a->numCsiRsForRateMatching);
  uint32_t ea[14][NRB + 6], eb[14][NRB + 6];
  for (int m = 0; m < 14; m++)
    for (int crb = 0; crb < NRB + 6; crb++) ea[m][crb] = nr_dlsch_csi_erase_mask(a, m, crb);
  nr_dlsch_csi_erase_set(b->csiRsForRateMatching, b->numCsiRsForRateMatching);
  for (int m = 0; m < 14; m++)
    for (int crb = 0; crb < NRB + 6; crb++) eb[m][crb] = nr_dlsch_csi_erase_mask(b, m, crb);
  CHECK(memcmp(ea, eb, sizeof(ea)) == 0, "%s erase masks differ", what);
  nr_dlsch_csi_erase_set(NULL, 0);
}

int main(void)
{
  fp = calloc(1, sizeof(*fp));
  rx_idx = calloc(14 * NFFT, sizeof(c16_t));
  est_idx = calloc(EST_SZ, sizeof(int32_t));
  oy_a = malloc(sizeof(c16_t) * 14 * EXT_SZ);
  oy_b = malloc(sizeof(c16_t) * 14 * EXT_SZ);
  oh_a = malloc(sizeof(int32_t) * 14 * EXT_SZ);
  oh_b = malloc(sizeof(int32_t) * 14 * EXT_SZ);
  fp->ofdm_symbol_size = NFFT;
  fp->first_carrier_offset = NFFT - NRB * 6;
  fp->samples_per_slot_wCP = 14 * NFFT;
  fp->nb_antennas_rx = 1;
  for (int s = 0; s < 14 * NFFT; s++) rx_idx[s] = (c16_t){(int16_t)(s % NFFT), 1};
  for (int s = 0; s < EST_SZ; s++) ((c16_t *)est_idx)[s] = (c16_t){(int16_t)(s % NFFT), (int16_t)(s / NFFT + 1)};

  fapi_nr_dl_config_dlsch_pdu_rel15_t a, b;
  freq_alloc_bitmap_t fa;
  memset(&a, 0, sizeof(a));
  a.BWPStart = 0;
  a.BWPSize = NRB;
  a.start_symbol = 0;
  a.number_symbols = 14;
  a.dmrsConfigType = NFAPI_NR_DMRS_TYPE1;
  a.n_dmrs_cdm_groups = 1;
  a.dlDmrsSymbPos = 1u << 2;
  alloc(&fa, 0, NRB);

  /* (1a) row-1 TRS: symbol 4, k = 0,4,8 (0x111), RB 2..13, every RB */
  b = a;
  a.numCsiRsForRateMatching = b.numCsiRsForRateMatching = 1;
  csirs(&a.csiRsForRateMatching[0], 1, 2, 12, 4, 0x1, 3);
  nr_re_pattern_make(&b.csiRsForRateMatching[0], 1u << 4, 0x111, 3, 2, 12);
  equivalent("TRS row1", &a, &b, &fa);
  CHECK(nr_dlsch_csi_unav_res(&b, &fa) == 12 * 3, "TRS unav %u", nr_dlsch_csi_unav_res(&b, &fa));

  /* (1b) row-2 single RE per RB (freq_domain bit 5 -> RE 5), density 0.5 odd RBs, RB 0..19 */
  csirs(&a.csiRsForRateMatching[0], 2, 0, NRB, 9, 1u << 5, 1);
  nr_re_pattern_make(&b.csiRsForRateMatching[0], 1u << 9, 1u << 5, 1, 0, NRB);
  equivalent("row2 dot5 odd", &a, &b, &fa);
  CHECK(nr_dlsch_csi_unav_res(&b, &fa) == 10, "row2 unav %u", nr_dlsch_csi_unav_res(&b, &fa));

  /* (1c) row 5 (two symbols, REs 2*bit + {0,1}), two entries mixed: CSI-RS PDU vs pattern, partial RB range, plus the TRS of (1a) */
  a.numCsiRsForRateMatching = b.numCsiRsForRateMatching = 2;
  csirs(&a.csiRsForRateMatching[0], 5, 4, 8, 11, 1u << 3, 0);
  nr_re_pattern_make(&b.csiRsForRateMatching[0], 3u << 11, 0x3u << 6, 0, 4, 8);
  csirs(&a.csiRsForRateMatching[1], 1, 2, 12, 4, 0x1, 3);
  nr_re_pattern_make(&b.csiRsForRateMatching[1], 1u << 4, 0x111, 3, 2, 12);
  equivalent("row5 + TRS", &a, &b, &fa);

  /* (1d) a pattern may also stand next to a CSI-RS PDU in the SAME list: same result as the all-CSI-RS list */
  b.csiRsForRateMatching[0] = a.csiRsForRateMatching[0];
  equivalent("mixed list", &a, &b, &fa);

  /* (2) patterns no CSI-RS row expresses: the PCI 923 footprints. Union of sym 8,9,12,13 x sc 0xff0 (8 REs) and sym 9 x sc 0x444 (3 REs) over
   *     all RBs of a 20-RB grant: symbols 8,12,13: 8 REs/RB; symbol 9: 8 + 3 - overlap(0xff0 & 0x444 = 0x440 -> 2) = 9 REs/RB. */
  b = a;
  b.numCsiRsForRateMatching = 2;
  nr_re_pattern_make(&b.csiRsForRateMatching[0], 0x3300, 0xff0, 2, 0, NRB);
  nr_re_pattern_make(&b.csiRsForRateMatching[1], 0x200, 0x444, 3, 0, NRB);
  nr_dlsch_csi_extent_override = 1;
  {
    const uint32_t want = NRB * (8 + 9 + 8 + 8);
    CHECK(nr_dlsch_csi_unav_res(&b, &fa) == want, "footprint unav %u (want %u)", nr_dlsch_csi_unav_res(&b, &fa), want);
    uint32_t nb[14];
    const uint32_t tot = geometry(&b, &fa, oy_b, oh_b, nb);
    CHECK(tot == 14u * NRB * 12 - NRB * 6 /* DM-RS symbol 2: data REs only */ - want, "total RE count %u", tot);
    CHECK(nb[8] == NRB * 4 && nb[12] == NRB * 4 && nb[13] == NRB * 4 && nb[9] == NRB * 3, "per-symbol RE counts %u %u %u %u", nb[8], nb[9], nb[12], nb[13]);
    CHECK(nb[0] == NRB * 12 && nb[5] == NRB * 12 && nb[10] == NRB * 12 && nb[2] == NRB * 6, "untouched symbols");
    /* the surviving REs of symbol 8 are exactly subcarriers 0..3 of every RB (0xff0 removed) */
    bool ok = true;
    for (int rb = 0; rb < NRB && ok; rb++)
      for (int re = 0; re < 4; re++) {
        const c16_t y = oy_b[8 * EXT_SZ + rb * 4 + re];
        ok &= y.r == (int16_t)((fp->first_carrier_offset + rb * 12 + re) % NFFT);
      }
    CHECK(ok, "symbol 8 survivors are subcarriers 0..3 of each RB");
  }
  /* density 0.5 and RB range on a pattern the rows cannot express */
  nr_re_pattern_make(&b.csiRsForRateMatching[0], 0x3300, 0xff0, 0, 5, 10); /* even RBs 6,8,10,12,14 */
  b.numCsiRsForRateMatching = 1;
  CHECK(nr_dlsch_csi_unav_res(&b, &fa) == 5 * 4 * 8, "even-RB footprint unav %u", nr_dlsch_csi_unav_res(&b, &fa));

  /* (3) DM-RS safety: a pattern on a DM-RS RE of a DM-RS symbol is dropped, one on the other REs is kept */
  {
    fapi_nr_dl_config_dlsch_pdu_rel15_t d = a;
    d.numCsiRsForRateMatching = 3;
    nr_re_pattern_make(&d.csiRsForRateMatching[0], 1u << 2, 0x001, 3, 0, NRB); /* type-1, 1 group: DM-RS on even REs -> overlap */
    nr_re_pattern_make(&d.csiRsForRateMatching[1], 1u << 2, 0x002, 3, 0, NRB); /* odd RE of the DM-RS symbol: legal */
    nr_re_pattern_make(&d.csiRsForRateMatching[2], 1u << 8, 0xfff, 3, 0, NRB);
    CHECK(nr_dlsch_csi_drop_dmrs_overlap(&d) == 1 && d.numCsiRsForRateMatching == 2, "pattern overlap dropped: %u", d.numCsiRsForRateMatching);
    CHECK(nr_re_pattern_sc(&d.csiRsForRateMatching[0]) == 0x002 && nr_re_pattern_sym(&d.csiRsForRateMatching[1]) == 1u << 8, "survivors");
  }
  printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
