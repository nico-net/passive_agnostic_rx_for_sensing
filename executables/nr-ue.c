#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
#include "nr_rx_continuity.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_acq_state.h" // acquisition-state tracker: hard sync-loss edge
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_sources.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_metrics.h" // ISAC_METRICS pci
#include <dlfcn.h>
/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PHY/defs_nr_common.h"
#define _GNU_SOURCE // For pthread_setname_np
#include <pthread.h>
#include "executables/nr-ue-ru.h"
#include "executables/nr-uesoftmodem.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/INIT/nr_phy_init.h"
#include "NR_MAC_UE/mac_proto.h"
#include "RRC/NR_UE/rrc_proto.h"
#include "RRC/NR_UE/L2_interface_ue.h"
#include "SCHED_NR_UE/defs.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h"
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.h" // passive UL PUSCH hook
#include "executables/softmodem-common.h"
#include "radio/COMMON/common_lib.h"
#include "LAYER2/nr_pdcp/nr_pdcp_oai_api.h"
#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "openair1/PHY/TOOLS/phy_scope_interface.h"
#include "instrumentation.h"
#include "common/utils/threadPool/notified_fifo.h"
#include "position_interface.h"
#include "nr_phy_common.h"
#include "common/utils/time_manager/time_manager.h"
#include "log.h"
#include <stdatomic.h>

/// Defined in nr_adjust_synch_ue.c -- freezes the timing integrator during a stream outage.
extern _Atomic int nr_ue_rf_signal_absent;

// TEMPORARY DIAGNOSTIC (2026-08-05): receive-buffer producer/consumer lag measurement. See
// PBCH_TRACKING_BUFFER_HANDOVER.md -- PHY-domain diagnostics have exhausted what they can show
// (acquisition decodes cleanly, tracking's entire live rxdata buffer contains no SSB anywhere in
// it) and the remaining candidate is a stale-buffer read: PBCH tracking runs on an async worker
// (UE->dl_actors, num_dl_actors=4 by default) that consumes ue->common_vars.rxdata BY REFERENCE
// (only .proc metadata is copied into the queued message, not the samples) while THIS read loop
// continues overwriting that same physical buffer on every subsequent slot with no backpressure.
// Updated by the producer (this file's main read loop) immediately after each nrue_ru_read()
// completes; read by the consumer (nr_process_pbch_symbol() in phy_procedures_nr_ue.c) to compute
// the actual lag in slots/wall-clock time between "when this buffer region was last written" and
// "when tracking is now reading it". Genuinely cross-thread (producer and the dl_actors workers are
// different threads), hence _Atomic rather than __thread; relaxed ordering is sufficient for a
// diagnostic counter that isn't gating any correctness-relevant control flow.
_Atomic long nr_ue_diag_producer_absolute_slot = -1;
_Atomic long nr_ue_diag_producer_wall_ns = 0;

/*
 *  NR SLOT PROCESSING SEQUENCE
 *
 *  Processing occurs with following steps for connected mode:
 *
 *  - Rx samples for a slot are received,
 *  - PDCCH processing (including DCI extraction for downlink and uplink),
 *  - PDSCH processing (including transport blocks decoding),
 *  - PUCCH/PUSCH (transmission of acknowledgements, CSI, ... or data).
 *
 *  Time between reception of the slot and related transmission depends on UE processing performance.
 *  It is defined by the value NR_UE_CAPABILITY_SLOT_RX_TO_TX.
 *
 *  In NR, network gives the duration between Rx slot and Tx slot in the DCI:
 *  - for reception of a PDSCH and its associated acknowledgment slot (with a PUCCH or a PUSCH),
 *  - for reception of an uplink grant and its associated PUSCH slot.
 *
 *  So duration between reception and it associated transmission depends on its transmission slot given in the DCI.
 *  NR_UE_CAPABILITY_SLOT_RX_TO_TX means the minimum duration but higher duration can be given by the network because UE can support it.
 *
 *                                                                                                    Slot k
 *                                                                                  -------+------------+--------
 *                Frame                                                                    | Tx samples |
 *                Subframe                                                                 |   buffer   |
 *                Slot n                                                            -------+------------+--------
 *       ------ +------------+--------                                                     |
 *              | Rx samples |                                                             |
 *              |   buffer   |                                                             |
 *       -------+------------+--------                                                     |
 *                           |                                                             |
 *                           V                                                             |
 *                           +------------+                                                |
 *                           |   PDCCH    |                                                |
 *                           | processing |                                                |
 *                           +------------+                                                |
 *                           |            |                                                |
 *                           |            v                                                |
 *                           |            +------------+                                   |
 *                           |            |   PDSCH    |                                   |
 *                           |            | processing | decoding result                   |
 *                           |            +------------+    -> ACK/NACK of PDSCH           |
 *                           |                         |                                   |
 *                           |                         v                                   |
 *                           |                         +-------------+------------+        |
 *                           |                         | PUCCH/PUSCH | Tx samples |        |
 *                           |                         |  processing | transfer   |        |
 *                           |                         +-------------+------------+        |
 *                           |                                                             |
 *                           |/___________________________________________________________\|
 *                            \  duration between reception and associated transmission   /
 *
 * Remark: processing is done slot by slot, it can be distribute on different threads which are executed in parallel.
 * This is an architecture optimization in order to cope with real time constraints.
 * By example, for LTE, subframe processing is spread over 4 different threads.
 *
 */

static void start_process_slot_tx(void* arg) {
  notifiedFIFO_elt_t *newTx = arg;
  nr_rxtx_thread_data_t *curMsgTx = NotifiedFifoData(newTx);
  int num_ul_actors = get_nrUE_params()->num_ul_actors;
  if (num_ul_actors > 0) {
    pushNotifiedFIFO(&curMsgTx->UE->ul_actors[curMsgTx->proc.nr_slot_tx % num_ul_actors].fifo, newTx);
  } else {
    newTx->processingFunc(curMsgTx);
  }
}

static size_t dump_L1_UE_meas_stats(PHY_VARS_NR_UE *ue, char *output, size_t max_len)
{
  const char *begin = output;
  const char *end = output + max_len;
  for (int i = 0; i < MAX_CPU_STAT_TYPE; i++) {
    output += print_meas_log(&ue->phy_cpu_stats.cpu_time_stats[i],
                             ue->phy_cpu_stats.cpu_time_stats[i].meas_name,
                             NULL,
                             NULL,
                             output,
                             end - output);
  }
  return output - begin;
}

static void *nrL1_UE_stats_thread(void *param)
{
  PHY_VARS_NR_UE *ue = (PHY_VARS_NR_UE *) param;
  const int max_len = 16384;
  char output[max_len];
  char filename[30];
  snprintf(filename, 29, "nrL1_UE_stats-%d.log", ue->Mod_id);
  filename[29] = 0;
  FILE *fd = fopen(filename, "w");
  AssertFatal(fd != NULL, "Cannot open %s\n", filename);

  while (!oai_exit) {
    sleep(1);
    const int len = dump_L1_UE_meas_stats(ue, output, max_len);
    AssertFatal(len < max_len, "exceeded length\n");
    fwrite(output, len + 1, 1, fd); // + 1 for terminating NULL byte
    fflush(fd);
    fseek(fd, 0, SEEK_SET);
  }
  fclose(fd);

  return NULL;
}

static int determine_N_TA_offset(PHY_VARS_NR_UE *ue) {
  if (ue->sl_mode == 2)
    return 0;
  else {
    int N_TA_offset = ue->nrUE_config.cell_config.N_TA_offset;
    if (N_TA_offset == -1) {
      return set_default_nta_offset(ue->frame_parms.freq_range, ue->frame_parms.samples_per_subframe);
    } else {
      // Return N_TA_offet in samples, as described in 38.211 4.1 and 4.3.1
      // T_c[s] =  1/(Δf_max x N_f) = 1 / (480 * 1000 * 4096)
      // N_TA_offset[s] = N_TA_offset x T_c
      // N_TA_offset[samples] = samples_per_second x N_TA_offset[s]
      // N_TA_offset[samples] = N_TA_offset x samples_per_subframe x 1000 x T_c
      return (N_TA_offset * ue->frame_parms.samples_per_subframe) / (4096 * 480);
    }
  }
}

void init_nr_ue_vars(PHY_VARS_NR_UE *ue, uint8_t UE_id)
{
  int nb_connected_gNB = 1;

  ue->Mod_id      = UE_id;
  ue->if_inst     = nr_ue_if_module_init(UE_id);
  ue->dci_thres   = 0;
  ue->target_Nid_cell = -1;
  /* ISAC_TARGET_PCI=<pci>: pin the initial SSB search to one N_ID_2 (PCI%3), a hard filter in
   * pss_nr.c (pss_index_start=pss_index_end-1=GET_NID2(target)) -- correlates against exactly
   * that one PSS sequence, never even evaluates the other two. Existing target_Nid_cell plumbing
   * (nr_initial_sync.c/nr_ue_measurements.c) was previously only reachable via a NAS-triggered
   * re-sync request; this is the FIRST wiring for the initial blind acquisition itself. Unset =
   * -1 = unchanged blind-scan behaviour, bit-identical to before. Does not disambiguate two
   * cells sharing the same N_ID_2 (SSS/N_ID_1 is still whichever correlates -- see
   * wideband-tracking-uses-wrong-cell-dmrs memory); only rules out N_ID_2-distinct co-channel
   * cells entirely, which is what it is being used for here. */
  {
    const char *e = getenv("ISAC_TARGET_PCI");
    if (e && *e) {
      ue->target_Nid_cell = atoi(e);
      LOG_W(NR_PHY, "ISAC_TARGET_PCI=%d: pinning initial acquisition to N_ID_2=%d\n",
            ue->target_Nid_cell, ue->target_Nid_cell % 3);
    }
  }

  // initialize all signal buffers
  init_nr_ue_signal(ue, nb_connected_gNB);

  // intialize transport
  init_nr_ue_transport(ue);

  // Initialization of measurement variables
  init_phy_nr_measurements(ue);

  ue->ta_frame = -1;
  ue->ta_slot = -1;
}

/*!
 * It performs band scanning and synchonization.
 * \param arg is a pointer to a \ref PHY_VARS_NR_UE structure.
 */

typedef struct {
  PHY_VARS_NR_UE *UE;
  UE_nr_rxtx_proc_t proc;
  nr_gscn_info_t gscnInfo[MAX_GSCN_BAND];
  int numGscn;
  int rx_offset;
  openair0_timestamp_t capture_end;
} syncData_t;

extern _Atomic int nr_ue_cfo_resync_request; // set by the CFO trim loop (phy_procedures_nr_ue.c)
extern int         nr_ue_cfo_resync_hz;

static void UE_synch(void *arg) {
  syncData_t *syncD = (syncData_t *)arg;
  PHY_VARS_NR_UE *UE = syncD->UE;
  UE->is_synchronized = 0;

  if (UE->target_Nid_cell != -1) {
    LOG_W(NR_PHY, "Starting re-sync detection for target Nid_cell %i\n", UE->target_Nid_cell);
  } else {
    LOG_W(NR_PHY, "Starting sync detection\n");
  }

  LOG_I(PHY, "[UE thread Synch] Running Initial Synch \n");

  uint64_t dl_carrier, ul_carrier;
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  nr_initial_sync_t ret = {false, 0, 0};
  if (UE->sl_mode == 2) {
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;
    dl_carrier = fp->sl_CarrierFreq;
    ul_carrier = fp->sl_CarrierFreq;
    ret = sl_nr_slss_search(UE, &syncD->proc, SL_NR_SSB_REPETITION_IN_FRAMES);
  } else {
    nr_get_carrier_frequencies(UE, &dl_carrier, &ul_carrier);
    ret = nr_initial_sync(&syncD->proc, UE, 2, syncD->gscnInfo, syncD->numGscn);
  }

  if (ret.cell_detected) {
    syncD->rx_offset = ret.rx_offset;
    const int freq_offset = UE->common_vars.freq_offset; // frequency offset computed with pss in initial sync
    const int hw_slot_offset =
        ((ret.rx_offset << 1) / fp->samples_per_subframe * fp->slots_per_subframe)
        + round((float)((ret.rx_offset << 1) % fp->samples_per_subframe) / fp->samples_per_slot0);

    UE->freq_offset = freq_offset - UE->dl_Doppler_shift;
    if (!get_nrUE_params()->cont_fo_comp) {
      // rerun with new cell parameters and frequency-offset
      nrue_ru_set_freq(UE, ul_carrier, dl_carrier, freq_offset);
    }

    if (get_nrUE_params()->agc) {
      /* The ISAC_FREEZE_RF_GAIN escape hatch that used to gate this belonged to the retracted
       * PBCH-tracking investigation, and its declaration lived in the block x410-100MHz replaced.
       * Restored to the baseline behaviour: always apply the post-sync gain adjustment. */
      nrue_ru_adjust_rx_gain(UE, UE->adjust_rxgain);
    }

    LOG_I(PHY, "Got synch: hw_slot_offset %d, carrier off %d Hz\n", hw_slot_offset, freq_offset);

    UE->is_synchronized = 1;
  } else {
    int gain_change = 0;
    if (get_nrUE_params()->agc)
      gain_change = nrue_ru_adjust_rx_gain(UE, INCREASE_IN_RXGAIN);
    if (gain_change)
      LOG_I(PHY, "synch retry: Rx gain increased \n");
    else
      LOG_E(PHY, "synch Failed: \n");
  }
}

static int nr_ue_slot_select(const fapi_nr_config_request_t *cfg, int nr_slot)
{
  if (cfg->cell_config.frame_duplex_type == FDD)
    return NR_UPLINK_SLOT | NR_DOWNLINK_SLOT;

  const fapi_nr_tdd_table_t *tdd_table = &cfg->tdd_table;
  int rel_slot = nr_slot % tdd_table->tdd_period_in_slots;

  if (tdd_table->max_tdd_periodicity_list == NULL) // this happens before receiving TDD configuration
    return NR_DOWNLINK_SLOT;

  const fapi_nr_max_tdd_periodicity_t *current_slot = &tdd_table->max_tdd_periodicity_list[rel_slot];

  // if the 1st symbol is UL the whole slot is UL
  if (current_slot->max_num_of_symbol_per_slot_list[0].slot_config == 1)
    return NR_UPLINK_SLOT;

  // if the 1st symbol is flexible the whole slot is mixed
  if (current_slot->max_num_of_symbol_per_slot_list[0].slot_config == 2)
    return NR_MIXED_SLOT;

  for (int i = 1; i < NR_SYMBOLS_PER_SLOT; i++) {
    // if the 1st symbol is DL and any other is not, the slot is mixed
    if (current_slot->max_num_of_symbol_per_slot_list[i].slot_config != 0) {
      return NR_MIXED_SLOT;
    }
  }

  // if here, all the symbols where DL
  return NR_DOWNLINK_SLOT;
}

