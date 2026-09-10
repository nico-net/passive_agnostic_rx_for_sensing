/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>

#include <libbladeRF.h>
#include "common_lib.h"
#include "log.h"

/*! \brief BladeRF specific data structure */
typedef struct {
  //! opaque BladeRF device struct. An empty ("") or NULL device identifier will result in the first encountered device being opened
  //! (using the first discovered backend)
  struct bladerf *dev;

  //! Number of buffers
  unsigned int num_buffers;
  //! Buffer size
  unsigned int buffer_size;
  //! Number of transfers
  unsigned int num_transfers;
  //! RX timeout
  unsigned int rx_timeout_ms;
  //! TX timeout
  unsigned int tx_timeout_ms;
  //! Metadata for RX
  struct bladerf_metadata meta_rx;
  //! Metadata for TX
  struct bladerf_metadata meta_tx;
  //! Sample rate
  unsigned int sample_rate;
  //! Stream sample format
  bladerf_format format;
  //! True when the oversample feature and packed 8-bit samples are used
  bool use_sc8;
  //! Number of complex samples available in each conversion buffer
  size_t conversion_buffer_size;
  //! Packed SC8 Q7 RX/TX conversion buffers
  int8_t *rx_conversion_buffer;
  int8_t *tx_conversion_buffer;

  // --------------------------------
  // Debug and output control
  // --------------------------------
  //! Number of underflows
  int num_underflows;
  //! Number of overflows
  int num_overflows;
  //! number of RX errors
  int num_rx_errors;
  //! Number of TX errors
  int num_tx_errors;
  //! timestamp of current TX
  uint64_t tx_current_ts;
  //! timestamp of current RX
  uint64_t rx_current_ts;
  //! number of actual samples transmitted
  uint64_t tx_actual_nsamps;
  //! number of actual samples received
  uint64_t rx_actual_nsamps;
  //! number of TX samples
  uint64_t tx_nsamps;
  //! number of RX samples
  uint64_t rx_nsamps;
  //! number of TX count
  uint64_t tx_count;
  //! number of RX count
  uint64_t rx_count;
} brf_state_t;

/** @addtogroup _BLADERF_PHY_RF_INTERFACE_
 * @{
 */

#define BLADERF_CHECK(COND, FMT, ...)                                                        \
  if (COND) {                                                                                \
    LOG_I(HW, FMT "\n" __VA_OPT__(, ) __VA_ARGS__);                                          \
  } else {                                                                                   \
    LOG_E(HW, "Failed: " FMT ": %s\n" __VA_OPT__(, ) __VA_ARGS__, bladerf_strerror(status)); \
    return -1;                                                                               \
  }

static const char *ch_dir(bladerf_channel ch)
{
  switch (ch) {
    case BLADERF_CHANNEL_RX(0):
      return "RX0";
    case BLADERF_CHANNEL_RX(1):
      return "RX1";
    case BLADERF_CHANNEL_TX(0):
      return "TX0";
    case BLADERF_CHANNEL_TX(1):
      return "TX1";
    default:
      return "unknown";
  }
  return "unknown";
}

/*! \brief Start BladeRF
 * \param device the hardware to use
 * \returns 0 on success
 */
static int brf_open_and_configure(brf_state_t *brf, openair0_config_t *openair0_cfg);

