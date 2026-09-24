#include <string.h>
#include "nr_pdcch_gf2_rnti.h"
#include "openair1/PHY/gold.h"

#define GF2_MAX_BITS 128 /* headroom past the >=15 minimum; callers pass what they have */
#define GF2_N_UNKNOWN 15 /* RNTI bit 15 is masked out of c_init and unrecoverable; see the header */

/* Consume ALL 32 bits of each gold_generic() word before advancing, exactly like every real caller
 * in this codebase (e.g. LTE_TRANSPORT/dlsch_scrambling.c's e[k] ^= (s >> k) & 1 for k = 0..31).
 * The first version of this module took only bit 0 per call, sampling c(0), c(32), c(64), ... instead
 * of consecutive c(0), c(1), c(2), ... -- a real bug caught by external review, not by this module's
 * own (self-referential) tests. */
static void gold_seq(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 0, x2 = c_init;
  uint32_t word = gold_generic(&x1, &x2, 1);
  int produced = 0;
  for (;;) {
    int take = (n_bits - produced) < 32 ? (n_bits - produced) : 32;
    for (int k = 0; k < take; k++)
      out[produced + k] = (uint8_t)((word >> k) & 1);
    produced += take;
    if (produced >= n_bits)
      break;
    word = gold_generic(&x1, &x2, 0);
  }
}

/* TS 38.211 7.3.2.3, masked exactly like openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1073's
 * `% (1U << 31)`. Only bits 0-14 of n_rnti_low15 are meaningful here -- bit 15 must never be set by
 * a caller of this static helper (build_model() only ever passes 0..14). */
static uint32_t scramble_c_init(uint32_t n_rnti_low15, uint16_t n_id)
{
  return ((n_rnti_low15 << 16) + n_id) % (1u << 31);
}

/* Perturb one of the 15 recoverable RNTI bits at a time from the n_RNTI=0 baseline to build the
 * GF(2) linear map without assuming its structure. */
static void build_model(uint16_t n_id, int n_bits, uint8_t M[GF2_MAX_BITS][GF2_N_UNKNOWN], uint8_t *baseline)
{
  gold_seq(scramble_c_init(0, n_id), n_bits, baseline);
  for (int bit = 0; bit < GF2_N_UNKNOWN; bit++) {
    uint8_t perturbed[GF2_MAX_BITS];
    gold_seq(scramble_c_init(1u << bit, n_id), n_bits, perturbed);
    for (int i = 0; i < n_bits; i++)
      M[i][bit] = perturbed[i] ^ baseline[i];
  }
}

int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out)
{
  if (n_bits < GF2_N_UNKNOWN || n_bits > GF2_MAX_BITS)
    return 0;
  uint8_t M[GF2_MAX_BITS][GF2_N_UNKNOWN], baseline[GF2_MAX_BITS];
  build_model(n_id, n_bits, M, baseline);

  /* Gaussian elimination over GF(2), pivoting across the FULL n_bits-row system (the leading
   * GF2_N_UNKNOWN x GF2_N_UNKNOWN block is not guaranteed full rank -- measured 14/16 in the
   * original 16-unknown version; the same risk applies here and is handled the same way). */
  uint8_t A[GF2_MAX_BITS][GF2_N_UNKNOWN + 1];
  for (int r = 0; r < n_bits; r++) {
    for (int c = 0; c < GF2_N_UNKNOWN; c++)
      A[r][c] = M[r][c];
    A[r][GF2_N_UNKNOWN] = seq[r] ^ baseline[r];
  }
  int row = 0;
  for (int col = 0; col < GF2_N_UNKNOWN; col++) {
    int pivot = -1;
    for (int r = row; r < n_bits; r++)
      if (A[r][col]) { pivot = r; break; }
    if (pivot < 0)
      return 0; /* underdetermined at this n_bits -- never silently guess */
    if (pivot != row) {
      uint8_t tmp[GF2_N_UNKNOWN + 1];
      memcpy(tmp, A[row], GF2_N_UNKNOWN + 1);
      memcpy(A[row], A[pivot], GF2_N_UNKNOWN + 1);
      memcpy(A[pivot], tmp, GF2_N_UNKNOWN + 1);
    }
    for (int r = 0; r < n_bits; r++)
      if (r != row && A[r][col])
        for (int c = 0; c <= GF2_N_UNKNOWN; c++)
          A[r][c] ^= A[row][c];
    row++;
  }
  /* CONSISTENCY CHECK (the second real bug an external review found missing): every row beyond the
   * GF2_N_UNKNOWN pivot rows must now have a zero residual, or `seq` is not a valid scrambling
   * sequence for ANY RNTI at this n_id (wrong n_id, corrupted input, or plain noise) -- measured
   * 1000/1000 false accepts on random noise before this check existed, 0/1000 after. */
  for (int r = GF2_N_UNKNOWN; r < n_bits; r++)
    if (A[r][GF2_N_UNKNOWN])
      return 0;

  uint16_t rnti = 0;
  for (int i = 0; i < GF2_N_UNKNOWN; i++)
    if (A[i][GF2_N_UNKNOWN])
      rnti |= (uint16_t)(1u << i);
  *rnti_out = rnti; /* bit 15 always 0 -- see header: unrecoverable from the sequence alone */
  return 1;
}
