/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 4, host side of the DL graph pass: see nr_gpu_pass_dl.h and nr_gpu_pass.h. */
#include "nr_gpu_pass_dl.h"
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_pdsch_gpu_exact.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_ldpc.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_pdsch_cpu_route.h"
#include "nr_llr_norm.h"
#include "nr_pdsch_evm_acc.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "common/utils/LOG/log.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

int nr_get_R_ldpc_decoder(int rvidx, int E, int BG, int Z, int *llrLen, int round);
int nr_get_E(uint32_t G, uint8_t C, uint8_t Qm, uint8_t Nl, uint8_t r);

/* where a pass was not taken (counted; the first few are logged) */
enum { FB_OFF, FB_CELL, FB_FO, FB_CHEST, FB_DEMOD, FB_TBPARAM, FB_MODULE, FB_N };
static const char *const fb_name[FB_N] = {"off", "cell", "fo", "chest", "demod", "tbparam", "module"};
static struct {
  nr_gpu_pass_dl_try_fn try_;
  nr_gpu_pass_stats_fn stats;
  _Atomic int on;
  int nbrx, check;
  _Atomic uint64_t fb[FB_N], ev[NR_GPU_PASS_EV_N], runs, shadow_n;
} G;

int nr_gpu_pass_active(void) { return atomic_load_explicit(&G.on, memory_order_relaxed); }

void nr_gpu_pass_note(int ev)
{
  if (ev >= 0 && ev < NR_GPU_PASS_EV_N)
    atomic_fetch_add_explicit(&G.ev[ev], 1, memory_order_relaxed);
}

void nr_gpu_pass_disable(const char *why)
{
  if (atomic_exchange(&G.on, 0))
    LOG_E(PHY, "SENSING: GPUPASS DISABLED: %s -- the old path decodes from now on\n", why);
}

int nr_gpu_pass_shadow_due(void)
{
  if (G.check <= 0)
    return 0;
  const uint64_t n = atomic_fetch_add_explicit(&G.shadow_n, 1, memory_order_relaxed) + 1;
  return n % (uint64_t)G.check == 0;
}

static void census(void)
{
  char buf[512];
  if (G.stats)
    G.stats(buf, sizeof(buf));
  else
    buf[0] = 0;
  LOG_A(PHY,
        "SENSING: %s | host: fallback[off=%lu cell=%lu fo=%lu chest=%lu demod=%lu tbparam=%lu module=%lu] commit=%lu retry_old=%lu zero_tb=%lu lifetime=%lu "
        "shadow=%lu shadow_bad=%lu\n",
        buf, (unsigned long)G.fb[FB_OFF], (unsigned long)G.fb[FB_CELL], (unsigned long)G.fb[FB_FO], (unsigned long)G.fb[FB_CHEST],
        (unsigned long)G.fb[FB_DEMOD], (unsigned long)G.fb[FB_TBPARAM], (unsigned long)G.fb[FB_MODULE], (unsigned long)G.ev[NR_GPU_PASS_EV_COMMIT],
        (unsigned long)G.ev[NR_GPU_PASS_EV_RETRY_OLD], (unsigned long)G.ev[NR_GPU_PASS_EV_ZERO_TB], (unsigned long)G.ev[NR_GPU_PASS_EV_LIFETIME],
        (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW], (unsigned long)G.ev[NR_GPU_PASS_EV_SHADOW_BAD]);
}

static int fb(int why)
{
  const uint64_t n = atomic_fetch_add_explicit(&G.fb[why], 1, memory_order_relaxed) + 1;
  if (n == 1)
    LOG_A(PHY, "SENSING: GPUPASS first fallback: %s\n", fb_name[why]);
  return 1;
}

/* flags whose host paths the pass does not reproduce: any of them set = the pass stays off for the run (logged) */
static const char *incompatible_env(void)
{
  static const char *const nz[] = {"ISAC_PDSCH_EVM", "ISAC_SFO_CORRECT", "ISAC_DMRS_FO_APPLY", "ISAC_CHEST_OCC", "ISAC_RX_IRC", "ISAC_CHEST_TINTERP",
                                   "ISAC_DC_FIX", "ISAC_PDSCH_ANT_GATE", "ISAC_LLR_SCALE", "ISAC_PDSCH_RM_GENIE", "ISAC_PDSCH_LLR_DUMP", "ISAC_RX_BRANCH_FO",
                                   "ISAC_PDSCH_MMSE_AB", "ISAC_MMSE2_DIAG", "ISAC_PDSCH_OWN_PDCCH_RM", "ISAC_TD_TB_CPU_WHILE_ACQ"};
  for (size_t i = 0; i < sizeof(nz) / sizeof(nz[0]); i++) {
    const char *e = getenv(nz[i]);
    if (e && atoi(e) != 0)
      return nz[i];
  }
  const char *e = getenv("ISAC_LLR_NORM");
  (void)e;
  return NULL;
}

