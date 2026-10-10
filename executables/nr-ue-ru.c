/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <unistd.h>
#include <stdatomic.h>
#include "common/utils/nr_shutdown.h"
#include "common/utils/nr_core_plan.h"
#include "nr-ue-ru.h"
#include "nr-uesoftmodem.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
#include "common/config/config_paramdesc.h"
#include "common/config/config_userapi.h"
#include "openair1/PHY/phy_extern_nr_ue.h"
#include "PHY/MODULATION/nr_modulation.h" // init_symbol_rotation
#include "common/utils/nr/nr_common.h"     // nr_band_scan_windows
#define NRUE_BAND_SCAN_MAX 64
static uint64_t s_band_scan_c[NRUE_BAND_SCAN_MAX];
static int s_band_scan_n = 0, s_band_scan_i = -1;

/* NR UE RU configuration section name */
#define CONFIG_STRING_NRUE_RU_LIST "RUs"

#define NRUE_RU_NB_TX "nb_tx"
#define NRUE_RU_NB_RX "nb_rx"
#define NRUE_RU_ATT_TX "att_tx"
#define NRUE_RU_ATT_RX "att_rx"
#define NRUE_RU_MAX_RXGAIN "max_rxgain"
#define NRUE_RU_SDR_ADDRS "sdr_addrs"
#define NRUE_RU_TX_SUBDEV "tx_subdev"
#define NRUE_RU_RX_SUBDEV "rx_subdev"
#define NRUE_RU_CLOCK_SRC "clock_src"
#define NRUE_RU_TIME_SRC "time_src"
#define NRUE_RU_TUNE_OFFSET "tune_offset"
#define NRUE_RU_IF_FREQUENCY "if_freq"
#define NRUE_RU_IF_FREQ_OFFSET "if_offset"

#define NRUE_RU_SRC_CHECK                                                                                        \
  &(checkedparam_t)                                                                                              \
  {                                                                                                              \
    .s3a = { config_checkstr_assign_integer, {"internal", "external", "gpsdo"}, {internal, external, gpsdo}, 3 } \
  }

/*------------------------------------------------------------------------------------------------------------------------------------------------*/
/*                                                       NR UE RU configuration parameters                                                        */
/* optname                   helpstr                    paramflags  XXXptr        defXXXval              type         numelt  chkPptr             */
/*------------------------------------------------------------------------------------------------------------------------------------------------*/
// clang-format off
#define NRUE_RU_PARAMS_DESC { \
  {NRUE_RU_NB_TX,            CONFIG_HLP_UENANTT,        0,         .uptr=NULL,   .defuintval=1,          TYPE_UINT,   0,      NULL              }, \
  {NRUE_RU_NB_RX,            CONFIG_HLP_UENANTR,        0,         .uptr=NULL,   .defuintval=1,          TYPE_UINT,   0,      NULL              }, \
  {NRUE_RU_ATT_TX,           NULL,                      0,         .uptr=NULL,   .defuintval=0,          TYPE_UINT,   0,      NULL              }, \
  {NRUE_RU_ATT_RX,           NULL,                      0,         .uptr=NULL,   .defuintval=0,          TYPE_UINT,   0,      NULL              }, \
  {NRUE_RU_MAX_RXGAIN,       NULL,                      0,         .iptr=NULL,   .defintval=120,         TYPE_INT,    0,      NULL              }, \
  {NRUE_RU_SDR_ADDRS,        CONFIG_HLP_USRP_ARGS,      0,         .strptr=NULL, .defstrval="type=b200", TYPE_STRING, 0,      NULL              }, \
  {NRUE_RU_TX_SUBDEV,        CONFIG_HLP_TX_SUBDEV,      0,         .strptr=NULL, .defstrval="",          TYPE_STRING, 0,      NULL              }, \
  {NRUE_RU_RX_SUBDEV,        CONFIG_HLP_RX_SUBDEV,      0,         .strptr=NULL, .defstrval="",          TYPE_STRING, 0,      NULL              }, \
  {NRUE_RU_CLOCK_SRC,        NULL,                      0,         .strptr=NULL, .defstrval="internal",  TYPE_STRING, 0,      NRUE_RU_SRC_CHECK }, \
  {NRUE_RU_TIME_SRC,         NULL,                      0,         .strptr=NULL, .defstrval="internal",  TYPE_STRING, 0,      NRUE_RU_SRC_CHECK }, \
  {NRUE_RU_TUNE_OFFSET,      CONFIG_HLP_TUNE_OFFSET,    0,         .dblptr=NULL, .defdblval=0.0,         TYPE_DOUBLE, 0,      NULL              }, \
  {NRUE_RU_IF_FREQUENCY,     CONFIG_HLP_IF_FREQ,        0,         .u64ptr=NULL, .defint64val=0,         TYPE_UINT64, 0,      NULL              }, \
  {NRUE_RU_IF_FREQ_OFFSET,   CONFIG_HLP_IF_FREQ_OFF,    0,         .iptr=NULL,   .defintval=0,           TYPE_INT,    0,      NULL              }, \
}
// clang-format on

/* NR UE cell configuration section name */
#define CONFIG_STRING_NRUE_CELL_LIST "cells"

#define NRUE_CELL_RU_ID "ru_id"
#define NRUE_CELL_BAND "band"
#define NRUE_CELL_RF_FREQUENCY "rf_freq"
#define NRUE_CELL_RF_FREQ_OFFSET "rf_offset"
#define NRUE_CELL_NUMEROLOGY "numerology"
#define NRUE_CELL_N_RB_DL "N_RB_DL"
#define NRUE_CELL_SSB_START "ssb_start"

