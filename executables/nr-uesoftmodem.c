/* A8: the cell manager headers come first: OAI's nr_polar_defs.h defines the macro `nmax`, a field name of nr_cell_mgr_t */
#include "common/utils/nr_cell_life.h"
#include "common/utils/nr_cell_inventory.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
#include "PHY/NR_UE_TRANSPORT/nr_initial_sync_budget.h"
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
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.h" /* NR_GPU_PASS_RING */
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "common/utils/nr_dashboard.h"
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "PHY/NR_UE_TRANSPORT/nr_srs_passive.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc.h"
#include "PHY/TOOLS/nr_host_membudget.h"
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
#include "common/utils/nr_core_layout.h"
#include "common/utils/nr_cell.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_cell.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_ul_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc_cell.h"
#include <math.h>

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

/* exit_function: fatal-exit handler of the softmodem (used by the assertion macros and exit_fun()): requests a bounded shutdown with status EXIT_FAILURE for assertions, logs the caller, sets oai_exit and finishes through nr_shutdown_finish().
 *   Logs with LOG_W when the logger exists, else to stderr. Never returns. Callers: common/utils/assertions.h (AssertFatal and exit paths), exit_fun().
 */
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
static int ssb_given, fo_given; // explicit --ssb / --initial-fo on the command line (pinned cell)
/* get_options: reads the NR UE command-line parameters (CMDLINE_NRUEPARAMS_DESC); asserts that no extra PDU session was requested. Caller: main(). */
static void get_options(configmodule_interface_t *cfg)
{
  paramdef_t cmdline_params[] = CMDLINE_NRUEPARAMS_DESC;
  int numparams = sizeofArray(cmdline_params);
  config_get(cfg, cmdline_params, numparams, NULL);
  for (int i = 0; i < numparams; i++) {
    const int set = !!(cmdline_params[i].paramflags & PARAMFLAG_PARAMSET);
    if (!strcmp(cmdline_params[i].optname, "ssb"))
      ssb_given = set;
    else if (!strcmp(cmdline_params[i].optname, "initial-fo"))
      fo_given = set;
  }
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

/* ================================================================ multi-PCI cell slots (plan task A8) ==========================================
 * Cells come ONLY from the blind front end's inventory <run dir>/cells.json (D1; run dir = the directory of stdout when it is a regular file,
 * the same rule as uectx.jsonl) and the admission plan (B3, through the cell manager and nr_cell_life). Cell 0 is always the cell the process
 * is started on (its command-line tuning); inventory cells 1..N-1 are added as UE instances 1..N-1, each with its own RU reading its cell's
 * IQ ring "nrcell_<inventory id>" through the shm_iq consumer device, its own RT slot thread (decision F2 (a), core = the core plan's
 * rt_cell[k]) and all its threads bound to its cell. No option, no environment variable: no inventory, or an inventory that adds no served
 * cell, is the single-cell receiver, byte for byte (only the MPCI_ADMIT / CORE_PLAN lines are added when an inventory exists).
 * Input: the cells' rings must already be published (configured device "shm_iq": an out-of-process publisher, e.g. the file publisher
 * tests/passive_rx/mpci/mpci_file_pub). The in-process channelizer feed from a radio (C2 iq_chan) is NOT wired here (open item): with a
 * radio device the receiver logs MPCI_INPUT and serves the started cell only. N = 1 keeps the direct radio path (no channelizer hop). */
static nr_cell_life_t g_cells;
static int g_ncells = 1; /* 1: the single-cell receiver; every N = 1 path below is the code of before */
static nr_cell_inventory_watch_t g_inv_watch;
static char g_uectx_path[PATH_MAX + 32]; /* cell 0's uectx path: cells k > 0 use nr_cell_path(k, ...) */

static const char *mpci_argv_val(int argc, char **argv, const char *opt)
{
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], opt))
      return argv[i + 1];
  return NULL;
}
static int64_t mpci_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
/* the run directory: the directory of stdout when it is a regular file (uectx.jsonl rule); -1 otherwise */
static int mpci_run_dir(char *out, size_t n)
{
  char l[PATH_MAX];
  const ssize_t k = readlink("/proc/self/fd/1", l, sizeof(l) - 1);
  struct stat st;
  l[k > 0 ? k : 0] = 0;
  if (l[0] != '/' || stat(l, &st) || !S_ISREG(st.st_mode))
    return -1;
  snprintf(out, n, "%s", dirname(l));
  return 0;
}