static int brf_start_stream(openair0_device_t *device)
{
  brf_state_t *brf = device->priv;
  int status;

  brf->meta_tx.flags = 0;

  if (device->openair0_cfg->tx_num_channels > 0) {
    status = bladerf_sync_config(brf->dev, BLADERF_CHANNEL_TX(0), brf->format,
                                 brf->num_buffers, brf->buffer_size, brf->num_transfers, brf->tx_timeout_ms);
    BLADERF_CHECK(status == 0,
                  "Set TX sync interface num_buffers %u, buffer_size %u, num_transfer %u, tx_timeout_ms %u",
                  brf->num_buffers, brf->buffer_size, brf->num_transfers, brf->tx_timeout_ms);
  } else {
    LOG_I(HW, "Skip TX sync interface in RX-only mode\n");
  }

  status = bladerf_sync_config(brf->dev, BLADERF_CHANNEL_RX(0), brf->format,
                               brf->num_buffers, brf->buffer_size, brf->num_transfers, brf->rx_timeout_ms);
  BLADERF_CHECK(status == 0,
                "Set RX sync interface num_buffers %u, buffer_size %u, num_transfer %u, rx_timeout_ms %u",
                brf->num_buffers, brf->buffer_size, brf->num_transfers, brf->rx_timeout_ms);

  /* We must always enable the TX module after calling bladerf_sync_config(), and
   * before  attempting to TX samples via  bladerf_sync_tx(). */
  if (device->openair0_cfg->tx_num_channels > 0) {
    status = bladerf_enable_module(brf->dev, BLADERF_CHANNEL_TX(0), true);
    BLADERF_CHECK(status == 0, "Enable TX0 module");
  } else {
    LOG_I(HW, "Skip TX0 module enable in RX-only mode\n");
  }

  /* We must always enable the RX module after calling bladerf_sync_config(), and
   * before  attempting to RX samples via  bladerf_sync_rx(). */
  status = bladerf_enable_module(brf->dev, BLADERF_CHANNEL_RX(0), true);
  BLADERF_CHECK(status == 0, "Enable RX module");

  /* The AD9361 manual gain written in device_init() does not stick: with the
   * oversample feature enabled, bladerf_set_gain() returns success but leaves
   * the RX gain pinned at the range maximum (60 dB) until the RX module has
   * actually been enabled. At 60 dB a strong 100 MHz carrier saturates the ADC
   * (>50% of samples at full scale), which destroys PBCH/PDSCH demodulation
   * while still leaving PSS detectable. Re-apply the requested gain here, now
   * that the module is running, and verify it took effect. */
  const double want_rx_gain = device->openair0_cfg->rx_gain[0] - device->openair0_cfg->rx_gain_offset[0];
  status = bladerf_set_gain(brf->dev, BLADERF_CHANNEL_RX(0), (bladerf_gain)lround(want_rx_gain));
  BLADERF_CHECK(status == 0, "Re-apply RX0 gain %.0f after module enable", want_rx_gain);
  bladerf_gain applied_rx_gain = 0;
  status = bladerf_get_gain(brf->dev, BLADERF_CHANNEL_RX(0), &applied_rx_gain);
  BLADERF_CHECK(status == 0, "Read back RX0 gain");
  if (applied_rx_gain != (bladerf_gain)lround(want_rx_gain))
    LOG_E(HW, "RX0 gain did not take effect: requested %.0f dB, device reports %d dB\n", want_rx_gain, applied_rx_gain);
  else
    LOG_I(HW, "RX0 gain re-applied after module enable: %d dB\n", applied_rx_gain);

  // in case it was set to verbosity before, disable now because otherwise the
  // terminal will be flooded
  bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);
  return 0;
}

/* Number of 0.5 ms reads used to judge whether a freshly started RX stream is
 * healthy, and how many of the first ones to ignore (a good stream still shows
 * a handful of overruns while the USB transfers ramp up). */
#define BRF_PROBE_READS       400
#define BRF_PROBE_SKIP        50
#define BRF_PROBE_MAX_BAD_PCT 25
#define BRF_START_ATTEMPTS    12

/*! \brief Check that a freshly started RX stream actually delivers samples.
 *
 * At 122.88 Msps the bladeRF stream comes up broken roughly half the time: every
 * bladerf_sync_rx() then returns immediately with BLADERF_META_STATUS_OVERRUN and
 * exactly one USB message worth of samples (4088), forever. Restarting the stream
 * alone does not clear it -- only closing and reopening the device does.
 * \returns 0 if the stream looks healthy, -1 if it is stuck in permanent overrun
 */