/*------------------------------------------------------------------------------------------------------------------------------------------------*/
/*                                                      NR UE cell configuration parameters                                                       */
/* optname                   helpstr                    paramflags  XXXptr        defXXXval              type         numelt  chkPptr             */
/*------------------------------------------------------------------------------------------------------------------------------------------------*/
// clang-format off
#define NRUE_CELL_PARAMS_DESC { \
  {NRUE_CELL_RU_ID,          NULL,                      0,         .iptr=NULL,   .defintval=0,           TYPE_INT,    0,      NULL              }, \
  {NRUE_CELL_BAND,           CONFIG_HLP_BAND,           0,         .iptr=NULL,   .defintval=78,          TYPE_INT,    0,      NULL              }, \
  {NRUE_CELL_RF_FREQUENCY,   CONFIG_HLP_DLF,            0,         .u64ptr=NULL, .defint64val=0,         TYPE_UINT64, 0,      NULL              }, \
  {NRUE_CELL_RF_FREQ_OFFSET, CONFIG_HLP_ULF,            0,         .i64ptr=NULL, .defint64val=0,         TYPE_INT64,  0,      NULL              }, \
  {NRUE_CELL_NUMEROLOGY,     CONFIG_HLP_NUMEROLOGY,     0,         .iptr=NULL,   .defintval=1,           TYPE_INT,    0,      NULL              }, \
  {NRUE_CELL_N_RB_DL,        CONFIG_HLP_PRB_SA,         0,         .iptr=NULL,   .defintval=106,         TYPE_INT,    0,      NULL              }, \
  {NRUE_CELL_SSB_START,      CONFIG_HLP_SSC,            0,         .iptr=NULL,   .defintval=516,         TYPE_INT,    0,      NULL              }, \
}
// clang-format on

static int nrue_cell_count;
static nrUE_cell_params_t *nrue_cells;
static NR_DL_FRAME_PARMS *nrue_cell_fp;

static int nrue_ru_count;
static nrUE_RU_params_t *nrue_rus;

openair0_config_t openair0_cfg_g[MAX_CARDS] = {};
static openair0_device_t openair0_dev[MAX_CARDS] = {};
static unsigned long rf_fail_after_reads;
static atomic_ulong rf_read_count;

/* nrue_get_cell_count: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:11, executables/nr-uesoftmodem.c:508, executables/nr-uesoftmodem.c:535, executables/nr-uesoftmodem.c:539.
 */
int nrue_get_cell_count(void)
{
  return nrue_cell_count;
}

/* nrue_get_band: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:16, executables/nr-ue.c:1413.
 */
int nrue_get_band(const PHY_VARS_NR_UE *UE)
{
  int cell_id = nrue_rus[UE->rf_map.card].used_by_cell;
  return nrue_cells[cell_id].band;
}

/* nrue_get_cell: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:12, executables/nr-uesoftmodem.c:490, executables/nr-uesoftmodem.c:509, executables/nr-uesoftmodem.c:525 (+1 more).
 */
const nrUE_cell_params_t *nrue_get_cell(int cell_id)
{
  AssertFatal(cell_id >= 0 && cell_id < nrue_cell_count, "Invalid cell ID %d! Cell count = %d\n", cell_id, nrue_cell_count);
  return &nrue_cells[cell_id];
}

/* nrue_get_cell_fp: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:13, executables/nr-uesoftmodem.c:304, executables/nr-uesoftmodem.c:523, executables/nr-uesoftmodem.c:525 (+1 more).
 */
NR_DL_FRAME_PARMS *nrue_get_cell_fp(int cell_id)
{
  AssertFatal(cell_id >= 0 && cell_id < nrue_cell_count, "Invalid cell ID %d! Cell count = %d\n", cell_id, nrue_cell_count);
  return &nrue_cell_fp[cell_id];
}

/* nrue_set_cell: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:14, executables/nr-uesoftmodem.c:572.
 */
void nrue_set_cell(int cell_id, const nrUE_cell_params_t *cell)
{
  AssertFatal(cell_id >= 0 && cell_id < nrue_cell_count, "Invalid cell ID %d! Cell count = %d\n", cell_id, nrue_cell_count);
  nrue_cells[cell_id] = *cell;
}

/* nrue_set_cell_params: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_BANDSCAN_FIRST_HZ.
 *   Callers: executables/nr-ue-ru.h:15, executables/nr-uesoftmodem.c:484.
 */
void nrue_set_cell_params(configmodule_interface_t *cfg)
{
  paramdef_t cellParams[] = NRUE_CELL_PARAMS_DESC;
  paramlist_def_t cellParamList = {CONFIG_STRING_NRUE_CELL_LIST, NULL, 0};
  config_getlist(cfg, &cellParamList, cellParams, sizeofArray(cellParams), NULL);

  if (cellParamList.numelt <= 0) {
    nrue_cell_count = 1;
    nrue_cell_fp = calloc_or_fail(nrue_cell_count, sizeof(NR_DL_FRAME_PARMS));
    nrue_cells = calloc_or_fail(nrue_cell_count, sizeof(nrUE_cell_params_t));
    uint64_t rf_frequency = downlink_frequency[0][0];
    /* BAND-WIDE SEARCH: no -C given, only --band. The RX centre steps across the band's SSB raster
     * in sampling-bandwidth windows (nrue_band_scan_next, one per failed acquisition); the first
     * window is tuned here so the radio never starts at 0 Hz. Once MIB/SIB1 give point A the
     * existing SYNC REQ retune moves the carrier to the cell's own centre. */
    if (rf_frequency == 0 && get_nrUE_params()->UE_scan_carrier) {
      s_band_scan_n = nr_band_scan_windows(get_softmodem_params()->band, get_softmodem_params()->numerology,
                                           get_nrUE_params()->N_RB_DL, s_band_scan_c, NRUE_BAND_SCAN_MAX);
      AssertFatal(s_band_scan_n > 0, "band-wide search: no synchronisation raster for band %d\n", get_softmodem_params()->band);
      s_band_scan_i = 0;
      /* rfsim carries the gNB's baseband whatever the UE is tuned to, so a band scan there can only
       * be exercised mechanically; ISAC_BANDSCAN_FIRST_HZ puts the cell's centre first (test knob). */
      const char *first = getenv("ISAC_BANDSCAN_FIRST_HZ");
      if (first != NULL && strtoull(first, NULL, 10) > 0) {
        for (int i = s_band_scan_n; i > 0; i--)
          s_band_scan_c[i] = s_band_scan_c[i - 1];
        s_band_scan_c[0] = strtoull(first, NULL, 10);
        s_band_scan_n = s_band_scan_n < NRUE_BAND_SCAN_MAX ? s_band_scan_n + 1 : NRUE_BAND_SCAN_MAX;
      }
      rf_frequency = s_band_scan_c[0];
      LOG_W(PHY, "SENSING: BANDSCAN band %d: %d windows of %d RB, first centre %lu Hz\n",
            get_softmodem_params()->band, s_band_scan_n, get_nrUE_params()->N_RB_DL, rf_frequency);
    }
    nrue_cells[0] = (nrUE_cell_params_t){.ru_id = 0,
                                         .band = get_softmodem_params()->band,
                                         .rf_frequency = rf_frequency,
                                         .rf_freq_offset = uplink_frequency_offset[0][0],
                                         .numerology = get_softmodem_params()->numerology,
                                         .N_RB_DL = get_nrUE_params()->N_RB_DL,
                                         .ssb_start = get_nrUE_params()->ssb_start_subcarrier,
                                         .used_by_ue = -1};
    return;
  }

  nrue_cell_count = cellParamList.numelt;
  nrue_cell_fp = calloc_or_fail(nrue_cell_count, sizeof(NR_DL_FRAME_PARMS));
  nrue_cells = calloc_or_fail(nrue_cell_count, sizeof(nrUE_cell_params_t));

  LOG_W(NR_PHY, "Ignoring command line cell parameters, using the following instead:\n");

  for (int cell_id = 0; cell_id < nrue_cell_count; cell_id++) {
    nrue_cells[cell_id].ru_id          = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_RU_ID)->iptr);
    nrue_cells[cell_id].band           = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_BAND)->iptr);
    nrue_cells[cell_id].rf_frequency   = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_RF_FREQUENCY)->u64ptr);
    nrue_cells[cell_id].rf_freq_offset = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_RF_FREQ_OFFSET)->i64ptr);
    nrue_cells[cell_id].numerology     = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_NUMEROLOGY)->iptr);
    nrue_cells[cell_id].N_RB_DL        = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_N_RB_DL)->iptr);
    nrue_cells[cell_id].ssb_start      = *(gpd(cellParamList.paramarray[cell_id], sizeofArray(cellParams), NRUE_CELL_SSB_START)->iptr);
    nrue_cells[cell_id].used_by_ue     = -1;

    LOG_I(NR_PHY,
          "cell %d: ru_id %d, band %d, DL freq %lu, UL freq_offset %ld, numerology %d, N_RB_DL %d, ssb_start %d\n",
          cell_id,
          nrue_cells[cell_id].ru_id,
          nrue_cells[cell_id].band,
          nrue_cells[cell_id].rf_frequency,
          nrue_cells[cell_id].rf_freq_offset,
          nrue_cells[cell_id].numerology,
          nrue_cells[cell_id].N_RB_DL,
          nrue_cells[cell_id].ssb_start);
  }
}

