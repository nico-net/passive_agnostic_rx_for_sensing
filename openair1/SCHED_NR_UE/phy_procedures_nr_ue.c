/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Implementation of UE procedures from 36.213 LTE specifications
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdatomic.h>
#include <time.h>
// TEMPORARY DIAGNOSTIC (2026-08-05): producer-side counters, defined in executables/nr-ue.c. See
// that file's own comment and PBCH_TRACKING_BUFFER_HANDOVER.md.
extern _Atomic long nr_ue_diag_producer_absolute_slot;
extern _Atomic long nr_ue_diag_producer_wall_ns;
#include "nr/nr_common.h"
#include "assertions.h"
#include "defs.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/INIT/nr_phy_init.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_REFSIG/ptrs_nr.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "SCHED_NR_UE/phy_sch_processing_time.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/NR_UE_ISAC/nr_isac_ssb_axis.h"
#include "PHY/NR_UE_TRANSPORT/nr_csirs_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "executables/softmodem-common.h"
#include "executables/nr-uesoftmodem.h"
#include "SCHED_NR_UE/pucch_uci_ue_nr.h"
#include <openair1/PHY/TOOLS/phy_scope_interface.h>
#include "nfapi/open-nFAPI/nfapi/public_inc/nfapi_nr_interface.h"

//#define DEBUG_PHY_PROC
//#define NR_PDCCH_SCHED_DEBUG
//#define NR_PUCCH_SCHED
//#define NR_PUCCH_SCHED_DEBUG
//#define NR_PDSCH_DEBUG

#ifndef PUCCH
#define PUCCH
#endif

#include "common/utils/LOG/log.h"

#include "UTIL/OPT/opt.h"
#include "intertask_interface.h"
#include "T.h"
#include "instrumentation.h"
#include "executables/nr-ue-ru.h" // nrue_ru_set_freq() -- CFO trim loop, see CFOTRK below
/* Set by the CFO trim loop, consumed by the UE thread in nr-ue.c: the corrected total offset to
 * retune to, plus a one-shot request flag. Split this way because only the UE thread owns the
 * stream/timing state a clean re-acquisition has to reset. */
_Atomic int nr_ue_cfo_resync_request = 0;
int         nr_ue_cfo_resync_hz = 0;

static const unsigned int gain_table[31] = {100,  112,  126,  141,  158,  178,  200,  224,  251, 282,  316,
                                            359,  398,  447,  501,  562,  631,  708,  794,  891, 1000, 1122,
                                            1258, 1412, 1585, 1778, 1995, 2239, 2512, 2818, 3162};

static void nr_ue_prach_procedures(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, c16_t **txData);

static uint32_t get_ssb_arfcn(NR_DL_FRAME_PARMS *frame_parms)
{
  uint32_t band_size_hz = frame_parms->N_RB_DL * 12 * frame_parms->subcarrier_spacing;
  int ssb_center_sc = frame_parms->ssb_start_subcarrier + 120; // ssb is 20 PRBs -> 240 sub-carriers
  uint64_t ssb_freq = frame_parms->dl_CarrierFreq - (band_size_hz / 2) + frame_parms->subcarrier_spacing * ssb_center_sc;
  return to_nrarfcn(ssb_freq);
}

void nr_fill_rx_indication(fapi_nr_rx_indication_t *rx_ind,
                           uint8_t pdu_type,
                           PHY_VARS_NR_UE *ue,
                           int cw_idx,
                           int harq_pid,
                           NR_UE_DLSCH_t *dlsch,
                           const UE_nr_rxtx_proc_t *proc,
                           void *typeSpecific)
{
  AssertFatal(rx_ind->number_pdus < NFAPI_RX_IND_MAX_PDU - 1, "Exceeded rx_ind array size\n");
  fapi_nr_rx_indication_body_t *rx = rx_ind->rx_indication_body + rx_ind->number_pdus;
  rx_ind->number_pdus++;
  *rx = (fapi_nr_rx_indication_body_t){.pdu_type = pdu_type};
  switch (pdu_type){
    case FAPI_NR_RX_PDU_TYPE_SIB:
    case FAPI_NR_RX_PDU_TYPE_RAR:
    case FAPI_NR_RX_PDU_TYPE_DLSCH:
    case FAPI_NR_RX_PDU_TYPE_PCCH:
      if(dlsch) {
        NR_DL_UE_HARQ_t *dl_harq = &ue->dl_harq_processes[cw_idx][harq_pid];
        rx->pdsch_pdu.harq_pid = harq_pid;
        rx->pdsch_pdu.cw_idx = cw_idx;
        rx->pdsch_pdu.ack_nack = dl_harq->decodeResult;
        rx->pdsch_pdu.pdu = (uint8_t *)typeSpecific;
        rx->pdsch_pdu.pdu_length = dlsch->cw_info.TBS / 8;
        if (dl_harq->decodeResult) {
          int t = WS_C_RNTI;
          if (pdu_type == FAPI_NR_RX_PDU_TYPE_RAR)
            t = WS_RA_RNTI;
          if (pdu_type == FAPI_NR_RX_PDU_TYPE_SIB)
            t = WS_SI_RNTI;
          if (pdu_type == FAPI_NR_RX_PDU_TYPE_PCCH)
            t = WS_P_RNTI;
          ws_trace_t tmp = {.nr = true,
                            .direction = DIRECTION_DOWNLINK,
                            .type = ue->frame_parms.frame_type == FDD ? FDD_RADIO : TDD_RADIO,
                            .pdu_buffer = rx->pdsch_pdu.pdu,
                            .pdu_buffer_size = rx->pdsch_pdu.pdu_length,
                            .ueid = 0,
                            .rntiType = t,
                            .rnti = dlsch->rnti,
                            .sysFrame = proc->frame_rx,
                            .subframe = proc->nr_slot_rx,
                            .harq_pid = harq_pid};
          trace_pdu(&tmp);
        }
      }
      break;
    case FAPI_NR_RX_PDU_TYPE_SSB: {
      if (typeSpecific) {
        NR_DL_FRAME_PARMS *frame_parms = &ue->frame_parms;
        fapiPbch_t *pbch = (fapiPbch_t *)typeSpecific;
        memcpy(rx->ssb_pdu.pdu, pbch->decoded_output, sizeof(pbch->decoded_output));
        rx->ssb_pdu.additional_bits = pbch->xtra_byte;
        rx->ssb_pdu.ssb_index = (frame_parms->ssb_index) & 0x7;
        rx->ssb_pdu.ssb_length = frame_parms->Lmax;
        rx->ssb_pdu.cell_id = frame_parms->Nid_cell;
        rx->ssb_pdu.ssb_start_subcarrier = frame_parms->ssb_start_subcarrier;
        rx->ssb_pdu.arfcn = get_ssb_arfcn(frame_parms);
        rx->ssb_pdu.radiolink_monitoring = RLM_in_sync; // TODO to be removed from here
        rx->ssb_pdu.decoded_pdu = true;
      } else {
        /* A failed MIB re-decode does NOT mean radio-link failure for a passive receiver.
         *
         * These two assignments carry upstream's own "TODO to be removed from here" and are not
         * spec-conformant: 38.133 sec 8.1 derives RLM from RLM-RS *quality*, not from whether a
         * MIB happened to decode. In normal operation the UE attaches within seconds and the
         * shortcut rarely bites; a --passive-rx receiver never attaches, so it sits in the
         * PBCH-tracking-only regime indefinitely and every failed MIB drives
         * handle_rlm -> nr_mac_rrc_sync_ind -> N310 -> T310 -> RRC IDLE -> full re-acquisition.
         *
         * MEASURED (2026-08-02, live 100 MHz cell): with DL traffic on the cell, PBCH tracking
         * fails on essentially every SSB occasion (1827-1943 failures / 90 s) and the receiver
         * re-acquired 32-34 times, while the SAME run still produced valid sensing CPIs. With the
         * cell idle: 0 failures. Since traffic IS the illuminator for passive sensing, obeying
         * this shortcut would make the receiver tear its own timing down exactly when it is
         * supposed to be collecting.
         *
         * Timing is demonstrably still good across these failures -- the SSB burst stays on
         * symbols 2-6 and initial sync succeeds every time -- and the sensing CFR taps come from
         * CSI-RS / PDSCH DM-RS / data-aided paths, NOT from PBCH channel estimation. So in passive
         * mode we keep the previous RLM state instead of declaring out-of-sync. Active mode is
         * untouched: it still needs MIB re-decode as its (imperfect) liveness signal.
         */
        if (IS_PASSIVE_RX_MODE(get_softmodem_params())) {
          rx->ssb_pdu.radiolink_monitoring = RLM_in_sync;
        } else {
          rx->ssb_pdu.radiolink_monitoring = RLM_out_of_sync; // TODO to be removed from here
        }
        rx->ssb_pdu.decoded_pdu = false;
      }
    } break;
    case FAPI_NR_MEAS_IND:
      memcpy(&rx->l1_measurements, typeSpecific, sizeof(fapi_nr_l1_measurements_t));
      break;
    default:
    break;
  }
}

int get_tx_amp_prach(int power_dBm, int power_max_dBm, int N_RB_UL){

  int gain_dB = power_dBm - power_max_dBm, amp_x_100 = -1;

  switch (N_RB_UL) {
  case 6:
  amp_x_100 = AMP;      // PRACH is 6 PRBS so no scale
  break;
  case 15:
  amp_x_100 = 158*AMP;  // 158 = 100*sqrt(15/6)
  break;
  case 25:
  amp_x_100 = 204*AMP;  // 204 = 100*sqrt(25/6)
  break;
  case 50:
  amp_x_100 = 286*AMP;  // 286 = 100*sqrt(50/6)
  break;
  case 75:
  amp_x_100 = 354*AMP;  // 354 = 100*sqrt(75/6)
  break;
  case 100:
  amp_x_100 = 408*AMP;  // 408 = 100*sqrt(100/6)
  break;
  default:
  LOG_E(PHY, "Unknown PRB size %d\n", N_RB_UL);
  return (amp_x_100);
  break;
  }
  if (gain_dB < -30) {
    return (amp_x_100/3162);
  } else if (gain_dB > 0)
    return (amp_x_100);
  else
    return (amp_x_100/gain_table[-gain_dB]);  // 245 corresponds to the factor sqrt(25/6)

  return (amp_x_100);
}

// UL time alignment procedures:
// - If the current tx frame and slot match the TA configuration
//   then timing advance is processed and set to be applied in the next UL transmission
// - Application of timing adjustment according to TS 38.213 p4.2
// - handle RAR TA application as per ch 4.2 TS 38.213
void ue_ta_procedures(PHY_VARS_NR_UE *ue, int slot_tx, int frame_tx)
{
  if (frame_tx == ue->ta_frame && slot_tx == ue->ta_slot) {
    uint16_t ofdm_symbol_size = ue->frame_parms.ofdm_symbol_size;

    // convert time factor "16 * 64 * T_c / (2^mu)" in N_TA calculation in TS38.213 section 4.2 to samples by multiplying with
    // samples per second
    //   16 * 64 * T_c            / (2^mu) * samples_per_second
    // = 16 * T_s                 / (2^mu) * samples_per_second
    // = 16 * 1 / (15 kHz * 2048) / (2^mu) * (15 kHz * 2^mu * ofdm_symbol_size)
    // = 16 * 1 /           2048           *                  ofdm_symbol_size
    // = 16 * ofdm_symbol_size / 2048
    uint16_t bw_scaling = 16 * ofdm_symbol_size / 2048;

    ue->timing_advance += (ue->ta_command - 31) * bw_scaling;

    LOG_D(PHY,
          "[UE %d] [%d.%d] Got timing advance command %u from MAC, new value is %d\n",
          ue->Mod_id,
          frame_tx,
          slot_tx,
          ue->ta_command,
          ue->timing_advance);

    ue->ta_frame = -1;
    ue->ta_slot = -1;
  }
}

static void configure_srs_info(const fapi_nr_ul_config_srs_pdu *srs_config_pdu, nr_srs_info_t *nr_srs_info)
{
  nr_srs_info->B_SRS = srs_config_pdu->bandwidth_index;
  nr_srs_info->C_SRS = srs_config_pdu->config_index;
  nr_srs_info->b_hop = srs_config_pdu->frequency_hopping;
  nr_srs_info->comb_size = srs_config_pdu->comb_size;
  nr_srs_info->K_TC_overbar = srs_config_pdu->comb_offset;
  nr_srs_info->n_SRS_cs = srs_config_pdu->cyclic_shift;
  nr_srs_info->n_ID_SRS = srs_config_pdu->sequence_id;
  // It adjusts the SRS allocation to align with the common resource block grid in multiples of four
  nr_srs_info->n_shift = srs_config_pdu->frequency_shift;
  nr_srs_info->n_RRC = srs_config_pdu->frequency_position;
  nr_srs_info->groupOrSequenceHopping = srs_config_pdu->group_or_sequence_hopping;
  nr_srs_info->l_offset = srs_config_pdu->time_start_position;
  nr_srs_info->T_SRS = srs_config_pdu->t_srs;
  nr_srs_info->T_offset = srs_config_pdu->t_offset;
  nr_srs_info->R = 1 << srs_config_pdu->num_repetitions;
  nr_srs_info->N_symb_SRS = 1 << srs_config_pdu->num_symbols; // Number of consecutive OFDM symbols
  nr_srs_info->n_srs_ports = 1 << srs_config_pdu->num_ant_ports; // Number of antenna port for transmission
  nr_srs_info->resource_type = srs_config_pdu->resource_type;
}

/*******************************************************************
*
* NAME :         ue_srs_procedures_nr
*
* PARAMETERS :   pointer to ue context
*                pointer to rxtx context*
*
* DESCRIPTION :  ue srs procedure
*                send srs according to current configuration
*
*********************************************************************/
void ue_srs_procedures_nr(PHY_VARS_NR_UE *ue,
                          const UE_nr_rxtx_proc_t *proc,
                          c16_t **txdataF,
                          const fapi_nr_ul_config_srs_pdu *srs_config_pdu,
                          bool was_symbol_used[NR_SYMBOLS_PER_SLOT])
{
  NR_DL_FRAME_PARMS *frame_parms = &(ue->frame_parms);
  const uint8_t l0 = srs_config_pdu->time_start_position; // L2 sends the absolute symbol index
  // Num consecutive SRS symbols according to 38.211 6.4.1.4.1
  int num_srs_symbols[] = {1, 2, 4, 8, 12};
  int last_srs_symbol = l0 + num_srs_symbols[srs_config_pdu->num_symbols] - 1;
  for (int i = l0; i <= last_srs_symbol; i++) {
    was_symbol_used[i] = true;
  }

#ifdef SRS_DEBUG
  LOG_I(NR_PHY,"Frame = %i, slot = %i\n", proc->frame_tx, proc->nr_slot_tx);
  LOG_I(NR_PHY,"srs_config_pdu->rnti = 0x%04x\n", srs_config_pdu->rnti);
  LOG_I(NR_PHY,"srs_config_pdu->handle = %u\n", srs_config_pdu->handle);
  LOG_I(NR_PHY,"srs_config_pdu->bwp_size = %u\n", srs_config_pdu->bwp_size);
  LOG_I(NR_PHY,"srs_config_pdu->bwp_start = %u\n", srs_config_pdu->bwp_start);
  LOG_I(NR_PHY,"srs_config_pdu->subcarrier_spacing = %u\n", srs_config_pdu->subcarrier_spacing);
  LOG_I(NR_PHY,"srs_config_pdu->cyclic_prefix = %u (0: Normal; 1: Extended)\n", srs_config_pdu->cyclic_prefix);
  LOG_I(NR_PHY,"srs_config_pdu->num_ant_ports = %u (0 = 1 port, 1 = 2 ports, 2 = 4 ports)\n", srs_config_pdu->num_ant_ports);
  LOG_I(NR_PHY,"srs_config_pdu->num_symbols = %u (0 = 1 symbol, 1 = 2 symbols, 2 = 4 symbols)\n", srs_config_pdu->num_symbols);
  LOG_I(NR_PHY,"srs_config_pdu->num_repetitions = %u (0 = 1, 1 = 2, 2 = 4)\n", srs_config_pdu->num_repetitions);
  LOG_I(NR_PHY,"srs_config_pdu->time_start_position = %u\n", srs_config_pdu->time_start_position);
  LOG_I(NR_PHY,"srs_config_pdu->config_index = %u\n", srs_config_pdu->config_index);
  LOG_I(NR_PHY,"srs_config_pdu->sequence_id = %u\n", srs_config_pdu->sequence_id);
  LOG_I(NR_PHY,"srs_config_pdu->bandwidth_index = %u\n", srs_config_pdu->bandwidth_index);
  LOG_I(NR_PHY,"srs_config_pdu->comb_size = %u (0 = comb size 2, 1 = comb size 4, 2 = comb size 8)\n", srs_config_pdu->comb_size);
  LOG_I(NR_PHY,"srs_config_pdu->comb_offset = %u\n", srs_config_pdu->comb_offset);
  LOG_I(NR_PHY,"srs_config_pdu->cyclic_shift = %u\n", srs_config_pdu->cyclic_shift);
  LOG_I(NR_PHY,"srs_config_pdu->frequency_position = %u\n", srs_config_pdu->frequency_position);
  LOG_I(NR_PHY,"srs_config_pdu->frequency_shift = %u\n", srs_config_pdu->frequency_shift);
  LOG_I(NR_PHY,"srs_config_pdu->frequency_hopping = %u\n", srs_config_pdu->frequency_hopping);
  LOG_I(NR_PHY,"srs_config_pdu->group_or_sequence_hopping = %u (0 = No hopping, 1 = Group hopping groupOrSequenceHopping, 2 = Sequence hopping)\n", srs_config_pdu->group_or_sequence_hopping);
  LOG_I(NR_PHY,"srs_config_pdu->resource_type = %u (0: aperiodic, 1: semi-persistent, 2: periodic)\n", srs_config_pdu->resource_type);
  LOG_I(NR_PHY,"srs_config_pdu->t_srs = %u\n", srs_config_pdu->t_srs);
  LOG_I(NR_PHY,"srs_config_pdu->t_offset = %u\n", srs_config_pdu->t_offset);
#endif

  nr_srs_info_t nr_srs_info = {0};
  configure_srs_info(srs_config_pdu, &nr_srs_info);
  uint16_t symbol_offset = l0 * frame_parms->ofdm_symbol_size;
  bool generated = generate_srs_nr(frame_parms,
                                   txdataF,
                                   symbol_offset,
                                   srs_config_pdu->bwp_start,
                                   &nr_srs_info,
                                   AMP,
                                   proc->frame_tx,
                                   proc->nr_slot_tx,
                                   frame_parms->nb_antennas_tx);
  DevAssert(generated); // if we can't generate despite the SRS config, there
                        // is a problem
}

