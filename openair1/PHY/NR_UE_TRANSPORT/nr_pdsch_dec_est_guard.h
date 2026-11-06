/* Identity guard and bookkeeping of the decoder-estimate export (nr_pdsch_passive_decode.h struct nr_pdsch_dec_est), header only so it is unit-tested alone.
 * The decoder's estimate may feed the DM-RS tap only if it was made for THIS grant and hypothesis; every refusal (and every reason the decoder kept none) has a counter slot. */
#ifndef NR_PDSCH_DEC_EST_GUARD_H
#define NR_PDSCH_DEC_EST_GUARD_H
#include <stdint.h>
/* NR_DER_*: the fate of one DM-RS tap's estimate. 0..2 = the decoder's estimate was used (exact one-run grant / segmented grant, first run / estimate band wider than the grant: its noise variance is the band's);
 * the rest = the tap estimated itself, and why. The decoder writes NR_DER_PASS_NOEST..NR_DER_EARLY into the struct when it kept none; the tap adds the guard ones. */
enum { NR_DER_EXACT, NR_DER_SEG, NR_DER_BAND, NR_DER_NODEC /* no decode result (monitor-site tap, decode never ran) */, NR_DER_PASS_NOEST /* the GPU pass took the grant, no hand-back */,
       NR_DER_GW /* shared GrantWork chest: this body made none */, NR_DER_SINGLE_BRANCH, NR_DER_SEG_CPU /* segmented grant on the CPU path */, NR_DER_BUF /* buffer / band bounds */,
       NR_DER_EARLY /* the decode returned before any estimate */, NR_DER_G_ID /* rnti / slot */, NR_DER_G_SHAPE /* RX count, layers, DM-RS ports */, NR_DER_G_WIDTH /* subcarriers / first PRB */,
       NR_DER_G_SYM /* the estimate's symbol is not a DM-RS symbol of the grant */, NR_DER_NVAR0, NR_DER_N };
static const char *const nr_der_name[NR_DER_N] = {"exact", "seg", "band", "nodec", "pass_noest", "gw", "single_branch", "seg_cpu", "buf", "early", "g_id", "g_shape", "g_width", "g_sym", "nvar0"};
struct nr_dec_est_id { uint16_t rnti, first_rb, nsc, ports; long slot; uint8_t nant, nl; };
/* 0 = same grant and hypothesis, else the NR_DER_G_* reason */
static inline int nr_dec_est_match(const struct nr_dec_est_id *have, const struct nr_dec_est_id *want)
{
  if (have->rnti != want->rnti || have->slot != want->slot) return NR_DER_G_ID;
  if (have->nant != want->nant || have->nl != want->nl || have->ports != want->ports) return NR_DER_G_SHAPE;
  if (have->nsc != want->nsc || have->first_rb != want->first_rb) return NR_DER_G_WIDTH;
  return 0;
}
/* the estimate's symbol must be a DM-RS symbol inside the grant's symbols */
static inline int nr_dec_est_sym_ok(int sym, uint32_t dmrs_symb_pos, int start_symbol, int nsym)
{
  return sym >= start_symbol && sym < start_symbol + nsym && sym < 32 && ((dmrs_symb_pos >> sym) & 1u);
}
/* leading run of consecutive PRBs of a data-ordered list (what the tap's contiguous estimate covers) */
static inline int nr_prb_list_run(const uint16_t *prb, int n)
{
  int run = n > 0 ? 1 : 0;
  while (run < n && prb[run] == prb[run - 1] + 1) run++;
  return run;
}
#endif
