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

struct Grant {
  double snr;
  int rank, mcs;
  bool exercised, new_tx;
  float snr_est_noise;
};

static SimResult run_sim(const SimCfg &cfg)
{
  SimResult R;
  if (cfg.equiv && cfg.twins < 2) {
    fprintf(stderr, "nr_td_sim: --equiv 1 requires --twins >= 2 (the --twins < 2 stress arm is not equivalence-consistent)\n");
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
    const int truth = (int)(arng() % (uint64_t)n_hyp);
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
    if (fb2 && cfg.inject_wrong_field >= 0 && cfg.inject_wrong_field < NR_TD_F_COUNT) {
      int32_t wv = -1;
      const int f = cfg.inject_wrong_field;
      if (f == NR_TD_F_TDRA) {
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
    int n_ok_prev = 0; /* earlier RNTIs of this acquisition whose oracle state was ok */
    for (int k = 0; k < cfg.rntis_per_acq; k++) {
      std::mt19937_64 crng(mix(cfg.seed, a, 0x100 + k)); /* channel: fixed draws per grant, so arms are paired */
      std::mt19937_64 prng(mix(cfg.seed, a, 0x200 + k)); /* probe inconclusive draws */
      std::mt19937_64 orng(mix(cfg.seed, a, 0x400 + k)); /* oracle state/decoy draws (own stream: crng stays unchanged) */
      std::mt19937_64 frng(mix(cfg.seed, a, 0x500 + k)); /* HARQ-trap / CRC false-pass draws */
      std::normal_distribution<double> nd(0, 1);
      std::uniform_real_distribution<double> ud(0, 1);
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
      const uint16_t rnti = (uint16_t)(0x4000 + k);

      RntiRec rec{};
      rec.oracle_state = ostate;
      rec.acq = a; rec.rnti_rank = k; rec.truth_table = T.mcs_table;
      /* Oracles (today's runtime): the DM-RS mask / last symbol / k0 observation (nr_pdsch_config_sweep_observe ->
       * prune_to_observed) runs on EVERY decoded grant (a no-op once nothing more can be removed), as at runtime; the cell-wide observation set is published
       * once a second RNTI has seen the same mask, so RNTIs k >= 2 start already pruned. k0 = the truth's (the oracle
       * measures on the slot of the job that carried the DM-RS). Qm oracle: two-sighting rule, below. */
      uint8_t qm_tables = 0; int qm_obs = 0;
      auto do_observe = [&]() {
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.dmrs_mask == decoy_mask && h.tda_start + h.tda_length == decoy_end && h.k0 == T.k0;
        });
      };
      /* cell-wide pre-pruning is the consensus of >= 2 earlier RNTIs whose oracles were ok: always the TRUTH's observation */
      auto do_observe_cellwide = [&]() {
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.dmrs_mask == T.dmrs_mask && h.tda_start + h.tda_length == T.tda_start + T.tda_length && h.k0 == T.k0;
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
        if (cfg.oracle && k >= 2 && n_ok_prev >= 2) do_observe_cellwide();
      } else {
        /* Reversible pruning: the DESTRUCTIVE oracle pre-prune first (it compacts any mask), then the masks (index-based, applied
         * to the final catalogue; the sim never extends the catalogue, so no new entries appear after this point). */
        if (cfg.oracle && k >= 2 && n_ok_prev >= 2) do_observe_cellwide();
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
      int winner = -1;
      long g = 0;
      std::uniform_real_distribution<double> uf(0, 1);
      bool trap_active = false; /* per-grant HARQ-trap draw (frng) */
      auto is_k0_neighbour = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return (h.k0 == T.k0 + 1 || h.k0 == T.k0 - 1) && h.mcs_table == T.mcs_table && h.tda_start == T.tda_start
               && h.tda_length == T.tda_length && h.dmrs_add_pos == T.dmrs_add_pos && h.dmrs_max_len == T.dmrs_max_len
               && h.dmrs_mask == T.dmrs_mask && h.mapping_type == T.mapping_type;
      };
      auto base_pass = [&](int h, const Grant &gr, bool truth_pass) {
        if (is_truth(st->hyp[h])) return truth_pass;
        if (is_twin_h(st->hyp[h])) return gr.exercised ? false : truth_pass;
        return false;
      };
      auto full_pass = [&](int h, const Grant &gr, bool truth_pass) {
        const bool p = base_pass(h, gr, truth_pass);
        if (trap_active && is_k0_neighbour(st->hyp[h])) { rec.harq_trap_passes++; return true; }
        if (!p && cfg.crc_false > 0 && uf(frng) < cfg.crc_false) { rec.false_passes++; return true; }
        return p;
      };
      int out_main_idx = -1;
      /* Class of the decoded hypothesis d on this grant: alive j with identical geometry/DM-RS/mapping and either the same
       * MCS table or a grant that does not exercise the table (then the computation is identical). */
      auto feed_equiv_main = [&](bool pass, const Grant &gr) {
        const int d = out_main_idx;
        int cls[NR_PDSCH_SWEEP_MAX_HYP];
        int nc = 0;
        cls[nc++] = d;
        const nr_pdsch_cfg_hypothesis_t &hd = st->hyp[d];
        for (int j = 0; j < st->n_hyp; j++) {
          const nr_pdsch_cfg_hypothesis_t &hj = st->hyp[j];
          if (j != d && hj.tda_start == hd.tda_start && hj.tda_length == hd.tda_length && hj.k0 == hd.k0
              && hj.dmrs_add_pos == hd.dmrs_add_pos && hj.dmrs_max_len == hd.dmrs_max_len && hj.dmrs_mask == hd.dmrs_mask
              && hj.mapping_type == hd.mapping_type && (hj.mcs_table == hd.mcs_table || !gr.exercised))
            cls[nc++] = j;
        }
        return nr_pdsch_config_sweep_feed_equiv(st.get(), cls, nc, pass, gr.new_tx);
      };
      for (; g < cap && winner < 0;) {
        g++;
        Grant gr;
        gr.snr = cfg.mu + cfg.fade * nd(crng);
        gr.rank = ud(crng) < cfg.rank2_frac ? 2 : 1;
        gr.mcs = (int)(ud(crng) * 28) % 28;
        gr.exercised = ud(crng) < cfg.table_exercise;
        gr.new_tx = ud(crng) < 0.75;
        gr.snr_est_noise = (float)(cfg.snr_est_sigma * nd(crng));
        const bool truth_pass = gr.rank <= cfg.n_rx && gr.snr >= nr_td_required_snr_db(gr.mcs, T.mcs_table);
        if (cfg.gate) {
          nr_td_grant_view_t v = {gr.rank, gr.mcs, 2}; /* table 2 = lowest requirement = most permissive */
          nr_td_rx_view_t rx = {cfg.n_rx, (float)gr.snr + gr.snr_est_noise, (int)std::min<long>(g, 1000000), 6.0f};
          const nr_td_gate_t gt = nr_td_grant_gate(&v, &rx);
          if (gt == NR_TD_GATED_PHYSICAL) { rec.gated_phys++; continue; }
          if (gt == NR_TD_GATED_CHANNEL_QUALITY) { rec.gated_chan++; continue; }
        }
        trap_active = cfg.harq_trap > 0 && uf(frng) < cfg.harq_trap;
        int idx[NR_TD_MAX_K];
        nr_pdsch_cfg_hypothesis_t hy[NR_TD_MAX_K];
        const int n = nr_pdsch_config_sweep_next_k(st.get(), cfg.K, idx, hy); /* K=1 == next() (Task 4 bit-identity) */
        if (n < 1) break;
        nr_td_outcome_t out[NR_TD_MAX_K];
        out_main_idx = idx[0];
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
        if (cfg.equiv && cfg.K > 1) {
          /* Lever E: the main decode credits its grant-equivalence class; probe outcomes keep feed_k (probes unchanged). */
          winner = feed_equiv_main(out[0].result == NR_TD_PASS, gr);
          if (n > 1) {
            const int w = nr_pdsch_config_sweep_feed_k(st.get(), out + 1, n - 1);
            if (w >= 0) winner = w;
          }
        } else if (cfg.equiv) {
          winner = feed_equiv_main(out[0].result == NR_TD_PASS, gr);
        } else {
          winner = nr_pdsch_config_sweep_feed_k(st.get(), out, n);
        }
        if (fb2 && winner < 0 && !st->fail_open && nr_pdsch_config_sweep_n_active(st.get()) < st->n_hyp
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
          if (cfg.oracle && decoded && winner < 0) { do_observe(); find_truth(); }
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
        R.fail_opens += rec.fail_open; R.active_start_sum += rec.active_start;
      }
      R.total_grants += g; R.wrong += rec.wrong; R.undecidable += rec.undecidable; R.acquisitions_rntis++;
      R.n_full += rec.n_full; R.n_probe += rec.n_probe; R.gated_phys += rec.gated_phys; R.gated_chan += rec.gated_chan;
      R.promotions += rec.promotions; R.withdrawals += rec.withdrawals;
      n_ok_prev += ostate == 0;
      R.oracle_miss_rntis += ostate == 1; R.oracle_wrong_rntis += ostate == 2;
      R.harq_trap_passes += rec.harq_trap_passes; R.false_passes += rec.false_passes;
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
    else if (f == "--inject-wrong-field") c.inject_wrong_field = atoi(v);
    else if (f == "--fo-alpha") c.fo_alpha = atof(v);
    else if (f == "--fo-pmin") c.fo_pmin = atof(v);
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
  printf("}}\n");
  return 0;
}
#endif