static void RU_write(nr_rxtx_thread_data_t *rxtxD, bool sl_tx_action, c16_t **txp)
{
  int writeBlockSize = rxtxD->writeBlockSize;
  if (writeBlockSize == 0)
    return;

  /* Passive receive-only mode: MAC never leaves UE_RECEIVING_SIB so txp is all-zero anyway, but the
     radio TX stream would still be keyed every slot (LO leakage / DAC noise into a co-located RX).
     Skip the write entirely on real radios. Under rfsimulator the UE's write drives the simulator's
     sample clock, so it must be kept (cf. the explicit dummyWrite() in readFrame()). */
  if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && !IS_SOFTMODEM_RFSIM)
    return;

  PHY_VARS_NR_UE *UE = rxtxD->UE;
  const fapi_nr_config_request_t *cfg = &UE->nrUE_config;
  const UE_nr_rxtx_proc_t *proc = &rxtxD->proc;

  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  if (UE->sl_mode == 2)
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  int slot = proc->nr_slot_tx;

  radio_tx_burst_flag_t flags = TX_BURST_INVALID;

  if (UE->received_config_request) {
    if (fp->frame_type == FDD || get_softmodem_params()->continuous_tx) {
      flags = TX_BURST_MIDDLE;
    // In case of Sidelink, USRP write needed only in case transmission
    // needs to be done in this slot and not based on tdd ULDL configuration.
    } else if (UE->sl_mode == 2) {
      if (sl_tx_action)
        flags = TX_BURST_START_AND_END;
    } else {
      int slots_frame = fp->slots_per_frame;
      int curr_slot = nr_ue_slot_select(cfg, slot);
      if (curr_slot != NR_DOWNLINK_SLOT) {
        int next_slot = nr_ue_slot_select(cfg, (slot + 1) % slots_frame);
        int prev_slot = nr_ue_slot_select(cfg, (slot + slots_frame - 1) % slots_frame);
        if (prev_slot == NR_DOWNLINK_SLOT)
          flags = TX_BURST_START;
        else if (next_slot == NR_DOWNLINK_SLOT)
          flags = TX_BURST_END;
        else
          flags = TX_BURST_MIDDLE;
      }
    }
  }

  if (!IS_SOFTMODEM_RFSIM) {
    uint64_t deadline_us = rxtxD->absolute_deadline_us;
    struct timespec current_time;
    if (clock_gettime(CLOCK_REALTIME, &current_time)) {
      LOG_E(PHY, "clock_gettime failed\n");
    }
    uint64_t current_time_us = current_time.tv_sec * 1e6 + current_time.tv_nsec / 1e3;
    if (current_time_us > deadline_us) {
      static unsigned int deadline_warning_rate_limit = 0;
      if (deadline_warning_rate_limit % 1000 == 0) {
        LOG_W(PHY,
              "Deadline missed for tx slot %d.%d (current time %lu us, deadline %lu us, missed by %lu)\n",
              proc->frame_tx,
              proc->nr_slot_tx,
              current_time_us,
              deadline_us,
              current_time_us - deadline_us);
      }
      deadline_warning_rate_limit++;
    }
  }

  openair0_timestamp_t writeTimestamp = proc->timestamp_tx;
  // if writeBlockSize gets longer that slot size, fill with dummy
  const int maxWriteBlockSize = get_samples_per_slot(proc->nr_slot_tx, fp);
  while (writeBlockSize > maxWriteBlockSize) {
    const int dummyBlockSize = min(writeBlockSize - maxWriteBlockSize, maxWriteBlockSize);
    int tmp = nrue_ru_write_reorder(UE, writeTimestamp, (void **)txp, dummyBlockSize, fp->nb_antennas_tx, flags);
    AssertFatal(tmp == dummyBlockSize, "write samples to reorder function failed %d", tmp);

    writeTimestamp += dummyBlockSize;
    writeBlockSize -= dummyBlockSize;
  }

  // pre-compensate UL frequency offset
  if (flags != TX_BURST_INVALID && get_nrUE_params()->cont_fo_comp) {
    double ul_freq_offset = -UE->freq_offset * ((double)fp->ul_CarrierFreq / (double)fp->dl_CarrierFreq);
    if (get_nrUE_params()->cont_fo_comp == 2) // different from LO frequency error compensation, Doppler UL pre-compensation has to be negative
      ul_freq_offset = -ul_freq_offset;
    else if (get_nrUE_params()->cont_fo_comp == 3) // do not consider residual DL FO for UL pre-compensation at all
      ul_freq_offset = 0;
    for (int i = 0; i < fp->nb_antennas_tx; i++)
      nr_fo_compensation(UE->ul_Doppler_shift + ul_freq_offset,
                         fp->samples_per_subframe,
                         writeTimestamp,
                         txp[i],
                         txp[i],
                         writeBlockSize);
  }

  int tmp = nrue_ru_write_reorder(UE, writeTimestamp, (void **)txp, writeBlockSize, fp->nb_antennas_tx, flags);
  AssertFatal(tmp == writeBlockSize, "write to reorder function failed %d", tmp);
}

void processSlotTX(void *arg)
{
  TracyCZone(ctx, true);
  nr_rxtx_thread_data_t *rxtxD = arg;
  const UE_nr_rxtx_proc_t *proc = &rxtxD->proc;
  PHY_VARS_NR_UE *UE = rxtxD->UE;
  nr_phy_data_tx_t phy_data = {0};
  bool sl_tx_action = false;

  if (UE->if_inst)
    UE->if_inst->slot_indication(UE->Mod_id, true);

  LOG_D(PHY, "SlotTx %d.%d => slot type %d\n", proc->frame_tx, proc->nr_slot_tx, proc->tx_slot_type);

  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  c16_t *txp[fp->nb_antennas_tx];
  for (int i = 0; i < fp->nb_antennas_tx; i++) {
    txp[i] = UE->common_vars.txData[i] + get_samples_slot_timestamp(fp, proc->nr_slot_tx);
  }

  if (proc->tx_slot_type == NR_UPLINK_SLOT || proc->tx_slot_type == NR_MIXED_SLOT) {
    if (UE->sl_mode == 2 && proc->tx_slot_type == NR_SIDELINK_SLOT) {
      // trigger L2 to run ue_sidelink_scheduler thru IF module
      if (UE->if_inst != NULL && UE->if_inst->sl_indication != NULL) {
        start_meas(&UE->ue_ul_indication_stats);
        nr_sidelink_indication_t sl_indication = {.module_id = UE->Mod_id,
                                                  .gNB_index = proc->gNB_id,
                                                  .cc_id = UE->CC_id,
                                                  .hfn_tx = proc->hfn_tx,
                                                  .frame_tx = proc->frame_tx,
                                                  .slot_tx = proc->nr_slot_tx,
                                                  .hfn_rx = proc->hfn_rx,
                                                  .frame_rx = proc->frame_rx,
                                                  .slot_rx = proc->nr_slot_rx,
                                                  .slot_type = SIDELINK_SLOT_TYPE_TX,
                                                  .phy_data = &phy_data};

        UE->if_inst->sl_indication(&sl_indication);
        stop_meas(&UE->ue_ul_indication_stats);
      }
      dynamic_barrier_join(rxtxD->next_barrier);

      if (phy_data.sl_tx_action) {

        AssertFatal((phy_data.sl_tx_action >= SL_NR_CONFIG_TYPE_TX_PSBCH &&
                     phy_data.sl_tx_action < SL_NR_CONFIG_TYPE_TX_MAXIMUM), "Incorrect SL TX Action Scheduled\n");

        phy_procedures_nrUE_SL_TX(UE, proc, &phy_data, txp);

        sl_tx_action = true;
      }

    } else {
      // trigger L2 to run ue_scheduler thru IF module
      // [TODO] mapping right after NR initial sync
      if (UE->if_inst != NULL && UE->if_inst->ul_indication != NULL) {
        start_meas(&UE->ue_ul_indication_stats);
        nr_uplink_indication_t ul_indication = {.module_id = UE->Mod_id,
                                                .gNB_index = proc->gNB_id,
                                                .cc_id = UE->CC_id,
                                                .frame = proc->frame_tx,
                                                .slot = proc->nr_slot_tx,
                                                .phy_data = &phy_data};

        UE->if_inst->ul_indication(&ul_indication);
        stop_meas(&UE->ue_ul_indication_stats);
      }
      dynamic_barrier_join(rxtxD->next_barrier);

      phy_procedures_nrUE_TX(UE, proc, &phy_data, txp);
    }
  } else {
    dynamic_barrier_join(rxtxD->next_barrier);
  }
  RU_write(rxtxD, sl_tx_action, txp);
  TracyCZoneEnd(ctx);
}

static uint64_t get_carrier_frequency(const int N_RB, const int mu, const uint32_t pointA_freq_khz)
{
  const uint64_t bw = (NR_NB_SC_PER_RB * N_RB) * MU_SCS(mu);
  const uint64_t carrier_freq = (pointA_freq_khz + bw / 2) * 1000;
  return carrier_freq;
}

static int handle_sync_req_from_mac(PHY_VARS_NR_UE *UE)
{
  NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  // Start synchronization with a target gNB
  if (UE->synch_request.received_synch_request == 1) {
    // if upper layers signal BW scan we do as instructed by command line parameter
    // if upper layers disable BW scan we set it to false
    if (UE->synch_request.synch_req.ssb_bw_scan)
      UE->UE_scan_carrier = get_nrUE_params()->UE_scan_carrier;
    else
      UE->UE_scan_carrier = false;
    UE->target_Nid_cell = UE->synch_request.synch_req.target_Nid_cell;

    const fapi_nr_config_request_t *config = &UE->nrUE_config;
    const fapi_nr_ue_carrier_config_t *cfg = &config->carrier_config;
    uint64_t dl_CarrierFreq = get_carrier_frequency(fp->N_RB_DL, fp->numerology_index, cfg->dl_frequency);
    uint64_t ul_CarrierFreq = get_carrier_frequency(fp->N_RB_UL, fp->numerology_index, cfg->uplink_frequency);
    // cfg->dl_frequency is point A in kHz and is only populated once SIB1 has been decoded. Before
    // that it is 0, and get_carrier_frequency() then returns just half the carrier bandwidth --
    // e.g. 49.14 MHz at 273 PRB / 30 kHz -- which is not a frequency at all. Acting on it retunes
    // the radio away from the band and makes every subsequent sync attempt impossible, silently
    // discarding the -C given on the command line. MEASURED on a live 100 MHz srsRAN cell: the
    // first attempt correctly searched 3414990000, every retry searched 49140000 and never
    // recovered, so only one real attempt ever happened. Keep whatever frequency we were told to
    // use until upper layers actually know point A.
    if (cfg->dl_frequency == 0) {
      LOG_D(NR_PHY, "SYNC REQ: point A not known yet (dl_frequency=0), keeping current RF frequency\n");
    } else if (dl_CarrierFreq != fp->dl_CarrierFreq || ul_CarrierFreq != fp->ul_CarrierFreq) {
      LOG_I(NR_PHY,
            "[UE %d] SYNC REQ: RF frequency change: dl %lu->%lu Hz, ul %lu->%lu Hz (from dl_frequency=%u kHz, target_Nid_cell=%d)\n",
            UE->Mod_id,
            fp->dl_CarrierFreq,
            dl_CarrierFreq,
            fp->ul_CarrierFreq,
            ul_CarrierFreq,
            cfg->dl_frequency,
            UE->target_Nid_cell);
      nrue_ru_set_freq(UE, ul_CarrierFreq, dl_CarrierFreq, 0);
      fp->dl_CarrierFreq = dl_CarrierFreq;
      fp->ul_CarrierFreq = ul_CarrierFreq;
      init_symbol_rotation(fp);
    }

    // Same pre-SIB1 caveat as the carrier frequency above: ssb_table is only populated once upper
    // layers know the cell, so recomputing from it before that yields 0 and wipes the SSB position
    // supplied on the command line (--ssb). MEASURED: after a FIRST successful sync the next sync
    // request re-derived offset 0 and every following attempt searched the wrong place. Gate both
    // updates on the same "do upper layers actually know the cell yet" test.
    if (cfg->dl_frequency != 0) {
      int ssb_start_subcarrier = nr_get_ssb_start_sc(fp->numerology_index,
                                                     config->ssb_table.ssb_offset_point_a,
                                                     config->ssb_table.ssb_subcarrier_offset,
                                                     fp->freq_range);
      // SSB location can change during for ex: handover on the target cell
      if (ssb_start_subcarrier != fp->ssb_start_subcarrier) {
        fp->ssb_start_subcarrier = ssb_start_subcarrier;
        LOG_I(NR_PHY, "SYNC REQ: SSB location changed:%d\n", fp->ssb_start_subcarrier);
      }
    }

    // Apply Doppler based on NTN-Config for target cell
    if (UE->nrUE_config.ntn_config.is_targetcell)
      apply_ntn_timing_advance_and_doppler(UE, fp, -1);
    // Apply NTN DL Doppler as initial FO
    UE->initial_fo = UE->dl_Doppler_shift;

    /* Clearing UE harq while DL actors are active causes race condition.
        So we let the current execution to complete here.*/
    for (int i = 0; i < get_nrUE_params()->num_dl_actors; i++) {
      flush_actor(UE->dl_actors + i);
    }
    for (int i = 0; i < get_nrUE_params()->num_ul_actors; i++) {
      flush_actor(UE->ul_actors + i);
    }

    clean_UE_harq(UE);
    UE->is_synchronized = 0;
    UE->synch_request.received_synch_request = 0;
    return 0;
  }
  return 1;
}

static int UE_dl_preprocessing(PHY_VARS_NR_UE *UE,
                               const UE_nr_rxtx_proc_t *proc,
                               int *tx_wait_for_dlsch,
                               nr_phy_data_t *phy_data,
                               bool *stats_printed)
{
  TracyCZone(ctx, true);
  int sampleShift = INT_MAX;
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  if (UE->sl_mode == 2)
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  // process what RRC thread sent to MAC
  do {
    notifiedFIFO_elt_t *elt = pollNotifiedFIFO(&get_mac_inst(UE->Mod_id)->input_nf);
    if (!elt) {
      break;
    }
    process_msg_rcc_to_mac(NotifiedFifoData(elt), UE->Mod_id);
    delNotifiedFIFO_elt(elt);
  } while (true);

  if (UE->if_inst)
    UE->if_inst->slot_indication(UE->Mod_id, false);

  bool dl_slot = false;
  if (proc->rx_slot_type == NR_DOWNLINK_SLOT || proc->rx_slot_type == NR_MIXED_SLOT) {
    dl_slot = true;
    if(UE->if_inst != NULL && UE->if_inst->dl_indication != NULL) {
      nr_downlink_indication_t dl_indication = (nr_downlink_indication_t){
          .gNB_index = proc->gNB_id,
          .module_id = UE->Mod_id,
          .cc_id = UE->CC_id,
          .hfn = proc->hfn_rx,
          .frame = proc->frame_rx,
          .slot = proc->nr_slot_rx,
          .phy_data = phy_data,
      };
      UE->if_inst->dl_indication(&dl_indication);
    }

    sampleShift = pbch_processing(UE, proc, phy_data);
    pdcch_processing(UE, proc, phy_data);
    // Phase 3 (TOTAL_PASSIVE_UE_HANDOVER.md): blind PDCCH/DCI-1_1 decode for a passive receiver with
    // no RRC context of its own -- a fully local PDCCH config, never phy_data's real MAC-driven one
    // (see nr_pdcch_blind_monitor_rt.h). No-op unless [sensing] pdcch_blind_monitor_* is configured.
    nr_pdcch_blind_monitor_process(UE, proc);
    if (phy_data->dlsch[0].active
        && (phy_data->dlsch[0].rnti_type == TYPE_C_RNTI_ || phy_data->dlsch[0].rnti_type == TYPE_RA_RNTI_)) {
      // indicate to tx thread to wait for DLSCH decoding
      if (phy_data->dlsch_config.k1_feedback) {  // if feedback is 0 there is no HARQ associated with this DLSCH
        const int ack_nack_slot = (proc->nr_slot_rx + phy_data->dlsch_config.k1_feedback) % fp->slots_per_frame;
        tx_wait_for_dlsch[ack_nack_slot]++;
      }
    }
  }
  if (fp->frame_type == FDD || !dl_slot) {
    // good time to print statistics, we don't have to spend time  to decode DCI
    if (proc->frame_rx % 128 == 0) {
      if (*stats_printed == false) {
        print_ue_mac_stats(UE->Mod_id, proc->frame_rx, proc->nr_slot_rx);
        *stats_printed = true;
      }
    } else {
      *stats_printed = false;
    }
  }

  if (UE->sl_mode == 2) {
    if (proc->rx_slot_type == NR_SIDELINK_SLOT) {
      phy_data->sl_rx_action = 0;
      if (UE->if_inst != NULL && UE->if_inst->sl_indication != NULL) {
        nr_sidelink_indication_t sl_indication;
        nr_fill_sl_indication(&sl_indication, NULL, NULL, proc, UE, phy_data);
        UE->if_inst->sl_indication(&sl_indication);
      }

      if (phy_data->sl_rx_action) {

        AssertFatal((phy_data->sl_rx_action >= SL_NR_CONFIG_TYPE_RX_PSBCH &&
                     phy_data->sl_rx_action < SL_NR_CONFIG_TYPE_RX_MAXIMUM), "Incorrect SL RX Action Scheduled\n");

        sampleShift = psbch_pscch_processing(UE, proc, phy_data);
      }
    }
  } else {
    /* UPLINK slot. Nothing used to run here -- a UE has no reason to process one. A passive
     * receiver does: this is where another UE's PUSCH actually is, k2 slots after the DCI that
     * scheduled it. No-op unless [sensing] pdcch_blind_monitor_ul_pusch is configured. */
    ue_ta_procedures(UE, proc->nr_slot_tx, proc->frame_tx);
  }

  if (IS_PASSIVE_RX_MODE(get_softmodem_params())) {
    nr_passive_replay_slot(UE, proc);
    nr_pusch_passive_monitor_process(UE, proc);
  }

  TracyCZoneEnd(ctx);
  return sampleShift;
}