static int brf_probe_stream(brf_state_t *brf)
{
  const unsigned int nsamps = brf->conversion_buffer_size / 2; /* 0.5 ms */
  int bad = 0, considered = 0;

  for (int i = 0; i < BRF_PROBE_READS; i++) {
    struct bladerf_metadata meta = {0};
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    int status = bladerf_sync_rx(brf->dev, brf->rx_conversion_buffer, nsamps, &meta, 2 * brf->rx_timeout_ms);
    if (status != 0) {
      LOG_W(HW, "RX stream probe: read failed: %s\n", bladerf_strerror(status));
      return -1;
    }
    if (i < BRF_PROBE_SKIP)
      continue;
    considered++;
    if (meta.status & BLADERF_META_STATUS_OVERRUN)
      bad++;
  }

  const int bad_pct = considered ? (100 * bad) / considered : 100;
  if (bad_pct > BRF_PROBE_MAX_BAD_PCT) {
    LOG_W(HW, "RX stream probe: %d%% of reads overran (%d/%d), stream is stuck\n", bad_pct, bad, considered);
    return -1;
  }
  LOG_I(HW, "RX stream probe: healthy (%d/%d reads overran)\n", bad, considered);
  return 0;
}

static int trx_brf_start(openair0_device_t *device)
{
  brf_state_t *brf = device->priv;

  for (int attempt = 0; attempt < BRF_START_ATTEMPTS; attempt++) {
    if (attempt > 0) {
      /* Reopening the device is the only thing that clears the stuck-stream
       * state, so tear the whole thing down and configure it again. */
      LOG_W(HW, "Reopening bladeRF to recover the RX stream (attempt %d/%d)\n", attempt + 1, BRF_START_ATTEMPTS);
      bladerf_enable_module(brf->dev, BLADERF_CHANNEL_RX(0), false);
      if (device->openair0_cfg->tx_num_channels > 0)
        bladerf_enable_module(brf->dev, BLADERF_CHANNEL_TX(0), false);
      bladerf_close(brf->dev);
      brf->dev = NULL;
      usleep(200000);
      if (brf_open_and_configure(brf, device->openair0_cfg) != 0)
        return -1;
    }

    if (brf_start_stream(device) != 0)
      return -1;

    /* Only the 122.88 Msps oversample path shows the stuck-stream failure, and
     * it is the only one with the conversion buffer the probe reads into. */
    if (!brf->use_sc8)
      return 0;

    if (brf_probe_stream(brf) == 0)
      return 0;
  }

  LOG_E(HW, "bladeRF RX stream still stuck after %d attempts, giving up\n", BRF_START_ATTEMPTS);
  return -1;
}


/*! \brief Called to send samples to the BladeRF RF target
      \param device pointer to the device structure specific to the RF hardware target
      \param timestamp The timestamp at which the first sample MUST be sent
      \param buff Buffer which holds the samples
      \param nsamps number of samples to be sent
      \param cc index of the component carrier
      \param flags Ignored for the moment
      \returns 0 on success
*/
static int trx_brf_write(openair0_device_t *device, openair0_timestamp_t ptimestamp, void **buff, int nsamps, int cc, int flags)
{
  brf_state_t *brf = device->priv;
  if (device->openair0_cfg->tx_num_channels == 0)
    return nsamps;

  /* BRF has only 1 rx/tx chaine : is it correct? TODO: now, handle also two! */
  int16_t *samples = (int16_t *)buff[0];
  void *wire_samples = samples;
  if (brf->use_sc8) {
    DevAssert(nsamps <= brf->conversion_buffer_size);
    for (int i = 0; i < 2 * nsamps; ++i) {
      int16_t sample = samples[i] >> 4;
      brf->tx_conversion_buffer[i] = sample > INT8_MAX ? INT8_MAX : sample < INT8_MIN ? INT8_MIN : sample;
    }
    wire_samples = brf->tx_conversion_buffer;
  }
  ptimestamp -= device->openair0_cfg->command_line_sample_advance - device->openair0_cfg->tx_sample_advance;

  // When BLADERF_META_FLAG_TX_NOW is used the timestamp is not used, so one can't schedule a tx
  if (brf->meta_tx.flags == 0)
    brf->meta_tx.flags = BLADERF_META_FLAG_TX_BURST_START;
  brf->meta_tx.timestamp = (uint64_t)(ptimestamp);

  int status = bladerf_sync_tx(brf->dev, wire_samples, nsamps, &brf->meta_tx, 2 * brf->tx_timeout_ms);
  if (status == BLADERF_ERR_TIME_PAST) {
    brf->num_tx_errors++;
    LOG_W(HW, "Timestamp error: Requested timestamp %ld is in the past.\n", ptimestamp);
  } else if (status != 0) {
    brf->num_tx_errors++;
    LOG_W(HW, "Error during TX: %s\n", bladerf_strerror(status));
  }

  if (brf->meta_tx.flags == BLADERF_META_FLAG_TX_BURST_START)
    brf->meta_tx.flags = BLADERF_META_FLAG_TX_UPDATE_TIMESTAMP;

  brf->tx_current_ts = brf->meta_tx.timestamp;
  brf->tx_actual_nsamps += brf->meta_tx.actual_count;
  brf->tx_nsamps += nsamps;
  brf->tx_count++;

  return nsamps; // brf->meta_tx.actual_count;
}