/* nrue_get_ru_count: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:17, executables/nr-uesoftmodem.c:183, executables/nr-uesoftmodem.c:510, executables/nr-uesoftmodem.c:514.
 */
int nrue_get_ru_count(void)
{
  return nrue_ru_count;
}

/* nrue_get_ru: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:18, executables/nr-uesoftmodem.c:186, executables/nr-uesoftmodem.c:277, executables/nr-uesoftmodem.c:305 (+2 more).
 */
const nrUE_RU_params_t *nrue_get_ru(int ru_id)
{
  AssertFatal(ru_id >= 0 && ru_id < nrue_ru_count, "Invalid RU ID %d! RU count = %d\n", ru_id, nrue_ru_count);
  return &nrue_rus[ru_id];
}

/* nrue_set_ru_cell_id: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:19, executables/nr-uesoftmodem.c:520.
 */
void nrue_set_ru_cell_id(int ru_id, int cell_id)
{
  AssertFatal(ru_id >= 0 && ru_id < nrue_ru_count, "Invalid RU ID %d! RU count = %d\n", ru_id, nrue_ru_count);
  AssertFatal(cell_id >= 0 && cell_id < nrue_cell_count, "Invalid cell ID %d! Cell count = %d\n", cell_id, nrue_cell_count);
  nrue_rus[ru_id].used_by_cell = cell_id;
}

/* nrue_set_ru_params: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:20, executables/nr-uesoftmodem.c:406, executables/nr-uesoftmodem.c:483.
 */
void nrue_set_ru_params(configmodule_interface_t *cfg)
{
  paramdef_t RUParams[] = NRUE_RU_PARAMS_DESC;
  paramlist_def_t RUParamList = {CONFIG_STRING_NRUE_RU_LIST, NULL, 0};
  config_getlist(cfg, &RUParamList, RUParams, sizeofArray(RUParams), NULL);

  if (RUParamList.numelt <= 0) {
    nrue_ru_count = 1;
    nrue_rus = calloc_or_fail(nrue_ru_count, sizeof(nrUE_RU_params_t));
    nrue_rus[0] = (nrUE_RU_params_t){.nb_tx = get_nrUE_params()->nb_antennas_tx,
                                     .nb_rx = get_nrUE_params()->nb_antennas_rx,
                                     .att_tx = get_nrUE_params()->tx_gain,
                                     .att_rx = 0,
                                     .max_rxgain = get_nrUE_params()->rx_gain,
                                     .sdr_addrs = get_nrUE_params()->usrp_args,
                                     .tx_subdev = get_nrUE_params()->tx_subdev,
                                     .rx_subdev = get_nrUE_params()->rx_subdev,
                                     .clock_source = get_softmodem_params()->clock_source,
                                     .time_source = get_softmodem_params()->timing_source,
                                     .tune_offset = get_softmodem_params()->tune_offset,
                                     .if_frequency = get_nrUE_params()->if_freq,
                                     .if_freq_offset = get_nrUE_params()->if_freq_off,
                                     .used_by_cell = -1};
    return;
  }

  nrue_ru_count = RUParamList.numelt;
  nrue_rus = calloc_or_fail(nrue_ru_count, sizeof(nrUE_RU_params_t));

  LOG_W(NR_PHY, "Ignoring command line RU parameters, using the following instead:\n");

  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    nrue_rus[ru_id].nb_tx            = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_NB_TX)->uptr);
    nrue_rus[ru_id].nb_rx            = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_NB_RX)->uptr);
    nrue_rus[ru_id].att_tx           = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_ATT_TX)->uptr);
    nrue_rus[ru_id].att_rx           = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_ATT_RX)->uptr);
    nrue_rus[ru_id].max_rxgain       = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_MAX_RXGAIN)->iptr);
    nrue_rus[ru_id].tune_offset      = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_TUNE_OFFSET)->dblptr);
    nrue_rus[ru_id].if_frequency     = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_IF_FREQUENCY)->u64ptr);
    nrue_rus[ru_id].if_freq_offset   = *(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_IF_FREQ_OFFSET)->iptr);
    nrue_rus[ru_id].used_by_cell     = -1;

    nrue_rus[ru_id].sdr_addrs = strdup(*(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_SDR_ADDRS)->strptr));
    nrue_rus[ru_id].tx_subdev = strdup(*(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_TX_SUBDEV)->strptr));
    nrue_rus[ru_id].rx_subdev = strdup(*(gpd(RUParamList.paramarray[ru_id], sizeofArray(RUParams), NRUE_RU_RX_SUBDEV)->strptr));

    int clock_src_idx = config_paramidx_fromname(RUParams, sizeofArray(RUParams), NRUE_RU_CLOCK_SRC);
    AssertFatal(clock_src_idx >= 0, "Index for clock_src config option not found!\n");
    nrue_rus[ru_id].clock_source = config_get_processedint(cfg, &RUParamList.paramarray[ru_id][clock_src_idx]);

    int time_src_idx = config_paramidx_fromname(RUParams, sizeofArray(RUParams), NRUE_RU_TIME_SRC);
    AssertFatal(time_src_idx >= 0, "Index for time_src config option not found!\n");
    nrue_rus[ru_id].time_source = config_get_processedint(cfg, &RUParamList.paramarray[ru_id][time_src_idx]);

    LOG_I(NR_PHY,
          "RU %d: nb_tx %d, nb_rx %d, att_tx %d, att_rx %d, max_rxgain %d, tune_offset %f, if_frequency %lu, if_freq_offset %d, "
          "sdr_addrs \"%s\", tx_subdev \"%s\", rx_subdev \"%s\", clock_source %d, time_source %d\n",
          ru_id,
          nrue_rus[ru_id].nb_tx,
          nrue_rus[ru_id].nb_rx,
          nrue_rus[ru_id].att_tx,
          nrue_rus[ru_id].att_rx,
          nrue_rus[ru_id].max_rxgain,
          nrue_rus[ru_id].tune_offset,
          nrue_rus[ru_id].if_frequency,
          nrue_rus[ru_id].if_freq_offset,
          nrue_rus[ru_id].sdr_addrs,
          nrue_rus[ru_id].tx_subdev,
          nrue_rus[ru_id].rx_subdev,
          nrue_rus[ru_id].clock_source,
          nrue_rus[ru_id].time_source);
  }
}

