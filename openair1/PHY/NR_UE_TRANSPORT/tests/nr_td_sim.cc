/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Correlated Monte-Carlo simulator for Technique D convergence levers (spec 2026-10-01 section 6.1).
 * Links the REAL engine (nr_pdsch_config_sweep), gate, ordering and field book; only the channel/decoder is
 * modelled. Results are SIMULATED, never MEASURED. Per grant ONE latent state (SNR, rank, MCS, table exercise,
 * new-tx) drives the main decode and every probe (shared-IQ correlation). */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>
extern "C" {
#include "nr_pdsch_config_sweep.h"
#include "nr_td_fieldbook.h"
#include "nr_td_legal.h"
#include "nr_td_gate.h"
#include "nr_td_order.h"
#include "nr_pdsch_qm_oracle.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}
extern "C" {
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *file, const char *fn, int line, const char *message, int fatal)
{
  if (message) fprintf(stderr, "%s:%d %s: %s\n", file, line, fn, message);
  if (fatal) abort();
  exit(EXIT_SUCCESS);
}
}

struct SimCfg {
  int acq, seed, catalog_tda, n_rx, K, sib1, fieldbook, gate, p2, twins, rntis_per_acq;
  int inject_wrong_field; /* --fieldbook 2 only: -1 none; 0 TDRA, 1 add_pos, 2 max_len: force_promote a WRONG value at acquisition start */
  double fo_alpha, fo_pmin; /* fail-open thresholds (nr_pdsch_config_sweep_fail_open_due) */
  int equiv;            /* 1 = lever E: the main full decode credits every grant-equivalent hypothesis (needs twins >= 2) */
  int prior;            /* 1 (default) = today's cell-wide/RNTI prior pruning; forced OFF when fieldbook=1 (plan R2 rule) */
  int oracle;           /* 1 = today's runtime (DM-RS/last-symbol/k0 + Qm oracles prune the catalogue), 0 = blind arm */
  int dmrs_typea_pos;   /* ASN.1 enum: 0 = pos2 (rfsim gNB default, 106 PRB cell), 1 = pos3 */
  double mu, fade, snr_est_sigma, rank2_frac, grants_per_s, probe_inconclusive, table_exercise, cap_s;
  /* Realism (spec 2026-10-01 blind convergence section 7), all default 0 = today's perfect oracles / no traps.
   * oracle_miss/oracle_wrong: per RNTI probabilities that the DM-RS+Qm oracles produce nothing / a destructive wrong decision.
   * harq_trap: per grant, the k0+-1 neighbour of the truth passes. crc_false: per FAILING decode false-pass probability. */
  double oracle_miss, oracle_wrong, harq_trap, crc_false;
  /* Experimental acceptance levers (spec 2026-10-01 sections 3/3b), default 0 = today's engine, output byte-identical.
   * crc_accept: lever C (CRC-pass acceptance); geom_pin: lever P (geometry pinning). Both receive the FULL grant-equivalence class
   * of the main decode (dormant members included); with --equiv 0 only attribution uses it (feed_attr: credits idx[0] alone). */
  int crc_accept, geom_pin;
  /* 1 = the HARQ trap fires only on RETRANSMISSION grants (new_tx false), the physical mechanism (spec section 3 (c): a wrong k0 points at
   * the slot of a retransmitted TB, whose HARQ buffer combining makes it pass). 0 (default, the BC0 model) = it fires on any grant.
   * The frng draw is made either way, so the random streams are unchanged. */
  int harq_trap_retx;
  /* Trap model, three SEPARATE categories (fix round 1): --crc-false (ordinary CRC false pass), --retx-trap P (on grants with new_tx false the k0+-1
   * sibling passes with probability P) and --k0-trap-adj A (NEW-TB consecutive-slot k0 trap: per grant, with probability A the adjacent slot carries an
   * identical allocation and MCS for the same RNTI, and then every k0+-1 sibling decode passes iff the truth's decode would pass). --harq-trap /
   * --harq-trap-retx are LEGACY (kept for byte identity of old runs). All trap draws use dedicated streams, never crng. */
  double retx_trap, k0_trap_adj;
  /* Simulator v2 (BC8, notes section 6). ALL default to the v1 behaviour: with slot_model 0 the output is byte-identical to the v1 binary.
   * slot_model 1: per RNTI a slot-indexed grant timeline (grant_prob per eligible DL slot, adjacency = P(grant | previous eligible slot had one),
   * persist rho = P(next grant repeats the previous allocation+MCS)); the k0 trap is PHYSICAL (decode of slot dci+k0'); the DCIs the receiver
   * holds are observed with miss prob dci_miss and polluted by spurious DCIs (dci_false per PDCCH slot); a per-grant per-k0 certified flag is
   * computed from the OBSERVED DCIs only. tdd = "DDDSU"-style pattern ('U' = no DL, DCI or PDSCH). k0_oracle_legacy 1 = today's oracle
   * (perfect k0 with slot_model 0; the K39 bug with slot_model 1: claims k0 = 0 whenever the DCI's own slot carries any same-RNTI PDSCH),
   * 0 = K39 fix (mask + last symbol only, never k0). fo_always 1 = fail-open also outside fieldbook 2. truth_k0 >= 0 forces the truth's k0. */
  int slot_model, k0_oracle_legacy, fo_always, truth_k0;
  double grant_prob, persist, adjacency, dci_miss, dci_false;
  /* v2 fidelity round 1: other_ue_occ = P(another UE's PDSCH overlaps the grant's PRBs in a DL slot; cell-scrambled DM-RS, wrong mask),
   * tdd_s_dl_symbols = DL symbols of an 'S' slot, mcs_change = per-slot MCS redraw prob (-1 = 1 - persist), snr_rho = AR(1) SNR coefficient (0 = i.i.d.) */
  double other_ue_occ, mcs_change, snr_rho;
  int tdd_s_dl_symbols;
  int other_ue_same_cfg; /* 1 = another UE's DM-RS config (add_pos / max_len) equals the truth's (cell-common): its mask differs only through S/L (1-3 distinct masks) */
  std::string tdd;
  float sib_pmin, sib_eps; /* sibling guard (engine st->sib_pmin / sib_eps); --sib-pmin 0 disables the guard (fix A only) */
  float w_sib1, w_default, w_obs, w_field, w_probe;
  bool check_correlation;
  static SimCfg defaults()
  {
    SimCfg c;
    c.acq = 100; c.seed = 1; c.catalog_tda = 4; c.n_rx = 4; c.K = 1; c.sib1 = 0; c.fieldbook = 0; c.gate = 0; c.p2 = 0;
    c.inject_wrong_field = -1; c.fo_alpha = 1e-3; c.fo_pmin = 0.05;
    c.equiv = 0; c.twins = 2; c.rntis_per_acq = 4; c.oracle = 1; c.prior = 1; c.dmrs_typea_pos = 0;
    c.mu = 15; c.fade = 6; c.snr_est_sigma = 2; c.rank2_frac = 0.3; c.grants_per_s = 200;
    c.probe_inconclusive = 0.1; c.table_exercise = 0.9; c.cap_s = 3600;
    c.oracle_miss = c.oracle_wrong = c.harq_trap = c.crc_false = 0;
    c.crc_accept = c.geom_pin = c.harq_trap_retx = 0;
    c.slot_model = 0; c.k0_oracle_legacy = 1; c.fo_always = 0; c.truth_k0 = -1;
    c.other_ue_same_cfg = 0; c.other_ue_occ = 0; c.mcs_change = -1; c.snr_rho = 0; c.tdd_s_dl_symbols = 6; c.grant_prob = 0.5; c.persist = 0.9; c.adjacency = -1; c.dci_miss = c.dci_false = 0;
    c.retx_trap = c.k0_trap_adj = 0; c.sib_pmin = 0.05f; c.sib_eps = 1e-6f;
    /* Grant cap per RNTI = cap_s * grants_per_s. It MUST exceed the largest baseline (levers-off, oracle-off) need,
     * ~2200 s p95 / ~3000 s max at 1 RX blind: a smaller cap turns slow-but-correct RNTIs into censored
     * `undecidable` ones and biases every quantile downwards. Capped RNTIs are reported separately and excluded
     * from the quantiles/means (censoring). */
    c.w_sib1 = c.w_default = c.w_obs = c.w_field = c.w_probe = 0;
    c.check_correlation = false;
    return c;
  }
};
struct RntiRec {
  int acq, rnti_rank, truth_table;
  long grants;
  double seconds;
  bool winner_ok, wrong, undecidable;
  int oracle_state;       /* 0 ok, 1 miss, 2 wrong */
  long harq_trap_passes, false_passes;
  long sib_trials, sib_blocks, retx_trap_passes, k0_trap_passes;
  long dci_missed, dci_false, proc_grants, adj_grants, trap_grants, certified_grants, certified_sib, certified_wrong; /* slot model (v2) */
  int truth_k0;
  long restores, restore_hyp; /* restore_observed_typea events / entries re-added */
  long k0_probes, k0_probe_hyp, k0_probe_layers, n_hyp_end; /* I3: runtime k0 >= 2 probe layers */
  long geom_pins, geom_blocks, crc_accepts; /* lever events in this RNTI (see SimCfg::crc_accept/geom_pin) */
  /* Analytical-bound bookkeeping (spec section 3 evidence standard): wrong_pins = pins that kept a geometry other than the truth's;
   * *_bound = union bound of that event on the run's own trial counts at the event, with p_f = --crc-false (see run_sim). */
  long wrong_pins, crc_wrong;
  double geom_bound, crc_bound;
  long n_full, n_probe, gated_phys, gated_chan, promotions, withdrawals;
  long p2_admitted_fail;  /* probe FAILs admitted as KL failures (st->p2 && p2_admissible && FAIL) */
  int active_start = -1;     /* --fieldbook 2: active hypotheses after the start masks */
  bool fail_open = false;    /* --fieldbook 2: fail-open fired for this RNTI */
  uint32_t pruned_fields = 0; /* --fieldbook 2: bit f = field f pruned (dormant) in this context, cleared on fail-open */
  long truth_full;        /* full-TB decodes of the true hypothesis until the decision */
  long truth_kl_trials;   /* engine KL trials of the true hypothesis at the decision (since the last prune), -1 if pruned */
  long truth_elim;        /* KL failures the engine added to the truth on grants where its full decode passes */
  char winner_key[48];    /* content of the winning hypothesis (pairing P1 vs P2), "-" if undecidable */
};
struct SimResult {
  long total_grants = 0, wrong = 0, undecidable = 0, acquisitions_rntis = 0, correlation_violations = 0;
  long n_full = 0, n_probe = 0, gated_phys = 0, gated_chan = 0, promotions = 0, withdrawals = 0;
  long twins_min = -1, twins_sum = 0;
  /* Review Focus 4: KL failures that feed_k added to the TRUE hypothesis on grants where its full decode passes,
   * measured on the engine's own counters (delta trials - delta ok around feed_k). Must be 0. */
  long truth_eliminated_by_probe = 0, p2_admitted_fail = 0;
  double median_s = 0, p95_s = 0, mean_s = 0, mean_grants = 0, mean_s_steady = 0; /* steady = RNTIs k >= 2, decided only */
  /* --fieldbook 2 */
  long fail_opens = 0, untrusted_after = 0, injected = 0, inject_skipped = 0, recovery_never = 0;
  double active_start_sum = 0;
  std::vector<long> recovery_grants, recovery_rntis; /* per injected acquisition that recovered */
  long n_decided = 0;
  long oracle_miss_rntis = 0, oracle_wrong_rntis = 0, harq_trap_passes = 0, false_passes = 0;
  long geom_pins = 0, geom_blocks = 0, crc_accepts = 0, wrong_pins = 0, crc_wrong = 0, sib_trials = 0, sib_blocks = 0, retx_trap_passes = 0, k0_trap_passes = 0;
  double geom_bound = 0, crc_bound = 0;
  long dci_missed = 0, dci_false = 0, proc_grants = 0, adj_grants = 0, trap_grants = 0, certified_grants = 0, certified_sib = 0, certified_wrong = 0;
  long restores = 0, restore_hyp = 0;
  long k0_probes = 0, k0_probe_hyp = 0, k0_probe_layers = 0, n_hyp_end = 0;
  struct TableStat { long n = 0, wrong = 0; double sum_s = 0; std::vector<double> v; } by_table[3];
  std::vector<RntiRec> recs;
};

/* Production legality function (nr_pdcch_blind_dmrs_mask(), the one the runtime passes to
 * nr_pdsch_config_sweep_select()). The module is not linkable in a pure test, so its two TS 38.211 tables and
 * blind_fill_dmrs_mask() are copied VERBATIM from nr_pdcch_blind_monitor.c (same duplication that file makes from
 * nr_mac_common.c). dmrs_TypeA_Position is the ASN.1 enum: pos2 = 0, pos3 = 1. Cell parameter used: the 106-PRB
 * rfsim cell's pos2 (gNB default), overridable with --dmrs-typea-pos. */
