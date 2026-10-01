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
  double mu, fade, snr_est_sigma, rank2_frac, grants_per_s, probe_inconclusive, table_exercise, cap_s;
  float w_sib1, w_default, w_obs, w_field, w_probe;
  bool check_correlation;
  static SimCfg defaults()
  {
    SimCfg c;
    c.acq = 100; c.seed = 1; c.catalog_tda = 4; c.n_rx = 4; c.K = 1; c.sib1 = 0; c.fieldbook = 0; c.gate = 0; c.p2 = 0;
    c.twins = 0; c.rntis_per_acq = 4;
    c.mu = 15; c.fade = 6; c.snr_est_sigma = 2; c.rank2_frac = 0.3; c.grants_per_s = 200;
    c.probe_inconclusive = 0.1; c.table_exercise = 0.9; c.cap_s = 3600; /* grant cap per RNTI = cap_s * grants_per_s (K=1 baseline needs up to ~1500 s) */
    c.w_sib1 = c.w_default = c.w_obs = c.w_field = c.w_probe = 0;
    c.check_correlation = false;
    return c;
  }
};
struct RntiRec {
  int acq, rnti_rank;
  long grants;
  double seconds;
  bool winner_ok, wrong, undecidable;
  long n_full, n_probe, gated_phys, gated_chan, promotions, withdrawals;
};
struct SimResult {
  long total_grants = 0, wrong = 0, undecidable = 0, acquisitions_rntis = 0, correlation_violations = 0;
  long n_full = 0, n_probe = 0, gated_phys = 0, gated_chan = 0, promotions = 0, withdrawals = 0;
  long twins_min = -1, twins_sum = 0;
  double median_s = 0, p95_s = 0;
  std::vector<RntiRec> recs;
};

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
  nr_pdsch_config_sweep_init(tmpl.get(), cfg.catalog_tda);
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
      if (cfg.w_default > 0)
        si.dmrs_typeA_pos = 2;
      if (cfg.sib1) {
        si.n_sib1 = 4;
        si.sib1[0] = {T.tda_start, T.tda_length, T.mapping_type, T.k0};
        for (int j = 1; j < 4; j++) {
          uint8_t S = (uint8_t)(arng() % 4), L = (uint8_t)(3 + arng() % 11);
          if (S + L > 14) L = (uint8_t)(14 - S);
          si.sib1[j] = {S, L, 0, (uint8_t)(arng() % 2)};
        }
      }
      if (cfg.w_obs > 0)
        si.obs_dmrs_mask = T.dmrs_mask; /* DM-RS oracle reliable in rfsim */
      if (cfg.fieldbook)
        nr_td_fieldbook_fill_side_info(&fb, &si);
      st->side = &si;
      st->p2 = cfg.p2 != 0;
      const uint16_t rnti = (uint16_t)(0x4000 + k);

      RntiRec rec{};
      rec.acq = a; rec.rnti_rank = k;
      bool distinguished = false;
      int winner = -1;
      long g = 0;
      auto full_pass = [&](int h, const Grant &gr, bool truth_pass) {
        if (h == truth) return truth_pass;
        if (is_twin[h]) return gr.exercised ? false : truth_pass;
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
              if ((idx[i] == truth && !truth_pass && p) || (is_twin[idx[i]] && !gr.exercised && p != truth_pass))
                R.correlation_violations++;
            }
          }
        }
        if (gr.exercised && truth_pass)
          distinguished = true;
        winner = nr_pdsch_config_sweep_feed_k(st.get(), out, n);
      }
      rec.grants = g;
      rec.seconds = (double)g / cfg.grants_per_s;
      if (winner < 0) {
        rec.undecidable = true;
      } else {
        rec.wrong = winner != truth && !(is_twin[winner] && !distinguished);
        rec.winner_ok = !rec.wrong;
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
      secs.push_back(rec.seconds);
      R.recs.push_back(rec);
    }
  }
  if (!secs.empty()) {
    std::sort(secs.begin(), secs.end());
    R.median_s = secs[secs.size() / 2];
    R.p95_s = secs[std::min(secs.size() - 1, (size_t)std::ceil(0.95 * secs.size()) - 1)];
  }
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
    else { fprintf(stderr, "unknown flag %s\n", f.c_str()); return 2; }
  }
  const SimResult r = run_sim(c);
  for (const RntiRec &x : r.recs)
    printf("{\"acq\":%d,\"rnti_rank\":%d,\"grants\":%ld,\"seconds\":%.4f,\"winner_ok\":%s,\"wrong\":%d,\"undecidable\":%d,"
           "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld}\n",
           x.acq, x.rnti_rank, x.grants, x.seconds, x.winner_ok ? "true" : "false", (int)x.wrong, (int)x.undecidable, x.n_full,
           x.n_probe, x.gated_phys, x.gated_chan, x.promotions, x.withdrawals);
  printf("{\"summary\":{\"acq\":%d,\"rntis\":%ld,\"median_s\":%.3f,\"p95_s\":%.3f,\"wrong\":%ld,\"undecidable\":%ld,"
         "\"n_full\":%ld,\"n_probe\":%ld,\"gated_phys\":%ld,\"gated_chan\":%ld,\"promotions\":%ld,\"withdrawals\":%ld,"
         "\"twins_requested\":%d,\"twins_min\":%ld,\"cap_s\":%.0f,\"seed\":%d,\"n_rx\":%d,\"K\":%d}}\n",
         c.acq, r.acquisitions_rntis, r.median_s, r.p95_s, r.wrong, r.undecidable, r.n_full, r.n_probe, r.gated_phys,
         r.gated_chan, r.promotions, r.withdrawals, c.twins, r.twins_min, c.cap_s, c.seed, c.n_rx, c.K);
  return 0;
}
#endif
