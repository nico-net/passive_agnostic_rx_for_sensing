/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "PHY/defs_gNB.h"
#include "PHY/defs_nr_common.h"
#include <math.h>
#include "modulation_UE.h"
#include "nr_modulation.h"
#include "PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include <common/utils/LOG/log.h>

/* rxdataF should be 16 bytes aligned */
void nr_symbol_fep(const NR_DL_FRAME_PARMS *frame_parms,
                   const int slot,
                   const unsigned char symbol,
                   const int link_type,
                   c16_t *rxdata[frame_parms->nb_antennas_rx],
                   c16_t *rxdataF[frame_parms->nb_antennas_rx],
                   time_stats_t* dft_stats)
{
  AssertFatal(symbol < frame_parms->symbols_per_slot,
              "slot_fep: symbol must be between 0 and %d\n",
              frame_parms->symbols_per_slot - 1);
  AssertFatal(slot < frame_parms->slots_per_frame, "slot_fep: Ns must be between 0 and %d\n", frame_parms->slots_per_frame - 1);

  dft_size_idx_t dftsize = get_dft(frame_parms->ofdm_symbol_size);
  for (unsigned char aa = 0; aa < frame_parms->nb_antennas_rx; aa++) {
    if (dft_stats) start_meas(dft_stats);
    dft(dftsize, (int16_t *)rxdata[aa], (int16_t *)rxdataF[aa], 1);
    if (dft_stats) stop_meas(dft_stats);

    const bool is_sl = (link_type == link_type_sl);
    apply_nr_rotation_symbol_RX(frame_parms->symbols_per_slot,
                                frame_parms->slots_per_subframe,
                                frame_parms->timeshift_symbol_rotation,
                                frame_parms->first_carrier_offset,
                                rxdataF[aa],
                                frame_parms->symbol_rotation[link_type],
                                is_sl ? frame_parms->N_RB_SL : frame_parms->N_RB_DL,
                                slot,
                                symbol);
  }
}

// TEMPORARY DIAGNOSTIC (2026-08-05): exported so callers (acquisition's do_time_to_freq() in
// nr_initial_sync.c does NOT call this function at all -- see that file -- and tracking's
// nr_process_pbch_symbol() in phy_procedures_nr_ue.c DOES) can read the exact rx_offset/CP-length
// this call actually used, without duplicating (and risking drifting out of sync with) the offset
// arithmetic above. Read immediately after the call returns, on the same thread -- this function is
// called synchronously, never through the thread pool, so a plain __thread is sufficient.
/* Frequency offset (Hz) this thread's FEP must de-rotate by, instead of reading
 * ue->dl_Doppler_shift + ue->freq_offset live. NAN = unset = read from ue (every existing caller,
 * including the RT receive thread, so behaviour there is unchanged).
 *
 * Why it exists: the passive PDSCH/PDCCH/PUSCH consumer threads decode samples captured EARLIER by
 * the receive thread, which keeps mutating those two fields. Reading them at decode time de-rotates
 * with a frequency offset newer than the samples it is applied to. Those queues used to REFUSE to
 * start under --cont-fo-comp for exactly this reason and fall back to decoding in-line on the RT
 * thread -- which overran the slot deadline on 99.5 % of grants and cost PBCH lock under load.
 * Capturing the offset at enqueue and replaying it here fixes the hazard instead of avoiding it.
 * Thread-local, so one consumer cannot perturb another or the RT thread. */
__thread double nr_slot_fep_fo_override_hz = NAN;

/* PER-BRANCH FREQUENCY OFFSET (2026-09-03) -- the "duplicate the pipeline per branch" table.
 *
 * Motivation, measured: branches 1-3 rescued 0 of 15411 transport blocks that branch 0 failed,
 * while branch 2 is only 1.7 dB down. A 1.7 dB deficit cannot turn 42 % into 0 % (LDPC's whole
 * waterfall is 1-2 dB), so those branches are not weak, they are not COHERENTLY usable. The X410
 * puts ch0/1 on RF daughterboard A and ch2/3 on board B, and the RX tune loop in usrp_lib.cpp
 * issues set_rx_freq() per channel with NO set_command_time(), so nothing forces the two boards'
 * synthesizers to agree. A residual per-board frequency error rotates a branch's channel across
 * the slot: strong DM-RS estimate, garbage by the last data symbol.
 *
 * This table is what makes each branch's FEP independent -- its own de-rotation, on top of the
 * common one. It is NOT a guess: nr_pdsch_passive_decode.c MEASURES the per-branch drift from the
 * DM-RS phase slope across symbols and writes it here.
 *
 * Plain doubles, no atomics: this is a slowly-varying calibration, a torn or stale read costs one
 * slot of slightly-wrong de-rotation and self-corrects on the next update. All-zero (the default)
 * is bit-identical to the previous behaviour. */