static const int32_t g_table_7_4_1_1_2_3[13][8] = {
    {-1, -1, -1, -1, 1, 1, 1, 1}, {0, 0, 0, 0, 1, 1, 1, 1}, {0, 0, 0, 0, 1, 1, 1, 1}, {0, 0, 0, 0, 1, 17, 17, 17},
    {0, 0, 0, 0, 1, 17, 17, 17}, {0, 0, 0, 0, 1, 17, 17, 17}, {0, 128, 128, 128, 1, 65, 73, 73},
    {0, 128, 128, 128, 1, 129, 145, 145}, {0, 512, 576, 576, 1, 129, 145, 145}, {0, 512, 576, 576, 1, 257, 273, 585},
    {0, 512, 576, 2336, 1, 513, 545, 585}, {0, 2048, 2176, 2336, 1, 513, 545, 585}, {0, 2048, 2176, 2336, -1, -1, -1, -1},
};
static const int32_t g_table_7_4_1_1_2_4[12][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1}, {0, 0, -1, -1, -1, -1, -1, -1}, {0, 0, -1, -1, 3, 3, -1, -1}, {0, 0, -1, -1, 3, 3, -1, -1},
    {0, 0, -1, -1, 3, 3, -1, -1}, {0, 0, -1, -1, 3, 99, -1, -1}, {0, 0, -1, -1, 3, 99, -1, -1}, {0, 768, -1, -1, 3, 387, -1, -1},
    {0, 768, -1, -1, 3, 387, -1, -1}, {0, 768, -1, -1, 3, 771, -1, -1}, {0, 3072, -1, -1, 3, 771, -1, -1}, {0, 3072, -1, -1, -1, -1, -1, -1},
};
/* Mirrors nr_pdcch_blind_dmrs_mask() -> blind_fill_dmrs_mask() in openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
 * (pinned by TdSim.DmrsLegalityPinned in nr_td_sim_test.cc). */
static int32_t sim_legality(int typeA_pos, int NrOfSymbols, int startSymbol, int is_b, int add_pos, int length)
{
  if (add_pos < 0 || add_pos > 3 || (length != 1 && length != 2)) return -1;
  int l0 = 0;
  if (!is_b) {
    if (typeA_pos == 0) l0 = 2; else if (typeA_pos == 1) l0 = 3; else return -1;
    if (l0 == 3 && add_pos == 3) return -1;
    if (startSymbol > l0) return -1;
  }
  const int column = !is_b ? add_pos : add_pos + 4;
  const int ld = !is_b ? NrOfSymbols + startSymbol : NrOfSymbols;
  if (ld <= 1 || ld >= 15 || NrOfSymbols + startSymbol >= 15) return -1;
  if (!is_b && l0 == 3 && (ld == 3 || ld == 4)) return -1;
  int32_t lp, sh;
  if (length == 1) { lp = g_table_7_4_1_1_2_3[ld - 2][column]; sh = 1 << l0; }
  else {
    const int row = ld < 4 ? 0 : ld - 3;
    if (row >= 12) return -1;
    lp = g_table_7_4_1_1_2_4[row][column]; sh = (1 << l0) | (1 << (l0 + 1));
  }
  if (lp < 0) return -1;
  return !is_b ? (lp | sh) : (lp << startSymbol);
}

/* Destructive pruning by predicate = the engine's own prune_keep (prune_commit semantics, dormant masks compacted). Thin
 * trampoline only: no prune semantics are copied here. */
template <class F> static int sim_prune(nr_pdsch_config_sweep_state_t *st, F keep)
{
  return nr_pdsch_config_sweep_prune_keep(
      st, [](const nr_pdsch_cfg_hypothesis_t *h, const void *arg) -> bool { return (*(const F *)arg)(*h); }, &keep);
}
/* Dormant (reversible) mask for `cause` by predicate (nr_pdsch_config_sweep_set_dormant). Returns its result (-1 = refused). */
template <class F> static int sim_dormant(nr_pdsch_config_sweep_state_t *st, int cause, F keep)
{
  return nr_pdsch_config_sweep_set_dormant(
      st, cause, [](const nr_pdsch_cfg_hypothesis_t *h, const void *arg) -> bool { return (*(const F *)arg)(*h); }, &keep);
}
static uint8_t sim_qm_table_mask(int mcs, int qm)
{
  uint8_t m = 0;
  for (uint8_t t = 0; t < 3; t++) if (nr_pdsch_qm_of_mcs((uint8_t)mcs, t) == qm) m |= (uint8_t)(1u << t);
  return m;
}

static uint64_t mix(uint64_t a, uint64_t b, uint64_t c)
{
  uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0xBF58476D1CE4E5B9ull) * 0x94D049BB133111EBull ^ (c + 0x2545F4914F6CDD1Dull) * 0xD6E8FEB86659FD93ull;
  x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
  return x;
}

/* ---- Simulator v2: slot-indexed grant timeline (notes section 6) ---- */
/* DL MCS tables VERBATIM from openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c (Table_51311/12/13: {Qm, 10 x R}); the table index is
 * nr_pdsch_cfg_hypothesis_t::mcs_table (0 = 38.214 Table 5.1.3.1-1, 1 = -2, 2 = -3). Pinned by TdSimV2.MacMcsTablesPinned. */
static const uint16_t Table_51311[32][2] = {{2, 1200}, {2, 1570}, {2, 1930}, {2, 2510}, {2, 3080}, {2, 3790}, {2, 4490}, {2, 5260},
                                            {2, 6020}, {2, 6790}, {4, 3400}, {4, 3780}, {4, 4340}, {4, 4900}, {4, 5530}, {4, 6160},
                                            {4, 6580}, {6, 4380}, {6, 4660}, {6, 5170}, {6, 5670}, {6, 6160}, {6, 6660}, {6, 7190},
                                            {6, 7720}, {6, 8220}, {6, 8730}, {6, 9100}, {6, 9480}, {2, 0}, {4, 0}, {6, 0}};
static const uint16_t Table_51312[32][2] = {{2, 1200}, {2, 1930}, {2, 3080}, {2, 4490}, {2, 6020}, {4, 3780}, {4, 4340},
                                            {4, 4900}, {4, 5530}, {4, 6160}, {4, 6580}, {6, 4660}, {6, 5170}, {6, 5670},
                                            {6, 6160}, {6, 6660}, {6, 7190}, {6, 7720}, {6, 8220}, {6, 8730}, {8, 6825},
                                            {8, 7110}, {8, 7540}, {8, 7970}, {8, 8410}, {8, 8850}, {8, 9165}, {8, 9480},
                                            {2, 0}, {4, 0}, {6, 0}, {8, 0}};
static const uint16_t Table_51313[32][2] = {{2, 300},  {2, 400},  {2, 500},  {2, 640},  {2, 780},  {2, 990},  {2, 1200}, {2, 1570},
                                            {2, 1930}, {2, 2510}, {2, 3080}, {2, 3790}, {2, 4490}, {2, 5260}, {2, 6020}, {4, 3400},
                                            {4, 3780}, {4, 4340}, {4, 4900}, {4, 5530}, {4, 6160}, {6, 4380}, {6, 4660}, {6, 5170},
                                            {6, 5670}, {6, 6160}, {6, 6660}, {6, 7190}, {6, 7720}, {2, 0}, {4, 0}, {6, 0}};
/* (Qm, R) of an MCS index under a table, as one key: Qm << 16 | 10 x R. Two (table, mcs) pairs decode the same computation iff the keys are equal. */
static uint32_t sim_cr(int table, int mcs)
{
  const uint16_t(*t)[2] = table == 0 ? Table_51311 : table == 1 ? Table_51312 : Table_51313;
  mcs = std::max(0, std::min(31, mcs));
  return ((uint32_t)t[mcs][0] << 16) | t[mcs][1];
}
/* the table choice changes the computation of this MCS index (a twin table is distinguishable on this grant) */
static bool sim_tbl_differs(int mcs, int ta, int tb) { return sim_cr(ta, mcs) != sim_cr(tb, mcs); }

/* The decode computation of one PDSCH as the DCI describes it: MCS index (-> Qm, R per table), PRBs, layers/ports, rv. */
struct SimKey { int mcs = 0, rank = 1, prb = 0, rv = 0; };
/* Compatibility (a decode of one can pass as the other) when the MCS index is read under tables ta (for a) and tb (for b). */
static bool sim_compat_tbl(const SimKey &a, int ta, const SimKey &b, int tb)
{
  return a.rank == b.rank && a.prb == b.prb && a.rv == b.rv && sim_cr(ta, a.mcs) == sim_cr(tb, b.mcs);
}
/* The receiver does not know which table an observed DCI used: compatible if ANY pair of ALIVE tables (tblmask bit t) gives the same computation
 * (notes 1.2: incompatible only if different under EVERY pair of alive tables). */
static bool sim_compat_any(const SimKey &a, const SimKey &b, unsigned tblmask)
{
  if (!tblmask) tblmask = 7;
  for (int ta = 0; ta < 3; ta++)
    for (int tb = 0; tb < 3; tb++)
      if (((tblmask >> ta) & 1) && ((tblmask >> tb) & 1) && sim_compat_tbl(a, ta, b, tb)) return true;
  return false;
}
/* The REAL occupant b (encoded with the true table tt) against grant a decoded under any alive hypothesis table. */
static bool sim_compat_real(const SimKey &a, const SimKey &b, int tt, unsigned tblmask)
{
  if (!tblmask) tblmask = 7;
  for (int ta = 0; ta < 3; ta++)
    if (((tblmask >> ta) & 1) && sim_compat_tbl(a, ta, b, tt)) return true;
  return false;
}
struct SimOcc {
  bool present = false, dci_seen = false, new_tx = true, exercised = false;
  SimKey key;
  double snr = 0;
  float snr_est_noise = 0;
};
/* other = another UE's PDSCH overlapping this RNTI's PRBs in this slot (DM-RS is cell-scrambled: it triggers the DM-RS oracle); its mask / last symbol
 * are those of a random catalogue entry. */
struct SimSlot { SimOcc occ; bool spur = false; SimKey spur_key; bool other = false; uint32_t other_mask = 0; int other_end = 0; };
/* One RNTI's timeline. Slot s is the PDSCH slot of occ(s); its DCI is in PDCCH slot s - k0 (the TRUE k0). PDCCH slots: 'D' and 'S'. PDSCH slots: 'D', and 'S'
 * only when the truth's allocation ends within the S slot's DL symbols (per-hypothesis rule: an entry ending after the last DL symbol cannot be in a
 * mixed slot); 'U' carries neither. Lazily generated, deterministic from the two seeds (srng: traffic, drng: DCI observation / spurious). Channel and link
 * adaptation evolve every slot: SNR AR(1) (--snr-rho; 0 = i.i.d.), MCS redrawn per slot with --mcs-change (default 1 - persist), independent of the
 * allocation (PRB, rank) which repeats per GRANT with --persist. */
