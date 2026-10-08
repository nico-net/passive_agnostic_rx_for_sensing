#include "PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */


#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdbool.h>
#include <signal.h>
#include "common/utils/nr_shutdown.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.h"
#include <errno.h>

#include "T.h"
#include "common/oai_version.h"
#include "assertions.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/TOOLS/nr_gpu_platform.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "PHY/NR_UE_TRANSPORT/nr_srs_passive.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_obs.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.h"
#include "SCHED_NR_UE/defs.h"
#include "common/ran_context.h"
#include "common/config/config_userapi.h"
#include "common/utils/load_module_shlib.h"
#include "common/utils/nr/nr_common.h"
#include "radio/ETHERNET/if_defs.h"
#include "openair1/PHY/MODULATION/nr_modulation.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/phy_vars_nr_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"

#include "PHY_INTERFACE/phy_interface_vars.h"
#include "NR_IF_Module.h"
#include "openair1/SIMULATION/TOOLS/sim.h"
#include "openair2/RRC/NR_UE/L2_interface_ue.h"
#include "openair1/PHY/phy_extern_nr_ue.h"

#ifdef SMBV
#include "PHY/TOOLS/smbv.h"
unsigned short config_frames[4] = {2,9,11,13};
#endif
#include "common/utils/LOG/log.h"
#include "common/utils/time_manager/time_manager.h"

#include "UTIL/OPT/opt.h"
#include "LAYER2/nr_pdcp/nr_pdcp_oai_api.h"

#include "intertask_interface.h"

#include "PHY/INIT/nr_phy_init.h"
#include "system.h"
#include <openair2/RRC/NR_UE/rrc_proto.h>
#include <openair2/LAYER2/NR_MAC_UE/mac_defs.h>
#include <openair2/LAYER2/NR_MAC_UE/mac_proto.h>
#include <openair2/NR_UE_PHY_INTERFACE/NR_IF_Module.h>
#include <openair1/SCHED_NR_UE/fapi_nr_ue_l1.h>
#include "nr_rlc/nr_rlc_oai_api.h"
/* Callbacks, globals and object handlers */

//#include "stats.h"
// current status is that every UE has a DL scope for a SINGLE eNB (eNB_id=0)
#include "PHY/TOOLS/phy_scope_interface.h"
#include "PHY/TOOLS/nr_phy_scope.h"
#include "executables/nr-ue-ru.h"
#include <executables/nr-uesoftmodem.h>
#include "executables/softmodem-common.h"
#include "executables/thread-common.h"

#include "nr_nas_msg.h"
#include "actor.h"
#include "common/utils/nr_core_plan.h"

THREAD_STRUCT thread_struct;
nrUE_params_t nrUE_params = {0};

int get_node_type() {return -1;}

RAN_CONTEXT_t RC;
int oai_exit = 0;

uint64_t        downlink_frequency[MAX_NUM_CCs][4];
int64_t         uplink_frequency_offset[MAX_NUM_CCs][4];
uint64_t        sidelink_frequency[MAX_NUM_CCs][4];

// UE and OAI config variables
double            cpuf;

int create_tasks_nrue(uint32_t ue_nb) {
  LOG_D(NR_RRC, "%s(ue_nb:%d)\n", __FUNCTION__, ue_nb);
  itti_wait_ready(1);

  if (ue_nb > 0) {
    LOG_I(NR_RRC,"create TASK_RRC_NRUE \n");
    const ittiTask_parms_t parmsRRC = {NULL, rrc_nrue};
    if (itti_create_task(TASK_RRC_NRUE, rrc_nrue_task, &parmsRRC) < 0) {
      LOG_E(NR_RRC, "Create task for RRC UE failed\n");
      return -1;
    }
    const ittiTask_parms_t parmsNAS = {NULL, nas_nrue};
    if (itti_create_task(TASK_NAS_NRUE, nas_nrue_task, &parmsNAS) < 0) {
      LOG_E(NR_RRC, "Create task for NAS UE failed\n");
      return -1;
    }
  }

  itti_wait_ready(0);

  return 0;
}

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  /* Arm the deadline before even attempting logging: a failed worker may own
   * a logger lock. Radio release must not depend on that log completing. */
  nr_shutdown_request(assert ? EXIT_FAILURE : 0);
  /* main() can reach exit_fun() (e.g. load_configmodule() failing on a missing -O file) BEFORE logInit() has
   * run; LOG_W dereferences g_log, so that error path used to segfault instead of printing the error. */
  if (g_log != NULL)
    LOG_W(NR_PHY, "called by: %s:%d %s() Exiting OAI softmodem: %s\n", file, line, function, s ? s : "no msg");
  else
    fprintf(stderr, "called by: %s:%d %s() Exiting OAI softmodem: %s\n", file, line, function, s ? s : "no msg");

  oai_exit = 1;
  /* A fatal callback can run on an actor or on main during startup. It must
   * never destroy a radio concurrently with a reader or join itself. */
  nr_shutdown_finish();
}

uint64_t get_nrUE_optmask(void) {
  return nrUE_params.optmask;
}