static double g_branch_fo_hz[NR_MAX_BRANCH_FO];

void nr_ue_set_branch_fo_hz(int ant, double hz)
{
  if (ant >= 0 && ant < NR_MAX_BRANCH_FO) {
    g_branch_fo_hz[ant] = hz;
  }
}

double nr_ue_get_branch_fo_hz(int ant)
{
  return (ant >= 0 && ant < NR_MAX_BRANCH_FO) ? g_branch_fo_hz[ant] : 0.0;
}

__thread unsigned int nr_slot_fep_diag_rx_offset = 0;
__thread unsigned int nr_slot_fep_diag_nb_prefix_samples = 0;
__thread unsigned int nr_slot_fep_diag_nb_prefix_samples0 = 0;
__thread int nr_slot_fep_diag_is_synchronized = 0;

int nr_slot_fep(PHY_VARS_NR_UE *ue,
                const NR_DL_FRAME_PARMS *frame_parms,
                unsigned int slot,
                unsigned int symbol,
                c16_t rxdataF[][frame_parms->samples_per_slot_wCP],
                enum nr_Link linktype,
                uint32_t sample_offset,
                c16_t **rxdata)
{
  AssertFatal(symbol < frame_parms->symbols_per_slot,
              "slot_fep: symbol must be between 0 and %d\n",
              frame_parms->symbols_per_slot - 1);
  AssertFatal(slot < frame_parms->slots_per_frame, "slot_fep: Ns must be between 0 and %d\n", frame_parms->slots_per_frame - 1);

  bool is_sl = (linktype == link_type_sl);
  bool is_synchronized = (ue) ? ue->is_synchronized : false;
  unsigned int nb_prefix_samples = frame_parms->nb_prefix_samples;
  unsigned int nb_prefix_samples0 = (is_synchronized || is_sl) ? frame_parms->nb_prefix_samples0 : nb_prefix_samples;

  // For Sidelink 16 frames worth of samples is processed to find SSB, for 5G-NR 2.
  const unsigned int total_samples = (is_sl) ? 16 * frame_parms->samples_per_frame : 2 * frame_parms->samples_per_frame;

  unsigned int rx_offset = get_samples_slot_timestamp(frame_parms, slot);
  const unsigned int abs_symbol = slot * frame_parms->symbols_per_slot + symbol;
  for (int idx_symb = slot * frame_parms->symbols_per_slot; idx_symb <= abs_symbol; idx_symb++)
    rx_offset += (idx_symb % (0x7 << frame_parms->numerology_index)) ? nb_prefix_samples : nb_prefix_samples0;
  rx_offset += frame_parms->ofdm_symbol_size * symbol;

  rx_offset += sample_offset;

  // use OFDM symbol from within 1/8th of the CP to avoid ISI
  rx_offset -= (nb_prefix_samples / frame_parms->ofdm_offset_divisor);

  nr_slot_fep_diag_rx_offset = rx_offset;
  nr_slot_fep_diag_nb_prefix_samples = nb_prefix_samples;
  nr_slot_fep_diag_nb_prefix_samples0 = nb_prefix_samples0;
  nr_slot_fep_diag_is_synchronized = is_synchronized ? 1 : 0;

  LOG_D(PHY,
        "slot_fep: slot %d, symbol %d, nb_prefix_samples %u, nb_prefix_samples0 %u, rx_offset %u energy %d\n",
        slot,
        symbol,
        nb_prefix_samples,
        nb_prefix_samples0,
        rx_offset,
        dB_fixed(signal_energy((int32_t *)&rxdata[0][rx_offset], frame_parms->ofdm_symbol_size)));

  c16_t tmp_dft_in[frame_parms->nb_antennas_rx][frame_parms->ofdm_symbol_size] __attribute__((aligned(32)));
  c16_t *rxdata_symb_ptr[frame_parms->nb_antennas_rx];
  c16_t *rxdataF_symb_ptr[frame_parms->nb_antennas_rx];
  for (unsigned char aa = 0; aa < frame_parms->nb_antennas_rx; aa++) {
    rxdataF_symb_ptr[aa] = &rxdataF[aa][frame_parms->ofdm_symbol_size * symbol];
    // This happens only during initial sync
    if (rx_offset + frame_parms->ofdm_symbol_size > total_samples) {
      // we have to wrap on the end
      memcpy(&tmp_dft_in[aa][0], &rxdata[aa][rx_offset], (total_samples - rx_offset) * sizeof(int32_t));
      memcpy(&tmp_dft_in[aa][total_samples - rx_offset],
             &rxdata[aa][0],
             (frame_parms->ofdm_symbol_size - (total_samples - rx_offset)) * sizeof(int32_t));
      rxdata_symb_ptr[aa] = tmp_dft_in[aa];
    } else {
      rxdata_symb_ptr[aa] = &rxdata[aa][rx_offset];
    }

    if (ue && ue->cont_fo_comp) {
      start_meas_nr_ue_phy(ue, RX_FO_COMPENSATION_STATS);
      nr_fo_compensation(isnan(nr_slot_fep_fo_override_hz) ? (ue->dl_Doppler_shift + ue->freq_offset)
                                                            : nr_slot_fep_fo_override_hz,
                         frame_parms->samples_per_subframe,
                         rx_offset,
                         rxdata_symb_ptr[aa],
                         tmp_dft_in[aa],
                         frame_parms->ofdm_symbol_size);
      stop_meas_nr_ue_phy(ue, RX_FO_COMPENSATION_STATS);
      rxdata_symb_ptr[aa] = tmp_dft_in[aa];
    }
  }
  time_stats_t* dft_stats = NULL;
  if (ue) dft_stats = &ue->phy_cpu_stats.cpu_time_stats[RX_DFT_STATS];
  nr_symbol_fep(frame_parms, slot, symbol, linktype, rxdata_symb_ptr, rxdataF_symb_ptr, dft_stats);
  return 0;
}