static int mpci_cell_prealloc(void *ctx, int k, const nr_mgr_slot_t *slot);
static int mpci_cell_start(void *ctx, int k, const nr_mgr_slot_t *slot, int reactivation);
static void mpci_cell_retire(void *ctx, int k, const nr_mgr_slot_t *slot);
static void mpci_cell_retune(void *ctx, int k, const nr_cell_mgr_event_t *ev);
static void mpci_log(void *ctx, const char *line)
{
  (void)ctx;
  if (!g_log) /* before logInit (the plan) the line goes to stdout; later through LOG (tagged when N > 1) */
    printf("%s\n", line);
  else
    LOG_I(PHY, "SENSING: %s\n", line);
}

/* Called once at the very start of main(), before the core plan: inventory -> admission -> slots. Returns the number of cells to serve. */
static int mpci_plan_cells(int argc, char **argv, bool radio)
{
  char rd[PATH_MAX], path[PATH_MAX + 16];
  if (mpci_run_dir(rd, sizeof(rd)) || nr_cell_inventory_default_path(rd, path, sizeof(path)) || nr_cell_inventory_watch_init(&g_inv_watch, path))
    return 1;
  char err[200] = "";
  const int r = nr_cell_inventory_poll(&g_inv_watch, err, sizeof(err));
  if (r == 0)
    return 1; /* no inventory: the single-cell receiver, nothing printed */
  if (r < 0) {
    printf("MPCI_INVENTORY rejected path=%s reason=%s: single cell\n", path, err);
    return 1;
  }
  nr_cell_desc_t p;
  nr_cell_desc_init(&p);
  const char *v;
  p.centre_hz = (v = mpci_argv_val(argc, argv, "-C")) ? strtod(v, NULL) : 0;
  p.n_rb = (v = mpci_argv_val(argc, argv, "-r")) ? atoi(v) : 0;
  const int mu = (v = mpci_argv_val(argc, argv, "--numerology")) ? atoi(v) : 1;
  p.scs_khz = 15 << (mu >= 0 && mu < 4 ? mu : 1);
  p.n_rx = (v = mpci_argv_val(argc, argv, "--ue-nb-ant-rx")) ? atoi(v) : 1;
  p.cfo_hz = (v = mpci_argv_val(argc, argv, "--initial-fo")) ? strtod(v, NULL) : 0;
  const int ssb = (v = mpci_argv_val(argc, argv, "--ssb")) ? atoi(v) : 0;
  if (p.centre_hz <= 0) {
    printf("MPCI_ADMIT skipped: the started cell has no -C (band search): single cell\n");
    return 1;
  }
  /* host caps from the device: RAM probe, core topology (the layout's own cell limit); GPU not probed before the CUDA configuration
   * (tier NONE here: the GPU side of the admission is G1/G3 work); the cells' streams come as published rings (no channelizer budget) */
  nr_cl_topo_t topo;
  nr_cl_over_t over;
  nr_cl_topo_read(&topo, radio);
  nr_cl_over_from_env(&over);
  nr_core_layout_t lay;
  nr_cl_build_n(&topo, &over, 1, &lay);
  nr_host_probe_t hp;
  nr_host_probe_at("/proc", "/sys/fs/cgroup", &hp);
  nr_host_caps_t caps;
  memset(&caps, 0, sizeof(caps));
  caps.mem_avail = hp.avail;
  caps.max_cells = nr_cl_max_cells(&topo, &over);
  if (caps.max_cells < 1)
    caps.max_cells = 1;
  caps.chan_budget_msps = -1;
  caps.consumers = lay.npool < 1 ? 1 : lay.npool > 6 ? 6 : lay.npool;
  caps.depth = 8 * caps.consumers;
  caps.csirs_enabled = caps.recovery_enabled = caps.gw_enabled = 1;
  nr_cell_mgr_cfg_t mc;
  memset(&mc, 0, sizeof(mc));
  mc.grid_n_rb = p.n_rb > 0 ? p.n_rb : 106; /* OAI default -r */
  mc.pass1_ssb = ssb > 0 ? ssb : 516;      /* OAI default --ssb */
  mc.pass1_k_ssb = 0;                      /* pass 2 from the log is not wired in A8 */
  const nr_cell_life_ops_t ops = {.prealloc = mpci_cell_prealloc, .start = mpci_cell_start, .retire = mpci_cell_retire, .retune = mpci_cell_retune,
                                  .log = mpci_log};
  const int n = nr_cell_life_plan(&g_cells, &mc, &caps, &ops, &p, &g_inv_watch.inv, 1000.0);
  if (n <= 1)
    return 1;
  const int added = nr_cell_life_admit(&g_cells, &g_inv_watch.inv, mpci_now_ns());
  return added > 1 ? added : 1;
}