/*! \brief Receive samples from hardware.
 * Read \ref nsamps samples from each channel to buffers. buff[0] is the array for
 * the first channel. *ptimestamp is the time at which the first sample
 * was received.
 * \param device the hardware to use
 * \param[out] ptimestamp the time at which the first sample was received.
 * \param[out] buff An array of pointers to buffers for received samples. The buffers must be large enough to hold the number of samples \ref nsamps.
 * \param nsamps Number of samples. One sample is 2 byte I + 2 byte Q => 4 byte.
 * \param cc  Index of component carrier
 * \returns number of samples read
*/
static int trx_brf_read(openair0_device_t *device, openair0_timestamp_t *ptimestamp, void **buff, int nsamps, int cc)
{
  brf_state_t *brf = device->priv;

  // BRF has only one rx/tx chain
  int16_t *samples = (int16_t *)buff[0];
  void *wire_samples = samples;
  if (brf->use_sc8) {
    DevAssert(nsamps <= brf->conversion_buffer_size);
    wire_samples = brf->rx_conversion_buffer;
  }

  brf->meta_rx.actual_count = 0;
  brf->meta_rx.flags = BLADERF_META_FLAG_RX_NOW;
  int status = bladerf_sync_rx(brf->dev, wire_samples, (unsigned int)nsamps, &brf->meta_rx, 2 * brf->rx_timeout_ms);

  if (brf->use_sc8 && status == 0) {
    for (int i = 0; i < 2 * nsamps; ++i)
      samples[i] = (int16_t)brf->rx_conversion_buffer[i] * 16;
  }

  if (status != 0) {
    LOG_E(HW, "RX failed: %s\n", bladerf_strerror(status));
    brf->num_rx_errors++;
  } else if (brf->meta_rx.status & BLADERF_META_STATUS_OVERRUN) {
    brf->num_overflows++;
    LOG_W(HW,
          "%d RX overrun is detected. timestamp %ld. Requested %u but got %u samples\n",
          brf->num_overflows,
          brf->meta_rx.timestamp,
          nsamps,
          brf->meta_rx.actual_count);
  }

  if (brf->meta_rx.actual_count != nsamps) {
    LOG_E(HW, "RX bad samples count, wanted %d, got %d\n", nsamps, brf->meta_rx.actual_count);
  }

  brf->rx_current_ts = brf->meta_rx.timestamp;
  brf->rx_actual_nsamps += brf->meta_rx.actual_count;
  brf->rx_nsamps += nsamps;
  brf->rx_count++;

  *ptimestamp = brf->meta_rx.timestamp;

  return nsamps; // brf->meta_rx.actual_count;
}

/*! \brief Terminate operation of the BladeRF transceiver -- free all associated resources
 * \param device the hardware to use
 */
