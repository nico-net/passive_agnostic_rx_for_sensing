/* MCS-table retry of a failed high-SNR PUSCH (pure helpers). The UL MCS table (qam64 / qam256 / qam64LowSE, TS 38.214 Table 6.1.4.1-1/-2/-3)
 * is RRC state a passive receiver cannot read. The same 5-bit MCS index gives a different Qm and code rate, hence a different TBS and
 * demapper, in each table: a grant read with the wrong table fails every code block however strong the signal ([MEASURED, DGX OTA 2026-11-03
 * M1_1_151634] rnti 0x4698, 892 failures at 20..40 dB). A grant that fails while its DM-RS SNR clears the requirement of another table is
 * re-decoded under that table; the table that decodes is learned per RNTI and tried first on that RNTI's next grants. */
#ifndef NR_UL_TBL_RETRY_H
#define NR_UL_TBL_RETRY_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "nr_snr_gate.h"

#define NR_UL_TBLR_UES 64
typedef struct {
  uint16_t rnti;
  bool used;
  uint8_t learned; /* table + 1 of the last winner, 0 none */
  uint8_t passes;  /* consecutive passes under `learned` (learned counts once passes >= 2: a CRC16 TB false-passes at ~1.5e-5 per trial) */
  uint32_t retries; /* retry slots offered to this RNTI while unlearned */
} nr_ul_tblr_ent_t;

static inline nr_ul_tblr_ent_t *nr_ul_tblr_get(nr_ul_tblr_ent_t *tab, uint16_t rnti, bool create)
{
  nr_ul_tblr_ent_t *free_e = NULL;
  for (int i = 0; i < NR_UL_TBLR_UES; i++) {
    if (tab[i].used && tab[i].rnti == rnti) return &tab[i];
    if (!tab[i].used && !free_e) free_e = &tab[i];
  }
  if (!create) return NULL;
  if (!free_e) free_e = &tab[rnti % NR_UL_TBLR_UES];
  *free_e = (nr_ul_tblr_ent_t){.rnti = rnti, .used = true};
  return free_e;
}

/* table to decode first: the learned one (two passes behind it), else the DCI-side default */
static inline int nr_ul_tblr_first(const nr_ul_tblr_ent_t *e, int dflt) { return (e && e->learned && e->passes >= 2) ? e->learned - 1 : dflt; }

/* Tables worth a retry after `tried` failed: bit t set = try table t. Only SNR-feasible tables (DM-RS SNR >= required + margin) with a valid MCS
 * index. Unlearned RNTIs: every table on the first 2 offers, then one table per offer in rotation, 1 offer in 8 (the CPU bound). */
static inline unsigned nr_ul_tblr_pick(nr_ul_tblr_ent_t *e, int mcs, int tried, double snr_db, double margin_db)
{
  unsigned feasible = 0;
  for (int t = 0; t < 3; t++) {
    const double req = nr_required_snr_db(mcs, t);
    if (t != tried && isfinite(req) && isfinite(snr_db) && snr_db >= req + margin_db) feasible |= 1u << t;
  }
  if (!feasible || !e) return feasible;
  const uint32_t k = e->retries++;
  if (k < 2) return feasible;
  if (k % 8) return 0;
  const int n = __builtin_popcount(feasible);
  int skip = (int)((k / 8) % (uint32_t)n);
  for (int t = 0; t < 3; t++)
    if ((feasible >> t & 1u) && skip-- == 0) return 1u << t;
  return 0;
}

/* a CRC pass under table t; failures never unlearn (a failing grant does not say the table is wrong): another table's pass replaces it */
static inline void nr_ul_tblr_pass(nr_ul_tblr_ent_t *e, int t)
{
  if (!e) return;
  if (e->learned == t + 1) { if (e->passes < 255) e->passes++; }
  else { e->learned = (uint8_t)(t + 1); e->passes = 1; }
}
#endif