void phy_procedures_nrUE_TX(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_tx_t *phy_data, c16_t **txp)
{
  const int slot_tx = proc->nr_slot_tx;
  const int frame_tx = proc->frame_tx;

  AssertFatal(ue->CC_id == 0, "Transmission on secondary CCs is not supported yet\n");

#if T_TRACER
  T(T_UE_PHY_UL_TICK, T_INT(ue->Mod_id), T_INT(frame_tx % 1024), T_INT(slot_tx));
#endif

  const int samplesF_per_slot = ue->frame_parms.symbols_per_slot * ue->frame_parms.ofdm_symbol_size;
  c16_t txdataF_buf[ue->frame_parms.nb_antennas_tx * samplesF_per_slot] __attribute__((aligned(32)));
  memset(txdataF_buf, 0, sizeof(txdataF_buf));
  c16_t *txdataF[ue->frame_parms.nb_antennas_tx]; /* workaround to be compatible with current txdataF usage in all tx procedures. */
  for(int i=0; i< ue->frame_parms.nb_antennas_tx; ++i)
    txdataF[i] = &txdataF_buf[i * samplesF_per_slot];

  LOG_D(PHY,"****** start TX-Chain for AbsSubframe %d.%d ******\n", frame_tx, slot_tx);
  bool was_symbol_used[NR_SYMBOLS_PER_SLOT] = {0};

  start_meas_nr_ue_phy(ue, PHY_PROC_TX);

  nr_ue_ulsch_procedures(ue, frame_tx, slot_tx, phy_data, (c16_t **)&txdataF, was_symbol_used);

  if (phy_data->srs_vars.active)
    ue_srs_procedures_nr(ue, proc, (c16_t **)&txdataF, &phy_data->srs_vars.srs_config_pdu, was_symbol_used);

  pucch_procedures_ue_nr(ue, proc, phy_data, (c16_t **)&txdataF, was_symbol_used);

  LOG_D(PHY, "Sending Uplink data \n");

  // Don't do OFDM Mod if txdata contains prach
  const NR_UE_PRACH *prach_var = ue->prach_vars[proc->gNB_id];
  if (!prach_var->active) {
    start_meas_nr_ue_phy(ue, OFDM_MOD_STATS);
    nr_tx_rotation_and_ofdm_mod(proc->nr_slot_tx,
                                &ue->frame_parms,
                                ue->frame_parms.nb_antennas_tx,
                                (c16_t **)txdataF,
                                txp,
                                link_type_ul,
                                was_symbol_used,
                                ue->no_phase_pre_comp);
    stop_meas_nr_ue_phy(ue, OFDM_MOD_STATS);
  }

  nr_ue_prach_procedures(ue, proc, txp);

  LOG_D(PHY, "****** end TX-Chain for AbsSubframe %d.%d ******\n", proc->frame_tx, proc->nr_slot_tx);

  stop_meas_nr_ue_phy(ue, PHY_PROC_TX);
}

static void nr_ue_measurement_procedures(uint16_t l,
                                         PHY_VARS_NR_UE *ue,
                                         const UE_nr_rxtx_proc_t *proc,
                                         int number_rbs,
                                         uint32_t pdsch_est_size,
                                         int32_t dl_ch_estimates[][pdsch_est_size])
{
  int nr_slot_rx = proc->nr_slot_rx;
  int gNB_id = proc->gNB_id;

  LOG_D(PHY,
        "Doing UE measurement procedures in symbol l %u Ncp %d nr_slot_rx %d, rxdata %p\n",
        l,
        ue->frame_parms.Ncp,
        nr_slot_rx,
        ue->common_vars.rxdata);
  nr_ue_measurements(ue, proc, number_rbs, l, pdsch_est_size, dl_ch_estimates);
#if T_TRACER
  if (nr_slot_rx == 0)
    T(T_UE_PHY_MEAS,
      T_INT(gNB_id),
      T_INT(proc->frame_rx % 1024),
      T_INT(nr_slot_rx),
      T_INT((int)(10 * log10(ue->measurements.rsrp[0]) - ue->rx_total_gain_dB)),
      T_INT((int)ue->measurements.rx_rssi_dBm[0]),
      T_INT((int)(ue->measurements.rx_power_avg_dB[0] - ue->measurements.n0_power_avg_dB)),
      T_INT((int)ue->measurements.rx_power_avg_dB[0]),
      T_INT((int)ue->measurements.n0_power_avg_dB),
      T_INT((int)ue->measurements.wideband_cqi_avg[0]),
      T_INT((int)ue->common_vars.freq_offset));
#endif

  // accumulate and filter timing offset estimation every subframe (instead of every frame)
  if (nr_slot_rx == 2) {
    // AGC
    //printf("start adjust gain power avg db %d\n", ue->measurements.rx_power_avg_dB[gNB_id]);
    phy_adjust_gain_nr (ue,ue->measurements.rx_power_avg_dB[gNB_id],gNB_id);
  }
}

static int nr_ue_pdsch_procedures(PHY_VARS_NR_UE *ue,
                                  const UE_nr_rxtx_proc_t *proc,
                                  NR_UE_DLSCH_t *dlsch,
                                  NR_DL_UE_HARQ_t *dlsch_harq,
                                  fapi_nr_dl_config_dlsch_pdu_rel15_t *dlschCfg,
                                  int16_t *llr,
                                  c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                  freq_alloc_bitmap_t *freq_alloc,
                                  uint32_t *nvar_out)
{
  int frame_rx = proc->frame_rx;
  int nr_slot_rx = proc->nr_slot_rx;

  // We handle only one CW now
  if (NR_MAX_NB_LAYERS > 4) {
    LOG_E(NR_PHY, "Handling of more than one CW not implemented\n");
    return -1;
  }

  int harq_pid = dlschCfg->harq_process_nbr;

  LOG_D(PHY,
        "[UE %d] frame_rx %d, nr_slot_rx %d, harq_pid %d (%d), BWP start %d, start RB %d, end RB %d, symbol_start %d, nb_symbols "
        "%d, DMRS mask "
        "%x, Nl %d\n",
        ue->Mod_id,
        frame_rx,
        nr_slot_rx,
        harq_pid,
        dlsch_harq->status,
        dlschCfg->BWPStart,
        freq_alloc->first_rb,
        freq_alloc->last_rb,
        dlschCfg->start_symbol,
        dlschCfg->number_symbols,
        dlschCfg->dlDmrsSymbPos,
        dlsch->cw_info.Nl);

  const int actor_idx = proc->nr_slot_rx % ue->pdsch_num_actors;
  pdsch_scratch_t *scratch = &ue->pdsch_scratch[actor_idx];
  const uint32_t pdsch_est_size = scratch->pdsch_est_size;
  const uint32_t pdsch_buf_size_max = scratch->pdsch_buf_size_max;
  int32_t (*pdsch_dl_ch_estimates)[pdsch_est_size] = (int32_t (*)[pdsch_est_size])scratch->pdsch_dl_ch_estimates;
  c16_t (*rxdataF_comp)[NR_MAX_NB_LAYERS][pdsch_buf_size_max] = (c16_t (*)[NR_MAX_NB_LAYERS][pdsch_buf_size_max])scratch->rxdataF_comp;
  c16_t (*dl_ch_mag)[NR_MAX_NB_LAYERS][pdsch_buf_size_max]    = (c16_t (*)[NR_MAX_NB_LAYERS][pdsch_buf_size_max])scratch->dl_ch_mag;
  c16_t (*dl_ch_magb)[NR_MAX_NB_LAYERS][pdsch_buf_size_max]   = (c16_t (*)[NR_MAX_NB_LAYERS][pdsch_buf_size_max])scratch->dl_ch_magb;
  c16_t (*dl_ch_magr)[NR_MAX_NB_LAYERS][pdsch_buf_size_max]   = (c16_t (*)[NR_MAX_NB_LAYERS][pdsch_buf_size_max])scratch->dl_ch_magr;
  c16_t (*rho_dl)[NR_MAX_NB_LAYERS * NR_MAX_NB_LAYERS][pdsch_buf_size_max] = (c16_t (*)[NR_MAX_NB_LAYERS * NR_MAX_NB_LAYERS][pdsch_buf_size_max])scratch->rho_dl;

  c16_t ptrs_phase_per_slot[ue->frame_parms.nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_phase_per_slot, 0, sizeof(ptrs_phase_per_slot));

  int32_t ptrs_re_per_slot[ue->frame_parms.nb_antennas_rx][NR_SYMBOLS_PER_SLOT];
  memset(ptrs_re_per_slot, 0, sizeof(ptrs_re_per_slot));

  uint32_t nvar = 0;

  start_meas_nr_ue_phy(ue, DLSCH_CHANNEL_ESTIMATION_STATS);
  for (int m = dlschCfg->start_symbol; m < (dlschCfg->start_symbol + dlschCfg->number_symbols); m++) {
    if (dlschCfg->dlDmrsSymbPos & (1 << m)) {
      for (int nl = 0; nl < dlsch->cw_info.Nl; nl++) { // for MIMO Config: it shall loop over no_layers
        LOG_D(PHY, "PDSCH Channel estimation layer %d, slot %d, symbol %d\n", nl, nr_slot_rx, m);
        uint32_t nvar_tmp = 0;
        nr_pdsch_channel_estimation(ue,
                                    proc,
                                    dlschCfg,
                                    freq_alloc,
                                    nl,
                                    get_dmrs_port(nl, dlschCfg->dmrs_ports),
                                    m,
                                    pdsch_est_size,
                                    pdsch_dl_ch_estimates,
                                    ue->frame_parms.samples_per_slot_wCP,
                                    rxdataF,
                                    &nvar_tmp);
        nvar += nvar_tmp;
#if 0
        ///LOG_M: the channel estimation
        char filename[100];
        for (uint8_t aarx=0; aarx<ue->frame_parms.nb_antennas_rx; aarx++) {
          sprintf(filename,"PDSCH_CHANNEL_frame%d_slot%d_sym%d_port%d_rx%d.m", frame_rx, nr_slot_rx, m, nl, aarx);
          int **dl_ch_estimates = ue->pdsch_vars[gNB_id]->dl_ch_estimates;
          LOG_M(filename,"channel_F",&dl_ch_estimates[nl*ue->frame_parms.nb_antennas_rx+aarx][ue->frame_parms.ofdm_symbol_size*m],ue->frame_parms.ofdm_symbol_size, 1, 1);
        }
#endif
      }
    }
  }
  stop_meas_nr_ue_phy(ue, DLSCH_CHANNEL_ESTIMATION_STATS);
  nvar /= (dlschCfg->number_symbols * dlsch->cw_info.Nl * ue->frame_parms.nb_antennas_rx);
  if (nvar_out != NULL)
    *nvar_out = nvar; // exposed for the ISAC data-aided tap's inverse-variance fusion weight
  uint32_t dmrs_mask = dlschCfg->dlDmrsSymbPos;
  int first_dmrs_symbol = get_first_bit_index_mask(&dmrs_mask, 1, 0, NR_SYMBOLS_PER_SLOT);
  nr_ue_measurement_procedures(first_dmrs_symbol, ue, proc, freq_alloc->num_rbs, pdsch_est_size, pdsch_dl_ch_estimates);

  if (ue->chest_time == 1) { // averaging time domain channel estimates
    nr_chest_time_domain_avg(&ue->frame_parms,
                             (int32_t **)pdsch_dl_ch_estimates,
                             dlschCfg->number_symbols,
                             dlschCfg->start_symbol,
                             dlschCfg->dlDmrsSymbPos,
                             freq_alloc->num_rbs,
                             dlsch->cw_info.Nl,
                             ue->frame_parms.nb_antennas_rx);
  }

  uint16_t first_symbol_with_data = dlschCfg->start_symbol;
  uint32_t dmrs_data_re;

  if (dlschCfg->dmrsConfigType == NFAPI_NR_DMRS_TYPE1)
    dmrs_data_re = 12 - 6 * dlschCfg->n_dmrs_cdm_groups;
  else
    dmrs_data_re = 12 - 4 * dlschCfg->n_dmrs_cdm_groups;

  while ((dmrs_data_re == 0) && (dlschCfg->dlDmrsSymbPos & (1 << first_symbol_with_data))) {
    first_symbol_with_data++;
  }

  uint32_t dl_valid_re[NR_SYMBOLS_PER_SLOT] = {0};

  int32_t log2_maxh = 0;

  start_meas_nr_ue_phy(ue, RX_PDSCH_STATS);
  pdsch_scope_req_t scope_req = {.copy_chanest_to_scope = false, .copy_rxdataF_to_scope = false, .scope_rxdataF_offset = 0};
  if (UEScopeHasTryLock(ue)) {
    metadata mt = {.frame = proc->frame_rx, .slot = proc->nr_slot_rx};
    scope_req.copy_chanest_to_scope = UETryLockScopeData(ue,
                                                         pdschChanEstimates,
                                                         sizeof(c16_t),
                                                         1,
                                                         freq_alloc->num_rbs * NR_NB_SC_PER_RB * dlschCfg->number_symbols,
                                                         &mt);
    scope_req.copy_rxdataF_to_scope = UETryLockScopeData(ue,
                                                         pdschRxdataF,
                                                         sizeof(c16_t),
                                                         1,
                                                         freq_alloc->num_rbs * NR_NB_SC_PER_RB * dlschCfg->number_symbols,
                                                         &mt);
  }

  for (int m = dlschCfg->start_symbol; m < (dlschCfg->number_symbols + dlschCfg->start_symbol); m++) {
    bool first_symbol_flag = false;
    if (m == first_symbol_with_data)
      first_symbol_flag = true;

    // process DLSCH received symbols in the slot
    // symbol by symbol processing (if data/DMRS are multiplexed is checked inside the function)
    if (nr_rx_pdsch(ue,
                    proc,
                    dlsch,
                    freq_alloc,
                    dlschCfg,
                    dlsch_harq,
                    m,
                    first_symbol_flag,
                    harq_pid,
                    pdsch_est_size,
                    pdsch_dl_ch_estimates,
                    llr,
                    dl_valid_re,
                    rxdataF,
                    &log2_maxh,
                    pdsch_buf_size_max,
                    ue->frame_parms.nb_antennas_rx,
                    rxdataF_comp,
                    dl_ch_mag,
                    dl_ch_magb,
                    dl_ch_magr,
                    ptrs_phase_per_slot,
                    ptrs_re_per_slot,
                    nvar,
                    &scope_req,
                    rho_dl)
        < 0) {
      if (scope_req.copy_chanest_to_scope) {
        UEunlockScopeData(ue, pdschChanEstimates);
      }
      if (scope_req.copy_rxdataF_to_scope) {
        UEunlockScopeData(ue, pdschRxdataF);
      }
      return -1;
    }
  } // CRNTI active
  stop_meas_nr_ue_phy(ue, RX_PDSCH_STATS);
  if (scope_req.copy_chanest_to_scope) {
    UEunlockScopeData(ue, pdschChanEstimates);
  }
  if (scope_req.copy_rxdataF_to_scope) {
    UEunlockScopeData(ue, pdschRxdataF);
  }
  return 0;
}