class SlotTimeline {
 public:
  SlotTimeline(const SimCfg &c, int true_k0, uint64_t seed_s, uint64_t seed_d, bool fit_in_s = true,
               const std::vector<std::pair<uint32_t, int>> *decoys = nullptr)
      : cfg_(c), k0_(true_k0), fit_s_(fit_in_s), decoys_(decoys), srng_(seed_s), drng_(seed_d)
  {
    phase_ = cfg_.tdd.empty() ? 0 : (int)(srng_() % cfg_.tdd.size());
    const double A = cfg_.adjacency, pi = cfg_.grant_prob;
    q_ = A < 0 ? pi : (pi >= 1 ? 1.0 : std::min(1.0, pi * (1 - A) / (1 - pi)));
  }
  char dir(long s) const { return cfg_.tdd.empty() ? 'D' : cfg_.tdd[(size_t)(s + phase_) % cfg_.tdd.size()]; }
  bool dl(long s) const { return s >= 0 && dir(s) != 'U' && dir(s) != 'u'; } /* PDCCH-capable ('D' or 'S') */
  bool pdsch_slot_ok(long s, int end_sym) const
  {
    if (!dl(s)) return false;
    const char d = dir(s);
    return !(d == 'S' || d == 's') || end_sym <= cfg_.tdd_s_dl_symbols;
  }
  bool pdsch_possible(long s) const { return pdsch_slot_ok(s, fit_s_ ? 0 : 99) && dl(s - k0_); }
  const SimSlot &slot(long s)
  {
    static const SimSlot none;
    if (s < base_) return none;
    while (base_ + (long)buf_.size() <= s) gen(base_ + (long)buf_.size());
    return buf_[(size_t)(s - base_)];
  }
  /* next PDSCH-bearing slot strictly after the cursor (cursor starts at -1) */
  long next_pdsch_slot()
  {
    for (long n = 0; n < 4000000; n++) {
      cursor_++;
      if (slot(cursor_).occ.present) return cursor_;
      if ((n & 0xFFF) == 0xFFF) trim(cursor_ - 40); /* (>= 33 kept: probe layers / certification read back to -32) long idle stretch (e.g. no eligible slot at all): keep the buffer bounded */
    }
    return -1; /* nothing schedulable (e.g. TDD leaves no DL slot after a DL slot for k0 = 1) */
  }
  /* DCIs the RECEIVER holds for PDCCH slot x: the real DCI (scheduling PDSCH slot x + k0) if it was detected, plus a spurious (false-accepted) one.
   * Empty = nothing observed (a miss, a UL slot, or before the start): never evidence of absence. */
  std::vector<SimKey> observed_dci(long x)
  {
    std::vector<SimKey> v;
    if (x < 0) return v;
    const SimSlot a = slot(x + k0_);
    if (a.occ.present && a.occ.dci_seen) v.push_back(a.occ.key);
    const SimSlot b = slot(x);
    if (b.spur) v.push_back(b.spur_key);
    return v;
  }
  void trim(long keep_from)
  {
    if (keep_from - base_ < 4096) return;
    const long drop = keep_from - base_;
    buf_.erase(buf_.begin(), buf_.begin() + drop);
    base_ += drop;
  }
  long n_spur = 0;
 private:
  void gen(long s)
  {
    SimSlot sl;
    /* channel + link adaptation evolve every slot */
    const double rho = cfg_.snr_rho;
    snr_x_ = rho > 0 ? rho * snr_x_ + std::sqrt(1 - rho * rho) * nd_(srng_) : nd_(srng_);
    const double mc = cfg_.mcs_change < 0 ? 1 - cfg_.persist : cfg_.mcs_change;
    if (!have_mcs_ || ud_(srng_) < mc) { mcs_ = (int)(ud_(srng_) * 28) % 28; have_mcs_ = true; }
    if (pdsch_possible(s)) {
      bool on;
      if (!seen_elig_) on = cfg_.adjacency >= 0 ? true : ud_(srng_) < cfg_.grant_prob;
      else if (cfg_.adjacency < 0) on = ud_(srng_) < cfg_.grant_prob;
      else on = ud_(srng_) < (prev_on_ ? cfg_.adjacency : q_);
      seen_elig_ = true;
      prev_on_ = on;
      if (on) {
        SimKey k;
        if (have_prev_ && ud_(srng_) < cfg_.persist) k = prev_key_;
        else {
          k.rank = ud_(srng_) < cfg_.rank2_frac ? 2 : 1;
          k.prb = (int)(srng_() % 8);
        }
        prev_key_ = k; have_prev_ = true;
        k.mcs = mcs_;
        sl.occ.present = true;
        sl.occ.new_tx = ud_(srng_) < 0.75;
        k.rv = sl.occ.new_tx ? 0 : 1 + (int)(srng_() % 3);
        sl.occ.key = k;
        sl.occ.snr = cfg_.mu + cfg_.fade * snr_x_;
        sl.occ.exercised = false; /* slot model: derived from the MCS tables (sim_tbl_differs), not drawn */
        sl.occ.snr_est_noise = (float)(cfg_.snr_est_sigma * nd_(srng_));
        sl.occ.dci_seen = ud_(drng_) >= cfg_.dci_miss;
      }
    }
    if (decoys_ && !decoys_->empty() && cfg_.other_ue_occ > 0 && dl(s) && ud_(srng_) < cfg_.other_ue_occ) {
      const std::pair<uint32_t, int> &d = (*decoys_)[(size_t)(srng_() % decoys_->size())];
      if (pdsch_slot_ok(s, d.second)) { sl.other = true; sl.other_mask = d.first; sl.other_end = d.second; }
    }
    if (dl(s) && cfg_.dci_false > 0 && ud_(drng_) < cfg_.dci_false) {
      sl.spur = true;
      sl.spur_key.mcs = (int)(ud_(drng_) * 28) % 28;
      sl.spur_key.rank = 1 + (int)(drng_() % 2);
      sl.spur_key.prb = (int)(drng_() % 8);
      sl.spur_key.rv = (int)(drng_() % 4);
      n_spur++;
    }
    buf_.push_back(sl);
  }
  const SimCfg &cfg_;
  int k0_, phase_ = 0;
  bool fit_s_;
  const std::vector<std::pair<uint32_t, int>> *decoys_;
  std::mt19937_64 srng_, drng_;
  std::uniform_real_distribution<double> ud_{0, 1};
  std::normal_distribution<double> nd_{0, 1};
  std::vector<SimSlot> buf_;
  long base_ = 0, cursor_ = -1;
  double q_ = 0, snr_x_ = 0;
  int mcs_ = 0;
  bool seen_elig_ = false, prev_on_ = false, have_prev_ = false, have_mcs_ = false;
  SimKey prev_key_;
};
/* Certified flag (notes section 1.2), from OBSERVED DCIs only. Grant g (DCI slot t) is k0-unambiguous for leader L iff the sibling set D(L) is NOT EMPTY
 * and for every other alive catalogue offset k_s a DCI was observed at t + (L - k_s) and every observed DCI there is incompatible with g under EVERY pair
 * of alive MCS tables. A miss, a UL slot or a history gap (nothing observed) => ambiguous; a compatible DCI => ambiguous. An EMPTY sibling set is never
 * "certified" (vacuous certification: e.g. every other k0 layer was pruned, so the claim rests on assumption A2 only). `k0mask` = bit k set for every alive
 * catalogue offset k (0..32), `tblmask` bit t for every alive MCS table. */
static bool sim_certified(SlotTimeline &tl, long t, const SimKey &g, int L, uint64_t k0mask, unsigned tblmask)
{
  int n_sib = 0;
  for (int ks = 0; ks < 33; ks++) {
    if (ks == L || !((k0mask >> ks) & 1)) continue;
    n_sib++;
    const std::vector<SimKey> obs = tl.observed_dci(t + (L - ks));
    if (obs.empty()) return false;
    for (const SimKey &x : obs)
      if (sim_compat_any(x, g, tblmask)) return false;
  }
  return n_sib > 0;
}

/* Observed-mask set, mirroring nr_pdsch_config_sweep.c obs_set_t / obs_record / obs_admits (OBS_MASKS_MAX = 8; a full set drops new masks): one per RNTI (r->obs)
 * and one cell-wide (g_obs, promoted when a second distinct RNTI observes the same mask). last = S+L-1 (-1 unknown); k0 = the legacy (K39 bug) pin, -1 = none. */
#define SIM_OBS_MAX 8
struct SimObs {
  uint16_t mask[SIM_OBS_MAX] = {};
  int8_t last[SIM_OBS_MAX] = {}, k0[SIM_OBS_MAX] = {};
  int n = 0;
  int find(uint16_t m) const { for (int i = 0; i < n; i++) if (mask[i] == m) return i; return -1; }
  int record(uint16_t m, int l, int kk)
  {
    int k = find(m);
    if (k < 0 && n < SIM_OBS_MAX) { k = n++; mask[k] = m; last[k] = -1; k0[k] = -1; }
    if (k >= 0) {
      if (l >= 0) last[k] = (last[k] < 0 || last[k] == l) ? (int8_t)l : -1;
      if (kk >= 0) k0[k] = (k0[k] < 0 || k0[k] == kk) ? (int8_t)kk : -1;
    }
    return k;
  }
  bool admits(const nr_pdsch_cfg_hypothesis_t &h, int k) const
  {
    if (h.dmrs_mask != mask[k]) return false;
    if (last[k] >= 0 && (int)h.tda_start + (int)h.tda_length - 1 != last[k]) return false;
    if (k0[k] >= 0 && h.k0 != k0[k]) return false;
    return true;
  }
  bool any_admits(const nr_pdsch_cfg_hypothesis_t &h) const { for (int k = 0; k < n; k++) if (admits(h, k)) return true; return false; }
};

struct Grant {
  double snr;
  int rank, mcs;
  bool exercised, new_tx, adj_same;
  float snr_est_noise;
  /* slot model (v2) */
  long dci_slot = 0;
  SimKey key;
  bool certified[33] = {}; /* per k0 leader; for BC9 (feed_attr) -- NOT passed to the engine here */
};

/* min(1, C(t, m) * pf^m) in the log domain (0 when t < m or pf <= 0). */
static double sim_binom_pf(double t, int m, double pf)
{
  if (pf <= 0 || t < m) return 0;
  const double l = lgamma(t + 1.0) - lgamma(m + 1.0) - lgamma(t - m + 1.0) + m * std::log(pf);
  return std::min(1.0, std::exp(l));
}