static void trx_brf_end(openair0_device_t *device)
{
  brf_state_t *brf = device->priv;
  // Disable RX module, shutting down our underlying RX stream

  int status;
  if ((status = bladerf_enable_module(brf->dev, BLADERF_CHANNEL_RX(0), false)) != 0)
    LOG_E(HW, "Failed: Disable RX module: %s\n", bladerf_strerror(status));
  if ((status = bladerf_enable_module(brf->dev, BLADERF_CHANNEL_TX(0), false)) != 0)
    LOG_E(HW, "Failed: Disable TX module: %s\n", bladerf_strerror(status));
  free(brf->rx_conversion_buffer);
  free(brf->tx_conversion_buffer);
  bladerf_close(brf->dev);
}


/*! \brief print the BladeRF statistics
* \param device the hardware to use
* \returns  0 on success
*/
static int trx_brf_get_stats(openair0_device_t *device)
{
  return 0;
}

/*! \brief Reset the BladeRF statistics
 * \param device the hardware to use
 * \returns  0 on success
 */
static int trx_brf_reset_stats(openair0_device_t *device)
{
  return 0;
}

/*! \brief Stop BladeRF
 * \param card the hardware to use
 * \returns 0 in success
 */
static int trx_brf_stop(openair0_device_t *device)
{
  return 0;
}

/*! \brief Set frequencies (TX/RX)
 * \param device the hardware to use
 * \param openair0_cfg1 openair0 Config structure (ignored. It is there to comply with RF common API)
 * \returns 0 in success
 */
static int trx_brf_set_freq(openair0_device_t *device, openair0_config_t *openair0_cfg1)
{
  int status;
  brf_state_t *brf = device->priv;
  openair0_config_t *openair0_cfg = (openair0_config_t *)device->openair0_cfg;

  /* In RX-only mode tx_freq is 0, which bladerf_set_frequency rejects. The
   * BLADERF_CHECK below would then return before the RX retune, silently
   * turning --ue-fo-compensation into a no-op and leaving the whole carrier
   * frequency offset (several kHz, i.e. a large fraction of the subcarrier
   * spacing) uncorrected, so nothing decodes after initial sync. */
  if (openair0_cfg->tx_num_channels > 0) {
    unsigned int tx_freq = (unsigned int)openair0_cfg->tx_freq[0];
    status = bladerf_set_frequency(brf->dev, BLADERF_CHANNEL_TX(0), tx_freq);
    BLADERF_CHECK(status == 0, "Set TX frequency %u", tx_freq);
  }

  unsigned int rx_freq = (unsigned int)openair0_cfg->rx_freq[0];
  status = bladerf_set_frequency(brf->dev, BLADERF_CHANNEL_RX(0), rx_freq);
  BLADERF_CHECK(status == 0, "Set RX frequency %u", rx_freq);

  return 0;
}


/*! \brief Set Gains (TX/RX)
 * \param device the hardware to use
 * \param openair0_cfg openair0 Config structure
 * \returns 0 in success
 */
static int trx_brf_set_gains(openair0_device_t *device, openair0_config_t *openair0_cfg)
{
  return 0;
}

static int trx_brf_write_init(openair0_device_t *device)
{
  return 0;
}

typedef struct channel_config {
    bladerf_channel channel;
    unsigned int frequency;
    unsigned int sample_rate;
    unsigned int bandwidth;
    int gain;
} channel_config_t;


