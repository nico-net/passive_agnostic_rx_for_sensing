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
#include "PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.h" /* MPCI-DBG3: per-cell pass registration */
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
#include "common/utils/nr_core_pool.h"
#include "common/utils/nr_cell.h"
#include "common/utils/nr_cell_dash.h" /* MPCI-DASH P2 CELL_GPU / P7 CELL_BUDGET formatters (lines only when nr_cell_count() > 1) */
#include "PHY/TOOLS/nr_gpu_memstat.h"
#include "common/utils/nr_core_pool.h" /* nr_pool_site / nr_pool_size (CELL_BUDGET tokens_fair) */
#include "PHY/NR_UE_TRANSPORT/nr_passive_metrics.h" /* nr_passive_metrics_pci_of (cell= pci= of the per-cell lines) */
#include "PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_cell.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_ul_cell_prealloc.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_disc_cell.h"
#include <math.h>
#include "radio/COMMON/iq_feed.h"
#include "radio/COMMON/iq_span.h"
#include "radio/COMMON/iq_live.h"
#include "radio/COMMON/ru_park.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_api.h"
#include <stdatomic.h>

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
 * Input (iq_feed_input_mode): published rings (configured device "shm_iq": an out-of-process publisher, e.g. tests/passive_rx/mpci/mpci_file_pub), or
 * (G6) a radio: ONE wideband capture whose centre and rate the planner derives from the admitted cells and the device's rate set (radio/COMMON/iq_span.h),
 * read into an anonymous wideband ring, channelized in process (iq_chan) into one ring "nrcell_<id>" per cell (cell 0 too) - see the G6 block below.
 * rfsim / offline replay with several cells: MPCI_INPUT, the started cell only. N = 1 keeps the direct radio path (no channelizer hop, no G6 code runs). */
static nr_cell_life_t g_cells;
static int g_ncells = 1; /* 1: the single-cell receiver; every N = 1 path below is the code of before */
static iq_live_t g_live; /* MPCI-LIVE: the live state of the slots (control thread) */
static int g_live_on;   /* ... initialised: the control thread runs */
static int g_nplan = 1;  /* MPCI-LIVE: slots that hold a cell at start (0..g_nplan-1); the slots above are spares: prepared, no cell, ring paused */
static nr_cell_inventory_watch_t g_inv_watch;
static char g_uectx_path[PATH_MAX + 32]; /* cell 0's uectx path: cells k > 0 use nr_cell_path(k, ...) */

/* ================================================================ G6: wideband capture through the in-process channelizer ================================
 * Only when several cells are admitted and the input is a radio (g_feed). Order in main(): RU 0 becomes a ring consumer like the other cells
 * (mpci_feed_rings_for_rus); after the per-cell frame parameters: the planner's provisional capture (no device rate set yet), the radio is loaded on an RT
 * thread (as the RU start) and its driver re-plans with the device's own rate set (nr_radio_wide_plan, called back by usrp_lib); the radio ring plan
 * (host RAM) at the capture rate, then the wideband ring and one channelizer entry per cell (served: running; outside the capture: created paused and
 * logged unserved); after the RX fence: memory locked, radio started, reader + channelizer threads; then the cells' ring RUs start as in A8.
 * At run time: a retired cell's entry is paused (the radio never stops), a re-admitted cell is resumed when it still fits the capture (no radio retune
 * in G6: a cell outside it is logged and stays unserved); MPCI_FEED every 10 s (radio reads/gaps/frozen stamps, channelizer overruns and threads,
 * per-cell ring overruns / drops / lag). */
#define MPCI_FEED_RING "nrcell" /* cell rings are "nrcell_<inventory id>": the names A8's ring RUs read */
static int g_feed;                                   /* 1: G6 path active */
static iq_span_cell_t g_feed_cells[NR_CELL_MAX];
static char g_feed_ids[NR_CELL_MAX][NR_INV_ID_MAX];
static iq_span_dev_t g_feed_dev;                     /* the device's rate set as its driver reported it (n_ranges 0: not reported) */
static iq_span_plan_t g_feed_plan;
static int g_feed_wanted;                            /* a plan is pending for the radio driver's callback */
static iq_feed_t *g_feed_obj;
static int g_feed_entry[NR_CELL_MAX];

