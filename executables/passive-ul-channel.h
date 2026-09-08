/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef OAI_PASSIVE_UL_CHANNEL_H
#define OAI_PASSIVE_UL_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>
#include "PHY/defs_nr_common.h"
#include "radio/COMMON/common_lib.h"

typedef struct PHY_VARS_NR_UE_s PHY_VARS_NR_UE;

bool passive_ul_channel_requested(void);
void passive_ul_channel_init(const NR_DL_FRAME_PARMS *fp);
void passive_ul_channel_register_write(openair0_timestamp_t timestamp, uint64_t radio_slot);
void passive_ul_channel_clear_pending(void);
void passive_ul_channel_after_normal_write(PHY_VARS_NR_UE *ue,
                                           openair0_device_t *device,
                                           openair0_timestamp_t timestamp,
                                           void **samples,
                                           int nsamps,
                                           int num_antennas);
void passive_ul_channel_shutdown(void);

#endif