/* After the configuration is loaded: the published-ring input is the only multi-cell input A8 wires. */
static void mpci_check_input(void)
{
  if (g_ncells <= 1)
    return;
  char *devname = NULL;
  paramdef_t dp = {"name", NULL, 0, .strptr = &devname, .defstrval = "", TYPE_STRING, 0};
  config_get(config_get_if(), &dp, 1, "device");
  const bool rings = devname && !strcmp(devname, "shm_iq");
  if (!rings || IS_SOFTMODEM_RFSIM || getenv("ISAC_PASSIVE_REPLAY_INPUT") || getenv("ISAC_PASSIVE_REPLAY_CAPTURE")) {
    LOG_W(PHY, "SENSING: MPCI_INPUT device=%s: cells beyond the started one need published rings (device shm_iq); the in-process channelizer feed "
          "from a radio is not wired (A8 open item): serving the started cell only (%d admitted)\n", devname && *devname ? devname : "default", g_ncells);
    g_ncells = 1;
  }
}

/* cell k > 0: its cell parameters and its ring-consumer RU (before the cell/frame-parameter loop of main) */
static void mpci_add_cell(int k)
{
  const nr_mgr_slot_t *s = nr_cell_mgr_slot(&g_cells.mgr, k);
  AssertFatal(s && s->active, "MPCI cell %d has no admitted slot\n", k);
  nrUE_cell_params_t c = *nrue_get_cell(0);
  c.rf_frequency = (uint64_t)llround(s->desc.centre_hz);
  if (s->desc.n_rb > 0)
    c.N_RB_DL = s->desc.n_rb;
  if (s->desc.scs_khz >= 15)
    c.numerology = (int)lround(log2(s->desc.scs_khz / 15.0));
  c.ssb_start = s->ssb;
  /* the cell's own SSB position in its grid, from the inventory geometry (inverse of nr_mgr_grid_centre_hz: centre = ssb_hz + (6 n_rb - 120 - ssb) scs);
   * the manager's value (the started --ssb) is kept when the inventory does not give the SSB frequency */
  if (s->desc.ssb_hz > 0 && c.N_RB_DL > 0) {
    const double scs = 15e3 * (double)(1 << c.numerology);
    const long ssb = lround(6.0 * c.N_RB_DL - 120.0 - (s->desc.centre_hz - s->desc.ssb_hz) / scs);
    if (ssb >= 0 && ssb + 240 <= 12L * c.N_RB_DL)
      c.ssb_start = (int)ssb;
  }
  nrUE_RU_params_t ru = *nrue_get_ru(0);
  char ring[NR_INV_ID_MAX + 8];
  snprintf(ring, sizeof(ring), "nrcell_%s", s->id);
  ru.sdr_addrs = strdup(ring);
  if (s->desc.n_rx > 0 && (unsigned)s->desc.n_rx < ru.nb_rx)
    ru.nb_rx = (unsigned)s->desc.n_rx;
  const int id = nrue_add_cell_ru(&c, &ru);
  AssertFatal(id == k, "MPCI cell %d: RU slot %d\n", k, id);
  LOG_I(PHY, "SENSING: MPCI_CELL_SLOT cell=%d id=%s pci=%d centre_hz=%.0f n_rb=%d ssb=%d cfo_hz=%.0f nb_rx=%u ring=/%s\n", k, s->id, s->desc.pci,
        s->desc.centre_hz, c.N_RB_DL, c.ssb_start, s->desc.cfo_hz, ru.nb_rx, ring);
}

/* per-cell uectx/1 writer of cell k > 0 (cell 0 opens its own in main, unchanged), opened on cell k's bound thread */
static void mpci_open_uectx(int k)
{
  if (!g_uectx_path[0])
    return;
  char p[PATH_MAX + 48];
  nr_cell_path(k, g_uectx_path, p, sizeof(p));
  if (nr_ue_ctx_open(p, 16384, 5.0))
    nr_cfg_epoch_subscribe(nr_ue_ctx_on_epoch);
  else
    LOG_W(PHY, "SENSING: UE context writer/state could not be opened (cell %d)\n", k);
}