void UE_dl_processing(void *arg) {
  TracyCZone(ctx, true);;
  nr_rxtx_thread_data_t *rxtxD = (nr_rxtx_thread_data_t *) arg;
  UE_nr_rxtx_proc_t *proc = &rxtxD->proc;
  PHY_VARS_NR_UE    *UE   = rxtxD->UE;
  nr_phy_data_t *phy_data = &rxtxD->phy_data;

  if (!UE->sl_mode)
    pdsch_processing(UE, proc, phy_data);

  TracyCZoneEnd(ctx);
}

void dummyWrite(PHY_VARS_NR_UE *UE, openair0_timestamp_t timestamp, int writeBlockSize)
{
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  if (UE->sl_mode == 2)
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  c16_t *dummy_tx[fp->nb_antennas_tx];
  c16_t dummy_tx_data[writeBlockSize];
  memset(dummy_tx_data, 0, sizeof(dummy_tx_data));
  for (int i = 0; i < fp->nb_antennas_tx; i++)
    dummy_tx[i] = dummy_tx_data;

  int tmp = nrue_ru_write(UE, timestamp, (void **)dummy_tx, writeBlockSize, fp->nb_antennas_tx, 4);
  AssertFatal(writeBlockSize == tmp, "write to reorder function failed %d", tmp);
}

void readFrame(PHY_VARS_NR_UE *UE, openair0_timestamp_t *timestamp, int duration_rx_to_tx, bool toTrash)
{
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  // two frames for initial sync
  int num_frames = 2;
  // In Sidelink worst case SL-SSB can be sent once in 16 frames
  if (UE->sl_mode == 2) {
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;
    num_frames = SL_NR_PSBCH_REPETITION_IN_FRAMES;
  }

  c16_t *rxp[fp->nb_antennas_rx];
  if (toTrash) {
    rxp[0] = malloc16(get_samples_per_slot(0, fp) * sizeof(c16_t));
    for (int i = 1; i < fp->nb_antennas_rx; i++)
      rxp[i] = rxp[0];
  }

  for (int x = 0; x < num_frames * NR_NUMBER_OF_SUBFRAMES_PER_FRAME; x++) { // two frames for initial sync
    for (int slot_rx = 0; slot_rx < fp->slots_per_subframe; slot_rx++) {
      if (!toTrash)
        for (int i = 0; i < fp->nb_antennas_rx; i++)
          rxp[i] = &UE->common_vars.rxdata[i][x * fp->samples_per_subframe + get_samples_slot_timestamp(fp, slot_rx)];

      int readBlockSize = get_samples_per_slot(slot_rx, fp);
      int tmp = nrue_ru_read(UE, timestamp, (void **)rxp, readBlockSize, fp->nb_antennas_rx);
      UEscopeCopy(UE, ueTimeDomainSamplesBeforeSync, rxp[0], sizeof(c16_t), 1, readBlockSize, 0);
      if (readBlockSize != tmp) {
        if (!oai_exit)
          LOG_E(PHY, "SENSING: RFSTALL readFrame got %d of %d RF samples; no acquisition on incomplete IQ\n", tmp, readBlockSize);
        oai_exit = 1;
        if (toTrash)
          free(rxp[0]);
        return;
      }

      if (IS_SOFTMODEM_RFSIM) {
        int slot_tx = (slot_rx + duration_rx_to_tx) % fp->slots_per_frame;
        int writeBlockSize = get_samples_per_slot(slot_tx, fp);
        int ta = UE->timing_advance + UE->timing_advance_ntn;
        const openair0_timestamp_t writeTimestamp =
            *timestamp + get_samples_slot_duration(fp, slot_rx, duration_rx_to_tx) - UE->N_TA_offset - ta;
        dummyWrite(UE, writeTimestamp, writeBlockSize);
      }
    }
  }

  if (toTrash)
    free(rxp[0]);
}


/* ---- DEFERRED COARSE TIMING REBASE (2026-08-06) ------------------------------------------------
 * A multi-thousand-sample displacement is a loss of COARSE alignment, not a fine correction.
 * Pushing it through shiftForNextFrame -- which adjusts readBlockSize incrementally -- moves the
 * sample timeline while the current frame/slot/symbol bookkeeping still describes the old one, and
 * the receiver then derives an impossible PBCH symbol index (measured: AssertFatal(dmrss<3) abort
 * on the first firing). So the anchor only REQUESTS a rebase here; it is applied atomically at a
 * frame boundary, through the same discard path acquisition uses to move the stream origin.
 */
_Atomic long nr_ue_pending_rebase_delta = 0;
_Atomic int nr_ue_pending_rebase_valid = 0;
/* Incremented on every applied rebase so consumers can tell which timing epoch a
 * measurement belongs to, and count occasions since the last one. */
_Atomic int nr_ue_rebase_epoch = 0;
/* Absolute RF timestamp of the most recent read, and total samples consumed since start.
 * Between two SSB occasions (20 ms) these must advance by exactly samples_per_frame*2 unless a
 * timing correction was explicitly applied -- so comparing them against that constant separates
 * "samples were physically discarded/duplicated" from "only the logical origin moved". */
_Atomic long nr_ue_diag_rf_timestamp = 0;
_Atomic long nr_ue_diag_samples_consumed = 0;
/* Every mutation of the global timing state, with its call site, so the one that coincides with
 * the coherence collapse is identifiable rather than inferred. */
void nr_ue_timing_mutation_log(const char *what, long before, long after, const char *file, int line)
{
  static int left = 200;
  if (before == after || left <= 0)
    return;
  left--;
  LOG_W(PHY, "SENSING: TIMEMUT %s %ld -> %ld (delta %+ld) at %s:%d ts=%ld consumed=%ld\n",
        what, before, after, after - before, file, line,
        atomic_load_explicit(&nr_ue_diag_rf_timestamp, memory_order_relaxed),
        atomic_load_explicit(&nr_ue_diag_samples_consumed, memory_order_relaxed));
}
#define LOG_TIMEMUT(w, b, a) nr_ue_timing_mutation_log((w), (long)(b), (long)(a), __FILE__, __LINE__)

/* ---- RF/PBCH CENSUS (2026-08-23) -------------------------------------------------------------
 * An UNCAPPED, UNCONDITIONAL periodic census, added because every existing timing/PBCH diagnostic
 * in this file is censored in a way that silently invalidates exactly the inference one wants to
 * draw from it: PBCHFAIL stops after 6 prints (nr_pbch.c), TRACKLOCK after 25, and TIMEMUT both
 * caps at 200 AND suppresses any event where the value did not change -- so "TIMEMUT went to zero"
 * means "shiftForNextFrame stopped moving", NOT "PBCH stopped decoding". Reading it as the latter
 * produced one wrong root cause here already.
 * Reports absolute RF power straight off the just-read time-domain slot buffer, which is
 * independent of frame/symbol alignment: it therefore separates "the signal went away" from "the
 * receiver is looking in the wrong place", which no other instrument here can do. */
static double g_census_pow;
static long g_census_pow_n;
/* RAW per-antenna receive power, measured on the TIME-DOMAIN slot buffer before any alignment,
 * demodulation or channel estimation. This exists to answer one question that nothing else here
 * can: RXBRANCH's `pw[]` is the DM-RS CHANNEL-ESTIMATE magnitude, so a branch that receives a
 * perfectly good signal but is misaligned in time reads 15 dB down anyway. Comparing these two
 * separates "this antenna is not receiving" (antennas/cabling) from "this antenna is receiving and
 * we are estimating it badly" (software) -- and on this rig the four branches span ~20 dB, which is
 * not a shape four elements at lambda/2 looking at the same gNB should produce. */
#define CENSUS_MAX_ANT 4
static double g_census_pow_ant[CENSUS_MAX_ANT];
/* TRUE per-antenna POWER, mean(I^2 + Q^2), unnormalised and in float before squaring.
 * ANTPOW above accumulates mean(|I|+|Q|) -- an AMPLITUDE -- and prints it normalised to the
 * strongest branch, so it yields a RATIO and never an absolute level. That is enough to see that
 * branches differ and useless for saying why: a quiet antenna, a gain that never landed, and a
 * port that is not the one the cable is in all produce the same ratio.
 * Reported in dB relative to FULL SCALE for int16 samples (32767^2 per component, two components),
 * so the number is comparable across runs and against the ADC ceiling -- NOT an RF-input power and
 * NOT an SNR. Clipping is counted separately, because a clipped branch reads HIGH while being the
 * most damaged. */
static double   g_census_pw2_ant[CENSUS_MAX_ANT];
static uint64_t g_census_clip_ant[CENSUS_MAX_ANT];
static int32_t  g_census_peak_ant[CENSUS_MAX_ANT]; /* max |I|,|Q| seen, int16 full scale */
static uint64_t g_census_hot_ant[CENSUS_MAX_ANT];  /* samples with |I| or |Q| > 0.9 FS (29490) */
static uint64_t g_census_smp_ant[CENSUS_MAX_ANT];  /* samples inspected, for the hot fraction */
static uint64_t g_census_pw2_n;

/* RX power reference, resolved from the USRP driver at runtime. The driver is a dlopen'd plugin,
 * so the executable cannot link against it -- dlsym is the only way to reach it, and a NULL result
 * simply means a non-USRP radio or an older driver, in which case only dBFS is reported. */
static double (*g_pwr_ref_fn)(int) = NULL;
static int g_pwr_ref_looked_up = 0;
static double nr_ue_rx_power_reference_dbm(int ch)
{
  if (!g_pwr_ref_looked_up) {
    g_pwr_ref_looked_up = 1;
    g_pwr_ref_fn = (double (*)(int))dlsym(RTLD_DEFAULT, "openair0_rx_power_reference_dbm");
  }
  return g_pwr_ref_fn ? g_pwr_ref_fn(ch) : NAN;
}
/* ULPROBE (ISAC_UL_PROBE=1, default OFF): the three Phase-0 questions the UL work is gated on, and
 * none of them can be answered from an existing capture.
 *   1. Does this receiver even KNOW the TDD pattern? nr_ue_slot_select() returns NR_DOWNLINK_SLOT
 *      for EVERY slot until UE->received_config_request goes true (this file's own fallback), so a
 *      passive receiver that never got a config request silently treats UL slots as DL.
 *   2. Are UL-slot samples present in rxdata at all? Passive mode skips RU_write entirely on a real
 *      radio, so they should be -- but "should be" is how this project acquires wrong conclusions.
 *   3. Is the UE AUDIBLE in UL slots? This is the go/no-go for the whole PUSCH path: the gNB
 *      transmits at tens of watts, a UE at a fraction of one, under power control that minimises it.
 * Splitting the SAME per-antenna accumulator by slot type answers 2 and 3 together, and costs one
 * compare per slot when enabled and nothing at all when not. */
static double g_ulprobe_pow[2][CENSUS_MAX_ANT];
static long g_ulprobe_n[2];
static int g_ulprobe_on = -1;
static long g_census_pow_ant_n;
static long g_census_ssb_slots;
static long g_census_slots;

static void syncInFrame(PHY_VARS_NR_UE *UE, openair0_timestamp_t *timestamp, int duration_rx_to_tx, openair0_timestamp_t rx_offset)
{
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  if (UE->sl_mode == 2)
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;

  LOG_I(PHY, "Resynchronizing RX by %ld samples\n", rx_offset);

  int size = rx_offset;
  while (size > 0) {
    // Set a maximum transfer size. As we usually read/write single slots, we use the size of slot 0 as maximum here.
    const int unitTransfer = min(get_samples_per_slot(0, fp), size);
    const int res = nrue_ru_read(UE, timestamp, (void **)UE->common_vars.rxdata, unitTransfer, fp->nb_antennas_rx);
    if (oai_exit)
      return;
    if (res <= 0) {
      LOG_W(PHY, "Unable to read RF samples while resynchronizing\n");
      break;
    }
    if (unitTransfer != res)
      LOG_W(PHY, "syncInFrame: got %d of %d RF samples\n", res, unitTransfer);
    if (IS_SOFTMODEM_RFSIM) {
      int ta = UE->timing_advance + UE->timing_advance_ntn;
      const openair0_timestamp_t writeTimestamp =
          *timestamp + get_samples_slot_duration(fp, 0, duration_rx_to_tx) - UE->N_TA_offset - ta;
      dummyWrite(UE, writeTimestamp, unitTransfer);
    }
    size -= res;
  }
}

static inline int get_firstSymSamp(uint16_t slot, const NR_DL_FRAME_PARMS *fp)
{
  return get_samples_symbol_duration(fp, slot, 0, 1);
}

static inline int get_readBlockSize(uint16_t slot, const NR_DL_FRAME_PARMS *fp)
{
  int rem_samples = get_samples_per_slot(slot, fp) - get_firstSymSamp(slot, fp);
  int next_slot_first_symbol = 0;
  if (slot < (fp->slots_per_frame-1))
    next_slot_first_symbol = get_firstSymSamp(slot+1, fp);
  return rem_samples + next_slot_first_symbol;
}

void trs_freq_correction(PHY_VARS_NR_UE *ue, int cfo)
{
  if (abs(cfo) > TRS_CFO_THRESH) {
    LOG_A(PHY, "CFO estimated (%d) from TRS exceeded threshold (%d). Adjusting radio CF\n", cfo, TRS_CFO_THRESH);
    ue->freq_offset += cfo;
    uint64_t dl_carrier;
    uint64_t ul_carrier;
    nr_get_carrier_frequencies(ue, &dl_carrier, &ul_carrier);
    nrue_ru_set_freq(ue, ul_carrier, dl_carrier, ue->freq_offset);
  }
}