/* nrue_init_openair0: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:22, executables/nr-uesoftmodem.c:639.
 */
void nrue_init_openair0(void)
{
  int freq_off = 0;
  bool is_sidelink = (get_softmodem_params()->sl_mode) ? true : false;
  AssertFatal(MAX_CARDS >= nrue_ru_count,
              "Too many RUs allocated (%d)! Maybe increase MAX_CARDS (%d).\n",
              nrue_ru_count,
              MAX_CARDS);

  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    int cell_id = nrue_rus[ru_id].used_by_cell;
    if (cell_id < 0) {
      LOG_W(PHY, "Skipping initialization of RU %d because it is not used by any cell!\n", ru_id);
      continue;
    }
    NR_DL_FRAME_PARMS *frame_parms = &nrue_cell_fp[cell_id];
    if (is_sidelink) {
      int UE_id = nrue_cells[cell_id].used_by_ue;
      if (UE_id < 0) {
        LOG_W(PHY, "Skipping initialization of RU %d because it is not used by any UE!\n", ru_id);
        continue;
      }
      frame_parms = &nrPHY_vars_UE_g[UE_id][0]->SL_UE_PHY_PARAMS.sl_frame_params;
    }

    openair0_config_t *cfg = &openair0_cfg_g[ru_id];
    cfg->configFilename    = NULL;
    cfg->sample_rate       = frame_parms->samples_per_subframe * 1e3;

    if (frame_parms->frame_type == TDD)
      cfg->duplex_mode = duplex_mode_TDD;
    else
      cfg->duplex_mode = duplex_mode_FDD;

    cfg->ru_id = ru_id;
    cfg->num_rb_dl = frame_parms->N_RB_DL;
    cfg->tx_num_channels = min(4, frame_parms->nb_antennas_tx);
    cfg->rx_num_channels = min(4, frame_parms->nb_antennas_rx);

    LOG_I(PHY,
          "HW: Configuring ru_id %d, sample_rate %f, tx/rx num_channels %d/%d, duplex_mode %s\n",
          ru_id,
          cfg->sample_rate,
          cfg->tx_num_channels,
          cfg->rx_num_channels,
          duplex_mode_txt[cfg->duplex_mode]);

    uint64_t dl_carrier, ul_carrier;
    if (is_sidelink || nrue_rus[ru_id].if_frequency == 0) {
      dl_carrier = frame_parms->dl_CarrierFreq;
      ul_carrier = frame_parms->ul_CarrierFreq;
    } else {
      dl_carrier = nrue_rus[ru_id].if_frequency;
      ul_carrier = nrue_rus[ru_id].if_frequency + nrue_rus[ru_id].if_freq_offset;
    }

    nr_rf_card_config_freq(cfg, ul_carrier, dl_carrier, freq_off);
    nr_rf_card_config_gain(cfg);

    cfg->configFilename = get_softmodem_params()->rf_config_file;

    cfg->sdr_addrs = nrue_rus[ru_id].sdr_addrs;
    cfg->tx_subdev = nrue_rus[ru_id].tx_subdev;
    cfg->rx_subdev = nrue_rus[ru_id].rx_subdev;
    cfg->clock_source = nrue_rus[ru_id].clock_source;
    cfg->time_source = nrue_rus[ru_id].time_source;
    cfg->tune_offset = nrue_rus[ru_id].tune_offset;
  }
}

/* nrue_ru_fault_config: reads the default-off fault-injection switch ISAC_RF_FAIL_AFTER_READS and stores the read count after which nrue_ru_read_impl() fails.
 *   Used by the orchestrator's shutdown acceptance test; the same fatal-read path as a real driver failure. Callers: nrue_ru_start_impl() and the test fixture nrue_ru_test_device().
 */
static void nrue_ru_fault_config(void)
{
  /* Independent, default-off fault injection for the orchestrator's shutdown
   * acceptance test. It uses the same fatal-read path as a driver failure. */
  const char *fail = getenv("ISAC_RF_FAIL_AFTER_READS");
  if (fail) {
    char *end;
    long value = strtol(fail, &end, 10);
    if (*fail && !*end && value > 0)
      rf_fail_after_reads = value;
  }
}