static int configure_channel(struct bladerf *dev, channel_config_t *c)
{
  int status;
  unsigned int actual_value = 0;
  const char *dir = ch_dir(c->channel);

  status = bladerf_set_frequency(dev, c->channel, c->frequency);
  BLADERF_CHECK(status == 0, "Set %s frequency %u", dir, c->frequency);

  status = bladerf_set_sample_rate(dev, c->channel, c->sample_rate, &actual_value);
  BLADERF_CHECK(status == 0, "Set %s sample rate %u (is %u)", dir, c->sample_rate, actual_value);

  if (c->bandwidth > 0) {
    status = bladerf_set_bandwidth(dev, c->channel, c->bandwidth, &actual_value);
    BLADERF_CHECK(status == 0, "Set %s bandwidth %u (is %u)", dir, c->bandwidth, actual_value);
    /* bladerf_set_bandwidth() reports Success and then silently clamps to the
     * AD9361's 56 MHz maximum, in oversample mode too. At 122.88 Msps you get
     * the SAMPLE RATE for a 100 MHz carrier but not the ANALOG BANDWIDTH, and
     * the shortfall is invisible unless it is said out loud: measured on a
     * 273-PRB band 78 carrier, SNR holds at 14-19 dB from -38 to +26 MHz and
     * collapses to 3-6 dB beyond that, so roughly 64 MHz of the 100 MHz is
     * usable. Narrow or low-rate allocations (SSB, CORESET#0, SIB1) are fine;
     * full-band high-MCS transport blocks lose their edge code blocks. */
    if (actual_value < c->bandwidth)
      LOG_W(HW,
            "%s analog bandwidth clamped to %u Hz, %u Hz was requested: the carrier is wider than "
            "this radio can pass, expect the band edges to be unusable\n",
            dir,
            actual_value,
            c->bandwidth);
  }

  status = bladerf_set_gain(dev, c->channel, c->gain);
  BLADERF_CHECK(status == 0, "Set %s gain %u", dir, c->gain);

  bladerf_gain g;
  status = bladerf_get_gain(dev, c->channel, &g);
  BLADERF_CHECK(status == 0, "Get %s gain: %u", dir, g);

  return 0;
}

/*! \brief Open the bladeRF and apply the full RF configuration.
 *
 * Split out of device_init() so that trx_brf_start() can close and reopen the
 * device from scratch when the RX stream comes up stuck. */