/* prealloc hook (bound to cell k, before the RX fence): every per-cell heap object, the per-cell module starts that allocate */
static int mpci_cell_prealloc(void *ctx, int k, const nr_mgr_slot_t *slot)
{
  (void)ctx;
  (void)slot;
  PHY_VARS_NR_UE *ue = nrPHY_vars_UE_g[k][0];
  nr_passive_reconf_rules_cell_prealloc(k);
  nr_passive_ue_ctx_cell_prealloc(k);
  nr_pdcch_cell_prealloc(k);
  nr_pdsch_cell_prealloc(k);
  nr_ul_cell_prealloc_all(k);
  if (k > 0) {
    nr_pdcch_blind_monitor_init(); /* cell 0 ran it at start-up (unchanged) */
    if (IS_PASSIVE_RX_MODE(get_softmodem_params()))
      mpci_open_uectx(k);
  }
  if (k > 0)
    nr_pdcch_nid_gpu_start(ue->frame_parms.N_RB_DL); /* per-cell nID sweep on the cell's own grid (cell 0 started in main, unchanged) */
  nr_csirs_disc_pipeline_init(&ue->frame_parms);
  if (nr_csirs_disc_cell_prealloc(k, &ue->frame_parms, ue->frame_parms.nb_antennas_rx) != 0)
    LOG_W(PHY, "SENSING: CSIRS_DISC worker could not start; slot discovery feed disabled\n");
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_pusch_passive_pipeline_enabled()) {
    const nr_pdcch_blind_monitor_cfg_t *ul_cfg = nr_pdcch_blind_monitor_get_cfg();
    if (ul_cfg && ul_cfg->ul_pusch_decode) {
      if (!nr_pusch_passive_queue_start(ue, ul_cfg->ul_queue_depth > 0 ? ul_cfg->ul_queue_depth : 8, 1, ul_cfg->ul_thread_core))
        LOG_W(PHY, "SENSING: UL pipeline prewarm failed; grants will be dropped and counted\n");
    }
  }
  return 0;
}

/* start hook (bound to cell k): the cell's RT slot thread and stats thread */
static int mpci_cell_start(void *ctx, int k, const nr_mgr_slot_t *slot, int reactivation)
{
  (void)ctx;
  if (reactivation) {
    LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s back in the inventory: its slot thread was kept running (no run-time teardown, A8 open item)\n", k,
          slot ? slot->id : "-");
    return 0;
  }
  LOG_I(PHY, "Intializing UE Threads for instance %d ...\n", k);
  init_NR_UE_threads(nrPHY_vars_UE_g[k][0]);
  return 0;
}
static void mpci_cell_retire(void *ctx, int k, const nr_mgr_slot_t *slot)
{
  (void)ctx;
  LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retired by the inventory: its slot thread keeps running (run-time teardown is an A8 open item)\n", k,
        slot ? slot->id : "-");
}
static void mpci_cell_retune(void *ctx, int k, const nr_cell_mgr_event_t *ev)
{
  (void)ctx;
  LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retune requested centre_hz %.0f -> %.0f cfo_hz %.0f -> %.0f: not applied at run time (A8 open item)\n", k,
        ev->id, ev->prev_centre_hz, ev->desc.centre_hz, ev->prev_cfo_hz, ev->desc.cfo_hz);
}
/* the inventory control thread (one owner of the manager): re-reads cells.json once a second, hysteresis tick */
static void *mpci_control_thread(void *arg)
{
  (void)arg;
  while (!oai_exit && !nr_shutdown_requested()) {
    char err[200] = "";
    const int r = nr_cell_inventory_poll(&g_inv_watch, err, sizeof(err));
    if (r < 0)
      LOG_W(PHY, "SENSING: MPCI_INVENTORY rejected update: %s (previous inventory kept)\n", err);
    nr_cell_life_poll(&g_cells, r == 1 ? &g_inv_watch.inv : NULL, mpci_now_ns());
    for (int i = 0; i < 10 && !oai_exit; i++)
      usleep(100000);
  }
  return NULL;
}
/* run `fn` once per served cell with the calling thread bound to it (highest cell first), the binding restored; N = 1: once, cell 0 */
static void mpci_for_each_cell(void (*fn)(int cell))
{
  const int prev = nr_cell_idx();
  for (int k = g_ncells - 1; k >= 0; k--) {
    nr_cell_bind(k);
    fn(k);
  }
  nr_cell_bind(prev);
}
static void mpci_stop_nid(int k) { (void)k; nr_pdcch_nid_gpu_stop(); }
static void mpci_stop_pusch(int k) { (void)k; if (nr_pusch_passive_pipeline_enabled()) nr_pusch_passive_queue_stop(); }
/* shutdown of one cell's consumers (bound to it): the single-cell sequence of before */
static void mpci_stop_cell_queues(int k)
{
  (void)k;
  nr_shutdown_step("passive_pdcch_stop_begin");
  nr_pdcch_blind_monitor_rt_final_stats();
  nr_pdcch_passive_queue_stop();
  nr_shutdown_step("passive_pdsch_gpu_fep_stop_begin");
  nr_pdsch_passive_queue_stop();
  nr_shutdown_step("passive_pusch_stop_begin");
  nr_pusch_passive_queue_stop();
  nr_pdcch_uss_tracker_stop();
}
static void mpci_close_cell_outputs(int k)
{
  (void)k;
  nr_passive_obs_close(); // drain + flush the per-grant observation file (no-op when not open)
  const bool uectx_was_open = nr_ue_ctx_enabled();
  nr_ue_ctx_close();
  if (uectx_was_open) {
    uint64_t events = 0, lines = 0, dropped = 0;
    nr_ue_ctx_stats(&events, &lines, &dropped);
    LOG_I(PHY, "SENSING: UECTX_STATS events=%llu written=%llu dropped=%llu\n",
          (unsigned long long)events, (unsigned long long)lines, (unsigned long long)dropped);
  }
}