/* nrue_ru_start_impl: loads and starts every radio device (openair0_device_load, then trx_start_func), and initialises the TX write thread when usrp_tx_thread is set.
 *   Returns 0, or -1 on the first failure or when shutdown was requested. Called only through nrue_ru_start(), which holds the lifetime gate.
 */
static int nrue_ru_start_impl(void)
{
  nrue_ru_fault_config();
  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    openair0_config_t *cfg = &openair0_cfg_g[ru_id];
    openair0_device_t *dev = &openair0_dev[ru_id];

    dev->host_type = RAU_HOST;
    int tmp = openair0_device_load(dev, cfg);
    if (tmp != 0) {
      LOG_E(HW, "SENSING: radio device %d initialization failed\n", ru_id); return -1;
    }
    if (nr_shutdown_requested())
      return -1;
    int tmp2 = dev->trx_start_func(dev);
    if (tmp2 != 0) {
      LOG_E(HW, "SENSING: radio device %d start failed\n", ru_id); return -1;
    }
    if (usrp_tx_thread == 1)
      dev->trx_write_init(dev);
  }
  return 0;
}

/* nrue_ru_stop_impl: stops the RX stream of every radio device that has a stop hook (trx_stop_func). Called only through nrue_ru_stop(). */
static void nrue_ru_stop_impl(void)
{
  for (openair0_device_t *ru = openair0_dev; ru < openair0_dev + nrue_ru_count; ru++) {
    if (ru->trx_stop_func)
      ru->trx_stop_func(ru);
  }
}

/**
 * nrue_ru_restart_rx_impl: restarts the RX stream of every device: stop, clear the cached first-timestamp origin (firstTS_initialized), then start again.
 *   Dropping the origin is required because the device restarts its sample counter at 0. Returns -1 when any start fails. Called only through nrue_ru_restart_rx().
 *
 * Runtime RX stream stop/start, for recovering the measured X410 stall where the device keeps
 * returning full-length reads with continuous timestamps but no signal (see the RFSTALL watchdog
 * in nr-ue.c). Separate from nrue_ru_stop()/nrue_ru_start() because it must ALSO drop the cached
 * timestamp origin: the device restarts its sample counter at 0, and firstTS was captured on the
 * first read of the previous stream, so keeping it would make every subsequent
 * `tmp_timestamp - dev->firstTS` negative and corrupt all downstream slot arithmetic. */
static int nrue_ru_restart_rx_impl(void)
{
  int rc = 0;
  for (openair0_device_t *ru = openair0_dev; ru < openair0_dev + nrue_ru_count; ru++) {
    if (ru->trx_stop_func)
      ru->trx_stop_func(ru);
    ru->firstTS_initialized = false;
    if (ru->trx_start_func && ru->trx_start_func(ru) < 0)
      rc = -1;
  }
  return rc;
}

/**
 * nrue_ru_reinit_impl: full re-open of every radio device: ends it (trx_end_func), re-loads it, and restarts it, retrying the load for about 40 s while the previous MPM claim expires.
 *   Deliberately does not assert on failure: returns -1 so the caller can stop the capture cleanly. Stops early when shutdown is requested. Called only through nrue_ru_reinit().
 *
 * FULL device teardown + re-open, for the case nrue_ru_restart_rx() cannot fix: the host has lost
 * its MPM claim on the X410, so every RPC to the device throws and no stream-level restart can
 * succeed. Releasing the multi_usrp handle (inside trx_end_func) is what drops the stale claim;
 * MPM then lets a fresh openair0_device_load() claim the device again.
 *
 * Deliberately NOT AssertFatal on failure, unlike nrue_ru_start(): this runs mid-capture, and the
 * caller has a defined fallback (stop the capture cleanly) that is better than aborting. */
static int nrue_ru_reinit_impl(void)
{
  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    openair0_device_t *dev = &openair0_dev[ru_id];
    if (dev->trx_end_func)
      dev->trx_end_func(dev); /* nulls the hooks and frees priv, so this cannot double-free */
    dev->host_type = RAU_HOST;
    /* MPM holds the stale claim for ~20 s after the previous session dies (MEASURED: a re-claim
     * is refused with "Someone tried to claim this device again" at t=10 s and succeeds by
     * t=20 s). When the claim was lost because the RPC link stalled, our release could not
     * un-claim it, so the device is still marked claimed and the first attempts MUST fail.
     * Retry across that window rather than giving up on the first refusal. */
#define MPMD_CLAIM_EXPIRY_ATTEMPTS 8
#define MPMD_CLAIM_EXPIRY_SLEEP_S  5
    int loaded = -1;
    for (int attempt = 0; attempt < MPMD_CLAIM_EXPIRY_ATTEMPTS; attempt++) {
      if (attempt)
        for (int tick = 0; tick < 10 * MPMD_CLAIM_EXPIRY_SLEEP_S && !nr_shutdown_requested(); tick++)
          usleep(100000);
      if (nr_shutdown_requested())
        return -1;
      loaded = openair0_device_load(dev, &openair0_cfg_g[ru_id]);
      if (loaded == 0)
        break;
      LOG_W(HW, "nrue_ru_reinit: device %d still unavailable (attempt %d/%d)\n",
            ru_id, attempt + 1, MPMD_CLAIM_EXPIRY_ATTEMPTS);
    }
    if (loaded != 0) {
      LOG_E(HW, "nrue_ru_reinit: could not re-open device %d\n", ru_id);
      return -1;
    }
    if (nr_shutdown_requested())
      return -1;
    dev->firstTS_initialized = false;
    if (dev->trx_start_func == NULL || dev->trx_start_func(dev) < 0) {
      LOG_E(HW, "nrue_ru_reinit: could not restart device %d\n", ru_id);
      return -1;
    }
    if (usrp_tx_thread == 1 && dev->trx_write_init)
      dev->trx_write_init(dev);
  }
  return 0;
}

/* nrue_ru_end_impl: collects the final statistics (trx_get_stats_func) and ends every radio device (trx_end_func). Called only through nrue_ru_end(). */
static void nrue_ru_end_impl(void)
{
  for (openair0_device_t *ru = openair0_dev; ru < openair0_dev + nrue_ru_count; ru++) {
    if (ru->trx_get_stats_func)
      ru->trx_get_stats_func(ru);
    if (ru->trx_end_func)
      ru->trx_end_func(ru);
  }
}

