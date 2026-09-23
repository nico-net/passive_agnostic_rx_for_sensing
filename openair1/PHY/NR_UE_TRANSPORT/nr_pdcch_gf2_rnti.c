#include <string.h>
#include "nr_pdcch_gf2_rnti.h"
#include "openair1/PHY/gold.h"

#define GF2_MAX_BITS 128 /* headroom past the >=16 minimum; callers pass what they have */

static void gold_seq(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 0, x2 = c_init;
  out[0] = (uint8_t)(gold_generic(&x1, &x2, 1) & 1);
  for (int n = 1; n < n_bits; n++)
    out[n] = (uint8_t)(gold_generic(&x1, &x2, 0) & 1);
}

/* Perturb one RNTI bit at a time from the n_RNTI=0 baseline (mirrors the verified Python exactly)
 * to build the 16-column GF(2) linear map without assuming its structure. */
static void build_model(uint16_t n_id, int n_bits, uint8_t M[GF2_MAX_BITS][16], uint8_t *baseline)
{
  gold_seq((uint32_t)n_id, n_bits, baseline);
  for (int bit = 0; bit < 16; bit++) {
    uint8_t perturbed[GF2_MAX_BITS];
    const uint32_t c = ((uint32_t)(1u << bit) << 16) + n_id;
    gold_seq(c, n_bits, perturbed);
    for (int i = 0; i < n_bits; i++)
      M[i][bit] = perturbed[i] ^ baseline[i];
  }
}

int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out)
{
  if (n_bits < 16 || n_bits > GF2_MAX_BITS)
    return 0;
  uint8_t M[GF2_MAX_BITS][16], baseline[GF2_MAX_BITS];
  build_model(n_id, n_bits, M, baseline);

  /* Gaussian elimination over GF(2), pivoting across the FULL n_bits-row system -- NOT just the
   * first 16 rows. Measured (see the PoC): the leading 16x16 submatrix is rank-deficient (14/16 at
   * n_id=2, n_bits=64), so a pivot search confined to it silently produces a wrong answer instead
   * of failing. Build an (n_bits x 17) augmented matrix and reduce column-by-column, searching the
   * whole remaining row range for a pivot each time. */
  uint8_t A[GF2_MAX_BITS][17];
  for (int r = 0; r < n_bits; r++) {
    for (int c = 0; c < 16; c++)
      A[r][c] = M[r][c];
    A[r][16] = seq[r] ^ baseline[r];
  }
  int row = 0;
  for (int col = 0; col < 16; col++) {
    int pivot = -1;
    for (int r = row; r < n_bits; r++)
      if (A[r][col]) { pivot = r; break; }
    if (pivot < 0)
      return 0; /* underdetermined at this n_bits -- never silently guess */
    if (pivot != row) {
      uint8_t tmp[17];
      memcpy(tmp, A[row], 17);
      memcpy(A[row], A[pivot], 17);
      memcpy(A[pivot], tmp, 17);
    }
    for (int r = 0; r < n_bits; r++)
      if (r != row && A[r][col])
        for (int c = 0; c < 17; c++)
          A[r][c] ^= A[row][c];
    row++;
  }
  uint16_t rnti = 0;
  for (int i = 0; i < 16; i++)
    if (A[i][16])
      rnti |= (uint16_t)(1u << i);
  *rnti_out = rnti;
  return 1;
}
