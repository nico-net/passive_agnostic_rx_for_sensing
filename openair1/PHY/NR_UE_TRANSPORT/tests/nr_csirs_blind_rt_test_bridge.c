/* Compile the real runtime export in C (its PHY headers contain C VLAs). Including the translation
 * unit lets this fixture seed its private banks without adding test hooks to the production API. */
#include "../nr_csirs_blind_rt.c"
#include "common/config/config_load_configmodule.h"
extern configmodule_interface_t *uniqCfg;

enum { TEST_FFT = 128, TEST_RE = TEST_FFT * NR_SYMBOLS_PER_SLOT };
static c16_t test_samples[TEST_RE];
static int test_fep_enabled, test_fep_mask;
static int test_maintenance_control;
static double test_union_score;
static int test_two_resources;

/* Deliberately invalid setup and controlled population are fixture inputs, never score mocks. */
void nr_csirs_blind_rt_test_maintenance_control(int control) { test_maintenance_control = control; }
double nr_csirs_blind_rt_test_union_score(void) { return test_union_score; }
void nr_csirs_blind_rt_test_status_next(void) { g_slots = 19999; }
void nr_csirs_blind_rt_test_two_resources(void)
{
  test_two_resources = 1;
  g_st.n = g_zp.n = 2;
  g_st.cand[1] = g_zp.cand[1] = g_st.cand[0];
  g_st.cand[1].symb_l0 = g_zp.cand[1].symb_l0 = 8;
}
int nr_csirs_blind_rt_test_bank_index(int k) { return k < g_zp.n_conf ? g_zp.conf_idx[k] : -1; }

/* Deterministic FFT boundary: copy samples only for symbols actually requested by runtime. */
int nr_slot_fep_ant(PHY_VARS_NR_UE *ue, const NR_DL_FRAME_PARMS *frame_parms,
                    unsigned int slot, unsigned int symbol, unsigned int ant,
                    c16_t rxdataF[][frame_parms->samples_per_slot_wCP], enum nr_Link link,
                    uint32_t sample_offset, c16_t **rxdata)
{
  if (!test_fep_enabled || symbol >= NR_SYMBOLS_PER_SLOT || ant != 0)
    abort();
  memcpy(&rxdataF[0][symbol * TEST_FFT], &test_samples[symbol * TEST_FFT], TEST_FFT * sizeof(c16_t));
  test_fep_mask |= 1 << symbol;
  return 0;
}

/* Exercise the REAL slot path: generated per-port mapping, sample scores, confirmation and export.
 * No stored confirmation or measured score is seeded. holes/empty select l0 and l0+1 samples. */