static int brf_open_and_configure(brf_state_t *brf, openair0_config_t *openair0_cfg)
{
  int status;

  status = bladerf_open(&brf->dev, openair0_cfg->sdr_addrs);
  BLADERF_CHECK(status == 0, "Init BladeRF device '%s'", openair0_cfg->sdr_addrs);

  if (bladerf_device_speed(brf->dev) != BLADERF_DEVICE_SPEED_SUPER) {
    LOG_E(HW, "Device does not operate at max speed, change the USB port\n");
    return -1;
    }

  if (brf->use_sc8) {
    /* Go through BLADERF_FEATURE_DEFAULT first: if the device is still in
     * oversample mode from a previous run, requesting it again is a no-op and
     * part of the setup is skipped, which makes the stuck-stream failure below
     * much more likely. Forcing the transition makes startup more reliable. */
    status = bladerf_enable_feature(brf->dev, BLADERF_FEATURE_DEFAULT, true);
    BLADERF_CHECK(status == 0, "Reset feature state to default");
    status = bladerf_enable_feature(brf->dev, BLADERF_FEATURE_OVERSAMPLE, true);
    BLADERF_CHECK(status == 0, "Enable oversample feature with packed 8-bit samples");
  }

  /** [Forcing Tuning Mode to FPGA] */
  const bladerf_tuning_mode tMode = BLADERF_TUNING_MODE_FPGA;
  status = bladerf_set_tuning_mode(brf->dev, tMode);
  BLADERF_CHECK(status == 0, "Set tuning mode FPGA");

  status = bladerf_set_gain_mode(brf->dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
  BLADERF_CHECK(status == 0, "Disable AGC");

  channel_config_t rx_chan = {
    .channel = BLADERF_CHANNEL_RX(0),
    .frequency = openair0_cfg->rx_freq[0],
    .sample_rate = openair0_cfg->sample_rate,
    /* At 122.88 Msps, calibrate the AD9361 RX baseband filter at its widest
     * supported bandwidth. libbladeRF's oversample setup, invoked by the
     * sample-rate calls, then applies its extended-bandwidth register setup.
     * Leaving this at zero retains the device's previous/default calibration
     * and can suppress an SSB near the edge of a 100 MHz NR carrier. */
    .bandwidth = brf->use_sc8 ? 56000000 : openair0_cfg->rx_bw,
    .gain = openair0_cfg->rx_gain[0],
  };
  configure_channel(brf->dev, &rx_chan);

  /* Say out loud when the carrier is wider than the radio can actually pass.
   * The AD9361's RX analog bandwidth is hard-capped at 56 MHz -- in oversample
   * mode too, where bladerf_set_bandwidth() reports Success for 61.44, 80, 100
   * and 122.88 MHz and clamps every one of them to 56 MHz. The 8-bit oversample
   * feature raises the SAMPLE RATE to 122.88 Msps, which is what is needed to
   * represent a 100 MHz carrier, but it does not widen this filter.
   *
   * Measured end to end by sweeping the LO so one fixed RF component lands at
   * different baseband offsets (level relative to band centre, symmetric):
   *   offset  0    +/-10  +/-20  +/-30  +/-40  +/-50 MHz
   *   level   0    -0.3   -1.0   -3.2   -8.6   -11.3 dB
   * a clean lowpass with its -3 dB corner at ~29 MHz, i.e. the 56 MHz RF
   * bandwidth the API reports. A 273-PRB carrier spans +/-49 MHz, so its outer
   * PRBs arrive about 11 dB below the centre ones. The whole 100 MHz IS
   * received and processed; it simply arrives tilted. */
  bladerf_bandwidth applied_rx_bw = 0;
  if (bladerf_get_bandwidth(brf->dev, BLADERF_CHANNEL_RX(0), &applied_rx_bw) == 0 && openair0_cfg->num_rb_dl > 0) {
    /* num_rb_dl RBs of 12 subcarriers, subcarrier spacing from the sample rate's numerology */
    const double carrier_hz = openair0_cfg->num_rb_dl * 12.0 * (openair0_cfg->sample_rate / 4096.0);
    if (carrier_hz > applied_rx_bw * 1.05)
      LOG_W(HW,
            "carrier is ~%.1f MHz but the RX analog bandwidth is %.1f MHz (AD9361 maximum): measured "
            "roll-off is -3 dB at +/-30 MHz and about -11 dB at the edge of a 100 MHz carrier, so the "
            "outer PRBs arrive that much weaker than the centre ones\n",
            carrier_hz / 1e6,
            applied_rx_bw / 1e6);
  }

  // This is a hack. The USRP library currently does max_gain-tx_gain (tx_gain
  // is the attenuation, so completely illogical), but let's keep what users
  // would expect by doing the same. Existing configs can then be reused.
  // Note also that the doc says that get_gain_range() should be called after
  // setting the frequency, but the gain should normally come from outside, so
  // be set by user and independent of the exact frequency.
  if (openair0_cfg->tx_num_channels > 0) {
    const struct bladerf_range *g = NULL;
    status = bladerf_get_gain_range(brf->dev, BLADERF_CHANNEL_TX(0), &g);
    BLADERF_CHECK(status == 0 && g != NULL, "Get TX0 gain range");
    channel_config_t tx_chan = {
      .channel = BLADERF_CHANNEL_TX(0),
      .frequency = openair0_cfg->tx_freq[0],
      .sample_rate = openair0_cfg->sample_rate,
      .bandwidth = brf->use_sc8 ? 0 : openair0_cfg->tx_bw,
      .gain = g->max - openair0_cfg->tx_gain[0], // see comment above
    };
    configure_channel(brf->dev, &tx_chan);
  } else {
    LOG_I(HW, "Skip TX0 channel configuration in RX-only mode\n");
  }

  LOG_I(HW, "BLADERF: Initializing device done\n");
  return 0;
}

/*! \brief Initialize Openair BLADERF target. It returns 0 if OK
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \returns 0 on success
 */
int device_init(openair0_device_t *device, openair0_config_t *openair0_cfg)
{
  brf_state_t brf = {0};

  switch ((long) openair0_cfg->sample_rate) {
    case 122880000:
      openair0_cfg->samples_per_packet = 16384;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 128;
      brf.num_transfers = 32;
      brf.rx_timeout_ms = 2000;
      brf.use_sc8 = true;
      break;

    case 46080000:
      openair0_cfg->samples_per_packet = 8192;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 128;
      brf.num_transfers = 32;
      brf.rx_timeout_ms = 2000;
      break;

    case 30720000:
      openair0_cfg->samples_per_packet = 4096;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 128;
      brf.num_transfers = 32;
      brf.rx_timeout_ms = 2000;
      break;

    case 23040000:
      openair0_cfg->samples_per_packet = 2048;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 64;
      brf.num_transfers = 16;
      brf.rx_timeout_ms = 2000;
      break;

    case 15360000:
      openair0_cfg->samples_per_packet = 2048;
      openair0_cfg->tx_sample_advance = 0;

      brf.num_buffers = 64;
      brf.num_transfers = 16;
      brf.rx_timeout_ms = 2000;
      break;
    case 11520000:
      openair0_cfg->samples_per_packet = 2048;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 64;
      brf.num_transfers = 16;
      brf.rx_timeout_ms = 2000;
      break;

    case 7680000:
      openair0_cfg->samples_per_packet = 1024;
      openair0_cfg->tx_sample_advance = 0;
      brf.num_buffers = 32;
      brf.num_transfers = 16;
      brf.rx_timeout_ms = 2000;
      break;

    case 1920000:
      openair0_cfg->samples_per_packet = 256;
      openair0_cfg->tx_sample_advance = 50;
      brf.num_buffers = 128;
      brf.num_transfers = 16;
      brf.rx_timeout_ms = 2000;
      break;

    default:
      LOG_E(HW, "Error: unknown sampling rate %f\n", openair0_cfg->sample_rate);
      return -1;
      break;
  }

  brf.tx_timeout_ms = 2000;
  brf.buffer_size = (unsigned int)openair0_cfg->samples_per_packet;
  brf.sample_rate   = (unsigned int)openair0_cfg->sample_rate;
  brf.format = brf.use_sc8 ? BLADERF_FORMAT_SC8_Q7_META : BLADERF_FORMAT_SC16_Q11_META;
  if (brf.use_sc8) {
    brf.conversion_buffer_size = brf.sample_rate / 1000;
    brf.rx_conversion_buffer = malloc_or_fail(2 * brf.conversion_buffer_size * sizeof(*brf.rx_conversion_buffer));
    brf.tx_conversion_buffer = malloc_or_fail(2 * brf.conversion_buffer_size * sizeof(*brf.tx_conversion_buffer));
  }

  float rxterm = brf.num_transfers * brf.buffer_size * 1000.0 / brf.rx_timeout_ms;
  float txterm = brf.num_transfers * brf.buffer_size * 1000.0 / brf.tx_timeout_ms;
  LOG_I(HW,
        "sample rate %d trnsfers %d timeout rx %d tx %d buffer size %d (rx %f, tx %f)\n",
        brf.sample_rate,
        brf.num_transfers,
        brf.rx_timeout_ms,
        brf.tx_timeout_ms,
        brf.buffer_size,
        rxterm,
        txterm);
  DevAssert(brf.sample_rate > txterm * 1.25); // see doc on async interface
  DevAssert(brf.sample_rate > rxterm * 1.25);

  memset(&brf.meta_rx, 0, sizeof(brf.meta_rx));
  memset(&brf.meta_tx, 0, sizeof(brf.meta_tx));

  // set this to BLADERF_LOG_LEVEL_VERBOSE for more detailed BladeRF info
  bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_INFO);

  if (brf_open_and_configure(&brf, openair0_cfg) != 0)
    return -1;

  LOG_I(HW, "BLADERF: Initializing device done\n");
  device->type             = BLADERF_DEV;
  device->trx_start_func = trx_brf_start;
  device->trx_end_func   = trx_brf_end;
  device->trx_read_func  = trx_brf_read;
  device->trx_write_func = trx_brf_write;
  device->trx_get_stats_func   = trx_brf_get_stats;
  device->trx_reset_stats_func = trx_brf_reset_stats;
  device->trx_stop_func        = trx_brf_stop;
  device->trx_set_freq_func    = trx_brf_set_freq;
  device->trx_set_gains_func   = trx_brf_set_gains;
  device->trx_write_init       = trx_brf_write_init;
  device->openair0_cfg = openair0_cfg;
  device->priv = malloc_or_fail(sizeof(brf_state_t));
  *(brf_state_t *)device->priv = brf;

  return 0;
}
/*@}*/