uint32_t nr_ue_csi_rm_unav_res(fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config, freq_alloc_bitmap_t *freq_alloc)
{
  uint32_t unav_res = 0;
  for (int i = 0; i < dlsch_config->numCsiRsForRateMatching; i++) {
    fapi_nr_dl_config_csirs_pdu_rel15_t *csi_pdu = &dlsch_config->csiRsForRateMatching[i];
    // check overlapping symbols
    int num_overlap_symb = 0;
    // num of consecutive csi symbols from l0 included
    int num_l0 [18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
    int num_symb = num_l0[csi_pdu->row - 1];
    for (int s = 0; s < num_symb; s++) {
      int l0_symb = csi_pdu->symb_l0 + s;
      if (l0_symb >= dlsch_config->start_symbol && l0_symb < dlsch_config->start_symbol + dlsch_config->number_symbols)
        num_overlap_symb++;
    }
    // check also l1 if relevant
    if (csi_pdu->row == 13 || csi_pdu->row == 14 || csi_pdu->row == 16 || csi_pdu->row == 17) {
      num_symb += 2;
      for (int s = 0; s < 2; s++) { // two consecutive symbols including l1
        int l1_symb = csi_pdu->symb_l1 + s;
        if (l1_symb >= dlsch_config->start_symbol && l1_symb < dlsch_config->start_symbol + dlsch_config->number_symbols)
          num_overlap_symb++;
      }
    }
    if (num_overlap_symb == 0)
      continue;
    // check number overlapping prbs
    // assuming CSI is spanning the whole BW
    AssertFatal(dlsch_config->BWPSize <= csi_pdu->nr_of_rbs, "Assuming CSI-RS is spanning the whold BWP this shouldn't happen\n");
    int num_overlapping_prbs = 0;
    for (int rb = freq_alloc->first_rb; rb <= freq_alloc->last_rb; rb++) {
      if (!check_rb_in_bitmap(freq_alloc, rb))
        continue;
      if (csi_pdu->freq_density < 2) {
        int abs_rb = rb + dlsch_config->BWPStart;
        if ((abs_rb % 2) == csi_pdu->freq_density)
          num_overlapping_prbs++;
      } else {
        num_overlapping_prbs++;
      }
    }
    // density is number or res per port per rb (over all symbols)
    int ports [18] = {1, 1, 2, 4, 4, 8, 8, 8, 12, 12, 16, 16, 24, 24, 24, 32, 32, 32};
    int num_csi_res_per_prb = csi_pdu->freq_density == 3 ? 3 : 1;
    num_csi_res_per_prb *= ports[csi_pdu->row - 1];
    unav_res += num_overlapping_prbs * num_csi_res_per_prb * num_overlap_symb / num_symb;
  }
  return unav_res;
}

/*! \brief Process the whole DLSCH slot
 */
static void nr_ue_dlsch_procedures(PHY_VARS_NR_UE *ue,
                                   const UE_nr_rxtx_proc_t *proc,
                                   NR_UE_DLSCH_t *dlsch,
                                   int cw_idx,
                                   int G,
                                   freq_alloc_bitmap_t *freq_alloc,
                                   fapi_nr_dl_config_dlsch_pdu_rel15_t *config,
                                   int16_t *llr,
                                   uint8_t **tb_out)
{
  if (dlsch->active == false) {
    LOG_E(PHY, "DLSCH should be active when calling this function\n");
    return;
  }

  int harq_pid = config->harq_process_nbr;
  int frame_rx = proc->frame_rx;
  int nr_slot_rx = proc->nr_slot_rx;

  LOG_D(PHY, "AbsSubframe %d.%d Start LDPC Decoder for CW%d [harq_pid %d]\n", frame_rx % 1024, nr_slot_rx, cw_idx, harq_pid);

  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  // exit dlsch procedures as there are no active dlsch
  NR_DL_UE_HARQ_t *dl_harq = &ue->dl_harq_processes[cw_idx][harq_pid];
  if (dl_harq->status != NR_ACTIVE) {
    // don't wait anymore
    LOG_E(NR_PHY, "Internal error  nr_ue_dlsch_procedure() called but no active cw on slot %d, harq %d\n", nr_slot_rx, harq_pid);
    if (config->k1_feedback) {
      const int ack_nack_slot_and_frame = (proc->nr_slot_rx + config->k1_feedback) + proc->frame_rx * fp->slots_per_frame;
      dynamic_barrier_join(&ue->process_slot_tx_barriers[ack_nack_slot_and_frame % NUM_PROCESS_SLOT_TX_BARRIERS]);
    }
    return;
  }

  start_meas_nr_ue_phy(ue, DLSCH_UNSCRAMBLING_STATS);
  nr_dlsch_unscrambling(llr, G, 0, config->dlDataScramblingId, dlsch->rnti);
  stop_meas_nr_ue_phy(ue, DLSCH_UNSCRAMBLING_STATS);

  start_meas_nr_ue_phy(ue, DLSCH_DECODING_STATS);
  uint8_t output[lenWithCrc(1, dlsch->cw_info.TBS) / 8];
  nr_dlsch_decoding(ue, proc, dlsch, cw_idx, config, llr, output, freq_alloc->num_rbs, G);
  /* Published for the ISAC data-aided tap. Points at this frame's stack buffer, so it is only
   * valid until this function returns -- the tap runs synchronously at the call site below. */
  if (tb_out != NULL)
    *tb_out = output;
  stop_meas_nr_ue_phy(ue, DLSCH_DECODING_STATS);

  int ind_type = -1;
  switch (dlsch->rnti_type) {
    case TYPE_RA_RNTI_:
      ind_type = FAPI_NR_RX_PDU_TYPE_RAR;
      break;
    case TYPE_SI_RNTI_:
      ind_type = FAPI_NR_RX_PDU_TYPE_SIB;
      break;
    case TYPE_C_RNTI_:
      ind_type = FAPI_NR_RX_PDU_TYPE_DLSCH;
      break;
    case TYPE_P_RNTI_:
      ind_type = FAPI_NR_RX_PDU_TYPE_PCCH;
      break;
    default:
      AssertFatal(false, "Invalid DLSCH type %d\n", dlsch->rnti_type);
      break;
  }

  LOG_D(PHY, "DL PDU length in bits: %d, in bytes: %d \n", dlsch->cw_info.TBS, dlsch->cw_info.TBS / 8);
  if (cpumeas(CPUMEAS_GETSTATE)) {
    LOG_D(PHY,
          " --> Unscrambling %5.3f\n",
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_UNSCRAMBLING_STATS].p_time / (cpuf * 1000.0));
    LOG_D(PHY,
          "AbsSubframe %d.%d --> LDPC Decoding %5.3f\n",
          frame_rx % 1024,
          nr_slot_rx,
          ue->phy_cpu_stats.cpu_time_stats[DLSCH_DECODING_STATS].p_time / (cpuf * 1000.0));
  }

  // send to mac
  if (ue->if_inst && ue->if_inst->dl_indication) {
    fapi_nr_rx_indication_t rx_ind;
    rx_ind.number_pdus = 0;
    nr_fill_rx_indication(&rx_ind, ind_type, ue, cw_idx, harq_pid, dlsch, proc, output);
    nr_downlink_indication_t dl_indication = (nr_downlink_indication_t){
        .gNB_index = proc->gNB_id,
        .module_id = ue->Mod_id,
        .cc_id = ue->CC_id,
        .hfn = proc->hfn_rx,
        .frame = proc->frame_rx,
        .slot = proc->nr_slot_rx,
        .rx_ind = &rx_ind,
    };
    ue->if_inst->dl_indication(&dl_indication);
  }

  // DLSCH decoding finished! don't wait anymore in Tx process, we know if we should answer ACK/NACK PUCCH
  if ((dlsch->rnti_type == TYPE_C_RNTI_ || dlsch->rnti_type == TYPE_RA_RNTI_) && config->k1_feedback) {
    const int ack_nack_slot_and_frame = (proc->nr_slot_rx + config->k1_feedback) + proc->frame_rx * fp->slots_per_frame;
    dynamic_barrier_join(&ue->process_slot_tx_barriers[ack_nack_slot_and_frame % NUM_PROCESS_SLOT_TX_BARRIERS]);
  }

  int a_segments = MAX_NUM_NR_DLSCH_SEGMENTS; // number of segments to be allocated
  if (freq_alloc->num_rbs != 273) {
    a_segments = a_segments * freq_alloc->num_rbs;
    a_segments = (a_segments / 273) + 1;
  }

  if (ue->phy_sim_dlsch_b)
    memcpy(ue->phy_sim_dlsch_b, output, sizeof(output));
}

static bool check_neighboring_cells_task(PHY_VARS_NR_UE *ue, bool task_pending)
{
  if (task_pending == true) {
    return false;
  }

  // Check if any intra-frequency neighbor cells are configured
  uint32_t serving_ssb_freq = get_ssb_arfcn(&ue->frame_parms);
  bool has_intra_freq_neighbors = false;

  for (int cell_idx = 0; cell_idx < NUMBER_OF_NEIGHBORING_CELLS_MAX; cell_idx++) {
    fapi_nr_neighboring_cell_t *nr_neighboring_cell = &ue->nrUE_config.meas_config.nr_neighboring_cell[cell_idx];
    if (nr_neighboring_cell->active == 1) {
      if (nr_neighboring_cell->ssb_freq == 0 || nr_neighboring_cell->ssb_freq == serving_ssb_freq) {
        has_intra_freq_neighbors = true;
      }
    }
  }

  return has_intra_freq_neighbors;
}

static bool is_ssb_index_transmitted(const PHY_VARS_NR_UE *ue, const int index)
{
  if (ue->received_config_request) {
    const fapi_nr_config_request_t *cfg = &ue->nrUE_config;
    const uint32_t curr_mask = cfg->ssb_table.ssb_mask_list[index / 32].ssb_mask;
    return ((curr_mask >> (31 - (index % 32))) & 0x01);
  } else
    return ue->frame_parms.ssb_index == index;
}

static int get_pdcch_max_rbs(NR_UE_PDCCH_CONFIG *phy_pdcch_config)
{
  int nb_rb = 0;
  int rb_offset = 0;
  for (int i = 0; i < phy_pdcch_config->nb_search_space; i++) {
    int tmp = 0;
    get_coreset_rballoc(phy_pdcch_config->pdcch_config[i].coreset.frequency_domain_resource, &tmp, &rb_offset);
    if (tmp > nb_rb)
      nb_rb = tmp;
  }
  return nb_rb;
}

static int get_max_pdcch_symb(const NR_UE_PDCCH_CONFIG *phy_pdcch_config)
{
  int max_pdcch_symb = 0;
  for (int i = 0; i < phy_pdcch_config->nb_search_space; i++)
    if (phy_pdcch_config->pdcch_config[i].coreset.duration > max_pdcch_symb)
      max_pdcch_symb = phy_pdcch_config->pdcch_config[i].coreset.duration;

  return max_pdcch_symb;
}

void pdcch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data)
{
  NR_UE_PDCCH_CONFIG *phy_pdcch_config = &phy_data->phy_pdcch_config;
  if (phy_pdcch_config->nb_search_space == 0)
    return;

  TracyCZone(ctx, true);
  /* process PDCCH */
  LOG_D(PHY, " ------ --> PDCCH ChannelComp/LLR Frame.slot %d.%d ------  \n", proc->frame_rx % 1024, proc->nr_slot_rx);
  start_meas_nr_ue_phy(ue, DLSCH_RX_PDCCH_STATS);
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  int num_monitoring_occ = get_max_pdcch_monOcc(phy_pdcch_config, fp->symbols_per_slot);
  int max_nb_symb_pdcch = get_max_pdcch_symb(phy_pdcch_config);
  int llr_size_symbol = get_pdcch_max_rbs(phy_pdcch_config) * 9;
  c16_t pdcch_llr[phy_pdcch_config->nb_search_space][num_monitoring_occ][max_nb_symb_pdcch * llr_size_symbol];
  int start_symb_pdcch, last_symb_pdcch;
  set_first_last_pdcch_symb(phy_pdcch_config, fp->symbols_per_slot, &start_symb_pdcch, &last_symb_pdcch);

  /* Temporarily loop over symbols in the slot, perform OFDM demod and process PDCCH.
     When symbol based proc design is fully merged, this function will be called to process only one symbol
     and OFDM demod will be removed from here. */
  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  __attribute__((aligned(32))) c16_t rxdataF[fp->nb_antennas_rx][rxdataF_sz];

  for (int symbol = start_symb_pdcch; symbol <= last_symb_pdcch; symbol++) {
    nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
    __attribute__((aligned(32))) c16_t rxdataF_symb[fp->nb_antennas_rx][((fp->ofdm_symbol_size + 7) / 8) * 8];

    for (int ant = 0; ant < fp->nb_antennas_rx; ant++)
      memcpy(rxdataF_symb[ant], &rxdataF[ant][symbol * fp->ofdm_symbol_size], sizeof(c16_t) * fp->ofdm_symbol_size);

    nr_pdcch_generate_llr(ue, proc, symbol, phy_data, llr_size_symbol, num_monitoring_occ, max_nb_symb_pdcch, rxdataF_symb, pdcch_llr);
    if (symbol == last_symb_pdcch) {
      nr_pdcch_dci_indication(proc, llr_size_symbol * max_nb_symb_pdcch, num_monitoring_occ, ue, phy_data, pdcch_llr);
      UEscopeCopy(ue, pdcchLlr, pdcch_llr, sizeof(c16_t), 1, sizeof(pdcch_llr) / sizeof(c16_t), 0);
    }
  }
  stop_meas_nr_ue_phy(ue, DLSCH_RX_PDCCH_STATS);
  TracyCZoneEnd(ctx);
  if (ue->phy_sim_rxdataF)
    memcpy(ue->phy_sim_rxdataF, rxdataF[0], sizeof(int32_t) * max_nb_symb_pdcch * fp->ofdm_symbol_size);
}

int is_ssb_in_symbol(const PHY_VARS_NR_UE *ue,
                     const int symbIdxInFrame,
                     const int slot,
                     const int ssbMask,
                     const int ssbIndex,
                     const int ssb_period)
{
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  // Skip if current SSB index is not transmitted
  if (!is_ssb_index_transmitted(ue, ssbIndex)) {
    return false;
  }

  const int startPbchSymb = nr_get_ssb_start_symbol(fp, ssbIndex) + 1;
  const int startPbchSymbHf = (ssb_period == 0) ? (startPbchSymb + (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2))
                                                : (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT);

  // Skip if no SSB in current symbol
  if ((symbIdxInFrame >= startPbchSymb && symbIdxInFrame < (startPbchSymb + NB_SYMBOLS_PBCH))
      || (symbIdxInFrame >= startPbchSymbHf && symbIdxInFrame < (startPbchSymbHf + NB_SYMBOLS_PBCH))) {
    return true;
  }

  return false;
}

int get_ssb_index_in_symbol(const PHY_VARS_NR_UE *ue, const int symbIdxInFrame, const int slot, const int frame)
{
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const fapi_nr_config_request_t *cfg = &ue->nrUE_config;
  // Checking if current frame is compatible with SSB periodicity
  const int default_ssb_period = 2;
  const int ssb_period = ue->received_config_request ? cfg->ssb_table.ssb_period : default_ssb_period;
  if (ssb_period != 0 && (frame % (1 << (ssb_period - 1)))) {
    return -1;
  }

  // Find the SSB index corresponding to current symbol
  for (int ssbIndex = 0; ssbIndex < fp->Lmax; ssbIndex++) {
    const int ssbMask = cfg->ssb_table.ssb_mask_list[ssbIndex / 32].ssb_mask;
    if (is_ssb_in_symbol(ue, symbIdxInFrame, slot, ssbMask, ssbIndex, ssb_period))
      return ssbIndex;
  }

  return -1;
}

/* Description: Generates PBCH LLRs from frequency domain signal for one OFDM symbol.
                Generates PBCH time domain channel response.
   Returns    : SSB index if symbol contains SSB. Else returns -1. */