/* nrue_shutdown_notify: notify callback of the shutdown subsystem: sets oai_exit and unblocks the ITTI tasks waiting on the message queue. Registered in main() through nr_shutdown_init(). */
static void nrue_shutdown_notify(void)
{
  oai_exit = 1;
  /* raw-IQ EOF takes nr_shutdown_finish()'s _exit path, so main's normal
   * consumer teardown is not reached. Join the prewarmed UL worker here (every cell's: A8). */
  mpci_for_each_cell(mpci_stop_pusch);
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
/* trigger_deregistration: SIGINT/SIGTERM handler of the active SA UE: the first request sends a NAS deregistration to every UE instance, the second request (or a 5-second alarm) shuts down unconditionally through trigger_stop().
 *   Passive RX has nothing to deregister and does not use this handler. Installed by main() when deregister_first is set.
 */
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

/* nrue_ru_start_thread: thread body that starts the radio (nrue_ru_start) and returns its result; created by main() at real-time priority on the planned UHD core. */
void *nrue_ru_start_thread(void *arg)
{
  (void)arg;
  return (void *)(intptr_t)nrue_ru_start();
}

int NB_UE_INST = 1;
configmodule_interface_t *uniqCfg = NULL;
nrLDPC_coding_interface_t nrLDPC_coding_interface = {0};

/* gpu_platform_log: logger installed by main() into the GPU platform (nr_gpu_set_logger): forwards its messages to LOG_W or LOG_I with the PHY component. */
static void gpu_platform_log(int warning, const char *text)
{
  if (warning) LOG_W(PHY, "%s\n", text);
  else LOG_I(PHY, "%s\n", text);
}

/* main: softmodem entry point for the NR UE and the passive receiver: starts the shutdown subsystem, loads the configuration, parses the options, and initialises the sensing, monitor, CPU plan, ITTI tasks, PDCP/NAS and cell parameters.
 *   Then starts the time manager, the GPU warm-up and the discovery pipelines, locks memory, starts the sensing and PRACH services and the radio thread, and waits for the ITTI end signal.
 *   Shutdown sequence: request, wait for the radio release, stop the passive queues and the discovery pipelines, close the outputs, join the ITTI tasks and services, then nr_shutdown_finish(). Offline replay (ISAC_PASSIVE_REPLAY_INPUT or CAPTURE) exits before any radio start-up.
 */
/* plan_usrp_ring: host-RAM budget for the USRP decoupled RX ring (ISAC_USRP_RING_MS, ~1 GB at 500 ms x 4 ch x 122.88 MS/s). Reserved before every other module
 *   (it is what keeps the X410 stream alive): up to 50 % of the headroom, floor 100 ms; the USRP driver reads the planned duration (nr_host_membudget_usrp_ring_ms).
 *   Below the floor the receiver refuses to start (exit 7) instead of an OOM kill during RX. No-op without the ring, on rfsim and on replays. Caller: main(). */
static void plan_usrp_ring(const NR_DL_FRAME_PARMS *fp)
{
  const char *e = getenv("ISAC_USRP_RING_MS");
  const int ms = e ? atoi(e) : 0;
  if (ms <= 0 || ms > 5000 || IS_SOFTMODEM_RFSIM || getenv("ISAC_PASSIVE_REPLAY_INPUT") || getenv("ISAC_PASSIVE_REPLAY_CAPTURE"))
    return;
  const size_t per_ms = (size_t)fp->samples_per_subframe * fp->nb_antennas_rx * sizeof(int32_t); /* one subframe = 1 ms */
  const long lo = ms < 100 ? ms : 100;
  const long got = nr_host_membudget_size(NR_HOST_MEM_USRP, 50, 0, per_ms, lo, ms);
  if (!got) {
    char hm[768];
    nr_host_membudget_format(hm, sizeof(hm));
    LOG_E(HW, "SENSING: HOST_MEM REFUSE: the USRP ring floor (%ld ms = %zu MB) does not fit the host budget (%s); free host memory or lower ISAC_USRP_RING_MS\n", lo,
          ((size_t)lo * per_ms) >> 20, hm);
    printf("SENSING: HOST_MEM PLAN verdict=REFUSE usrp_ring_floor_mb=%zu %s\n", ((size_t)lo * per_ms) >> 20, hm);
    fflush(stdout);
    logTerm();
    _exit(7);
  }
  nr_host_membudget_reserve(NR_HOST_MEM_USRP, (size_t)got * per_ms);
  nr_host_membudget_set_usrp_ring_ms((int)got);
  if (got != ms)
    LOG_W(HW, "SENSING: HOST_MEM USRP ring %d -> %ld ms (%zu MB, host-RAM budget)\n", ms, got, ((size_t)got * per_ms) >> 20);
  else
    LOG_I(HW, "SENSING: HOST_MEM USRP ring %d ms reserved (%zu MB)\n", ms, ((size_t)got * per_ms) >> 20);
}

int main(int argc, char **argv)
{
  /* t26yE site defaults of the passive receiver (never for the phy-test UE): visible before logInit() and the float lane. --passive-rx is on the command line on air. */
  bool passive_rx = false, rfsim_arg = false;
  for (int i = 1; i < argc; i++) {
    passive_rx |= !strcmp(argv[i], "--passive-rx");
    rfsim_arg |= !strcmp(argv[i], "--rfsim");
  }
  if (passive_rx) {
    /* Core plan from the host topology (nr_core_layout.h): one CORE_PLAN line, then the planned RT / pool / lane / radio-IO /
     * dashboard cores are exported for every unset ISAC_* core key. Must run before the float lane changes any affinity.
     * The radio IO cores are planned only when a hardware radio is used (not --rfsim, not an offline replay). */
    const bool radio = !rfsim_arg && !getenv("ISAC_PASSIVE_REPLAY_INPUT") && !getenv("ISAC_PASSIVE_REPLAY_CAPTURE");
    /* multi-PCI (A8): the cells to serve (inventory + admission) before the core plan, which gives each one its RT reader. No inventory: 1, and
     * nr_core_layout_init_n(.., 1) is nr_core_layout_init (byte-identical CORE_PLAN line). */
    g_ncells = mpci_plan_cells(argc, argv, radio);
    nr_core_layout_init_n(true, radio, g_ncells);
    setenv("ISAC_LOG_ASYNC", "1", 0); setenv("ISAC_CSIRS_DISC", "1", 0); setenv("ISAC_GPU_BUDGET", "1", 0); setenv("ISAC_RECONF", "1", 0); /* read by shared code (LDPC pool, GPU budget) */
    /* USRP driver (hardware radio shared with the gNB): site defaults of the receiver */
    setenv("ISAC_RX_RECOVER", "1", 0); setenv("ISAC_USRP_START_TIMEOUT_MS", "2200", 0);
    setenv("ISAC_USRP_RING_ZEROCOPY", "1", 0); setenv("ISAC_USRP_CATCHUP_X", "1.05", 0); setenv("ISAC_USRP_CHPOW_S", "1", 0); setenv("ISAC_USRP_NO_TX", "1", 0);
    /* CUDA driver setting, derived (not a receiver flag): hardware work queues per context. The GPU pass owns NR_GPU_PASS_RING instances of 3 streams each (real-time s, s2 and the
     * low-priority probe twin s_lo; nr_gpu_pass_graph.cu alloc_inst), plus the discovery / PDCCH / LDPC streams. With fewer queues than streams, independent streams are serialised on
     * a shared queue (false dependency, CUDA default 8); the queue count is the next power of two >= the pass streams, clamped to the CUDA maximum of 32 (21 pass streams -> 32).
     * Set before the first CUDA call; an operator-set value is kept. */
    {
      int conn = 8;
      while (conn < 3 * NR_GPU_PASS_RING && conn < 32) conn <<= 1;
      char cs[8];
      snprintf(cs, sizeof(cs), "%d", conn);
      setenv("CUDA_DEVICE_MAX_CONNECTIONS", cs, 0);
    }
    if (!nr_core_layout_active()) { /* ISAC_CORE_PLAN=0: the legacy DGX site cores */
      setenv("ISAC_UHD_CTRL_CORES", "15", 0);
      setenv("ISAC_FLOAT_CORES", "10-14", 0);
    }
  }
  nr_core_float_apply(); /* ISAC_FLOAT_CORES: unpinned threads (and everything they start) stay in one lane */
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
  nr_auto_acquire_on = nr_auto_acquire_trigger(IS_PASSIVE_RX_MODE(get_softmodem_params()), ssb_given, fo_given);
  CONFIG_CLEARRTFLAG(CONFIG_NOEXITONHELP);
  mpci_check_input(); /* A8: N > 1 needs published cell rings; otherwise the started cell only */

  // ISAC / passive-radar sensing pipeline: parse the [sensing] config section (no-op if disabled).
  nr_isac_init();
  // UE-agnostic CSI-RS sensing monitors (cell-common / other-UE resources); no-op if unset.
  nr_csirs_monitor_init();
  // Blind PDCCH/DCI-1_1 monitor (TOTAL_PASSIVE_UE_HANDOVER.md Phase 3); no-op if unset.
  nr_pdcch_blind_monitor_init();
  const bool pas = IS_PASSIVE_RX_MODE(get_softmodem_params());
  if (pas) { /* always-on dashboard (detached, non-root, own spare core, nice 19); its failure never affects the receiver */
    nr_dashboard_cfg_t dc = {.metrics_path = getenv("ISAC_METRICS_PATH"), .core = -1};
    char derr[200] = "", durl[64] = "", drun[PATH_MAX] = "";
    if (nr_dashboard_start(&dc, derr, sizeof(derr), durl, sizeof(durl), drun, sizeof(drun)) == 0)
      LOG_A(PHY, "SENSING: DASHBOARD %s (run %s)\n", durl, drun);
    else
      LOG_W(PHY, "SENSING: DASHBOARD not started: %s\n", derr);
  }
  if (pas) { /* UL PUSCH chain + solver are on in passive mode */
    /* uectx/1 JSONL (always on), next to run.log: the directory of /proc/self/fd/1 when stdout is a regular file, else the default runs
     * root (the dashboard's discovery, common/utils/nr_dashboard.c); neither usable -> the records are discarded. */
    char upath[PATH_MAX + 32] = "", ulog[PATH_MAX] = "";
    struct stat ust;
    const ssize_t ul = readlink("/proc/self/fd/1", ulog, sizeof(ulog) - 1);
    ulog[ul > 0 ? ul : 0] = 0;
    if (ulog[0] == '/' && !stat(ulog, &ust) && S_ISREG(ust.st_mode))
      snprintf(upath, sizeof(upath), "%s/uectx.jsonl", dirname(ulog));
    else {
      const char *home = getenv("HOME");
      char root[PATH_MAX];
      snprintf(root, sizeof(root), "%s/NICOLA/ota_runs", home ? home : "/tmp");
      if (!stat(root, &ust) && S_ISDIR(ust.st_mode))
        snprintf(upath, sizeof(upath), "%s/uectx_%d.jsonl", root, (int)getpid());
    }
    snprintf(g_uectx_path, sizeof(g_uectx_path), "%s", upath); /* A8: cells k > 0 derive theirs with nr_cell_path */
    if (nr_ue_ctx_open(upath, 16384, 5.0))
      nr_cfg_epoch_subscribe(nr_ue_ctx_on_epoch);
    else
      LOG_W(PHY, "SENSING: UE context writer/state could not be opened\n");
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

  NB_UE_INST = g_ncells; /* A8: one UE instance per served cell (instance k = cell k); 1 = the single-cell receiver */
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

  nrue_set_ru_params(uniqCfg);
  nrue_set_cell_params(uniqCfg);
  for (int k = 1; k < g_ncells; k++)
    mpci_add_cell(k); /* A8: cell k and its ring-consumer RU */

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
      if (inst > 0) /* A8: cell k's carrier offset seed comes from the inventory (cell 0 keeps --initial-fo) */
        UE_CC->initial_fo = nr_cell_mgr_slot(&g_cells.mgr, inst)->desc.cfo_hz;
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
      if (g_ncells > 1) { /* A8: the actors of UE instance k run cell k's PBCH / PDSCH / MAC work */
        nr_ue_bind_actor(&UE_CC->sync_actor, inst);
        for (int i = 0; i < get_nrUE_params()->num_dl_actors; i++)
          nr_ue_bind_actor(&UE_CC->dl_actors[i], inst);
        for (int i = 0; i < get_nrUE_params()->num_ul_actors; i++)
          nr_ue_bind_actor(&UE_CC->ul_actors[i], inst);
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
  plan_usrp_ring(&disc_ue->frame_parms); /* host-RAM budget: the USRP ring is reserved first (the radio needs it), before any module sizes itself */
  nr_dl_gpu_warmup(disc_ue);
  nr_pdcch_nid_gpu_start(disc_ue->frame_parms.N_RB_DL);
  if (g_ncells > 1) {
    /* A8: per cell, bound to it: heap objects, CSI-RS discovery, PDCCH monitor / uectx (k > 0), PUSCH queue; then the cell count (LOG tag) */
    nr_cell_life_prepare(&g_cells);
  } else {
  nr_csirs_disc_pipeline_init(&disc_ue->frame_parms);
  if (nr_csirs_disc_start(&disc_ue->frame_parms, disc_ue->frame_parms.nb_antennas_rx) != 0)
    LOG_W(PHY, "SENSING: CSIRS_DISC worker could not start; slot discovery feed disabled\n");
  }
  /* U1 owns exactly one bounded consumer/context, warmed before the RX seal.
   * A failed start leaves admission closed; the RT path only counts drops. */
  if (g_ncells == 1 && IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_pusch_passive_pipeline_enabled()) {
    const nr_pdcch_blind_monitor_cfg_t *ul_cfg = nr_pdcch_blind_monitor_get_cfg();
    if (ul_cfg && ul_cfg->ul_pusch_decode) {
      if (!nr_pusch_passive_queue_start(disc_ue, ul_cfg->ul_queue_depth > 0 ? ul_cfg->ul_queue_depth : 8,
                                        1, ul_cfg->ul_thread_core))
        LOG_W(PHY, "SENSING: UL pipeline prewarm failed; grants will be dropped and counted\n");
    }
  }

  /* The GrantWork pool is sized last from what the host budget has left; a host that cannot hold the fixed modules refuses here, before the radio. */
  const int hostmem_refused = nr_dl_gw_pool_warmup(disc_ue) != 0;
  if (hostmem_refused || getenv("ISAC_HOST_MEM_PLAN_ONLY")) {
    /* ISAC_HOST_MEM_PLAN_ONLY=1 (launcher preflight, tests/passive_rx/hostmem_preflight.sh): the receiver's own plan, then exit (0 = fits, 7 = refused); no radio */
    char hm[768];
    nr_host_membudget_format(hm, sizeof(hm));
    printf("SENSING: HOST_MEM PLAN verdict=%s %s\n", hostmem_refused ? "REFUSE" : "OK", hm);
    fflush(stdout);
    logTerm();
    _exit(hostmem_refused ? 7 : 0);
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
  if (g_ncells > 1)
    nr_cell_alloc_fence(); /* A8: no cell object is allocated after this (nr_cell.h); N = 1 keeps the lazy first-touch of before */
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

  if (g_ncells > 1) {
    nr_cell_life_start(&g_cells); /* A8: each cell's RT slot thread, created bound to its cell */
    pthread_t mpci_ctl;
    if (pthread_create(&mpci_ctl, NULL, mpci_control_thread, NULL) == 0)
      pthread_detach(mpci_ctl);
  } else
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
  mpci_for_each_cell(mpci_stop_cell_queues); /* A8: every cell's consumers, bound to the cell; N = 1: cell 0, the sequence of before */
  nr_shutdown_step("discovery_stop_begin");
  mpci_for_each_cell(mpci_stop_nid); /* every cell's nID sweep, bound to the cell; N = 1: cell 0, as before */
  if (g_ncells > 1)
    for (int k = g_ncells - 1; k >= 0; k--)
      nr_csirs_disc_cell_release(k);
  else
  nr_csirs_disc_stop();
  nr_shutdown_step("discovery_stopped");
  abortTpool(&nrUE_params.Tpool);
  nr_shutdown_step("thread_pool_stopped");

  // All slot producers have joined. Drain the PRACH worker before closing records.
  nr_prach_passive_flush();
  nr_srs_passive_stop();
  nr_isac_stop();
  nr_shutdown_step("sensing_stopped");

  mpci_for_each_cell(mpci_close_cell_outputs); /* A8: per cell; N = 1: cell 0, the sequence of before */

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