/* PASSIVE-RX ANTENNA PARALLELISM (2026-09-03): nr_slot_fep()/nr_symbol_fep() above loop over
 * EVERY RX antenna serially, on whichever single thread calls them. Measured on the passive
 * receiver (PDTIM instrumentation, tests/passive_rx): FEP cost scales ~4x with antenna count
 * (79.9us @1ant -> 321.3us @4ant), and combined with channel estimation's own antenna loop
 * (nr_pdsch_channel_estimation(), ~4x as well), pushes the passive PDSCH decode -- which still
 * runs INLINE on the RT receive thread, see nr_pdsch_passive_decode.c -- over the 500us/slot
 * budget at 30kHz SCS on every slot with 4 antennas, corrupting decode under real traffic (0%
 * CRC at MCS25/4-antenna vs 76-93% at 1 antenna, isolated by disabling MRC combining without
 * changing antenna count -- the failure tracked antenna count, not the combiner).
 *
 * Each antenna's DFT + rotation is fully independent of every other antenna's (no shared state
 * until the MRC/channel-compensation stage that runs afterward), so nr_pdsch_passive_decode.c
 * dispatches one nr_slot_fep_ant() call per antenna across the existing thread pool instead of
 * one nr_slot_fep() call that loops over all antennas on one thread.
 *
 * This duplicates nr_slot_fep()/nr_symbol_fep()'s per-antenna body rather than adding an antenna
 * filter parameter to those functions: both are shared PHY code called from many non-passive
 * paths (gNB and attached-UE alike), and threading an extra parameter through every call site
 * for a passive-rx-only optimisation is exactly the kind of upstream-diff noise CLAUDE.md's
 * coding conventions ask to avoid. Keep this in sync with nr_slot_fep() if its per-antenna body
 * changes.
 *
 * Deliberately does NOT touch ue->phy_cpu_stats' RX_DFT_STATS/RX_FO_COMPENSATION_STATS timers:
 * those are shared (non-atomic) accumulators on the UE struct, and incrementing them
 * concurrently from parallel tasks would race. PDTIM's own FEP timer (nr_pdsch_passive_decode.c)
 * wraps the whole per-grant dispatch from the single calling thread and is what actually matters
 * here; losing the legacy per-DFT stat inside the parallel path is an acceptable, deliberate
 * simplification. */