uint64_t set_nrUE_optmask(uint64_t bitmask) {
  nrUE_params.optmask = nrUE_params.optmask | bitmask;
  return nrUE_params.optmask;
}

nrUE_params_t *get_nrUE_params(void) {
  return &nrUE_params;
}
/* Read configuration before creating any RT workers. The solver itself is independent of OAI. */
static bool core_dl_count_explicit, core_ul_count_explicit;
static int core_env_value(const char *name)
{
  const char *s = getenv(name);
  if (!s) return -1;
  char *end; errno = 0; long c = strtol(s, &end, 10);
  if (errno || end == s || *end || c < 0 || c >= NR_CP_MAX_CPUS) return -2;
  return c;
}
static bool core_feature(const char *name, bool def)
{
  const char *s = getenv(name); return s ? atoi(s) != 0 : def;
}
static int passive_core_plan(void)
{
  if (!IS_PASSIVE_RX_MODE(get_softmodem_params()) || !core_feature("ISAC_CORE_PLAN", true)) return 0;
  nr_core_machine_t machine;
  nr_core_plan_t plan = {0};
  char error[512] = "invalid or excessive explicit core setting";
  const bool radio = !IS_SOFTMODEM_RFSIM && !getenv("ISAC_PASSIVE_REPLAY_INPUT");
  if (nrue_get_ru_count() != 1) {
    snprintf(error,sizeof(error),"automatic passive planning currently requires one RU"); goto fail;
  }
  const nrUE_RU_params_t *ru = nrue_get_ru(0);
  if (nr_core_machine_read(&machine, radio ? ru->sdr_addrs : NULL, error, sizeof(error))) goto fail;
  int physical = 0;
  for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (machine.online[c] && machine.allowed[c] && !machine.dpdk[c]) {
    bool duplicate = false;
    for (int j = 0; j < c; j++) if (machine.online[j] && machine.allowed[j]
        && machine.package[c] == machine.package[j] && machine.sibling[c] == machine.sibling[j]) duplicate = true;
    if (!duplicate) physical++;
  }
  /* Passive actors do little work compared with the deferred decoders. Preserve explicit counts;
   * scale the defaults so 4 DL + 2 UL actors do not consume the entire 12-core host. */
  const int actor_default = physical / 12 > 0 ? physical / 12 : 1;
  if (!core_dl_count_explicit) nrUE_params.num_dl_actors = actor_default < 4 ? actor_default : 4;
  if (!core_ul_count_explicit) nrUE_params.num_ul_actors = actor_default < 2 ? actor_default : 2;
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();
#define ADD(role, count, first) do { if (nr_core_plan_add(&plan, role, count, first)) goto fail; } while (0)
  ADD(NR_CP_UE, 1, core_env_value("ISAC_UE_RT_CORE"));
  if (radio && core_feature("ISAC_USRP_RING_MS", false)) ADD(NR_CP_RING, 1, core_env_value("ISAC_USRP_RING_CORE"));
  if (radio) {
    const char *s = getenv("ISAC_UHD_CTRL_CORES");
    if (s) {
      unsigned char mask[NR_CP_MAX_CPUS];
      if (nr_core_parse_list(s, mask)) goto fail;
      int count = 0;
      for (int c = 0; c < NR_CP_MAX_CPUS; c++) if (mask[c]) { ADD(NR_CP_UHD, 1, c); count++; }
      if (!count) goto fail;
    } else {
      const char *count_env = getenv("ISAC_UHD_CTRL_THREADS");
      int count = count_env ? atoi(count_env) : 1;
      if (count < 1) goto fail;
      ADD(NR_CP_UHD, count, -1);
    }
  }
  ADD(NR_CP_DL, nrUE_params.num_dl_actors, nrUE_params.dl_actor_core_start);
  ADD(NR_CP_UL, nrUE_params.num_ul_actors, nrUE_params.ul_actor_core_start);
  ADD(NR_CP_SYNC, 1, nrUE_params.sync_actor_core);
  ADD(NR_CP_SCAN, cfg->scan_thread < 4 ? cfg->scan_thread : 4, cfg->scan_thread_core);
  if (core_feature("ISAC_PDCCH_USS_TRACKER", true)) ADD(NR_CP_USS, 1, core_env_value("ISAC_PDCCH_USS_CORE"));
  ADD(NR_CP_PDSCH, cfg->pdsch_thread < 6 ? cfg->pdsch_thread : 6, cfg->pdsch_thread_core);
  ADD(NR_CP_PUSCH, nr_pusch_passive_pipeline_enabled() && cfg->ul_pusch_decode ? 1 :
      (cfg->ul_thread < 6 ? cfg->ul_thread : 6), cfg->ul_thread_core);
  if (get_softmodem_params()->threadPoolConfigExplicit) {
    char *text = strdup(get_softmodem_params()->threadPoolConfig), *save = NULL;
    if (!text) goto fail;
    bool valid = true;
    int worker_count = 0;
    for (char *s = strtok_r(text, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
      if (!strcmp(s,"n") || !strcmp(s,"N")) continue;
      char *end; errno = 0; long c = strtol(s,&end,10);
      if (++worker_count > 64 || errno || *end || end == s || c < -1 || c >= NR_CP_MAX_CPUS || nr_core_plan_add(&plan,NR_CP_POOL,1,c)) { valid = false; break; }
    }
    free(text); if (!valid) goto fail;
  } else {
    int workers = ru->nb_rx > 2 ? ru->nb_rx : 2;
    ADD(NR_CP_POOL, workers < 8 ? workers : 8, -1);
  }
  if (core_feature("NR_GPU_FEP", false)) ADD(NR_CP_GPU, 1, core_env_value("ISAC_GPU_WORKER_CORE"));
  if (core_feature("ISAC_CSIRS_DISC", false)) ADD(NR_CP_CSIRS, 1, core_env_value("ISAC_CSIRS_DISC_CORE"));
  if (core_feature("ISAC_PDCCH_NID_GPU", false)) ADD(NR_CP_NID, 1, core_env_value("ISAC_PDCCH_NID_GPU_CORE"));
  ADD(NR_CP_STATS, 1, -1);
#undef ADD
  if (nr_core_plan_build(&machine, &plan, error, sizeof(error))) goto fail;
  /* Replace only the default pool specification. Explicit -1 entries retain their count and are
   * filled from the plan too; explicit numbered entries are preserved by the solver. */
  static char pool[NR_CP_MAX_ROLES * 6];
  size_t pos = 0;
  for (int i = 0; i < plan.count; i++) if (plan.a[i].role == NR_CP_POOL)
    pos += snprintf(pool + pos, sizeof(pool) - pos, "%s%d", pos ? "," : "", plan.a[i].core);
  if (!pos) strcpy(pool,"n");
  get_softmodem_params()->threadPoolConfig = pool;
  if (nr_core_plan_activate(&machine, &plan)) { snprintf(error,sizeof(error),"cannot apply plan: %s",strerror(errno)); goto fail; }
  return 0;
fail:
  fprintf(stderr,"SENSING: CORE_PLAN error: %s; radio startup cancelled (ISAC_CORE_PLAN=0 restores legacy behavior)\n",error);
  return -1;
}

static void get_options(configmodule_interface_t *cfg)
{
  paramdef_t cmdline_params[] = CMDLINE_NRUEPARAMS_DESC;
  int numparams = sizeofArray(cmdline_params);
  config_get(cfg, cmdline_params, numparams, NULL);
  core_dl_count_explicit = config_isparamset(cmdline_params, config_paramidx_fromname(cmdline_params, numparams, "num-dl-actors"));
  core_ul_count_explicit = config_isparamset(cmdline_params, config_paramidx_fromname(cmdline_params, numparams, "num-ul-actors"));
  AssertFatal(nrUE_params.extra_pdu_id == -1,
              "Add additional PDU sessions in uicc.pdu_sessions array instead\n");
}

// set PHY vars from command line
static void set_UE_options(int CC_id, PHY_VARS_NR_UE *UE, int ru_id)
{
  const nrUE_RU_params_t *RU = nrue_get_ru(ru_id);

  // Set UE variables
  UE->rx_total_gain_dB = RU->max_rxgain - RU->att_rx;
  UE->tx_total_gain_dB = RU->att_tx;
  UE->if_freq          = RU->if_frequency;
  UE->if_freq_off      = RU->if_freq_offset;
  UE->rf_map.card      = ru_id;
  UE->rf_map.chain     = CC_id;

  UE->tx_power_max_dBm     = nrUE_params.tx_max_power;
  UE->max_ldpc_iterations  = nrUE_params.max_ldpc_iterations;
  UE->UE_scan_carrier      = nrUE_params.UE_scan_carrier;
  UE->UE_fo_compensation   = nrUE_params.UE_fo_compensation;
  UE->chest_freq           = nrUE_params.chest_freq;
  UE->chest_time           = nrUE_params.chest_time;
  UE->no_timing_correction = nrUE_params.no_timing_correction;
  UE->initial_fo           = nrUE_params.initial_fo;
  UE->cont_fo_comp         = nrUE_params.cont_fo_comp;
  UE->disable_blind_search = nrUE_params.disable_blind_search;

  LOG_I(PHY,"Set UE_fo_compensation %d, UE_scan_carrier %d, UE_no_timing_correction %d \n, chest-freq %d, chest-time %d\n",
        UE->UE_fo_compensation, UE->UE_scan_carrier, UE->no_timing_correction, UE->chest_freq, UE->chest_time);
}

static void set_fp_options(int cell_id, int ru_id)
{
  NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(cell_id);
  const nrUE_RU_params_t *RU = nrue_get_ru(ru_id);

  // Set FP variables
  fp->nb_antennas_rx = RU->nb_rx;
  fp->nb_antennas_tx = RU->nb_tx;

  fp->threequarter_fs     = get_softmodem_params()->threequarter_fs;
  fp->ofdm_offset_divisor = nrUE_params.ofdm_offset_divisor;

  LOG_I(PHY, "Set UE nb_rx_antenna %d, nb_tx_antenna %d, threequarter_fs %d, ofdm_offset_divisor %d\n", fp->nb_antennas_rx, fp->nb_antennas_tx, fp->threequarter_fs, fp->ofdm_offset_divisor);
}

static void nrue_shutdown_notify(void)
{
  oai_exit = 1;
  /* raw-IQ EOF takes nr_shutdown_finish()'s _exit path, so main's normal
   * consumer teardown is not reached. Join the prewarmed UL worker here. */
  if (nr_pusch_passive_pipeline_enabled()) nr_pusch_passive_queue_stop();
  itti_wait_tasks_unblock();
}

/* Active (non-passive) SA UE only: the first ^C asks NAS to deregister, the
 * second ^C or a 5 s alarm shuts down unconditionally (no oai_exit guard: after
 * a radio stall oai_exit is already 1 and the old guard made the alarm a no-op).
 * Passive RX has nothing to deregister: one SIGINT/SIGTERM stops it. */
static void trigger_stop(int sig)
{
  nr_shutdown_signal(SIGINT);
}
static void trigger_deregistration(int sig)
{
  static bool deregistration_sent = false;
  if (deregistration_sent || oai_exit || nr_shutdown_requested()) {
    nr_shutdown_signal(sig);
    return;
  }
  deregistration_sent = true;
  for (int ue_inst = 0; ue_inst < NB_UE_INST; ue_inst++) {
    MessageDef *msg = itti_alloc_new_message(TASK_NAS_NRUE, ue_inst, NAS_DEREGISTRATION_REQ);
    NAS_DEREGISTRATION_REQ(msg).cause = AS_DETACH;
    itti_send_msg_to_task(TASK_NAS_NRUE, ue_inst, msg);
  }
  static const char m[] = "Press ^C again to trigger immediate shutdown\n";
  __attribute__((unused)) int unused = write(STDOUT_FILENO, m, sizeof(m) - 1);
  signal(SIGALRM, trigger_stop);
  alarm(5);
}

void *nrue_ru_start_thread(void *arg)
{
  (void)arg;
  return (void *)(intptr_t)nrue_ru_start();
}

int NB_UE_INST = 1;
configmodule_interface_t *uniqCfg = NULL;
nrLDPC_coding_interface_t nrLDPC_coding_interface = {0};

static void gpu_platform_log(int warning, const char *text)
{
  if (warning) LOG_W(PHY, "%s\n", text);
  else LOG_I(PHY, "%s\n", text);
}

int main(int argc, char **argv)
{
  start_background_system();
  if (nr_shutdown_init(nrue_shutdown_notify, nrue_ru_shutdown_stop, nrue_ru_shutdown) != 0) {
    fprintf(stderr, "Cannot initialize bounded radio shutdown\n");
    return EXIT_FAILURE;
  }

  if ((uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY)) == NULL || CONFIG_ISFLAGSET(CONFIG_ABORT)) {
    exit_fun("[SOFTMODEM] Error, configuration module init failed\n");
  }
  /* SIGINT/SIGTERM are owned by nr_shutdown from before configuration. */
  CONFIG_SETRTFLAG(CONFIG_NOEXITONHELP);
  // initialize logging
  logInit();
  nr_gpu_set_logger(gpu_platform_log);
  // get options and fill parameters from configuration file

  get_options(uniqCfg); // Command-line options specific for NRUE
  IS_SOFTMODEM_5GUE = true;
  get_common_options(uniqCfg);
  CONFIG_CLEARRTFLAG(CONFIG_NOEXITONHELP);

  // ISAC / passive-radar sensing pipeline: parse the [sensing] config section (no-op if disabled).
  nr_isac_init();
  // UE-agnostic CSI-RS sensing monitors (cell-common / other-UE resources); no-op if unset.
  nr_csirs_monitor_init();
  // Blind PDCCH/DCI-1_1 monitor (TOTAL_PASSIVE_UE_HANDOVER.md Phase 3); no-op if unset.
  nr_pdcch_blind_monitor_init();
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && core_feature("ISAC_CORE_PLAN", true))
    nrue_set_ru_params(uniqCfg);
  if (passive_core_plan()) return EXIT_FAILURE;
  if (nr_cfg_reconf_enabled() || (getenv("ISAC_UL_UE_STATE") && strcmp(getenv("ISAC_UL_UE_STATE"), "1") == 0)) {
    const char *uectx_path = getenv("ISAC_UECTX_PATH");
    if ((uectx_path && *uectx_path) || (getenv("ISAC_UL_UE_STATE") && strcmp(getenv("ISAC_UL_UE_STATE"), "1") == 0)) {
      const char *period_env = getenv("ISAC_UECTX_PERIOD_S");
      const double period = period_env && *period_env ? atof(period_env) : 5.0;
      if (nr_ue_ctx_open(uectx_path, 16384, period))
        nr_cfg_epoch_subscribe(nr_ue_ctx_on_epoch);
      else
        LOG_W(PHY, "SENSING: UE context writer/state could not be opened\n");
    }
  }
  // Per-grant observation API (Task A3): JSONL of every decoded PDSCH/PUSCH grant; off unless ISAC_OBS_PATH is set.
  {
    const char *obs_path = getenv("ISAC_OBS_PATH");
    /* ISAC_OBS_TEST_RING_SLOTS: test hook (overload A/B with a tiny ring); the default is used when unset. */
    const char *ring_env = getenv("ISAC_OBS_TEST_RING_SLOTS");
    uint32_t ring = 1u << 14;
    if (ring_env && *ring_env) {
      char *end = NULL;
      const unsigned long v = strtoul(ring_env, &end, 10);
      if (end && *end == '\0' && v >= 1)
        ring = v > (1ul << 20) ? (1u << 20) : (uint32_t)v; // clamp to [1, 1<<20]
      else
        LOG_W(PHY, "SENSING: ISAC_OBS_TEST_RING_SLOTS=%s invalid, using %u\n", ring_env, ring);
    }
    if (obs_path && *obs_path) {
      if (nr_passive_obs_open(obs_path, ring))
        LOG_I(PHY, "SENSING: per-grant observations -> %s (ring %u, drop-on-full)\n", obs_path, ring);
      else
        LOG_W(PHY, "SENSING: ISAC_OBS_PATH=%s set but the observation writer could not be opened; no observations\n", obs_path);
    }
  }

  softmodem_verify_mode(get_softmodem_params());