/* nrue_band_scan_active: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:35, executables/nr-ue.c:1364.
 */
bool nrue_band_scan_active(void)
{
  return s_band_scan_n > 1;
}
/* nrue_band_scan_next: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.c:825, executables/nr-ue-ru.h:33, executables/nr-ue-ru.h:36, executables/nr-ue.c:1365.
 */
int nrue_band_scan_next(PHY_VARS_NR_UE *UE)
{
  if (s_band_scan_n <= 1)
    return 0;
  s_band_scan_i = (s_band_scan_i + 1) % s_band_scan_n;
  NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  const uint64_t dl = s_band_scan_c[s_band_scan_i];
  const int64_t duplex = (int64_t)fp->ul_CarrierFreq - (int64_t)fp->dl_CarrierFreq;
  LOG_W(PHY, "SENSING: BANDSCAN window %d/%d: retuning every channel to %lu Hz and re-initialising the device\n",
        s_band_scan_i + 1, s_band_scan_n, dl);
  /* All channels, via the device re-init that is proven to survive on the X410 (a bare live retune
   * moved channel 0 only and killed 3/3 runs -- CFOTRK's own record). */
  nrue_ru_set_freq(UE, dl + duplex, dl, UE->common_vars.freq_offset);
  fp->dl_CarrierFreq = dl;
  fp->ul_CarrierFreq = dl + duplex;
  init_symbol_rotation(fp);
  if (nrue_ru_reinit() != 0) {
    LOG_E(PHY, "SENSING: BANDSCAN device re-init FAILED at %lu Hz\n", dl);
    return -1;
  }
  return 0;
}

/* nrue_ru_set_freq_impl: retunes the radio card of a UE: picks the RU whose current cell is nearest to the requested DL/UL carriers among cells not used by another UE, then applies the new frequency.
 *   Updates the cell ownership (used_by_ue) of the old and the new cell. Called only through nrue_ru_set_freq().
 */
static void nrue_ru_set_freq_impl(PHY_VARS_NR_UE *UE, uint64_t ul_carrier, uint64_t dl_carrier, int freq_offset)
{
  int current_cell_id = nrue_rus[UE->rf_map.card].used_by_cell;
  NR_DL_FRAME_PARMS *fp0 = &nrue_cell_fp[current_cell_id];
  uint64_t dl_carrier_distance =
      fp0->dl_CarrierFreq > dl_carrier ? fp0->dl_CarrierFreq - dl_carrier : dl_carrier - fp0->dl_CarrierFreq;
  uint64_t ul_carrier_distance =
      fp0->ul_CarrierFreq > ul_carrier ? fp0->ul_CarrierFreq - ul_carrier : ul_carrier - fp0->ul_CarrierFreq;
  uint64_t carrier_distance = dl_carrier_distance + ul_carrier_distance;

  for (int ru_id = 0; carrier_distance != 0 && ru_id < nrue_ru_count; ru_id++) {
    int cell_id = nrue_rus[ru_id].used_by_cell;
    if (cell_id < 0) // skip RUs that have no cell definition
      continue;
    if (nrue_cells[cell_id].used_by_ue >= 0) // skip cells that are already used by an UE
      continue;

    fp0 = &nrue_cell_fp[cell_id];
    uint64_t this_dl_carrier_distance =
        fp0->dl_CarrierFreq > dl_carrier ? fp0->dl_CarrierFreq - dl_carrier : dl_carrier - fp0->dl_CarrierFreq;
    uint64_t this_ul_carrier_distance =
        fp0->ul_CarrierFreq > ul_carrier ? fp0->ul_CarrierFreq - ul_carrier : ul_carrier - fp0->ul_CarrierFreq;
    uint64_t this_carrier_distance = this_dl_carrier_distance + this_ul_carrier_distance;
    if (this_carrier_distance < carrier_distance) {
      UE->rf_map.card = ru_id;
      carrier_distance = this_carrier_distance;
    }
  }

  nrue_cells[current_cell_id].used_by_ue = -1;
  current_cell_id = nrue_rus[UE->rf_map.card].used_by_cell;
  nrue_cells[current_cell_id].used_by_ue = UE->Mod_id;

  openair0_config_t *cfg = &openair0_cfg_g[UE->rf_map.card];
  openair0_device_t *dev = &openair0_dev[UE->rf_map.card];
  nr_rf_card_config_freq(cfg, ul_carrier, dl_carrier, freq_offset);
  dev->trx_set_freq_func(dev, cfg);
}

/* nrue_ru_adjust_rx_gain_impl: changes the RX gain of the UE's card by gain_change dB, retrying with the clipped remainder when the device rejects the value.
 *   Also informs the PRACH passive gain tracker and, with the sensing enabled, emits a session record. Returns the applied change. Called only through nrue_ru_adjust_rx_gain().
 */
static int nrue_ru_adjust_rx_gain_impl(PHY_VARS_NR_UE *UE, int gain_change)
{
  openair0_config_t *cfg = &openair0_cfg_g[UE->rf_map.card];
  openair0_device_t *dev = &openair0_dev[UE->rf_map.card];

  // Increase the RX gain by the value determined by adjust_rxgain
  cfg->rx_gain[0] += gain_change;

  // Set new RX gain.
  int ret_gain = dev->trx_set_gains_func(dev, cfg);
  // APPLY RX gain again if crossed the MAX RX gain threshold
  if (ret_gain < 0) {
    gain_change += ret_gain;
    cfg->rx_gain[0] += ret_gain;
    ret_gain = dev->trx_set_gains_func(dev, cfg);
  }

  int applied_rxgain = cfg->rx_gain[0] - cfg->rx_gain_offset[0];
  nr_prach_passive_gain_changed(applied_rxgain);
  LOG_I(HW, "Rxgain adjusted by %d dB, RX gain: %d dB \n", gain_change, applied_rxgain);
  if (nr_sense_enabled()) nr_sense_emit_session(&UE->frame_parms, applied_rxgain);

  return gain_change;
}

/* nrue_ru_read_impl: reads nsamps from the UE's radio card and, for UE 0, also reads (and discards) one block from every RU not used by another UE so that they keep streaming.
 *   Short reads are continued up to 20 times on contiguous timestamps; any gap or failure sets the RFSTALL shutdown and returns -1 (never invalid IQ). The returned timestamp is relative to the device's first timestamp.
 *   Called only through nrue_ru_read(). Injected failures (ISAC_RF_FAIL_AFTER_READS) take this path too.
 */