int nr_process_pbch_symbol(
    PHY_VARS_NR_UE *ue,
    const UE_nr_rxtx_proc_t *proc,
    const int symbol,
    const int ssbIndexIn,
    c16_t dl_ch_estimates_time[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size],
    c16_t *dl_ch_estimates_symbol,
    int16_t pbch_e_rx[NR_POLAR_PBCH_E],
    double *log2_maxh_state)
{
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const int symbIdxInFrame = symbol + NR_SYMBOLS_PER_SLOT * proc->nr_slot_rx;

  // Search for SSB index if given SSB index is invalid
  const int ssbIndex =
      (ssbIndexIn < 0) ? get_ssb_index_in_symbol(ue, symbIdxInFrame, proc->nr_slot_rx, proc->frame_rx) : ssbIndexIn;

  if (ssbIndex < 0)
    return -1;

  LOG_D(PHY, "Frame %d, Slot %d, Symbol %d, SSB Index %d\n", proc->frame_rx, proc->nr_slot_rx, symbol, ssbIndex);
  const int startPbchSymb = nr_get_ssb_start_symbol(fp, ssbIndex) + 1;
  const int startPbchSymbHf = startPbchSymb + (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2);

  // Found PBCH. Process it
  __attribute__((aligned(32))) c16_t rxdataF[fp->nb_antennas_rx][fp->ofdm_symbol_size];
  {
    __attribute__((aligned(32))) c16_t tmp[fp->nb_antennas_rx][fp->samples_per_slot_wCP];
    // ---- TRACKING-ONLY PBCH FFT-WINDOW BIAS (2026-08-06, diagnostic) ------------------------
    // The offline DMRS-coherence scan located the PBCH DMRS signature (min-over-3-symbols
    // coherence 0.999, i_ssb=0) at +3620 samples from where this path FFTs. Applying that bias
    // HERE ONLY -- the PBCH extraction path, not acquisition, not the sample stream, not any
    // other consumer of rxdata -- tests whether the samples and decoder are fine and the handover
    // offset is the whole bug. ISAC_PBCH_OFFSET_BIAS=<samples>, default 0 = unchanged.
    static int s_pbch_bias = INT_MIN;
    if (s_pbch_bias == INT_MIN) {
      const char *e = getenv("ISAC_PBCH_OFFSET_BIAS");
      s_pbch_bias = e ? atoi(e) : 0;
      if (s_pbch_bias)
        LOG_W(PHY, "SENSING: PBCHBIAS applying %+d samples to tracking PBCH FFT window only\n", s_pbch_bias);
    }
    nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, tmp, link_type_dl, s_pbch_bias, ue->common_vars.rxdata);
    /* ISAC_SSB_IQ=<path> (diagnostic, default off): the RAW TIME-DOMAIN samples this FEP call just
     * transformed, so the SSB can be re-processed offline independently of OAI's fixed-point DFT --
     * in float at the native size, and via a 20 MHz decimated 1024-point path. Neither is decidable
     * from rxdataF, which is already the DFT's output. antenna 0, first 96 records (24 SSB x 4 symb):
     * header int32 {N, nb_prefix, rx_offset, frame, slot, symbol, ssb_start_subcarrier, nb_ant},
     * then (nb_prefix + N) c16_t from rxdata[0][rx_offset - nb_prefix]. No wrap: rxdata is
     * 2*samples_per_frame + ofdm_symbol_size long and the FEP reads contiguously. */
    {
      static int s_iq_left = -1;
      static FILE *s_iq = NULL;
      if (s_iq_left < 0) {
        const char *pth = getenv("ISAC_SSB_IQ");
        s_iq = (pth && pth[0]) ? fopen(pth, "wb") : NULL;
        s_iq_left = s_iq ? 96 : 0;
      }
      const unsigned int rxo = nr_slot_fep_diag_rx_offset, npfx = nr_slot_fep_diag_nb_prefix_samples;
      if (s_iq_left > 0 && s_iq && rxo >= npfx) {
        s_iq_left--;
        int32_t hdr[8] = {fp->ofdm_symbol_size, (int32_t)npfx, (int32_t)rxo, proc->frame_rx,
                          proc->nr_slot_rx, symbol, fp->ssb_start_subcarrier, fp->nb_antennas_rx};
        fwrite(hdr, sizeof(hdr), 1, s_iq);
        fwrite(&ue->common_vars.rxdata[0][rxo - npfx], sizeof(c16_t), npfx + fp->ofdm_symbol_size, s_iq);
        fflush(s_iq);
        if (s_iq_left == 0) { fclose(s_iq); s_iq = NULL; }
      }
    }
    // COORDINATE-SYSTEM CHECK (2026-08-06): nr_slot_fep_diag_rx_offset is the FINAL buffer index
    // the DFT reads from, after slot origin, symbol offset, sample_offset and the CP/divisor
    // backoff. Logging it with and without the bias proves whether ISAC_PBCH_OFFSET_BIAS actually
    // moves the FFT window by that many samples in the SAME coordinate the offline scanner reports
    // its best DMRS position in. Until the difference is EXACTLY the bias, a scanner delta cannot
    // be translated into sample_offset at all, and the bias test proves nothing.
    {
      // Per-occasion displacement measurement (bounded, small window copy, ISAC_DELTA_SCAN=1).
      {
        extern void nr_pbch_measure_delta(const NR_DL_FRAME_PARMS *, const UE_nr_rxtx_proc_t *,
                                          c16_t *const *, int, unsigned);
        nr_pbch_measure_delta(fp, proc, ue->common_vars.rxdata, 2 * fp->samples_per_frame,
                              nr_slot_fep_diag_rx_offset);
      }
      static int s_fftaddr_left = 6;
      if (s_fftaddr_left > 0) {
        s_fftaddr_left--;
        LOG_W(PHY,
              "SENSING: FFTADDR frame=%d slot=%d symbol=%d bias=%d fep_rx_offset=%u nb_pfx=%u nb_pfx0=%u\n",
              proc->frame_rx, proc->nr_slot_rx, symbol, s_pbch_bias, nr_slot_fep_diag_rx_offset,
              nr_slot_fep_diag_nb_prefix_samples, nr_slot_fep_diag_nb_prefix_samples0);
      }
    }
    for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
      memcpy(rxdataF[aarx], tmp[aarx] + symbol * fp->ofdm_symbol_size, sizeof(c16_t) * fp->ofdm_symbol_size);
    }

    // TEMPORARY DIAGNOSTIC (2026-08-05): 3-stage amplitude trace for the tracking path, to compare
    // against acquisition's do_time_to_freq() (nr_initial_sync.c) at the same 3 stages. Reads
    // nr_slot_fep's just-computed rx_offset/CP lengths (exported via slot_fep_nr.c, valid
    // immediately after the synchronous call above) so this does NOT duplicate/risk drifting from
    // that function's own offset arithmetic.
    {
      static int s_phy_diag = -1;
      if (s_phy_diag < 0)
        s_phy_diag = (getenv("ISAC_PHY_DIAG") && atoi(getenv("ISAC_PHY_DIAG"))) ? 1 : 0;
      // FEPDIAG/SSBTIME/FRAMESCAN are heavy: FRAMESCAN alone does ~280 4096-point FFTs
      // INLINE ON THE RT PATH. Measured elsewhere to stall processing long enough to cause
      // an RF overrun. Default OFF so a control run is not perturbed by its own instrumentation.
      static int s_fepdiag_left = 40;
      if (s_phy_diag && s_fepdiag_left > 0) {
        const unsigned int rx_off = nr_slot_fep_diag_rx_offset;
        const unsigned int nb_pfx = nr_slot_fep_diag_nb_prefix_samples;
        const c16_t *rxd = ue->common_vars.rxdata[0];
        // Stage 1: raw time-domain samples spanning a full CP-length lookback + the symbol itself
        // (i.e. BEFORE the CP is discarded), starting at rx_off - nb_pfx.
        double s1_sum = 0.0, s1_sumsq = 0.0;
        const int s1_n = (int)(nb_pfx + fp->ofdm_symbol_size);
        for (int i = 0; i < s1_n; i++) {
          const double m = hypot((double)rxd[rx_off - nb_pfx + i].r, (double)rxd[rx_off - nb_pfx + i].i);
          s1_sum += m;
          s1_sumsq += m * m;
        }
        // Stage 2: exactly the window nr_slot_fep hands to the DFT (post-CP-removal).
        double s2_sum = 0.0, s2_sumsq = 0.0;
        for (int i = 0; i < fp->ofdm_symbol_size; i++) {
          const double m = hypot((double)rxd[rx_off + i].r, (double)rxd[rx_off + i].i);
          s2_sum += m;
          s2_sumsq += m * m;
        }
        // Stage 3: raw FFT output for this symbol, BEFORE nr_pbch_extract subselects subcarriers.
        double s3_sum = 0.0, s3_sumsq = 0.0;
        const c16_t *fftout = &tmp[0][symbol * fp->ofdm_symbol_size];
        for (int i = 0; i < fp->ofdm_symbol_size; i++) {
          const double m = hypot((double)fftout[i].r, (double)fftout[i].i);
          s3_sum += m;
          s3_sumsq += m * m;
        }
        LOG_W(PHY,
              "SENSING: FEPDIAG path=tracking slot=%d symbol=%d rx_offset=%u nb_prefix=%u nb_prefix0=%u "
              "is_sync=%d s1_mean=%.2f s1_rms=%.2f s2_mean=%.2f s2_rms=%.2f s3_mean=%.2f s3_rms=%.2f\n",
              proc->nr_slot_rx, symbol, rx_off, nb_pfx, nr_slot_fep_diag_nb_prefix_samples0,
              nr_slot_fep_diag_is_synchronized, s1_sum / s1_n, sqrt(s1_sumsq / s1_n),
              s2_sum / fp->ofdm_symbol_size, sqrt(s2_sumsq / fp->ofdm_symbol_size),
              s3_sum / fp->ofdm_symbol_size, sqrt(s3_sumsq / fp->ofdm_symbol_size));

        // ---- SSB TIME-SELECTION DIAGNOSTIC (2026-08-05) ------------------------------------
        // The frequency-domain SPECDIAG showed tracking's FFT window contains NO SSB anywhere in
        // the spectrum (flat at the noise floor) while acquisition finds it at 19 dB from the same
        // RF -- so the fault is WHICH SAMPLES are selected, not how they are processed. This logs
        // the full time-coordinate state tracking used, and then SWEEPS +/-1 slot around the chosen
        // window to locate where the real SSB burst actually sits relative to it.
        //
        // Absolute sample offsets are the comparison currency, deliberately: acquisition indexes a
        // scan buffer by a PSS-correlation-derived sample position while tracking indexes the live
        // ring buffer by slot/symbol arithmetic, so their slot/symbol NUMBERS are not in the same
        // coordinate system and comparing those alone would be misleading.
        {
          const unsigned int total_samples = 2 * fp->samples_per_frame;
          const int sym_stride = fp->ofdm_symbol_size + (int)nb_pfx; // one OFDM symbol incl. CP
          const int span_syms = 14;                                  // +/- ~1 slot
          double best_e = -1.0;
          int best_d = 0;
          char sweep[640];
          int sp = 0;
          for (int k = -span_syms; k <= span_syms; k++) {
            const long d = (long)k * sym_stride;
            long base = (long)rx_off + d;
            base = ((base % (long)total_samples) + (long)total_samples) % (long)total_samples;
            double e = 0.0;
            int n = 0;
            for (int i = 0; i < fp->ofdm_symbol_size; i += 4) { // decimated: shape, not exactness
              const unsigned int idx = (unsigned int)((base + i) % (long)total_samples);
              const double re = rxd[idx].r, im = rxd[idx].i;
              e += re * re + im * im;
              n++;
            }
            e = sqrt(e / n);
            if (e > best_e) {
              best_e = e;
              best_d = k;
            }
            if (sp < (int)sizeof(sweep) - 8)
              sp += snprintf(sweep + sp, sizeof(sweep) - sp, "%.0f ", e);
          }
          // ---- PRODUCER/CONSUMER LAG (2026-08-05) ------------------------------------------
          // prod_slot: the read loop's raw absolute_slot counter at its LATEST completed write
          // (executables/nr-ue.c, updated right after nrue_ru_read() returns). wall_lag_us: wall-
          // clock time between that write and THIS diagnostic executing, on whatever dl_actor
          // worker thread ended up running this PBCH occasion. A large lag directly proves the
          // physical rxdata buffer this consumer is about to FFT has already been overwritten by
          // later slots by the time it gets here (the buffer holds only ~1 frame's worth of
          // samples -- see the handover doc -- so any lag approaching one frame duration, ~10ms
          // at mu=1, is already enough).
          const long prod_slot = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
          const long prod_wall_ns = atomic_load_explicit(&nr_ue_diag_producer_wall_ns, memory_order_relaxed);
          struct timespec diag_now_ts;
          clock_gettime(CLOCK_REALTIME, &diag_now_ts);
          const long diag_now_ns = (long)diag_now_ts.tv_sec * 1000000000L + diag_now_ts.tv_nsec;
          const double wall_lag_us = (prod_wall_ns > 0) ? (double)(diag_now_ns - prod_wall_ns) / 1000.0 : -1.0;
          const long consumer_abs_slot_wrapped = (long)proc->frame_rx * fp->slots_per_frame + proc->nr_slot_rx;

          LOG_W(PHY,
                "SENSING: SSBTIME path=tracking frame=%d slot=%d symbol=%d symbIdxInFrame=%d "
                "halfframe=%d ssbIndex=%d startPbchSymb=%d relPbchSymb=%d mu=%d ssb_period_cfg=%d "
                "rx_offset=%u acq_ssb_offset=%d acq_symbol_offset=%u delta_vs_acq=%ld "
                "ssb_start_sc=%d sym_stride=%d "
                "sweep_best_dsym=%+d sweep_best_rms=%.2f sweep_at_0=%.2f "
                "prod_abs_slot=%ld cons_abs_slot_wrapped=%ld wall_lag_us=%.1f sweep[-14..+14]: %s\n",
                proc->frame_rx, proc->nr_slot_rx, symbol, symbIdxInFrame,
                (symbIdxInFrame > (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2)) ? 1 : 0,
                ssbIndex, startPbchSymb, (symbIdxInFrame > (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2))
                                             ? (symbIdxInFrame - startPbchSymbHf)
                                             : (symbIdxInFrame - startPbchSymb),
                fp->numerology_index, ue->nrUE_config.ssb_table.ssb_period, rx_off,
                ue->ssb_offset, (unsigned)ue->symbol_offset,
                (long)rx_off - (long)ue->ssb_offset,
                fp->ssb_start_subcarrier, sym_stride, best_d, best_e,
                s2_sum / fp->ofdm_symbol_size, prod_slot, consumer_abs_slot_wrapped, wall_lag_us, sweep);

          // ---- ONE-SHOT FRAME-WIDE FREQUENCY-DOMAIN SSB SEARCH (2026-08-05) ----------------
          // The +/-1 slot TIME-domain sweep above is blind here on purpose-of-record: on a loaded
          // 273-PRB cell every symbol carries wideband traffic at ~17 dB, so a 240-subcarrier SSB
          // is invisible in total symbol energy (the existing SSBSWEEP reads a flat 17 across all
          // 14 symbols for exactly this reason). This instead FFTs each candidate symbol position
          // across a whole frame and scores the SSB's OWN 240 bins against that position's local
          // floor -- the same start_bin formula nr_pbch_extract() uses -- so the SSB is detectable
          // regardless of the traffic around it. Fires ONCE per process.
          static int s_framescan_done = 0;
          if (!s_framescan_done) {
            s_framescan_done = 1;
            const int N = fp->ofdm_symbol_size;
            const int start_bin = (fp->first_carrier_offset + fp->ssb_start_subcarrier) % N;
            const unsigned int total_samples = 2 * fp->samples_per_frame;
            const int nsym = fp->samples_per_frame / sym_stride; // ~280 symbol slots per frame
            dft_size_idx_t dsz = get_dft(N);
            __attribute__((aligned(32))) c16_t win[N];
            __attribute__((aligned(32))) c16_t spec[N];
            double best_ratio = -1.0;
            long best_off = -1;
            char top[256];
            int tp = 0;
            for (int k = 0; k < nsym; k++) {
              const long base = ((long)k * sym_stride) % (long)total_samples;
              for (int i = 0; i < N; i++)
                win[i] = rxd[(unsigned int)((base + i) % (long)total_samples)];
              dft(dsz, (int16_t *)win, (int16_t *)spec, 1);
              double in_p = 0.0, out_p = 0.0;
              for (int i = 0; i < 240; i++) {
                const c16_t v = spec[(start_bin + i) % N];
                in_p += (double)v.r * v.r + (double)v.i * v.i;
              }
              for (int i = 0; i < 240; i++) { // local floor: 300 bins below the SSB band
                const c16_t v = spec[(((start_bin - 300 + i) % N) + N) % N];
                out_p += (double)v.r * v.r + (double)v.i * v.i;
              }
              const double ratio = (out_p > 0.0) ? sqrt(in_p / out_p) : 0.0;
              if (ratio > best_ratio) {
                best_ratio = ratio;
                best_off = base;
              }
              if (ratio > 2.0 && tp < (int)sizeof(top) - 24)
                tp += snprintf(top + tp, sizeof(top) - tp, "%ld:%.1f ", base, ratio);
            }
            if (tp == 0)
              snprintf(top, sizeof(top), "(none above 2.0x)");
            LOG_W(PHY,
                  "SENSING: FRAMESCAN nsym=%d sym_stride=%d start_bin=%d tracking_rx_offset=%u "
                  "best_off=%ld best_ratio=%.2f delta_vs_tracking=%ld hits[>2x]: %s\n",
                  nsym, sym_stride, start_bin, rx_off, best_off, best_ratio,
                  best_off - (long)rx_off, top);

            // Same scan again via the SHARED implementation, with a CFO derotation sweep. The scan
            // above (and acquisition's own published figure) are not directly comparable: acquisition
            // measures AFTER compensate_freq_offset(), this measures raw. A residual offset near half
            // a subcarrier -- which this cell's measured ~-15 kHz at 30 kHz SCS is -- smears the SSB
            // across bins and can read as noise even when the SSB is perfectly present. If the swept
            // version finds a peak the unswept one misses, "no SSB in the buffer" was a CFO artifact,
            // not an empty buffer.
            extern void nr_isac_framescan(const c16_t *, unsigned int, int, int, int, int, long, double, double, double,
                                          const char *);
            const double fs_hz = (double)fp->samples_per_subframe * 1000.0;
            nr_isac_framescan(rxd, total_samples, N, start_bin, sym_stride, nsym, (long)rx_off, fs_hz, 0.0, 0.0,
                              "track_raw");
            nr_isac_framescan(rxd, total_samples, N, start_bin, sym_stride, nsym, (long)rx_off, fs_hz, 20000.0, 1000.0,
                              "track_raw_cfosweep");
          }
        }
        s_fepdiag_left--;
      }
    }
  }
  c16_t dl_ch_estimates[fp->nb_antennas_rx][fp->ofdm_symbol_size];

  const int relPbchSymb = (symbIdxInFrame > (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2)) ? (symbIdxInFrame - startPbchSymbHf)
                                                                                             : (symbIdxInFrame - startPbchSymb);

  // CFO-CORRECTION SWEEP (2026-08-04e, capture-only diagnostic, opt-in ISAC_PBCH_CFO_SWEEP=1, no
  // behavioural change to the real decode below). SUPERSEDES an earlier version that derotated
  // rxdataF (POST-FFT): a post-FFT rotation only removes CFO's common-phase term and cannot correct
  // inter-carrier interference (ICI), which only a PRE-FFT, per-sample time-domain derotation
  // removes. This version derotates the raw time-domain samples (ue->common_vars.rxdata) and
  // re-invokes the REAL nr_slot_fep() (FFT/CP-removal) on the corrected copy, then proceeds through
  // the REAL nr_pbch_channel_estimation / nr_generate_pbch_llr / nr_pbch_decode verbatim -- entirely
  // separate shadow buffers, nr_pbch_decode called with ue=NULL (an already-supported, null-checked
  // calling convention) so nothing here writes back into UE state.
  //
  // Window, not full-buffer: nr_slot_fep indexes rxdata at an ABSOLUTE sample offset computed from
  // get_samples_slot_timestamp() + per-symbol CP accounting, so the shadow buffer must be allocated
  // at the SAME total size nr_slot_fep expects (else its internal offset reads out of bounds) -- but
  // only the WINDOW actually read needs correct (derotated) content. Bounded using the real
  // get_samples_slot_timestamp() call (zero risk of misreplicating that arithmetic) plus a generous
  // fixed margin (8x nb_prefix_samples), rather than reimplementing nr_slot_fep's full offset
  // formula (including its wrapped-buffer edge case, which its own comment says only triggers during
  // initial sync -- out of scope for this ongoing-tracking diagnostic). Also clamped to 2x
  // samples_per_frame (nr_slot_fep's own total_samples), so the window can never read past the real
  // rxdata buffer's actual extent.
  {
    static int cfo_sweep = -1;
    if (cfo_sweep < 0)
      cfo_sweep = (getenv("ISAC_PBCH_CFO_SWEEP") != NULL) ? 1 : 0;
    if (cfo_sweep && relPbchSymb >= 0 && relPbchSymb < NB_SYMBOLS_PBCH) {
#define CFO_SWEEP_NHYP 33
#define CFO_SWEEP_MAX_SAMPLES 3000000
#define CFO_SWEEP_MAX_ANT 4
      static double hyp_hz[CFO_SWEEP_NHYP];
      static int hyp_init = 0;
      if (!hyp_init) {
        for (int h = 0; h < CFO_SWEEP_NHYP; h++)
          hyp_hz[h] = -8000.0 + 500.0 * h; // -8000 .. +8000 Hz in 500 Hz steps
        hyp_init = 1;
      }
      static __thread int16_t shadow_e_rx[CFO_SWEEP_NHYP][NR_POLAR_PBCH_E];
      static __thread double shadow_log2maxh[CFO_SWEEP_NHYP];
      static __thread c16_t shadow_sym0_est[CFO_SWEEP_NHYP][NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
      static __thread int shadow_occasions_left = 10;
      static __thread c16_t *shadow_rxdata[CFO_SWEEP_MAX_ANT];
      static __thread c16_t *shadow_rxdataF_full;

      if (fp->nb_antennas_rx <= CFO_SWEEP_MAX_ANT && shadow_rxdata[0] == NULL) {
        for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++)
          shadow_rxdata[aarx] = malloc(sizeof(c16_t) * CFO_SWEEP_MAX_SAMPLES);
        shadow_rxdataF_full = malloc(sizeof(c16_t) * fp->nb_antennas_rx * fp->samples_per_slot_wCP);
      }

      if (relPbchSymb == 0)
        for (int h = 0; h < CFO_SWEEP_NHYP; h++)
          shadow_log2maxh[h] = 0.0;

      if (shadow_occasions_left > 0 && shadow_rxdata[0] != NULL && fp->nb_antennas_rx <= CFO_SWEEP_MAX_ANT) {
        const double fs = fp->samples_per_subframe * 1000.0; // samples/sec
        const long total_samples = 2L * fp->samples_per_frame; // matches nr_slot_fep's own total_samples
        const uint32_t slot_base = get_samples_slot_timestamp(fp, proc->nr_slot_rx);
        const long margin = 8L * (long)fp->nb_prefix_samples;
        long w0 = (long)slot_base + (long)fp->ofdm_symbol_size * symbol - margin;
        long w1 = (long)slot_base + (long)fp->ofdm_symbol_size * (symbol + 1) + margin;
        if (w0 < 0) w0 = 0;
        if (w1 > total_samples) w1 = total_samples;
        if (w1 > CFO_SWEEP_MAX_SAMPLES) w1 = CFO_SWEEP_MAX_SAMPLES;

        for (int h = 0; h < CFO_SWEEP_NHYP; h++) {
          for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
            for (long n = w0; n < w1; n++) {
              const double t = (double)n / fs;
              // correction = e^{-j*2*pi*hyp_hz*t}, undoing a signal of the form s(t)*e^{+j*2*pi*cfo*t}
              // (sign convention verified against dot_product()'s conj(x)*y and nr_ue_pbch_freq_offset()).
              const double phase = -2.0 * M_PI * hyp_hz[h] * t;
              const double cr = cos(phase), ci = sin(phase);
              const double re = ue->common_vars.rxdata[aarx][n].r, im = ue->common_vars.rxdata[aarx][n].i;
              shadow_rxdata[aarx][n].r = (int16_t)lround(re * cr - im * ci);
              shadow_rxdata[aarx][n].i = (int16_t)lround(re * ci + im * cr);
            }
          }

          c16_t(*rxdataF_shadow_full)[fp->samples_per_slot_wCP] = (c16_t(*)[fp->samples_per_slot_wCP])shadow_rxdataF_full;
          nr_slot_fep(ue, fp, proc->nr_slot_rx, symbol, rxdataF_shadow_full, link_type_dl, 0, shadow_rxdata);

          c16_t rxdataF_shadow[fp->nb_antennas_rx][fp->ofdm_symbol_size];
          for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++)
            memcpy(rxdataF_shadow[aarx],
                   rxdataF_shadow_full[aarx] + symbol * fp->ofdm_symbol_size,
                   sizeof(c16_t) * fp->ofdm_symbol_size);

          c16_t dl_ch_estimates_shadow[fp->nb_antennas_rx][fp->ofdm_symbol_size];
          for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
            nr_pbch_channel_estimation(fp,
                                       NULL,
                                       dl_ch_estimates_shadow[aarx],
                                       proc,
                                       relPbchSymb,
                                       ssbIndex & 7,
                                       symbIdxInFrame > (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2),
                                       fp->ssb_start_subcarrier,
                                       rxdataF_shadow[aarx],
                                       false,
                                       fp->Nid_cell);
          }

          if (relPbchSymb == 0)
            memcpy(shadow_sym0_est[h], dl_ch_estimates_shadow[0], sizeof(shadow_sym0_est[h]));

          nr_generate_pbch_llr(ue,
                               proc,
                               fp,
                               relPbchSymb + 1,
                               ssbIndex,
                               fp->Nid_cell,
                               fp->ssb_start_subcarrier,
                               rxdataF_shadow,
                               dl_ch_estimates_shadow,
                               shadow_e_rx[h],
                               &shadow_log2maxh[h]);

          if (relPbchSymb == NB_SYMBOLS_PBCH - 1) {
            // coherence: |dot(sym0,sym2)| / sqrt(E0*E2) in [0,1] -- same conj(x)*y convention as
            // dot_product()/nr_ue_pbch_freq_offset(), magnitude-only so sign convention doesn't matter.
            const int nb_re = NR_PBCH_NUM_RB * NR_NB_SC_PER_RB;
            double lr = 0.0, li = 0.0, e0 = 0.0, e2 = 0.0;
            for (int k = 0; k < nb_re; k++) {
              const c16_t x = shadow_sym0_est[h][k], y = dl_ch_estimates_shadow[0][k];
              lr += (double)x.r * y.r + (double)x.i * y.i;
              li += (double)x.r * y.i - (double)x.i * y.r;
              e0 += (double)x.r * x.r + (double)x.i * x.i;
              e2 += (double)y.r * y.r + (double)y.i * y.i;
            }
            const double coh = (e0 > 0.0 && e2 > 0.0) ? sqrt(lr * lr + li * li) / sqrt(e0 * e2) : 0.0;

            double llr_absmean = 0.0;
            for (int i = 0; i < NR_POLAR_PBCH_E; i++)
              llr_absmean += fabs((double)shadow_e_rx[h][i]);
            llr_absmean /= NR_POLAR_PBCH_E;

            fapiPbch_t shadow_result;
            int shfb, sssb_idx, ssymb_offset = 0;
            const int shadowSuccess = nr_pbch_decode(NULL,
                                                      fp,
                                                      proc,
                                                      ssbIndex,
                                                      fp->Nid_cell,
                                                      shadow_e_rx[h],
                                                      &shfb,
                                                      &sssb_idx,
                                                      &ssymb_offset,
                                                      &shadow_result);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            LOG_I(PHY,
                  "TSYNC_CFO_SWEEP utc_ns=%lld frame=%d slot=%d ssb=%d hyp_hz=%.1f coherence=%.4f llr_absmean=%.2f crc_ok=%d\n",
                  (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
                  proc->frame_rx,
                  proc->nr_slot_rx,
                  ssbIndex,
                  hyp_hz[h],
                  coh,
                  llr_absmean,
                  (shadowSuccess == 0) ? 1 : 0);
          }
        }
        if (relPbchSymb == NB_SYMBOLS_PBCH - 1)
          shadow_occasions_left--;
      }
    }
  }

  const int nid = fp->Nid_cell;
  const int ssb_start_subcarrier = fp->ssb_start_subcarrier;
  /* FIXED-POINT HEADROOM FOR THE SSB REs BEFORE THE ESTIMATOR.
   *
   * nr_pbch_channel_estimation() is fixed point end to end -- an LS product at
   * c16mulShift(...,15) and an int16 filt16a interpolation -- so its output precision is set by how
   * far the received SSB REs sit below int16 full scale, and nothing downstream can recover bits
   * lost there. That level is BANDWIDTH DEPENDENT: at 122.88 MS/s the same ADC full scale covers
   * 100 MHz instead of 20 MHz, so each RE of the SSB's fixed 3.6 MHz carries proportionally less of
   * it, and the DFT spreads the same energy over 4x the bins.
   *
   * MEASURED on one cell, same SSB, minutes apart (ISAC_CHEST_COH): the estimate is real and smooth
   * at both widths (240 populated bins, adjacent-subcarrier coherence 0.93-0.97 at 51 PRB and
   * 0.87-0.89 at 273 -- so neither noise nor a wrong mapping), but the received SSB REs are 2.9x
   * lower at 4096 (ssb_band_mean 82.7 against 241.8, -9.3 dB) and the estimate 2.6x lower
   * (rms 108.4 against 280.7) while the int16 quantisation floor is unchanged. That is the CIR
   * peak/median collapse (2721x -> 95x) that made the +-CP timing search chase noise peaks and
   * killed PBCH tracking at every bandwidth above 20 MHz.
   *
   * So scale the estimator's INPUT, not its output: take the 240 SSB REs, derive a shift from their
   * own measured magnitude, and hand the estimator a copy at a fixed working point. The timing peak
   * search is scale invariant and the PBCH LLR path derives log2_maxh from the channel level it is
   * given, so both absorb a power-of-two gain; the shift is reported so saturation is measurable
   * rather than assumed. Applied only where the deficit exists (ofdm_symbol_size > 1024, the width
   * at which this chain is measured healthy), so the 20 MHz path stays bit-identical. */
  const int Nsym = fp->ofdm_symbol_size;
  unsigned int ssb_off0 = fp->first_carrier_offset + ssb_start_subcarrier;
  if (ssb_off0 >= (unsigned)Nsym)
    ssb_off0 -= Nsym;
  for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) {
    const c16_t *est_in = rxdataF[aarx];
    __attribute__((aligned(32))) c16_t rxf_scaled[Nsym];
    int ssb_shift = 0;
    if (Nsym > 1024) {
      int maxabs = 0;
      for (int i = 0; i < 240; i++) {
        const unsigned int sc = (ssb_off0 + i) % (unsigned)Nsym;
        const int r = abs(rxdataF[aarx][sc].r), im = abs(rxdataF[aarx][sc].i);
        if (r > maxabs)
          maxabs = r;
        if (im > maxabs)
          maxabs = im;
      }
      /* Target 2^13: the LS product and the 3-tap filt16a accumulation both run in int16, so leave
       * two bits of headroom below 2^15 for them rather than filling the word. */
      if (maxabs > 0)
        ssb_shift = 13 - (int)log2_approx((uint32_t)maxabs);
      if (ssb_shift < 0)
        ssb_shift = 0;
      if (ssb_shift > 0) {
        memcpy(rxf_scaled, rxdataF[aarx], sizeof(c16_t) * Nsym);
        for (int i = 0; i < 240; i++) {
          const unsigned int sc = (ssb_off0 + i) % (unsigned)Nsym;
          rxf_scaled[sc].r = (int16_t)(rxdataF[aarx][sc].r << ssb_shift);
          rxf_scaled[sc].i = (int16_t)(rxdataF[aarx][sc].i << ssb_shift);
        }
        est_in = rxf_scaled;
      }
    }
    nr_pbch_channel_estimation(&ue->frame_parms,
                               NULL,
                               dl_ch_estimates[aarx],
                               proc,
                               relPbchSymb,
                               ssbIndex & 7,
                               symbIdxInFrame > (fp->slots_per_frame * NR_SYMBOLS_PER_SLOT / 2),
                               ssb_start_subcarrier,
                               est_in,
                               false,
                               nid);
    /* Saturation check on the estimator's int16 output at the new input level -- the obvious
     * failure mode of adding gain, so measure it instead of assuming. Rate limited. */
    if (ssb_shift > 0) {
      static int s_sat_left = 12;
      if (s_sat_left > 0) {
        int hmax = 0;
        for (int i = 0; i < 244; i++) {
          const int r = abs(dl_ch_estimates[aarx][i].r), im = abs(dl_ch_estimates[aarx][i].i);
          if (r > hmax)
            hmax = r;
          if (im > hmax)
            hmax = im;
        }
        s_sat_left--;
        LOG_W(PHY, "SENSING: CHESTGAIN N=%d ant=%d shift=%d |H|max=%d%s\n",
              Nsym, aarx, ssb_shift, hmax, (hmax >= 32000) ? " SATURATED" : "");
      }
    }
    // Get channel response to measure timing error
    if ((fp->ssb_index == ssbIndex) && (relPbchSymb == NB_SYMBOLS_PBCH - 1)) {
      /* ISAC_CHEST_RAW=<path> (diagnostic, default off): dump the 240-value frequency-domain
       * estimate AND the 240 source REs it was built from, so the 1024-vs-4096 estimates can be
       * compared directly instead of inferred from CIR statistics. Per SSB, antenna 0 only, 24 max:
       * header int32 {N, ssb_offset, frame, slot}, then 244 c16_t of dl_ch_estimates[0..243],
       * then the WHOLE symbol: ofdm_symbol_size c16_t of rxdataF[0..N-1]. Whole-symbol (not just
       * the 240 SSB REs) so the SSB can be LOCATED offline rather than assumed: the PBCH DM-RS
       * repeats every 20 ms, so a per-subcarrier consecutive-SSB coherence profile marks the DM-RS
       * comb wherever it actually is. A 240-RE dump cannot distinguish "SSB destroyed" from
       * "reading the wrong 240 subcarriers on a fully loaded carrier". */
      {
        static int s_raw_left = -1;
        static FILE *s_raw = NULL;
        if (s_raw_left < 0) {
          const char *pth = getenv("ISAC_CHEST_RAW");
          s_raw = (pth && pth[0]) ? fopen(pth, "wb") : NULL;
          s_raw_left = s_raw ? 24 : 0;
        }
        if (s_raw_left > 0 && s_raw && aarx == 0) {
          s_raw_left--;
          unsigned int so = fp->first_carrier_offset + fp->ssb_start_subcarrier;
          if (so >= (unsigned)fp->ofdm_symbol_size)
            so -= fp->ofdm_symbol_size;
          int32_t hdr[4] = {fp->ofdm_symbol_size, (int32_t)so, proc->frame_rx, proc->nr_slot_rx};
          fwrite(hdr, sizeof(hdr), 1, s_raw);
          fwrite(&dl_ch_estimates[aarx][0], sizeof(c16_t), 244, s_raw);
          fwrite(&rxdataF[aarx][0], sizeof(c16_t), fp->ofdm_symbol_size, s_raw);
          fflush(s_raw);
          if (s_raw_left == 0) { fclose(s_raw); s_raw = NULL; }
        }
      }
      /* ISAC_CHEST_COH=1 (diagnostic, default off): adjacent-subcarrier coherence and RMS of the
       * TRACKING estimate over the REs it actually populated, to compare against the same quantity
       * on the acquisition path (nr_pbch.c's CIRTEST). A real channel is smooth across 3.6 MHz, so
       * coherence ~1; noise or a wrong DM-RS/subcarrier mapping gives ~0. Cheap: one pass. */
      static int s_coh = -1;
      if (s_coh < 0)
        s_coh = (getenv("ISAC_CHEST_COH") && atoi(getenv("ISAC_CHEST_COH"))) ? 1 : 0;
      static int s_coh_left = 24;
      if (s_coh && s_coh_left > 0 && aarx == 0) {
        s_coh_left--;
        const c16_t *h = dl_ch_estimates[aarx];
        const int N = fp->ofdm_symbol_size;
        double lr = 0.0, li = 0.0, den = 0.0, e = 0.0;
        int nz = 0, prev = -1;
        for (int k = 0; k < N; k++) {
          if (h[k].r == 0 && h[k].i == 0)
            continue;
          nz++;
          e += (double)h[k].r * h[k].r + (double)h[k].i * h[k].i;
          if (prev >= 0 && k == prev + 1) { // adjacent populated pair only
            const double ar = h[prev].r, ai = h[prev].i, br = h[k].r, bi = h[k].i;
            lr += ar * br + ai * bi;
            li += ar * bi - ai * br;
            den += sqrt((ar * ar + ai * ai) * (br * br + bi * bi));
          }
          prev = k;
        }
        /* Is the SSB actually in the bins the estimator reads? Compare the mean |rxdataF| over the
         * 240 subcarriers it walks (absolute positions, same arithmetic as the estimator) against
         * the mean over the whole symbol. SSB present and correctly mapped -> ratio well above 1;
         * ratio ~1 means those bins hold ordinary traffic/noise, i.e. a wrong mapping. */
        unsigned int ssb_off = fp->first_carrier_offset + ssb_start_subcarrier;
        if (ssb_off >= (unsigned)N)
          ssb_off -= N;
        double ssb_sum = 0.0, all_sum = 0.0;
        for (int i = 0; i < N; i++)
          all_sum += hypot((double)rxdataF[aarx][i].r, (double)rxdataF[aarx][i].i);
        double ssb_first = 0.0, ssb_last = 0.0; // first/last 20 SSB REs: structure vs flat
        for (int i = 0; i < 240; i++) {
          const unsigned int sc = (ssb_off + i) % (unsigned)N;
          const double m = hypot((double)rxdataF[aarx][sc].r, (double)rxdataF[aarx][sc].i);
          ssb_sum += m;
          if (i < 20)
            ssb_first += m;
          else if (i >= 220)
            ssb_last += m;
        }
        LOG_W(PHY,
              "SENSING: CHESTCOH path=tracking N=%d ssb=%d nz=%d rms=%.1f coh=%.4f "
              "ssb_band_mean=%.1f sym_mean=%.1f ratio=%.2f edge_first=%.1f edge_last=%.1f\n",
              N, ssbIndex, nz, nz ? sqrt(e / nz) : 0.0, den > 0.0 ? sqrt(lr * lr + li * li) / den : 0.0,
              ssb_sum / 240.0, all_sum / N, (all_sum > 0.0) ? (ssb_sum / 240.0) / (all_sum / N) : 0.0,
              ssb_first / 20.0, ssb_last / 20.0);
      }
      // do ifft of channel estimate
      freq2time(fp->ofdm_symbol_size, (int16_t *)&dl_ch_estimates[aarx], (int16_t *)dl_ch_estimates_time[aarx]);
      UEscopeCopy(ue, pbchDlChEstimateTime, (void *)dl_ch_estimates_time, sizeof(c16_t), fp->nb_antennas_rx, fp->ofdm_symbol_size, 0);
    }
  }

  // Phase 2 (roadmap artifact "Cell-Agnostic Passive Receiver"): submit this PBCH symbol's
  // per-antenna channel estimate as a CFR row. No grant, no RNTI, no decode required -- every
  // NR cell transmits this on a fixed raster, so it is the sensing source most robust to
  // cell-specific misconfiguration. Guarded by nr_isac_source_enabled() so this is a true no-op
  // (not even the k_abs derivation runs) unless "ssb" is in sensing.sources.
  if (nr_isac_enabled() && nr_isac_source_enabled(NR_ISAC_SRC_SSB)) {
    uint32_t k_abs[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
    uint32_t l_sym[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
    // P11fix (nr_isac_ssb_axis.h): ssb_start_subcarrier is already Point-A-referenced and needs
    // no k_ssb term here; the axis wraps modulo the carrier's own bandwidth in subcarriers
    // (fp->N_RB_DL * 12), NOT fp->ofdm_symbol_size (the FFT size), which differ (e.g. 3276 vs
    // 4096 at 273 PRB) and can wrap a value outside the declared [0, nof_prb*12) range.
    nr_isac_ssb_k_abs(ssb_start_subcarrier, fp->N_RB_DL * 12, k_abs);
    for (uint32_t i = 0; i < NR_PBCH_NUM_RB * NR_NB_SC_PER_RB; i++) {
      l_sym[i] = (uint32_t)relPbchSymb;
    }
    // NR_ISAC_SSB_MAX_ANT mirrors csi_rx.c's NR_ISAC_CSIRS_MAX_ANT convention -- there is no
    // shared NR_MAX_RX_ANTENNAS constant anywhere in the PHY tree (verified by grep), so this
    // follows the existing per-ISAC-tap local-bound pattern rather than inventing a new one.
    enum { NR_ISAC_SSB_MAX_ANT = 8 };
    const uint32_t nof_ant = nr_isac_rx_channels() > 0
                                  ? (nr_isac_rx_channels() < (uint32_t)fp->nb_antennas_rx ? nr_isac_rx_channels()
                                                                                            : (uint32_t)fp->nb_antennas_rx)
                                  : 1;
    // dl_ch_estimates is ANTENNA-MAJOR ALREADY at this call site (dl_ch_estimates[aarx]), and
    // nr_isac_submit_cfr_multi() wants one contiguous antenna-major buffer with an explicit
    // stride -- dl_ch_estimates[aarx] are separate allocations, not one contiguous block, so pack
    // them into a stack buffer rather than assuming a stride across dl_ch_estimates itself.
    float h_packed[2 * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) * NR_ISAC_SSB_MAX_ANT];
    const uint32_t nof_ant_clamped = nof_ant > NR_ISAC_SSB_MAX_ANT ? NR_ISAC_SSB_MAX_ANT : nof_ant;
    for (uint32_t a = 0; a < nof_ant_clamped; a++) {
      const c16_t* est = (const c16_t*)dl_ch_estimates[a];
      for (uint32_t i = 0; i < NR_PBCH_NUM_RB * NR_NB_SC_PER_RB; i++) {
        h_packed[2 * (a * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) + i) + 0] = (float)est[i].r;
        h_packed[2 * (a * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) + i) + 1] = (float)est[i].i;
      }
    }
    const nr_isac_carrier_t carrier = {
        .nof_prb         = (uint32_t)fp->N_RB_DL,
        .scs_hz          = fp->subcarrier_spacing,
        .dl_center_hz    = fp->dl_CarrierFreq,
        .pci             = (uint16_t)fp->Nid_cell,
        .slots_per_frame = fp->slots_per_frame,
    };
    // Absolute (frame,slot) index, matching csi_rx.c's own nr_isac_submit_cfr_multi() call
    // (nr_isac_carrier_t.slots_per_frame * frame_rx + nr_slot_rx) -- proc->nr_slot_rx ALONE is only
    // slot-within-frame [0, slots_per_frame), and this cell's single SSB beam recurs at the SAME
    // within-frame slot every occurrence (period 40 slots = 2 frames), so every submission reported
    // an IDENTICAL slot_idx: the engine's slow-time delta (sensing_engine.cc accumulate_cpi) saw
    // zero elapsed slots between consecutive rows, merged every submission into row 0, and no CPI
    // ever reached cpi_slots rows to close. Found live 2026-09-04 (Task 4: zero "SENSING: CPI #"
    // lines over a 150 s ssb-only capture, root-caused by inspecting sensing_engine.cc + the
    // csi_rs tap's slot_idx computation for comparison).
    const uint32_t ssb_abs_slot = (uint32_t)(proc->frame_rx * fp->slots_per_frame + proc->nr_slot_rx);
    nr_isac_submit_cfr_multi(ssb_abs_slot,
                             0.0f,
                             NR_ISAC_SRC_SSB,
                             &carrier,
                             h_packed,
                             nof_ant_clamped,
                             NR_PBCH_NUM_RB * NR_NB_SC_PER_RB,
                             k_abs,
                             l_sym,
                             NR_PBCH_NUM_RB * NR_NB_SC_PER_RB,
                             0.0f);
  }

  // Copy current symbol estimate for FO estimation
  if (dl_ch_estimates_symbol != NULL)
    memcpy(dl_ch_estimates_symbol, dl_ch_estimates[0], sizeof(*dl_ch_estimates_symbol) * NR_PBCH_NUM_RB * NR_NB_SC_PER_RB);

  const int symbIdxInSSB = relPbchSymb + 1;
  nr_generate_pbch_llr(ue, proc, fp, symbIdxInSSB, ssbIndex, nid, ssb_start_subcarrier, rxdataF, dl_ch_estimates, pbch_e_rx,
                       log2_maxh_state);
  // Do measurements on middle symbol of PBCH block
  if (relPbchSymb == 1) {
    nr_ue_ssb_rsrp_measurements(ue, ssbIndex, proc, rxdataF);
    nr_ue_rrc_measurements(ue, proc, rxdataF);
    // resetting ssb index for PBCH detection if there is a stronger SSB index
    if (ue->measurements.ssb_rsrp_dBm[ssbIndex] > ue->measurements.ssb_rsrp_dBm[fp->ssb_index]) {
      fp->ssb_index = ssbIndex;
    }
  }

  return ssbIndex;
}

