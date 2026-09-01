/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief      Functional helpers to configure the RF boards at UE side
 */

#include "openair1/PHY/defs_nr_UE.h"
#include "openair1/PHY/phy_extern_nr_ue.h"
#include "nr_transport_proto_ue.h"
#include "executables/softmodem-common.h"
#include <stdlib.h>
#include <string.h>

void nr_get_carrier_frequencies(const PHY_VARS_NR_UE *ue, uint64_t *dl_carrier, uint64_t *ul_carrier)
{
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (ue->if_freq!=0) {
    *dl_carrier = ue->if_freq;
    *ul_carrier = *dl_carrier + ue->if_freq_off;
  }
  else{
    *dl_carrier = fp->dl_CarrierFreq;
    *ul_carrier = fp->ul_CarrierFreq;
  }
}


/* PER-BRANCH RX GAIN TRIM -- ISAC_RX_GAIN_TRIM="d0,d1,d2,d3", dB ADDED to --ue-rxgain per channel.
 *
 * openair0_config_t::rx_gain is already a per-channel array and usrp_lib.cpp's device_init already
 * calls set_rx_gain(gain, chan) in a per-channel loop; the only reason every branch got the same
 * value is that this function wrote the same number into all four. So this is a fill change, not a
 * driver change.
 *
 * WHAT IT CANNOT DO: recover SNR on a branch that is down because of loss AHEAD of the LNA (cable,
 * connector, antenna). Gain there raises signal and that branch's own noise together, so its SNR is
 * unchanged. What it does buy is EQUAL LEVELS into the fixed-point combiner, which picks one shift
 * from the strongest branch (nr_ulsch_demodulation.c: avgs = cmax over antennas), so a branch 16 dB
 * down loses ~3 bits of the accumulator. Use it for that, and fix the cable for the SNR.
 */
static double nr_ue_rx_gain_trim(int ch)
{
  static double trim[8];
  static int parsed;
  if (!parsed) {
    parsed = 1;
    const char *e = getenv("ISAC_RX_GAIN_TRIM");
    if (e && *e) {
      char buf[128];
      strncpy(buf, e, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      int i = 0;
      for (char *t = strtok(buf, ","); t != NULL && i < 8; t = strtok(NULL, ","), i++) {
        trim[i] = atof(t);
      }
      LOG_W(PHY, "SENSING: RX gain trim = [%.1f %.1f %.1f %.1f] dB (added per branch)\n",
            trim[0], trim[1], trim[2], trim[3]);
    }
  }
  return (ch >= 0 && ch < 8) ? trim[ch] : 0.0;
}

void nr_rf_card_config_gain(openair0_config_t *openair0_cfg)
{
  uint8_t mod_id     = 0;
  uint8_t cc_id      = 0;
  PHY_VARS_NR_UE *ue = nrPHY_vars_UE_g[mod_id][cc_id];
  int rf_chain       = ue->rf_map.chain;
  double rx_gain     = ue->rx_total_gain_dB;
  double tx_gain     = ue->tx_total_gain_dB;

  for (int i = rf_chain; i < rf_chain + 4; i++) {

    if (tx_gain)
      openair0_cfg->tx_gain[i] = tx_gain;
    if (rx_gain)
      openair0_cfg->rx_gain[i] = rx_gain + nr_ue_rx_gain_trim(i - rf_chain);

    openair0_cfg->autocal[i] = 1;

    if (i < openair0_cfg->rx_num_channels) {
      LOG_I(PHY, "HW: Configuring channel %d (rf_chain %d): setting tx_gain %.0f, rx_gain %.0f\n",
        i,
        rf_chain,
        openair0_cfg->tx_gain[i],
        openair0_cfg->rx_gain[i]);
    }

  }
}

void nr_rf_card_config_freq(openair0_config_t *openair0_cfg,
                            uint64_t ul_carrier,
                            uint64_t dl_carrier,
                            int freq_offset){

  uint8_t mod_id     = 0;
  uint8_t cc_id      = 0;
  PHY_VARS_NR_UE *ue = nrPHY_vars_UE_g[mod_id][cc_id];
  int rf_chain       = ue->rf_map.chain;
  double freq_scale  = (double)(dl_carrier + freq_offset) / dl_carrier;

  for (int i = rf_chain; i < rf_chain + 4; i++) {

    if (i < openair0_cfg->rx_num_channels)
      openair0_cfg->rx_freq[i + rf_chain] = dl_carrier * freq_scale;
    else
      openair0_cfg->rx_freq[i] = 0.0;

    if (i<openair0_cfg->tx_num_channels)
      openair0_cfg->tx_freq[i] = ul_carrier * freq_scale;
    else
      openair0_cfg->tx_freq[i] = 0.0;

    openair0_cfg->autocal[i] = 1;

    if (i < openair0_cfg->rx_num_channels) {
      LOG_I(PHY, "HW: Configuring channel %d (rf_chain %d): setting tx_freq %.0f Hz, rx_freq %.0f Hz, tune_offset %.0f\n",
        i,
        rf_chain,
        openair0_cfg->tx_freq[i],
        openair0_cfg->rx_freq[i],
        openair0_cfg->tune_offset);
    }

  }
}


void nr_sl_rf_card_config_freq(PHY_VARS_NR_UE *ue, openair0_config_t *openair0_cfg, int freq_offset) {

  for (int i = 0; i < openair0_cfg->rx_num_channels; i++) {
    openair0_cfg->rx_gain[ue->rf_map.chain + i] = ue->rx_total_gain_dB;
    if (ue->UE_scan_carrier == 1) {
      if (freq_offset >= 0)
        openair0_cfg->rx_freq[ue->rf_map.chain + i] += abs(freq_offset);
      else
        openair0_cfg->rx_freq[ue->rf_map.chain + i] -= abs(freq_offset);
      freq_offset=0;
    }
  }
}