static SimResult run_sim(const SimCfg &cfg)
{
  SimResult R;
  if (cfg.equiv && cfg.twins < 2) {
    fprintf(stderr, "nr_td_sim: --equiv 1 requires --twins >= 2 (the --twins < 2 stress arm is not equivalence-consistent)\n");
    exit(EXIT_FAILURE);
  }
  if (cfg.slot_model && (cfg.grant_prob <= 0 || cfg.grant_prob > 1)) {
    fprintf(stderr, "nr_td_sim: --grant-prob must be in (0, 1]\n");
    exit(EXIT_FAILURE);
  }
  auto tmpl = std::make_unique<nr_pdsch_config_sweep_state_t>();
  /* Production catalogue: init_legal with the production legality (type A only, masks deduplicated). */
  nr_pdsch_config_sweep_init_legal(tmpl.get(), cfg.catalog_tda, cfg.dmrs_typea_pos, sim_legality);
  auto st = std::make_unique<nr_pdsch_config_sweep_state_t>();
  const int n_hyp = tmpl->n_hyp;
  const long cap = (long)(cfg.cap_s * cfg.grants_per_s);
  std::vector<double> secs;
  double steady_sum = 0; long steady_n = 0;
  for (int a = 0; a < cfg.acq; a++) {
    std::mt19937_64 arng(mix(cfg.seed, a, 0xACC));
    int truth = (int)(arng() % (uint64_t)n_hyp);
    /* --truth-k0 K (>= 0): redraw until the truth has that k0 (extra arng draws only when forced; default -1 = the v1 draw) */
    for (int tries = 0; cfg.truth_k0 >= 0 && tries < 100000 && tmpl->hyp[truth].k0 != cfg.truth_k0; tries++) truth = (int)(arng() % (uint64_t)n_hyp);
    const nr_pdsch_cfg_hypothesis_t &T = tmpl->hyp[truth];
    /* TWINS. Every entry that differs from the truth ONLY in mcs_table is a physical twin: it decodes exactly when the
     * truth does unless the grant exercises the table. --twins n >= 2 (default 2 = all physical twins in this catalogue)
     * keeps them all. --twins n < 2 is an UNPHYSICAL stress arm: only the first n twins behave as twins, the other
     * other-table entries always fail (as if the table were always distinguishable). */
    bool twin_tbl[3] = {false, false, false};
    int ntw = 0;
    for (int i = 0; i < n_hyp && ntw < cfg.twins; i++) {
      const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[i];
      if (i != truth && h.mcs_table != T.mcs_table && !twin_tbl[h.mcs_table] && h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.k0 == T.k0
          && h.dmrs_add_pos == T.dmrs_add_pos && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask
          && h.mapping_type == T.mapping_type) {
        twin_tbl[h.mcs_table] = true;
        ntw++;
      }
    }
    R.twins_sum += ntw;
    R.twins_min = R.twins_min < 0 ? ntw : std::min<long>(R.twins_min, ntw);
    /* Runtime prior (nr_pdsch_config_sweep.c): an RNTI's FIRST converged context records a private prior
     * {mcs_table, dmrs_add_pos, dmrs_max_len, mapping_type} (:1392); prior_promote_locked (:962) publishes it cell-wide
     * (g_prior) once a SECOND distinct RNTI converged on identical fields; select() (:1069-1078) then prunes only
     * contexts CREATED AFTERWARDS with prune_prior (:471: keep table == prior and (other mapping type or same add_pos
     * and max_len)) before the observed-mask prune (:1081). One TDA per simulated RNTI, so the per-RNTI own-prior path
     * (sibling TDA contexts) never applies. Off when ISAC_TD_FIELDBOOK=1 (plan R2). Not modelled: g_prior invalidation
     * on a failed probation (:1326). */
    const bool use_prior = cfg.prior && cfg.fieldbook != 1;
    const bool fb2 = cfg.fieldbook == 2;
    struct Prior { uint8_t tbl, add, len, map; };
    std::vector<Prior> rnti_priors;
    bool gprior_valid = false;
    Prior gprior{};
    nr_td_fieldbook_t fb;
    nr_td_fieldbook_init(&fb, 2, 2);
    /* --fieldbook 2: stale-field injection. Wrong values come from the template; no wrong value => skip and count. */
    int inj_field = -1;
    int32_t inj_value = -1;
    if (fb2 && cfg.inject_wrong_field >= 0 && cfg.inject_wrong_field <= NR_TD_F_COUNT) {
      int32_t wv = -1;
      /* inject_wrong_field 3 (v2): TDRA with the SAME S/L/mapping as the truth but another k0 ("dormant true sibling" hole) */
      const bool k0_only = cfg.inject_wrong_field == NR_TD_F_COUNT;
      const int f = k0_only ? (int)NR_TD_F_TDRA : cfg.inject_wrong_field;
      if (k0_only) {
        for (int i = 0; i < n_hyp && wv < 0; i++) {
          const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[i];
          if (h.mapping_type == T.mapping_type && h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.k0 != T.k0)
            wv = nr_td_pack_tdra(h.tda_start, h.tda_length, h.mapping_type, h.k0);
        }
      } else if (f == NR_TD_F_TDRA) {
        for (int i = 0; i < n_hyp && wv < 0; i++) {
          const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[i];
          if (h.mapping_type == T.mapping_type && (h.tda_start != T.tda_start || h.tda_length != T.tda_length))
            wv = nr_td_pack_tdra(h.tda_start, h.tda_length, h.mapping_type, h.k0);
        }
      } else {
        const int cand = f == NR_TD_F_DMRS_ADD_POS ? (T.dmrs_add_pos + 1) % 4 : 3 - T.dmrs_max_len;
        for (int i = 0; i < n_hyp && wv < 0; i++)
          if ((f == NR_TD_F_DMRS_ADD_POS ? tmpl->hyp[i].dmrs_add_pos : tmpl->hyp[i].dmrs_max_len) == cand) wv = cand;
      }
      if (wv < 0) R.inject_skipped++;
      else { nr_td_fieldbook_force_promote(&fb, (nr_td_field_t)f, wv); inj_field = f; inj_value = wv; R.injected++; }
    }
    long acq_grants = 0; /* grants since the acquisition start (recovery metric) */
    long rec_grants = -1, rec_rntis = -1;
    struct Relied { bool conv = false, counted = false; uint32_t bits = 0; int32_t val[NR_TD_F_COUNT]; };
    std::vector<Relied> relied(cfg.rntis_per_acq);
    std::vector<std::pair<uint32_t, int>> decoys; /* (dmrs mask, end symbol) of every catalogue entry: what another UE's PDSCH can look like */
    for (int i = 0; i < n_hyp; i++) {
      const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[i];
      if (cfg.other_ue_same_cfg && (h.dmrs_add_pos != T.dmrs_add_pos || h.dmrs_max_len != T.dmrs_max_len || h.mapping_type != 0)) continue;
      decoys.push_back({h.dmrs_mask, h.tda_start + h.tda_length});
    }
    SimObs g_obs;                 /* slot model: cell-wide observed masks of this acquisition (promoted when 2 distinct RNTIs agree) */
    std::vector<SimObs> rnti_obs; /* slot model: the observed set of every earlier RNTI of the acquisition (contexts persist at runtime) */
    int n_ok_prev = 0; /* earlier RNTIs of this acquisition whose oracle state was ok */
    for (int k = 0; k < cfg.rntis_per_acq; k++) {
      std::mt19937_64 crng(mix(cfg.seed, a, 0x100 + k)); /* channel: fixed draws per grant, so arms are paired */
      std::mt19937_64 prng(mix(cfg.seed, a, 0x200 + k)); /* probe inconclusive draws */
      std::mt19937_64 orng(mix(cfg.seed, a, 0x400 + k)); /* oracle state/decoy draws (own stream: crng stays unchanged) */
      std::mt19937_64 frng(mix(cfg.seed, a, 0x500 + k)); /* HARQ-trap / CRC false-pass draws */
      std::mt19937_64 trng(mix(cfg.seed, a, 0x700 + k)); /* --retx-trap draws (own stream) */
      std::mt19937_64 krng(mix(cfg.seed, a, 0x600 + k)); /* --k0-trap-adj draws (own stream) */
      std::normal_distribution<double> nd(0, 1);
      std::uniform_real_distribution<double> ud(0, 1);
      std::unique_ptr<SlotTimeline> tl;
      if (cfg.slot_model)
        tl = std::make_unique<SlotTimeline>(cfg, T.k0, mix(cfg.seed, a, 0x800 + k), mix(cfg.seed, a, 0x900 + k), T.tda_start + T.tda_length <= cfg.tdd_s_dl_symbols,
                                            &decoys);
      SimObs own; /* this RNTI's observed set (r->obs) */
      long probe_tick = 0; /* jobs with an empty own slot (runtime s_probe_tick: 1 in 8 probes) */
      memcpy((void *)st.get(), (const void *)tmpl.get(), sizeof(*st));
      st->random_state = (uint32_t)mix(cfg.seed, a, 0x300 + k) | 1u; /* engine RNG seeded from --seed */
      nr_td_side_info_t si;
      memset(&si, 0, sizeof(si));
      si.obs_dmrs_mask = si.obs_qm = si.obs_mcs = si.obs_last_symbol = -1;
      si.f_S = si.f_L = si.f_mapping = si.f_k0 = si.f_dmrs_add_pos = si.f_dmrs_max_len = -1;
      si.w_sib1 = cfg.w_sib1; si.w_default = cfg.w_default; si.w_obs = cfg.w_obs; si.w_field = cfg.w_field; si.w_probe = cfg.w_probe;
      /* NOTE: dmrs_typeA_pos is set ONLY when w_default > 0 (the default-table score is inert otherwise); the SIB1
       * decoys below are mapping-type-A rows only (S 0..3), k0 0/1. */
      if (cfg.w_default > 0)
        si.dmrs_typeA_pos = cfg.dmrs_typea_pos == 0 ? 2 : 3;
      if (cfg.sib1) {
        si.n_sib1 = 4;
        si.sib1[0] = {T.tda_start, T.tda_length, T.mapping_type, T.k0};
        for (int j = 1; j < 4; j++) {
          uint8_t S = (uint8_t)(arng() % 4), L = (uint8_t)(3 + arng() % 11);
          if (S + L > 14) L = (uint8_t)(14 - S);
          si.sib1[j] = {S, L, 0, (uint8_t)(arng() % 2)};
        }
      }
      /* Oracle state, drawn ONCE per RNTI from orng (u < miss: no observation at all; u < miss+wrong: decoy DM-RS + wrong Qm table). */
      int ostate = 0;
      if (cfg.oracle_miss > 0 || cfg.oracle_wrong > 0) {
        const double u = std::uniform_real_distribution<double>(0, 1)(orng);
        ostate = u < cfg.oracle_miss ? 1 : (u < cfg.oracle_miss + cfg.oracle_wrong ? 2 : 0);
      }
      /* Decoy for `wrong`: first catalogue entry (template order from orng() % n_hyp, wrapping) whose (dmrs_mask, S+L) differs
       * from the truth's. None (theoretically impossible) => the RNTI is treated as ok and counted. */
      uint32_t decoy_mask = T.dmrs_mask; int decoy_end = T.tda_start + T.tda_length;
      if (ostate == 2) {
        const int start = (int)(orng() % (uint64_t)n_hyp);
        bool found = false;
        for (int j = 0; j < n_hyp && !found; j++) {
          const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[(start + j) % n_hyp];
          if (h.dmrs_mask != T.dmrs_mask || h.tda_start + h.tda_length != T.tda_start + T.tda_length) {
            decoy_mask = h.dmrs_mask; decoy_end = h.tda_start + h.tda_length; found = true;
          }
        }
        if (!found) ostate = 0;
      }
      const int wrong_table = (T.mcs_table + 1) % 3;
      const bool obs_on = ostate != 1; /* miss: neither DM-RS observation nor Qm sighting */
      if (cfg.w_obs > 0 && obs_on) { /* ordering inputs (I2): the DM-RS oracle is reliable in rfsim; qm/mcs filled from the first measurable grant */
        si.obs_dmrs_mask = decoy_mask;
        si.obs_last_symbol = decoy_end - 1;
      }
      if (cfg.fieldbook)
        nr_td_fieldbook_fill_side_info(&fb, &si);
      st->side = &si;
      st->p2 = cfg.p2 != 0;
      st->crc_accept = cfg.crc_accept != 0;
      st->geom_pin = cfg.geom_pin != 0;
      st->sib_pmin = cfg.sib_pmin;
      st->sib_eps = cfg.sib_eps;
      const uint16_t rnti = (uint16_t)(0x4000 + k);

      RntiRec rec{};
      rec.oracle_state = ostate;
      rec.truth_k0 = T.k0;
      rec.acq = a; rec.rnti_rank = k; rec.truth_table = T.mcs_table;
      /* Oracles (today's runtime): the DM-RS mask / last symbol / k0 observation (nr_pdsch_config_sweep_observe ->
       * prune_to_observed) runs on EVERY decoded grant (a no-op once nothing more can be removed), as at runtime; the cell-wide observation set is published
       * once a second RNTI has seen the same mask, so RNTIs k >= 2 start already pruned. k0 = the truth's (the oracle
       * measures on the slot of the job that carried the DM-RS). Qm oracle: two-sighting rule, below. */
      uint8_t qm_tables = 0; int qm_obs = 0;
      /* k0 rule (BC8 ruling 3). slot_model 0 = v1: every decoded grant observes; legacy 1 pins the truth's k0 (a perfect oracle), legacy 0 never prunes on k0. */
      auto do_observe = [&]() {
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.dmrs_mask == decoy_mask && h.tda_start + h.tda_length == decoy_end && (cfg.k0_oracle_legacy ? h.k0 == T.k0 : true);
        });
      };
      /* slot model (I1/I2, nr_pdsch_passive_queue.c:577/786): the runtime measures DM-RS ONLY in the DCI's OWN slot, ONLY for a k0 == 0 pick and only on the
       * grant's PRBs; whatever occupies them (this RNTI's own PDSCH with overlapping PRBs, or another UE's) gives the mask / last symbol. Legacy (K39 bug):
       * the observation also pins k0 = 0 (the job's hypothesised k0, kept per mask); fixed: mask + last symbol only. Then the runtime protocol of
       * nr_pdsch_config_sweep_observe() (sweep.c, the same on both arms): (1) record into r->obs; (2) promote the mask to g_obs once a second distinct RNTI
       * has it, refine g_obs; (3) restore_observed_typea: if no alive entry admits the new observation, re-append the matching type-A CATALOGUE entries
       * (catalogue k0, or the pinned k0 in legacy; evidence zeroed; qm / prior filters); (4) prune_to_observed: keep entries admitted by ANY mask of r->obs or
       * g_obs (a UNION, so an earlier correct mask protects the truth from a later foreign one), untouched when nothing matches. */
      auto prune_by_obs = [&](const SimObs &o1, const SimObs &o2) {
        if (o1.n + o2.n <= 0) return;
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) { return o1.any_admits(h) || o2.any_admits(h); });
      };
      auto restore_observed = [&](uint16_t m, int last, int kp) {
        SimObs one; one.n = 1; one.mask[0] = m; one.last[0] = (int8_t)last; one.k0[0] = (int8_t)kp;
        for (int i = 0; i < st->n_hyp; i++) if (one.admits(st->hyp[i], 0)) return;
        const int before = st->n_hyp;
        for (int i = 0; i < tmpl->n_hyp && st->n_hyp < NR_PDSCH_SWEEP_MAX_HYP; i++) {
          nr_pdsch_cfg_hypothesis_t h = tmpl->hyp[i];
          if (kp >= 0) h.k0 = (uint8_t)kp;
          if (h.mapping_type != 0 || !one.admits(h, 0)) continue;
          if (qm_obs >= 2 && !((qm_tables >> h.mcs_table) & 1)) continue;
          if (use_prior && gprior_valid && (h.mcs_table != gprior.tbl || (gprior.map == h.mapping_type && (h.dmrs_add_pos != gprior.add || h.dmrs_max_len != gprior.len)))) continue;
          bool dup = false;
          for (int j = before; j < st->n_hyp; j++) {
            const nr_pdsch_cfg_hypothesis_t &o = st->hyp[j];
            dup |= o.tda_start == h.tda_start && o.tda_length == h.tda_length && o.k0 == h.k0 && o.dmrs_mask == h.dmrs_mask && o.mcs_table == h.mcs_table;
          }
          if (dup) continue;
          const int at = st->n_hyp++;
          st->hyp[at] = h;
          st->trials[at] = st->ok[at] = 0;
          st->probe_pass[at] = st->probe_fail[at] = st->probe_inconclusive[at] = 0;
          st->ok_unique[at] = 0;
          st->fp_trials[at] = st->sib_trials[at] = 0;
          memset(st->geom_key, 0, sizeof(st->geom_key)); memset(st->ok_geom, 0, sizeof(st->ok_geom)); st->n_geom = 0; st->geom_blocked = false;
          st->order[at] = at;
        }
        rec.restores += st->n_hyp > before;
        rec.restore_hyp += st->n_hyp - before;
      };
      auto do_observe_slot = [&](uint32_t m, int e) {
        if (!m) return; /* the runtime ignores an empty mask */
        const int last = e - 1, kp = cfg.k0_oracle_legacy ? 0 : -1;
        own.record((uint16_t)m, last, kp);
        if (g_obs.find((uint16_t)m) < 0)
          for (const SimObs &o : rnti_obs) {
            const int j = o.find((uint16_t)m);
            if (j >= 0) { g_obs.record((uint16_t)m, o.last[j], o.k0[j]); break; }
          }
        if (g_obs.find((uint16_t)m) >= 0) g_obs.record((uint16_t)m, last, kp);
        restore_observed((uint16_t)m, last, kp);
        prune_by_obs(own, g_obs);
      };
      /* v1 cell-wide pre-pruning (slot model 0 only): the consensus of >= 2 earlier RNTIs whose oracles were ok: always the TRUTH's observation */
      auto do_observe_cellwide = [&]() {
        const bool pin_truth = cfg.k0_oracle_legacy;
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.dmrs_mask == T.dmrs_mask && h.tda_start + h.tda_length == T.tda_start + T.tda_length && (pin_truth ? h.k0 == T.k0 : true);
        });
      };
      /* Hypotheses are identified by CONTENT: oracle pruning re-indexes the state, so catalogue indices drift. */
      auto same_but_table = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.k0 == T.k0 && h.dmrs_add_pos == T.dmrs_add_pos
               && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask && h.mapping_type == T.mapping_type;
      };
      auto is_truth = [&](const nr_pdsch_cfg_hypothesis_t &h) { return same_but_table(h) && h.mcs_table == T.mcs_table; };
      /* honours --twins: only the SELECTED twins behave as twins (see above) */
      auto is_twin_h = [&](const nr_pdsch_cfg_hypothesis_t &h) { return same_but_table(h) && h.mcs_table != T.mcs_table && twin_tbl[h.mcs_table]; };
      int ti = -1; /* current index of the truth in the (re-indexed) state, -1 if pruned out */
      auto find_truth = [&]() {
        ti = -1;
        for (int i = 0; i < st->n_hyp && ti < 0; i++)
          if (is_truth(st->hyp[i])) ti = i;
      };
      auto prior_keep = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return h.mcs_table == gprior.tbl && (h.mapping_type != gprior.map || (h.dmrs_add_pos == gprior.add && h.dmrs_max_len == gprior.len));
      };
      if (!fb2) {
        if (use_prior && gprior_valid) sim_prune(st.get(), prior_keep);
        if (cfg.oracle && (cfg.slot_model ? true : (k >= 2 && n_ok_prev >= 2))) { if (cfg.slot_model) prune_by_obs(own, g_obs); else do_observe_cellwide(); }
      } else {
        /* Reversible pruning: the DESTRUCTIVE oracle pre-prune first (it compacts any mask), then the masks (index-based, applied
         * to the final catalogue; the sim never extends the catalogue, so no new entries appear after this point). */
        if (cfg.oracle && (cfg.slot_model ? true : (k >= 2 && n_ok_prev >= 2))) { if (cfg.slot_model) prune_by_obs(own, g_obs); else do_observe_cellwide(); }
        if (use_prior && gprior_valid) sim_dormant(st.get(), NR_TD_DORMANT_PRIOR, prior_keep);
        for (int f = 0; f < NR_TD_F_COUNT; f++) {
          int32_t v;
          if (!nr_td_fieldbook_prunes(&fb, (nr_td_field_t)f, &v)) continue;
          const int rc = sim_dormant(st.get(), NR_TD_DORMANT_FIELD_BASE + f,
                                     [&](const nr_pdsch_cfg_hypothesis_t &h) { return nr_td_fieldbook_hyp_matches((nr_td_field_t)f, v, &h); });
          if (rc >= 0) { rec.pruned_fields |= 1u << f; relied[k].val[f] = v; }
        }
        rec.active_start = nr_pdsch_config_sweep_n_active(st.get());
      }
      find_truth();
      bool distinguished = false;
      uint64_t k0mask = 0; unsigned tblmask = 0; int k0mask_nhyp = -1; /* slot model: alive k0 offsets / MCS tables of the truth's row, recomputed when the catalogue changes */
      int winner = -1;
      long g = 0;
      std::uniform_real_distribution<double> uf(0, 1);
      bool trap_active = false; /* per-grant HARQ-trap draw (frng) */
      bool retx_trap_active = false; /* per-grant --retx-trap draw (trng) */
      nr_td_pick_t pick_kind = NR_TD_PICK_EXPLORE;
      /* k0+-1 sibling of the truth (identical but for k0, any MCS table): the hypotheses the k0 trap can make pass */
      auto is_k0_sibling = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return (h.k0 == T.k0 + 1 || h.k0 == T.k0 - 1) && h.tda_start == T.tda_start && h.tda_length == T.tda_length
               && h.dmrs_add_pos == T.dmrs_add_pos && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask
               && h.mapping_type == T.mapping_type;
      };
      auto is_k0_neighbour = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return (h.k0 == T.k0 + 1 || h.k0 == T.k0 - 1) && h.mcs_table == T.mcs_table && h.tda_start == T.tda_start
               && h.tda_length == T.tda_length && h.dmrs_add_pos == T.dmrs_add_pos && h.dmrs_max_len == T.dmrs_max_len
               && h.dmrs_mask == T.dmrs_mask && h.mapping_type == T.mapping_type;
      };
      /* slot model: any k0 != the truth's with otherwise identical content (the runtime adds k0 >= 2 layers) */
      auto is_k0_sib_any = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return h.k0 != T.k0 && h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.dmrs_add_pos == T.dmrs_add_pos
               && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask && h.mapping_type == T.mapping_type;
      };
      /* twin table distinguishable on this grant: v1 = the i.i.d. `exercised` draw; slot model = (Qm, R) of the MCS index differ between the tables (M2) */
      auto twin_differs = [&](const nr_pdsch_cfg_hypothesis_t &h, const Grant &gr) {
        return cfg.slot_model ? sim_tbl_differs(gr.mcs, T.mcs_table, h.mcs_table) : gr.exercised;
      };
      auto base_pass = [&](int h, const Grant &gr, bool truth_pass) {
        if (is_truth(st->hyp[h])) return truth_pass;
        if (is_twin_h(st->hyp[h])) return twin_differs(st->hyp[h], gr) ? false : truth_pass;
        return false;
      };
      auto full_pass = [&](int h, const Grant &gr, bool truth_pass) {
        const bool p = base_pass(h, gr, truth_pass);
        if (trap_active && is_k0_neighbour(st->hyp[h])) { rec.harq_trap_passes++; return true; }
        if (retx_trap_active && is_k0_neighbour(st->hyp[h])) { rec.retx_trap_passes++; return true; }
        if (cfg.slot_model) {
          /* PHYSICAL shifted-slot trap: a hypothesis with k0' decodes slot dci + k0'; it passes iff that slot carries a same-RNTI transmission whose
           * decode computation is compatible with this DCI's (TBS/PRBs/MCS/layers/rv) and that transmission would pass at ITS SNR. No random probability. */
          const nr_pdsch_cfg_hypothesis_t &hs = st->hyp[h];
          if (is_k0_sib_any(hs)) {
            const SimOcc nb = tl->slot(gr.dci_slot + hs.k0).occ;
            /* NB twin_tbl / --twins are IGNORED here (stress-arm semantics differ): the trap follows the physical (Qm, R) equality for every hypothesis table.
             * the neighbour was encoded with the TRUE table; this hypothesis reads the grant's MCS under ITS table: the computations must coincide (I4) */
            if (nb.present && sim_compat_tbl(gr.key, hs.mcs_table, nb.key, T.mcs_table)) {
              const bool np = nb.key.rank <= cfg.n_rx && nb.snr >= nr_td_required_snr_db(nb.key.mcs, T.mcs_table);
              if (np) { rec.k0_trap_passes++; return true; }
            }
          }
        } else if (gr.adj_same && is_k0_sibling(st->hyp[h])) {
          /* the sibling decodes the adjacent slot's identical-allocation TB: same computation as the truth's, so it passes iff the truth
           * would (same table, or a twin table on a grant that does not exercise the table) */
          const nr_pdsch_cfg_hypothesis_t &hs = st->hyp[h];
          const bool q = hs.mcs_table == T.mcs_table ? truth_pass : (twin_tbl[hs.mcs_table] && !gr.exercised ? truth_pass : false);
          if (q) { rec.k0_trap_passes++; return true; }
        }
        if (!p && cfg.crc_false > 0 && uf(frng) < cfg.crc_false) { rec.false_passes++; return true; }
        return p;
      };
      int out_main_idx = -1;
      /* Engine-free class builder: class of the decoded hypothesis d on this grant = alive-or-dormant j (the FULL class, dormant
       * members included) with identical geometry/DM-RS/mapping and either the same MCS table or a grant that does not exercise
       * the table (then the computation is identical). cls[0] = d. Returns the member count. */
      auto build_class = [&](int d, const Grant &gr, int *cls) {
        int nc = 0;
        cls[nc++] = d;
        const nr_pdsch_cfg_hypothesis_t &hd = st->hyp[d];
        for (int j = 0; j < st->n_hyp; j++) {
          const nr_pdsch_cfg_hypothesis_t &hj = st->hyp[j];
          if (j != d && hj.tda_start == hd.tda_start && hj.tda_length == hd.tda_length && hj.k0 == hd.k0
              && hj.dmrs_add_pos == hd.dmrs_add_pos && hj.dmrs_max_len == hd.dmrs_max_len && hj.dmrs_mask == hd.dmrs_mask
              && hj.mapping_type == hd.mapping_type
              && (hj.mcs_table == hd.mcs_table || (cfg.slot_model ? !sim_tbl_differs(gr.mcs, hj.mcs_table, hd.mcs_table) : !gr.exercised)))
            cls[nc++] = j;
        }
        return nc;
      };
      /* --equiv 1 (lever E): credit the whole class. --equiv 0 with a lever on: credit idx[0] alone, attribute with the class. */
      auto feed_main = [&](bool pass, const Grant &gr) {
        int cls[NR_PDSCH_SWEEP_MAX_HYP];
        const int nc = build_class(out_main_idx, gr, cls);
        if (cfg.equiv)
          return nr_pdsch_config_sweep_feed_equiv_cx(st.get(), cls, nc, pass, gr.new_tx, pick_kind, true);
        return nr_pdsch_config_sweep_feed_attr_cx(st.get(), out_main_idx, cls, nc, pass, gr.new_tx, pick_kind, true);
      };
      for (; g < cap && winner < 0;) {
        g++;
        Grant gr;
        if (cfg.slot_model) {
          const long u = tl->next_pdsch_slot();
          if (u < 0) break;
          const SimOcc o = tl->slot(u).occ;
          gr.snr = o.snr; gr.rank = o.key.rank; gr.mcs = o.key.mcs; gr.new_tx = o.new_tx;
          gr.exercised = false;
          for (int tb = 0; tb < 3; tb++) gr.exercised |= twin_tbl[tb] && sim_tbl_differs(gr.mcs, T.mcs_table, tb); /* some twin is distinguishable */
          gr.snr_est_noise = o.snr_est_noise; gr.adj_same = false; gr.key = o.key;
          gr.dci_slot = u - T.k0;
          tl->trim(gr.dci_slot - 40); /* >= 33 slots kept: certification reads offsets down to -32 */
          if (!o.dci_seen) { rec.dci_missed++; continue; } /* the grant happened (time passes) but the receiver never detected its DCI: no job */
        } else {
        gr.snr = cfg.mu + cfg.fade * nd(crng);
        gr.rank = ud(crng) < cfg.rank2_frac ? 2 : 1;
        gr.mcs = (int)(ud(crng) * 28) % 28;
        gr.exercised = ud(crng) < cfg.table_exercise;
        gr.new_tx = ud(crng) < 0.75;
        gr.snr_est_noise = (float)(cfg.snr_est_sigma * nd(crng));
        gr.adj_same = cfg.k0_trap_adj > 0 && std::uniform_real_distribution<double>(0, 1)(krng) < cfg.k0_trap_adj;
        }
        const bool truth_pass = gr.rank <= cfg.n_rx && gr.snr >= nr_td_required_snr_db(gr.mcs, T.mcs_table);
        if (cfg.gate) {
          nr_td_grant_view_t v = {gr.rank, gr.mcs, 2}; /* table 2 = lowest requirement = most permissive */
          nr_td_rx_view_t rx = {cfg.n_rx, (float)gr.snr + gr.snr_est_noise, (int)std::min<long>(g, 1000000), 6.0f};
          const nr_td_gate_t gt = nr_td_grant_gate(&v, &rx);
          if (gt == NR_TD_GATED_PHYSICAL) { rec.gated_phys++; continue; }
          if (gt == NR_TD_GATED_CHANNEL_QUALITY) { rec.gated_chan++; continue; }
        }
        if (cfg.slot_model) {
          rec.proc_grants++;
          const long u = gr.dci_slot + T.k0;
          rec.adj_grants += tl->slot(u - 1).occ.present || tl->slot(u + 1).occ.present;
          /* alive catalogue offsets / tables of the truth's row (S, L, mapping, mask) incl. dormant ones: conservative (more siblings, harder to certify).
           * NB add_pos / max_len are ignored in the row key: the sibling set is a conservative SUPERSET (entries with other add_pos/max_len but the same
           * mask are equivalent for the occupant check), so it can only make certification harder, never easier. */
          if (st->n_hyp != k0mask_nhyp) {
            k0mask = 0; tblmask = 0;
            for (int i = 0; i < st->n_hyp; i++) {
              const nr_pdsch_cfg_hypothesis_t &h = st->hyp[i];
              if (h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.mapping_type == T.mapping_type && h.dmrs_mask == T.dmrs_mask && h.k0 <= 32) {
                k0mask |= UINT64_C(1) << h.k0;
                tblmask |= 1u << h.mcs_table;
              }
            }
            k0mask_nhyp = st->n_hyp;
          }
          bool trap_possible = false;
          for (int L = 0; L < 33; L++) {
            if (!((k0mask >> L) & 1)) continue;
            gr.certified[L] = sim_certified(*tl, gr.dci_slot, gr.key, L, k0mask, tblmask);
            if (L == T.k0) { rec.certified_grants += gr.certified[L]; continue; }
            /* a sibling leader L != truth: its decode reads the REAL occupant of slot dci + L. certified_wrong = certified although that occupant
             * is compatible (the real DCI was missed and a spurious incompatible DCI stood in for it). */
            const SimOcc nb = tl->slot(gr.dci_slot + L).occ;
            const bool compat = nb.present && sim_compat_real(gr.key, nb.key, T.mcs_table, tblmask);
            trap_possible |= compat;
            rec.certified_sib += gr.certified[L];
            rec.certified_wrong += gr.certified[L] && compat;
          }
          rec.trap_grants += trap_possible;
        }
        retx_trap_active = cfg.retx_trap > 0 && std::uniform_real_distribution<double>(0, 1)(trng) < cfg.retx_trap && !gr.new_tx;
        trap_active = cfg.harq_trap > 0 && uf(frng) < cfg.harq_trap && !(cfg.harq_trap_retx && gr.new_tx);
        int idx[NR_TD_MAX_K];
        nr_pdsch_cfg_hypothesis_t hy[NR_TD_MAX_K];
        const int n = nr_pdsch_config_sweep_next_k_ex(st.get(), cfg.K, idx, hy, &pick_kind); /* K=1 == next() (Task 4 bit-identity) */
        if (n < 1) break;
        rec.sib_trials += pick_kind == NR_TD_PICK_SIBLING;
        nr_td_outcome_t out[NR_TD_MAX_K];
        out_main_idx = idx[0];
        const int main_k0 = hy[0].k0; /* the oracle runs only on k0 == 0 jobs (queue.c:786) */
        for (int i = 0; i < n; i++) {
          out[i].hyp = idx[i];
          out[i].p2_admissible = false;
          const bool p = full_pass(idx[i], gr, truth_pass);
          if (i == 0) {
            out[i].kind = NR_TD_FULL_TB;
            out[i].result = p ? NR_TD_PASS : NR_TD_FAIL;
            rec.n_full++;
            if (idx[i] == ti) rec.truth_full++;
          } else {
            out[i].kind = NR_TD_CB_PROBE;
            const bool inc = ud(prng) < cfg.probe_inconclusive;
            out[i].result = inc ? NR_TD_INCONCLUSIVE : (p ? NR_TD_PASS : NR_TD_FAIL);
            out[i].p2_admissible = gr.new_tx && !inc;
            rec.n_probe++;
            if (cfg.p2 && out[i].p2_admissible && out[i].result == NR_TD_FAIL) rec.p2_admitted_fail++;
            if (cfg.check_correlation && !inc) {
              if ((is_truth(st->hyp[idx[i]]) && !truth_pass && p) || (is_twin_h(st->hyp[idx[i]]) && !gr.exercised && p != truth_pass))
                R.correlation_violations++;
            }
          }
        }
        if (gr.exercised && truth_pass)
          distinguished = true;
        const long t0 = ti >= 0 ? (long)st->trials[ti] - (long)st->ok[ti] : 0;
        const bool lever_on = cfg.crc_accept || cfg.geom_pin;
        const bool pre_blocked = st->geom_blocked, pre_sib_blocked = st->sib_blocked;
        std::vector<char> pre_act; /* the engine active set just before the call (honours every dormancy cause incl. an earlier GEOM pin) */
        uint64_t pre_gdorm[NR_TD_DWORDS];
        uint64_t pre_gkey = 0; /* geometry of the single pass slot = the group a pin in this call keeps */
        std::vector<uint16_t> pre_fp; /* the pin restarts the engine evidence (fp_trials cleared): keep the explore-trial counts it was decided on */
        if (cfg.geom_pin) {
          memcpy(pre_gdorm, st->dormant[NR_TD_DORMANT_GEOM], sizeof(pre_gdorm));
          if (st->n_geom == 1) { pre_act.resize(st->n_hyp); for (int i = 0; i < st->n_hyp; i++) pre_act[i] = nr_pdsch_config_sweep_is_active(st.get(), i); pre_gkey = st->geom_key[0]; pre_fp.assign(st->fp_trials, st->fp_trials + st->n_hyp); if (pick_kind == NR_TD_PICK_EXPLORE) pre_fp[out_main_idx]++; }
        }
        if (cfg.equiv && cfg.K > 1) {
          /* Lever E: the main decode credits its grant-equivalence class; probe outcomes keep feed_k (probes unchanged). */
          winner = feed_main(out[0].result == NR_TD_PASS, gr);
          if (n > 1) {
            const int w = nr_pdsch_config_sweep_feed_k(st.get(), out + 1, n - 1);
            if (w >= 0) winner = w;
          }
        } else if (cfg.equiv) {
          winner = feed_main(out[0].result == NR_TD_PASS, gr);
        } else if (lever_on) {
          /* Levers without lever E: the main decode goes through feed_attr (singleton crediting, full-class attribution); the
           * probes go through feed_k with the main outcome INCONCLUSIVE (not fed again), so every probe is fed. */
          winner = feed_main(out[0].result == NR_TD_PASS, gr);
          if (n > 1) {
            nr_td_outcome_t pr[NR_TD_MAX_K];
            memcpy(pr, out, sizeof(nr_td_outcome_t) * (size_t)n);
            pr[0].result = NR_TD_INCONCLUSIVE;
            const int w = nr_pdsch_config_sweep_feed_k(st.get(), pr, n);
            if (w >= 0) winner = w;
          }
        } else {
          winner = nr_pdsch_config_sweep_feed_k(st.get(), out, n);
        }
        rec.sib_blocks += !pre_sib_blocked && st->sib_blocked;
        if (cfg.geom_pin) {
          rec.geom_blocks += !pre_blocked && st->geom_blocked;
          rec.geom_pins += memcmp(pre_gdorm, st->dormant[NR_TD_DORMANT_GEOM], sizeof(pre_gdorm)) != 0;
        }
        if (winner >= 0 && st->winner_by_crc) {
          rec.crc_accepts++;
          /* Lever C bound: P(some wrong active h collects m unique false passes in its T_h EXPLORE trials) <= sum_h C(T_h, m) pf^m. */
          uint32_t tmx = 0;
          for (int i = 0; i < st->n_hyp; i++) if (nr_pdsch_config_sweep_is_active(st.get(), i)) tmx = std::max<uint32_t>(tmx, st->fp_trials[i]);
          const int m = nr_pdsch_config_sweep_crc_accept_m(nr_pdsch_config_sweep_n_active(st.get()), tmx);
          double b = 0;
          for (int i = 0; i < st->n_hyp; i++)
            if (nr_pdsch_config_sweep_is_active(st.get(), i) && !is_truth(st->hyp[i])) b += sim_binom_pf((double)st->fp_trials[i], m, cfg.crc_false);
          rec.crc_bound += std::min(1.0, b);
        }
        if (cfg.geom_pin && memcmp(pre_gdorm, st->dormant[NR_TD_DORMANT_GEOM], sizeof(pre_gdorm)) != 0) {
          /* Lever P bound at the pin: n_groups/T_max over the hypotheses active just BEFORE it (every cause but GEOM), then
           * sum over WRONG groups g of C(T_g, m) pf^m, T_g = EXPLORE trials of all members of g. A pin to a group other than the truth's is a wrong pin. */
          auto pre_active = [&](int i) { return i < (int)pre_act.size() && pre_act[i]; }; /* same active set the engine used for n_groups / T_g */
          const uint64_t tkey = nr_td_geom_key(&T);
          std::vector<std::pair<uint64_t, double>> grp; /* key -> T_g */
          for (int i = 0; i < st->n_hyp; i++) {
            if (!pre_active(i)) continue;
            const uint32_t fpi = i < (int)pre_fp.size() ? pre_fp[i] : 0;
            const uint64_t k = nr_td_geom_key(&st->hyp[i]);
            bool f = false;
            for (auto &e : grp) if (e.first == k) { e.second += fpi; f = true; break; }
            if (!f) grp.push_back({k, (double)fpi});
          }
          double tgmax = 0; /* T_g,max: max over active groups of the SUM of fp_trials (as the engine, nr_pdsch_config_sweep_geom_groups) */
          for (auto &e : grp) tgmax = std::max(tgmax, e.second);
          const int m = nr_pdsch_config_sweep_crc_accept_m((int)grp.size(), (uint32_t)tgmax);
          double b = 0;
          for (auto &e : grp) if (e.first != tkey) b += sim_binom_pf(e.second, m, cfg.crc_false);
          rec.geom_bound += std::min(1.0, b);
          rec.wrong_pins += pre_gkey != tkey;
        }
        if ((fb2 || cfg.fo_always) && winner < 0 && !st->fail_open && nr_pdsch_config_sweep_n_active(st.get()) < st->n_hyp
            && nr_pdsch_config_sweep_fail_open_due(st.get(), cfg.fo_alpha, cfg.fo_pmin)) {
          nr_pdsch_config_sweep_set_fail_open(st.get(), true);
          rec.pruned_fields = 0; /* fail-open: the RNTI is independent of every field */
          rec.fail_open = true;
        }
        if (ti >= 0 && truth_pass && (long)st->trials[ti] - (long)st->ok[ti] > t0) rec.truth_elim++;
        if ((cfg.oracle || cfg.w_obs > 0) && obs_on) {
          /* [ASSUMPTION] The DM-RS oracle needs the layout to be decodable at all (rank <= n_rx); conservative: the runtime
           * measures per-symbol coherence on whatever RX it has. A GATED grant (`continue` above) contributes no
           * observation: gated + unsettled -> no trial -> no job (plan R2). */
          const bool decoded = gr.rank <= cfg.n_rx;
          if (cfg.oracle && decoded && winner < 0) {
            if (!cfg.slot_model) { do_observe(); find_truth(); }
            else if (main_k0 == 0) {
              /* own-slot measurement on the grant's PRBs (I1/I2): this RNTI's own PDSCH there (overlapping PRBs) gives the truth's mask (or the decoy
               * of a wrong oracle state), another UE's PDSCH gives ITS mask; nothing there = no observation. The own PDSCH is the one in the same slot
               * and PRBs only: another UE cannot overlap the RNTI's own overlapping PDSCH. */
              const SimSlot own = tl->slot(gr.dci_slot);
              const bool same_ov = own.occ.present && own.occ.key.prb == gr.key.prb;
              if (same_ov || own.other) {
                const uint32_t m = same_ov ? decoy_mask : own.other_mask;
                const int e = same_ov ? decoy_end : own.other_end;
                do_observe_slot(m, e);
                find_truth();
              } else if ((probe_tick++ % 8) == 0) {
                /* runtime k0 >= 2 probe (queue.c:807-868): DM-RS-free own slot, 1 job in 8 measures slots +1 .. +K (K = min(32, spf - 2), spf = 20 at
                 * 30 kHz => 18; every slot assumed retained and written in time) and adds a k0 layer for each slot k >= 2 that shows DM-RS on the grant's
                 * PRBs (this RNTI's own overlapping PDSCH or another UE's). */
                rec.k0_probes++;
                for (int kk = 2; kk <= 18; kk++) {
                  const SimSlot fs = tl->slot(gr.dci_slot + kk);
                  if ((fs.occ.present && fs.occ.key.prb == gr.key.prb) || fs.other) {
                    const int add = nr_pdsch_config_sweep_add_k0_layer(st.get(), (uint8_t)kk);
                    rec.k0_probe_hyp += add;
                    rec.k0_probe_layers += add > 0;
                  }
                }
                find_truth();
              }
            }
          }
          /* [ASSUMPTION] Qm abstention gate: nr_pdsch_qm_classify abstains at low SNR, but no code gives the threshold;
           * modelled as "the truth would pass at this SNR/MCS". Runtime-backed part: called after feedback, two sightings. */
          if (decoded && truth_pass) {
            const int qm = nr_pdsch_qm_of_mcs((uint8_t)gr.mcs, ostate == 2 ? (uint8_t)wrong_table : T.mcs_table);
            const uint8_t mask = sim_qm_table_mask(gr.mcs, qm);
            if (cfg.w_obs > 0 && si.obs_qm < 0 && mask != 0 && mask != 0x7) { si.obs_qm = qm; si.obs_mcs = gr.mcs; }
            if (cfg.oracle && winner < 0 && mask != 0 && mask != 0x7) {
              const uint8_t inter = qm_obs ? (uint8_t)(qm_tables & mask) : mask;
              if (inter == 0) { qm_obs = 0; qm_tables = 0; }
              else {
                qm_tables = inter; qm_obs++;
                if (qm_obs >= 2) { sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) { return (inter >> h.mcs_table) & 1; }); find_truth(); }
              }
            }
          }
        }
      }
      rec.grants = g;
      if (tl) rec.dci_false = tl->n_spur;
      acq_grants += g;
      rec.seconds = (double)g / cfg.grants_per_s;
      rec.truth_kl_trials = ti >= 0 ? (long)st->trials[ti] : -1;
      snprintf(rec.winner_key, sizeof(rec.winner_key), "-");
      if (winner < 0) {
        rec.undecidable = true;
      } else {
        const nr_pdsch_cfg_hypothesis_t &w = st->hyp[winner];
        snprintf(rec.winner_key, sizeof(rec.winner_key), "%d/%d/%d/%d/%d/%x/%d/%d", w.tda_start, w.tda_length, w.k0, w.dmrs_add_pos,
                 w.dmrs_max_len, w.dmrs_mask, w.mapping_type, w.mcs_table);
        rec.wrong = !is_truth(st->hyp[winner]) && !(is_twin_h(st->hyp[winner]) && !distinguished);
        rec.winner_ok = !rec.wrong;
        if (use_prior) {
          const nr_pdsch_cfg_hypothesis_t &w = st->hyp[winner];
          const Prior p{w.mcs_table, w.dmrs_add_pos, w.dmrs_max_len, w.mapping_type};
          if (!gprior_valid)
            for (const Prior &q : rnti_priors)
              if (q.tbl == p.tbl && q.add == p.add && q.len == p.len && q.map == p.map) { gprior = p; gprior_valid = true; break; }
          rnti_priors.push_back(p);
        }
        if (cfg.fieldbook) {
          int32_t before[NR_TD_F_COUNT];
          for (int f = 0; f < NR_TD_F_COUNT; f++) before[f] = fb.f[f].value;
          nr_td_fieldbook_converged(&fb, rnti, &st->hyp[winner], (uint64_t)g, fb2 ? rec.pruned_fields : 0);
          for (int f = 0; f < NR_TD_F_COUNT; f++) {
            if (before[f] != -1 && fb.f[f].value != before[f]) rec.withdrawals++;
            if (fb.f[f].value != -1 && fb.f[f].value != before[f]) rec.promotions++;
          }
          if (fb2) { relied[k].conv = true; relied[k].bits = rec.pruned_fields; }
        }
      }
      if (fb2) {
        /* converged RNTIs whose winner relied on a field (pruned on it) that is no longer PROMOTED at that value */
        for (int j = 0; j < cfg.rntis_per_acq; j++) {
          if (!relied[j].conv || relied[j].counted) continue;
          for (int f = 0; f < NR_TD_F_COUNT; f++)
            if ((relied[j].bits >> f & 1) && (nr_td_fieldbook_state(&fb, (nr_td_field_t)f) != NR_TD_FS_PROMOTED || fb.f[f].value != relied[j].val[f])) {
              relied[j].counted = true; R.untrusted_after++; break;
            }
        }
        if (inj_field >= 0 && rec_grants < 0) {
          const nr_td_field_state_t fs = nr_td_fieldbook_state(&fb, (nr_td_field_t)inj_field);
          /* the injected WRONG value is gone: field no longer PROMOTED/SUSPECT, or promoted/suspect at a different (re-learned true) value */
          if ((fs != NR_TD_FS_PROMOTED && fs != NR_TD_FS_SUSPECT) || fb.f[inj_field].value != inj_value) { rec_grants = acq_grants; rec_rntis = k + 1; }
        }
        R.active_start_sum += rec.active_start;
      }
      R.fail_opens += rec.fail_open;
      R.total_grants += g; R.wrong += rec.wrong; R.undecidable += rec.undecidable; R.acquisitions_rntis++;
      R.n_full += rec.n_full; R.n_probe += rec.n_probe; R.gated_phys += rec.gated_phys; R.gated_chan += rec.gated_chan;
      R.promotions += rec.promotions; R.withdrawals += rec.withdrawals;
      n_ok_prev += ostate == 0;
      if (cfg.slot_model) rnti_obs.push_back(own);
      rec.n_hyp_end = st->n_hyp;
      R.restores += rec.restores; R.restore_hyp += rec.restore_hyp; R.k0_probes += rec.k0_probes; R.k0_probe_hyp += rec.k0_probe_hyp; R.k0_probe_layers += rec.k0_probe_layers; R.n_hyp_end += rec.n_hyp_end;
      R.dci_missed += rec.dci_missed; R.dci_false += rec.dci_false; R.proc_grants += rec.proc_grants; R.adj_grants += rec.adj_grants;
      R.trap_grants += rec.trap_grants; R.certified_grants += rec.certified_grants; R.certified_sib += rec.certified_sib; R.certified_wrong += rec.certified_wrong;
      R.oracle_miss_rntis += ostate == 1; R.oracle_wrong_rntis += ostate == 2;
      R.harq_trap_passes += rec.harq_trap_passes; R.false_passes += rec.false_passes;
      R.geom_pins += rec.geom_pins; R.geom_blocks += rec.geom_blocks; R.crc_accepts += rec.crc_accepts;
      R.sib_trials += rec.sib_trials; R.sib_blocks += rec.sib_blocks; R.retx_trap_passes += rec.retx_trap_passes; R.k0_trap_passes += rec.k0_trap_passes;
      R.wrong_pins += rec.wrong_pins; R.geom_bound += rec.geom_bound; R.crc_bound += rec.crc_bound;
      R.crc_wrong += rec.wrong && rec.crc_accepts > 0;
      R.truth_eliminated_by_probe += rec.truth_elim; R.p2_admitted_fail += rec.p2_admitted_fail;
      if (!rec.undecidable) {
        secs.push_back(rec.seconds);
        if (k >= 2) { steady_sum += rec.seconds; steady_n++; }
        R.mean_grants += (double)g; R.n_decided++;
        SimResult::TableStat &ts = R.by_table[rec.truth_table];
        ts.n++; ts.wrong += rec.wrong; ts.sum_s += rec.seconds; ts.v.push_back(rec.seconds);
      }
      R.recs.push_back(rec);
    }
    if (inj_field >= 0) {
      if (rec_grants < 0) R.recovery_never++;
      else { R.recovery_grants.push_back(rec_grants); R.recovery_rntis.push_back(rec_rntis); }
    }
  }
  if (!secs.empty()) {
    std::sort(secs.begin(), secs.end());
    R.median_s = secs[secs.size() / 2];
    R.p95_s = secs[std::min(secs.size() - 1, (size_t)std::ceil(0.95 * secs.size()) - 1)];
    double sum = 0;
    for (double x : secs) sum += x;
    R.mean_s = sum / (double)secs.size();
    R.mean_grants /= (double)R.n_decided;
  }
  if (steady_n) R.mean_s_steady = steady_sum / (double)steady_n;
  for (auto &ts : R.by_table) std::sort(ts.v.begin(), ts.v.end());
  return R;
}