static int pbch_process(PHY_VARS_NR_UE *UE,
                        const UE_nr_rxtx_proc_t *proc,
                        const int symbol,
                        int *ssbIndex,
                        c16_t pbch_ch_est_sym1[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB],
                        c16_t pbch_ch_est_time[UE->frame_parms.nb_antennas_rx][UE->frame_parms.ofdm_symbol_size],
                        int16_t pbch_e_rx[NR_POLAR_PBCH_E],
                        int *pbchSymbCnt,
                        double *log2_maxh_state)
{
  int sampleShift = INT_MAX;

  // Buffer to hold symbol 3 estimates for FO estimation
  c16_t pbch_ch_est_sym3[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
  // Choose estimates buffer for FO compensation based on current PBCH symbol
  c16_t *cur_pbch_est = NULL;
  if (*pbchSymbCnt == 0)
    cur_pbch_est = pbch_ch_est_sym1;
  else if (*pbchSymbCnt == 2)
    cur_pbch_est = pbch_ch_est_sym3;

  *ssbIndex = nr_process_pbch_symbol(UE, proc, symbol, *ssbIndex, pbch_ch_est_time, cur_pbch_est, pbch_e_rx, log2_maxh_state);
  // If valid PBCH symbol, increment symbol count.
  if (*ssbIndex > -1)
    (*pbchSymbCnt)++;
  // Current symbol is last PBCH symbol, decode it.
  if (*pbchSymbCnt == 3) {
    if (*ssbIndex == UE->frame_parms.ssb_index) {
      fapiPbch_t pbchResult; // TODO: Not used anywhere. To be cleaned later
      int hfb, ssb_idx, symb_offset = 0;
      const int nid = UE->frame_parms.Nid_cell;
      const int pbchSuccess =
          nr_pbch_decode(UE, &UE->frame_parms, proc, *ssbIndex, nid, pbch_e_rx, &hfb, &ssb_idx, &symb_offset, &pbchResult);
      // TEMPORARY DIAGNOSTIC (2026-08-02): dump per-symbol PBCH internals ONLY on a failed decode,
      // together with the last successful occasion's, so a failing and a working decode under
      // otherwise identical conditions can be diffed directly.
      extern void nr_pbch_diag_report(int success, int frame, int slot, int ssbIndex);
      nr_pbch_diag_report(pbchSuccess == 0, proc->frame_rx, proc->nr_slot_rx, *ssbIndex);
      if (pbchSuccess != 0)
        LOG_E(PHY, "Frame %d, slot %d, SSB Index %d. Error decoding PBCH!\n", proc->frame_rx, proc->nr_slot_rx, *ssbIndex);
      else
        T(T_NRUE_PHY_MIB, T_INT(proc->frame_rx), T_INT(proc->nr_slot_rx), T_INT(*ssbIndex), T_BUFFER(pbchResult.decoded_output, 3));
      // TIME-TRACKING AUDIT (2026-08-04, instrumentation only). Time tracking runs ONLY on a
      // successful PBCH decode, so a PBCH failure silently freezes the FFT-window correction and the
      // window free-runs. Record every SSB occasion -- success or not -- so PBCH tracking failures
      // can be correlated against PDCCH coherence loss. Enabled by ISAC_TSYNC_AUDIT=1.
      {
        static int audit = -1;
        if (audit < 0)
          audit = (getenv("ISAC_TSYNC_AUDIT") != NULL) ? 1 : 0;
        if (audit) {
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          LOG_I(PHY, "TSYNC_PBCH utc_ns=%lld frame=%d slot=%d ssb=%d pbch_ok=%d tracking_applied=%d\n",
                (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
                proc->frame_rx, proc->nr_slot_rx, *ssbIndex, (pbchSuccess == 0) ? 1 : 0,
                (UE->no_timing_correction == 0 && pbchSuccess == 0) ? 1 : 0);
        }
      }

      // CFO CAPTURE (2026-08-04c, capture-only, no behavioural change). nr_ue_pbch_freq_offset()
      // already exists and is already correct -- it is just normally only CALLED after a successful
      // decode (get_nrUE_params()->cont_fo_comp && pbchSuccess==0 below), the same "only fires after
      // success" deadlock already found for timing correction. pbch_ch_est_sym1/sym3 are populated
      // regardless of decode outcome, so compute and log the estimate on EVERY occasion here,
      // independently of whether the production cont_fo_comp path also runs. Nothing reads this
      // value; it does not feed sample_shift, freq_offset, or any decode input.
      {
        static int cfo_audit = -1;
        if (cfo_audit < 0)
          cfo_audit = (getenv("ISAC_TSYNC_AUDIT") != NULL) ? 1 : 0;
        if (cfo_audit) {
          const double cfo_hz = nr_ue_pbch_freq_offset(&UE->frame_parms, pbch_ch_est_sym1, pbch_ch_est_sym3);
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          LOG_I(PHY, "TSYNC_CFO_PBCH utc_ns=%lld frame=%d slot=%d ssb=%d pbch_ok=%d cfo_hz=%.3f\n",
                (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
                proc->frame_rx, proc->nr_slot_rx, *ssbIndex, (pbchSuccess == 0) ? 1 : 0, cfo_hz);
        }
      }

      /* ---- CFO TRIM LOOP (ISAC_CFO_TRACK_HZ = threshold in Hz, 0 = off) -------------------------
       * ROOT CAUSE it addresses (measured 2026-08-26, 21 runs, zero exceptions): the carrier
       * frequency offset is estimated ONCE at acquisition and never revisited. |err| < 600 Hz always
       * decodes (14/14, 59-91 %); |err| >= 600 Hz always decodes EXACTLY 0.0 % (7/7). A positive
       * control (no FO compensation, ~13.9 kHz left uncorrected) gave 0.0 % on 3/3.
       *
       * WHY NOT JUST --cont-fo-comp. That flag already gates a PI controller on this same estimator
       * a few hundred lines below -- but it ALSO disables the acquisition radio retune
       * (`if (!cont_fo_comp) nrue_ru_set_freq(...)` in UE_synch), replacing it with a digital
       * de-rotation in nr_slot_fep. On this rig that trade is fatal: measured 2026-08-26, the flag
       * gave 0/2 runs synced against 3/3 without it, and CLAUDE.md's Phase 0 note already recorded
       * that neither FO flag tracks at sensing-grade rate. So this trims the offset the radio is
       * ACTUALLY tuned to, using the mechanism that demonstrably works, instead of swapping it for
       * one that does not.
       *
       * NOT gated on pbchSuccess. The PI loop below is (`cont_fo_comp && pbchSuccess == 0`), which
       * deadlocks exactly when it is needed most: a badly mis-tuned receiver may never decode PBCH,
       * so the correction that would rescue it never runs. The audit block directly above proves the
       * escape -- pbch_ch_est_sym1/sym3 are populated REGARDLESS of decode outcome. Same
       * "only fires after success" trap this file already documents for timing correction.
       *
       * TWO STAGES ON PURPOSE. ISAC_CFO_TRACK_HZ alone only MEASURES and logs what it would do;
       * ISAC_CFO_TRACK_APPLY=1 is required before it retunes anything. The sign convention between
       * this estimator and nrue_ru_set_freq() is not something to assume -- and today three
       * plausible fixes (timing offset, accumulator double-count, acquisition consistency) each died
       * on contact with data, so a loop does not get to touch a working radio unvalidated. */
      {
        static int    s_trk_hz = -1;
        static int    s_trk_spread = 500;
        static int    s_trk_apply = 0;
        /* Evaluation period in SSBs. 50 (~1 s) was the original, and it makes the EARLIEST possible
         * retune n=250 -- five agreeing windows -- i.e. ~5 s of a run already lost, and measured
         * runs acted much later than that. The two populations this gate separates do NOT depend on
         * the period: the genuine one fires from the FIRST opportunity whatever that is, and the
         * spurious one only appears after ~1085 SSBs. So shortening the period moves the genuine
         * correction earlier without weakening the discriminator, which is the 5-window spread. */
        static int    s_trk_period = 50;
        static double s_ema = 0.0;
        static int    s_n = 0;
#define TRK_HIST 5
        static double s_hist[TRK_HIST];
        static int    s_streak = 0;
        if (s_trk_hz < 0) {
          const char *e = getenv("ISAC_CFO_TRACK_HZ");
          s_trk_hz = (e != NULL) ? atoi(e) : 0;
          const char *v = getenv("ISAC_CFO_TRACK_SPREAD_HZ");
          if (v != NULL) {
            s_trk_spread = atoi(v);
          }
          s_trk_apply = (getenv("ISAC_CFO_TRACK_APPLY") != NULL) ? 1 : 0;
          const char *pv = getenv("ISAC_CFO_TRACK_PERIOD");
          if (pv != NULL && atoi(pv) > 0) {
            s_trk_period = atoi(pv);
          }
        }
        if (s_trk_hz > 0) {
          const double res_hz = nr_ue_pbch_freq_offset(&UE->frame_parms, pbch_ch_est_sym1, pbch_ch_est_sym3);
          s_ema = (s_n == 0) ? res_hz : (0.75 * s_ema + 0.25 * res_hz);
          s_n++;
          /* Evaluate once per 50 SSBs (~1 s). A retune disturbs samples in flight, so it must stay
           * rare, and the EMA needs samples to mean anything. */
          if (s_n >= 8 && (s_n % s_trk_period) == 0) {
            if (fabs(s_ema) > (double)s_trk_hz) {
              /* ---- VALIDITY GATE -------------------------------------------------------------
               * The estimate is EXACT while the receiver holds lock (measured: +6055 vs a true
               * +6055 Hz error, -5455 vs -5453) but produces LATE SPURIOUS readings otherwise. On
               * TK_r6 -- a HEALTHY 88.3 % run whose acquisition was 40 Hz from perfect -- a bare
               * threshold proposed retuning by 4.2 kHz, which would have destroyed a working
               * capture. So a threshold alone is not safe to act on.
               *
               * The two populations separate on ONSET and SPREAD, measured:
               *   genuine : fires from the first opportunity (n=50), 44 times, all within +/-100 Hz
               *   spurious: fires only after ~1085 SSBs, 3 times, with a 3082 Hz JUMP between the
               *             1st and 2nd reading (-1093 -> -4175, and -1250 -> -4228 on TK_r4)
               * Requiring 5 CONSECUTIVE above-threshold windows whose spread is under
               * ISAC_CFO_TRACK_SPREAD_HZ accepts the genuine case and rejects both spurious ones.
               *
               * NOTE the spread must include the ONSET reading: the LAST three spurious values
               * (-4218/-4275/-4338) agree to within 120 Hz and would pass a short window. It is the
               * jump from the first reading that exposes them. Hence 5, not 3. */
              s_hist[s_streak % TRK_HIST] = s_ema;
              s_streak++;
              double mn = s_hist[0], mx = s_hist[0];
              const int have = (s_streak < TRK_HIST) ? s_streak : TRK_HIST;
              for (int q = 0; q < have; q++) {
                if (s_hist[q] < mn) mn = s_hist[q];
                if (s_hist[q] > mx) mx = s_hist[q];
              }
              const double spread = mx - mn;
              const bool stable = (s_streak >= TRK_HIST) && (spread < (double)s_trk_spread);
              const double cur = (double)UE->common_vars.freq_offset;
              const double proposed = cur + s_ema;
              LOG_W(PHY,
                    "SENSING: CFOTRK ema=%.1f Hz streak=%d spread=%.0f Hz stable=%s current=%.0f "
                    "proposed=%.0f thr=%d %s\n",
                    s_ema, s_streak, spread, stable ? "yes" : "no", cur, proposed, s_trk_hz,
                    (stable && s_trk_apply) ? "APPLYING" : "(no action)");
              if (stable && s_trk_apply) {
                /* REQUEST a re-acquisition rather than retuning under a running stream.
                 * MEASURED 2026-08-26 (GT_r4): retuning in place produced a correct proposal
                 * (-14968 against a true -14390, from a 3599 Hz error) and the run then died at
                 * active_s=4 -- the LO moved under samples already in flight. The stall-recovery
                 * path shows the receiver survives a FULL device re-init and resumes decoding
                 * (4/4, grew=YES), so a clean re-acquisition is demonstrably the safer delivery.
                 * The retune itself is done by the UE thread, which owns stream_status,
                 * shiftForNextFrame and the rebase flag -- state this function cannot reach, and
                 * leaving it stale is what makes an in-place retune unrecoverable. */
                nr_ue_cfo_resync_hz = (int)lround(proposed);
                atomic_store_explicit(&nr_ue_cfo_resync_request, 1, memory_order_release);
                s_ema = 0.0;
                s_n = 0;
                s_streak = 0;
              }
            } else {
              s_streak = 0; // dropped below threshold: the run of agreeing windows is broken
            }
          }
        }
      }

      // RAW CAPTURE (2026-08-04c, capture-only, opt-in ISAC_PBCH_CAPTURE=1, capped at 300 occasions).
      // Dumps pbch_ch_est_sym1/sym3 (channel estimate, pre-decode) and pbch_e_rx (LLRs going into the
      // polar decoder) so a CFO derotation and/or replay of the decode can be done OFFLINE against
      // the exact same captured data -- this file writes nothing back into the live decode path.
      {
        static int pbch_cap = -1;
        if (pbch_cap < 0)
          pbch_cap = (getenv("ISAC_PBCH_CAPTURE") != NULL) ? 1 : 0;
        static FILE *pbch_cap_fp = NULL;
        static int pbch_cap_left = 300;
        if (pbch_cap && pbch_cap_left > 0) {
          if (!pbch_cap_fp) {
            const char *path = getenv("ISAC_PBCH_CAPTURE_PATH");
            pbch_cap_fp = fopen(path ? path : "/tmp/pbch_capture.bin", "wb");
          }
          if (pbch_cap_fp) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            const int64_t utc_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            const int32_t frame_i = proc->frame_rx, slot_i = proc->nr_slot_rx, ssb_i = *ssbIndex;
            const int32_t ok_i = (pbchSuccess == 0) ? 1 : 0;
            const int32_t nre = NR_PBCH_NUM_RB * NR_NB_SC_PER_RB;
            const int32_t ne = NR_POLAR_PBCH_E;
            fwrite(&utc_ns, sizeof(utc_ns), 1, pbch_cap_fp);
            fwrite(&frame_i, sizeof(frame_i), 1, pbch_cap_fp);
            fwrite(&slot_i, sizeof(slot_i), 1, pbch_cap_fp);
            fwrite(&ssb_i, sizeof(ssb_i), 1, pbch_cap_fp);
            fwrite(&ok_i, sizeof(ok_i), 1, pbch_cap_fp);
            fwrite(&nre, sizeof(nre), 1, pbch_cap_fp);
            fwrite(&ne, sizeof(ne), 1, pbch_cap_fp);
            fwrite(pbch_ch_est_sym1, sizeof(c16_t), nre, pbch_cap_fp);
            fwrite(pbch_ch_est_sym3, sizeof(c16_t), nre, pbch_cap_fp);
            fwrite(pbch_e_rx, sizeof(int16_t), ne, pbch_cap_fp);
            fflush(pbch_cap_fp);
            pbch_cap_left--;
          }
        }
      }

      // Measure timing offset if PBCH is present in slot.
      // CAUSAL TEST (2026-08-04b): normally this only runs AFTER a successful decode, which is a
      // structural deadlock if PBCH has never once succeeded (correction needs success, success may
      // need correction). ISAC_FORCE_GLOBAL_SYNC=1 also runs it on FAILURES, so the forced
      // full-symbol correction inside nr_adjust_synch_ue() actually gets a chance to run and be
      // tested, instead of silently never firing.
      static int force_global_call = -1;
      if (force_global_call < 0)
        force_global_call = (getenv("ISAC_FORCE_GLOBAL_SYNC") != NULL) ? 1 : 0;
      // ---- CRC-INDEPENDENT TIMING UPDATE (2026-08-06) -------------------------------------
      // A CRC protects the decoded MIB bits. The timing estimate comes from the PBCH-DMRS channel
      // impulse response and is independent of whether those bits happened to decode -- so gating
      // this call on pbchSuccess made a failed payload stop timing recovery, precisely when timing
      // recovery is most needed. That is the deadlock: no decode -> no timing update -> no decode.
      // The reliability of the MEASUREMENT is now judged inside nr_adjust_synch_ue() from the CIR
      // itself (ISAC_TSYNC_MIN_EWIN), which is the correct place for it.
      // MIB contents are still taken only on CRC success -- that is untouched above.
      // ISAC_TSYNC_CRC_GATE=1 restores the old CRC-gated behaviour for comparison.
      static int crc_gate = -1;
      if (crc_gate < 0)
        crc_gate = (getenv("ISAC_TSYNC_CRC_GATE") && atoi(getenv("ISAC_TSYNC_CRC_GATE"))) ? 1 : 0;
      const bool timing_allowed = crc_gate ? (pbchSuccess == 0 || force_global_call) : true;
      // ---- TWO-STAGE TIMING (2026-08-06) ------------------------------------------------
      // Stage 1 (coarse): when PBCH-DMRS coherence at the position we are using is below the
      // valid-match level, the CIR estimator cannot be trusted -- it only sees +-nb_prefix_samples
      // and, measured on this cell, reports a confident near-zero while sitting ~2100 samples away
      // on a traffic feature. Re-anchor from a bounded DMRS-coherence search instead, and return
      // the displacement so it flows into shiftForNextFrame, i.e. the GLOBAL stream origin --
      // PDCCH and the rest of the slot move with PBCH, unlike a PBCH-local FFT bias.
      // Stage 2 (fine): once coherence is valid the receiver is genuinely on the SSB and inside
      // the estimator's capture range, so nr_adjust_synch_ue() does the few-sample correction it
      // was designed for.
      // Thresholds are diagnostic values from this cell's calibration (valid 0.999, floor
      // 0.80-0.92) with hysteresis; they need re-calibrating at lower SNR before production use.
      static int s_anchor = -1;
      if (s_anchor < 0)
        s_anchor = (getenv("ISAC_DMRS_ANCHOR") && atoi(getenv("ISAC_DMRS_ANCHOR"))) ? 1 : 0;
      static double s_enter = -1.0, s_stay = -1.0;
      if (s_enter < 0.0) {
        const char *e = getenv("ISAC_ANCHOR_ENTER");
        s_enter = e ? atof(e) : 0.99;
        const char *t = getenv("ISAC_ANCHOR_STAY");
        s_stay = t ? atof(t) : 0.97;
      }
      static int s_sign = INT_MIN;
      if (s_sign == INT_MIN) {
        const char *e = getenv("ISAC_ANCHOR_SIGN");
        s_sign = e ? atoi(e) : -1;
      }
      static bool s_locked = false;
      static int s_last_cir_resid = 0;
      if (s_anchor && UE->no_timing_correction == 0) {
        extern int nr_pbch_dmrs_anchor(const NR_DL_FRAME_PARMS *, const UE_nr_rxtx_proc_t *,
                                       c16_t *const *, int, unsigned, double *, double *, long *);
        double cohp = 0.0, cohb = 0.0;
        long dl = 0;
        /* Always 0: the first-PBCH-symbol FEP offset used to be latched by the block that
         * x410-100MHz replaced. This diagnostic is from the retracted PBCH-tracking
         * investigation, so it stays inert rather than being resurrected on a guess. */
        static const int s_pbch_sym1_off = 0;
        if (s_pbch_sym1_off != 0
            && nr_pbch_dmrs_anchor(&UE->frame_parms, proc, UE->common_vars.rxdata,
                                   2 * UE->frame_parms.samples_per_frame, s_pbch_sym1_off,
                                   &cohp, &cohb, &dl)) {
          const double thr = s_locked ? s_stay : s_enter;
          static int s_anchor_log = 40;

          // ---- POST-REBASE VERIFICATION TRACE (2026-08-06) -------------------------------
          // Logged on EVERY occasion, not only when the anchor fires, and tagged with how many
          // occasions have elapsed since the last rebase. This is what separates the three
          // possible outcomes: rebase lands then drifts (coh starts ~0.999 and decays, delta
          // grows), rebase never lands (coh stays low and delta stays large from occasion 0),
          // or rebase lands and holds while CRC still fails (coh stays high -- timing is then
          // no longer the PBCH problem). cir_resid is the previous occasion's
          // nr_adjust_synch_ue() return, i.e. the fine correction it wanted.
          {
            extern _Atomic int nr_ue_rebase_epoch;
            extern _Atomic long nr_ue_diag_rf_timestamp;
            extern _Atomic long nr_ue_diag_samples_consumed;
            static int s_last_epoch = -1;
            static int s_since_rebase = -1;
            const int ep = atomic_load_explicit(&nr_ue_rebase_epoch, memory_order_relaxed);
            if (ep != s_last_epoch) { s_last_epoch = ep; s_since_rebase = 0; }
            else if (s_since_rebase >= 0) s_since_rebase++;
            const long ts_now = atomic_load_explicit(&nr_ue_diag_rf_timestamp, memory_order_relaxed);
            const long cons_now = atomic_load_explicit(&nr_ue_diag_samples_consumed, memory_order_relaxed);
            static long s_ts_prev = 0, s_cons_prev = 0;
            static int s_trace_left = 120;
            if (s_trace_left > 0) {
              s_trace_left--;
              LOG_W(PHY,
                    "SENSING: POSTREBASE frame=%d n_since_rebase=%d epoch=%d state=%s "
                    "coh_at_pred=%.4f best_coh=%.4f delta=%+ld thr=%.3f cir_resid_prev=%d "
                    "ts=%ld d_ts=%ld consumed=%ld d_consumed=%ld\n",
                    proc->frame_rx, s_since_rebase, ep, s_locked ? "LOCKED" : "SUSPECT",
                    cohp, cohb, dl, thr, s_last_cir_resid,
                    ts_now, (s_ts_prev ? ts_now - s_ts_prev : 0),
                    cons_now, (s_cons_prev ? cons_now - s_cons_prev : 0));
              s_ts_prev = ts_now;
              s_cons_prev = cons_now;
            }
          }

          if (cohp >= thr) {
            s_locked = true; /* on the real SSB and inside capture range -> fine tracking below */
          } else {
            s_locked = false;
            const int shift = (int)(s_sign * dl);
            if (s_anchor_log > 0) {
              s_anchor_log--;
              LOG_W(PHY,
                    "SENSING: ANCHOR frame=%d slot=%d coh_at_pred=%.4f best_coh=%.4f delta=%+ld "
                    "-> global shift=%+d (coarse re-anchor)\n",
                    proc->frame_rx, proc->nr_slot_rx, cohp, cohb, dl, shift);
            }
            if (cohb > s_enter) {
              // REQUEST an atomic rebase at the next frame boundary; do NOT apply it here and do
              // NOT let this occasion continue. Applying a multi-thousand-sample shift through
              // shiftForNextFrame moves the timeline under in-flight frame/slot/symbol state and
              // aborts on AssertFatal(dmrss<3) -- measured. delta is positive when the SSB is LATER
              // than predicted, and syncInFrame discards samples to advance the stream, so the
              // rebase amount is +delta.
              extern _Atomic long nr_ue_pending_rebase_delta;
              extern _Atomic int nr_ue_pending_rebase_valid;
              if (!atomic_load_explicit(&nr_ue_pending_rebase_valid, memory_order_relaxed)) {
                atomic_store_explicit(&nr_ue_pending_rebase_delta, dl, memory_order_relaxed);
                atomic_store_explicit(&nr_ue_pending_rebase_valid, 1, memory_order_relaxed);
                LOG_W(PHY, "SENSING: ANCHOR requesting deferred rebase of %+ld samples (coh %.4f)\n", dl, cohb);
              }
              (void)shift;
              // Abandoning the occasion must also RESET its state machine. Returning early here
              // skips the `*pbchSymbCnt = 0; *ssbIndex = -1;` reset at the end of this block, which
              // leaves the counter at 3; the next PBCH symbol then pushes it to 4 and dmrss goes out
              // of range -> AssertFatal(dmrss < 3). Measured: this, not the rebase itself, was the
              // second abort.
              *pbchSymbCnt = 0;
              *ssbIndex = -1;
              return INT_MAX; /* no incremental shift; occasion abandoned cleanly */
            }
          }
        }
      }

      if (UE->no_timing_correction == 0 && timing_allowed) {
        // DEADLOCK-BREAK EVIDENCE (2026-08-06): max_pos_acc across the FIRST successful tracking
        // PBCH decodes. If the CFO seed (ISAC_CFO_DRIFT_SEED, executables/nr-ue.c) merely masked
        // the problem, this never runs. If it genuinely bootstrapped the loop, max_pos_acc starts
        // moving AWAY from its seeded value under real PBCH-DMRS measurements -- that migration is
        // the success signal, not the decode alone.
        const int mpa_before = UE->max_pos_acc;
        sampleShift = nr_adjust_synch_ue(&UE->frame_parms, UE, pbch_ch_est_time, proc->frame_rx, proc->nr_slot_rx, 16384);
        s_last_cir_resid = sampleShift;
        {
          static int s_track_log_left = 25;
          if (s_track_log_left > 0 && pbchSuccess == 0) {
            s_track_log_left--;
            LOG_W(PHY,
                  "SENSING: TRACKLOCK frame=%d slot=%d ssb=%d pbch_crc_ok=1 max_pos_acc %d -> %d (delta %+d) "
                  "sampleShift=%d\n",
                  proc->frame_rx, proc->nr_slot_rx, *ssbIndex, mpa_before, UE->max_pos_acc,
                  UE->max_pos_acc - mpa_before, sampleShift);
          }
        }
      }

      // Continuous FO estimation and compensation
      if (get_nrUE_params()->cont_fo_comp && pbchSuccess == 0) {
        double freq_offset = nr_ue_pbch_freq_offset(&UE->frame_parms, pbch_ch_est_sym1, pbch_ch_est_sym3);
        LOG_D(PHY,
              "compensated freq offset = %.3f Hz, detected residual freq offset = %.3f Hz, accumulated freq offset = %.3f Hz\n",
              UE->freq_offset,
              freq_offset,
              UE->freq_off_acc);

        // PI controller
        const double PID_P = get_nrUE_params()->freq_sync_P;
        const double PID_I = get_nrUE_params()->freq_sync_I;
        UE->freq_offset += freq_offset * PID_P + UE->freq_off_acc * PID_I;
        UE->freq_off_acc += freq_offset;
      }
    }
    *pbchSymbCnt = 0; // For next SSB index
    *ssbIndex = -1;
  }
  return sampleShift;
}

static nr_meas_task_args_t *create_meas_task_args(const UE_nr_rxtx_proc_t *proc, PHY_VARS_NR_UE *ue)
{
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  // Extra headroom so that nr_slot_fep() can read all NR_N_SYMBOLS_SSB symbols
  // when the PSS is detected at the very end of the samples_per_slot_wCP search window
  uint32_t rxdata_size = fp->samples_per_slot_wCP
                       + NR_N_SYMBOLS_SSB * (fp->ofdm_symbol_size + fp->nb_prefix_samples);
  size_t total_size = sizeof(nr_meas_task_args_t) + fp->nb_antennas_rx * rxdata_size * sizeof(c16_t);
  nr_meas_task_args_t *args = malloc_or_fail(total_size);
  args->proc = *proc;
  args->ue = ue;
  args->rxdata_size = rxdata_size;
  args->nb_ant = fp->nb_antennas_rx;
  uint32_t slot_offset = get_samples_slot_timestamp(fp, proc->nr_slot_rx);
  for (int i = 0; i < fp->nb_antennas_rx; i++)
    memcpy(args->rxdata_ant + i * rxdata_size, &ue->common_vars.rxdata[i][slot_offset], rxdata_size * sizeof(c16_t));
  return args;
}

int pbch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data)
{
  TracyCZone(ctx, true);
  int frame_rx = proc->frame_rx;
  int nr_slot_rx = proc->nr_slot_rx;
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  int sampleShift = INT_MAX;
  nr_ue_dlsch_init(phy_data->dlsch, NR_MAX_NB_LAYERS > 4 ? 2 : 1, ue->max_ldpc_iterations);

#if T_TRACER
  T(T_UE_PHY_DL_TICK, T_INT(ue->Mod_id), T_INT(frame_rx % 1024), T_INT(nr_slot_rx));
#endif

  LOG_D(PHY," ****** start RX-Chain for Frame.Slot %d.%d ******  \n",
        frame_rx%1024, nr_slot_rx);

  // ---- PBCH TRACKING IN PASSIVE MODE: RE-ENABLED BY DEFAULT (2026-08-20) ----------------------
  // This used to latch the acquisition MIB and SKIP periodic PBCH tracking whenever
  // IS_PASSIVE_RX_MODE, on the reasoning that a sensing receiver does not need the MIB re-decoded
  // every 20 ms and that "timing therefore stays with the acquisition/PDCCH/CSI-RS path, which is
  // the path that demonstrably works here."
  //
  // That reasoning was WRONG, and measurably so. On a terrestrial cell nr_adjust_synch_ue() is the
  // ONLY thing that advances the receiver's timing loop (max_pos_acc is otherwise seeded just from
  // ntn_init_time_drift, i.e. 0), and it is gated on a successful PBCH TRACKING decode. Skipping
  // tracking therefore does not merely "leave timing to another path" -- it leaves the FFT window
  // uncorrected after initial sync, free-running against the gNB's clock. Nothing else closes it.
  //
  // MEASURED 2026-08-20, X410 / live srsRAN 273 PRB cell, blind PDCCH monitor, identical binary and
  // config, only this flag differing:
  //     tracking skipped (old default) : accepts = 0
  //     tracking enabled               : accepts = 15089, ALL of them the live C-RNTI 0x4604,
  //                                      no other value in the histogram at all
  // ~137 recovered DCIs/s against the ~170 DL grants/s the gNB's own log shows for that UE.
  //
  // The old justification ("tracking PBCH fails on nearly every occasion at 273 PRB") was itself
  // measured on a tree that could not decode SIB1 even as an attached UE -- a branch-level
  // regression fixed by merging x410-100MHz. With a working receive chain, tracking succeeds.
  //
  // Escape hatch kept for A/B: ISAC_PBCH_NO_TRACK=1 restores the old cached-MIB behaviour.
  static int s_pbch_no_track = -1;
  if (s_pbch_no_track < 0)
    s_pbch_no_track = (getenv("ISAC_PBCH_NO_TRACK") && atoi(getenv("ISAC_PBCH_NO_TRACK"))) ? 1 : 0;
  const bool use_cached_mib =
      s_pbch_no_track && IS_PASSIVE_RX_MODE(get_softmodem_params()) && ue->is_synchronized;
  if (use_cached_mib) {
    static int s_announced = 0;
    if (!s_announced) {
      s_announced = 1;
      LOG_I(PHY,
            "SENSING: PBCH tracking DISABLED (cached MIB from acquisition, Nid_cell=%d ssb_index=%d). "
            "Timing/CFO stay with the acquisition/PDCCH/CSI-RS path. Set ISAC_PBCH_TRACK=1 to re-enable.\n",
            fp->Nid_cell, fp->ssb_index);
    }
  }

  if (!use_cached_mib) {
    int pbchSymbCnt = 0;
    __attribute__((aligned(32))) c16_t pbch_ch_est_time[ue->frame_parms.nb_antennas_rx][ue->frame_parms.ofdm_symbol_size];
    int16_t pbch_e_rx[NR_POLAR_PBCH_E];
    // Buffer to hold estimates of symbol 1 for FO compensation in symbol 3
    c16_t pbch_ch_est_sym1[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
    // Channel-compensation shift, shared across this SSB's three PBCH symbols so their LLRs stay on
    // a common scale for the single polar codeword -- see nr_generate_pbch_llr().
    double pbch_log2_maxh = -1.0;

    // TEMPORARY DIAGNOSTIC (acquisition->tracking handoff root-cause hunt, 2026-08-02):
    // per-symbol energy across the SSB slot, computed with the SAME window arithmetic nr_slot_fep
    // uses, so we can see which symbols the SSB burst actually occupies after the handoff.
    // Expected: burst on symbols 2..6 (PSS/PBCH/SSS/PBCH for ssbIndex 0, Case C band n78).
    if (nr_slot_rx == 0) {
      static int sweeps = 0;
      static int s_sweep_diag = -1;
      if (s_sweep_diag < 0)
        s_sweep_diag = (getenv("ISAC_PHY_DIAG") && atoi(getenv("ISAC_PHY_DIAG"))) ? 1 : 0;
      if (s_sweep_diag && sweeps < 8) {
        sweeps++;
        char buf[768];
        int p = 0;
        int off = get_samples_slot_timestamp(fp, nr_slot_rx);
        for (int s = 0; s < fp->symbols_per_slot; s++) {
          const int abs_s = nr_slot_rx * fp->symbols_per_slot + s;
          off += (abs_s % (0x7 << fp->numerology_index)) ? fp->nb_prefix_samples : fp->nb_prefix_samples0;
          const int e = dB_fixed(signal_energy((int32_t *)&ue->common_vars.rxdata[0][off], fp->ofdm_symbol_size));
          p += snprintf(buf + p, sizeof(buf) - p, " s%d=%d", s, e);
          off += fp->ofdm_symbol_size;
        }
        LOG_I(PHY, "SSBSWEEP frame=%d slot=%d energy_dB:%s\n", frame_rx, nr_slot_rx, buf);
      }
    }

    int ssbIndex = -1;
    uint8_t log2_maxh = 0;
    // TODO: Remove loopover symbols when symbol based receiver is fully integrated.
    for (int symbol = 0; symbol < fp->symbols_per_slot; symbol++) {
      const int pbch_sampleShift =
          pbch_process(ue, proc, symbol, &ssbIndex, pbch_ch_est_sym1, pbch_ch_est_time, pbch_e_rx, &pbchSymbCnt,
                       &pbch_log2_maxh);
      // To prevent overwrite estimated shift by consecutive symbol calls
      sampleShift = (sampleShift == INT_MAX) ? pbch_sampleShift : sampleShift;
    }
  }

  // Check for PRS slot - section 7.4.1.7.4 in 3GPP rel16 38.211
  for(int gNB_id = 0; gNB_id < ue->prs_active_gNBs; gNB_id++)
  {
    __attribute__((aligned(32))) c16_t rxdataF[ue->frame_parms.nb_antennas_rx][ue->frame_parms.samples_per_slot_wCP];
    for(int rsc_id = 0; rsc_id < ue->prs_vars[gNB_id]->NumPRSResources; rsc_id++)
    {
      prs_config_t *prs_config = &ue->prs_vars[gNB_id]->prs_resource[rsc_id].prs_cfg;
      for (int i = 0; i < prs_config->PRSResourceRepetition; i++)
      {
        if( (((frame_rx*fp->slots_per_frame + nr_slot_rx) - (prs_config->PRSResourceSetPeriod[1] + prs_config->PRSResourceOffset) + prs_config->PRSResourceSetPeriod[0])%prs_config->PRSResourceSetPeriod[0]) == i*prs_config->PRSResourceTimeGap)
        {
          for(int j = prs_config->SymbolStart; j < (prs_config->SymbolStart+prs_config->NumPRSSymbols); j++)
          {
            nr_slot_fep(ue, fp, proc->nr_slot_rx, (j % fp->symbols_per_slot), rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
          }
          nr_prs_channel_estimation(gNB_id, rsc_id, i, ue, proc, fp, rxdataF);
        }
      } // for i
    } // for rsc_id
  } // for gNB_id

  PHY_NR_MEASUREMENTS *measurements = &ue->measurements;

  // Measurements on known neighboring cells
  if (check_neighboring_cells_task(ue, measurements->meas_request_pending)) {
    measurements->meas_request_pending = true;
    nr_meas_task_args_t *args = create_meas_task_args(proc, ue);
    task_t t = {.func = nr_ue_meas_neighboring_cell, .args = args};
    pushTpool(&get_nrUE_params()->Tpool, t);
  }

  // Search for unknown neighboring cells
  if (!ue->disable_blind_search && proc->frame_rx % 256 == 0 && proc->nr_slot_rx == 0
      && measurements->last_blind_slot == measurements->last_slot)
    measurements->last_blind_slot = -1;
  uint16_t slots_per_frame = ue->frame_parms.slots_per_frame;
  bool is_meas_slot = proc->frame_rx % 256 == 0 && proc->nr_slot_rx >= CIRCULAR_INC(measurements->last_blind_slot, 1, slots_per_frame);
  if (!ue->disable_blind_search && is_meas_slot && check_neighboring_cells_task(ue, measurements->search_new_cells_pending)) {
    measurements->search_new_cells_pending = true;
    measurements->last_blind_slot = proc->nr_slot_rx;
    nr_meas_task_args_t *args = create_meas_task_args(proc, ue);
    task_t t = {.func = nr_ue_search_new_neighboring_cell, .args = args};
    pushTpool(&get_nrUE_params()->Tpool, t);
  }
  measurements->last_slot = proc->nr_slot_rx;

  TracyCZoneEnd(ctx);
  return sampleShift;
}

// ISAC sensing tap (PDSCH data-aided source): after a CRC-verified DL-SCH decode, re-encode the
// CONFIRMED-CORRECT payload through the real chain (LDPC encode + rate match + scramble + modulate,
// reusing the exact primitives the UE's own PUSCH TX path uses -- see nr_ulsch_encoding() for the
// template this mirrors) to reconstruct the transmitted symbol X at EVERY data RE, then submit
// Ĥ[k] = Y[k]/X[k] to the sensing engine. Unlike the old pdsch_data behaviour (removed above -- it
// just resampled the DM-RS-INTERPOLATED estimate at comb-1, inheriting a "comb-106" per-PRB
// interpolator ripple, see tests/sensing_sim/README.md), this is genuine data-aided reconstruction:
// no interpolation at all, a fresh Ĥ computed directly from the raw received samples at every RE.
//
// Scope (falls back to no tap, silently, outside these -- the common do-ra/rfsim-test conditions;
// generalizing is future work, not a correctness risk since we simply contribute nothing then):
//   - decode must have succeeded (harq->decodeResult, i.e. CRC passed) -- we ONLY ever re-encode a
//     confirmed-correct payload, never a guess.
//   - single layer (Nl==1), no PTRS (pduBitmap bit 0), no CSI-RS rate-matching overlap -- the RE
//     enumeration accounts for DM-RS (including data REs sharing a DM-RS symbol, via
//     nr_dlsch_extract_rbs()'s own bitmaps) but not for punctured PTRS or CSI-RS-rate-matched REs.
//
// The scope guards live here; the reconstruction itself moved to
// PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c (2026-07-30) so the PASSIVE receiver, which decodes an
// overheard grant instead of its own, reaches the SAME chain rather than a second copy of it.
static void nr_isac_pdsch_data_aided_tap(PHY_VARS_NR_UE *ue,
                                         const UE_nr_rxtx_proc_t *proc,
                                         const NR_UE_DLSCH_t *dlsch,
                                         const NR_DL_UE_HARQ_t *harq,
                                         const uint8_t *decoded_tb,
                                         const fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config,
                                         const freq_alloc_bitmap_t *freq_alloc,
                                         const c16_t rxdataF[][ue->frame_parms.samples_per_slot_wCP],
                                         double nvar)
{
  if (!nr_isac_enabled() || !nr_isac_source_enabled(NR_ISAC_SRC_PDSCH_DATA))
    return;
  if (decoded_tb == NULL)
    return; // caller did not publish a transport block (decode path did not run)
  if (!harq->decodeResult)
    return; // only re-encode a CRC-verified transport block
  if (dlsch->cw_info.Nl != 1)
    return; // single-layer only (scope)
  if (dlsch_config->pduBitmap & 0x1)
    return; // PTRS present: RE enumeration doesn't account for punctured REs (scope)
  if (dlsch_config->numCsiRsForRateMatching > 0)
    return; // CSI-RS rate-matching overlap: not accounted for (scope)

  // Offset well clear of real harq_pid / 2*harq_pid+cw_idx ranges used by concurrent PDSCH decode
  // and PUSCH encode on this same nrLDPC_coding_interface, to avoid any id collision.
  nr_isac_pdsch_data_aided_submit(ue, proc, &dlsch->cw_info, dlsch_config, freq_alloc, dlsch->rnti, (uint8_t *)decoded_tb,
                                  1000 + dlsch_config->harq_process_nbr, rxdataF, nvar);
}

void pdsch_processing(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, nr_phy_data_t *phy_data)
{
  int frame_rx = proc->frame_rx;
  int nr_slot_rx = proc->nr_slot_rx;
  int gNB_id = proc->gNB_id;

  // do procedures for C-RNTI

  bool slot_fep_map[14] = {0};
  const uint32_t rxdataF_sz = ue->frame_parms.samples_per_slot_wCP;
  __attribute__ ((aligned(32))) c16_t rxdataF[ue->frame_parms.nb_antennas_rx][rxdataF_sz];

  // do procedures for CSI-IM
  if (phy_data->csiim_vars.active == 1) {
    for(int symb_idx = 0; symb_idx < 4; symb_idx++) {
      int symb = phy_data->csiim_vars.csiim_config_pdu.l_csiim[symb_idx];
      if (!slot_fep_map[symb]) {
        nr_slot_fep(ue, &ue->frame_parms, proc->nr_slot_rx, symb, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
        slot_fep_map[symb] = true;
      }
    }
    nr_ue_csi_im_procedures(ue, rxdataF, &phy_data->csiim_vars.csiim_config_pdu);
  }

  // do procedures for CSI-RS
  {
    /*
    CSI-RS for tracking use only one port.
    Number of CSI-RS resources for tracking is always 2 per slot.
    Computed estimates from first resource is saved and used while estimating second resource.
    */
    c16_t trs_estimates[ue->frame_parms.nb_antennas_rx][1][ue->frame_parms.ofdm_symbol_size];
    for (int res = 0; res < MAX_CSI_RES_SLOT; res++) {
      if (phy_data->csirs_vars[res].active == 1) {
        for (int symb = 0; symb < ue->frame_parms.symbols_per_slot; symb++) {
          if (is_csi_rs_in_symbol(phy_data->csirs_vars[res].csirs_config_pdu, symb)) {
            if (!slot_fep_map[symb]) {
              nr_slot_fep(ue, &ue->frame_parms, proc->nr_slot_rx, symb, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
              slot_fep_map[symb] = true;
            }
          }
        }
        if (res > 0 && phy_data->csirs_vars[res].csirs_config_pdu.csi_type == 0) // tracking CSI
          AssertFatal(phy_data->csirs_vars[res - 1].active && (phy_data->csirs_vars[res - 1].csirs_config_pdu.csi_type == 0),
                      "CSI-RS for tracking must have two consecutive active resources\n");
        nr_ue_csi_rs_procedures(ue,
                                proc,
                                rxdataF,
                                &phy_data->csirs_vars[res].csirs_config_pdu,
                                trs_estimates,
                                res,
                                (res == 1) ? phy_data->csirs_vars[0].csirs_config_pdu.symb_l0 : -1);
      }
    }
  }

  // UE-agnostic CSI-RS sensing monitors: cell-common / other-UE CSI-RS resources from the
  // [sensing] csirs_monitor list, processed for passive sensing independent of our own
  // RRC CSI-MeasConfig. Each due resource is FEP'd and run through the lean sensing-only
  // capture (no CSI report to MAC). See nr_csirs_monitor.{h,c}.
  /* CSI-RS monitoring is a RECEIVE function, not a sensing one: it recovers the cell's own
   * reference signals and yields a channel estimate with no grant and no attachment. It was gated
   * on nr_isac_enabled() only because it was originally written to feed the sensing engine, so on a
   * receiver-only build (ISAC stubbed) CSI-RS silently stopped being received at all. Gate it on
   * its own config instead; the SUBMISSION to sensing stays gated on ISAC inside the capture. */
  if (nr_csirs_monitor_enabled()) {
    const fapi_nr_dl_config_csirs_pdu_rel15_t *mon[NR_CSIRS_MONITOR_MAX];
    const int nmon = nr_csirs_monitor_due(proc->frame_rx, proc->nr_slot_rx, ue->frame_parms.slots_per_frame, mon,
                                          NR_CSIRS_MONITOR_MAX);
    for (int i = 0; i < nmon; i++) {
      for (int symb = 0; symb < ue->frame_parms.symbols_per_slot; symb++) {
        if (is_csi_rs_in_symbol(*mon[i], symb) && !slot_fep_map[symb]) {
          nr_slot_fep(ue, &ue->frame_parms, proc->nr_slot_rx, symb, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
          slot_fep_map[symb] = true;
        }
      }
      nr_ue_csi_rs_sensing_capture(ue, proc, rxdataF, mon[i]);
    }
  }

  const int actor_idx_llr = proc->nr_slot_rx % ue->pdsch_num_actors;
  int16_t **llr = ue->pdsch_scratch[actor_idx_llr].llr;
  fapi_nr_dl_config_dlsch_pdu_rel15_t *dlsch_config = &phy_data->dlsch_config;
  for (int c = 0; c < phy_data->n_dlsch_codewords; c++) {
    NR_UE_DLSCH_t *dlsch = &phy_data->dlsch[c];
    if (!dlsch->active)
      continue;

    uint16_t nb_symb_sch = dlsch_config->number_symbols;
    uint16_t start_symb_sch = dlsch_config->start_symbol;

    LOG_D(PHY," ------ --> PDSCH ChannelComp/LLR Frame.slot %d.%d ------  \n", frame_rx % 1024, nr_slot_rx);

    for (int m = start_symb_sch; m < (nb_symb_sch + start_symb_sch) ; m++) {
      if (!slot_fep_map[m]) {
        nr_slot_fep(ue, &ue->frame_parms, proc->nr_slot_rx, m, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
        slot_fep_map[m] = true;
      }
    }

    freq_alloc_bitmap_t freq_alloc = {0};
    if (dlsch_config->resource_alloc == 0) {
      int alloc_size = (dlsch_config->BWPSize / 8) + (dlsch_config->BWPSize % 8 > 0);
      freq_alloc = set_start_end_from_bitmap(dlsch_config->BWPSize, alloc_size, dlsch_config->rb_bitmap);
    } else
      freq_alloc = set_bitmap_from_start_size(dlsch_config->start_rb, dlsch_config->number_rbs);

    const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(dlsch_config->dmrsConfigType, dlsch_config->n_dmrs_cdm_groups);
    uint16_t dmrs_len = get_num_dmrs(dlsch_config->dlDmrsSymbPos);
    uint32_t unav_res = 0;
    if(dlsch_config->pduBitmap & 0x1) {
      uint16_t ptrsSymbPos = 0;
      set_ptrs_symb_idx(&ptrsSymbPos,
                        dlsch_config->number_symbols,
                        dlsch_config->start_symbol,
                        1 << dlsch_config->PTRSTimeDensity,
                        dlsch_config->dlDmrsSymbPos);
      int n_ptrs = (freq_alloc.num_rbs + dlsch_config->PTRSFreqDensity - 1) / dlsch_config->PTRSFreqDensity;
      int ptrsSymbPerSlot = get_ptrs_symbols_in_slot(ptrsSymbPos, dlsch_config->start_symbol, dlsch_config->number_symbols);
      unav_res = n_ptrs * ptrsSymbPerSlot;
    }
    unav_res += nr_ue_csi_rm_unav_res(dlsch_config, &freq_alloc);
    int G = nr_get_G(freq_alloc.num_rbs,
                     dlsch_config->number_symbols,
                     nb_re_dmrs,
                     dmrs_len,
                     unav_res,
                     dlsch->cw_info.qamModOrder,
                     dlsch->cw_info.Nl);
    const uint32_t rx_llr_buf_sz = ALIGNARRAYSIZE(G, 32); // each LLR is 2 bytes hence 64 byte aligned

    // dlsch_harq contains the previous transmissions data for this harq pid
    NR_DL_UE_HARQ_t *harq = &ue->dl_harq_processes[c][dlsch_config->harq_process_nbr];
    // it returns -1 in case of internal failure, or 0 in case of normal result
    uint32_t nvar = 0;
    int ret_pdsch = nr_ue_pdsch_procedures(ue, proc, dlsch, harq, dlsch_config, llr[c], rxdataF, &freq_alloc, &nvar);
    TracyCPlot("pdsch mcs", dlsch->cw_info.mcs);

    UEscopeCopy(ue, pdschLlr, llr[c], sizeof(int16_t), 1, G, 0);

    LOG_D(PHY, "DLSCH data reception at nr_slot_rx: %d\n", nr_slot_rx);
    start_meas_nr_ue_phy(ue, DLSCH_PROCEDURES_STATS);

    if (ret_pdsch >= 0) {
      uint8_t *decoded_tb = NULL;
      nr_ue_dlsch_procedures(ue, proc, dlsch, c, G, &freq_alloc, dlsch_config, llr[c], &decoded_tb);
      // ISAC data-aided PDSCH tap: only meaningful once decode has run (harq->decodeResult is set
      // inside nr_ue_dlsch_procedures -> nr_dlsch_decoding). See nr_isac_pdsch_data_aided_tap()'s own
      // scope-guard comments for when it actually contributes vs. silently no-ops.
      nr_isac_pdsch_data_aided_tap(ue, proc, dlsch, harq, decoded_tb, dlsch_config, &freq_alloc, rxdataF, (double)nvar);
    } else {
      LOG_E(NR_PHY, "Demodulation impossible, internal error\n");
      if (dlsch_config->k1_feedback) {
        const int ack_nack_slot_and_frame =
            proc->nr_slot_rx + dlsch_config->k1_feedback + proc->frame_rx * ue->frame_parms.slots_per_frame;
        dynamic_barrier_join(&ue->process_slot_tx_barriers[ack_nack_slot_and_frame % NUM_PROCESS_SLOT_TX_BARRIERS]);
      }
      LOG_W(NR_PHY, "nr_ue_pdsch_procedures failed in slot %d\n", proc->nr_slot_rx);
    }

    stop_meas_nr_ue_phy(ue, DLSCH_PROCEDURES_STATS);
    if (cpumeas(CPUMEAS_GETSTATE)) {
      LOG_D(PHY, "[SFN %d] Slot0 Slot1: Dlsch Proc %5.2f\n",nr_slot_rx,ue->phy_cpu_stats.cpu_time_stats[DLSCH_PROCEDURES_STATS].p_time/(cpuf*1000.0));
    }

    if (ue->phy_sim_rxdataF)
      memcpy(ue->phy_sim_rxdataF + start_symb_sch * ue->frame_parms.ofdm_symbol_size * sizeof(c16_t),
             &rxdataF[0][start_symb_sch * ue->frame_parms.ofdm_symbol_size],
             sizeof(int32_t) * nb_symb_sch * ue->frame_parms.ofdm_symbol_size);
    if (ue->phy_sim_pdsch_llr)
      memcpy(ue->phy_sim_pdsch_llr, llr[c], sizeof(int16_t) * rx_llr_buf_sz);

  }

  if (nr_slot_rx==9) {
    if (frame_rx % 10 == 0) {
      if ((ue->dlsch_received[gNB_id] - ue->dlsch_received_last[gNB_id]) != 0)
        ue->dlsch_fer[gNB_id] = (100*(ue->dlsch_errors[gNB_id] - ue->dlsch_errors_last[gNB_id]))/(ue->dlsch_received[gNB_id] - ue->dlsch_received_last[gNB_id]);

      ue->dlsch_errors_last[gNB_id] = ue->dlsch_errors[gNB_id];
      ue->dlsch_received_last[gNB_id] = ue->dlsch_received[gNB_id];
    }


    ue->bitrate[gNB_id] = (ue->total_TBS[gNB_id] - ue->total_TBS_last[gNB_id])*100;
    ue->total_TBS_last[gNB_id] = ue->total_TBS[gNB_id];
    LOG_D(PHY,"[UE %d] Calculating bitrate Frame %d: total_TBS = %d, total_TBS_last = %d, bitrate %f kbits\n",
          ue->Mod_id,frame_rx,ue->total_TBS[gNB_id],
          ue->total_TBS_last[gNB_id],(float) ue->bitrate[gNB_id]/1000.0);
  }

  LOG_D(PHY," ****** end RX-Chain  for AbsSubframe %d.%d ******  \n", frame_rx%1024, nr_slot_rx);
  UEscopeCopy(ue, commonRxdataF, rxdataF, sizeof(int32_t), ue->frame_parms.nb_antennas_rx, rxdataF_sz, 0);
}


// todo:
// - power control as per 38.213 ch 7.4
static void nr_ue_prach_procedures(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc, c16_t **txData)
{
  int gNB_id = proc->gNB_id;
  int frame_tx = proc->frame_tx, nr_slot_tx = proc->nr_slot_tx, prach_power; // tx_amp
  uint8_t mod_id = ue->Mod_id;

  NR_UE_PRACH *prach_var = ue->prach_vars[gNB_id];
  if (prach_var->active) {
    fapi_nr_ul_config_prach_pdu *prach_pdu = &prach_var->prach_pdu;
    // Generate PRACH in first slot. For L839, the following slots are also filled in this slot.
    if (prach_pdu->prach_slot == nr_slot_tx) {
      ue->tx_power_dBm[nr_slot_tx] = prach_pdu->prach_tx_power;

      LOG_D(PHY,
            "In %s: [UE %d][RAPROC][%d.%d]: Generating PRACH Msg1 (preamble %d, P0_PRACH %d)\n",
            __FUNCTION__,
            mod_id,
            frame_tx,
            nr_slot_tx,
            prach_pdu->ra_PreambleIndex,
            ue->tx_power_dBm[nr_slot_tx]);

      prach_var->amp = AMP;

      start_meas_nr_ue_phy(ue, PRACH_GEN_STATS);
      prach_power = generate_nr_prach(ue, gNB_id, frame_tx, nr_slot_tx, txData);
      stop_meas_nr_ue_phy(ue, PRACH_GEN_STATS);
      if (cpumeas(CPUMEAS_GETSTATE)) {
        LOG_D(PHY,
              "[SFN %d.%d] PRACH Proc %5.2f\n",
              proc->frame_tx,
              proc->nr_slot_tx,
              ue->phy_cpu_stats.cpu_time_stats[PRACH_GEN_STATS].p_time / (cpuf * 1000.0));
      }

      LOG_D(PHY,
            "In %s: [UE %d][RAPROC][%d.%d]: Generated PRACH Msg1 (TX power PRACH %d dBm, digital power %d dBW (amp %d)\n",
            __FUNCTION__,
            mod_id,
            frame_tx,
            nr_slot_tx,
            ue->tx_power_dBm[nr_slot_tx],
            dB_fixed(prach_power),
            ue->prach_vars[gNB_id]->amp);

      // set duration of prach slots so we know when to skip OFDM modulation
      const int prach_format = ue->prach_vars[gNB_id]->prach_pdu.prach_format;
      const int prach_slots = (prach_format < 4) ? get_long_prach_dur(prach_format, ue->frame_parms.numerology_index) : 1;
      prach_var->num_prach_slots = prach_slots;
    }

    // set as inactive in the last slot
    prach_var->active = !(nr_slot_tx == (prach_pdu->prach_slot + prach_var->num_prach_slots - 1));
  }
}
