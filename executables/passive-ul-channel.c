/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "executables/passive-ul-channel.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "PHY/defs_nr_UE.h"
#include "common/utils/LOG/log.h"
#include "common/utils/assertions.h"

#ifdef ENABLE_SIONNA_RK_PLUGINS
#include "openair1/PHY/defs_RU.h"
#include "plugins/common/src/plugins.h"
#include "plugins/channel_emulation/cuda_emulator/src/chn_emu_cuda.h"

#define PASSIVE_UL_OBSERVED_ANTENNAS 4
#define PASSIVE_UL_PENDING_WRITES 512

typedef struct {
  openair0_timestamp_t timestamp;
  uint64_t radio_slot;
  bool valid;
} passive_ul_pending_write_t;

static int g_source_id;
static bool g_initialized;
static NR_DL_FRAME_PARMS g_frame_parms;
static RU_t g_observed_ru;
static int32_t *g_observed_data[PASSIVE_UL_OBSERVED_ANTENNAS];
static passive_ul_pending_write_t g_pending[PASSIVE_UL_PENDING_WRITES];
static pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_channel_mutex = PTHREAD_MUTEX_INITIALIZER;

static int configured_source_id(void)
{
  const char *text = getenv("OAI_PASSIVE_UL_SOURCE_ID");
  if (text == NULL || text[0] == '\0')
    return 0;
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  AssertFatal(errno == 0 && end != text && *end == '\0' && value >= 1 && value <= 4,
              "OAI_PASSIVE_UL_SOURCE_ID must be in 1..4 (got '%s')\n",
              text);
  return (int)value;
}

bool passive_ul_channel_requested(void)
{
  return configured_source_id() > 0;
}

void passive_ul_channel_init(const NR_DL_FRAME_PARMS *fp)
{
  g_source_id = configured_source_id();
  if (g_source_id == 0)
    return;
  AssertFatal(fp != NULL, "passive UL channel requires frame parameters\n");
  AssertFatal(is_channel_emulation_enabled(),
              "passive UL source %d requires its own --cir-folder bank\n",
              g_source_id);
  const char *num_rx_text = getenv("SIONNA_CIR_NUM_RX_ANTENNAS");
  AssertFatal(num_rx_text != NULL && atoi(num_rx_text) == PASSIVE_UL_OBSERVED_ANTENNAS,
              "passive UL source %d bank must contain exactly %d receive planes (got '%s')\n",
              g_source_id,
              PASSIVE_UL_OBSERVED_ANTENNAS,
              num_rx_text ? num_rx_text : "unset");

  g_frame_parms = *fp;
  memset(&g_observed_ru, 0, sizeof(g_observed_ru));
  g_observed_ru.common.rxdata = g_observed_data;
  for (int ant = 0; ant < PASSIVE_UL_OBSERVED_ANTENNAS; ++ant) {
    g_observed_data[ant] = calloc((size_t)fp->samples_per_frame, sizeof(*g_observed_data[ant]));
    AssertFatal(g_observed_data[ant] != NULL,
                "failed to allocate passive UL source %d antenna %d work ring\n",
                g_source_id,
                ant);
  }
  memset(g_pending, 0, sizeof(g_pending));
  g_initialized = true;
  LOG_I(HW,
        "Passive UL source channel initialized: source_id=%d antennas=%d frame_samples=%d\n",
        g_source_id,
        PASSIVE_UL_OBSERVED_ANTENNAS,
        fp->samples_per_frame);
}

void passive_ul_channel_register_write(openair0_timestamp_t timestamp, uint64_t radio_slot)
{
  if (!g_initialized)
    return;
  pthread_mutex_lock(&g_pending_mutex);
  int free_index = -1;
  for (int i = 0; i < PASSIVE_UL_PENDING_WRITES; ++i) {
    if (g_pending[i].valid && g_pending[i].timestamp == timestamp) {
      g_pending[i].radio_slot = radio_slot;
      pthread_mutex_unlock(&g_pending_mutex);
      return;
    }
    if (!g_pending[i].valid && free_index < 0)
      free_index = i;
  }
  AssertFatal(free_index >= 0,
              "passive UL source %d write metadata queue exhausted\n",
              g_source_id);
  g_pending[free_index] = (passive_ul_pending_write_t){.timestamp = timestamp,
                                                       .radio_slot = radio_slot,
                                                       .valid = true};
  pthread_mutex_unlock(&g_pending_mutex);
}