#ifndef NR_TD_SIM_NO_MAIN
int main(int argc, char **argv)
{
  logInit();
  SimCfg c = SimCfg::defaults();
  for (int i = 1; i < argc; i++) {
    const std::string f = argv[i];
    if (f == "--help") {
      puts("nr_td_sim: Technique D Monte-Carlo (SIMULATED results). Flags (value follows): --acq --seed --catalog-tda --p-true-snr-mu --fade-db\n"
           "  --snr-est-sigma --n-rx --rank2-frac --grants-per-s --sib1 --K --w-sib1 --w-default --w-obs --w-field --w-probe --fieldbook\n"
           "  --oracle-miss P --oracle-wrong P --harq-trap P --crc-false P (realism, default 0)\n"
           "  --fieldbook 0|1|2 (0 prior pruning, 1 ordering-only field book, 2 reversible pruning: prior + promoted fields as dormant masks, fail-open)\n"
           "  --inject-wrong-field F (fieldbook 2: 0 TDRA, 1 add_pos, 2 max_len; force-promote a wrong value) --fo-alpha 1e-3 --fo-pmin 0.05\n"
           "  --gate --p2 --rntis-per-acq --probe-inconclusive --table-exercise --cap-s --oracle --prior --dmrs-typea-pos\n"
           "  --crc-accept 0|1 (lever C: CRC-pass acceptance) --geom-pin 0|1 (lever P: geometry pinning); both experimental, default 0;\n"
           "  --harq-trap P / --harq-trap-retx 0|1: LEGACY trap models (BC0, any grant / retransmission only); kept for byte identity\n"
           "  --retx-trap P (on new_tx==false grants the k0+-1 sibling passes with probability P)\n"
           "  --k0-trap-adj A (new-TB k0 trap: with probability A per grant the adjacent slot has an identical allocation/MCS; then every k0+-1 sibling\n"
           "    passes iff the truth's decode would). Own random streams. Counters retx_trap_passes, k0_trap_passes (separate from false_passes)\n"
           "  Simulator v2 (all default to v1 behaviour; --slot-model 0 output is byte-identical to v1):\n"
           "  --slot-model 0|1 (slot-indexed grants per RNTI + PHYSICAL shifted-slot k0 trap + DCI observation + certified flag)\n"
           "  --grant-prob P (per eligible DL slot, default 0.5) --adjacency A (P(grant | previous eligible slot had one); default -1 = i.i.d. grant-prob)\n"
           "  --other-ue-same-cfg 0|1 (other UE's DM-RS add_pos/max_len = the truth's, i.e. few distinct masks; default 0 = any catalogue entry)\n"
           "  --other-ue-occ P (another UE's PDSCH overlaps the grant's PRBs in a DL slot; triggers the K39 legacy oracle + wrong mask) --tdd-s-dl-symbols N (6)\n"
           "  --mcs-change P (per-slot MCS redraw, default 1-persist) --snr-rho R (AR(1) SNR, default 0 = i.i.d.)\n"
           "  --persist RHO (P(next grant repeats the allocation PRB/rank), default 0.9; MCS changes per slot with prob 1-RHO unless --mcs-change) --dci-miss P --dci-false P (spurious DCI per PDCCH slot)\n"
           "  --tdd \"DDDSU\" (U slots carry no PDSCH/PDCCH; S counts as DL) --k0-oracle-legacy 1|0 (1: today's oracle, with the slot model the K39 bug\n"
           "    (claims k0 = 0 when the DCI's own slot carries any same-RNTI PDSCH); 0: K39 fix, mask+last symbol only) --truth-k0 K (force the truth's k0)\n"
           "  --fo-always 0|1 (fail-open outside fieldbook 2) --inject-wrong-field 3 (fieldbook 2: TDRA with the truth's S/L/mapping but another k0)\n"
           "  --sib-pmin P (k0-sibling guard p_min, default 0.05; 0 = guard DISABLED, i.e. fast-path fix A only) --sib-eps E (default 1e-6)\n"
           "  with --equiv 0 the main decode is fed through feed_attr (singleton crediting, full-class attribution)\n"
           "  --equiv 0|1 (lever E: grant-equivalence crediting of the main decode; requires --twins >= 2)\n  --twins N (default 2 = all physical twins, i.e. every other-table entry). N < 2 is an UNPHYSICAL stress arm: only N\n"
           "  twins behave as twins, the other other-table entries always fail.");
      return 0;
    }
    if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", f.c_str()); return 2; }
    const char *v = argv[++i];
    if (f == "--acq") c.acq = atoi(v);
    else if (f == "--seed") c.seed = atoi(v);
    else if (f == "--catalog-tda") c.catalog_tda = atoi(v);
    else if (f == "--p-true-snr-mu") c.mu = atof(v);
    else if (f == "--fade-db") c.fade = atof(v);
    else if (f == "--snr-est-sigma") c.snr_est_sigma = atof(v);
    else if (f == "--n-rx") c.n_rx = atoi(v);
    else if (f == "--rank2-frac") c.rank2_frac = atof(v);
    else if (f == "--grants-per-s") c.grants_per_s = atof(v);
    else if (f == "--sib1") c.sib1 = atoi(v);
    else if (f == "--K") c.K = atoi(v);
    else if (f == "--w-sib1") c.w_sib1 = (float)atof(v);
    else if (f == "--w-default") c.w_default = (float)atof(v);
    else if (f == "--w-obs") c.w_obs = (float)atof(v);
    else if (f == "--w-field") c.w_field = (float)atof(v);
    else if (f == "--w-probe") c.w_probe = (float)atof(v);
    else if (f == "--fieldbook") c.fieldbook = atoi(v);
    else if (f == "--gate") c.gate = atoi(v);
    else if (f == "--p2") c.p2 = atoi(v);
    else if (f == "--twins") c.twins = atoi(v);
    else if (f == "--equiv") c.equiv = atoi(v);
    else if (f == "--rntis-per-acq") c.rntis_per_acq = atoi(v);
    else if (f == "--probe-inconclusive") c.probe_inconclusive = atof(v);
    else if (f == "--table-exercise") c.table_exercise = atof(v);
    else if (f == "--cap-s") c.cap_s = atof(v);
    else if (f == "--oracle") c.oracle = atoi(v);
    else if (f == "--oracle-miss") c.oracle_miss = atof(v);
    else if (f == "--oracle-wrong") c.oracle_wrong = atof(v);
    else if (f == "--harq-trap") c.harq_trap = atof(v);
    else if (f == "--crc-false") c.crc_false = atof(v);
    else if (f == "--crc-accept") c.crc_accept = atoi(v);
    else if (f == "--harq-trap-retx") c.harq_trap_retx = atoi(v);
    else if (f == "--retx-trap") c.retx_trap = atof(v);
    else if (f == "--k0-trap-adj") c.k0_trap_adj = atof(v);
    else if (f == "--sib-pmin") c.sib_pmin = (float)atof(v);
    else if (f == "--sib-eps") c.sib_eps = (float)atof(v);
    else if (f == "--geom-pin") c.geom_pin = atoi(v);
    else if (f == "--inject-wrong-field") c.inject_wrong_field = atoi(v);
    else if (f == "--fo-alpha") c.fo_alpha = atof(v);
    else if (f == "--fo-pmin") c.fo_pmin = atof(v);
    else if (f == "--slot-model") c.slot_model = atoi(v);
    else if (f == "--grant-prob") c.grant_prob = atof(v);
    else if (f == "--adjacency") c.adjacency = atof(v);
    else if (f == "--persist") c.persist = atof(v);
    else if (f == "--dci-miss") c.dci_miss = atof(v);
    else if (f == "--dci-false") c.dci_false = atof(v);
    else if (f == "--tdd") c.tdd = v;
    else if (f == "--k0-oracle-legacy") c.k0_oracle_legacy = atoi(v);
    else if (f == "--other-ue-occ") c.other_ue_occ = atof(v);
    else if (f == "--other-ue-same-cfg") c.other_ue_same_cfg = atoi(v);
    else if (f == "--tdd-s-dl-symbols") c.tdd_s_dl_symbols = atoi(v);
    else if (f == "--mcs-change") c.mcs_change = atof(v);
    else if (f == "--snr-rho") c.snr_rho = atof(v);
    else if (f == "--fo-always") c.fo_always = atoi(v);
    else if (f == "--truth-k0") c.truth_k0 = atoi(v);
    else if (f == "--prior") c.prior = atoi(v);
    else if (f == "--dmrs-typea-pos") c.dmrs_typea_pos = atoi(v);
    else { fprintf(stderr, "unknown flag %s\n", f.c_str()); return 2; }
  }
  const SimResult r = run_sim(c);
  for (const RntiRec &x : r.recs) {
    printf("{\"acq\":%d,\"rnti_rank\":%d,\"truth_table\":%d,\"grants\":%ld,\"seconds\":%.4f,\"winner_ok\":%s,\"wrong\":%d,\"undecidable\":%d,"
           "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld,"
           "\"p2_admitted_fail\":%ld,\"truth_full\":%ld,\"truth_kl_trials\":%ld,\"truth_elim\":%ld,\"winner\":\"%s\",\"oracle_state\":\"%s\"",
           x.acq, x.rnti_rank, x.truth_table, x.grants, x.seconds, x.winner_ok ? "true" : "false", (int)x.wrong, (int)x.undecidable, x.n_full,
           x.n_probe, x.gated_phys, x.gated_chan, x.promotions, x.withdrawals, x.p2_admitted_fail, x.truth_full, x.truth_kl_trials,
           x.truth_elim, x.winner_key, x.oracle_state == 1 ? "miss" : x.oracle_state == 2 ? "wrong" : "ok");
    /* fieldbook-2 keys are emitted only for --fieldbook 2: --fieldbook 0/1 output stays byte-identical to the pre-BC5 simulator. */
    if (c.fieldbook == 2) printf(",\"active_start\":%d,\"fail_open\":%s,\"pruned_fields\":%u", x.active_start, x.fail_open ? "true" : "false", x.pruned_fields);
    /* lever keys only when a lever is on: with both 0 the output stays byte-identical to 25c4d5ac7e. */
    if (c.geom_pin || c.crc_accept) printf(",\"geom_pins\":%ld,\"geom_blocks\":%ld,\"crc_accepts\":%ld,\"sib_trials\":%ld,\"sib_blocks\":%ld", x.geom_pins, x.geom_blocks, x.crc_accepts, x.sib_trials, x.sib_blocks);
    if (c.retx_trap > 0 || c.k0_trap_adj > 0 || c.slot_model) printf(",\"retx_trap_passes\":%ld,\"k0_trap_passes\":%ld", x.retx_trap_passes, x.k0_trap_passes);
    if (c.slot_model)
      printf(",\"truth_k0\":%d,\"dci_missed\":%ld,\"dci_false\":%ld,\"proc_grants\":%ld,\"adj_grants\":%ld,\"trap_grants\":%ld,\"certified_grants\":%ld,"
             "\"certified_sib\":%ld,\"certified_wrong\":%ld,\"k0_probes\":%ld,\"k0_probe_hyp\":%ld,\"k0_probe_layers\":%ld,\"n_hyp_end\":%ld,\"restores\":%ld,\"restore_hyp\":%ld",
             x.truth_k0, x.dci_missed, x.dci_false, x.proc_grants, x.adj_grants, x.trap_grants, x.certified_grants,
             x.certified_sib, x.certified_wrong, x.k0_probes, x.k0_probe_hyp, x.k0_probe_layers, x.n_hyp_end, x.restores, x.restore_hyp);
    puts("}");
  }
  /* Quantiles/means are over DECIDED RNTIs only; capped (undecidable) RNTIs are censored and counted separately.
   * NB: separation is checked every 16 trials, so seconds move in steps of 16 x n_hyp / grants_per_s: prefer the means. */
  printf("{\"summary\":{\"acq\":%d,\"rntis\":%ld,\"decided\":%ld,\"median_s\":%.3f,\"p95_s\":%.3f,\"mean_s\":%.3f,"
         "\"mean_grants\":%.1f,\"wrong\":%ld,\"undecidable\":%ld,"
         "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld,"
         "\"p2\":%d,\"p2_admitted_fail\":%ld,\"truth_eliminated_by_probe\":%ld,"
         "\"twins_requested\":%d,\"twins_min\":%ld,\"cap_s\":%.0f,\"seed\":%d,\"n_rx\":%d,\"K\":%d,\"oracle\":%d,\"prior\":%d,"
         "\"oracle_miss_rntis\":%ld,\"oracle_wrong_rntis\":%ld,\"harq_trap_passes\":%ld,\"false_passes\":%ld,\"by_table\":{",
         c.acq, r.acquisitions_rntis, r.n_decided, r.median_s, r.p95_s, r.mean_s, r.mean_grants, r.wrong, r.undecidable,
         r.n_full, r.n_probe, r.gated_phys, r.gated_chan, r.promotions, r.withdrawals, c.p2, r.p2_admitted_fail,
         r.truth_eliminated_by_probe, c.twins, r.twins_min, c.cap_s,
         c.seed, c.n_rx, c.K, c.oracle, c.prior && c.fieldbook != 1, r.oracle_miss_rntis, r.oracle_wrong_rntis, r.harq_trap_passes, r.false_passes);
  for (int t = 0; t < 3; t++) {
    const SimResult::TableStat &ts = r.by_table[t];
    printf("%s\"%d\":{\"n\":%ld,\"median_s\":%.3f,\"mean_s\":%.3f,\"wrong\":%ld}", t ? "," : "", t, ts.n,
           ts.v.empty() ? 0.0 : ts.v[ts.v.size() / 2], ts.n ? ts.sum_s / (double)ts.n : 0.0, ts.wrong);
  }
  printf("}");
  if (c.fieldbook == 2) {
    auto mean_l = [](const std::vector<long> &v) { double t = 0; for (long x : v) t += (double)x; return v.empty() ? -1.0 : t / (double)v.size(); };
    printf(",\"fail_opens\":%ld,\"active_start_mean\":%.1f,\"mean_s_steady\":%.3f,\"untrusted_after\":%ld,\"injected\":%ld,\"inject_skipped\":%ld,"
           "\"recovery_grants\":%.1f,\"recovery_rntis\":%.2f,\"recovery_never\":%ld",
           r.fail_opens, r.acquisitions_rntis ? r.active_start_sum / (double)r.acquisitions_rntis : 0.0, r.mean_s_steady, r.untrusted_after,
           r.injected, r.inject_skipped, mean_l(r.recovery_grants), mean_l(r.recovery_rntis), r.recovery_never);
  }
  if (c.geom_pin || c.crc_accept)
    printf(",\"sib_trials\":%ld,\"sib_blocks\":%ld,\"sib_pmin\":%g,\"sib_eps\":%g", r.sib_trials, r.sib_blocks, (double)c.sib_pmin, (double)c.sib_eps);
  if (c.slot_model)
    printf(",\"slot_model\":1,\"k0_oracle_legacy\":%d,\"dci_missed\":%ld,\"dci_false\":%ld,\"proc_grants\":%ld,\"adj_grants\":%ld,\"trap_grants\":%ld,"
           "\"certified_grants\":%ld,\"certified_sib\":%ld,\"certified_wrong\":%ld,\"k0_probes\":%ld,\"k0_probe_hyp\":%ld,\"k0_probe_layers\":%ld,"
           "\"n_hyp_end\":%ld,\"restores\":%ld,\"restore_hyp\":%ld", c.k0_oracle_legacy, r.dci_missed, r.dci_false, r.proc_grants,
           r.adj_grants, r.trap_grants, r.certified_grants, r.certified_sib, r.certified_wrong, r.k0_probes, r.k0_probe_hyp, r.k0_probe_layers, r.n_hyp_end, r.restores, r.restore_hyp);
  if (c.fo_always && c.fieldbook != 2) printf(",\"fail_opens\":%ld", r.fail_opens);
  if (c.retx_trap > 0 || c.k0_trap_adj > 0 || c.slot_model)
    printf(",\"retx_trap\":%g,\"k0_trap_adj\":%g,\"retx_trap_passes\":%ld,\"k0_trap_passes\":%ld", c.retx_trap, c.k0_trap_adj, r.retx_trap_passes,
           r.k0_trap_passes);
  if (c.geom_pin || c.crc_accept)
    printf(",\"geom_pin\":%d,\"crc_accept\":%d,\"geom_pins\":%ld,\"geom_blocks\":%ld,\"crc_accepts\":%ld,\"crc_wrong\":%ld,"
           "\"crc_bound\":%.6g,\"wrong_pins\":%ld,\"geom_bound\":%.6g,\"crc_false\":%g",
           c.geom_pin, c.crc_accept, r.geom_pins, r.geom_blocks, r.crc_accepts, r.crc_wrong, r.crc_bound, r.wrong_pins, r.geom_bound,
           c.crc_false);
  printf("}}\n");
  return 0;
}
#endif