/* the radio driver's callbacks (usrp_lib usrp_wide_plan, found with dlsym; exported by -rdynamic) */
__attribute__((visibility("default"))) int nr_radio_wide_wanted(void) { return g_feed_wanted; }
__attribute__((visibility("default"))) int nr_radio_wide_plan(openair0_config_t *cfg, const double *lo, const double *hi, const double *step, int n, double analog_bw_hz,
                                                             int max_rx)
{
  if (!g_feed_wanted || !cfg)
    return 0;
  iq_span_dev_t d;
  memset(&d, 0, sizeof(d));
  for (int i = 0; i < n && i < IQ_SPAN_MAX_RANGES; i++)
    d.range[d.n_ranges++] = (iq_rate_range_t){lo[i], hi[i], step[i]};
  d.analog_bw_hz = analog_bw_hz;
  d.max_rx = max_rx > 0 ? (unsigned)max_rx : 0;
  if (d.n_ranges == 0) {
    LOG_E(PHY, "SENSING: MPCI_SPAN the radio reports no usable rate: the capture cannot be planned\n");
    return -1;
  }
  iq_span_plan_t p;
  if (iq_span_plan(&d, g_feed_cells, g_nplan, &p) <= 0) {
    LOG_E(PHY, "SENSING: MPCI_SPAN the radio cannot capture the started cell (reason %s)\n", iq_span_reason_name(p.cell[0].reason));
    return -1;
  }
  g_feed_dev = d;
  g_feed_plan = p;
  cfg->sample_rate = p.rate;
  for (int c = 0; c < cfg->rx_num_channels && c < 8; c++)
    cfg->rx_freq[c] = p.centre_hz;
  return 1;
}

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
static int mpci_slot_fit(void *ctx, int slot, const nr_cell_desc_t *d);
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
  mc.slot_fit = mpci_slot_fit; /* MPCI-LIVE: a free slot carries a cell only when the cell has the slot's prepared grid */
  /* MPCI-LIVE: with a radio (several cells from one capture) spare slots are prepared with the cells (admission decides how many fit; they are admitted last); a
   * single-cell inventory never gets one. mpci_check_input() drops them again when the input turns out to be published rings. */
  const int n = nr_cell_life_plan_spare(&g_cells, &mc, &caps, &ops, &p, &g_inv_watch.inv, 1000.0, radio ? NR_CELL_LIFE_SPARE_DEFAULT : 0);
  if (n <= 1)
    return 1;
  const int added = nr_cell_life_admit(&g_cells, &g_inv_watch.inv, mpci_now_ns());
  int real = 0;
  for (int i = 0; i < added; i++)
    real += !nr_cell_life_is_spare(&g_cells, nr_cell_life_admitted(&g_cells, i));
  g_nplan = real > 0 ? real : 1;
  for (int i = 0; i < added; i++) /* the spares are admitted last: slots 0..g_nplan-1 hold the cells */
    AssertFatal(!nr_cell_life_is_spare(&g_cells, nr_cell_life_admitted(&g_cells, i)) || nr_cell_life_admitted(&g_cells, i) >= g_nplan, "MPCI spare slot below a cell slot\n");
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
  const int sim = IS_SOFTMODEM_RFSIM || getenv("ISAC_PASSIVE_REPLAY_INPUT") || getenv("ISAC_PASSIVE_REPLAY_CAPTURE");
  const iq_feed_mode_t m = iq_feed_input_mode(g_ncells, devname, sim);
  if (m == IQ_FEED_WIDEBAND) { /* G6 */
    g_feed = 1;
    LOG_I(PHY, "SENSING: MPCI_INPUT device=%s: %d cells from one wideband capture through the in-process channelizer\n", devname && *devname ? devname : "default",
          g_ncells);
  } else if (m != IQ_FEED_RINGS) {
    LOG_W(PHY, "SENSING: MPCI_INPUT device=%s: rfsim / offline replay input carries one cell: serving the started cell only (%d admitted)\n",
          devname && *devname ? devname : "default", g_ncells);
    g_ncells = 1;
  } else { /* published rings: nobody publishes a ring for a spare slot */
    const int dropped = nr_cell_life_drop_spares(&g_cells);
    if (dropped > 0) {
      g_ncells -= dropped;
      LOG_I(PHY, "SENSING: MPCI_LIFE %d spare slot(s) dropped: the input is published rings\n", dropped);
    }
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

/* G6: every cell's RU (cell 0's too) reads its ring "nrcell_<id>"; the configured radio becomes the capture device */
static void mpci_feed_rings_for_rus(void)
{
  if (!g_feed)
    return;
  const nr_mgr_slot_t *s0 = nr_cell_mgr_slot(&g_cells.mgr, 0);
  char ring[NR_INV_ID_MAX + 8];
  snprintf(ring, sizeof(ring), MPCI_FEED_RING "_%s", s0 && s0->id[0] ? s0->id : NR_CELL_LIFE_PRIMARY_ID);
  AssertFatal(nrue_ru_make_ring(0, ring) == 0, "MPCI feed: RU 0 cannot become a ring consumer\n");
}

typedef struct { int rc; } mpci_feed_thr_t;
static void *mpci_feed_load_thread(void *arg)
{
  ((mpci_feed_thr_t *)arg)->rc = nrue_feed_load();
  return NULL;
}
static void *mpci_feed_start_thread(void *arg)
{
  ((mpci_feed_thr_t *)arg)->rc = nrue_feed_start();
  return NULL;
}
/* run fn on a temporary RT thread placed like the RU start thread (UHD control threads inherit it) */
static int mpci_feed_on_rt(void *(*fn)(void *), const char *name)
{
  mpci_feed_thr_t r = {.rc = -1};
  pthread_t t;
  threadCreate(&t, fn, &r, (char *)name, nr_core_plan_cpu(NR_CP_UHD, 0, -1), OAI_PRIORITY_RT_MAX);
  pthread_join(t, NULL);
  return r.rc;
}
static void mpci_feed_fail(const char *why)
{
  LOG_E(HW, "SENSING: MPCI_FEED %s; receiver exiting before RX\n", why);
  nr_shutdown_request(EXIT_FAILURE);
  nr_shutdown_finish();
}

/* after the per-cell frame parameters: the cells as the planner sees them, the provisional capture, the radio loaded (its driver re-plans) */
static void mpci_feed_open(void)
{
  if (!g_feed)
    return;
  for (int k = 0; k < g_ncells; k++) {
    const NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(k);
    const nr_mgr_slot_t *sl = nr_cell_mgr_slot(&g_cells.mgr, k);
    snprintf(g_feed_ids[k], sizeof(g_feed_ids[k]), "%s", sl && sl->id[0] ? sl->id : NR_CELL_LIFE_PRIMARY_ID);
    g_feed_cells[k] = (iq_span_cell_t){.id = g_feed_ids[k], .centre_hz = (double)fp->dl_CarrierFreq, .n_rb = fp->N_RB_DL,
                                       .scs_hz = 15e3 * (double)(1 << fp->numerology_index), .rate = fp->samples_per_subframe * 1e3,
                                       .n_rx = (unsigned)fp->nb_antennas_rx};
  }
  /* provisional: any rate (a device whose driver does not report its rate set keeps this one; usrp_lib replaces it from the device) */
  const iq_span_dev_t any = {0};
  if (iq_span_plan(&any, g_feed_cells, g_nplan, &g_feed_plan) <= 0)
    mpci_feed_fail("the started cell has no capture geometry");
  openair0_config_t *cfg = nrue_feed_config(g_feed_plan.rate, g_feed_plan.centre_hz, (int)g_feed_plan.n_rx);
  if (!cfg)
    mpci_feed_fail("no radio parameters for the capture device");
  g_feed_wanted = 1;
  const int rc = mpci_feed_on_rt(mpci_feed_load_thread, "feed_load");
  g_feed_wanted = 0;
  if (rc != 0)
    mpci_feed_fail("the capture device could not be loaded");
  if (fabs(nrue_feed_cfg()->sample_rate - g_feed_plan.rate) > 1.0)
    mpci_feed_fail("the device did not keep the planned capture rate");
  char line[1536];
  iq_span_format(&g_feed_plan, g_feed_cells, line, sizeof(line));
  LOG_I(PHY, "SENSING: %s device_rates=%d analog_mhz=%.1f\n", line, g_feed_dev.n_ranges, g_feed_dev.analog_bw_hz / 1e6);
  for (int k = 0; k < g_nplan; k++)
    if (!g_feed_plan.cell[k].served)
      LOG_W(PHY, "SENSING: MPCI_SPAN cell=%d id=%s unserved reason=%s: outside this capture (a run-time change may re-plan it, MPCI_LIFE); its slot waits on a paused ring\n", k, g_feed_ids[k],
            iq_span_reason_name(g_feed_plan.cell[k].reason));
  for (int k = g_nplan; k < g_ncells; k++)
    LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s spare slot prepared (grid of the started cell, ring paused): a cell added at run time takes it\n", k, g_feed_ids[k]);
}

/* the bytes per ms of the radio's own decoupled ring at the capture (plan_usrp_ring) */
static size_t mpci_feed_usrp_per_ms(void) { return (size_t)llround(g_feed_plan.rate / 1000.0) * g_feed_plan.n_rx * sizeof(int32_t); }

/* the wideband ring (host-RAM plan: half of the headroom left after the radio ring, at most the 1 s target, never below the 100 ms floor) and the cell entries */
static void mpci_feed_rings(void)
{
  if (!g_feed)
    return;
  const size_t hr = nr_host_membudget_headroom();
  const size_t share = hr == SIZE_MAX ? SIZE_MAX : hr / 2;
  const uint64_t cap = shm_iq_ring_plan_capacity(share, g_feed_plan.rate, g_feed_plan.n_rx, 1, SHM_IQ_RING_WIDE_TARGET_MS, SHM_IQ_RING_WIDE_FLOOR_MS);
  if (!cap) {
    char hm[768];
    nr_host_membudget_format(hm, sizeof(hm));
    LOG_E(HW, "SENSING: HOST_MEM REFUSE: the wideband ring floor (%d ms at %.2f MS/s x %u) does not fit the host budget (%s)\n", SHM_IQ_RING_WIDE_FLOOR_MS,
          g_feed_plan.rate / 1e6, g_feed_plan.n_rx, hm);
    printf("SENSING: HOST_MEM PLAN verdict=REFUSE wide_ring %s\n", hm);
    fflush(stdout);
    logTerm();
    _exit(7);
  }
  const size_t wide = shm_iq_ring_bytes(g_feed_plan.n_rx, cap);
  const size_t narrow = share == SIZE_MAX ? 0 : share - wide;
  iq_chan_load_t load = {.in_rate = g_feed_plan.rate, .nch = g_feed_plan.n_rx};
  for (int k = 0; k < g_ncells && load.n_cells < IQ_CHAN_MAX_CELLS; k++)
    if (g_feed_plan.cell[k].served) {
      load.cell[load.n_cells].decim = g_feed_plan.cell[k].decim;
      load.cell[load.n_cells].shifted = g_feed_plan.cell[k].shift_hz != 0;
      load.cell[load.n_cells].passband_hz = g_feed_plan.cell[k].passband_hz;
      load.n_cells++;
    }
  static iq_chan_load_t s_load;
  s_load = load;
  const iq_feed_cfg_t fc = {.read = nrue_feed_read, .nch = g_feed_plan.n_rx, .rate = g_feed_plan.rate, .centre_hz = g_feed_plan.centre_hz, .name = MPCI_FEED_RING,
                            .wide_capacity = cap, .narrow_budget_bytes = narrow, .load_hint = &s_load, .radio_id = nrue_feed_cfg()->sdr_addrs,
                            .cancelled = nr_shutdown_requested};
  g_feed_obj = iq_feed_create(&fc);
  if (!g_feed_obj)
    mpci_feed_fail("the wideband ring / channelizer could not be created");
  size_t bytes = wide;
  for (int k = 0; k < g_ncells; k++) {
    const iq_span_cell_plan_t *c = &g_feed_plan.cell[k];
    int decim = c->decim;
    if (!c->served) { /* paused entry on the cell's own rate when the capture rate makes it, so its RU can attach and wait */
      const double q = g_feed_plan.rate / g_feed_cells[k].rate;
      decim = (int)lround(q);
      if (decim < 1 || decim > 16 || (decim & (decim - 1)) || fabs(q - decim) > 1e-9 * q)
        mpci_feed_fail("a cell's sample rate is not the capture rate / 2^j: its RU could not even wait on a ring (remove it from the inventory)");
    }
    const double pb = c->served ? c->passband_hz : iq_ddc_cell_passband(g_feed_cells[k].n_rb, g_feed_cells[k].scs_hz, g_feed_cells[k].rate, 0);
    g_feed_entry[k] = iq_feed_cell_add(g_feed_obj, g_feed_ids[k], g_feed_cells[k].centre_hz, decim, pb, !c->served);
    if (g_feed_entry[k] < 0)
      mpci_feed_fail("a cell ring does not fit the host-RAM plan");
    bytes += shm_iq_ring_bytes(g_feed_plan.n_rx, iq_feed_cell_ring(g_feed_obj, g_feed_entry[k])->hdr->capacity);
  }
  nr_host_membudget_commit(NR_HOST_MEM_OTHER, bytes, 1.0);
  LOG_I(HW, "SENSING: HOST_MEM wideband ring %llu samples (%.0f ms, %zu MB) + %d cell rings: %zu MB\n", (unsigned long long)cap, 1e3 * (double)cap / g_feed_plan.rate,
        wide >> 20, g_ncells, bytes >> 20);
}

static int mpci_chan_acquire(void *ctx, int blocking) { (void)ctx; return nr_pool_site_chan_acquire(blocking); }
static void mpci_chan_release(void *ctx) { (void)ctx; nr_pool_site_chan_release(); }
static void mpci_chan_demand(void *ctx, int tokens) { (void)ctx; nr_pool_site_chan_set_demand(tokens); }
static int mpci_chan_max(void *ctx) { (void)ctx; return nr_pool_site_chan_max(); }
/* after the RX fence: the radio stream, then the reader (core plan feed role, RT low) and the channelizer (k derived; workers lease class CHAN of the core pool) */
static void mpci_feed_start(void)
{
  if (!g_feed_obj)
    return;
  if (mpci_feed_on_rt(mpci_feed_start_thread, "feed_start") != 0)
    mpci_feed_fail("the capture device did not start");
  /* CHANPOOL: the reader is a real-time I/O thread on the core plan's feed role (nr_core_layout_init_nf); the channelizer's workers lease class CHAN of the
   * core pool per block (no CPU of their own, nothing pinned here). Without a plan (ISAC_CORE_PLAN=0) the reader floats on the home lane. */
  static const iq_chan_lease_t lease = {.acquire = mpci_chan_acquire, .release = mpci_chan_release, .demand = mpci_chan_demand, .max_tokens = mpci_chan_max};
  iq_chan_set_lease(&lease);
  const int reader = nr_core_layout_feed_cpu();
  /* MPCI-FEEDSTART: returns once every running cell ring holds its first block, so the RU start below (exclusive radio gate, shm_iq device_init waiting for
   * that block) never waits for the reader, which needs the same gate for every radio read */
  const int rc = iq_feed_start(g_feed_obj, reader, OAI_PRIORITY_RT_LOW, NULL, 0);
  if (rc == -ECANCELED)
    return; /* shutdown requested during start-up: the RU start sees it and the receiver finishes */
  if (rc == -EIO)
    mpci_feed_fail("the radio read failed before every cell ring had its first block");
  if (rc == -ETIMEDOUT)
    mpci_feed_fail("the cell rings had no first block within the start-up limit (the radio delivered nothing or the channelizer wrote nothing)");
  if (rc != 0)
    mpci_feed_fail("the feed threads did not start");
  iq_feed_stats_t st;
  iq_feed_stats(g_feed_obj, &st);
  LOG_I(PHY, "SENSING: MPCI_FEED started reader_cpu=%d chan=pool(max %d) threads=%d needed=%d capacity_msps=%.1f backend=%s live_ms=%.1f reads=%llu\n", reader,
        nr_pool_site_chan_max(), st.chan.threads, st.chan.threads_needed, st.chan.capacity_sps / 1e6, st.chan.backend, st.live_ms, (unsigned long long)st.reads);
}
static void mpci_feed_log(void)
{
  if (!g_feed_obj)
    return;
  char line[2048];
  iq_feed_format(g_feed_obj, line, sizeof(line));
  LOG_I(PHY, "SENSING: %s\n", line);
  if (g_live_on) { /* MPCI-LIVE: slot states, the capture in force, the slot threads parked outside the radio gate, the CHAN demand */
    char lv[600];
    iq_live_format(&g_live, lv, sizeof(lv));
    char pk[80] = "";
    for (int k = 0, o = 0; k < g_ncells && o < (int)sizeof(pk) - 8; k++)
      if (nrue_ru_ring_parked(k))
        o += snprintf(pk + o, sizeof(pk) - (size_t)o, "%s%d", o ? "," : "", k);
    LOG_I(PHY, "SENSING: %s parked=%s chan_demand=%d\n", lv, pk[0] ? pk : "-", iq_chan_demand(iq_feed_chan(g_feed_obj)));
  }
  if (nr_cell_count() > 1) { /* MPCI-DASH P3: one CHAN_CELL line per served cell (the feed exists on the multi-cell path only; the first call opens the windows) */
    iq_feed_cell_ref_t refs[NR_CELL_MAX];
    int n = 0;
    for (int k = 0; k < g_ncells && k < NR_CELL_MAX; k++) {
      const nr_mgr_slot_t *sl = nr_cell_mgr_slot(&g_cells.mgr, k);
      if (g_feed_entry[k] < 0 || !sl || !sl->active)
        continue;
      const int lp = nr_passive_metrics_pci_of(k);
      refs[n++] = (iq_feed_cell_ref_t){.id = g_feed_entry[k], .cell = k, .pci = lp >= 0 ? lp : sl->desc.pci};
    }
    char cl[IQ_CHAN_MAX_CELLS * 400];
    if (iq_feed_format_chan_cells(g_feed_obj, mpci_now_ns(), refs, n, cl, sizeof(cl)) > 0)
      for (char *sp = NULL, *ln = strtok_r(cl, "\n", &sp); ln; ln = strtok_r(NULL, "\n", &sp))
        LOG_I(PHY, "%s\n", ln);
  }
}
/* ================================================================ MPCI-LIVE: live cell changes (plan task A12) ===================================
 * The inventory control thread applies what the manager reports (retire after the hysteresis, an add into a slot prepared before the fence, a retune) through
 * radio/COMMON/iq_live.c, which decides and orders; the hooks below are its actions on the real objects. Everything exists since start-up (cells, rings, entries, UE
 * instances, threads): nothing is allocated here. Without a radio feed (published rings) there is no channelizer to move: the cell's ring belongs to its publisher; a
 * retire / add only logs and wakes the slot thread (a ring its publisher paused parks the thread outside the radio gate: ru_park). */

/* the cell of slot k as the inventory describes it, completed with the slot's grid: the UE's --ssb is derived from the SSB frequency exactly as at start */
static int mpci_cell_ssb(int k, const nr_cell_desc_t *d, int dflt)
{
  const NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(k);
  if (d->ssb_hz > 0 && fp->N_RB_DL > 0) {
    const double scs = 15e3 * (double)(1 << fp->numerology_index);
    const long ssb = lround(6.0 * fp->N_RB_DL - 120.0 - (d->centre_hz - d->ssb_hz) / scs);
    if (ssb >= 0 && ssb + 240 <= 12L * fp->N_RB_DL)
      return (int)ssb;
  }
  return dflt;
}
static iq_live_tune_t mpci_tune_of(int k, const nr_cell_desc_t *d, int ssb_dflt)
{
  return (iq_live_tune_t){.centre_hz = d->centre_hz, .cfo_hz = d->cfo_hz, .ssb = mpci_cell_ssb(k, d, ssb_dflt), .n_rb = d->n_rb,
                          .scs_hz = d->scs_khz > 0 ? d->scs_khz * 1e3 : 0, .n_rx = d->n_rx > 0 ? (unsigned)d->n_rx : 0};
}
/* the manager's slot choice: only a slot with the cell's grid (prepared before the fence, fixed since) can carry it */
static int mpci_slot_fit(void *ctx, int slot, const nr_cell_desc_t *d)
{
  (void)ctx;
  if (!g_live_on || slot <= 0 || slot >= g_ncells)
    return 1;
  const NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(slot);
  if (d->n_rb > 0 && d->n_rb != fp->N_RB_DL)
    return 0;
  if (d->scs_khz > 0 && fabs(d->scs_khz * 1e3 - 15e3 * (double)(1 << fp->numerology_index)) > 1.0)
    return 0;
  if (d->n_rx > 0 && d->n_rx > (int)fp->nb_antennas_rx)
    return 0;
  return 1;
}

static void mpci_live_log(void *ctx, const char *line)
{
  (void)ctx;
  LOG_I(PHY, "SENSING: %s\n", line);
}
static int mpci_live_pause(void *ctx, int k)
{
  (void)ctx;
  if (!g_feed_obj || g_feed_entry[k] < 0)
    return 0;
  const int rc = iq_feed_cell_pause(g_feed_obj, g_feed_entry[k]);
  LOG_I(PHY, "SENSING: MPCI_FEED cell=%d id=%s paused (radio not stopped) rc=%d\n", k, g_feed_ids[k], rc);
  return rc;
}
static int mpci_live_resume(void *ctx, int k)
{
  (void)ctx;
  if (!g_feed_obj || g_feed_entry[k] < 0)
    return 0;
  const int rc = iq_feed_cell_resume(g_feed_obj, g_feed_entry[k]);
  LOG_I(PHY, "SENSING: MPCI_FEED cell=%d id=%s resumed (radio not stopped) rc=%d\n", k, g_feed_ids[k], rc);
  return rc;
}
static int mpci_live_chan_retune(void *ctx, int k, double centre_hz)
{
  (void)ctx;
  if (!g_feed_obj || g_feed_entry[k] < 0)
    return 0;
  return iq_feed_cell_retune(g_feed_obj, g_feed_entry[k], centre_hz);
}
/* the cell's slot thread must be parked (its entry is paused) before its ring RU is retuned: no read may be inside the device */
static int mpci_live_wait_parked(int k)
{
  for (int i = 0; i < 3000 && !oai_exit; i++) { /* 3 s: a slot thread finishes its slot, then reads and finds the ring paused */
    if (nrue_ru_ring_parked(k))
      return 0;
    usleep(1000);
  }
  LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s slot thread did not park within 3 s: tuning not applied\n", k, g_feed_ids[k]);
  return -1;
}
static int mpci_live_ue_retune(void *ctx, int k, const iq_live_tune_t *t)
{
  (void)ctx;
  if (!g_feed_obj)
    return 0;
  if (mpci_live_wait_parked(k))
    return -1;
  if (fabs(t->centre_hz - g_feed_cells[k].centre_hz) > 0.5 && nrue_ru_ring_set_centre(k, t->centre_hz)) {
    LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s ring RU retune to %.0f Hz failed\n", k, g_feed_ids[k], t->centre_hz);
    return -1;
  }
  g_feed_cells[k].centre_hz = t->centre_hz;
  ru_tune_t rq = {.centre_hz = t->centre_hz, .cfo_hz = t->cfo_hz, .ssb = t->ssb};
  ru_tune_post(k, &rq);
  return 0;
}
static void mpci_live_wake(void *ctx)
{
  (void)ctx;
  nrue_ru_ring_wake();
}
static void mpci_live_set_centre_cb(void *arg)
{
  iq_feed_set_centre(g_feed_obj, *(double *)arg);
}
static int mpci_live_recentre(void *ctx, double centre_hz)
{
  (void)ctx;
  if (!g_feed_obj)
    return -1;
  double c = centre_hz;
  const int rc = nrue_feed_recentre(centre_hz, mpci_live_set_centre_cb, &c);
  LOG_W(PHY, "SENSING: MPCI_SPAN radio recentre %.0f -> %.0f Hz at rate %.0f (bounded radio retune, every served cell resyncs once): %s\n", g_live.plan.centre_hz, centre_hz,
        g_live.plan.rate, rc == 0 ? "done" : "FAILED, capture unchanged");
  return rc;
}
static void mpci_live_span_changed(void *ctx, const iq_span_plan_t *p)
{
  (void)ctx;
  g_feed_plan.centre_hz = p->centre_hz;
  g_feed_plan.usable_hz = p->usable_hz;
  g_feed_plan.lo_hz = p->lo_hz;
  g_feed_plan.hi_hz = p->hi_hz;
}

/* after the per-cell frame parameters and the feed plan: the live state of every slot */
static void mpci_live_init(void)
{
  if (g_ncells <= 1)
    return;
  if (g_feed_obj) {
    const iq_live_ops_t ops = {.chan_pause = mpci_live_pause, .chan_resume = mpci_live_resume, .chan_retune = mpci_live_chan_retune, .ue_retune = mpci_live_ue_retune,
                               .wake = mpci_live_wake, .radio_recentre = mpci_live_recentre, .span_changed = mpci_live_span_changed, .log = mpci_live_log};
    iq_live_init(&g_live, &g_feed_dev, &g_feed_plan, &ops);
    for (int k = 0; k < g_ncells; k++) {
      const NR_DL_FRAME_PARMS *fp = nrue_get_cell_fp(k);
      const iq_live_tune_t geom = {.n_rb = fp->N_RB_DL, .scs_hz = 15e3 * (double)(1 << fp->numerology_index), .n_rx = (unsigned)fp->nb_antennas_rx};
      iq_live_tune_t cur = geom;
      cur.centre_hz = g_feed_cells[k].centre_hz;
      cur.cfo_hz = nrPHY_vars_UE_g[k][0]->initial_fo;
      cur.ssb = fp->ssb_start_subcarrier;
      iq_live_slot_prepare(&g_live, k, g_feed_ids[k], &geom, &cur, g_feed_cells[k].rate, k < g_nplan);
    }
    char l[600];
    iq_live_format(&g_live, l, sizeof(l));
    LOG_I(PHY, "SENSING: %s\n", l);
  }
  g_live_on = 1;
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

/* CELLFEAT: the per-cell sensing services of cell k (bound to it, before the RX fence): the heap objects of the PRACH / SRS state, then the sense-API session
 * (rings, writer, files), the passive PRACH worker and the passive SRS searcher. Cell 0 of a single-cell receiver keeps the old place (after the radio start). */
/* MPCI-INT: the per-cell sensing services follow the cell's retire / re-activation (ru_park.h registry): pause = producers stop feeding the cell's PRACH / SRS /
 * sense session, nothing is allocated or closed (closing stays in mpci_stop_cell_sensing); resume = they feed again. The dashboard (nr_dashboard.c) is a launcher of one
 * process reading every cell's files and holds no per-cell state, so it has no hook. */
static void mpci_sensing_quiesce(void *ctx, int cell)
{
  (void)ctx;
  nr_sense_cell_pause(cell, true);
  nr_prach_passive_cell_pause(cell, true);
  nr_srs_passive_cell_pause(cell, true);
  LOG_I(PHY, "SENSING: MPCI_PARK cell=%d sense-session, PRACH and SRS paused (sessions kept open)\n", cell);
}
static void mpci_sensing_resume(void *ctx, int cell)
{
  (void)ctx;
  nr_srs_passive_cell_pause(cell, false);
  nr_prach_passive_cell_pause(cell, false);
  nr_sense_cell_pause(cell, false);
  LOG_I(PHY, "SENSING: MPCI_PARK cell=%d sense-session, PRACH and SRS resumed\n", cell);
}
static void mpci_cell_sensing_open(int k, PHY_VARS_NR_UE *ue)
{
  nr_prach_passive_cell_prealloc(k);
  nr_srs_passive_cell_prealloc(k);
  if (g_ncells <= 1)
    return;
  static atomic_int hook_registered; /* PROCESS: one registration for all cells (prealloc runs once per cell, bound to it) */
  if (!atomic_exchange(&hook_registered, 1)) {
    const ru_cell_hook_t h = {.name = "cellfeat", .quiesce = mpci_sensing_quiesce, .resume = mpci_sensing_resume, .ctx = NULL};
    if (ru_cell_hook_register(&h) != 0)
      LOG_W(PHY, "SENSING: MPCI_PARK hook registry full; retired cells keep their sensing services running\n");
  }
  nr_isac_start();
  nr_prach_passive_start();
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()))
    nr_srs_passive_start(ue);
}
/* CELLFEAT: the cell's session record, once its radio is up (the start hook; the session rings were allocated in the prealloc hook) */
static void mpci_cell_sensing_session(int k)
{
  if (!nr_sense_enabled())
    return;
  PHY_VARS_NR_UE *sense_ue = nrPHY_vars_UE_g[k][0];
  const openair0_config_t *sense_cfg = &openair0_cfg_g[sense_ue->rf_map.card];
  nr_sense_emit_session(&sense_ue->frame_parms, sense_cfg->rx_gain[sense_ue->rf_map.chain] - sense_cfg->rx_gain_offset[sense_ue->rf_map.chain]);
}
/* CELLFEAT shutdown of one cell's sensing services (bound to it; cell 0 last, mpci_for_each_cell runs the highest cell first): the PRACH worker is
 * drained, the SRS searcher stopped, the sense-API session closed. N = 1: the old sequence on cell 0. */