int nr_csirs_blind_rt_test_slot_pattern(int row, uint32_t slot, int holes, int empty,
                                      int extra_holes, int reset, int *fep_mask)
{
  static PHY_VARS_NR_UE ue;
  static c16_t rx[1][TEST_RE];
  static int initialized;
  if (!initialized) {
    char name[] = "csirs-runtime-test";
    char *argv[] = {name, NULL};
    uniqCfg = load_configmodule(1, argv, CONFIG_ENABLECMDLINEONLY);
    logInit();
    set_glog(OAILOG_ERR);
    nr_generate_modulation_table();
    initialized = 1;
  }
  if (reset) {
    memset(&ue, 0, sizeof(ue));
    ue.frame_parms = (NR_DL_FRAME_PARMS){.N_RB_DL = 4, .Nid_cell = 17,
        .ofdm_symbol_size = TEST_FFT, .first_carrier_offset = TEST_FFT - 24,
        .symbols_per_slot = NR_SYMBOLS_PER_SLOT, .slots_per_frame = 20,
        .numerology_index = 1, .nb_antennas_rx = 1, .samples_per_slot_wCP = TEST_RE};
    memset(&g_st, 0, sizeof(g_st));
    memset(&g_zp, 0, sizeof(g_zp));
    g_st.confirmed = g_st.pinned = g_zp.confirmed = g_zp.pinned = -1;
    g_st.n = g_zp.n = 1;
    g_st.cand[0] = g_zp.cand[0] = (nr_csirs_candidate_t){
        .row = row, .nr_of_rbs = 4, .freq_domain = 2, .symb_l0 = 7,
        .cdm_type = row >= 3, .freq_density = 2, .scramb_id = 17};
    g_on = g_armed = 1;
    g_rank = g_wide = g_ids = g_slotsweep = 0;
    g_conf_logged = g_null_n = g_null_w = g_zp_null_n = g_zp_null_w = 0;
    memset(g_zp_logged, 0, sizeof(g_zp_logged));
    memset(g_zp_events, 0, sizeof(g_zp_events));
    memset(g_zp_maint, 0, sizeof(g_zp_maint));
    g_slots = 0;
    memset(g_zp_geometry_veto, 0, sizeof(g_zp_geometry_veto));
    g_zp_geometry_veto_total = 0;
    test_maintenance_control = 0;
    test_two_resources = 0;
  }
  c16_t refs[4][TEST_RE] = {0};
  c16_t *planes[] = {refs[0], refs[1], refs[2], refs[3]};
  const nr_csirs_candidate_t *c = &g_st.cand[0];
  const csi_mapping_parms_t mapping = get_csi_mapping_parms(c->row, c->freq_domain, c->symb_l0, c->symb_l1);
  nr_generate_csi_rs(&ue.frame_parms, &mapping, AMP, slot % 20, c->freq_density, 0, 4,
                     c->symb_l0, c->symb_l1, c->row, c->scramb_id, 0, c->cdm_type, planes);
  memset(test_samples, 0, sizeof(test_samples));
  for (int symbol = 7; symbol <= 8; symbol++) {
    for (int k = 0; k < 48; k++) {
      const int pos = symbol * TEST_FFT + k;
      int on = 0;
      for (int p = 0; p < nr_csirs_blind_row_ports(row); p++)
        on |= refs[p][pos].r != 0 || refs[p][pos].i != 0;
      if (test_two_resources && symbol == 8)
        on = refs[0][pos - TEST_FFT].r != 0 || refs[0][pos - TEST_FFT].i != 0;
      const int bit = 1 << (symbol - 7);
      /* Additional structural silence independent of the tested geometry: mode 1 is the
       * quiet half of a comb; mode 2 is one extra quiet tone (k=7) in each RB. Both include
       * the candidate's k=1 hole only when `holes` requests it. */
      const int structural = (holes & bit)
          && ((extra_holes == 1 && (k & 1)) || (extra_holes == 2 && k % 12 == 7));
      const int16_t amp = (empty & bit) || (extra_holes == 3 && (refs[0][pos].r || refs[0][pos].i))
          ? 0 : (((holes & bit) && on) || structural) ? 2 : 700;
      const int shifted = symbol * TEST_FFT + (k + ue.frame_parms.first_carrier_offset) % TEST_FFT;
      test_samples[shifted] = (c16_t){(k & 1) ? amp : -amp, (k & 2) ? amp : -amp};
    }
  }
  memset(rx, 0, sizeof(rx));
  test_fep_mask = 0;
  test_fep_enabled = 1;
  const int16_t *union_refs[] = {(int16_t *)&refs[0][7 * TEST_FFT], (int16_t *)&refs[1][7 * TEST_FFT],
                               (int16_t *)&refs[2][7 * TEST_FFT], (int16_t *)&refs[3][7 * TEST_FFT]};
  test_union_score = nr_csirs_blind_zero_score_ports_shift((int16_t *)&test_samples[7 * TEST_FFT],
      union_refs, nr_csirs_blind_row_ports(row), TEST_FFT, ue.frame_parms.first_carrier_offset);
  if (test_maintenance_control == 1)
    g_zp_null_n = 0;
  if (test_maintenance_control == 2 || test_maintenance_control == 3) {
    g_zp_null_n = 64;
    for (int i = 0; i < 64; i++)
      g_zp_null[i] = test_maintenance_control == 2 ? 0.9 : 0.0;
  }
  const uint8_t saved_symbol = g_st.cand[0].symb_l0;
  if (test_maintenance_control == 4)
    g_st.cand[0].symb_l0 = 13; // row5 must reject l0+1 beyond the slot before generating
  nr_csirs_blind_rt_slot(&ue, slot % 20, slot, rx);
  g_st.cand[0].symb_l0 = saved_symbol;
  /* Supply discovery observations to both test resources independent of rotation order.
   * Confirmed maintenance still runs only via the real predicted-occurrence dispatcher. */
  if (test_two_resources && !nr_csirs_blind_is_confirmed(&g_zp, 1))
    score_candidate(&ue, slot % 20, slot, rx, 1, false);
  test_fep_enabled = 0;
  *fep_mask = test_fep_mask;
  fapi_nr_dl_config_csirs_pdu_rel15_t out[2];
  const int n = nr_csirs_blind_rt_rate_match_all(slot, out, 2);
  for (int i = 0; i < n; i++)
    if (out[i].csi_type != 2 || out[i].row != row || (out[i].symb_l0 != 7 && !(test_two_resources && out[i].symb_l0 == 8)))
      return -1;
  return n;
}