void *UE_thread(void *arg)
{
  //this thread should be over the processing thread to keep in real time
  PHY_VARS_NR_UE *UE = (PHY_VARS_NR_UE *)arg;
  const NR_DL_FRAME_PARMS *fp = &UE->frame_parms;
  //  int tx_enabled = 0;
  enum stream_status_e stream_status = STREAM_STATUS_UNSYNC;
  nr_rx_continuity_t rx_continuity = {0};
  fapi_nr_config_request_t *cfg = &UE->nrUE_config;
  sl_nr_phy_config_request_t *sl_cfg = NULL;
  if (UE->sl_mode == 2) {
    fp = &UE->SL_UE_PHY_PARAMS.sl_frame_params;
    sl_cfg = &UE->SL_UE_PHY_PARAMS.sl_config;
  }

  UE->is_synchronized = 0;
  InitSinLUT();

  notifiedFIFO_t nf;
  initNotifiedFIFO(&nf);

  notifiedFIFO_t freeBlocks;
  initNotifiedFIFO_nothreadSafe(&freeBlocks);

  const double ntn_init_time_drift = get_nrUE_params()->ntn_init_time_drift;
  if (get_nrUE_params()->time_sync_I)
    // ntn_init_time_drift is in µs/s, max_pos_acc * time_sync_I is in samples/frame
    UE->max_pos_acc = ntn_init_time_drift * 1e-6 * fp->samples_per_frame / get_nrUE_params()->time_sync_I;
  else
    UE->max_pos_acc = 0;

  bool ntn_targetcell = false;
  int ntn_koffset = 0;
  int duration_rx_to_tx = NR_UE_CAPABILITY_SLOT_RX_TO_TX;
  int timing_advance = UE->timing_advance + UE->timing_advance_ntn;
  UE->N_TA_offset = determine_N_TA_offset(UE);
  NR_UE_MAC_INST_t *mac = get_mac_inst(UE->Mod_id);

  bool syncRunning = false;
  const int nb_slot_frame = fp->slots_per_frame;
  int absolute_slot = 0, decoded_frame_rx = MAX_FRAME_NUMBER - 1, trashed_frames = 0;
  int tx_wait_for_dlsch[NR_MAX_SLOTS_PER_FRAME];

  for(int i = 0; i < NUM_PROCESS_SLOT_TX_BARRIERS; i++) {
    dynamic_barrier_init(&UE->process_slot_tx_barriers[i]);
  }
  int shiftForNextFrame = 0;
  int intialSyncOffset = 0;
  const char *auto_acquire_env = getenv("ISAC_AUTO_ACQUIRE");
  const bool auto_timing = IS_PASSIVE_RX_MODE(get_softmodem_params())
                           && auto_acquire_env && !strcmp(auto_acquire_env, "1");
  /* BLIND-SCAN CONFIRM PASS (2026-09-19). MEASURED on the Swisscom macro (PCI 382, 3610.56 MHz SSB,
   * 273 PRB, identical -C and RX gain): from a 64-GSCN blind scan the receiver acquires and decodes
   * the MIB every time, then NEVER tracks PBCH (0/50 in all 45 windows, in-window CIR energy
   * 0.42-0.77, SIB1 0). With the SSB PINNED (--ssb 204) the same cell tracks 50/50, in-window 0.991,
   * and SIB1 decodes. Acquisition output is identical between the two (same PCI, SSB index, symbol
   * offset, sync_pos_frame, sub-kHz CFO), so the multi-GSCN scan leaves some other state the pinned
   * path sets correctly. Rather than model that difference: once the scan has WON a GSCN, redo the
   * acquisition against that SSB alone -- the second pass IS the pinned path. Costs one extra
   * acquisition (~0.7 s) on a blind-scan start only; a pinned start never triggers it.
   * ISAC_SCAN_CONFIRM=0 disables. */
  const char *scan_confirm_env = getenv("ISAC_SCAN_CONFIRM");
  const bool scan_confirm_on = (scan_confirm_env == NULL) || (atoi(scan_confirm_env) != 0);
  bool scan_confirm_pending = false;
  int scan_confirm_left = 1; /* once per process: a second pass that also fails must not loop */
  nr_gscn_info_t scan_confirm_ssb = {0};
  bool scan_confirm_check = false;
  int scan_confirm_fo1 = 0;
  bool auto_anchor_valid = false, auto_drift_ready = false;
  openair0_timestamp_t auto_anchor_timestamp = 0;
  int auto_anchor_frame = 0, auto_anchor_pci = -1;
  double auto_drift_samples_per_frame = 0;
  nr_gscn_info_t auto_anchor_ssb = {0};

  openair0_timestamp_t sync_timestamp;
  bool stats_printed = false;

  if (get_softmodem_params()->sync_ref && UE->sl_mode == 2) {
    UE->is_synchronized = 1;
  } else {
    //warm up the RF board
    openair0_timestamp_t tmp;
    for (int i = 0; i < 50; i++)
      readFrame(UE, &tmp, duration_rx_to_tx, true);
  }

  while (!oai_exit) {
    if (syncRunning) {
      notifiedFIFO_elt_t *res = pollNotifiedFIFO(&nf);

      if (res) {
        syncRunning = false;
        if (UE->is_synchronized) {
          UE->synch_request.received_synch_request = 0;
          if (UE->sl_mode == SL_MODE2_SUPPORTED)
            decoded_frame_rx = UE->SL_UE_PHY_PARAMS.sync_params.DFN;
          else {
            // We must wait the RRC layer decoded the MIB and sent us the frame number
            /* RRC may queue SCHED_SIB after CONFIG_MIB. A later acquisition
             * must not mistake that scheduling message for a fresh MIB.
             * Process every message, preserving FIFO order, until the actual
             * MIB configuration has been applied. */
            bool received_mib = false;
            do {
              notifiedFIFO_elt_t *elt = pullNotifiedFIFO(&mac->input_nf);
              AssertFatal(elt != NULL, "fifo error while waiting for MIB");
              nr_mac_rrc_message_t *message = NotifiedFifoData(elt);
              received_mib = message->payload_type == NR_MAC_RRC_CONFIG_MIB;
              process_msg_rcc_to_mac(message, UE->Mod_id);
              delNotifiedFIFO_elt(elt);
            } while (!received_mib);
            decoded_frame_rx = mac->mib_frame;
          }
          /* Post-scan geometry: the SSB position is only known once acquisition found it. */
          nr_passive_acq_set_phy_geometry(UE->frame_parms.N_RB_DL, UE->frame_parms.numerology_index,
                                          UE->frame_parms.ssb_start_subcarrier, (double)UE->frame_parms.dl_CarrierFreq);
          nr_passive_acq_note_pbch_locked(); // acquisition-state tracker: MIB applied, frame known
          if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_cfg_reconf_enabled()) {
            /* The acquired SSB centre is stable across SFN/beam-index changes. Point A is refined on SIB1. */
            const uint64_t ssb_hz = UE->frame_parms.dl_CarrierFreq
                + ((int64_t)UE->frame_parms.ssb_start_subcarrier + 120
                   - 6 * (int64_t)UE->frame_parms.N_RB_DL)
                      * (15000LL << UE->frame_parms.numerology_index);
            nr_pdcch_blind_set_sib1_semantic_hash(0);
            nr_cfg_epoch_note_identity(UE->frame_parms.Nid_cell,
                nr_cfg_identity_frequency(ssb_hz, 15000u << UE->frame_parms.numerology_index), 0);
            nr_cfg_epoch_set_slots_per_second(1000u << UE->frame_parms.numerology_index);
          }
          atomic_store_explicit(&nr_passive_metrics_pci, UE->frame_parms.Nid_cell, memory_order_relaxed); // for the ISAC_METRICS JSON (Task A2)
          LOG_A(PHY,
                "UE synchronized! decoded_frame_rx=%d UE->init_sync_frame=%d trashed_frames=%d\n",
                decoded_frame_rx,
                UE->init_sync_frame,
                trashed_frames);
          syncData_t *syncMsg = (syncData_t *)NotifiedFifoData(res);
          if (auto_timing && !auto_drift_ready) {
            /* Timestamp of the frame whose SFN was decoded from THIS PBCH.
             * init_sync_frame includes both capture frame index and offset wrap.
             * Use hardware sample time, never host wall time or CFO/frequency. */
            const openair0_timestamp_t anchor = syncMsg->capture_end
                - (openair0_timestamp_t)(UE->init_sync_frame + 1) * fp->samples_per_frame
                + syncMsg->rx_offset;
            if (auto_anchor_valid && auto_anchor_pci == fp->Nid_cell) {
              const int64_t delta_samples = anchor - auto_anchor_timestamp;
              const int64_t delta_frames = llround((double)delta_samples / fp->samples_per_frame);
              const int sfn_delta = (decoded_frame_rx - auto_anchor_frame + MAX_FRAME_NUMBER) % MAX_FRAME_NUMBER;
              if (delta_frames >= 2 && delta_frames % MAX_FRAME_NUMBER == sfn_delta) {
                const double measured = (double)delta_samples / delta_frames - fp->samples_per_frame;
                const double ppm = measured * 1e6 / fp->samples_per_frame;
                if (isfinite(ppm) && fabs(ppm) <= 200.0 && get_nrUE_params()->time_sync_I > 0) {
                  auto_drift_samples_per_frame = measured;
                  auto_drift_ready = true;
                  /* Steady-state readBlockSize subtracts shiftForNextFrame;
                   * shiftForNextFrame = -I*max_pos_acc. Positive drift therefore
                   * requires a positive integral and MORE samples per frame. */
                  /* NEGATED: measured drift is +samples/frame, but the timing integrator's
                   * steady state for that drift is NEGATIVE. Measured on this rig: auto_timing
                   * reads samples_per_frame = +5.2242 while the working (non-auto) loop converges
                   * to max_pos_acc = -522 == -(5.2242/0.01) -- same magnitude, opposite sign. The
                   * unnegated seed started the loop at double the error in the wrong direction:
                   * the FFT window walked off, PBCH still correlated (timing-tolerant) but SIB1's
                   * PDSCH extraction never landed, so acquisition stuck at PBCH_LOCKED forever. */
                  UE->max_pos_acc = -lround(measured / get_nrUE_params()->time_sync_I);
                  UE->max_pos_iir = 0;
                  LOG_I(PHY,
                        "ISAC_ACQ_DRIFT {\"pci\":%d,\"delta_frames\":%ld,\"delta_samples\":%ld,"
                        "\"samples_per_frame\":%.6f,\"sfo_ppm\":%.6f}\n",
                        fp->Nid_cell, (long)delta_frames, (long)delta_samples, measured, ppm);
                }
              }
            }
            if (!auto_drift_ready) {
              LOG_I(PHY, "ISAC_ACQ_TIMING_ANCHOR pci=%d sfn=%d timestamp=%ld; requesting fresh PBCH\n",
                    fp->Nid_cell, decoded_frame_rx, (long)anchor);
              auto_anchor_valid = true;
              auto_anchor_timestamp = anchor;
              auto_anchor_frame = decoded_frame_rx;
              auto_anchor_pci = fp->Nid_cell;
              auto_anchor_ssb = (nr_gscn_info_t){.ssbFirstSC = fp->ssb_start_subcarrier};
              for (int i = 0; i < syncMsg->numGscn; ++i)
                if (syncMsg->gscnInfo[i].ssbFirstSC == fp->ssb_start_subcarrier)
                  auto_anchor_ssb = syncMsg->gscnInfo[i];
              UE->is_synchronized = 0;
              delNotifiedFIFO_elt(res);
              stream_status = STREAM_STATUS_UNSYNC;
              continue;
            }
          }
          // shift the frame index with all the frames we trashed meanwhile we perform the synch search
          decoded_frame_rx = (decoded_frame_rx + UE->init_sync_frame + trashed_frames) % MAX_FRAME_NUMBER;
          intialSyncOffset = syncMsg->rx_offset;
          if (scan_confirm_check) {
            scan_confirm_check = false;
            const int fo2 = UE->common_vars.freq_offset;
            if (abs(scan_confirm_fo1) > 3000 && 10 * abs(fo2) < abs(scan_confirm_fo1))
              LOG_W(PHY,
                    "SENSING: SCAN_CONFIRM pass measured %d Hz vs %d Hz on the scan pass: the radio was not "
                    "reset to the initial CFO seed, the confirm result is a residual applied as a total\n",
                    fo2, scan_confirm_fo1);
          }
          /* See scan_confirm_on: re-acquire against the SSB this scan just won, so tracking starts
           * from the single-SSB path's state rather than the multi-GSCN scan's. */
          if (scan_confirm_on && scan_confirm_left > 0 && syncMsg->numGscn > 1) {
            scan_confirm_left--;
            scan_confirm_ssb = (nr_gscn_info_t){.ssbFirstSC = fp->ssb_start_subcarrier};
            for (int i = 0; i < syncMsg->numGscn; ++i)
              if (syncMsg->gscnInfo[i].ssbFirstSC == fp->ssb_start_subcarrier)
                scan_confirm_ssb = syncMsg->gscnInfo[i];
            scan_confirm_pending = true;
            UE->is_synchronized = 0;
            LOG_W(PHY,
                  "SENSING: SCAN_CONFIRM blind scan won PCI %d at SSB subcarrier %d; re-acquiring "
                  "against that SSB alone before tracking\n",
                  fp->Nid_cell, fp->ssb_start_subcarrier);
            /* UE_synch() already retuned by the scan's CFO. The confirm pass is seeded from
             * initial_fo and UE_synch() applies its result as the TOTAL offset, so re-acquiring on
             * the corrected radio measures only the residual and throws the correction away. Put
             * the radio back to the pinned-start state so the confirm pass measures the full CFO. */
            scan_confirm_fo1 = UE->common_vars.freq_offset;
            scan_confirm_check = true;
            uint64_t dl_carrier = 0, ul_carrier = 0;
            nr_get_carrier_frequencies(UE, &dl_carrier, &ul_carrier);
            nrue_ru_set_freq(UE, ul_carrier, dl_carrier, UE->initial_fo);
            UE->common_vars.freq_offset = UE->initial_fo;
            /* The retune does not reach the very next samples: capturing straight after it measured
             * the residual again (-12 Hz vs -14657, 2026-09-23). OTA, the drain below found no
             * host backlog (first read waited 19.7 ms), yet discarding those >= 2 reads (40 ms) was
             * enough (-14607 vs -14596), so the lag is in-flight/retune latency, not a queue. Drain
             * any queue anyway (until a 2-frame read waits >= 15 ms of its 20 ms), then read once more. */
            {
              int flushed = 0;
              double dt = 0.0;
              struct timespec t0, t1;
              while (dt < 15e-3 && flushed < 400 && !oai_exit) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                readFrame(UE, &sync_timestamp, duration_rx_to_tx, true);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                dt = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
                flushed += 2;
              }
              readFrame(UE, &sync_timestamp, duration_rx_to_tx, true);
              LOG_W(PHY, "SENSING: SCAN_CONFIRM drained %d queued frames after the retune (last read %.1f ms)\n",
                    flushed, dt * 1e3);
            }
            delNotifiedFIFO_elt(res);
            stream_status = STREAM_STATUS_UNSYNC;
            continue;
          }
        }
        delNotifiedFIFO_elt(res);
        stream_status = STREAM_STATUS_UNSYNC;
      } else {
        if (IS_SOFTMODEM_IQPLAYER || IS_SOFTMODEM_IQRECORDER) {
          /* For IQ recorder-player we force synchronization to happen in a fixed duration so that
             the replay runs in sync with recorded samples.
          */
          openair0_config_t *cfg0 = &openair0_cfg_g[UE->rf_map.card];
          const unsigned int sync_in_frames = cfg0->recplay_conf->u_f_sync;
          while (trashed_frames != sync_in_frames) {
            readFrame(UE, &sync_timestamp, duration_rx_to_tx, true);
            trashed_frames += 2;
          }
        } else {
          readFrame(UE, &sync_timestamp, duration_rx_to_tx, true);
          trashed_frames += ((UE->sl_mode == 2) ? SL_NR_PSBCH_REPETITION_IN_FRAMES : 2);
        }
        continue;
      }
    }

    AssertFatal(!syncRunning, "At this point synchronization can't be running\n");

    if (!UE->is_synchronized) {
      /* Acquisition consumes frames outside the slot-read accounting. Never compare
       * a new lock against the final timestamp of the previous lock. */
      nr_rx_continuity_reset(&rx_continuity);
      if (auto_timing && auto_drift_ready) {
        auto_drift_ready = false;
        auto_anchor_valid = false;
      }
      if (get_nrUE_params()->time_sync_I)
        UE->max_pos_acc = ntn_init_time_drift * 1e-6 * fp->samples_per_frame / get_nrUE_params()->time_sync_I;
      else
        UE->max_pos_acc = 0;
      UE->max_pos_iir = 0;
      /* Band-wide search (no -C): every failed acquisition steps the RX window before the next
       * capture; the first attempt uses the window tuned at start-up. */
      {
        static int s_band_scan_attempts = 0;
        if (nrue_band_scan_active() && s_band_scan_attempts++ > 0)
          nrue_band_scan_next(UE);
      }
      readFrame(UE, &sync_timestamp, duration_rx_to_tx, false);
      if (oai_exit)
        break;
      notifiedFIFO_elt_t *Msg = newNotifiedFIFO_elt(sizeof(syncData_t), 0, &nf, UE_synch);
      syncData_t *syncMsg = (syncData_t *)NotifiedFifoData(Msg);
      *syncMsg = (syncData_t){0};
      syncMsg->capture_end = sync_timestamp + get_samples_per_slot(fp->slots_per_subframe - 1, fp);
      if (auto_timing && auto_anchor_valid) {
        // The second timing observation searches only the SSB just measured OTA.
        syncMsg->gscnInfo[0] = auto_anchor_ssb;
        syncMsg->numGscn = 1;
      } else if (scan_confirm_pending) {
        // Confirm pass: only the SSB the blind scan just won (see scan_confirm_on).
        scan_confirm_pending = false;
        syncMsg->gscnInfo[0] = scan_confirm_ssb;
        syncMsg->numGscn = 1;
      } else if (UE->UE_scan_carrier) {
        // Get list of GSCN in this band for UE's bandwidth and center frequency.
        LOG_W(PHY, "UE set to scan all GSCN in current bandwidth\n");
        syncMsg->numGscn =
            get_scan_ssb_first_sc(fp->dl_CarrierFreq, fp->N_RB_DL, nrue_get_band(UE), fp->numerology_index, syncMsg->gscnInfo);
      } else {
        LOG_W(PHY, "SSB position provided\n");
        syncMsg->gscnInfo[0] = (nr_gscn_info_t){.ssbFirstSC = fp->ssb_start_subcarrier};
        syncMsg->numGscn = 1;
      }
      syncMsg->UE = UE;
      memset(&syncMsg->proc, 0, sizeof(syncMsg->proc));
      pushNotifiedFIFO(&UE->sync_actor.fifo, Msg);
      trashed_frames = 0;
      syncRunning = true;
      continue;
    }

    if (stream_status == STREAM_STATUS_UNSYNC) {
      stream_status = STREAM_STATUS_SYNCING;
      const int elapsed_frames = UE->init_sync_frame + trashed_frames + 2;
      /* Projecting the integrator across the acquisition gap is only meaningful if the integrator
       * is still describing the SAME timing origin. After a fault it is not: a runaway leaves
       * max_pos_acc at whatever the divergence reached (measured -2141 before the clamp, and
       * pinned at the +/-1024 bound after it), and multiplying THAT by elapsed_frames turns a
       * corrupt state into a large bogus sync offset. Measured 2026-09-01: max_pos_acc < 0 appears
       * in 23/100 "signal present but PBCH dead" windows against 2/1126 healthy ones -- a fresh
       * lock landing in a place PBCH never decodes from, then settling with the sign flipped.
       *
       * Bound the projection so a corrupt integrator cannot move the sync point further than a
       * plausible real drift. At the measured 0.11 ppm this link drifts ~0.14 samples/frame, so a
       * cap of one sample per frame is already an order of magnitude of headroom. */
      static int tsync_cap = -1;
      if (tsync_cap < 0) {
        const char *e = getenv("ISAC_TSYNC_RESET");
        tsync_cap = (e != NULL) ? atoi(e) : 0;
      }
      double drift = elapsed_frames * (double)UE->max_pos_acc * get_nrUE_params()->time_sync_I;
      if (tsync_cap) {
        /* Same switch, same reason: unvalidated. At the measured 0.11 ppm this link drifts ~0.14
         * samples/frame, so one sample per frame is an order of magnitude of headroom -- but a cap
         * that is wrong in the other direction breaks a legitimate long-gap re-acquisition. */
        const double drift_cap = (double)elapsed_frames;
        if (drift > drift_cap) {
          drift = drift_cap;
        } else if (drift < -drift_cap) {
          drift = -drift_cap;
        }
      }
      const int initial_drift_shift = auto_timing && auto_drift_ready
          ? lround((elapsed_frames - 1) * auto_drift_samples_per_frame) : -round(drift);
      int corrected_sync_offset = intialSyncOffset + initial_drift_shift;
      if (auto_timing && auto_drift_ready) {
        const int period = lround(fp->samples_per_frame + auto_drift_samples_per_frame);
        while (corrected_sync_offset < 0) {
          corrected_sync_offset += period;
          decoded_frame_rx = (decoded_frame_rx + MAX_FRAME_NUMBER - 1) % MAX_FRAME_NUMBER;
        }
        while (corrected_sync_offset >= period) {
          corrected_sync_offset -= period;
          decoded_frame_rx = (decoded_frame_rx + 1) % MAX_FRAME_NUMBER;
        }
        LOG_I(PHY, "ISAC_ACQ_TIMING_HANDOFF age_frames=%d drift_shift=%d offset=%d integral=%d\n",
              elapsed_frames - 1, initial_drift_shift, corrected_sync_offset, UE->max_pos_acc);
      }
      if (corrected_sync_offset >= 0) {
        syncInFrame(UE, &sync_timestamp, duration_rx_to_tx, corrected_sync_offset);
      } else {
        LOG_W(PHY,
              "Initial drift correction %d exceeds sync offset %d, using uncorrected offset\n",
              initial_drift_shift,
              intialSyncOffset);
        syncInFrame(UE, &sync_timestamp, duration_rx_to_tx, intialSyncOffset);
      }
      nrue_ru_write_reorder_clear_context(UE);
      /* NEW ORIGIN, NEW INTEGRAL. syncInFrame() above has just redefined where the frame starts, so
       * every sample of correction accumulated against the previous origin is now describing
       * something that no longer exists. Carrying it was measurable: it is what let a post-fault
       * re-acquisition inherit a wound-up (often sign-flipped) value and never recover PBCH. The
       * loop re-learns the standing offset within a few frames -- healthy runs settle back to
       * ~+450-500 -- so there is nothing to preserve. */
      /* OPT-IN, DEFAULT OFF (ISAC_TSYNC_RESET=1).
       *
       * The reasoning is sound -- max_pos_acc is an integral against the OLD timing origin and
       * syncInFrame() above has just replaced that origin -- and the defect it targets is real:
       * max_pos_acc < 0 appears in 23/100 "signal present, PBCH dead" windows against 2/1126
       * healthy ones. But it is NOT VALIDATED and the first evidence points the wrong way: runs
       * since it went in last 4-16 s against 164 s before, and this rig is documented as swinging
       * wildly run to run, so a handful of short runs cannot attribute that either way.
       *
       * There is also a real mechanism for it to HURT: the standing offset a healthy receiver holds
       * (+450 to +500, i.e. shift -5) may be a genuine constant of this signal path rather than
       * accumulated error, in which case zeroing it starts every re-acquisition 4-5 samples off and
       * makes the loop re-learn it while PBCH is trying to decode. Default off until an A/B of >=5
       * runs per arm says otherwise. */
      /* DEFAULT OFF. Opt in with ISAC_TSYNC_RESET=1.
       *
       * The DIAGNOSIS stands: max_pos_acc is an integral against the OLD timing origin, syncInFrame()
       * has just replaced that origin, and the acquisition path then AMPLIFIES the stale value by
       * elapsed_frames when projecting drift across the gap. max_pos_acc < 0 appears in 23/100
       * "signal present, PBCH dead" windows against 2/1126 healthy ones.
       *
       * But the BENEFIT does not survive a clean measurement. An early A/B showed off 38/57 s vs on
       * 229/249 s, zero overlap, and this default was flipped on that. Those runs were later found
       * to have been taken while the host had lost its 100 GbE address and UHD was falling back to
       * the 1 GbE management link -- a starved stream, not a fair test. Repeated on a healthy rig,
       * 5 reps per arm, alternated:
       *
       *     off  mean  42.8 s  median 30 s   runs 30, 75, 0, 89, 20
       *     on   mean 122.4 s  median 32 s   runs 32, 254, 23, 25, 278
       *     Welch t=1.30, df=4.7, p ~ 0.2-0.3, arms overlap
       *
       * The higher ON mean rests entirely on two outlier runs; its other three are
       * indistinguishable from off, and the medians match. No demonstrated benefit, so it does not
       * ship on. Re-test with more reps, or with a metric less noisy than run length, before
       * enabling it. */
      static int tsync_reset = -1;
      if (tsync_reset < 0) {
        const char *e = getenv("ISAC_TSYNC_RESET");
        tsync_reset = (e != NULL) ? atoi(e) : 0;
      }
      if (tsync_reset) {
        nr_ue_reset_time_sync_loop(UE);
      }
      shiftForNextFrame = -round(UE->max_pos_acc * get_nrUE_params()->time_sync_I);
      LOG_I(PHY,
            "max_pos_acc = %d, initial_drift_shift = %d, shiftForNextFrame = %d\n",
            UE->max_pos_acc,
            initial_drift_shift,
            shiftForNextFrame);
      // read in first symbol
      int ret = nrue_ru_read(UE,
                             &sync_timestamp,
                             (void **)UE->common_vars.rxdata,
                             fp->ofdm_symbol_size + fp->nb_prefix_samples0,
                             fp->nb_antennas_rx);
      if (oai_exit || ret < 0)
        break;
      if (fp->ofdm_symbol_size + fp->nb_prefix_samples0 != ret)
        LOG_W(PHY, "Initial symbol: got %d RF samples\n", ret);
      // we have the decoded frame index in the return of the synch process
      // and we shifted above to the first slot of next frame
      decoded_frame_rx = (decoded_frame_rx + 1) % MAX_FRAME_NUMBER;
      const int prev_frame_rx = (absolute_slot / nb_slot_frame) % MAX_FRAME_NUMBER;
      const int prev_hfn_rx = (absolute_slot / nb_slot_frame) / MAX_FRAME_NUMBER;
      int decoded_hfn_rx = prev_hfn_rx;
      if (decoded_frame_rx <= prev_frame_rx)
        decoded_hfn_rx++;
      // we do ++ first in the regular processing, so it will be begin of frame;
      absolute_slot = (decoded_hfn_rx * MAX_FRAME_NUMBER + decoded_frame_rx) * nb_slot_frame - 1;
      if (UE->sl_mode == 2) {
        // Set to the slot where the SL-SSB was decoded
        absolute_slot += UE->SL_UE_PHY_PARAMS.sync_params.slot_offset;
      }
      // With the correct frame and slot numbers, we can now fix the UL timing
      fix_ntn_epoch_hfn(UE, decoded_hfn_rx, decoded_frame_rx);
      if (UE->nrUE_config.ntn_config.params_changed) {
        apply_ntn_config(UE,
                         fp,
                         decoded_hfn_rx,
                         decoded_frame_rx,
                         0,
                         &duration_rx_to_tx,
                         &timing_advance,
                         &ntn_koffset,
                         &ntn_targetcell);
      } else {
        const int abs_subframe_tx = (absolute_slot + 1 + duration_rx_to_tx) / fp->slots_per_subframe;
        apply_ntn_timing_advance_and_doppler(UE, fp, abs_subframe_tx);
        ntn_targetcell = false;
      }
      UE->timing_advance = 0;
      // We have resynchronized, maybe after RF loss so we need to purge any existing context
      memset(tx_wait_for_dlsch, 0, sizeof(tx_wait_for_dlsch));
      for (int i = 0; i < NUM_PROCESS_SLOT_TX_BARRIERS; i++) {
        dynamic_barrier_reset(&UE->process_slot_tx_barriers[i]);
      }
      continue;
    }

    /* check if MAC has sent sync request */
    if (handle_sync_req_from_mac(UE) == 0)
      continue;

    // start of normal case, the UE is in sync
    absolute_slot++;
    TracyCFrameMark;

    // pretend we have 1 iq sample per slot
    // and so nb_slot_frame * 100 iq samples per second (1 frame being 10ms)
    time_manager_iq_samples(1, nb_slot_frame * 100);

    int slot_nr = absolute_slot % nb_slot_frame;
    const bool slot_nr_prev0 = (slot_nr == 0);
    // ---- APPLY A PENDING COARSE REBASE AT A FRAME BOUNDARY (2026-08-06) --------------------
    // Only at slot 0, so the frame/slot/symbol origin is recomputed consistently rather than
    // shifted underneath an in-flight occasion. Discards `delta` samples through the same path
    // acquisition uses (syncInFrame), then clears every accumulated fine-timing state so the CIR
    // loop restarts from the new origin instead of integrating corrections from the old one.
    if (atomic_load_explicit(&nr_ue_pending_rebase_valid, memory_order_relaxed) && slot_nr_prev0) {
      long d = atomic_load_explicit(&nr_ue_pending_rebase_delta, memory_order_relaxed);
      atomic_store_explicit(&nr_ue_pending_rebase_valid, 0, memory_order_relaxed);
      // A LATE window (d < 0) cannot rewind the stream: discard one frame minus |d| instead, and
      // count that frame so frame/slot numbering stays aligned with the air.
      if (d < 0 && -d < (long)fp->samples_per_frame) {
        d += fp->samples_per_frame;
        absolute_slot += nb_slot_frame;
      }
      if (d > 0 && d < (long)fp->samples_per_frame) {
        LOG_W(PHY, "SENSING: REBASE applying coarse timing rebase of %ld samples at frame boundary\n", d);
        syncInFrame(UE, &sync_timestamp, duration_rx_to_tx, (openair0_timestamp_t)d);
        nrue_ru_write_reorder_clear_context(UE);
        nr_rx_continuity_reset(&rx_continuity);
        UE->max_pos_acc = 0;
        UE->max_pos_iir = 0;
        shiftForNextFrame = 0;
        atomic_fetch_add_explicit(&nr_ue_rebase_epoch, 1, memory_order_relaxed);
        LOG_W(PHY, "SENSING: REBASE done; fine-timing state reset (epoch %d)\n",
              atomic_load_explicit(&nr_ue_rebase_epoch, memory_order_relaxed));
      } else {
        LOG_W(PHY, "SENSING: REBASE rejected implausible delta %ld\n", d);
      }
    }

    nr_rxtx_thread_data_t curMsg = {0};
    curMsg.UE=UE;
    // update thread index for received subframe
    curMsg.proc.nr_slot_rx  = slot_nr;
    curMsg.proc.nr_slot_tx  = (absolute_slot + duration_rx_to_tx) % nb_slot_frame;
    curMsg.proc.frame_rx    = (absolute_slot / nb_slot_frame) % MAX_FRAME_NUMBER;
    curMsg.proc.frame_tx    = ((absolute_slot + duration_rx_to_tx) / nb_slot_frame) % MAX_FRAME_NUMBER;
    curMsg.proc.hfn_rx      = (absolute_slot / nb_slot_frame) / MAX_FRAME_NUMBER;
    curMsg.proc.hfn_tx      = ((absolute_slot + duration_rx_to_tx) / nb_slot_frame) / MAX_FRAME_NUMBER;
    if (g_ulprobe_on < 0)
      g_ulprobe_on = (getenv("ISAC_UL_PROBE") != NULL) ? atoi(getenv("ISAC_UL_PROBE")) : 0;
    if (g_ulprobe_on > 0 && UE->received_config_request) {
      /* One shot, the first time a config request has landed: the actual slot-type MAP. Printing
       * the map rather than a yes/no matters -- "received_config_request is true" does not tell you
       * the TDD table was populated, and nr_ue_slot_select() returns NR_DOWNLINK_SLOT for a NULL
       * max_tdd_periodicity_list too, which is indistinguishable from a genuinely all-DL cell. */
      static int s_map_done = 0;
      if (!s_map_done) {
        s_map_done = 1;
        char map[64];
        int mu2 = 0;
        for (int sl = 0; sl < nb_slot_frame && sl < 40; sl++) {
          const int t = nr_ue_slot_select(cfg, sl);
          map[mu2++] = (t == NR_UPLINK_SLOT) ? 'U' : ((t == NR_MIXED_SLOT) ? 'M' : 'D');
        }
        map[mu2] = 0;
        LOG_I(PHY, "SENSING: ULPROBE slot_map(frame)=%s  (D=downlink U=uplink M=mixed)\n", map);
      }
    }
    if (UE->received_config_request) {
      if (UE->sl_mode) {
        curMsg.proc.rx_slot_type = sl_nr_ue_slot_select(sl_cfg, curMsg.proc.nr_slot_rx, TDD);
        curMsg.proc.tx_slot_type = sl_nr_ue_slot_select(sl_cfg, curMsg.proc.nr_slot_tx, TDD);
      } else {
        curMsg.proc.rx_slot_type = nr_ue_slot_select(cfg, curMsg.proc.nr_slot_rx);
        curMsg.proc.tx_slot_type = nr_ue_slot_select(cfg, curMsg.proc.nr_slot_tx);
      }
    }
    else {
      curMsg.proc.rx_slot_type = NR_DOWNLINK_SLOT;
      curMsg.proc.tx_slot_type = NR_DOWNLINK_SLOT;
    }

    int firstSymSamp = get_firstSymSamp(slot_nr, fp);
    c16_t *rxp[fp->nb_antennas_rx];
    for (int i = 0; i < fp->nb_antennas_rx; i++)
      rxp[i] = &UE->common_vars.rxdata[i][firstSymSamp + get_samples_slot_timestamp(fp, slot_nr)];

    int iq_shift_to_apply = 0;
    if (slot_nr == nb_slot_frame - 1) {
      // we shift of half of measured drift, at each beginning of frame for both rx and tx
      iq_shift_to_apply = shiftForNextFrame;
      /* ISAC_SHIFT_CENSUS=<frames> (default off): the CUMULATIVE window motion actually applied.
       * The 2026-09-17 offline analysis could not obtain this. Measured from raw SSB IQ at 217 PRB,
       * the PBCH DM-RS peak ramps away from the FFT window (R^2 = 0.983) until it leaves
       * +-nb_prefix_samples, while at 51 PRB it settles. That ramp is not clock drift -- the LO and
       * the ADC clock share a reference and the residual CFO puts it at ~4.1 ppm -- so the receiver
       * is moving its own window. rx_offset cannot show it (it is a pure function of (slot,symbol)
       * and reads nominal at BOTH widths; an applied shift moves the DATA within the ring, not the
       * index). This census names the source: if the cumulative rate here matches the ramp seen on
       * air, the motion comes through shiftForNextFrame and LOG_TIMEMUT says who wrote it; if it
       * does not, the stream is being moved somewhere other than readBlockSize. */
      {
        static int s_census = -1;
        static long s_cum = 0, s_frames = 0, s_nz = 0;
        if (s_census < 0) {
          const char *e = getenv("ISAC_SHIFT_CENSUS");
          s_census = e ? atoi(e) : 0;
        }
        if (s_census > 0) {
          s_cum += iq_shift_to_apply;
          s_frames++;
          if (iq_shift_to_apply)
            s_nz++;
          if (s_frames % s_census == 0)
            LOG_W(PHY,
                  "SENSING: SHIFTCENSUS frames=%ld applied_cum=%ld mean=%.3f samples/frame "
                  "(%.2f ppm) nonzero=%ld/%ld max_pos_acc=%d shiftForNextFrame=%d\n",
                  s_frames, s_cum, (double)s_cum / s_frames,
                  (double)s_cum / s_frames / (double)fp->samples_per_frame * 1e6,
                  s_nz, s_frames, UE->max_pos_acc, shiftForNextFrame);
        }
      }
      // autonomous timing advance calculation, which does not use SIB19 information
      if (ntn_koffset && get_nrUE_params()->autonomous_ta)
        UE->timing_advance_ntn -= 2 * shiftForNextFrame;
      { const int b = shiftForNextFrame;
        shiftForNextFrame = -round(UE->max_pos_acc * get_nrUE_params()->time_sync_I);
        LOG_TIMEMUT("shiftForNextFrame(perframe)", b, shiftForNextFrame); }
    }

    // Calculate new TA based on SIB19 information for each subframe in NTN mode, if "autonomous_ta" is not enabled
    if (ntn_koffset && !ntn_targetcell && !get_nrUE_params()->autonomous_ta
        && (absolute_slot + duration_rx_to_tx) % fp->slots_per_subframe == 0) {
      const int abs_subframe_tx = (absolute_slot + duration_rx_to_tx) / fp->slots_per_subframe;
      apply_ntn_timing_advance_and_doppler(UE, fp, abs_subframe_tx);
    }

    const int readBlockSize = get_readBlockSize(slot_nr, fp) - iq_shift_to_apply;
    openair0_timestamp_t rx_timestamp;
    int tmp = nrue_ru_read(UE, &rx_timestamp, (void **)rxp, readBlockSize, fp->nb_antennas_rx);
    /* Cancellation has no sample timestamp and must never reach continuity
     * checks or the decoder/sensing consumers. */
    if (oai_exit || tmp < 0)
      break;
    {
      struct timespec diag_ts;
      clock_gettime(CLOCK_REALTIME, &diag_ts);
      atomic_store_explicit(&nr_ue_diag_producer_absolute_slot, absolute_slot, memory_order_relaxed);
      atomic_store_explicit(&nr_ue_diag_producer_wall_ns,
                            (long)diag_ts.tv_sec * 1000000000L + diag_ts.tv_nsec, memory_order_relaxed);
    }
    // ---- RF SAMPLE-STREAM CONTINUITY (2026-08-06) --------------------------------------------
    // Matching software slot counters (the producer/consumer lag check above) prove the PIPELINE
    // is keeping up; they say nothing about whether consecutive reads returned CONSECUTIVE RF
    // samples. The radio's own timestamp does. A gap here means the buffer's contents are not the
    // contiguous signal every downstream time-offset computation assumes -- which would produce
    // exactly "timing arithmetic correct, no SSB in the buffer". Especially suspected right after
    // the post-sync retune / stream re-basing. Bounded so a persistently broken stream cannot
    // flood the log.
    // Accounting note: this iteration may consume MORE than readBlockSize -- the short-read retries
    // just below, and the extra first_symbols read at end-of-frame -- so the expected next
    // timestamp is accumulated in rx_samples_consumed and only committed at the end of the reads.
    static long s_rxts_prev_consumed = 0;
    static long s_rxts_discont_total = 0;
    long rx_samples_consumed = 0;
    if (rx_continuity.valid) {
      const openair0_timestamp_t expected = rx_continuity.next_timestamp;
      if (!nr_rx_continuity_check(&rx_continuity, rx_timestamp)) {
        // ---- RF DISCONTINUITY => INVALIDATE SYNCHRONISATION (2026-08-06) --------------------
        // Measured root cause: at 4x122.88 MS/s the host periodically fails to consume samples
        // fast enough, UHD overflows and the stream resumes SECONDS later. OAI previously advanced
        // its frame counters by the samples it RECEIVED, so the frame/SFN-to-sample mapping silently
        // became wrong and every downstream stage -- CIR tracking, PBCH, PDCCH -- operated on the
        // wrong samples. Nothing detected it: PBCH simply stopped decoding.
        //
        // The discontinuity ITSELF proves the current frame mapping is invalid, so recovery must not
        // wait for PBCH failures or T310. Note the clean read FOLLOWING an overflow carries
        // ERROR_CODE_NONE and must still be treated as discontinuous -- which is why this tests the
        // timestamp rather than the metadata.
        //
        // A multi-second hole is beyond any bounded coarse search, so it forces full reacquisition
        // rather than a widened DMRS anchor: the receiver no longer knows which radio frame its
        // samples belong to. The anchor remains for ordinary coarse errors and must not conceal
        // stream loss. Event-driven, no line cap.
        s_rxts_discont_total++;
        const long long jump = (long long)(rx_timestamp - expected);
        LOG_E(PHY,
              "SENSING: RXDISCONT abs_slot=%d frame=%d slot=%d expected=%llu actual=%llu "
              "delta=%lld prev_consumed=%ld n=%ld -> INVALIDATING SYNC\n",
              absolute_slot, curMsg.proc.frame_rx, curMsg.proc.nr_slot_rx,
              (unsigned long long)expected, (unsigned long long)rx_timestamp, jump,
              s_rxts_prev_consumed, s_rxts_discont_total);
        static int s_disc_invalidate = -1;
        if (s_disc_invalidate < 0)
          s_disc_invalidate = (getenv("ISAC_DISC_NO_RESYNC") && atoi(getenv("ISAC_DISC_NO_RESYNC"))) ? 0 : 1;
        if (s_disc_invalidate && UE->is_synchronized) {
          UE->is_synchronized = 0;
          stream_status = STREAM_STATUS_UNSYNC;
          UE->max_pos_acc = 0;
          UE->max_pos_iir = 0;
          shiftForNextFrame = 0;
          atomic_store_explicit(&nr_ue_pending_rebase_valid, 0, memory_order_relaxed);
          decoded_frame_rx = MAX_FRAME_NUMBER - 1;
          trashed_frames = 0;
          nr_rx_continuity_reset(&rx_continuity);
          LOG_W(PHY, "SENSING: RXDISCONT sync invalidated, timing state cleared, reacquiring\n");
          /* The state tracker's other inputs are all latched discovery state and cannot regress
           * on a stream loss; this edge is the only thing that can tell it the mapping is gone. */
          nr_passive_acq_note_sync_loss();
          if (IS_PASSIVE_RX_MODE(get_softmodem_params()) && nr_cfg_reconf_enabled())
            nr_cfg_epoch_note_continuity_loss_samples(jump, fp->samples_per_subframe);
          /* No RX/TX job has been allocated for this slot yet. Dispatching it would
           * feed invalid samples to discovery and overwrite UNSYNC with SYNCED below. */
          if (IS_PASSIVE_RX_MODE(get_softmodem_params()))
            continue;
        }
      }
    }
    rx_samples_consumed += (tmp > 0) ? tmp : readBlockSize;
    atomic_store_explicit(&nr_ue_diag_rf_timestamp, (long)rx_timestamp, memory_order_relaxed);
    metadata meta = {.slot =  curMsg.proc.nr_slot_rx, .frame =  curMsg.proc.frame_rx};
    UEscopeCopyWithMetadata(UE, ueTimeDomainSamples, rxp[0] - firstSymSamp, sizeof(c16_t), 1, readBlockSize, 0, &meta);
    if (readBlockSize != tmp)
      LOG_W(PHY, "UE slot: got %d of %d RF samples\n", tmp, readBlockSize);
    struct timespec current_time;
    if (clock_gettime(CLOCK_REALTIME, &current_time)) {
      LOG_E(PHY, "clock_gettime failed\n");
    }

    if(slot_nr == (nb_slot_frame - 1)) {
      // read in first symbol of next frame and adjust for timing drift
      int first_symbols = fp->ofdm_symbol_size + fp->nb_prefix_samples0; // first symbol of every frames

      if (first_symbols > 0) {
        openair0_timestamp_t ignore_timestamp;
        int tmp = nrue_ru_read(UE, &ignore_timestamp, (void **)UE->common_vars.rxdata, first_symbols, fp->nb_antennas_rx);
        if (oai_exit || tmp < 0)
          break;
        /* This read MUST be counted. It is an EXTRA read on top of readBlockSize, so leaving it out
         * makes the next iteration's expected timestamp short by exactly first_symbols and the
         * RXDISCONT continuity test above fires on every frame boundary -- a false positive, not an
         * RF fault. Measured after it was lost in the x410-100MHz merge: 11582 discontinuities per
         * 120 s, all with delta = 4448 = ofdm_symbol_size + nb_prefix_samples0 (4096 + 352), i.e.
         * exactly this read, at ~96/s = one per 10 ms radio frame. */
        rx_samples_consumed += (tmp > 0) ? tmp : first_symbols;
        if (first_symbols != tmp)
          LOG_W(PHY, "Next-frame symbol: got %d of %d RF samples\n", tmp, first_symbols);

      } else
        LOG_E(PHY,"can't compensate: diff =%d\n", first_symbols);
    }

    // Every read of this iteration is now counted; commit for the next iteration's continuity test.
    s_rxts_prev_consumed = rx_samples_consumed;
    nr_rx_continuity_commit(&rx_continuity, rx_timestamp, rx_samples_consumed);
    /* Sub-sampled mean |I|+|Q| of this slot, antenna 0. Every 64th sample keeps the cost
     * negligible on the RT thread while still averaging hundreds of points per slot. */
    {
      const int n = (tmp > 0) ? tmp : readBlockSize;
      const int nant = (fp->nb_antennas_rx < CENSUS_MAX_ANT) ? fp->nb_antennas_rx : CENSUS_MAX_ANT;
      double acc = 0.0;
      int cnt = 0;
      for (int i = 0; i < n; i += 64) {
        const int ar = rxp[0][i].r < 0 ? -rxp[0][i].r : rxp[0][i].r;
        const int ai = rxp[0][i].i < 0 ? -rxp[0][i].i : rxp[0][i].i;
        acc += (double)(ar + ai);
        cnt++;
      }
      if (cnt) {
        g_census_pow += acc / cnt;
        g_census_pow_n++;
      }
      /* Same sub-sampling, every antenna. Cost is nant/64 of an add per sample -- negligible beside
       * the FEP, and this runs on the RT thread so it stays a sum of absolute values, not a norm. */
      for (int a2 = 0; a2 < nant; a2++) {
        double acca = 0.0;
        int cnta = 0;
        for (int i = 0; i < n; i += 64) {
          const int ar = rxp[a2][i].r < 0 ? -rxp[a2][i].r : rxp[a2][i].r;
          const int ai = rxp[a2][i].i < 0 ? -rxp[a2][i].i : rxp[a2][i].i;
          acca += (double)(ar + ai);
          cnta++;
        }
        {
          /* Second, independent accumulator: squares, in double, over the SAME sub-sampled window
           * so the two are directly comparable. Kept separate from acca rather than replacing it,
           * to avoid changing a metric other diagnostics are already calibrated against. */
          double accp = 0.0;
          uint64_t clip = 0;
          for (int i = 0; i < n; i += 64) {
            const double xr = (double)rxp[a2][i].r;
            const double xi = (double)rxp[a2][i].i;
            accp += xr * xr + xi * xi;
            if (rxp[a2][i].r >= 32767 || rxp[a2][i].r <= -32768
                || rxp[a2][i].i >= 32767 || rxp[a2][i].i <= -32768) {
              clip++;
            }
            /* ADC headroom (2026-09-16): the X410 saturates well before an int16 full-scale hit
             * registers in `clip`, so track the peak and the fraction of samples above 0.9 FS. */
            const int32_t pk = (int32_t)(xr < 0 ? -xr : xr) > (int32_t)(xi < 0 ? -xi : xi)
                                   ? (int32_t)(xr < 0 ? -xr : xr) : (int32_t)(xi < 0 ? -xi : xi);
            if (pk > g_census_peak_ant[a2]) g_census_peak_ant[a2] = pk;
            if (pk > 29490) g_census_hot_ant[a2]++;
            g_census_smp_ant[a2]++;
          }
          if (cnta) {
            g_census_pw2_ant[a2] += accp / (double)cnta;
            g_census_clip_ant[a2] += clip;
            if (a2 == 0) {
              g_census_pw2_n++;
            }
          }
        }
        if (cnta) {
          g_census_pow_ant[a2] += acca / cnta;
          if (g_ulprobe_on > 0) {
            /* NR_MIXED_SLOT counts as DL here: its UL symbols sit at the END of the slot while this
             * accumulator spans the whole slot, so a mixed slot cannot be attributed cleanly and
             * would only dilute both bins. This cell configures nrofUplinkSymbols=0 anyway. */
            const int ul = (curMsg.proc.rx_slot_type == NR_UPLINK_SLOT) ? 1 : 0;
            g_ulprobe_pow[ul][a2] += acca / cnta;
            if (a2 == 0)
              g_ulprobe_n[ul]++;
          }
        }
      }
      g_census_pow_ant_n++;
    }
    atomic_fetch_add_explicit(&nr_ue_diag_samples_consumed, rx_samples_consumed, memory_order_relaxed);

    // use previous timing_advance value to compute writeTimestamp
    const openair0_timestamp_t writeTimestamp =
        rx_timestamp + get_samples_slot_duration(fp, slot_nr, duration_rx_to_tx) - firstSymSamp - UE->N_TA_offset - timing_advance;

    // Calculate TX deadline, approximately 1 symbol before the first sample should be written
    const uint64_t samples_diff = writeTimestamp - rx_timestamp - fp->ofdm_symbol_size;
    const float deadline_us = samples_diff * 1e3 / fp->samples_per_subframe;
    const uint64_t absolute_deadline_us = current_time.tv_sec * 1e6 + current_time.tv_nsec * 1e-3 + deadline_us;

    // but use current UE->timing_advance value to compute writeBlockSize
    int writeBlockSize = get_samples_per_slot((slot_nr + duration_rx_to_tx) % nb_slot_frame, fp) - iq_shift_to_apply;
    int new_timing_advance = UE->timing_advance + UE->timing_advance_ntn;
    if (new_timing_advance != timing_advance) {
      writeBlockSize -= new_timing_advance - timing_advance;
      timing_advance = new_timing_advance;
    }
    int new_N_TA_offset = determine_N_TA_offset(UE);
    if (new_N_TA_offset != UE->N_TA_offset) {
      LOG_I(PHY, "N_TA_offset changed from %d to %d\n", UE->N_TA_offset, new_N_TA_offset);
      writeBlockSize -= new_N_TA_offset - UE->N_TA_offset;
      UE->N_TA_offset = new_N_TA_offset;
    }
    if (writeBlockSize < 0) {
      timing_advance += writeBlockSize;
      LOG_I(PHY, "writeBlockSize is %d, setting it to 0 and changing timing_advance to %d\n", writeBlockSize, timing_advance);
      writeBlockSize = 0;
    }

    if (curMsg.proc.nr_slot_rx == 0)
      nr_ue_rrc_timer_trigger(UE->Mod_id, curMsg.proc.hfn_rx, curMsg.proc.frame_rx, curMsg.proc.gNB_id);

    // RX slot processing. We launch and forget.
    notifiedFIFO_elt_t *newRx = newNotifiedFIFO_elt(sizeof(nr_rxtx_thread_data_t), curMsg.proc.nr_slot_tx, NULL, UE_dl_processing);
    nr_rxtx_thread_data_t *curMsgRx = (nr_rxtx_thread_data_t *)NotifiedFifoData(newRx);
    *curMsgRx = (nr_rxtx_thread_data_t){.proc = curMsg.proc, .UE = UE};
    int ret = UE_dl_preprocessing(UE, &curMsgRx->proc, tx_wait_for_dlsch, &curMsgRx->phy_data, &stats_printed);
    if (ret != INT_MAX) {
      /* ISAC_PBCH_SHIFT_CLAMP=<samples> (default 0 = off): the PBCH-based timing step is read off a
       * peaky multipath CIR and jumps +/-50 samples per SSB (TSYNC_OBS corr_pos swings +/-200) while
       * the true drift is ~6 samples/frame (5 ppm SFO; the per-frame estimator reads 5-8). Unclamped,
       * the loop walks off the CP in ~30 % of starts ("timing runaway"). Clamp the step, keep the sign. */
      static int s_clamp = -1;
      if (s_clamp < 0) {
        const char *e = getenv("ISAC_PBCH_SHIFT_CLAMP");
        s_clamp = (e != NULL) ? atoi(e) : 0;
      }
      if (s_clamp > 0) {
        if (ret > s_clamp) ret = s_clamp;
        if (ret < -s_clamp) ret = -s_clamp;
      }
      const int b = shiftForNextFrame;
      shiftForNextFrame = ret;
      LOG_TIMEMUT("shiftForNextFrame(pbch)", b, shiftForNextFrame);
    }

    /* ---- RF STALL WATCHDOG + census ---------------------------------------------------------
     * ROOT CAUSE THIS EXISTS FOR, measured 2026-08-23 on the live n78 cell. The X410 receive path
     * stops delivering signal at a random point in a run: the mean |I|+|Q| of the raw time-domain
     * slot buffer drops from ~70-81 to a PINNED 4.90-4.93 (constant to two decimals for tens of
     * seconds -- a fixed level, not a fade) and NEVER recovers. It is invisible to everything that
     * already exists here: UHD reports no error, reads return full length, and the RF timestamps
     * stay perfectly continuous, so neither RFTSDISC (usrp_lib.cpp, uncapped) nor the RXDISCONT
     * path above ever fires. Downstream nothing looks broken either -- the blind PDCCH monitor
     * keeps emitting noise decodes (5129 distinct RNTIs over 5317 accepts) -- but real DCI accepts
     * for the live C-RNTI go to EXACTLY zero for the remainder of the run.
     *
     * That failure, not any property of the CORESET, is what PASSIVE_RX_ONLY_HANDOVER.md section
     * 10.5 measured as "2-3 % catch rate on the dedicated CORESET". Across 6 runs the catch rate
     * is predicted by the healthy fraction alone at ~0.70 x healthy: 7 % healthy -> 4.6 % catch,
     * 35 % -> 22.9 %, 66 % -> 49.1 %, 100 % -> 69.6/75.0 %. A receiver that stays healthy sustains
     * ~70 %, and one 200 s run did so end to end.
     *
     * Keyed on RF POWER and nothing else. An earlier version of this watchdog keyed on "PBCH
     * decoded" via UE_dl_preprocessing()'s return and could never fire: that return is set for
     * every SSB slot PROCESSED, not for a successful decode, so it keeps resetting the counter
     * straight through the stall. Power off the raw buffer is measured before any alignment,
     * demodulation or gating, so it cannot be confounded by them.
     *
     * The reference is deliberately NOT a plain EMA of recent power: an EMA follows the signal
     * down into the stall and then reports it as the new normal (the same self-referential trap
     * that made a median-normalised offline detector report "no collapse" for a run that was
     * collapsed for 93 % of its length). It is updated ONLY from windows that are still within
     * 0.5x of it, so a genuine stall can never move it.
     *
     * Recovery mirrors the RXDISCONT block above verbatim -- same fields, same order -- so there
     * is one invalidate-and-reacquire path rather than two that can drift apart. max_pos_acc is
     * re-seeded from ntn_init_time_drift by the !UE->is_synchronized branch, so clearing it here
     * is correct.
     */
    {
      static int s_wd_on = -1;
      static double s_ref;      /* healthy reference power; never follows the signal downward */
      static int s_bad;         /* consecutive collapsed census windows */
      static long s_fires;
      if (s_wd_on < 0) {
        const char *e = getenv("ISAC_RF_STALL_WATCHDOG");
        s_wd_on = e ? atoi(e) : 1; /* 0 disables */
      }
      if (ret != INT_MAX)
        g_census_ssb_slots++;
      if (++g_census_slots >= 2000) { /* 2000 slots = 100 frames = 1 s at mu=1 */
        const double w = g_census_pow_n ? g_census_pow / g_census_pow_n : -1.0;
        extern void nr_pbch_diag_counts(unsigned long *ok, unsigned long *fail);
        unsigned long pbch_ok = 0, pbch_fail = 0;
        nr_pbch_diag_counts(&pbch_ok, &pbch_fail);
        bool rf_collapsed = false, pbch_dead = false;
        if (w >= 0.0) {
          /* Running max with a hard-bounded fall. The previous form ("EMA, but only from windows
           * within 0.5x of the reference") was NOT safe: it permits a GEOMETRIC RATCHET DOWNWARD
           * -- 77 -> 40 -> 37 -> ... each step legal -- and MEASURED it did exactly that, ending
           * at ref=14.13 while rf_pow sat at 4.90, so the collapse never tripped the 0.25x test.
           * Rise instantly to any new healthy level; fall at most 0.1 %/window, which is far
           * slower than the ~1 s the detector needs, so a real stall can never drag the reference
           * into itself while still tracking genuine slow gain/channel drift. */
          if (s_ref <= 0.0)
            s_ref = w;
          else if (w > s_ref)
            s_ref = w;
          else
            s_ref = (w > s_ref * 0.999) ? w : s_ref * 0.999;
          /* Two independent loss-of-lock signatures, both MEASURED on this cell:
           *   - RF stall: the X410 stops delivering signal, power collapses to a pinned ~4.9.
           *   - Timing runaway: power stays healthy (75-87) while max_pos_acc diverges
           *     (439 -> 1394 and climbing) and the FFT window walks off the signal.
           * Only the second is invisible to the power test, and PBCH failure is common to both --
           * so a window in which NOT ONE SSB decoded is the general detector. A healthy receiver
           * decodes every SSB (50/s at this cell's 20 ms period), so zero is unambiguous. */
          rf_collapsed = (s_ref > 0.0 && w < 0.25 * s_ref);
          pbch_dead = (pbch_ok == 0 && pbch_fail > 0);
          /* TRIGGER ON PBCH ONLY. rf_pow alone FALSE-POSITIVES and was MEASURED doing so: the
           * running-max reference latches any transient startup spike (observed ref=155 while the
           * run's steady level was 16.5), after which perfectly normal operation sits below the
           * 0.25x test and the watchdog kills a HEALTHY capture -- seen with pbch_ok=50
           * pbch_fail=0 for all 77 windows, i.e. every SSB decoding. The absolute level also
           * varies several-fold between runs, so no reference derived from it is trustworthy.
           * In EVERY genuine stall pbch_ok was 0, so PBCH already catches them and the power test
           * contributes only false alarms. rf_pow is still measured and REPORTED, because it is
           * what distinguishes an RF stream stall from a timing runaway once the trigger fires. */
          if (pbch_dead)
            s_bad++;
          else
            s_bad = 0;

          /* Freeze the timing integrator while there is no signal to integrate -- but ONLY once
           * synchronised.
           *
           * The is_synchronized guard is load-bearing and was learned the hard way: during initial
           * acquisition pbch_dead is true BY DEFINITION (nothing has decoded yet), and s_ref can
           * already have latched a startup level, so the freeze engaged before the first lock and
           * stopped the loop from ever converging. Runs went from 31-81 s down to 3-34 s. The
           * freeze exists for an outage AFTER a working lock, which is the only situation where
           * "hold the last good correction" is the right answer; before a lock there is no good
           * correction to hold and the loop must be free to search.
           *
           * rf_collapsed is deliberately NOT a trigger here -- the comment above records it
           * false-positiving on a latched reference -- only a modifier. Both of its effects
           * (suppress integration, buy patience) are one-directional and cannot end a healthy
           * capture on their own. */
          atomic_store_explicit(&nr_ue_rf_signal_absent,
                                (rf_collapsed && pbch_dead && UE->is_synchronized) ? 1 : 0,
                                memory_order_relaxed);
        }
        LOG_I(PHY,
              "SENSING: RFCENSUS slots=%ld ssb_slots=%ld pbch_ok=%lu pbch_fail=%lu rf_pow=%.2f "
              "ref=%.2f bad=%d shiftForNextFrame=%d max_pos_acc=%d frame=%d\n",
              g_census_slots, g_census_ssb_slots, pbch_ok, pbch_fail, w, s_ref, s_bad,
              shiftForNextFrame, UE->max_pos_acc, curMsg.proc.frame_rx);
        /* ANTPOW: raw per-antenna receive power and its dB spread, to be read ALONGSIDE RXBRANCH's
         * pw[]. If these are flat and pw[] is not, the imbalance is in the estimation path, not the
         * antennas. */
        if (g_census_pw2_n > 0) {
          /* RFPOW: absolute per-branch level, no normalisation. Full scale for a complex int16
           * sample pair is 2*32767^2. */
          const double fs = 2.0 * 32767.0 * 32767.0;
          char rb[320];
          size_t u = 0;
          const int na = (UE->frame_parms.nb_antennas_rx < CENSUS_MAX_ANT)
                             ? UE->frame_parms.nb_antennas_rx : CENSUS_MAX_ANT;
          for (int a2 = 0; a2 < na && u < sizeof(rb) - 48; a2++) {
            const double mp = g_census_pw2_ant[a2] / (double)g_census_pw2_n;
            const double dbfs = (mp > 0.0) ? 10.0 * log10(mp / fs) : -199.0;
            /* Absolute RF input power where the device can supply the calibration mapping. This is
             * the ONLY number here that is an RF-side quantity; dBFS alone cannot separate a quiet
             * antenna from a gain that never landed. NaN prints as "--", never as 0 dBm. */
            const double ref = nr_ue_rx_power_reference_dbm(a2);
            if (isnan(ref)) {
              u += snprintf(rb + u, sizeof(rb) - u, "ch%d=%.2fdBFS(rf=--,clip=%lu) ", a2, dbfs,
                            (unsigned long)g_census_clip_ant[a2]);
            } else {
              u += snprintf(rb + u, sizeof(rb) - u, "ch%d=%.2fdBFS(rf=%.2fdBm,clip=%lu) ", a2, dbfs,
                            dbfs + ref, (unsigned long)g_census_clip_ant[a2]);
            }
          }
          LOG_I(PHY, "SENSING: RFPOW mean(I^2+Q^2) absolute, absolute; rf= is TRUE RF input power via the UHD power reference, -- when uncalibrated: %s\n", rb);
          {
            char pb[200];
            size_t v = 0;
            for (int a2 = 0; a2 < na && v < sizeof(pb) - 40; a2++)
              v += snprintf(pb + v, sizeof(pb) - v, "ch%d=%.1fdBFS(hot=%.4f%%) ", a2,
                            g_census_peak_ant[a2] > 0 ? 20.0 * log10(g_census_peak_ant[a2] / 32767.0) : -199.0,
                            g_census_smp_ant[a2] ? 100.0 * (double)g_census_hot_ant[a2] / (double)g_census_smp_ant[a2] : 0.0);
            LOG_I(PHY, "SENSING: ADCPEAK max|I|,|Q| since start; hot = samples above 0.9 full scale: %s\n", pb);
          }

          /* ---- BRSNR: per-branch signal-to-noise, from the only valid noise window available ----
           * NOT from nr_dl_chest_nvar_ant[]. That array is |dl_ls_est - dl_ch|^2 -- the residual
           * between the raw LS estimate and the FILTERED one -- so it is noise PLUS filter
           * mismatch, and the mismatch term scales with |H|. Using it as an SNR denominator gives a
           * denominator that tracks its own numerator: measured pw/resid = 2.3 on three branches at
           * once while raw power said those branches clearly differed. It cannot separate a branch
           * that is quiet from a branch that is noisy, which is the entire question here.
           *
           * The gNB does not transmit in the UL slots of this TDD pattern, so UL-slot receive power
           * is a genuine per-branch NOISE FLOOR, measured on the same samples and the same scale as
           * the DL power directly above it. SNR is then (P_dl - P_ul) / P_ul per branch.
           *
           * CONTAMINATION, stated rather than hidden: the served UE transmits in those UL slots, so
           * P_ul is noise PLUS whatever of that UE's uplink reaches this receiver. That inflates
           * P_ul and makes this an SNR LOWER BOUND. It is common-mode across branches, so the
           * RELATIVE ordering between branches -- which is what the imbalance question needs -- is
           * far more trustworthy than the absolute value. Requires ISAC_UL_PROBE=1.
           * The two accumulators use the same sub-sampling and the same window, so no rescaling. */
          if (g_ulprobe_on > 0 && g_ulprobe_n[0] > 0 && g_ulprobe_n[1] > 0) {
            char sb[300];
            size_t v = 0;
            for (int a2 = 0; a2 < na && v < sizeof(sb) - 40; a2++) {
              const double pdl = g_ulprobe_pow[0][a2] / (double)g_ulprobe_n[0];
              const double pul = g_ulprobe_pow[1][a2] / (double)g_ulprobe_n[1];
              /* These are mean(|I|+|Q|) amplitudes, so square before forming a power ratio. */
              const double sdl = pdl * pdl, sul = pul * pul;
              const double snr = (sul > 0.0 && sdl > sul) ? 10.0 * log10((sdl - sul) / sul) : -99.0;
              v += snprintf(sb + v, sizeof(sb) - v, "ch%d=%.1fdB(n=%.0f) ", a2, snr, pul);
            }
            if (v > 0) {
              LOG_I(PHY,
                    "SENSING: BRSNR per-branch SNR lower bound from UL-slot noise floor "
                    "(NOT from chest nvar, which is invalid here); n= is the noise amplitude: %s\n",
                    sb);
            }
          }
        }
        if (g_census_pow_ant_n > 0) {
          double pa[CENSUS_MAX_ANT], mx = 0.0;
          const int nant = (UE->frame_parms.nb_antennas_rx < CENSUS_MAX_ANT) ? UE->frame_parms.nb_antennas_rx
                                                                            : CENSUS_MAX_ANT;
          for (int a2 = 0; a2 < nant; a2++) {
            pa[a2] = g_census_pow_ant[a2] / (double)g_census_pow_ant_n;
            if (pa[a2] > mx)
              mx = pa[a2];
          }
          if (mx <= 0.0)
            mx = 1.0;
          LOG_I(PHY,
                "SENSING: ANTPOW raw=[%.2f %.2f %.2f %.2f] dB=[%.1f %.1f %.1f %.1f] (raw |I|+|Q| off "
                "the time-domain buffer, BEFORE any alignment -- compare against RXBRANCH pw[])\n",
                pa[0], nant > 1 ? pa[1] : 0.0, nant > 2 ? pa[2] : 0.0, nant > 3 ? pa[3] : 0.0,
                /* 20*log10, NOT 10: this accumulates mean(|I|+|Q|), an AMPLITUDE, whereas RXBRANCH's
                 * pw[] is |h|^2, a POWER. Printing an amplitude ratio with 10*log10 halves it, which
                 * made the raw imbalance look like -9 dB when it is -18 dB and made the estimate look
                 * like it was inventing 10 dB of extra spread that it was not. */
                20.0 * log10((pa[0] > 0 ? pa[0] : 1e-9) / mx),
                nant > 1 ? 20.0 * log10((pa[1] > 0 ? pa[1] : 1e-9) / mx) : 0.0,
                nant > 2 ? 20.0 * log10((pa[2] > 0 ? pa[2] : 1e-9) / mx) : 0.0,
                nant > 3 ? 20.0 * log10((pa[3] > 0 ? pa[3] : 1e-9) / mx) : 0.0);
        }
        if (g_ulprobe_on > 0) {
          const int nantu = (UE->frame_parms.nb_antennas_rx < CENSUS_MAX_ANT) ? UE->frame_parms.nb_antennas_rx
                                                                              : CENSUS_MAX_ANT;
          double d[CENSUS_MAX_ANT] = {0}, u[CENSUS_MAX_ANT] = {0};
          for (int a2 = 0; a2 < nantu; a2++) {
            d[a2] = g_ulprobe_n[0] ? g_ulprobe_pow[0][a2] / (double)g_ulprobe_n[0] : 0.0;
            u[a2] = g_ulprobe_n[1] ? g_ulprobe_pow[1][a2] / (double)g_ulprobe_n[1] : 0.0;
          }
          /* ul_slots=0 with cfg_req=1 would mean the TDD table says this cell has no UL slot;
           * ul_slots=0 with cfg_req=0 means the receiver never learned the pattern and is treating
           * every slot as DL. Completely different problems, so both numbers are printed. */
          LOG_I(PHY,
                "SENSING: ULPROBE cfg_req=%d dl_slots=%ld ul_slots=%ld dl_pow=[%.2f %.2f %.2f %.2f] "
                "ul_pow=[%.2f %.2f %.2f %.2f] ul_minus_dl_dB=[%.1f %.1f %.1f %.1f]\n",
                UE->received_config_request ? 1 : 0, g_ulprobe_n[0], g_ulprobe_n[1],
                d[0], d[1], d[2], d[3], u[0], u[1], u[2], u[3],
                (d[0] > 0 && u[0] > 0) ? 20.0 * log10(u[0] / d[0]) : -99.0,
                (d[1] > 0 && u[1] > 0) ? 20.0 * log10(u[1] / d[1]) : -99.0,
                (d[2] > 0 && u[2] > 0) ? 20.0 * log10(u[2] / d[2]) : -99.0,
                (d[3] > 0 && u[3] > 0) ? 20.0 * log10(u[3] / d[3]) : -99.0);
          for (int a2 = 0; a2 < CENSUS_MAX_ANT; a2++) {
            g_ulprobe_pow[0][a2] = 0.0;
            g_ulprobe_pow[1][a2] = 0.0;
          }
          g_ulprobe_n[0] = 0;
          g_ulprobe_n[1] = 0;
        }
        g_census_slots = 0;
        g_census_ssb_slots = 0;
        g_census_pow = 0.0;
        g_census_pow_n = 0;
        for (int a2 = 0; a2 < CENSUS_MAX_ANT; a2++) {
          g_census_pow_ant[a2] = 0.0;
          g_census_pw2_ant[a2] = 0.0;
          g_census_clip_ant[a2] = 0;
        }
        g_census_pow_ant_n = 0;
        g_census_pw2_n = 0;
        /* Two consecutive windows (2 s) before acting: one window is enough to be sure given how
         * far apart the two levels sit, but the stall is permanent and a spurious reacquisition
         * costs a real capture gap, so require it to persist. */
        /* PATIENCE: 8 windows for BOTH faults.
         *
         * All four branches at the noise floor is a STREAM OUTAGE, and measured 2026-09-01 those
         * recover ON THEIR OWN after 3-4 s (17 of 122 census windows deaf across a run, always
         * followed by a full return to pbch_ok=50/50). Killing at 2 windows threw away captures
         * that were about to come back -- the receiver was ending the run over a transient it would
         * have survived. With the integrator frozen above there is nothing to gain by acting fast,
         * so wait long enough to let the outage clear.
         *
         * Signal present but no SSB decoding used to be treated as a timing runaway and acted on
         * after 2, on the premise that it was the ONLY way to get pbch_ok=0 with signal present.
         * Measured 2026-09-17 it is not: with the SSB beam the scan happened to acquire sitting at
         * the PBCH decode edge (1-3 of 50 per window, then 0), the receiver was killed while its
         * timing integrator was stable (max_pos_acc -3..-36, PBCH-derived steps of a few samples),
         * SIB1 had decoded twice and 17k CSI-RS confirmations were flowing -- a working receiver,
         * ended over a marginal broadcast beam. The integrator is frozen while PBCH is dead and the
         * per-frame loop keeps applying its held drift estimate, so nothing walks off in the extra
         * windows; a genuine runaway still trips after 8 (16 s), one reacquisition later than
         * before, while a beam that decodes even one SSB per window resets the count and survives. */
        const int bad_needed = 8;
        if (s_wd_on && s_bad >= bad_needed && UE->is_synchronized) {
          s_fires++;
          LOG_E(PHY,
                "SENSING: RFSTALL %s (rf_pow=%.2f ref=%.2f pbch_ok=%lu pbch_fail=%lu "
                "max_pos_acc=%d) for %d windows (frame=%d) n=%ld -> reacquiring\n",
                rf_collapsed ? "RF stream stalled" : "PBCH lock lost (timing runaway)",
                w, s_ref, pbch_ok, pbch_fail, UE->max_pos_acc, s_bad,
                curMsg.proc.frame_rx, s_fires);
          /* Re-acquisition ALONE does not work here, measured: after invalidating sync the UE made
           * 52 consecutive failed initial-sync attempts, because there is no signal to acquire --
           * the stream itself is stalled, not merely mis-aligned. The RX stream must be torn down
           * and restarted at the device before any re-sync can succeed. */
          if (1) { /* stream-level restart never succeeded in any observed case; go straight to re-init */
            /* A stream-level restart is not enough when the MPM claim itself is gone: every RPC
             * to the device throws (measured: set_gpio_src, then get_timekeeper_time at a 2000 ms
             * timeout). Escalate to a full teardown + re-open, which releases the stale claim and
             * takes a fresh one. Bounded, so a device that is genuinely gone cannot spin here. */
            /* Cap is generous on purpose. MEASURED: this device can stall 8 times in 42 s, and
             * each re-init recovers it with the real-accept rate staying flat across the gap --
             * so giving up after a handful throws away a capture that was still working. The cap
             * exists only so a permanently dead device cannot spin forever. Override with
             * ISAC_RF_STALL_MAX_REINIT. */
            /* IN-PROCESS RE-INIT IS OFF BY DEFAULT. It demonstrably restores the RF stream and
             * PDCCH decoding -- real DCI accepts resume at full rate across the gap -- but whether
             * PDSCH decoding survives it is NOT established. On one run whose re-init landed 15 %
             * in, pdsch crc_ok froze at 726 for the remainder (84.7 % -> 10.1 % cumulative), which
             * looked causal; it is not safe to read it that way, because a change in the SERVED
             * UE's reported PMI produces the identical signature and was not ruled out. (Measured
             * separately: PDSCH is precoded toward the served UE while PDCCH carries no pm_index
             * at all, so a PMI change can null PDSCH at a passive receiver while leaving PDCCH at
             * 84 % -- observed exactly, pm_index moving 3 -> 15/19 after a UE re-attach.)
             * Defaulting to exit is the safe choice under that ambiguity: a capture is then either
             * good or absent, never silently half-working. Set ISAC_RF_STALL_MAX_REINIT > 0 to opt
             * in, and re-test the PDSCH question with the PMI held constant before trusting it. */
            static int s_reinits;
            static int s_reinit_cap = -1;
            if (s_reinit_cap < 0) {
              /* DEFAULT NOW 4, was 0. The open question that kept this off -- whether PDSCH
               * decoding SURVIVES a re-init -- was answered 2026-08-26 at ~160 grants/s, where CRC
               * is a stable 91 % and a post-recovery freeze would be unambiguous: 4/4 recovered runs
               * kept decoding (crc_ok grew 749->28547, 833->2447, 4168->7683, 739->1782), and dwell
               * went from active_s=8 (run dies at the stall) to 50-66 s. Set 0 to restore exit-on-
               * stall. */
              const char *e = getenv("ISAC_RF_STALL_MAX_REINIT");
              s_reinit_cap = e ? atoi(e) : 4;
            }
            if (s_reinits < s_reinit_cap && nrue_ru_reinit() == 0) {
              s_reinits++;
              LOG_W(PHY, "SENSING: RFSTALL recovered by full device re-init (n=%d)\n", s_reinits);
            } else {
              /* Exiting is the correct outcome. Every sample from here on is a constant near-zero
               * level that still produces plausible-looking noise decodes, so continuing would
               * silently corrupt the capture -- which is exactly how this fault stayed hidden. A
               * supervisor can restart the run; a contaminated result cannot be fixed afterwards. */
              LOG_E(PHY, "SENSING: RFSTALL -- stopping capture (exit 3); releasing radio before exit\n");
              /* This is the RX owner, outside trx_read_func(). Stop accepting
               * work and release UHD handles before exiting. exit(3) alone
               * skips destruction of the heap-owned radio and its streamers.
               * Do not use exit_function(): it converts normal exits to zero. */
              oai_exit = 1;
              nrue_ru_stop();
              nrue_ru_end();
              LOG_I(PHY, "USRP_RFSTALL_CLEANUP_COMPLETE exit=3\n");
              exit(3);
            }
          }
          UE->is_synchronized = 0;
          stream_status = STREAM_STATUS_UNSYNC;
          UE->max_pos_acc = 0;
          UE->max_pos_iir = 0;
          shiftForNextFrame = 0;
          atomic_store_explicit(&nr_ue_pending_rebase_valid, 0, memory_order_relaxed);
          decoded_frame_rx = MAX_FRAME_NUMBER - 1;
          trashed_frames = 0;
          s_bad = 0;
        }
      }
    }

    /* ---- CFO TRIM: retune + clean re-acquisition, requested by the trim loop in pbch_process().
     * Done HERE because this scope owns stream_status / shiftForNextFrame / the rebase flag, which
     * an in-place retune from the PBCH path leaves stale -- measured as a run dying at active_s=4
     * immediately after an otherwise CORRECT correction (GT_r4, proposal -14968 vs a true -14390).
     * Re-acquiring with the radio already on the corrected frequency means the fresh acquisition
     * measures a near-zero offset, which is the state a healthy run starts from anyway. */
    if (atomic_load_explicit(&nr_ue_cfo_resync_request, memory_order_acquire)) {
      atomic_store_explicit(&nr_ue_cfo_resync_request, 0, memory_order_relaxed);
      uint64_t dl_carrier = 0, ul_carrier = 0;
      nr_get_carrier_frequencies(UE, &dl_carrier, &ul_carrier);
      /* DELIVERY VIA FULL DEVICE RE-INIT, not a bare retune.
       * MEASURED 2026-08-26: retuning under a running stream and clearing is_synchronized produced
       * CORRECT corrections (-13677->-14310, -13051->-14293, -15723->-14312, all within ~100 Hz of
       * the true -14390) and the receiver never came back -- 3/3 runs died at active_s=4. The
       * lighter path does not recover on this hardware.
       * The stall-recovery path DOES: nrue_ru_reinit() tears the device down and re-opens it, and
       * 4/4 runs resumed decoding afterwards (grew=YES, one going 749 -> 28547 crc_ok). So the
       * correction is delivered the way that is already proven to survive on this X410, rather than
       * the way that is cheaper. */
      LOG_W(PHY, "SENSING: CFOTRK retuning %d -> %d Hz and re-initialising the device\n",
            UE->common_vars.freq_offset, nr_ue_cfo_resync_hz);
      nrue_ru_set_freq(UE, ul_carrier, dl_carrier, nr_ue_cfo_resync_hz);
      UE->common_vars.freq_offset = nr_ue_cfo_resync_hz;
      UE->initial_fo = nr_ue_cfo_resync_hz; /* the seed the comment below demands; was MISSING */
      /* SEED THE RE-ACQUISITION WITH THE CORRECTION WE JUST APPLIED.
       * Without this the fix silently undoes itself. MEASURED (F2_r7): acquisition #1 gave -20234,
       * the loop correctly retuned to -15066, and the re-acquisition then measured -1176 -- the
       * RESIDUAL left on an already-corrected radio -- and UE_synch applied that -1176 as the TOTAL
       * via its unconditional nrue_ru_set_freq(). The radio went from nearly right back to ~14 kHz
       * off and the run decoded 0.0 %, with the trim loop afterwards proposing nonsense (-844/-691/
       * -741) because common_vars.freq_offset no longer described the hardware.
       * nr_initial_sync() seeds its accumulator from ue->initial_fo (`ssbInfo->freqOffset =
       * ue->initial_fo`), so setting it here makes the next acquisition ACCUMULATE onto the applied
       * correction (-15066 + -1176 = -16242) instead of replacing it. That accumulator is known to
       * converge: seeding +6000 Hz took it back to within 34-276 Hz on 3/3 runs. */
      if (nrue_ru_reinit() != 0) {
        LOG_E(PHY, "SENSING: CFOTRK device re-init FAILED; the offset is set but the stream may not recover\n");
      }
      UE->is_synchronized = 0;
      stream_status = STREAM_STATUS_UNSYNC;
      UE->max_pos_acc = 0;
      UE->max_pos_iir = 0;
      shiftForNextFrame = 0;
      atomic_store_explicit(&nr_ue_pending_rebase_valid, 0, memory_order_relaxed);
      decoded_frame_rx = MAX_FRAME_NUMBER - 1;
      trashed_frames = 0;
    }
    if (get_nrUE_params()->num_dl_actors > 0) {
      pushNotifiedFIFO(&UE->dl_actors[curMsg.proc.nr_slot_rx % get_nrUE_params()->num_dl_actors].fifo, newRx);
    } else {
      newRx->processingFunc(curMsgRx);
    }

    // apply new NTN timing information
    apply_ntn_config(UE,
                     fp,
                     curMsg.proc.hfn_rx,
                     curMsg.proc.frame_rx,
                     curMsg.proc.nr_slot_rx,
                     &duration_rx_to_tx,
                     &timing_advance,
                     &ntn_koffset,
                     &ntn_targetcell);

    // Start TX slot processing here. It runs in parallel with RX slot processing
    // in current code, DURATION_RX_TO_TX constant is the limit to get UL data to encode from a RX slot
    notifiedFIFO_elt_t *newTx = newNotifiedFIFO_elt(sizeof(nr_rxtx_thread_data_t), 0, 0, processSlotTX);
    nr_rxtx_thread_data_t *curMsgTx = NotifiedFifoData(newTx);
    memset(curMsgTx, 0, sizeof(*curMsgTx));
    curMsgTx->proc = curMsg.proc;
    curMsgTx->writeBlockSize = writeBlockSize;
    curMsgTx->proc.timestamp_tx = writeTimestamp;
    curMsgTx->UE = UE;
    curMsgTx->absolute_deadline_us = absolute_deadline_us;

    int slot = curMsgTx->proc.nr_slot_tx;
    int slot_and_frame = slot + curMsgTx->proc.frame_tx * nb_slot_frame;
    int next_tx_slot_and_frame = absolute_slot + duration_rx_to_tx + 1;
    int wait_for_prev_slot = stream_status == STREAM_STATUS_SYNCED ? 1 : 0;

    dynamic_barrier_t *next_barrier = &UE->process_slot_tx_barriers[next_tx_slot_and_frame % NUM_PROCESS_SLOT_TX_BARRIERS];
    curMsgTx->next_barrier = next_barrier;
    dynamic_barrier_update(&UE->process_slot_tx_barriers[slot_and_frame % NUM_PROCESS_SLOT_TX_BARRIERS],
                           tx_wait_for_dlsch[slot] + wait_for_prev_slot,
                           start_process_slot_tx,
                           newTx);
    if (UE->is_synchronized)
      stream_status = STREAM_STATUS_SYNCED;
    tx_wait_for_dlsch[slot] = 0;
  }
  LOG_W(NR_PHY, "UE main thread is ending\n");
  return NULL;
}

