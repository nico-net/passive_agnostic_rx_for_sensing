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
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nr_pdcch_blind_monitor_test_stubs.c
 * \brief Minimal stubs for offline unit tests of nr_pdcch_blind_monitor library.
 * These replace runtime-specific implementations that are not needed for gtest execution.
 */

#include <stdbool.h>

/*! Test-local stub: offline tests do not populate the CORESET bank.
 * Returning false (not found) allows the offline discovery logic to continue
 * without needing the full RT harness state.
 */
bool nr_pdcch_blind_monitor_bank_has_geometry(int rb_offset, int groups, int duration, int bundle, int interleaver,
                                               int shift, int nid)
{
  (void)rb_offset;
  (void)groups;
  (void)duration;
  (void)bundle;
  (void)interleaver;
  (void)shift;
  (void)nid;
  return false;
}