int nr_slot_fep_ant(PHY_VARS_NR_UE *ue,
                    const NR_DL_FRAME_PARMS *frame_parms,
                    unsigned int slot,
                    unsigned int symbol,
                    unsigned int ant,
                    c16_t rxdataF[][frame_parms->samples_per_slot_wCP],
                    enum nr_Link linktype,
                    uint32_t sample_offset,
                    c16_t **rxdata)
{
  AssertFatal(symbol < frame_parms->symbols_per_slot,
              "nr_slot_fep_ant: symbol must be between 0 and %d\n",
              frame_parms->symbols_per_slot - 1);
  AssertFatal(slot < frame_parms->slots_per_frame, "nr_slot_fep_ant: Ns must be between 0 and %d\n", frame_parms->slots_per_frame - 1);
  AssertFatal(ant < frame_parms->nb_antennas_rx, "nr_slot_fep_ant: ant %u >= nb_antennas_rx %d\n", ant, frame_parms->nb_antennas_rx);

  bool is_sl = (linktype == link_type_sl);
  bool is_synchronized = (ue) ? ue->is_synchronized : false;
  unsigned int nb_prefix_samples = frame_parms->nb_prefix_samples;
  unsigned int nb_prefix_samples0 = (is_synchronized || is_sl) ? frame_parms->nb_prefix_samples0 : nb_prefix_samples;

  const unsigned int total_samples = (is_sl) ? 16 * frame_parms->samples_per_frame : 2 * frame_parms->samples_per_frame;

  unsigned int rx_offset = get_samples_slot_timestamp(frame_parms, slot);
  const unsigned int abs_symbol = slot * frame_parms->symbols_per_slot + symbol;
  for (int idx_symb = slot * frame_parms->symbols_per_slot; idx_symb <= abs_symbol; idx_symb++)
    rx_offset += (idx_symb % (0x7 << frame_parms->numerology_index)) ? nb_prefix_samples : nb_prefix_samples0;
  rx_offset += frame_parms->ofdm_symbol_size * symbol;
  rx_offset += sample_offset;
  // use OFDM symbol from within 1/8th of the CP to avoid ISI
  rx_offset -= (nb_prefix_samples / frame_parms->ofdm_offset_divisor);

  c16_t tmp_dft_in[frame_parms->ofdm_symbol_size] __attribute__((aligned(32)));
  c16_t *rxdataF_symb_ptr = &rxdataF[ant][frame_parms->ofdm_symbol_size * symbol];
  c16_t *rxdata_symb_ptr;
  // This happens only during initial sync
  if (rx_offset + frame_parms->ofdm_symbol_size > total_samples) {
    // we have to wrap on the end
    memcpy(&tmp_dft_in[0], &rxdata[ant][rx_offset], (total_samples - rx_offset) * sizeof(int32_t));
    memcpy(&tmp_dft_in[total_samples - rx_offset],
           &rxdata[ant][0],
           (frame_parms->ofdm_symbol_size - (total_samples - rx_offset)) * sizeof(int32_t));
    rxdata_symb_ptr = tmp_dft_in;
  } else {
    rxdata_symb_ptr = &rxdata[ant][rx_offset];
  }

  /* Common de-rotation (unchanged) PLUS this branch's own residual. The branch term is applied
   * even when cont_fo_comp is off -- which is the configuration this deployment actually runs, so
   * gating it on cont_fo_comp would have made the whole per-branch correction dead code. With an
   * all-zero table and cont_fo_comp off, no compensation call happens at all, exactly as before. */
  const double common_fo_hz = (ue && ue->cont_fo_comp)
                                  ? (isnan(nr_slot_fep_fo_override_hz) ? (ue->dl_Doppler_shift + ue->freq_offset)
                                                                       : nr_slot_fep_fo_override_hz)
                                  : 0.0;
  const double total_fo_hz = common_fo_hz + nr_ue_get_branch_fo_hz((int)ant);
  if (total_fo_hz != 0.0) {
    nr_fo_compensation(total_fo_hz,
                       frame_parms->samples_per_subframe,
                       rx_offset,
                       rxdata_symb_ptr,
                       tmp_dft_in,
                       frame_parms->ofdm_symbol_size);
    rxdata_symb_ptr = tmp_dft_in;
  }

  dft_size_idx_t dftsize = get_dft(frame_parms->ofdm_symbol_size);
  dft(dftsize, (int16_t *)rxdata_symb_ptr, (int16_t *)rxdataF_symb_ptr, 1);

  const bool is_sl2 = (linktype == link_type_sl);
  apply_nr_rotation_symbol_RX(frame_parms->symbols_per_slot,
                              frame_parms->slots_per_subframe,
                              frame_parms->timeshift_symbol_rotation,
                              frame_parms->first_carrier_offset,
                              rxdataF_symb_ptr,
                              frame_parms->symbol_rotation[linktype],
                              is_sl2 ? frame_parms->N_RB_SL : frame_parms->N_RB_DL,
                              slot,
                              symbol);
  return 0;
}