void init_NR_UE(int nb_inst, char *uecap_file, char *reconfig_file, char *rbconfig_file, int numerology)
{
  for (int instance_id = 0; instance_id < nb_inst; instance_id++) {
    NR_UE_RRC_INST_t* rrc = nr_rrc_init_ue(uecap_file, instance_id, get_nrUE_params()->nb_antennas_tx);
    NR_UE_MAC_INST_t *mac = nr_l2_init_ue(instance_id, numerology);

    nr_rrc_set_mac_queue(instance_id, &mac->input_nf);
    mac->if_module = nr_ue_if_module_init(instance_id);
    AssertFatal(mac->if_module, "can not initialize IF module\n");
    if (!IS_SA_MODE(get_softmodem_params()) && !get_softmodem_params()->sl_mode) {
      init_nsa_message(rrc, reconfig_file, rbconfig_file);
      nr_rlc_activate_srb0(mac->crnti, NULL, send_srb0_rrc);
    }
    //TODO: Move this call to RRC
    start_sidelink(instance_id);
  }
}

void init_NR_UE_threads(PHY_VARS_NR_UE *UE) {
  char thread_name[16];
  sprintf(thread_name, "UEthread_%d", UE->Mod_id);
  /* ---- PIN THE PHY RECEIVE THREAD (ISAC_UE_RT_CORE, default -1 = unpinned, as before) ---------
   * This thread carries the hard real-time deadline: miss it and the timing loop loses PBCH lock,
   * which is the measured mechanism behind the grant-rate-driven PDSCH CRC collapse
   * (BRANCH_IMBALANCE_HARQ_PLAN.md section 12 -- 8/8 healthy at 165 grants/s, 17/32 dead at ~1500).
   *
   * It has always been created UNPINNED. RT priority alone does not protect it: unpinned, it is
   * free to be migrated onto whichever core the scheduler likes, including the ones already
   * saturated by --thread-pool (0,1,4-7), the PDSCH decode consumers (9-13) and the blind-PDCCH
   * scan consumer. Every migration also costs its working set.
   *
   * MEASURED on this host: cores 2-3 are ISOLATED for exactly this purpose --
   * `isolcpus=domain,managed_irq,2-3` and `nohz_full=2-3`, so no ordinary task is scheduled there
   * and the timer tick is off -- and nothing was using them. An explicitly pinned thread still runs
   * on an isolated core; that is what isolation is for.
   *
   * Left OFF by default because the right core is host-specific and pinning to a BUSY core would be
   * worse than not pinning at all. Set ISAC_UE_RT_CORE to an isolated core (2 or 3 here) to opt in;
   * check /sys/devices/system/cpu/isolated before choosing. */
  int rt_core = -1;
  {
    const char *e = getenv("ISAC_UE_RT_CORE");
    if (e != NULL) {
      rt_core = atoi(e);
      LOG_I(PHY, "SENSING: pinning the PHY receive thread to core %d (ISAC_UE_RT_CORE)\n", rt_core);
    }
  }
  threadCreate(&UE->main_thread, UE_thread, (void *)UE, thread_name, rt_core, OAI_PRIORITY_RT_MAX);
  if (!IS_SOFTMODEM_NOSTATS) {
    sprintf(thread_name, "L1_UE_stats_%d", UE->Mod_id);
    threadCreate(&UE->stat_thread, nrL1_UE_stats_thread, UE, thread_name, -1, OAI_PRIORITY_RT_LOW);
  }
}