int nr_csirs_blind_rt_test_slot(int row, uint32_t slot, int holes, int empty, int reset, int *fep_mask)
{
  return nr_csirs_blind_rt_test_slot_pattern(row, slot, holes, empty, 0, reset, fep_mask);
}

void nr_csirs_blind_rt_test_logging(int enabled)
{
  set_glog(enabled ? OAILOG_INFO : OAILOG_ERR);
}

int nr_csirs_blind_rt_test_future_export(uint32_t slot)
{
  fapi_nr_dl_config_csirs_pdu_rel15_t out[2];
  return nr_csirs_blind_rt_rate_match_all(slot, out, 2);
}

int nr_csirs_blind_rt_test_nzp_confirmed(void)
{
  return g_st.n_conf;
}

int nr_csirs_blind_rt_test_export(int rank, int banks, int types[2], int *untouched)
{
  nr_csirs_blind_state_t *states[] = {&g_st, &g_zp};
  for (int b = 0; b < 2; b++) {
    memset(states[b], 0, sizeof(*states[b]));
    states[b]->n = 1;
    states[b]->cand[0] = (nr_csirs_candidate_t){
        .row = 1, .nr_of_rbs = 51, .freq_domain = 1, .symb_l0 = 4,
        .freq_density = 3, .scramb_id = 1};
    states[b]->n_conf = (banks >> b) & 1;
    states[b]->conf_period[0] = 40;
    states[b]->conf_n_off[0] = 1;
    states[b]->conf_off[0][0] = 2;
  }
  g_on = g_armed = 1;
  g_rank = rank;
  fapi_nr_dl_config_csirs_pdu_rel15_t out[2], before[2];
  memset(out, 0xa5, sizeof(out));
  memcpy(before, out, sizeof(out));
  const int n = nr_csirs_blind_rt_rate_match_all(42, out, 2);
  *untouched = memcmp(out, before, sizeof(out)) == 0;
  for (int i = 0; i < n; i++)
    types[i] = out[i].csi_type;
  return n;
}

/* Feed measured scores into the real ZP confirmation/export path, without seeding a confirmation. */
int nr_csirs_blind_rt_test_zp_feed(uint32_t slot, double score, int reset)
{
  if (reset) {
    memset(&g_st, 0, sizeof(g_st));
    memset(&g_zp, 0, sizeof(g_zp));
    g_st.confirmed = g_st.pinned = g_zp.confirmed = g_zp.pinned = -1;
    g_zp.n = 1;
    g_zp.cand[0] = (nr_csirs_candidate_t){
        .row = 2, .nr_of_rbs = 4, .freq_domain = 1, .symb_l0 = 7,
        .freq_density = 2, .scramb_id = 17};
    g_on = g_armed = 1;
    g_rank = 0;
  }
  nr_csirs_blind_zp_feed(&g_zp, 0, slot, score, 0.02);
  fapi_nr_dl_config_csirs_pdu_rel15_t out[2];
  return nr_csirs_blind_rt_rate_match_all(slot, out, 2);
}