#if T_TRACER
  T_Config_Init();
#endif
  initTpool(get_softmodem_params()->threadPoolConfig, &(nrUE_params.Tpool), cpumeas(CPUMEAS_GETSTATE));
  set_taus_seed (0);

  if (!has_cap_sys_nice())
    LOG_W(UTIL,
          "no SYS_NICE capability: cannot set thread priority and affinity, consider running with sudo for optimum performance\n");

  cpuf=get_cpu_freq_GHz();
  itti_init(TASK_MAX, tasks_info);

  init_opt();

  int ret_loader = load_nrLDPC_coding_interface(NULL, &nrLDPC_coding_interface, 32);
  AssertFatal(ret_loader == 0, "error loading LDPC library\n");

  // strdup to put the sring in the core file for post mortem identification
  char *pckg = strdup(OAI_PACKAGE_VERSION);
  LOG_I(HW, "Version: %s\n", pckg);

  nrPHY_vars_UE_g = calloc_or_fail(NB_UE_INST, sizeof(*nrPHY_vars_UE_g));
  for (int inst = 0; inst < NB_UE_INST; inst++) {
    nrPHY_vars_UE_g[inst] = calloc_or_fail(MAX_NUM_CCs, sizeof(*nrPHY_vars_UE_g[inst]));
    for (int CC_id = 0; CC_id < MAX_NUM_CCs; CC_id++) {
      nrPHY_vars_UE_g[inst][CC_id] = calloc_or_fail(1, sizeof(*nrPHY_vars_UE_g[inst][CC_id]));
      // All instances use the same coding interface
      nrPHY_vars_UE_g[inst][CC_id]->nrLDPC_coding_interface = nrLDPC_coding_interface;
    }
  }

  if (create_tasks_nrue(1) < 0) {
    printf("cannot create ITTI tasks\n");
    exit(-1); // need a softer mode
  }

  nr_pdcp_layer_init();
  nas_init_nrue(NB_UE_INST);

  if (!nr_core_plan_active()) nrue_set_ru_params(uniqCfg);
  nrue_set_cell_params(uniqCfg);

  init_NR_UE(NB_UE_INST,
             get_nrUE_params()->uecap_file,
             get_nrUE_params()->reconfig_file,
             get_nrUE_params()->rbconfig_file,
             nrue_get_cell(0)->numerology);

  // start time manager with some reasonable default for the running mode
  // (may be overwritten in configuration file or command line)
  void nr_pdcp_ms_tick(void);
  void nr_rlc_ms_tick(void);
  time_manager_tick_function_t tick_functions[] = {
    nr_pdcp_ms_tick,
    nr_rlc_ms_tick
  };
  int tick_functions_count = 2;
  time_manager_start(tick_functions, tick_functions_count,
                     // iq_samples time source for rfsim,
                     // realtime time source if not
                     IS_SOFTMODEM_RFSIM ? TIME_SOURCE_IQ_SAMPLES
                                        : TIME_SOURCE_REALTIME);

  // initialize per-cell frame parameters
  for (int cell_id = 0; cell_id < nrue_get_cell_count(); cell_id++) {
    int ru_id = nrue_get_cell(cell_id)->ru_id;
    AssertFatal(ru_id >= 0 && ru_id < nrue_get_ru_count(),
                "Invalid ru_id (%d) for cell %d. Should be >= 0 and < %d\n",
                ru_id,
                cell_id,
                nrue_get_ru_count());
    AssertFatal(nrue_get_ru(ru_id)->used_by_cell == -1,
                "RU %d is already used by cell %d and therefore cannot also be used by cell %d\n",
                ru_id,
                nrue_get_ru(ru_id)->used_by_cell,
                cell_id);
    nrue_set_ru_cell_id(ru_id, cell_id);

    set_fp_options(cell_id, ru_id);
    nr_isac_set_rx_antennas(nrue_get_cell_fp(cell_id)->nb_antennas_rx);
    if (IS_SA_MODE(get_softmodem_params()) || get_softmodem_params()->sl_mode)
      nr_init_frame_parms_ue_sa(nrue_get_cell_fp(cell_id), nrue_get_cell(cell_id));
  }

  int cell_id = 0;
  for (int inst = 0; inst < NB_UE_INST; inst++) {
    NR_UE_MAC_INST_t *mac = get_mac_inst(inst);

    for (int CC_id = 0; CC_id < MAX_NUM_CCs; CC_id++) {
      PHY_VARS_NR_UE *UE_CC = nrPHY_vars_UE_g[inst][CC_id];

      AssertFatal(cell_id >= 0 && cell_id < nrue_get_cell_count(),
                  "There are not enough cell definitions for all UEs! NB_UE_INST = %d, MAX_NUM_CCs = %d, nrue_cell_count = %d\n",
                  NB_UE_INST,
                  MAX_NUM_CCs,
                  nrue_get_cell_count());
      nrUE_cell_params_t cell = *nrue_get_cell(cell_id);

      AssertFatal(cell.used_by_ue == -1,
                  "Cell %d is already used by UE %d and cannot also be used by UE %d as cells map 1:1 to RUs and RU sharing is not implemented\n",
                  cell_id,
                  cell.used_by_ue,
                  inst);
      cell.used_by_ue = inst;

      set_UE_options(CC_id, UE_CC, cell.ru_id);
      init_nr_ue_phy_cpu_stats(&UE_CC->phy_cpu_stats);

      NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(cell_id);
      if (!IS_SA_MODE(get_softmodem_params()) && !get_softmodem_params()->sl_mode) {
        do {
          notifiedFIFO_elt_t *elt = pollNotifiedFIFO(&mac->input_nf);
          if (!elt) {
            break;
          }
          process_msg_rcc_to_mac(NotifiedFifoData(elt), inst);
          delNotifiedFIFO_elt(elt);
        } while (true);
        fapi_nr_config_request_t *nrUE_config = &UE_CC->nrUE_config;
        nr_init_frame_parms_ue(fp, nrUE_config, mac->nr_band);

        cell.band = mac->nr_band;
        cell.rf_frequency = fp->dl_CarrierFreq;
        cell.rf_freq_offset = fp->ul_CarrierFreq - fp->dl_CarrierFreq;
        cell.numerology = fp->numerology_index;
        cell.N_RB_DL = fp->N_RB_DL;
        cell.ssb_start = fp->ssb_start_subcarrier;
      }
      nrue_set_cell(cell_id, &cell);

      UE_CC->frame_parms = *fp;
      mac->nr_band = cell.band;
      mac->ssb_start_subcarrier = cell.ssb_start;
      mac->dl_frequency = cell.rf_frequency;

      UE_CC->sl_mode = get_softmodem_params()->sl_mode;
      init_actor(&UE_CC->sync_actor, "SYNC_", nr_core_plan_cpu(NR_CP_SYNC, 0, get_nrUE_params()->sync_actor_core));
      if (get_nrUE_params()->num_dl_actors > 0) {
        UE_CC->dl_actors = calloc_or_fail(get_nrUE_params()->num_dl_actors, sizeof(*UE_CC->dl_actors));
        for (int i = 0; i < get_nrUE_params()->num_dl_actors; i++) {
          const int first_core = get_nrUE_params()->dl_actor_core_start;
          init_actor(&UE_CC->dl_actors[i], "DL_", nr_core_plan_cpu(NR_CP_DL, i, first_core < 0 ? -1 : first_core + i));
        }
      }
      if (get_nrUE_params()->num_ul_actors > 0) {
        UE_CC->ul_actors = calloc_or_fail(get_nrUE_params()->num_ul_actors, sizeof(*UE_CC->ul_actors));
        for (int i = 0; i < get_nrUE_params()->num_ul_actors; i++) {
          const int first_core = get_nrUE_params()->ul_actor_core_start;
          init_actor(&UE_CC->ul_actors[i], "UL_", nr_core_plan_cpu(NR_CP_UL, i, first_core < 0 ? -1 : first_core + i));
        }
      }
      init_nr_ue_vars(UE_CC, inst);

      if (UE_CC->sl_mode) {
        AssertFatal(UE_CC->sl_mode == 2, "Only Sidelink mode 2 supported. Mode 1 not yet supported\n");
        DevAssert(mac->if_module != NULL && mac->if_module->sl_phy_config_request != NULL);
        nr_sl_phy_config_t *phycfg = &mac->SL_MAC_PARAMS->sl_phy_config;
        phycfg->sl_config_req.sl_carrier_config.sl_num_rx_ant = get_nrUE_params()->nb_antennas_rx;
        phycfg->sl_config_req.sl_carrier_config.sl_num_tx_ant = get_nrUE_params()->nb_antennas_tx;
        mac->if_module->sl_phy_config_request(phycfg);
        sl_nr_ue_phy_params_t *sl_phy = &UE_CC->SL_UE_PHY_PARAMS;
        nr_init_frame_parms_ue_sl(&sl_phy->sl_frame_params,
                                  &sl_phy->sl_config,
                                  get_softmodem_params()->threequarter_fs,
                                  get_nrUE_params()->ofdm_offset_divisor);
        sl_ue_phy_init(UE_CC);
      }

      cell_id++; // initially connect each UE and carrier to its own cell
    }
  }

  PHY_VARS_NR_UE *disc_ue = nrPHY_vars_UE_g[0][0];
  /* Strong reference: the feed reaches the pipeline through a weak symbol, which alone does not
   * pull nr_csirs_disc.o out of the static PHY archive (the hook stayed NULL, zero batches). */
  nr_dl_gpu_warmup(disc_ue);
  nr_pdcch_nid_gpu_start(disc_ue->frame_parms.N_RB_DL);
  nr_csirs_disc_pipeline_init(&disc_ue->frame_parms);
  if (nr_csirs_disc_start(&disc_ue->frame_parms, disc_ue->frame_parms.nb_antennas_rx) != 0)
    LOG_W(PHY, "SENSING: CSIRS_DISC worker could not start; slot discovery feed disabled\n");
  /* U1 owns exactly one bounded consumer/context, warmed before the RX seal.
   * A failed start leaves admission closed; the RT path only counts drops. */
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_pusch_passive_pipeline_enabled()) {
    const nr_pdcch_blind_monitor_cfg_t *ul_cfg = nr_pdcch_blind_monitor_get_cfg();
    if (ul_cfg && ul_cfg->ul_pusch_decode) {
      if (!nr_pusch_passive_queue_start(disc_ue, ul_cfg->ul_queue_depth > 0 ? ul_cfg->ul_queue_depth : 8,
                                        1, ul_cfg->ul_thread_core))
        LOG_W(PHY, "SENSING: UL pipeline prewarm failed; grants will be dropped and counted\n");
    }
  }

  /* Offline replay exits before any radio initialization. */
  const char *replay_input = getenv("ISAC_PASSIVE_REPLAY_INPUT");
  if (replay_input || getenv("ISAC_PASSIVE_REPLAY_CAPTURE")) {
    AssertFatal(IS_PASSIVE_RX_MODE(get_softmodem_params()), "Replay requires --passive-rx\n");
    if (replay_input) {
      const int replay_result = nr_passive_replay_read(nrPHY_vars_UE_g[0][0], replay_input);
      nr_pusch_passive_queue_stop();
      nr_pdcch_nid_gpu_stop();
      nr_csirs_disc_stop();
      return replay_result;
    }
    nr_passive_replay_init(nrPHY_vars_UE_g[0][0]);
  }

  nr_gpu_rx_started(); /* All migrated CUDA allocation/registration ends before RX. */
  nrue_init_openair0();



  lock_memory_to_ram();

  if (IS_SOFTMODEM_DOSCOPE) {
    load_softscope("nr", nrPHY_vars_UE_g[0][0]);
  }
  if (IS_SOFTMODEM_IMSCOPE_ENABLED) {
    load_softscope("im", nrPHY_vars_UE_g[0][0]);
  }
  AssertFatal(!(IS_SOFTMODEM_IMSCOPE_ENABLED && IS_SOFTMODEM_IMSCOPE_RECORD_ENABLED),
              "Data recoding and ImScope cannot be enabled at the same time\n");
  if (IS_SOFTMODEM_IMSCOPE_RECORD_ENABLED) {
    load_module_shlib("imscope_record", NULL, 0, nrPHY_vars_UE_g[0][0]);
  }

  // Launch a temporary high-priority thread to start the UE RU, ensuring radio library threads inherit this priority
  pthread_t ru_start_thread;
  threadCreate(&ru_start_thread, nrue_ru_start_thread, NULL, "ru_start_thread", nr_core_plan_cpu(NR_CP_UHD, 0, -1), OAI_PRIORITY_RT_MAX);
  void *ru_start_result = NULL;
  int ret = pthread_join(ru_start_thread, &ru_start_result);
  AssertFatal(ret == 0, "pthread_join error %d, errno %d (%s)\n", ret, errno, strerror(errno));

  if (ru_start_result) {
    LOG_E(HW, "SENSING: radio startup failed; receiver exiting without starting RX workers\n");
    nr_shutdown_request(EXIT_FAILURE);
    nr_shutdown_finish();
  }

  if (nr_shutdown_requested())
    nr_shutdown_finish();

  // Launch the sensing engine consumer thread before the DL workers start producing CFR snapshots.
  nr_isac_start();
  if (nr_sense_enabled()) {
    PHY_VARS_NR_UE *sense_ue = nrPHY_vars_UE_g[0][0];
    const openair0_config_t *sense_cfg = &openair0_cfg_g[sense_ue->rf_map.card];
    nr_sense_emit_session(&sense_ue->frame_parms,
                          sense_cfg->rx_gain[sense_ue->rf_map.chain] - sense_cfg->rx_gain_offset[sense_ue->rf_map.chain]);
  }
  nr_prach_passive_start();
  if (IS_PASSIVE_RX_MODE(get_softmodem_params())) nr_srs_passive_start(nrPHY_vars_UE_g[0][0]);

  for (int inst = 0; inst < NB_UE_INST; inst++) {
    LOG_I(PHY,"Intializing UE Threads for instance %d ...\n", inst);
    init_NR_UE_threads(nrPHY_vars_UE_g[inst][0]);
  }

  // wait for end of program
  printf("TYPE <CTRL-C> TO TERMINATE\n");

  // Sleep a while before checking all parameters have been used
  // Some are used directly in external threads, asynchronously
  sleep(2);
  config_check_unknown_cmdlineopt(uniqCfg, CONFIG_CHECKALLSECTIONS);

  // wait for end of program
  printf("Entering ITTI signals handler\n");
  printf("TYPE <CTRL-C> TO TERMINATE\n");
  const bool deregister_first = IS_SA_MODE(get_softmodem_params()) && !IS_PASSIVE_RX_MODE(get_softmodem_params());
  itti_wait_tasks_end(deregister_first ? trigger_deregistration : nr_shutdown_signal);
  LOG_W(NR_PHY, "Returned from ITTI signal handler\n");
  oai_exit = 1;

  nr_shutdown_request(0);
  nr_shutdown_wait_radio();
  nr_shutdown_step("radio_released_before_worker_joins");

  if (nrPHY_vars_UE_g) {
    for (int inst = 0; inst < NB_UE_INST; inst++) {
      for (int CC_id = 0; CC_id < MAX_NUM_CCs; CC_id++) {
        PHY_VARS_NR_UE *phy_vars = nrPHY_vars_UE_g[inst][CC_id];
        if (!phy_vars)
          continue;
        /* Stop the producer before closing its actors. Actors and the task
         * pool remain available while an in-flight UE task finishes. */
        nr_shutdown_step("ue_join_begin");
        pthread_join(phy_vars->main_thread, NULL);
        if (!IS_SOFTMODEM_NOSTATS)
          pthread_join(phy_vars->stat_thread, NULL);
        nr_shutdown_step("ue_join_complete");
        shutdown_actor(&phy_vars->sync_actor);
        for (int i = 0; i < get_nrUE_params()->num_dl_actors; i++)
          shutdown_actor(&phy_vars->dl_actors[i]);
        for (int i = 0; i < get_nrUE_params()->num_ul_actors; i++)
          shutdown_actor(&phy_vars->ul_actors[i]);
        nr_shutdown_step("actors_joined");
      }
    }
  }

  /* Scan consumers can enqueue PDSCH/PUSCH work. Stop them first; the PDSCH
   * stop API also wakes and joins gpuFepWorker. Keep Tpool alive until all
   * consumers and discovery workers have returned. */
  nr_shutdown_step("passive_pdcch_stop_begin");
  nr_pdcch_passive_queue_stop();
  nr_shutdown_step("passive_pdsch_gpu_fep_stop_begin");
  nr_pdsch_passive_queue_stop();
  nr_shutdown_step("passive_pusch_stop_begin");
  nr_pusch_passive_queue_stop();
  nr_pdcch_uss_tracker_stop();
  nr_shutdown_step("discovery_stop_begin");
  nr_pdcch_nid_gpu_stop();
  nr_csirs_disc_stop();
  nr_shutdown_step("discovery_stopped");
  abortTpool(&nrUE_params.Tpool);
  nr_shutdown_step("thread_pool_stopped");

  // All slot producers have joined. Drain the PRACH worker before closing records.
  nr_prach_passive_flush();
  nr_srs_passive_stop();
  nr_isac_stop();
  nr_shutdown_step("sensing_stopped");

  nr_passive_obs_close(); // drain + flush the per-grant observation file (no-op when not open)
  const bool uectx_was_open = nr_ue_ctx_enabled();
  nr_ue_ctx_close();
  if (uectx_was_open) {
    uint64_t events = 0, lines = 0, dropped = 0;
    nr_ue_ctx_stats(&events, &lines, &dropped);
    LOG_I(PHY, "SENSING: UECTX_STATS events=%llu written=%llu dropped=%llu\n",
          (unsigned long long)events, (unsigned long long)lines, (unsigned long long)dropped);
  }

  nr_shutdown_step("outputs_closed");

  free_nrLDPC_coding_interface(&nrLDPC_coding_interface);

  nr_shutdown_step("itti_stop_begin");
  itti_stop_join_tasks();
  time_manager_finish();
  nr_pdcp_stop_threads();
  stop_background_system();

  free(pckg);
  nr_shutdown_step("services_stopped");
  nr_shutdown_finish();
}