static void mpci_stop_cell_sensing(int k)
{
  (void)k;
  nr_prach_passive_flush();
  nr_srs_passive_stop();
  nr_isac_stop();
}

/* prealloc hook (bound to cell k, before the RX fence): every per-cell heap object, the per-cell module starts that allocate */
static int mpci_cell_prealloc(void *ctx, int k, const nr_mgr_slot_t *slot)
{
  (void)ctx;
  PHY_VARS_NR_UE *ue = nrPHY_vars_UE_g[k][0];
  ru_cell_want_pci_set(k, slot ? slot->desc.pci : -1); /* A10: the cell's sync pins the descriptor's PCI (before the slot thread's first search) */
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
  if (k > 0) {
    nr_pdcch_nid_gpu_start(ue->frame_parms.N_RB_DL); /* per-cell nID sweep on the cell's own grid (cell 0 started in main, unchanged) */
    /* MPCI-DBG3: register the cell with the GPU decode pass that cell 0 warmed in nr_dl_gpu_warmup (G3/G3b: own HARQ slab, instance floor, its own FEP
     * geometry when it differs; UL pass gate). Without it every grant of the cell fell back (fallback[cell], measured 2026-10-27 mp27e_P1: 45583 of
     * 45583). Bound to cell k, before the RX fence; a module that is not up, or a geometry it cannot serve, leaves the cell on the old path (logged). */
    if (nr_gpu_pass_active())
      nr_gpu_pass_warmup(ue);
  }
  nr_csirs_disc_pipeline_init(&ue->frame_parms);
  if (nr_csirs_disc_cell_prealloc(k, &ue->frame_parms, ue->frame_parms.nb_antennas_rx) != 0)
    LOG_W(PHY, "SENSING: CSIRS_DISC worker could not start; slot discovery feed disabled\n");
  mpci_cell_sensing_open(k, ue); /* CELLFEAT: this cell's sense-API session, passive PRACH and SRS (heap objects first, then the sessions' rings and workers) */
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_pusch_passive_pipeline_enabled()) {
    const nr_pdcch_blind_monitor_cfg_t *ul_cfg = nr_pdcch_blind_monitor_get_cfg();
    if (ul_cfg && ul_cfg->ul_pusch_decode) {
      if (!nr_pusch_passive_queue_start(ue, ul_cfg->ul_queue_depth > 0 ? ul_cfg->ul_queue_depth : 8, 1, ul_cfg->ul_thread_core))
        LOG_W(PHY, "SENSING: UL pipeline prewarm failed; grants will be dropped and counted\n");
    }
  }
  return 0;
}