static int env_list(const char *name, const char *dflt, int *v, int max)
{
  const char *e = getenv(name);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s", e && *e ? e : dflt);
  int n = 0;
  for (char *s = strtok(buf, ","); s && n < max; s = strtok(NULL, ","))
    v[n++] = atoi(s);
  return n;
}

void nr_gpu_pass_warmup(PHY_VARS_NR_UE *ue)
{
  const char *e = getenv("ISAC_GPU_PASS");
  if (!e || atoi(e) != 1 || !ue)
    return;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const char *bad = incompatible_env();
  if (bad) {
    LOG_W(PHY, "SENSING: GPUPASS requested but %s is set (a host path the pass does not reproduce): off\n", bad);
    return;
  }
  if (fp->ofdm_symbol_size != 4096 || fp->symbols_per_slot != 14 || fp->nb_antennas_rx < 1 || fp->nb_antennas_rx > 4) {
    LOG_W(PHY, "SENSING: GPUPASS needs N = 4096, 14 symbols, <= 4 RX: off\n");
    return;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  nr_gpu_pass_config_fn cfgf = h ? (nr_gpu_pass_config_fn)dlsym(h, "nr_gpu_pass_config") : NULL;
  nr_gpu_pass_init_fn initf = h ? (nr_gpu_pass_init_fn)dlsym(h, "nr_gpu_pass_init") : NULL;
  nr_gpu_pass_warm_fn warmf = h ? (nr_gpu_pass_warm_fn)dlsym(h, "nr_gpu_pass_warm") : NULL;
  nr_gpu_demod_init_fn dinit = h ? (nr_gpu_demod_init_fn)dlsym(h, "nr_pdsch_gpu_demod_init") : NULL;
  nr_gpu_ldpc_init_fn linit = h ? (nr_gpu_ldpc_init_fn)dlsym(h, "nr_pdsch_gpu_ldpc_init") : NULL;
  G.try_ = h ? (nr_gpu_pass_dl_try_fn)dlsym(h, "nr_gpu_pass_dl_try") : NULL;
  G.stats = h ? (nr_gpu_pass_stats_fn)dlsym(h, "nr_gpu_pass_stats") : NULL;
  if (!cfgf || !initf || !warmf || !dinit || !linit || !G.try_ || dinit(fp->nb_antennas_rx) != 0 || linit() != 0) {
    LOG_W(PHY, "SENSING: GPUPASS requested but libpdsch_gpu.so has no usable graph pass: off\n");
    return;
  }
  nr_gpu_pass_cfg_t c = {.nbRx = fp->nb_antennas_rx, .n_rb_dl = fp->N_RB_DL, .delay_table = (const int16_t *)fp->delay_table[0],
                         .dt_stride = NR_MAX_OFDM_SYMBOL_SIZE, .max_delay_comp = MAX_DELAY_COMP};
  if (nr_gpu_model() == NR_GPU_MODEL_DISCRETE) { /* the graphs read the delay table on the device */
    static nr_gpu_buf_t dt;
    const size_t bytes = sizeof(fp->delay_table);
    if (nr_gpu_buf_alloc(&dt, bytes) != 0) {
      LOG_W(PHY, "SENSING: GPUPASS: no device copy of the delay table: off\n");
      return;
    }
    memcpy(dt.host, fp->delay_table, bytes);
    nr_gpu_buf_h2d_async(&dt, 0, bytes, NULL);
    c.delay_table = (const int16_t *)dt.dev;
  }
  if (cfgf(&c) != 0 || initf(0) != 0) {
    LOG_W(PHY, "SENSING: GPUPASS: nr_gpu_pass_config/init failed: off\n");
    return;
  }
  /* the class list: PRB classes x layers x the code block counts that can occur x DM-RS symbol counts, in the order of expected use, up to ISAC_GPU_PASS_CLASSES */
  int nd[4], nnd = env_list("ISAC_GPU_PASS_NDMRS", "3,2", nd, 4);
  const char *ce = getenv("ISAC_GPU_PASS_CLASSES");
  const int budget = ce ? atoi(ce) : 512;
  const int evm = (getenv("ISAC_PDSCH_EVM_GRANT") || getenv("ISAC_PDSCH_SNR_GATE") || (getenv("ISAC_PDSCH_DECODE_DIAG") && atoi(getenv("ISAC_PDSCH_DECODE_DIAG")) != 0)) ? 1 : 0;
  static const int nrbs[] = {273, 217, 136, 96, 48, 24, 12, 6};
  static const int nls[] = {1, 2, 4, 3};
  int made = 0, failed = 0;
  for (int di = 0; di < nnd; di++)
    for (int ni = 0; ni < 8 && made < budget; ni++)
      for (int li = 0; li < 4 && made < budget; li++) {
        const int nl = nls[li];
        if (nl > fp->nb_antennas_rx)
          continue;
        /* the demod variants: QPSK and higher can resolve different MMSE modes for 3 and 4 layers */
        int variants[2][2], nvar = 0;
        for (int qm = 2; qm <= 6; qm += 4) {
          int qmode = 0, align = 0;
          if (nr_pdsch_gpu_demod_pass_variant(ue, nl, qm, &qmode, &align) != 0)
            continue;
          int dup = 0;
          for (int v = 0; v < nvar; v++)
            dup |= variants[v][0] == qmode && variants[v][1] == align;
          if (!dup) {
            variants[nvar][0] = qmode;
            variants[nvar][1] = align;
            nvar++;
          }
        }
        /* code blocks per pass: from 1 to the most the grant can carry (256QAM, rate 0.93) */
        const int cmax = nrbs[ni] * nl * 132 * 8 * 93 / 100 / 8400 + 1;
        for (int ncb = 1; ncb <= 32 && made < budget; ncb <<= 1) {
          if (ncb / 2 >= cmax)
            break;
          for (int v = 0; v < nvar && made < budget; v++) {
            const nr_gpu_pass_shape_t sh = {(uint16_t)nr_gpu_pass_nrb_class(nrbs[ni]), 13, (uint16_t)nl, 6, (uint16_t)nd[di], (uint16_t)ncb, 1};
            if (warmf(&sh, variants[v][0], variants[v][1], evm) == 0)
              made++;
            else
              failed++;
          }
        }
      }
  G.nbrx = fp->nb_antennas_rx;
  const char *ch = getenv("ISAC_GPU_PASS_CHECK");
  G.check = ch ? atoi(ch) : 64;
  atomic_store(&G.on, made > 0);
  LOG_A(PHY, "SENSING: GPUPASS %s: %d shape classes x %d instances captured (EVM branch %d, %d failed), shadow check every %d passes, %s memory model\n",
        made > 0 ? "enabled" : "NOT enabled", made, NR_GPU_PASS_RING, evm, failed, G.check, nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE");
}

int nr_gpu_pass_dl_run(const nr_gpu_pass_dl_in_t *in, nr_gpu_pass_dl_out_t *out)
{
  if (!nr_gpu_pass_active())
    return fb(FB_OFF);
  {
    static _Atomic uint64_t calls;
    const uint64_t c = atomic_fetch_add(&calls, 1) + 1;
    if (c == 100 || (c % 2048) == 0)
      census();
  }
  PHY_VARS_NR_UE *ue = in->ue;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const fapi_nr_dl_cw_info_t *cw = in->cw;
  if (fp->nb_antennas_rx != G.nbrx || fp->ofdm_symbol_size != 4096 || fp->symbols_per_slot != 14 || cw->Nl > fp->nb_antennas_rx)
    return fb(FB_CELL);
  /* the exact FEP's own refusals: a frequency offset in the FEP, an unsynchronised UE, a per-branch offset */
  bool fo = ue->cont_fo_comp != 0 || !ue->is_synchronized || in->fep_fo != 0.0;
  for (int a = 0; a < fp->nb_antennas_rx && !fo; a++)
    fo = nr_ue_get_branch_fo_hz(a) != 0.0;
  if (fo || nr_pdsch_route_cpu())
    return fb(FB_FO);
  static __thread nr_gpu_pass_job_t *jobp = NULL; /* heap: large */
  if (!jobp && !(jobp = (nr_gpu_pass_job_t *)aligned_alloc(64, (sizeof(*jobp) + 63) & ~(size_t)63)))
    return fb(FB_MODULE);
  nr_gpu_pass_job_t *j = jobp;
  memset(j, 0, offsetof(nr_gpu_pass_job_t, dj));
  /* ---- channel estimate inputs */
  int ndmrs = 0, k0 = 0, nrb = 0;
  if (nr_pdsch_gpu_chest_pass_inputs(ue, in->proc, in->dlsch_config, in->freq_alloc, cw->Nl, in->dmrs_full, in->chest_diag, &ndmrs, j->dmrs_sym, j->delta, j->pil,
                                     &k0, &nrb) != 0)
    return fb(FB_CHEST);
  j->nbRx = fp->nb_antennas_rx;
  j->nl = cw->Nl;
  j->ndmrs = ndmrs;
  j->nrb = nrb;
  j->k0 = k0;
  /* ---- demod job (the old GPU demod's own refusals and geometry) */
  NR_UE_DLSCH_t dlsch = {0};
  dlsch.cw_info = *cw;
  dlsch.rnti = in->rnti;
  dlsch.rnti_type = TYPE_C_RNTI_;
  dlsch.active = true;
  dlsch.max_ldpc_iterations = 8;
  NR_DL_UE_HARQ_t harq = {0};
  harq.status = NR_ACTIVE;
  harq.first_rx = 1;
  pdsch_scope_req_t scope = {.copy_chanest_to_scope = false, .copy_rxdataF_to_scope = false, .scope_rxdataF_offset = 0};
  nr_pdsch_gpu_demod_call_t call = {.ue = ue, .proc = in->proc, .dlsch = &dlsch, .freq_alloc = in->freq_alloc, .cfg = in->dlsch_config, .harq = &harq,
                                    .first_symbol = in->first_symbol, .probe_last_sym = -1, .est_size = in->est_size, .buf = in->rx_size_symbol,
                                    .rxdataF = (c16_t *)in->rxdataF, .scope = &scope, .ssb_rm = in->ssb_rm};
  if (nr_pdsch_gpu_demod_pass_job(&call, &j->dj, &j->geo_y, &j->geo_h) != 0)
    return fb(FB_DEMOD);
  if (nr_pdsch_evm_acc.armed && !j->dj.want_evm) /* the EVM would need the CPU copy of the equaliser output */
    return fb(FB_DEMOD);
  uint32_t G_dem = 0;
  for (int m = 0; m < 14; m++)
    if (j->dj.sym_mask >> m & 1)
      G_dem += (uint32_t)j->dj.nre[m] * cw->qamModOrder * cw->Nl;
  /* ---- transport block parameters, as passive_ldpc_decode_core derives them (no HARQ combining: first transmission, d cleared) */
  unsigned C = 0, K = 0, Z = 0, F = 0;
  if (nr_segmentation(NULL, NULL, lenWithCrc(1, cw->TBS), &C, &K, &Z, &F, cw->ldpcBaseGraph) < 0 || C < 1 || C > NR_GPU_PASS_MAX_CB || (Z & 3) || (K & 7) || (K >> 3) > 1056
      || G_dem != in->G || in->G == 0)
    return fb(FB_TBPARAM);
  const int ncb = nr_gpu_pass_ncb_class((int)C);
  const int nrbc = nr_gpu_pass_nrb_class(nrb);
  if (!ncb || !nrbc || in->it_max < 1 || in->it_max > 17)
    return fb(FB_TBPARAM);
  const uint32_t E = (uint32_t)nr_get_E(in->G, (uint8_t)C, cw->qamModOrder, cw->Nl, 0);
  int llrLen = 0;
  uint32_t E2 = E, first = C;
  uint8_t R = (uint8_t)nr_get_R_ldpc_decoder(cw->rv, (int)E, cw->ldpcBaseGraph, (int)Z, &llrLen, 0), R2 = R;
  for (uint32_t r = 1; r < C; r++) {
    const int Er = nr_get_E(in->G, (uint8_t)C, cw->qamModOrder, cw->Nl, (uint8_t)r);
    if ((uint32_t)Er != E) {
      E2 = (uint32_t)Er;
      R2 = (uint8_t)nr_get_R_ldpc_decoder(cw->rv, Er, cw->ldpcBaseGraph, (int)Z, &llrLen, 0);
      first = r;
      break;
    }
  }
  j->A = cw->TBS;
  j->C = C; j->K = K; j->Z = Z; j->F = F; j->E = E; j->E2 = E2; j->first_rE2 = first; j->tbslbrm = in->tbslbrm;
  j->BG = cw->ldpcBaseGraph; j->R = R; j->R2 = R2; j->rv = cw->rv; j->Qm = cw->qamModOrder; j->it_max = (uint8_t)in->it_max;
  /* ---- LLR post: descrambling words (the gold sequence nr_codeword_unscrambling reads) and the K38 normalisation */
  const int words = (int)((in->G + 31) / 32);
  j->scr = gold_cache(((uint32_t)in->rnti << 15) + (0u << 14) + in->dlsch_config->dlDataScramblingId, words);
  j->G = in->G;
  j->norm = in->norm && in->G >= 64;
  j->norm_span = nr_llr_norm_span(in->G, nr_llr_norm_num_cb(cw->TBS, cw->ldpcBaseGraph));
  /* ---- front end */
  c16_t **iq = nr_pdsch_passive_iq_for_slot(in->proc->nr_slot_rx, ue->common_vars.rxdata);
  for (int a = 0; a < fp->nb_antennas_rx; a++)
    j->rxdata[a] = (const int16_t *)iq[a];
  j->ring_len = 2u * fp->samples_per_frame;
  j->ring_offset = get_samples_slot_timestamp(fp, in->proc->nr_slot_rx);
  j->symbol_rot = (const int16_t *)(fp->symbol_rotation[link_type_dl] + (in->proc->nr_slot_rx % fp->slots_per_subframe) * fp->symbols_per_slot);
  j->rxdataF = (int16_t *)in->rxdataF;
  j->rx_stride = in->rx_stride;
  j->copy_mask = 0x3fff;
  /* ---- chest post: nvar normalisation, per-branch substitution, DM-RS FO/SFO sums */
  j->number_symbols = in->dlsch_config->number_symbols;
  j->nvfix = in->nvfix;
  j->branch = in->branch;
  j->fo_first = ndmrs > 1 ? 0 : -1;
  j->fo_last = ndmrs > 1 ? ndmrs - 1 : -1;
  j->dt_d = (1.0e-3 / (double)fp->slots_per_subframe) / (double)fp->symbols_per_slot * (double)(j->dmrs_sym[ndmrs - 1] - j->dmrs_sym[0]);
  j->df_d = (double)fp->subcarrier_spacing;
  j->shape = (nr_gpu_pass_shape_t){(uint16_t)nrbc, (uint16_t)in->dlsch_config->number_symbols, (uint16_t)cw->Nl, (uint16_t)cw->qamModOrder, (uint16_t)ndmrs,
                                   (uint16_t)ncb, (uint8_t)!j->dj.evm_fp64};
  /* ---- the pass */
  memset(&out->r, 0, sizeof(out->r));
  out->r.tb = in->tb;
  out->r.demod_out = &out->demod;
  out->r.brfo_rows = out->brfo_rows;
  int qm_sym = -1;
  uint32_t qm_n = 0;
  for (int m = in->dlsch_config->start_symbol; m < in->dlsch_config->start_symbol + in->dlsch_config->number_symbols; m++)
    if (j->dj.nre[m] > qm_n) {
      qm_n = j->dj.nre[m];
      qm_sym = m;
    }
  j->qm_sym = qm_sym;
  j->qm_n = qm_n > 4096 ? 4096 : qm_n;
  out->r.qm_row = out->qm_row;
  atomic_fetch_add_explicit(&G.runs, 1, memory_order_relaxed);
  const int rc = G.try_(j, &out->r);
  if (rc != 0) /* unseen class / no free instance / unsupported variant / error: counted by the module */
    return fb(FB_MODULE);
  for (int m = 0; m < 14; m++)
    out->dl_valid_re[m] = j->dj.sym_mask >> m & 1 ? j->dj.nre[m] : 0;
  out->qm_sym = qm_sym;
  out->qm_n = j->qm_n;
  out->ndmrs = ndmrs;
  out->dmrs_first = j->dmrs_sym[0];
  out->dmrs_last = j->dmrs_sym[ndmrs - 1];
  out->C = (int)C;
  out->K = K; out->F = F; out->Z = Z; out->E = E; out->R = R; out->BG = cw->ldpcBaseGraph;
  memcpy(out->nvar_ant, out->r.nvar_ant, sizeof(out->nvar_ant));
  {
    const uint64_t n = G.runs;
    if (n == 1 || n == 64 || (n & 1023) == 0)
      census();
  }
  return 0;
}