static int nrue_ru_read_impl(PHY_VARS_NR_UE *UE, openair0_timestamp_t *ptimestamp, void **buff, int nsamps, int num_antennas)
{
  openair0_device_t *dev = &openair0_dev[UE->rf_map.card];
  openair0_timestamp_t tmp_timestamp = 0;
  bool inject_failure = rf_fail_after_reads && atomic_fetch_add(&rf_read_count, 1) >= rf_fail_after_reads;
  int ret = inject_failure ? -1 : dev->trx_read_func(dev, &tmp_timestamp, buff, nsamps, num_antennas);
  if (ret < 0) {
    *ptimestamp = 0;
    bool was_stopping = nr_shutdown_requested();
    oai_exit = 1;
    nr_shutdown_request(3);
    if (!was_stopping)
      LOG_E(HW, "SENSING: RFSTALL radio read failed; refusing invalid IQ\n");
    return ret;
  }
  openair0_timestamp_t first_timestamp = tmp_timestamp;
  bool have_timestamp = ret > 0;

  if (ret >= 0 && ret < nsamps) {
    LOG_W(HW, "Short RX read: got %d of %d samples, retrying\n", ret, nsamps);
    for (int retry = 0; ret < nsamps && retry < 20 && !nr_shutdown_requested(); retry++) {
      void *retry_buf[num_antennas];
      for (int ant = 0; ant < num_antennas; ant++)
        retry_buf[ant] = (char *)buff[ant] + (size_t)ret * sizeof(int32_t);

      openair0_timestamp_t retry_timestamp;
      int got = dev->trx_read_func(dev, &retry_timestamp, retry_buf, nsamps - ret, num_antennas);
      if (got < 0 || (got > 0 && have_timestamp && retry_timestamp != first_timestamp + ret)) {
        *ptimestamp = 0;
        bool was_stopping = nr_shutdown_requested();
        oai_exit = 1;
        nr_shutdown_request(3);
        if (!was_stopping)
          LOG_E(HW, "SENSING: RFSTALL failed/non-contiguous short-read continuation\n");
        return -1;
      }
      if (got > 0) {
        if (!have_timestamp) {
          first_timestamp = retry_timestamp;
          have_timestamp = true;
        }
        ret += got;
      }
    }
    if (ret < nsamps) {
      *ptimestamp = 0;
      oai_exit = 1;
      nr_shutdown_request(3);
      LOG_E(HW, "SENSING: RFSTALL short RX read exhausted: got %d of %d samples\n", ret, nsamps);
      return -1;
    }
  }

  if (!have_timestamp) {
    *ptimestamp = 0;
    return ret;
  }
  tmp_timestamp = first_timestamp;
  if (!dev->firstTS_initialized) {
    dev->firstTS = tmp_timestamp;
    dev->firstTS_initialized = true;
  }
  *ptimestamp = tmp_timestamp - dev->firstTS;

  if (UE->Mod_id != 0)
    return ret;

  // UE 0 needs to read from all RUs that are not used by any other UE

  void *tmp_buf[num_antennas];
  uint32_t tmp_samples[nsamps];
  for (int ant = 0; ant < num_antennas; ant++)
    tmp_buf[ant] = tmp_samples;

  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    int cell_id = nrue_rus[ru_id].used_by_cell;
    if (cell_id < 0) // skip RUs that have no cell definition
      continue;
    int ue_id = nrue_cells[cell_id].used_by_ue;
    if (ue_id >= 0) // skip cells that are already used by an UE
      continue;

    dev = &openair0_dev[ru_id];
    if (dev->trx_read_func(dev, &tmp_timestamp, tmp_buf, nsamps, num_antennas) != nsamps) {
      oai_exit = 1;
      nr_shutdown_request(3);
      return -1;
    }
    if (!dev->firstTS_initialized) {
      dev->firstTS = tmp_timestamp;
      dev->firstTS_initialized = true;
    }
  }

  return ret;
}

/* nrue_ru_write_impl: sends samples on the UE's radio card and, for UE 0, sends zeros on every RU not used by another UE.
 *   Timestamps are offset by the device's first timestamp. Called only through nrue_ru_write().
 */
static int nrue_ru_write_impl(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **buff, int nsamps, int num_antennas, int flags)
{
  openair0_device_t *dev = &openair0_dev[UE->rf_map.card];
  int ret = dev->trx_write_func(dev, timestamp + dev->firstTS, buff, nsamps, num_antennas, flags);

  if (UE->Mod_id != 0)
    return ret;

  // UE 0 needs to write to all RUs that are not used by any other UE

  void *tmp_buf[num_antennas];
  uint32_t tmp_samples[nsamps];
  memset(tmp_samples, 0, sizeof(tmp_samples));
  for (int ant = 0; ant < num_antennas; ant++)
    tmp_buf[ant] = tmp_samples;

  for (int ru_id = 0; ru_id < nrue_ru_count; ru_id++) {
    int cell_id = nrue_rus[ru_id].used_by_cell;
    if (cell_id < 0) // skip RUs that have no cell definition
      continue;
    int ue_id = nrue_cells[cell_id].used_by_ue;
    if (ue_id >= 0) // skip cells that are already used by an UE
      continue;

    dev = &openair0_dev[ru_id];
    dev->trx_write_func(dev, timestamp + dev->firstTS, tmp_buf, nsamps, num_antennas, flags);
  }
  return ret;
}

/* nrue_ru_write_reorder_impl: writes TX samples through openair0_write_reorder_common() on the UE's card, so that out-of-order TX blocks are reordered before nrue_ru_write_impl() sends them.
 *   Called only through nrue_ru_write_reorder().
 */
static int nrue_ru_write_reorder_impl(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **txp, int nsamps, int nbAnt, int flags)
{
  openair0_device_t *device = &openair0_dev[UE->rf_map.card];
  return openair0_write_reorder_common(nrue_ru_write_impl, UE, device, timestamp, txp, nsamps, nbAnt, flags);
}

/* nrue_ru_write_reorder_clear_context_impl: drops the TX reorder context of the UE's card (openair0_write_reorder_clear_context). Called only through nrue_ru_write_reorder_clear_context(). */
static void nrue_ru_write_reorder_clear_context_impl(PHY_VARS_NR_UE *UE)
{
  openair0_device_t *device = &openair0_dev[UE->rf_map.card];
  openair0_write_reorder_clear_context(device);
}

