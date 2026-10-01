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
  int prior;            /* 1 (default) = today's cell-wide/RNTI prior pruning; forced OFF when fieldbook=1 (plan R2 rule) */
  int oracle;           /* 1 = today's runtime (DM-RS/last-symbol/k0 + Qm oracles prune the catalogue), 0 = blind arm */
  int dmrs_typea_pos;   /* ASN.1 enum: 0 = pos2 (rfsim gNB default, 106 PRB cell), 1 = pos3 */
  double mu, fade, snr_est_sigma, rank2_frac, grants_per_s, probe_inconclusive, table_exercise, cap_s;
  float w_sib1, w_default, w_obs, w_field, w_probe;
  bool check_correlation;
  static SimCfg defaults()
  {
    SimCfg c;
    c.acq = 100; c.seed = 1; c.catalog_tda = 4; c.n_rx = 4; c.K = 1; c.sib1 = 0; c.fieldbook = 0; c.gate = 0; c.p2 = 0;
    c.twins = 0; c.rntis_per_acq = 4; c.oracle = 1; c.prior = 1; c.dmrs_typea_pos = 0;
    c.mu = 15; c.fade = 6; c.snr_est_sigma = 2; c.rank2_frac = 0.3; c.grants_per_s = 200;
    c.probe_inconclusive = 0.1; c.table_exercise = 0.9; c.cap_s = 3600;
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
  long n_full, n_probe, gated_phys, gated_chan, promotions, withdrawals;
};
struct SimResult {
  long total_grants = 0, wrong = 0, undecidable = 0, acquisitions_rntis = 0, correlation_violations = 0;
  long n_full = 0, n_probe = 0, gated_phys = 0, gated_chan = 0, promotions = 0, withdrawals = 0;
  long twins_min = -1, twins_sum = 0;
  double median_s = 0, p95_s = 0, mean_s = 0, mean_grants = 0;
  long n_decided = 0;
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

/* Oracle pruning, local re-implementation of the runtime's commit: keep entries admitted by `keep`, then (only if
 * something was removed) discard evidence exactly as prune_commit() does. Config fields side/p2 are untouched. */
template <class F> static void sim_prune(nr_pdsch_config_sweep_state_t *st, F keep)
{
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (keep(st->hyp[i])) st->hyp[n++] = st->hyp[i];
  if (n <= 0 || n == st->n_hyp) return; /* nothing matched / nothing removed: state untouched (same as the runtime) */
  st->n_hyp = n;
  memset(st->trials, 0, sizeof(st->trials)); memset(st->ok, 0, sizeof(st->ok));
  memset(st->probe_pass, 0, sizeof(st->probe_pass)); memset(st->probe_fail, 0, sizeof(st->probe_fail));
  memset(st->probe_inconclusive, 0, sizeof(st->probe_inconclusive));
  for (int i = 0; i < n; i++) st->order[i] = i;
  st->cursor = 0; st->winner = -1;
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
  auto tmpl = std::make_unique<nr_pdsch_config_sweep_state_t>();
  /* Production catalogue: init_legal with the production legality (type A only, masks deduplicated). */
  nr_pdsch_config_sweep_init_legal(tmpl.get(), cfg.catalog_tda, cfg.dmrs_typea_pos, sim_legality);
  auto st = std::make_unique<nr_pdsch_config_sweep_state_t>();
  const int n_hyp = tmpl->n_hyp;
  const long cap = (long)(cfg.cap_s * cfg.grants_per_s);
  std::vector<double> secs;
  for (int a = 0; a < cfg.acq; a++) {
    std::mt19937_64 arng(mix(cfg.seed, a, 0xACC));
    const int truth = (int)(arng() % (uint64_t)n_hyp);
    const nr_pdsch_cfg_hypothesis_t &T = tmpl->hyp[truth];
    std::vector<char> is_twin(n_hyp, 0);
    int ntw = 0;
    for (int i = 0; i < n_hyp && ntw < cfg.twins; i++) {
      const nr_pdsch_cfg_hypothesis_t &h = tmpl->hyp[i];
      if (i != truth && h.mcs_table != T.mcs_table && h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.k0 == T.k0
          && h.dmrs_add_pos == T.dmrs_add_pos && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask
          && h.mapping_type == T.mapping_type) {
        is_twin[i] = 1;
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
    const bool use_prior = cfg.prior && !cfg.fieldbook;
    struct Prior { uint8_t tbl, add, len, map; };
    std::vector<Prior> rnti_priors;
    bool gprior_valid = false;
    Prior gprior{};
    nr_td_fieldbook_t fb;
    nr_td_fieldbook_init(&fb, 2, 2);
    for (int k = 0; k < cfg.rntis_per_acq; k++) {
      std::mt19937_64 crng(mix(cfg.seed, a, 0x100 + k)); /* channel: fixed draws per grant, so arms are paired */
      std::mt19937_64 prng(mix(cfg.seed, a, 0x200 + k)); /* probe inconclusive draws */
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
      if (cfg.w_obs > 0) { /* ordering inputs (I2): the DM-RS oracle is reliable in rfsim; qm/mcs filled from the first measurable grant */
        si.obs_dmrs_mask = T.dmrs_mask;
        si.obs_last_symbol = T.tda_start + T.tda_length - 1;
      }
      if (cfg.fieldbook)
        nr_td_fieldbook_fill_side_info(&fb, &si);
      st->side = &si;
      st->p2 = cfg.p2 != 0;
      const uint16_t rnti = (uint16_t)(0x4000 + k);

      RntiRec rec{};
      rec.acq = a; rec.rnti_rank = k; rec.truth_table = T.mcs_table;
      /* Oracles (today's runtime): the DM-RS mask / last symbol / k0 observation (nr_pdsch_config_sweep_observe ->
       * prune_to_observed) fires on the first decoded grant of an RNTI; the cell-wide observation set is published
       * once a second RNTI has seen the same mask, so RNTIs k >= 2 start already pruned. k0 = the truth's (the oracle
       * measures on the slot of the job that carried the DM-RS). Qm oracle: two-sighting rule, below. */
      bool obs_done = false;
      uint8_t qm_tables = 0; int qm_obs = 0;
      auto do_observe = [&]() {
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.dmrs_mask == T.dmrs_mask && h.tda_start + h.tda_length == T.tda_start + T.tda_length && h.k0 == T.k0;
        });
      };
      if (use_prior && gprior_valid)
        sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) {
          return h.mcs_table == gprior.tbl && (h.mapping_type != gprior.map || (h.dmrs_add_pos == gprior.add && h.dmrs_max_len == gprior.len));
        });
      if (cfg.oracle && k >= 2) do_observe();
      bool distinguished = false;
      int winner = -1;
      long g = 0;
      /* Hypotheses are identified by CONTENT: oracle pruning re-indexes the state, so catalogue indices drift. */
      auto same_but_table = [&](const nr_pdsch_cfg_hypothesis_t &h) {
        return h.tda_start == T.tda_start && h.tda_length == T.tda_length && h.k0 == T.k0 && h.dmrs_add_pos == T.dmrs_add_pos
               && h.dmrs_max_len == T.dmrs_max_len && h.dmrs_mask == T.dmrs_mask && h.mapping_type == T.mapping_type;
      };
      auto is_truth = [&](const nr_pdsch_cfg_hypothesis_t &h) { return same_but_table(h) && h.mcs_table == T.mcs_table; };
      auto is_twin_h = [&](const nr_pdsch_cfg_hypothesis_t &h) { return same_but_table(h) && h.mcs_table != T.mcs_table; };
      auto full_pass = [&](int h, const Grant &gr, bool truth_pass) {
        if (is_truth(st->hyp[h])) return truth_pass;
        if (is_twin_h(st->hyp[h])) return gr.exercised ? false : truth_pass;
        return false;
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
        int idx[NR_TD_MAX_K];
        nr_pdsch_cfg_hypothesis_t hy[NR_TD_MAX_K];
        const int n = nr_pdsch_config_sweep_next_k(st.get(), cfg.K, idx, hy); /* K=1 == next() (Task 4 bit-identity) */
        if (n < 1) break;
        nr_td_outcome_t out[NR_TD_MAX_K];
        for (int i = 0; i < n; i++) {
          out[i].hyp = idx[i];
          out[i].p2_admissible = false;
          const bool p = full_pass(idx[i], gr, truth_pass);
          if (i == 0) {
            out[i].kind = NR_TD_FULL_TB;
            out[i].result = p ? NR_TD_PASS : NR_TD_FAIL;
            rec.n_full++;
          } else {
            out[i].kind = NR_TD_CB_PROBE;
            const bool inc = ud(prng) < cfg.probe_inconclusive;
            out[i].result = inc ? NR_TD_INCONCLUSIVE : (p ? NR_TD_PASS : NR_TD_FAIL);
            out[i].p2_admissible = gr.new_tx && !inc;
            rec.n_probe++;
            if (cfg.check_correlation && !inc) {
              if ((is_truth(st->hyp[idx[i]]) && !truth_pass && p) || (is_twin_h(st->hyp[idx[i]]) && !gr.exercised && p != truth_pass))
                R.correlation_violations++;
            }
          }
        }
        if (gr.exercised && truth_pass)
          distinguished = true;
        winner = nr_pdsch_config_sweep_feed_k(st.get(), out, n);
        if (cfg.oracle || cfg.w_obs > 0) {
          const bool decoded = gr.rank <= cfg.n_rx; /* the layout is decodable at all -> the oracles can measure */
          if (cfg.oracle && decoded && !obs_done && winner < 0) do_observe();
          if (decoded && truth_pass) { /* constellation measurable only at a decodable SNR */
            const int qm = nr_pdsch_qm_of_mcs((uint8_t)gr.mcs, T.mcs_table);
            const uint8_t mask = sim_qm_table_mask(gr.mcs, qm);
            if (cfg.w_obs > 0 && si.obs_qm < 0 && mask != 0 && mask != 0x7) { si.obs_qm = qm; si.obs_mcs = gr.mcs; }
            if (cfg.oracle && winner < 0 && mask != 0 && mask != 0x7) {
              const uint8_t inter = qm_obs ? (uint8_t)(qm_tables & mask) : mask;
              if (inter == 0) { qm_obs = 0; qm_tables = 0; }
              else {
                qm_tables = inter; qm_obs++;
                if (qm_obs >= 2) sim_prune(st.get(), [&](const nr_pdsch_cfg_hypothesis_t &h) { return (inter >> h.mcs_table) & 1; });
              }
            }
          }
        }
      }
      rec.grants = g;
      rec.seconds = (double)g / cfg.grants_per_s;
      if (winner < 0) {
        rec.undecidable = true;
      } else {
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
          nr_td_fieldbook_converged(&fb, rnti, &st->hyp[winner], (uint64_t)g);
          for (int f = 0; f < NR_TD_F_COUNT; f++) {
            if (before[f] != -1 && fb.f[f].value != before[f]) rec.withdrawals++;
            if (fb.f[f].value != -1 && fb.f[f].value != before[f]) rec.promotions++;
          }
        }
      }
      R.total_grants += g; R.wrong += rec.wrong; R.undecidable += rec.undecidable; R.acquisitions_rntis++;
      R.n_full += rec.n_full; R.n_probe += rec.n_probe; R.gated_phys += rec.gated_phys; R.gated_chan += rec.gated_chan;
      R.promotions += rec.promotions; R.withdrawals += rec.withdrawals;
      if (!rec.undecidable) {
        secs.push_back(rec.seconds);
        R.mean_grants += (double)g; R.n_decided++;
        SimResult::TableStat &ts = R.by_table[rec.truth_table];
        ts.n++; ts.wrong += rec.wrong; ts.sum_s += rec.seconds; ts.v.push_back(rec.seconds);
      }
      R.recs.push_back(rec);
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
    else if (f == "--rntis-per-acq") c.rntis_per_acq = atoi(v);
    else if (f == "--probe-inconclusive") c.probe_inconclusive = atof(v);
    else if (f == "--table-exercise") c.table_exercise = atof(v);
    else if (f == "--cap-s") c.cap_s = atof(v);
    else if (f == "--oracle") c.oracle = atoi(v);
    else if (f == "--prior") c.prior = atoi(v);
    else if (f == "--dmrs-typea-pos") c.dmrs_typea_pos = atoi(v);
    else { fprintf(stderr, "unknown flag %s\n", f.c_str()); return 2; }
  }
  const SimResult r = run_sim(c);
  for (const RntiRec &x : r.recs)
    printf("{\"acq\":%d,\"rnti_rank\":%d,\"truth_table\":%d,\"grants\":%ld,\"seconds\":%.4f,\"winner_ok\":%s,\"wrong\":%d,\"undecidable\":%d,"
           "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld}\n",
           x.acq, x.rnti_rank, x.truth_table, x.grants, x.seconds, x.winner_ok ? "true" : "false", (int)x.wrong, (int)x.undecidable, x.n_full,
           x.n_probe, x.gated_phys, x.gated_chan, x.promotions, x.withdrawals);
  /* Quantiles/means are over DECIDED RNTIs only; capped (undecidable) RNTIs are censored and counted separately.
   * NB: separation is checked every 16 trials, so seconds move in steps of 16 x n_hyp / grants_per_s: prefer the means. */
  printf("{\"summary\":{\"acq\":%d,\"rntis\":%ld,\"decided\":%ld,\"median_s\":%.3f,\"p95_s\":%.3f,\"mean_s\":%.3f,"
         "\"mean_grants\":%.1f,\"wrong\":%ld,\"undecidable\":%ld,"
         "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld,"
         "\"twins_requested\":%d,\"twins_min\":%ld,\"cap_s\":%.0f,\"seed\":%d,\"n_rx\":%d,\"K\":%d,\"oracle\":%d,\"prior\":%d,\"by_table\":{",
         c.acq, r.acquisitions_rntis, r.n_decided, r.median_s, r.p95_s, r.mean_s, r.mean_grants, r.wrong, r.undecidable,
         r.n_full, r.n_probe, r.gated_phys, r.gated_chan, r.promotions, r.withdrawals, c.twins, r.twins_min, c.cap_s,
         c.seed, c.n_rx, c.K, c.oracle, c.prior && !c.fieldbook);
  for (int t = 0; t < 3; t++) {
    const SimResult::TableStat &ts = r.by_table[t];
    printf("%s\"%d\":{\"n\":%ld,\"median_s\":%.3f,\"mean_s\":%.3f,\"wrong\":%ld}", t ? "," : "", t, ts.n,
           ts.v.empty() ? 0.0 : ts.v[ts.v.size() / 2], ts.n ? ts.sum_s / (double)ts.n : 0.0, ts.wrong);
  }
  printf("}}}\n");
  return 0;
}
#endif