/* start hook (bound to cell k): the cell's RT slot thread and stats thread; reactivation: the slot's threads exist (a retired cell's slot, or a spare), so the slot
 * takes the cell now (MPCI-LIVE: iq_live_add) */
static int mpci_cell_start(void *ctx, int k, const nr_mgr_slot_t *slot, int reactivation)
{
  (void)ctx;
  if (reactivation) {
    ru_cell_want_pci_set(k, slot ? slot->desc.pci : -1); /* A10: before the tuning is posted, so the re-acquisition pins the re-added cell's PCI */
    if (g_live_on && g_feed_obj && slot) {
      const iq_live_tune_t t = mpci_tune_of(k, &slot->desc, slot->ssb);
      const int st = iq_live_add(&g_live, k, slot->id, &t);
      LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s back in the inventory: slot %s\n", k, slot->id, st == IQ_LIVE_SERVED ? "served" : "unserved (logged above)");
      ru_cell_resume(k);
    } else {
      LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s back in the inventory: its ring belongs to its publisher; the slot thread is woken\n", k, slot ? slot->id : "-");
      ru_cell_resume(k); /* quiesce is called on this path too (mpci_cell_retire), so the resume must be */
      nrue_ru_ring_wake();
    }
    return 0;
  }
  mpci_cell_sensing_session(k);
  LOG_I(PHY, "Intializing UE Threads for instance %d ...\n", k);
  init_NR_UE_threads(nrPHY_vars_UE_g[k][0]);
  return 0;
}
static void mpci_cell_retire(void *ctx, int k, const nr_mgr_slot_t *slot)
{
  (void)ctx;
  if (g_live_on && g_feed_obj) {
    iq_live_remove(&g_live, k, "retired");
    ru_cell_quiesce(k); /* per-cell sessions registered by other modules (PRACH / SRS / sense session / dashboard view) */
    const int n = iq_live_retry(&g_live); /* what the capture could not carry before may fit the smaller set now */
    char l[600];
    iq_live_format(&g_live, l, sizeof(l));
    LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retired by the inventory (retry served %d): %s\n", k, slot ? slot->id : "-", n, l);
  } else {
    ru_cell_quiesce(k);
    LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retired by the inventory: the ring belongs to its publisher (a paused ring parks the slot thread outside the radio gate)\n", k,
          slot ? slot->id : "-");
  }
}
static void mpci_cell_retune(void *ctx, int k, const nr_cell_mgr_event_t *ev)
{
  (void)ctx;
  ru_cell_want_pci_set(k, ev->desc.pci); /* A10: before the retune is posted (the re-acquisition after it pins the descriptor's PCI) */
  if (g_live_on && g_feed_obj) {
    const iq_live_tune_t t = mpci_tune_of(k, &ev->desc, ev->ssb);
    const int st = iq_live_retune(&g_live, k, &t);
    LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retune requested centre_hz %.0f -> %.0f cfo_hz %.0f -> %.0f ssb %d: slot state %d\n", k, ev->id, ev->prev_centre_hz,
          ev->desc.centre_hz, ev->prev_cfo_hz, ev->desc.cfo_hz, t.ssb, st);
  } else
    LOG_W(PHY, "SENSING: MPCI_LIFE cell=%d id=%s retune requested centre_hz %.0f -> %.0f: not applied (the ring belongs to its publisher)\n", k, ev->id, ev->prev_centre_hz,
          ev->desc.centre_hz);
}
static void mpci_dash_gpu_log(void);
static void mpci_dash_budget_log(void);
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
    {
      static int s_parked[NR_CELL_MAX]; /* MPCI-LIVE: edges of the slot threads parked outside the radio gate (a paused ring: backlog read first, then PAUSED) */
      for (int k = 0; k < g_ncells && k < NR_CELL_MAX; k++) {
        const int pk = nrue_ru_ring_parked(k);
        if (pk != s_parked[k]) {
          s_parked[k] = pk;
          LOG_I(PHY, "SENSING: MPCI_LIFE cell=%d slot thread %s (ring paused: read returned PAUSED, radio gate not held; parks=%lu)\n", k, pk ? "PARKED" : "running again", ru_park_waits(k));
        }
      }
    }
    static int s_feed_tick;
    if (++s_feed_tick % 10 == 0) {
      mpci_feed_log(); /* G6: MPCI_FEED every 10 s (+ MPCI-DASH P3 CHAN_CELL) */
      mpci_dash_gpu_log(); /* MPCI-DASH P2 CELL_GPU, every 10 s */
    }
    if (s_feed_tick % 30 == 0)
      mpci_dash_budget_log(); /* MPCI-DASH P7 CELL_BUDGET, every 30 s */
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
/* MPCI-DASH P2 CELL_GPU and P7 CELL_BUDGET: per-cell lines from the manager thread, once per cell with the thread bound to it (mpci_for_each_cell: the [c<k>] tag is the cell
 * the line is about, and cell=/pci= are in the line anyway). Only when more than one cell is served; counters and memory books are read, nothing is measured on a hot path. */
