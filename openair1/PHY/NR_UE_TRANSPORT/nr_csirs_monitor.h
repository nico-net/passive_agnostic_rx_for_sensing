/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_csirs_monitor.h
 * \brief UE-agnostic CSI-RS monitor for ISAC passive sensing.
 *
 * The receiver's own CSI-RS reception is driven by its RRC CSI-MeasConfig (the resources the
 * gNB told *this* UE to measure). For passive sensing we also want to capture cell-common /
 * other-UE CSI-RS resources (e.g. the srsRAN tracking NZP-CSI-RS) whose parameters are known
 * from the cell config (gnb_remote_logs/), independent of what our own MAC schedules.
 *
 * This module parses a list of such resources from the [sensing] config section and answers,
 * per slot, which of them occur. The PHY DL procedures then run each due resource through the
 * lean sensing-only capture path (nr_ue_csi_rs_sensing_capture) which estimates Ĥ and feeds the
 * ISAC engine, WITHOUT emitting any CSI report to MAC/gNB.
 */

#ifndef NR_CSIRS_MONITOR_H
#define NR_CSIRS_MONITOR_H

#include "PHY/defs_nr_UE.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum number of monitor CSI-RS resources parsed from the config.
#define NR_CSIRS_MONITOR_MAX 16

/**
 * @brief Parse the [sensing] csirs_monitor list. Safe to call once at UE start-up; a no-op that
 * leaves the monitor disabled when the parameter is unset/empty.
 */
void nr_csirs_monitor_init(void);

/// Non-zero when at least one monitor CSI-RS resource is configured.
int nr_csirs_monitor_enabled(void);

/**
 * @brief Collect the monitor CSI-RS resources that occur in absolute slot (@p frame, @p slot).
 *
 * @param frame            Rx frame (SFN)
 * @param slot             Rx slot within the frame
 * @param slots_per_frame  Slots per 10 ms radio frame at this numerology
 * @param[out] out         Filled with pointers to the due resources' PDUs (owned by this module)
 * @param max              Capacity of @p out
 * @return number of resources written to @p out
 */
int nr_csirs_monitor_due(int                                         frame,
                         int                                         slot,
                         int                                         slots_per_frame,
                         const fapi_nr_dl_config_csirs_pdu_rel15_t** out,
                         int                                         max);

#ifdef __cplusplus
}
#endif

#endif // NR_CSIRS_MONITOR_H