int nr_symbol_fep_ul(const NR_DL_FRAME_PARMS *fp,
                     const c16_t *rxdata,
                     c16_t *rxdataF,
                     unsigned char symbol,
                     unsigned char slot,
                     int sample_offset)
{
  dft_size_idx_t dftsize = get_dft(fp->ofdm_symbol_size);
  // This is for misalignment issues
  int32_t tmp_dft_in[fp->ofdm_symbol_size] __attribute__((aligned(32)));

  // offset of first OFDM symbol
  uint32_t prefix_length = get_samples_symbol_duration(fp, slot, symbol, 1) - fp->ofdm_symbol_size;
  unsigned int rxdata_offset =
      get_samples_slot_timestamp(fp, slot) + get_samples_symbol_timestamp(fp, slot, symbol) + prefix_length;
  // use OFDM symbol from within 1/8th of the CP to avoid ISI
  rxdata_offset -= (fp->nb_prefix_samples / fp->ofdm_offset_divisor);

  int16_t *rxdata_ptr;
  if (rxdata_offset >= sample_offset)
    rxdata_offset -= sample_offset;
  else
    rxdata_offset += fp->samples_per_frame - sample_offset;

  if (rxdata_offset + fp->ofdm_symbol_size > fp->samples_per_frame) {
    memcpy(&tmp_dft_in[0],
           &rxdata[rxdata_offset],
           (fp->samples_per_frame - rxdata_offset) * sizeof(int32_t));
    memcpy(&tmp_dft_in[fp->samples_per_frame - rxdata_offset],
           &rxdata[0],
           (fp->ofdm_symbol_size - fp->samples_per_frame + rxdata_offset) * sizeof(int32_t));
    rxdata_ptr = (int16_t *)tmp_dft_in;
  } else {
    // use dft input from RX buffer directly
    rxdata_ptr = (int16_t *)&rxdata[rxdata_offset];
  }

  dft(dftsize, rxdata_ptr, (int16_t *)rxdataF, 1);

  return 0;
}

void apply_nr_rotation_symbol_RX(const int symbols_per_slot,
                                 const int slots_per_subframe,
                                 const c16_t *shift_rot,
                                 const int first_carrier_offset,
                                 c16_t *rxdataF,
                                 const c16_t *rot,
                                 int nb_rb,
                                 int slot,
                                 int symbol)
{
  const int symb_offset = (slot % slots_per_subframe) * symbols_per_slot;

  c16_t rot2 = rot[symbol + symb_offset];
  rot2.i = -rot2.i;
  LOG_D(PHY, "slot %d, symb_offset %d rotating by %d.%d\n", slot, symb_offset, rot2.r, rot2.i);
  c16_t *this_symbol = rxdataF;

  if (nb_rb & 1) {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, (nb_rb + 1) * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset - 6, rot2, this_symbol + first_carrier_offset - 6, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset - 6,
                    shift_rot + first_carrier_offset - 6,
                    this_symbol + first_carrier_offset - 6,
                    (nb_rb + 1) * 6,
                    15);
  } else {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, nb_rb * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset, rot2, this_symbol + first_carrier_offset, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset,
                    shift_rot + first_carrier_offset,
                    this_symbol + first_carrier_offset,
                    nb_rb * 6,
                    15);
  }
}

void nr_ofdm_demod_and_rx_rotation(c16_t **rxdata,
                                   c16_t **rxdataF,
                                   const NR_DL_FRAME_PARMS *fp,
                                   int nb_antennas,
                                   int slot,
                                   int slot_offsetF,
                                   enum nr_Link linktype,
                                   bool was_symbol_used[NR_SYMBOLS_PER_SLOT])
{
  for (int aa = 0; aa < nb_antennas; aa++) {
    for (uint8_t symbol = 0; symbol < fp->symbols_per_slot; symbol++) {
      if (was_symbol_used[symbol] == true) {
        nr_symbol_fep_ul(fp, &rxdata[aa][0], &rxdataF[aa][slot_offsetF + symbol * fp->ofdm_symbol_size], symbol, slot, 0);
        apply_nr_rotation_symbol_RX(fp->symbols_per_slot,
                                    fp->slots_per_subframe,
                                    fp->timeshift_symbol_rotation,
                                    fp->first_carrier_offset,
                                    &rxdataF[aa][slot_offsetF + symbol * fp->ofdm_symbol_size],
                                    fp->symbol_rotation[linktype],
                                    fp->N_RB_UL,
                                    slot,
                                    symbol);
      }
    }
  }
}