static nr_dash_gpu_t s_dash_gpu_prev[NR_CELL_MAX];
static nr_dash_win_t s_dash_gpu_win[NR_CELL_MAX];
static uint64_t s_dash_now_ns;
static int mpci_dash_pci(int k, const nr_mgr_slot_t *sl)
{
  const int lp = nr_passive_metrics_pci_of(k);
  return lp >= 0 ? lp : sl->desc.pci;
}
static void mpci_dash_cell_gpu(int k)
{
  const nr_mgr_slot_t *sl = nr_cell_mgr_slot(&g_cells.mgr, k);
  if (k >= NR_CELL_MAX || !sl || !sl->active)
    return;
  nr_dash_gpu_t cur;
  memset(&cur, 0, sizeof(cur));
  nr_gpu_pass_dl_cell_counters(k, &cur.calls, &cur.fallback, &cur.active);
  cur.mem_dev_b = nr_gpu_memstat_cell_bytes(k, 0);
  cur.mem_pin_b = nr_gpu_memstat_cell_bytes(k, 1);
  cur.mem_mgd_b = nr_gpu_memstat_cell_bytes(k, 2);
  char l[320];
  if (nr_dash_fmt_cell_gpu(l, sizeof(l), nr_cell_count(), k, mpci_dash_pci(k, sl), nr_dash_win_tick(&s_dash_gpu_win[k], s_dash_now_ns), &cur, &s_dash_gpu_prev[k]) > 0)
    LOG_A(PHY, "%s\n", l);
  s_dash_gpu_prev[k] = cur;
}
static void mpci_dash_cell_budget(int k)
{
  const nr_mgr_slot_t *sl = nr_cell_mgr_slot(&g_cells.mgr, k);
  if (k >= NR_CELL_MAX || !sl || !sl->active)
    return;
  nr_dash_budget_t b = {.ram_admit_mb = -1, .gpu_admit_mb = -1};
  const nr_admit_plan_t *pl = &g_cells.mgr.plan; /* the plan's per-cell rows are in admission order: the cell of slot k is the served row of rank k */
  for (int i = 0; i < pl->n && i < NR_ADMIT_PLAN_MAX; i++)
    if (pl->cell[i].served && pl->cell[i].order == k) {
      b.ram_admit_mb = (long long)(pl->cell[i].host_bytes >> 20);
      b.gpu_admit_mb = (long long)(pl->cell[i].gpu_bytes >> 20);
    }
  b.gpu_now_mb = (long long)((nr_gpu_memstat_cell_bytes(k, 0) + nr_gpu_memstat_cell_bytes(k, 2)) >> 20);
  b.pin_now_mb = (long long)(nr_gpu_memstat_cell_bytes(k, 1) >> 20);
  b.tokens_fair = nr_pool_site() ? (double)nr_pool_size(nr_pool_site()) / (double)nr_cell_count() : 0.0;
  char l[320];
  if (nr_dash_fmt_cell_budget(l, sizeof(l), nr_cell_count(), k, mpci_dash_pci(k, sl), &b) > 0)
    LOG_A(PHY, "%s\n", l);
}
static void mpci_dash_gpu_log(void)
{
  if (nr_cell_count() <= 1)
    return;
  s_dash_now_ns = mpci_now_ns();
  mpci_for_each_cell(mpci_dash_cell_gpu);
}
static void mpci_dash_budget_log(void)
{
  if (nr_cell_count() <= 1)
    return;
  const size_t tot = nr_host_membudget_total(), used = nr_host_membudget_committed(), head = nr_host_membudget_headroom(), avail = nr_host_membudget_live_avail();
  long long rss_mb = -1;
  FILE *sf = fopen("/proc/self/statm", "r");
  if (sf) {
    long sz = 0, res = 0;
    if (fscanf(sf, "%ld %ld", &sz, &res) == 2)
      rss_mb = (long long)res * (long long)sysconf(_SC_PAGESIZE) / (1024 * 1024);
    fclose(sf);
  }
#define DASH_MB(x) ((x) == SIZE_MAX ? -1LL : (long long)((x) >> 20))
  char l[320];
  if (nr_dash_fmt_budget_proc(l, sizeof(l), nr_cell_count(), DASH_MB(tot), DASH_MB(used), DASH_MB(head), DASH_MB(avail), rss_mb) > 0)
    LOG_A(PHY, "%s\n", l);
#undef DASH_MB
  mpci_for_each_cell(mpci_dash_cell_budget);
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
  /* one subframe = 1 ms; G6: the radio streams the wideband capture, not the cell */
  const size_t per_ms = g_feed ? mpci_feed_usrp_per_ms() : (size_t)fp->samples_per_subframe * fp->nb_antennas_rx * sizeof(int32_t);
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
    nr_core_layout_init_nf(true, radio, g_ncells, radio && g_ncells > 1); /* CHANPOOL: several cells on a radio = a wideband feed reader in the plan */
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
  mpci_feed_rings_for_rus(); /* G6: cell 0 reads its ring too; the radio is the capture device */

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
    {
      const int prev_cell = nr_cell_idx(); /* CELLFEAT: the antenna count of the cell's own sense-API session */
      nr_cell_bind(cell_id);
      nr_isac_set_rx_antennas(nrue_get_cell_fp(cell_id)->nb_antennas_rx);
      nr_cell_bind(prev_cell);
    }
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
  mpci_feed_open(); /* G6: capture plan and the radio loaded (its driver re-plans with the device's rate set) */
  plan_usrp_ring(&disc_ue->frame_parms); /* host-RAM budget: the USRP ring is reserved first (the radio needs it), before any module sizes itself */
  mpci_feed_rings(); /* G6: the wideband ring and the cell rings, next in the host-RAM plan */
  nr_dl_gpu_warmup(disc_ue);
  nr_pdcch_nid_gpu_start(disc_ue->frame_parms.N_RB_DL);
  if (g_ncells > 1) {
    /* A8: per cell, bound to it: heap objects, CSI-RS discovery, PDCCH monitor / uectx (k > 0), PUSCH queue; then the cell count (LOG tag) */
    nr_cell_life_prepare(&g_cells);
  } else {
  nr_prach_passive_cell_prealloc(0); /* CELLFEAT: the PRACH / SRS buffers are allocated here (before the memory lock), as the static arrays of before were in the image */
  nr_srs_passive_cell_prealloc(0);
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
  if (g_feed) { /* G6: memory locked before the radio streams, then the capture runs before the cells' ring RUs attach (they wait for its first samples) */
    lock_memory_to_ram();
    mpci_feed_start();
  }
  nrue_init_openair0();



  if (!g_feed)
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
  if (g_ncells == 1) { /* N > 1: every cell's session, PRACH and SRS were opened in its prealloc hook (before the fence) and its start hook emits the session record */
    nr_isac_start();
    mpci_cell_sensing_session(0);
    nr_prach_passive_start();
    if (IS_PASSIVE_RX_MODE(get_softmodem_params())) nr_srs_passive_start(nrPHY_vars_UE_g[0][0]);
  }

  if (g_ncells > 1) {
    mpci_live_init(); /* MPCI-LIVE: the slots' live state (before any hook of the control thread can run) */
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
  if (g_feed_obj) { /* G6: the radio is released: the reader has left on its failed read; stop the channelizer, final counters */
    iq_feed_stop(g_feed_obj);
    mpci_feed_log();
  }

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
  mpci_for_each_cell(mpci_stop_cell_sensing); /* CELLFEAT: per cell, highest first (cell 0 last); N = 1: the sequence of before */
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