static bool consume_radio_slot(openair0_timestamp_t timestamp, uint64_t *radio_slot)
{
  bool found = false;
  pthread_mutex_lock(&g_pending_mutex);
  for (int i = 0; i < PASSIVE_UL_PENDING_WRITES; ++i) {
    if (g_pending[i].valid && g_pending[i].timestamp == timestamp) {
      *radio_slot = g_pending[i].radio_slot;
      g_pending[i].valid = false;
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_pending_mutex);
  return found;
}

void passive_ul_channel_clear_pending(void)
{
  if (!g_initialized)
    return;
  pthread_mutex_lock(&g_pending_mutex);
  memset(g_pending, 0, sizeof(g_pending));
  pthread_mutex_unlock(&g_pending_mutex);
}

static bool samples_are_zero(void **samples, int nsamps, int num_antennas)
{
  for (int ant = 0; ant < num_antennas; ++ant) {
    const uint32_t *iq = samples[ant];
    for (int i = 0; i < nsamps; ++i)
      if (iq[i] != 0)
        return false;
  }
  return true;
}

void passive_ul_channel_after_normal_write(PHY_VARS_NR_UE *ue,
                                           openair0_device_t *device,
                                           openair0_timestamp_t timestamp,
                                           void **samples,
                                           int nsamps,
                                           int num_antennas)
{
  if (!g_initialized)
    return;
  AssertFatal(device->trx_write_passive_ul_func != NULL,
              "passive UL source %d requires RFsim observed-UL support\n",
              g_source_id);
  uint64_t radio_slot = 0;
  if (!consume_radio_slot(timestamp, &radio_slot)) {
    // Acquisition/re-alignment writes are not logical TX slots and therefore have no CIR mapping.
    // Publish a timestamp-only watermark: the passive mixer remains complete without inventing a
    // bank slot or leaking unchannelized samples into the observed waveform.
    const int written = device->trx_write_passive_ul_func(device,
                                                          timestamp + device->firstTS,
                                                          NULL,
                                                          nsamps,
                                                          PASSIVE_UL_OBSERVED_ANTENNAS,
                                                          true);
    AssertFatal(written == nsamps, "failed to publish passive UL alignment watermark\n");
    return;
  }
  AssertFatal(num_antennas == 1,
              "passive UL source banks model one UE transmit stream (got %d)\n",
              num_antennas);

  if (samples_are_zero(samples, nsamps, num_antennas)) {
    const int written = device->trx_write_passive_ul_func(device,
                                                          timestamp + device->firstTS,
                                                          NULL,
                                                          nsamps,
                                                          PASSIVE_UL_OBSERVED_ANTENNAS,
                                                          true);
    AssertFatal(written == nsamps, "failed to publish passive UL empty watermark\n");
    return;
  }

  c16_t *raw_base = (c16_t *)ue->common_vars.txData[0];
  c16_t *raw_block = (c16_t *)samples[0];
  const ptrdiff_t data_offset = raw_block - raw_base;
  AssertFatal(data_offset >= 0 && data_offset < g_frame_parms.samples_per_frame,
              "passive UL source %d write buffer is outside the UE TX ring\n",
              g_source_id);
  const int slot = (int)(radio_slot % (uint64_t)g_frame_parms.slots_per_frame);
  const int slot_samples = get_samples_per_slot(slot, &g_frame_parms);
  AssertFatal(nsamps <= slot_samples,
              "passive UL source %d write has %d samples, slot %d has %d\n",
              g_source_id,
              nsamps,
              slot,
              slot_samples);

  pthread_mutex_lock(&g_channel_mutex);
  c16_t *work = (c16_t *)g_observed_data[0];
  for (int relative = -(MAX_TAP_DELAY - 1); relative < slot_samples; ++relative) {
    const int index = ((int)data_offset + relative + g_frame_parms.samples_per_frame)
                      % g_frame_parms.samples_per_frame;
    work[index] = raw_base[index];
  }
  const void *cir = channel_emulator_cir_read_and_apply_at_slot(radio_slot);
  AssertFatal(cir != NULL,
              "passive UL source %d failed to select CIR for radio slot %" PRIu64 "\n",
              g_source_id,
              radio_slot);
  // Receiver thermal noise belongs to the passive receiver and is already generated once by its
  // DL-bank convolution. UL source processes contribute only their independently propagated signal;
  // otherwise an N-UE scenario would incorrectly contain N additional receiver-noise fields.
  AssertFatal(chn_emu_interface.set_sigma_scaling != NULL && chn_emu_interface.set_sigma_max != NULL,
              "passive UL source %d channel emulator cannot disable per-leg receiver noise\n",
              g_source_id);
  chn_emu_interface.set_sigma_scaling(0.0f);
  chn_emu_interface.set_sigma_max(0.0f);
  chn_emu_interface.compute(&g_observed_ru,
                            slot,
                            &g_frame_parms,
                            g_frame_parms.ofdm_symbol_size + g_frame_parms.nb_prefix_samples0,
                            g_frame_parms.ofdm_symbol_size + g_frame_parms.nb_prefix_samples,
                            "rx",
                            (int)data_offset,
                            cir);
  void *observed[PASSIVE_UL_OBSERVED_ANTENNAS];
  for (int ant = 0; ant < PASSIVE_UL_OBSERVED_ANTENNAS; ++ant)
    observed[ant] = (c16_t *)g_observed_data[ant] + data_offset;
  const int written = device->trx_write_passive_ul_func(device,
                                                        timestamp + device->firstTS,
                                                        observed,
                                                        nsamps,
                                                        PASSIVE_UL_OBSERVED_ANTENNAS,
                                                        false);
  pthread_mutex_unlock(&g_channel_mutex);
  AssertFatal(written == nsamps, "failed to publish channelized passive UL samples\n");
}

void passive_ul_channel_shutdown(void)
{
  if (!g_initialized)
    return;
  for (int ant = 0; ant < PASSIVE_UL_OBSERVED_ANTENNAS; ++ant) {
    free(g_observed_data[ant]);
    g_observed_data[ant] = NULL;
  }
  memset(g_pending, 0, sizeof(g_pending));
  g_initialized = false;
  g_source_id = 0;
}

#else

bool passive_ul_channel_requested(void) { return false; }
void passive_ul_channel_init(const NR_DL_FRAME_PARMS *fp) { (void)fp; }
void passive_ul_channel_register_write(openair0_timestamp_t timestamp, uint64_t radio_slot)
{
  (void)timestamp;
  (void)radio_slot;
}
void passive_ul_channel_clear_pending(void) {}
void passive_ul_channel_after_normal_write(PHY_VARS_NR_UE *ue,
                                           openair0_device_t *device,
                                           openair0_timestamp_t timestamp,
                                           void **samples,
                                           int nsamps,
                                           int num_antennas)
{
  (void)ue;
  (void)device;
  (void)timestamp;
  (void)samples;
  (void)nsamps;
  (void)num_antennas;
}
void passive_ul_channel_shutdown(void) {}

#endif