/* Shared lifetime gate: cancellation may run concurrently with read/write. */
/* nrue_ru_shutdown_stop: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:29.
 */
void nrue_ru_shutdown_stop(void)
{
  for (int i = 0; i < MAX_CARDS; i++)
    if (openair0_dev[i].trx_stop_func)
      openair0_dev[i].trx_stop_func(&openair0_dev[i]);
}

/* The shutdown coordinator holds the exclusive device lifetime gate here. */
/* nrue_ru_shutdown: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue-ru.h:28.
 */
void nrue_ru_shutdown(void)
{
  for (int i = 0; i < MAX_CARDS; i++)
    if (openair0_dev[i].trx_end_func)
      openair0_dev[i].trx_end_func(&openair0_dev[i]);
}

/* nrue_ru_start: public: starts all radio devices under the exclusive radio lifetime gate (nr_shutdown_radio_enter(1)). Returns the start result, or -1 when shutdown is in progress.
 *   Caller: executables/nr-uesoftmodem.c (receiver start-up).
 */
int nrue_ru_start(void)
{
  if (!nr_shutdown_radio_enter(1))
    return -1;
  int result = nrue_ru_start_impl();
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_stop: public: stops the RX streams under the exclusive lifetime gate; does nothing when shutdown is already in progress. Caller: nr-ue-ru.h API (receiver stop path). */
void nrue_ru_stop(void)
{
  if (!nr_shutdown_radio_enter(1))
    return;
  nrue_ru_stop_impl();
  nr_shutdown_radio_leave();
}

/* nrue_ru_end: public: ends all radio devices under the exclusive lifetime gate; does nothing during shutdown. Caller: nr-ue-ru.h API (receiver teardown). */
void nrue_ru_end(void)
{
  if (!nr_shutdown_radio_enter(1))
    return;
  nrue_ru_end_impl();
  nr_shutdown_radio_leave();
}

/* nrue_ru_restart_rx: public: restarts the RX streams (stop, clear origin, start) under the exclusive gate. Used by the RFSTALL recovery in nr-ue.c. Returns -1 on failure. */
int nrue_ru_restart_rx(void)
{
  if (!nr_shutdown_radio_enter(1))
    return -1;
  int result = nrue_ru_restart_rx_impl();
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_reinit: public: full device teardown and re-open (nrue_ru_reinit_impl) under the exclusive gate. Used by the RFSTALL recovery and the band-scan re-tune (nrue_band_scan_next) in this file and nr-ue.c. */
int nrue_ru_reinit(void)
{
  if (!nr_shutdown_radio_enter(1))
    return -1;
  int result = nrue_ru_reinit_impl();
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_set_freq: public: retunes the UE's card under the shared gate (nr_shutdown_radio_enter(0)). Callers: phy_procedures_nr_ue.c, nr_pdsch_passive_decode.c, nr-ue.c and nrue_band_scan_next(). */
void nrue_ru_set_freq(PHY_VARS_NR_UE *UE, uint64_t ul_carrier, uint64_t dl_carrier, int freq_offset)
{
  if (!nr_shutdown_radio_enter(0))
    return;
  nrue_ru_set_freq_impl(UE, ul_carrier, dl_carrier, freq_offset);
  nr_shutdown_radio_leave();
}

/* nrue_ru_adjust_rx_gain: public: changes the RX gain under the shared gate; returns the applied gain change (or -1 during shutdown). Callers: nr-ue.c (AGC). */
int nrue_ru_adjust_rx_gain(PHY_VARS_NR_UE *UE, int gain_change)
{
  if (!nr_shutdown_radio_enter(0))
    return -1;
  int result = nrue_ru_adjust_rx_gain_impl(UE, gain_change);
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_read: public: reads samples under the shared gate; sets *ptimestamp to 0 first and returns the sample count, or -1 during shutdown or on fault. Callers: nr-ue.c read loop, phy_procedures_nr_ue.c and the passive queues. */
int nrue_ru_read(PHY_VARS_NR_UE *UE, openair0_timestamp_t *ptimestamp, void **buff, int nsamps, int num_antennas)
{
  *ptimestamp = 0;
  if (!nr_shutdown_radio_enter(0))
    return -1;
  int result = nrue_ru_read_impl(UE, ptimestamp, buff, nsamps, num_antennas);
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_write: public: writes samples under the shared gate; returns the write result, or -1 during shutdown. Callers: nr-ue.c and radio/COMMON/common_lib.c. */
int nrue_ru_write(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **buff, int nsamps, int num_antennas, int flags)
{
  if (!nr_shutdown_radio_enter(0))
    return -1;
  int result = nrue_ru_write_impl(UE, timestamp, buff, nsamps, num_antennas, flags);
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_write_reorder: public: writes TX samples through the reorder path under the shared gate; returns -1 during shutdown. Callers: nr-ue.c (TX path). */
int nrue_ru_write_reorder(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, void **txp, int nsamps, int nbAnt, int flags)
{
  if (!nr_shutdown_radio_enter(0))
    return -1;
  int result = nrue_ru_write_reorder_impl(UE, timestamp, txp, nsamps, nbAnt, flags);
  nr_shutdown_radio_leave();
  return result;
}

/* nrue_ru_write_reorder_clear_context: public: clears the TX reorder context under the shared gate; does nothing during shutdown. Callers: nr-ue.c. */
void nrue_ru_write_reorder_clear_context(PHY_VARS_NR_UE *UE)
{
  if (!nr_shutdown_radio_enter(0))
    return;
  nrue_ru_write_reorder_clear_context_impl(UE);
  nr_shutdown_radio_leave();
}

#ifdef NRUE_RU_SHUTDOWN_TEST
/* Hardware-free integration fixture; not present in the receiver binary. */
/* nrue_ru_test_device: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: common/utils/tests/nr_shutdown_test.c:15, common/utils/tests/nr_shutdown_test.c:117, executables/nr-ue-ru.c:372.
 */
void nrue_ru_test_device(openair0_device_t device)
{
  static nrUE_RU_params_t ru = {.used_by_cell = 0};
  static nrUE_cell_params_t cell = {.used_by_ue = 0};
  nrue_ru_count = nrue_cell_count = 1;
  nrue_rus = &ru;
  nrue_cells = &cell;
  openair0_dev[0] = device;
  nrue_ru_fault_config();
}
#endif
